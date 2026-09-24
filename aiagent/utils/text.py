"""Работа с текстом: обрезка, превью, таблицы, склонения."""
from __future__ import annotations

import re
from collections.abc import Iterable, Sequence
from typing import Any

_ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


def clip(text: str, limit: int, *, head_ratio: float = 0.6, note: str = "…обрезано") -> str:
    """
    Умная обрезка: сохраняет начало и конец (для ошибок и логов конец важнее).
    Не режет посередине строки, если это возможно.
    """
    if limit <= 0 or len(text) <= limit:
        return text
    if limit <= len(note) + 12:      # слишком маленький лимит — просто режем
        return text[:limit]
    head = int(limit * head_ratio)
    tail = max(0, limit - head - len(note) - 10)
    start = text[:head]
    newline = start.rfind("\n")
    if newline > head * 0.5:
        start = start[:newline]
    end = text[-tail:] if tail else ""
    newline_end = end.find("\n")
    if 0 < newline_end < tail * 0.5:
        end = end[newline_end + 1:]
    removed = len(text) - len(start) - len(end)
    return f"{start}\n{note} ({removed} симв.)\n{end}"


def truncate_middle(text: str, limit: int) -> str:
    return clip(text, limit, head_ratio=0.5)


def preview(text: str, limit: int = 120) -> str:
    """Однострочное превью для логов и карточек инструментов."""
    flat = "⏎".join(line.strip() for line in str(text).splitlines() if line.strip())
    return flat if len(flat) <= limit else flat[: limit - 1] + "…"


def text_table(rows: Sequence[Sequence[Any]], headers: Sequence[str] | None = None,
               *, max_width: int = 60) -> str:
    """Простая таблица с выравниванием (работает без внешних библиотек)."""
    data: list[list[str]] = []
    if headers:
        data.append([str(h) for h in headers])
    for row in rows:
        data.append([clip(str(cell), max_width, head_ratio=1.0, note="…") for cell in row])
    if not data:
        return ""
    widths = [max(len(row[i]) for row in data) for i in range(len(data[0]))]
    lines = []
    for index, row in enumerate(data):
        line = "  ".join(cell.ljust(widths[i]) for i, cell in enumerate(row))
        lines.append(line.rstrip())
        if headers and index == 0:
            lines.append("  ".join("─" * w for w in widths))
    return "\n".join(lines)


def plural_ru(count: int, one: str, few: str, many: str) -> str:
    """1 файл, 2 файла, 5 файлов."""
    count = abs(int(count))
    if count % 10 == 1 and count % 100 != 11:
        return one
    if 2 <= count % 10 <= 4 and not 12 <= count % 100 <= 14:
        return few
    return many


def human_size(size: float) -> str:
    for unit in ("Б", "КБ", "МБ", "ГБ", "ТБ"):
        if abs(size) < 1024:
            return f"{size:.0f} {unit}" if unit == "Б" else f"{size:.1f} {unit}"
        size /= 1024
    return f"{size:.1f} ПБ"


def human_duration(seconds: float) -> str:
    if seconds < 1:
        return f"{seconds * 1000:.0f} мс"
    if seconds < 60:
        return f"{seconds:.1f} с"
    minutes, rest = divmod(int(seconds), 60)
    if minutes < 60:
        return f"{minutes} мин {rest} с"
    hours, minutes = divmod(minutes, 60)
    return f"{hours} ч {minutes} мин"


def is_binary(data: bytes, sample: int = 4096) -> bool:
    """Эвристика: бинарный ли файл (нулевые байты или много непечатаемых)."""
    chunk = data[:sample]
    if b"\x00" in chunk:
        return True
    if not chunk:
        return False
    printable = sum(
        1 for byte in chunk
        if 9 <= byte <= 13 or 32 <= byte <= 126 or byte >= 128
    )
    return printable / len(chunk) < 0.75


def count_lines(text: str) -> int:
    return text.count("\n") + (0 if text.endswith("\n") or not text else 1)


def strip_ansi(text: str) -> str:
    return _ANSI_RE.sub("", text)


def indent(text: str, prefix: str = "  ", skip_first: bool = True) -> str:
    lines = str(text).splitlines() or [""]
    out = [lines[0]] if skip_first else []
    out.extend(f"{prefix}{line}" for line in (lines[1:] if skip_first else lines))
    return "\n".join(out)


def normalize_whitespace(text: str) -> str:
    return re.sub(r"[ \t]+", " ", re.sub(r"\n{3,}", "\n\n", text)).strip()


def word_wrap(text: str, width: int = 100) -> str:
    words, lines, current = text.split(), [], ""
    for word in words:
        if len(current) + len(word) + 1 > width:
            lines.append(current)
            current = word
        else:
            current = f"{current} {word}".strip()
    if current:
        lines.append(current)
    return "\n".join(lines)


def join_sections(sections: Iterable[str], sep: str = "\n\n") -> str:
    return sep.join(s.strip() for s in sections if s and s.strip())


__all__ = [
    "clip", "count_lines", "human_duration", "human_size", "indent", "is_binary",
    "join_sections", "normalize_whitespace", "plural_ru", "preview", "strip_ansi",
    "text_table", "truncate_middle", "word_wrap",
]
