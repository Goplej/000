"""Инструменты работы с файлами: чтение, запись, правка, поиск, перемещение."""
from __future__ import annotations

import fnmatch
import os
import re
import shutil
from pathlib import Path
from typing import Any

from ..utils.diff import diff_stats, unified_diff
from ..utils.security import safe_path
from ..utils.text import clip, count_lines, human_size, is_binary, preview
from . import Tool, ToolContext, ToolError, ToolResult

SKIP_DIRS = {
    ".git", "node_modules", "__pycache__", ".venv", "venv", "env", "dist", "build",
    "target", ".mypy_cache", ".pytest_cache", ".ruff_cache", ".next", ".nuxt", "coverage",
    ".idea", ".vscode", ".cache", ".aiagent", ".aia", "site-packages", ".tox", ".eggs",
}
MAX_READ_LINES = 2000
MAX_FILE_BYTES = 600_000


def resolve(ctx: ToolContext, raw: str, *, must_exist: bool = False) -> Path:
    """Безопасный путь внутри рабочей папки (и доп. каталогов из конфига)."""
    extra = list(ctx.config.extra_dirs) + list(ctx.config.permissions.extra_dirs)
    try:
        return safe_path(raw or ".", Path(ctx.workspace), extra, must_exist=must_exist)
    except PermissionError as e:
        raise ToolError(str(e)) from e
    except FileNotFoundError as e:
        raise ToolError(str(e)) from e


def rel(ctx: ToolContext, path: Path) -> str:
    try:
        return str(Path(path).relative_to(Path(ctx.workspace)))
    except ValueError:
        return str(path)


# --------------------------------------------------------------------------------------
# Чтение
# --------------------------------------------------------------------------------------
def read_file(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    path = resolve(ctx, args["path"], must_exist=True)
    if path.is_dir():
        raise ToolError(f"{rel(ctx, path)} — это каталог, используй list_dir")

    data = path.read_bytes()
    if is_binary(data) and path.suffix.lower() not in {".pdf", ".png", ".jpg", ".zip"}:
        return ToolResult.done(f"{rel(ctx, path)} — бинарный файл ({human_size(len(data))}). "
                               f"Чтение текстом невозможно.",
                               display=f"read_file {rel(ctx, path)} (бинарный)")
    if len(data) > MAX_FILE_BYTES and not args.get("force"):
        return ToolResult.error(
            f"Файл слишком большой ({human_size(len(data))}, {count_lines(data.decode('utf-8', 'replace'))} строк). "
            "Укажи start_line/end_line или используй grep_search, чтобы найти нужное место."
        )

    text = data.decode("utf-8", "replace")
    lines = text.splitlines()
    start = max(1, int(args.get("start_line") or 1))
    end = int(args.get("end_line") or 0) or min(len(lines), start + MAX_READ_LINES - 1)
    end = min(end, len(lines))
    chunk = lines[start - 1:end]
    numbered = "\n".join(f"{index + start:>6}| {line}" for index, line in enumerate(chunk))
    header = f"{rel(ctx, path)} — строки {start}–{end} из {len(lines)} ({human_size(len(data))})"
    return ToolResult.done(f"{header}\n{numbered}",
                           display=f"read_file {rel(ctx, path)}:{start}-{end}",
                           path=str(path), lines=len(lines))


def list_dir(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    root = resolve(ctx, args.get("path") or ".", must_exist=True)
    depth = max(1, min(int(args.get("depth") or 2), 6))
    show_hidden = bool(args.get("hidden"))
    want_sizes = bool(args.get("sizes", True))
    limit = int(args.get("limit") or 400)

    lines: list[str] = []
    count = 0

    def walk(directory: Path, level: int) -> None:
        nonlocal count
        if count >= limit:
            return
        try:
            entries = sorted(directory.iterdir(), key=lambda p: (p.is_file(), p.name.lower()))
        except PermissionError:
            lines.append("  " * level + "(нет доступа)")
            return
        for entry in entries:
            if count >= limit:
                lines.append("  " * level + "… (список обрезан)")
                return
            if not show_hidden and entry.name.startswith("."):
                continue
            if entry.is_dir():
                if entry.name in SKIP_DIRS:
                    continue
                lines.append("  " * level + f"{entry.name}/")
                if level + 1 < depth:
                    walk(entry, level + 1)
            else:
                size = ""
                if want_sizes:
                    try:
                        size = f"  ({human_size(entry.stat().st_size)})"
                    except OSError:
                        size = ""
                lines.append("  " * level + f"{entry.name}{size}")
                count += 1

    lines.append(f"{rel(ctx, root) or '.'}/")
    walk(root, 1)
    if not lines[1:]:
        lines.append("  (пусто)")
    return ToolResult.done("\n".join(lines), display=f"list_dir {rel(ctx, root)}",
                           entries=count)


# --------------------------------------------------------------------------------------
# Запись и правка
# --------------------------------------------------------------------------------------
def write_file(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    path = resolve(ctx, args["path"])
    content = args.get("content")
    if content is None:
        raise ToolError("не передан параметр content")
    if path.exists() and path.is_dir():
        raise ToolError(f"{rel(ctx, path)} — это каталог")

    old = ""
    existed = path.exists()
    if existed:
        old = path.read_text(encoding="utf-8", errors="replace")

    if ctx.checkpoints is not None:
        ctx.checkpoints.snapshot(path)

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(str(content), encoding="utf-8")

    stats = diff_stats(old, str(content)) if existed else None
    action = "перезаписан" if existed else "создан"
    summary = f"Файл {rel(ctx, path)} {action}: {count_lines(str(content))} строк, {human_size(len(str(content)))}"
    if stats and stats.total:
        summary += f" ({stats.summary()})"
    if existed and stats and stats.total:
        diff = unified_diff(old, str(content), filename=rel(ctx, path))
        summary += "\n\n" + clip(diff, 4000, note="…diff обрезан")
    return ToolResult.done(summary, display=f"write_file {rel(ctx, path)} [{action}]",
                           path=str(path), created=not existed)


def edit_file(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    path = resolve(ctx, args["path"], must_exist=True)
    old_text = args.get("old_text") or args.get("old_string")
    new_text = args.get("new_text")
    if new_text is None:
        new_text = args.get("new_string")
    if old_text is None or new_text is None:
        raise ToolError("нужны параметры old_text и new_text")
    old_text, new_text = str(old_text), str(new_text)

    content = path.read_text(encoding="utf-8", errors="replace")
    occurrences = content.count(old_text)
    replace_all = bool(args.get("replace_all"))

    if occurrences == 0:
        hint = _not_found_hint(content, old_text)
        raise ToolError(f"фрагмент не найден в {rel(ctx, path)}.{hint}")
    if occurrences > 1 and not replace_all:
        return ToolResult.error(
            f"фрагмент встречается {occurrences} раз — добавь контекста в old_text "
            f"или передай replace_all=true"
        )

    if ctx.checkpoints is not None:
        ctx.checkpoints.snapshot(path)

    updated = content.replace(old_text, new_text) if replace_all else content.replace(old_text, new_text, 1)
    path.write_text(updated, encoding="utf-8")
    stats = diff_stats(content, updated)
    replaced = occurrences if replace_all else 1
    diff = unified_diff(content, updated, filename=rel(ctx, path), context=2)
    return ToolResult.done(
        f"{rel(ctx, path)}: заменено {replaced} ({stats.summary()})\n\n" + clip(diff, 3000, note="…diff обрезан"),
        display=f"edit_file {rel(ctx, path)} (×{replaced})",
        path=str(path), replaced=replaced,
    )


def multi_edit(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    """Несколько правок одного файла за один вызов — экономит шаги агента."""
    edits = args.get("edits") or []
    if not isinstance(edits, list) or not edits:
        raise ToolError("нужен непустой список edits=[{old_text, new_text}, …]")

    path = resolve(ctx, args["path"], must_exist=True)
    content = path.read_text(encoding="utf-8", errors="replace")
    original = content
    applied = 0

    for index, edit in enumerate(edits, 1):
        if not isinstance(edit, dict):
            raise ToolError(f"edits[{index}] должен быть объектом с old_text/new_text")
        old_text = str(edit.get("old_text") or edit.get("old_string") or "")
        new_text = str(edit.get("new_text") if edit.get("new_text") is not None
                       else edit.get("new_string", ""))
        if not old_text:
            raise ToolError(f"edits[{index}]: пустой old_text")
        if old_text not in content:
            raise ToolError(f"edits[{index}]: фрагмент не найден{_not_found_hint(content, old_text)}")
        content = content.replace(old_text, new_text, 1)
        applied += 1

    if ctx.checkpoints is not None:
        ctx.checkpoints.snapshot(path)
    path.write_text(content, encoding="utf-8")
    stats = diff_stats(original, content)
    return ToolResult.done(
        f"{rel(ctx, path)}: применено правок {applied} ({stats.summary()})",
        display=f"multi_edit {rel(ctx, path)} ({applied})", path=str(path),
    )


def delete_file(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    path = resolve(ctx, args["path"], must_exist=True)
    if path.is_dir():
        if not args.get("recursive"):
            raise ToolError("это каталог — передай recursive=true, если действительно нужно удалить его целиком")
        if ctx.checkpoints is not None:
            ctx.checkpoints.begin(f"delete_dir {rel(ctx, path)}")
        shutil.rmtree(path)
        return ToolResult.done(f"каталог {rel(ctx, path)} удалён (откат — /undo)",
                               display=f"delete_dir {rel(ctx, path)}")
    if ctx.checkpoints is not None:
        ctx.checkpoints.snapshot(path)
    path.unlink()
    return ToolResult.done(f"файл {rel(ctx, path)} удалён (можно вернуть: /undo)",
                           display=f"delete_file {rel(ctx, path)}", path=str(path))


def move_file(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    source = resolve(ctx, args["source"], must_exist=True)
    target = resolve(ctx, args["target"])
    if target.exists() and not args.get("overwrite"):
        return ToolResult.error(f"{rel(ctx, target)} уже существует (передай overwrite=true)")
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.move(str(source), str(target))
    return ToolResult.done(f"{rel(ctx, source)} → {rel(ctx, target)}",
                           display=f"move {rel(ctx, source)} → {rel(ctx, target)}")


# --------------------------------------------------------------------------------------
# Поиск
# --------------------------------------------------------------------------------------
def glob_search(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    pattern = args.get("pattern") or "**/*"
    root = resolve(ctx, args.get("path") or ".", must_exist=True)
    limit = int(args.get("max_results") or 200)
    newest_first = bool(args.get("sort_by_time"))

    matches: list[tuple[float, str]] = []
    for path in root.rglob("*"):
        if any(part in SKIP_DIRS for part in path.parts):
            continue
        if not path.is_file():
            continue
        relative = rel(ctx, path)
        if (fnmatch.fnmatch(relative, pattern) or fnmatch.fnmatch(path.name, pattern)
                or path.match(pattern)):
            try:
                mtime = path.stat().st_mtime
                size = human_size(path.stat().st_size)
            except OSError:
                mtime, size = 0.0, "?"
            matches.append((mtime, f"{relative}  ({size})"))

    if newest_first:
        matches.sort(key=lambda item: item[0], reverse=True)
    else:
        matches.sort(key=lambda item: item[1])

    files = [item[1] for item in matches[:limit]]
    if not files:
        return ToolResult.done(f"по шаблону {pattern} ничего не найдено")
    tail = f"\n… ещё {len(matches) - limit}" if len(matches) > limit else ""
    return ToolResult.done(f"найдено файлов: {len(matches)}\n" + "\n".join(files) + tail,
                           display=f"glob {pattern} ({len(matches)})")


def grep_search(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    pattern = str(args.get("pattern") or "")
    if not pattern:
        raise ToolError("пустой pattern")
    root = resolve(ctx, args.get("path") or ".", must_exist=True)
    file_glob = args.get("file_glob") or "*"
    limit = int(args.get("max_results") or 80)
    use_regex = bool(args.get("regex", True))
    ignore_case = bool(args.get("ignore_case", True))
    context_lines = int(args.get("context") or 0)

    flags = re.IGNORECASE if ignore_case else 0
    try:
        regex = re.compile(pattern if use_regex else re.escape(pattern), flags)
    except re.error as e:
        raise ToolError(f"некорректное регулярное выражение: {e}") from e

    hits: list[str] = []
    scanned = 0
    for directory, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS and not d.startswith(".git")]
        for name in filenames:
            if not fnmatch.fnmatch(name, file_glob):
                continue
            path = Path(directory) / name
            try:
                if path.stat().st_size > 3_000_000:
                    continue
                data = path.read_bytes()
            except OSError:
                continue
            if is_binary(data):
                continue
            scanned += 1
            lines = data.decode("utf-8", "replace").splitlines()
            for index, line in enumerate(lines, 1):
                if regex.search(line):
                    label = f"{rel(ctx, path)}:{index}"
                    hits.append(f"{label}: {line.strip()[:240]}")
                    for offset in range(1, context_lines + 1):
                        if index + offset <= len(lines):
                            hits.append(f"{label}+{offset}: {lines[index + offset - 1].strip()[:200]}")
                    if len(hits) >= limit:
                        break
            if len(hits) >= limit:
                break
        if len(hits) >= limit:
            break

    if not hits:
        return ToolResult.done(f"по запросу «{pattern}» ничего не найдено (проверено файлов: {scanned})")
    tail = "\n… (показаны не все совпадения)" if len(hits) >= limit else ""
    return ToolResult.done(f"совпадений: {len(hits)} (файлов проверено: {scanned})\n"
                           + "\n".join(hits) + tail,
                           display=f"grep «{preview(pattern, 40)}» → {len(hits)}")


def file_info(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    path = resolve(ctx, args["path"], must_exist=True)
    stat = path.stat()
    lines = [
        f"путь: {rel(ctx, path)}",
        f"размер: {human_size(stat.st_size)}",
        f"строк: {count_lines(path.read_text(encoding='utf-8', errors='replace'))}"
        if stat.st_size < 2_000_000 else "строк: (файл большой)",
        f"изменён: {__import__('time').strftime('%Y-%m-%d %H:%M:%S', __import__('time').localtime(stat.st_mtime))}",
    ]
    return ToolResult.done("\n".join(lines), display=f"file_info {rel(ctx, path)}")


def _not_found_hint(content: str, needle: str) -> str:
    """Подсказка модели: как именно не совпал фрагмент."""
    stripped = needle.strip()
    if not stripped:
        return " Фрагмент пустой."
    if stripped in content:
        return " Фрагмент найден без крайних пробелов — проверь отступы в начале и конце."
    first = stripped.splitlines()[0].strip()
    if first and first in content:
        return f" Первая строка фрагмента присутствует ({preview(first, 60)!r}) — вероятно, отличаются остальные строки или отступы."
    # похожие строки
    words = [word for word in re.findall(r"\w{4,}", stripped)[:4]]
    for word in words:
        if word in content:
            return f" Слово {word!r} в файле есть — проверь точный текст вокруг него (read_file)."
    return " Проверь файл через read_file и скопируй фрагмент точно."


TOOLS = [
    Tool("read_file", "Прочитать файл с нумерацией строк. Большие файлы читай частями (start_line/end_line).",
         {"path": {"type": "string", "description": "путь к файлу"},
          "start_line": {"type": "integer", "description": "первая строка (по умолчанию 1)"},
          "end_line": {"type": "integer", "description": "последняя строка"},
          "force": {"type": "boolean", "description": "читать даже очень большой файл"}},
         read_file, required=("path",), category="files"),

    Tool("write_file", "Создать файл или полностью перезаписать его содержимое.",
         {"path": {"type": "string", "description": "путь к файлу"},
          "content": {"type": "string", "description": "полное содержимое файла"}},
         write_file, required=("path", "content"), writes=True, category="files"),

    Tool("edit_file", "Заменить точный фрагмент в существующем файле (точечная правка).",
         {"path": {"type": "string", "description": "путь к файлу"},
          "old_text": {"type": "string", "description": "фрагмент, который заменяем (точно как в файле)"},
          "new_text": {"type": "string", "description": "на что заменить"},
          "replace_all": {"type": "boolean", "description": "заменить все вхождения"}},
         edit_file, required=("path", "old_text", "new_text"), writes=True, category="files"),

    Tool("multi_edit", "Несколько правок одного файла одним вызовом (быстрее и надёжнее).",
         {"path": {"type": "string", "description": "путь к файлу"},
          "edits": {"type": "array", "description": "список правок [{old_text, new_text}, …]",
                    "items": {"type": "object"}}},
         multi_edit, required=("path", "edits"), writes=True, category="files"),

    Tool("list_dir", "Показать структуру каталога (файлы, папки, размеры).",
         {"path": {"type": "string", "description": "каталог (по умолчанию текущий)"},
          "depth": {"type": "integer", "description": "глубина обхода (1-6)"},
          "hidden": {"type": "boolean", "description": "показывать скрытые файлы"},
          "limit": {"type": "integer", "description": "максимум записей"}},
         list_dir, category="files"),

    Tool("glob_search", "Найти файлы по маске, напр. **/*.py или src/**/*.tsx.",
         {"pattern": {"type": "string", "description": "маска поиска"},
          "path": {"type": "string", "description": "где искать"},
          "max_results": {"type": "integer", "description": "максимум результатов"},
          "sort_by_time": {"type": "boolean", "description": "сначала недавно изменённые"}},
         glob_search, required=("pattern",), category="files"),

    Tool("grep_search", "Найти текст/регулярку внутри файлов проекта — быстрый способ понять код.",
         {"pattern": {"type": "string", "description": "что искать (регулярка или подстрока)"},
          "path": {"type": "string", "description": "где искать"},
          "file_glob": {"type": "string", "description": "фильтр файлов, напр. *.py"},
          "max_results": {"type": "integer", "description": "максимум совпадений"},
          "regex": {"type": "boolean", "description": "считать pattern регуляркой (по умолчанию да)"},
          "ignore_case": {"type": "boolean", "description": "игнорировать регистр"},
          "context": {"type": "integer", "description": "сколько строк контекста после совпадения"}},
         grep_search, required=("pattern",), category="files"),

    Tool("delete_file", "Удалить файл или каталог (откатывается командой /undo).",
         {"path": {"type": "string", "description": "что удалить"},
          "recursive": {"type": "boolean", "description": "удалить каталог целиком"}},
         delete_file, required=("path",), writes=True, dangerous=True, category="files"),

    Tool("move_file", "Переместить или переименовать файл.",
         {"source": {"type": "string", "description": "откуда"},
          "target": {"type": "string", "description": "куда"},
          "overwrite": {"type": "boolean", "description": "перезаписать, если файл существует"}},
         move_file, required=("source", "target"), writes=True, category="files"),

    Tool("file_info", "Метаданные файла: размер, число строк, время изменения.",
         {"path": {"type": "string", "description": "путь"}},
         file_info, required=("path",), category="files"),
]

__all__ = ["TOOLS", "read_file", "write_file", "edit_file", "multi_edit", "list_dir",
           "glob_search", "grep_search", "delete_file", "move_file", "file_info", "resolve", "rel"]
