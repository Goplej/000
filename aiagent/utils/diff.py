"""Diff и патчи: показ изменений, подсчёт статистики, применение патчей."""
from __future__ import annotations

import difflib
import re
from collections.abc import Iterable
from dataclasses import dataclass


@dataclass
class DiffStats:
    added: int = 0
    removed: int = 0
    hunks: int = 0

    @property
    def total(self) -> int:
        return self.added + self.removed

    def summary(self) -> str:
        if not self.total:
            return "без изменений"
        return f"+{self.added} −{self.removed}"


def unified_diff(old: str, new: str, *, filename: str = "файл", context: int = 3) -> str:
    """Универсальный diff без заголовков с временем (стабильный вывод)."""
    old_lines = old.splitlines(keepends=True)
    new_lines = new.splitlines(keepends=True)
    lines = list(difflib.unified_diff(
        old_lines, new_lines,
        fromfile=f"a/{filename}", tofile=f"b/{filename}",
        n=context, lineterm="\n",
    ))
    return "".join(line if line.endswith("\n") else line + "\n" for line in lines)


def diff_stats(old: str, new: str) -> DiffStats:
    stats = DiffStats()
    for line in unified_diff(old, new).splitlines():
        if line.startswith("@@ "):
            stats.hunks += 1
        elif line.startswith("+") and not line.startswith("+++"):
            stats.added += 1
        elif line.startswith("-") and not line.startswith("---"):
            stats.removed += 1
    return stats


def colorize_diff(diff: str, colorize) -> str:
    """Раскрашивает diff переданной функцией (обычно Ansi.paint)."""
    out = []
    for line in diff.splitlines():
        if line.startswith("+++") or line.startswith("---"):
            out.append(colorize(line, "\033[1m"))
        elif line.startswith("+"):
            out.append(colorize(line, "\033[32m"))
        elif line.startswith("-"):
            out.append(colorize(line, "\033[31m"))
        elif line.startswith("@@"):
            out.append(colorize(line, "\033[36m"))
        else:
            out.append(line)
    return "\n".join(out)


# --------------------------------------------------------------------------------------
# Патчи в стиле «поиск/замена с контекстом» (как у Claude Code)
# --------------------------------------------------------------------------------------
@dataclass
class PatchBlock:
    search: str
    replace: str
    hint: str = ""


def parse_patch(text: str) -> list[PatchBlock]:
    """Разбирает патч из блоков <<<<<<< SEARCH / ======= / >>>>>>> REPLACE."""
    pattern = re.compile(
        r"<{5,}\s*SEARCH[^\n]*\n(.*?)\n?={5,}\s*\n(.*?)\n?>{5,}\s*REPLACE[^\n]*",
        re.DOTALL,
    )
    blocks = []
    for match in pattern.finditer(text):
        blocks.append(PatchBlock(search=match.group(1), replace=match.group(2)))
    return blocks


def apply_patch_blocks(content: str, blocks: Iterable[PatchBlock]) -> tuple[str, list[str]]:
    """
    Применяет блоки поиска/замены. Возвращает (новый_текст, список_ошибок).
    Сначала пробует точное совпадение, затем — с нормализованными отступами.
    """
    result = content
    errors: list[str] = []
    for index, block in enumerate(blocks, 1):
        if not block.search:
            errors.append(f"блок {index}: пустой поисковый фрагмент")
            continue
        if block.search in result:
            result = result.replace(block.search, block.replace, 1)
            continue
        # Мягкий режим: игнорируем различия в отступах и хвостовых пробелах
        fuzzy = _flexible_replace(result, block.search, block.replace)
        if fuzzy is None:
            errors.append(f"блок {index}: фрагмент не найден в файле")
        else:
            result = fuzzy
    return result, errors


def _flexible_replace(content: str, search: str, replace: str) -> str | None:
    """Ищет фрагмент, не обращая внимания на отступы строк и пустые строки."""
    def normalize(text: str) -> str:
        return "\n".join(line.strip() for line in text.strip().splitlines() if line.strip())

    target = normalize(search)
    if not target:
        return None
    lines = content.splitlines()
    target_lines = target.split("\n")
    for start in range(len(lines)):
        for end in range(start + 1, min(len(lines), start + len(target_lines) * 3 + 5) + 1):
            window = "\n".join(line.strip() for line in lines[start:end] if line.strip())
            if window == target:
                indent = re.match(r"\s*", lines[start]).group(0)
                replacement_lines = [
                    (indent + line.lstrip() if line.strip() else "")
                    for line in replace.splitlines()
                ]
                return "\n".join(
                    lines[:start] + replacement_lines + lines[end:]
                ) + ("\n" if content.endswith("\n") else "")
    return None


__all__ = [
    "DiffStats", "PatchBlock", "apply_patch_blocks", "colorize_diff", "diff_stats",
    "parse_patch", "unified_diff",
]
