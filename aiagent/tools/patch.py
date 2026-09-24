"""Инструмент apply_patch: правки в формате SEARCH/REPLACE и unified diff.

Формат (как у Claude Code):
    <<<<<<< SEARCH
    старый фрагмент
    =======
    новый фрагмент
    >>>>>>> REPLACE

Позволяет одной операцией менять много мест в одном или нескольких файлах —
это резко сокращает число шагов агента.
"""
from __future__ import annotations

from pathlib import Path
from typing import Any

from ..utils.diff import apply_patch_blocks, diff_stats, parse_patch, unified_diff
from ..utils.text import clip
from . import Tool, ToolContext, ToolError, ToolResult
from .files import rel, resolve


def apply_patch(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    patch = str(args.get("patch") or args.get("text") or "").strip()
    if not patch:
        raise ToolError("нужен текст патча (patch)")
    target = args.get("path")

    blocks = parse_patch(patch)
    if not blocks:
        return ToolResult.error(
            "в патче не найдено ни одного блока. Формат:\n"
            "<<<<<<< SEARCH\nстарый код\n=======\nновый код\n>>>>>>> REPLACE"
        )

    if target:
        paths = [str(target)]
    else:
        # Патч без указания файла: ищем единственные подходящие файлы автоматически
        paths = []

    if not paths:
        return ToolResult.error(
            "укажи параметр path: имя файла, к которому применяется патч "
            "(или используй edit_file/multi_edit для одного файла)"
        )

    results: list[str] = []
    ok_any = False
    for raw_path in paths:
        path = resolve(ctx, raw_path, must_exist=True)
        original = path.read_text(encoding="utf-8", errors="replace")
        updated, errors = apply_patch_blocks(original, blocks)
        if errors:
            results.append(f"{rel(ctx, path)}: " + "; ".join(errors))
            continue
        if updated == original:
            results.append(f"{rel(ctx, path)}: изменений не требуется")
            ok_any = True
            continue
        if ctx.checkpoints is not None:
            ctx.checkpoints.snapshot(path)
        path.write_text(updated, encoding="utf-8")
        stats = diff_stats(original, updated)
        diff = unified_diff(original, updated, filename=rel(ctx, path), context=2)
        results.append(f"{rel(ctx, path)}: применено ({stats.summary()})\n" + clip(diff, 2500))
        ok_any = True

    body = "\n\n".join(results)
    return ToolResult(ok_any, body, display=f"apply_patch ({len(blocks)} блоков)")


def preview_patch(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    """Показывает, что изменится, ничего не записывая (dry-run)."""
    patch = str(args.get("patch") or "").strip()
    target = args.get("path")
    if not patch or not target:
        raise ToolError("нужны patch и path")
    path = resolve(ctx, str(target), must_exist=True)
    original = path.read_text(encoding="utf-8", errors="replace")
    updated, errors = apply_patch_blocks(original, parse_patch(patch))
    if errors:
        return ToolResult.error("; ".join(errors))
    stats = diff_stats(original, updated)
    diff = unified_diff(original, updated, filename=rel(ctx, path))
    return ToolResult.done(f"{rel(ctx, path)} (dry-run, {stats.summary()}):\n" + clip(diff, 6000),
                           display=f"preview_patch {rel(ctx, path)}")


def replace_in_files(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    """Массовая замена по нескольким файлам (например, переименование функции)."""
    pattern = str(args.get("find") or "")
    replacement = str(args.get("replace") or "")
    if not pattern:
        raise ToolError("нужен параметр find")
    listing = args.get("files")
    if not listing:
        listing = sorted(str(path.relative_to(ctx.workspace))
                         for path in Path(ctx.workspace).rglob(str(args.get("file_glob") or "*"))
                         if path.is_file() and ".git" not in path.parts)[:200]

    changed: list[str] = []
    for name in listing:
        try:
            path = resolve(ctx, str(name), must_exist=True)
        except ToolError:
            continue
        if path.is_dir():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        if pattern not in text:
            continue
        updated = text.replace(pattern, replacement)
        if ctx.checkpoints is not None:
            ctx.checkpoints.snapshot(path)
        path.write_text(updated, encoding="utf-8")
        count = text.count(pattern)
        changed.append(f"{rel(ctx, path)} — {count} замен")

    if not changed:
        return ToolResult.done(f"Фрагмент {clip(pattern, 40)!r} не найден ни в одном файле.",
                               display="replace_in_files (0)")
    return ToolResult.done("Массовая замена выполнена:\n" + "\n".join(changed),
                           display=f"replace_in_files ({len(changed)} файлов)")


TOOLS = [
    Tool("apply_patch", "Применить патч с блоками SEARCH/REPLACE — удобно для нескольких правок сразу.",
         {"patch": {"type": "string", "description": "текст патча с блоками <<<<<<< SEARCH … >>>>>>> REPLACE"},
          "path": {"type": "string", "description": "файл, к которому применяется патч"}},
         apply_patch, required=("patch", "path"), writes=True, category="files"),

    Tool("preview_patch", "Показать будущие изменения по патчу, ничего не записывая (dry-run).",
         {"patch": {"type": "string", "description": "текст патча"},
          "path": {"type": "string", "description": "файл"}},
         preview_patch, required=("patch", "path"), category="files"),

    Tool("replace_in_files", "Массовая замена фрагмента по многим файлам (переименование и т.п.).",
         {"find": {"type": "string", "description": "что искать (точная подстрока)"},
          "replace": {"type": "string", "description": "на что заменить"},
          "files": {"type": "array", "description": "список файлов", "items": {"type": "string"}},
          "file_glob": {"type": "string", "description": "маска файлов, напр. *.py"}},
         replace_in_files, required=("find", "replace"), writes=True, category="files"),
]

__all__ = ["TOOLS", "apply_patch", "preview_patch", "replace_in_files"]
