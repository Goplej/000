"""Работа с Jupyter-ноутбуками (.ipynb) и запуск кода в них.

Ноутбук — обычный JSON, поэтому его можно читать и править без зависимостей.
Запуск ячеек возможен только если установлен jupyter/nbconvert; иначе предлагаем
сохранить код в .py и запустить через run_shell.
"""
from __future__ import annotations

import json
import shutil
from pathlib import Path
from typing import Any

from ..utils.text import clip
from . import Tool, ToolContext, ToolError, ToolResult
from .files import rel, resolve


def _load(path: Path) -> dict[str, Any]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as e:
        raise ToolError(f"не удалось прочитать ноутбук: {e}") from e
    if not isinstance(data, dict) or "cells" not in data:
        raise ToolError("файл не похож на Jupyter-ноутбук")
    return data


def _cell_source(cell: dict[str, Any]) -> str:
    source = cell.get("source") or []
    return "".join(source) if isinstance(source, list) else str(source)


def notebook_read(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    path = resolve(ctx, args["path"], must_exist=True)
    notebook = _load(path)
    cells = notebook.get("cells", [])
    max_cells = int(args.get("max_cells") or 60)
    only_code = bool(args.get("only_code"))

    lines = [f"{rel(ctx, path)}: ячеек {len(cells)}"]
    for index, cell in enumerate(cells[:max_cells]):
        kind = cell.get("cell_type", "?")
        if only_code and kind != "code":
            continue
        source = _cell_source(cell).rstrip()
        lines.append(f"\n── [{index}] {kind} " + "─" * 30)
        lines.append(clip(source, 2000))
        outputs = cell.get("outputs") or []
        for output in outputs[:3]:
            text = output.get("text") or output.get("data", {}).get("text/plain") or ""
            text = "".join(text) if isinstance(text, list) else str(text)
            if text.strip():
                lines.append("   вывод: " + clip(text.strip(), 600))
    if len(cells) > max_cells:
        lines.append(f"\n… ещё ячеек: {len(cells) - max_cells}")
    return ToolResult.done("\n".join(lines), display=f"notebook_read {rel(ctx, path)}")


def notebook_edit(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    """Меняет, добавляет или удаляет ячейку ноутбука."""
    path = resolve(ctx, args["path"], must_exist=True)
    notebook = _load(path)
    cells = notebook.setdefault("cells", [])
    action = str(args.get("action") or "replace").lower()
    index = int(args.get("index", -1))
    source = str(args.get("source") or "")
    cell_type = str(args.get("cell_type") or "code")

    if action == "insert":
        position = len(cells) if index < 0 else min(index, len(cells))
        cells.insert(position, _new_cell(cell_type, source))
        result = f"добавлена ячейка {cell_type} на позицию {position}"
    elif action == "delete":
        if not 0 <= index < len(cells):
            raise ToolError(f"ячейка {index} не существует (всего {len(cells)})")
        cells.pop(index)
        result = f"удалена ячейка {index}"
    else:
        if not 0 <= index < len(cells):
            raise ToolError(f"ячейка {index} не существует (всего {len(cells)})")
        cells[index]["source"] = _to_source(source)
        if cell_type:
            cells[index]["cell_type"] = cell_type
        if cell_type == "code":
            cells[index].setdefault("outputs", [])
            cells[index].setdefault("execution_count", None)
        cells[index].pop("outputs", None) if cell_type != "code" else None
        result = f"ячейка {index} обновлена"

    if ctx.checkpoints is not None:
        ctx.checkpoints.snapshot(path)
    path.write_text(json.dumps(notebook, ensure_ascii=False, indent=1), encoding="utf-8")
    return ToolResult.done(f"{rel(ctx, path)}: {result}", display=f"notebook_edit {rel(ctx, path)}")


def notebook_run(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    """Выполняет ноутбук целиком или одну ячейку через jupyter nbconvert."""
    path = resolve(ctx, args["path"], must_exist=True)
    if shutil.which("jupyter") is None and shutil.which("jupyter-nbconvert") is None:
        notebook = _load(path)
        code = "\n\n".join(_cell_source(c) for c in notebook.get("cells", [])
                           if c.get("cell_type") == "code")
        return ToolResult.error(
            "Jupyter не установлен. Код ячеек можно запустить вручную:\n"
            "  1) сохрани код в .py и вызови run_code/run_shell;\n"
            "  2) или установи: pip install jupyter nbconvert\n\n"
            "Код ноутбука (первые 2000 символов):\n" + clip(code, 2000)
        )

    command = f"jupyter nbconvert --to notebook --execute --inplace {shutil.quote(str(path))}"
    from .shell import run_shell
    result = run_shell({"command": command, "timeout": int(args.get("timeout") or 600)}, ctx)
    result.display = f"notebook_run {rel(ctx, path)}"
    return result


def _new_cell(cell_type: str, source: str) -> dict[str, Any]:
    cell: dict[str, Any] = {
        "cell_type": cell_type,
        "metadata": {},
        "source": _to_source(source),
    }
    if cell_type == "code":
        cell["outputs"] = []
        cell["execution_count"] = None
    return cell


def _to_source(text: str) -> list[str]:
    lines = text.splitlines(keepends=True)
    return lines or [""]


TOOLS = [
    Tool("notebook_read", "Прочитать Jupyter-ноутбук: ячейки, код, выводы.",
         {"path": {"type": "string", "description": "путь к .ipynb"},
          "max_cells": {"type": "integer", "description": "сколько ячеек показать"},
          "only_code": {"type": "boolean", "description": "только code-ячейки"}},
         notebook_read, required=("path",), category="notebook"),

    Tool("notebook_edit", "Изменить ноутбук: заменить, добавить или удалить ячейку.",
         {"path": {"type": "string", "description": "путь к .ipynb"},
          "action": {"type": "string", "description": "replace | insert | delete"},
          "index": {"type": "integer", "description": "номер ячейки (с 0)"},
          "source": {"type": "string", "description": "новый код ячейки"},
          "cell_type": {"type": "string", "description": "code | markdown"}},
         notebook_edit, required=("path",), writes=True, category="notebook"),

    Tool("notebook_run", "Выполнить ноутбук целиком (нужен установленный jupyter).",
         {"path": {"type": "string", "description": "путь к .ipynb"},
          "timeout": {"type": "integer", "description": "лимит времени"}},
         notebook_run, required=("path",), dangerous=True, category="notebook"),
]

__all__ = ["TOOLS", "notebook_edit", "notebook_read", "notebook_run"]
