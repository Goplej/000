"""Терпимый JSON: разбор «почти-JSON» от языковых моделей и поиск объектов в тексте."""
from __future__ import annotations

import json
import re
from collections.abc import Iterator
from typing import Any


def dumps_compact(data: Any) -> str:
    return json.dumps(data, ensure_ascii=False, separators=(",", ":"))


def dumps_pretty(data: Any) -> str:
    return json.dumps(data, ensure_ascii=False, indent=2)


def loads_lenient(text: str) -> Any:
    """
    Парсит JSON, прощая типичные вольности моделей:
      • одинарные кавычки            {'a': 1}
      • ключи без кавычек            {a: 1}
      • висячие запятые              {"a": 1,}
      • «умные» кавычки              “a”: “b”
      • комментарии                  // ...   /* ... */
      • Python-литералы              True/False/None
    Возвращает None, если восстановить не удалось.
    """
    if text is None:
        return None
    if isinstance(text, (dict, list, int, float, bool)):
        return text
    candidate = str(text).strip()
    if not candidate:
        return None

    for attempt in (candidate, _repair(candidate), _repair(_strip_comments(candidate))):
        try:
            return json.loads(attempt)
        except (json.JSONDecodeError, ValueError):
            continue
    return None


def parse_json_objects(text: str) -> Iterator[tuple[tuple[int, int], Any]]:
    """Ищет все сбалансированные JSON-объекты/массивы в тексте: ((начало, конец), значение)."""
    index, length = 0, len(text)
    while index < length:
        char = text[index]
        if char not in "{[":
            index += 1
            continue
        end = _match_bracket(text, index)
        if end < 0:
            break
        chunk = text[index:end + 1]
        value = loads_lenient(chunk)
        if value is not None:
            yield (index, end + 1), value
        index = end + 1


def find_balanced(text: str, start: int) -> int:
    """Индекс закрывающей скобки для открывающей на позиции start (-1, если не найдена)."""
    return _match_bracket(text, start)


def _match_bracket(text: str, start: int) -> int:
    pairs = {"{": "}", "[": "]"}
    opener = text[start]
    closer = pairs.get(opener)
    if closer is None:
        return -1
    depth = 0
    in_string = False
    escaped = False
    quote = ""
    for index in range(start, len(text)):
        char = text[index]
        if in_string:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                in_string = False
            continue
        if char in {'"', "'"}:
            in_string, quote = True, char
            continue
        if char == opener:
            depth += 1
        elif char == closer:
            depth -= 1
            if depth == 0:
                return index
    return -1


def _strip_comments(text: str) -> str:
    text = re.sub(r"/\*[\s\S]*?\*/", "", text)
    return re.sub(r"(?m)^\s*//.*$", "", text)


def _repair(text: str) -> str:
    fixed = (text
             .replace("\u201c", '"').replace("\u201d", '"')
             .replace("\u2018", "'").replace("\u2019", "'"))
    fixed = re.sub(r"//[^\n\"]*$", "", fixed, flags=re.MULTILINE)
    fixed = re.sub(r"\bTrue\b", "true", fixed)
    fixed = re.sub(r"\bFalse\b", "false", fixed)
    fixed = re.sub(r"\bNone\b", "null", fixed)
    # Ключи без кавычек: {key: ... } и ,key2:
    fixed = re.sub(r'([{,]\s*)([A-Za-z_][\w.\-]*)(\s*:)', r'\1"\2"\3', fixed)
    # Одинарные кавычки → двойные (аккуратно: только вокруг простых строк)
    fixed = re.sub(r"'([^'\"\\\n]{0,200}?)'(\s*[:,\]}])", r'"\1"\2', fixed)
    fixed = re.sub(r"(\[|:\s*)'([^'\"\\\n]{0,200}?)'", r'\1"\2"', fixed)
    # Висячие запятые
    fixed = re.sub(r",\s*([}\]])", r"\1", fixed)
    return fixed


def extract_json_after(text: str, key: str) -> Any:
    """Достаёт значение ключа, даже если объект разорван переносами строк."""
    pattern = re.compile(rf'"?{re.escape(key)}"?\s*[:=]\s*', re.IGNORECASE)
    match = pattern.search(text)
    if not match:
        return None
    rest = text[match.end():].lstrip()
    if rest[:1] in "{[":
        end = _match_bracket(rest, 0)
        if end > 0:
            return loads_lenient(rest[:end + 1])
    if rest[:1] in {'"', "'"}:
        quote = rest[0]
        end = rest.find(quote, 1)
        if end > 0:
            return rest[1:end]
    token = re.match(r"[^\s,\n}\]]+", rest)
    return token.group(0) if token else None


__all__ = [
    "dumps_compact", "dumps_pretty", "extract_json_after", "find_balanced",
    "loads_lenient", "parse_json_objects",
]
