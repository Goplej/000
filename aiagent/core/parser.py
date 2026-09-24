"""Разбор вызовов инструментов из текстового ответа модели.

Нужен для двух случаев:
  1) модель не умеет нативный tool calling (или он отключён) — тогда агент
     просит её писать <tool_call>{...}</tool_call>;
  2) модель смешала текст и вызовы — вызовы надо вытащить и исполнить.

Поддерживаемые форматы: <tool_call>, ```json, «голый» JSON, ReAct (Action/Action Input),
OpenAI-стиль {"function": {...}}, а также «почти-JSON» с одинарными кавычками.
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field

from ..core.types import ToolCall
from ..utils.json_utils import loads_lenient, parse_json_objects

_TAG_RE = re.compile(r"<\s*tool_call\s*>(.*?)<\s*/\s*tool_call\s*>", re.DOTALL | re.IGNORECASE)
_ALT_TAG_RE = re.compile(
    r"<\s*(?:tool|function|invoke|tool_use)\s*(?:name\s*=\s*[\"']?([\w.\-]+)[\"']?)?\s*>(.*?)"
    r"<\s*/\s*(?:tool|function|invoke|tool_use)\s*>",
    re.DOTALL | re.IGNORECASE,
)
_FENCE_RE = re.compile(r"```[ \t]*([\w+.\-]*)[ \t]*\n(.*?)```", re.DOTALL)
_REACT_RE = re.compile(
    r"^[ \t]*(?:\*\*)?Action(?:\s*\d+)?(?:\*\*)?[ \t]*[:：][ \t]*(?:\*\*)?([\w.\-]+)(?:\*\*)?[ \t]*$"
    r"([\s\S]*?)"
    r"^[ \t]*(?:\*\*)?Action[ \t]*(?:Input|Args|Arguments)(?:\*\*)?[ \t]*[:：][ \t]*([\s\S]*?)"
    r"(?=^[ \t]*(?:\*\*)?(?:Action|Observation|Final Answer|Thought)"
    r"|<\s*/?\s*tool_call|\Z)",
    re.DOTALL | re.MULTILINE | re.IGNORECASE,
)
NAME_KEYS = ("name", "tool", "tool_name", "toolName", "function", "action", "command")
ARGS_KEYS = ("arguments", "args", "parameters", "params", "input", "action_input",
             "tool_input", "toolInput")


@dataclass
class ParsedReply:
    text: str = ""
    calls: list[ToolCall] = field(default_factory=list)
    thinking: str = ""

    @property
    def has_calls(self) -> bool:
        return bool(self.calls)


def parse_reply(text: str, allowed: set[str] | None = None) -> ParsedReply:
    """Возвращает текст без служебных блоков и найденные вызовы инструментов."""
    calls: list[ToolCall] = []
    consumed: list[tuple[int, int]] = []
    thinking = ""
    if "<think" in text.lower() or "<reason" in text.lower():
        thinking, text = extract_thinking(text)

    for match in _TAG_RE.finditer(text):
        found = _calls_from_blob(match.group(1), match.group(0))
        if found:
            calls.extend(found)
            consumed.append(match.span())

    for match in _ALT_TAG_RE.finditer(text):
        if _overlaps(match.span(), consumed):
            continue
        found = _calls_from_blob(match.group(2), match.group(0), default_name=match.group(1))
        if found:
            calls.extend(found)
            consumed.append(match.span())

    for match in _FENCE_RE.finditer(text):
        language = (match.group(1) or "").lower()
        body = (match.group(2) or "").strip()
        looks_like_call = language in {"json", "tool_call", "tool", "function", "jsonc"} or body.startswith(
            ('{"name"', '{"tool"', "{'name'", '{"function"', '{"tool_calls"')
        )
        if not looks_like_call:
            continue
        found = _calls_from_blob(body, match.group(0))
        if found:
            calls.extend(found)
            consumed.append(match.span())

    for match in _REACT_RE.finditer(text):
        if _overlaps(match.span(), consumed):
            continue
        arguments = _parse_args(match.group(3))
        if arguments is not None:
            calls.append(ToolCall(name=match.group(1).strip(), arguments=arguments,
                                  raw=match.group(0)))
            consumed.append(match.span())

    if not calls:
        for span, obj in parse_json_objects(text):
            if _overlaps(span, consumed):
                continue
            found = _calls_from_obj(obj, text[span[0]:span[1]])
            if found:
                calls.extend(found)
                consumed.append(span)

    if allowed:
        # Вызовы несуществующих инструментов отбрасываем: модель не должна «изобретать» API.
        calls = [c for c in calls if c.name in allowed]

    return ParsedReply(text=strip_spans(text, consumed), calls=calls, thinking=thinking)


def strip_spans(text: str, spans: list[tuple[int, int]]) -> str:
    """Удаляет найденные служебные блоки, сохраняя остальной текст."""
    if not spans:
        return _tidy(text)
    out, position = [], 0
    for start, end in sorted(spans):
        if start < position:
            continue
        out.append(text[position:start])
        position = max(position, end)
    out.append(text[position:])
    return _tidy("".join(out))


def _tidy(text: str) -> str:
    return re.sub(r"\n{3,}", "\n\n", text).strip()


def _overlaps(span: tuple[int, int], spans: list[tuple[int, int]]) -> bool:
    return any(not (span[1] <= start or span[0] >= end) for start, end in spans)


def _calls_from_blob(blob: str, raw: str, default_name: str | None = None) -> list[ToolCall]:
    text = _strip_think(blob or "").strip()
    if not text:
        return []
    for candidate in (text, text.strip("`").strip()):
        obj = loads_lenient(candidate)
        if obj is not None:
            found = _calls_from_obj(obj, raw, default_name)
            if found:
                return found
    for span, obj in parse_json_objects(text):
        found = _calls_from_obj(obj, text[span[0]:span[1]], default_name)
        if found:
            return found
    # Формат «имя, затем аргументы текстом»
    match = re.match(r"^([\w.\-]+)\s*\n?(.*)$", text, re.DOTALL)
    if match and (default_name or match.group(1)):
        arguments = _parse_args(match.group(2))
        if arguments is not None:
            return [ToolCall(name=(default_name or match.group(1)).strip(),
                             arguments=arguments, raw=raw)]
    return []


def _calls_from_obj(obj, raw: str = "", default_name: str | None = None) -> list[ToolCall]:
    if isinstance(obj, list):
        out: list[ToolCall] = []
        for item in obj:
            out.extend(_calls_from_obj(item, raw, default_name))
        return out
    if not isinstance(obj, dict):
        return []

    if isinstance(obj.get("function"), dict):          # OpenAI-стиль
        merged = dict(obj["function"])
        merged.setdefault("name", obj.get("name"))
        obj = merged
    if isinstance(obj.get("tool_calls"), list):
        return _calls_from_obj(obj["tool_calls"], raw, default_name)

    name = default_name or ""
    for key in NAME_KEYS:
        value = obj.get(key)
        if isinstance(value, str) and value.strip():
            name = value.strip()
            break
    if not name:
        return []

    arguments: dict = {}
    for key in ARGS_KEYS:
        if key not in obj:
            continue
        parsed = _parse_args(obj[key])
        if parsed is None:
            parsed = {k: v for k, v in obj.items() if k not in NAME_KEYS}
        arguments = parsed
        break
    else:
        extra = {k: v for k, v in obj.items()
                 if k not in NAME_KEYS and not str(k).startswith("__")}
        if extra:
            arguments = extra

    if not isinstance(arguments, dict):
        return []
    return [ToolCall(name=name, arguments=_drop_none(arguments), raw=raw)]


def _parse_args(blob) -> dict | None:
    if isinstance(blob, dict):
        return _drop_none(blob)
    if blob is None or isinstance(blob, (int, float, bool)):
        return None
    text = _strip_think(str(blob)).strip().strip("`").strip()
    if not text:
        return {}
    if text.lower() in {"none", "null", "n/a", "-", "{}"}:
        return {}
    obj = loads_lenient(text)
    if isinstance(obj, dict):
        return _drop_none(obj)
    # key=value / key: value построчно
    pairs: dict = {}
    for line in text.splitlines():
        match = re.match(r"^\s*([\w.\-]+)\s*[:=]\s*(.+?)\s*$", line)
        if not match:
            return None
        key, value = match.group(1), match.group(2).strip().strip('"').strip("'")
        if value.lower() in {"true", "false"}:
            pairs[key] = value.lower() == "true"
        elif re.fullmatch(r"-?\d+", value):
            pairs[key] = int(value)
        elif re.fullmatch(r"-?\d+\.\d+", value):
            pairs[key] = float(value)
        else:
            pairs[key] = value
    return pairs or None


def _drop_none(data: dict) -> dict:
    return {k: v for k, v in data.items() if v is not None}


def _strip_think(text: str) -> str:
    """Убирает блоки размышлений Qwen3/DeepSeek-R1 из полезного текста."""
    return re.sub(
        r"<\s*(think|thinking|reasoning)\s*>.*?<\s*/\s*\1\s*>", "", text,
        flags=re.DOTALL | re.IGNORECASE,
    ).strip()


def extract_thinking(text: str) -> tuple[str, str]:
    """Возвращает (мышление, ответ) — для моделей с открытыми рассуждениями."""
    match = re.search(r"<\s*(think|thinking|reasoning)\s*>(.*?)<\s*/\s*\1\s*>", text,
                      flags=re.DOTALL | re.IGNORECASE)
    if not match:
        return "", text.strip()
    thinking = match.group(2).strip()
    return thinking, _strip_think(text)


__all__ = ["ParsedReply", "extract_thinking", "parse_reply", "strip_spans"]
