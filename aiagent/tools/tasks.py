"""Инструменты планирования и под-задач: todo, под-агенты, завершение работы."""
from __future__ import annotations

import json
import time
from pathlib import Path
from typing import Any

from ..paths import project_paths
from ..utils.text import clip, plural_ru
from . import Tool, ToolContext, ToolError, ToolResult

STATUS_MARKS = {"done": "[x]", "doing": "[~]", "skipped": "[-]", "pending": "[ ]"}
AGENT_TYPES = ("general", "researcher", "coder", "reviewer", "tester", "planner")


# --------------------------------------------------------------------------------------
# TODO-список (виден пользователю и держит агента в фокусе)
# --------------------------------------------------------------------------------------
def _todo_path(ctx: ToolContext) -> Path:
    return project_paths(ctx.workspace).todo_file


def todo_write(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    raw = args.get("items")
    if raw is None:
        raw = args.get("todos") or []
    if isinstance(raw, str):
        raw = [line.strip("-*• \t") for line in raw.splitlines() if line.strip()]

    items: list[dict[str, str]] = []
    for entry in raw if isinstance(raw, list) else []:
        if isinstance(entry, dict):
            text = str(entry.get("text") or entry.get("task") or entry.get("title") or "").strip()
            status = str(entry.get("status") or "pending").lower()
        else:
            text, status = str(entry).strip(), "pending"
            for mark, mapped in STATUS_MARKS.items():
                if text.lower().startswith(mark):
                    text, status = text[len(mark):].strip(), mapped
                    break
        if not text:
            continue
        if status not in STATUS_MARKS:
            status = "pending"
        items.append({"text": text, "status": status})

    path = _todo_path(ctx)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({"items": items, "updated": time.time()}, ensure_ascii=False, indent=2),
                    encoding="utf-8")
    if not items:
        return ToolResult.done("План очищен.", display="todo_write (очищено)")

    done = sum(1 for item in items if item["status"] == "done")
    lines = [f"{STATUS_MARKS[item['status']]} {item['text']}" for item in items]
    return ToolResult.done(
        f"План сохранён ({done} из {len(items)} готово):\n" + "\n".join(lines),
        display=f"todo_write ({done}/{len(items)})", total=len(items), done=done,
    )


def todo_read(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    path = _todo_path(ctx)
    if not path.is_file():
        return ToolResult.done("План пуст — можно создать через todo_write.",
                               display="todo_read (пусто)")
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return ToolResult.error("файл плана повреждён")
    items = data.get("items") or []
    if not items:
        return ToolResult.done("План пуст.", display="todo_read (пусто)")
    lines = [f"{STATUS_MARKS.get(item.get('status', 'pending'), '[ ]')} {item.get('text', '')}"
             for item in items]
    done = sum(1 for item in items if item.get("status") == "done")
    return ToolResult.done(f"План ({done}/{len(items)}):\n" + "\n".join(lines),
                           display=f"todo_read ({done}/{len(items)})")


# --------------------------------------------------------------------------------------
# Под-агенты: параллельная работа над подзадачами
# --------------------------------------------------------------------------------------
def task(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    """
    Запускает отдельного агента с чистой памятью и своим набором инструментов.
    Вложенные под-агенты запрещены, чтобы не уйти в бесконечную рекурсию.
    """
    description = str(args.get("description") or args.get("prompt") or "").strip()
    if not description:
        raise ToolError("нужно описание подзадачи (description)")

    if ctx.agent is None:
        # Под-агенты недоступны (например, вызов из самого под-агента)
        return ToolResult.error("под-агенты недоступны в этом режиме — выполни задачу сам")

    agent_type = str(args.get("agent_type") or args.get("type") or "general").lower()
    if agent_type not in AGENT_TYPES:
        agent_type = "general"

    from ..core.subagent import run_subagent  # импорт внутри, чтобы избежать циклов

    result = run_subagent(
        parent=ctx.agent,
        description=description,
        agent_type=agent_type,
        max_steps=int(args.get("max_steps") or 25),
        tools_filter=args.get("tools"),
        context=args.get("context") or "",
    )
    ok = not result.startswith("[под-агент: ошибка")
    return ToolResult(ok, result, display=f"task({agent_type}) — {clip(description, 60)}",
                      meta={"agent_type": agent_type})


# --------------------------------------------------------------------------------------
# Завершение
# --------------------------------------------------------------------------------------
def finish(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    """Явный сигнал «задача выполнена» с итогом для пользователя."""
    summary = str(args.get("summary") or args.get("message") or args.get("result") or "").strip()
    if not summary:
        summary = "Задача выполнена."
    files = args.get("files") or []
    if files:
        summary += "\nИзменённые файлы: " + ", ".join(str(f) for f in files)
    return ToolResult.done(summary, display="finish", summary=summary)


def ask_user(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    """Задать пользователю уточняющий вопрос и дождаться ответа."""
    question = str(args.get("question") or "").strip()
    if not question:
        raise ToolError("нужен текст вопроса")
    options = args.get("options") or []
    if options:
        question += "\nВарианты: " + ", ".join(str(option) for option in options)

    if ctx.ui is None or not hasattr(ctx.ui, "ask"):
        return ToolResult.error("интерфейс не поддерживает вопросы — сформулируй предположение и продолжай")
    answer = ctx.ui.ask(f"\n? {question}\n› ")
    if not answer:
        return ToolResult.done("Пользователь не ответил — действуй по разумному умолчанию и отметь это в итоге.",
                               display="ask_user (нет ответа)")
    return ToolResult.done(f"Ответ пользователя: {answer}", display=f"ask_user → {clip(answer, 40)}")


def update_plan(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    """Отмечает пункт плана выполненным (короткая форма todo_write)."""
    item = str(args.get("item") or "").strip()
    status = str(args.get("status") or "done").lower()
    if not item:
        raise ToolError("нужен текст пункта плана")
    path = _todo_path(ctx)
    data = {"items": []}
    if path.is_file():
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            data = {"items": []}

    matched = False
    for entry in data.get("items", []):
        if item.lower() in str(entry.get("text", "")).lower():
            entry["status"] = status
            matched = True
    if not matched:
        data.setdefault("items", []).append({"text": item, "status": status})
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")

    items = data.get("items", [])
    done = sum(1 for entry in items if entry.get("status") == "done")
    left = len(items) - done
    tail = ""
    if left:
        tail = f" Осталось {left} {plural_ru(left, 'пункт', 'пункта', 'пунктов')}."
    return ToolResult.done(f"Отмечено: «{clip(item, 60)}» → {status}.{tail}",
                           display=f"update_plan ({done}/{len(items)})")


TOOLS = [
    Tool("todo_write", "Записать план работы (список задач). Помогает не терять шаги в длинных задачах.",
         {"items": {"type": "array", "description": "задачи: строки или объекты {text,status}",
                    "items": {"type": "object"}}},
         todo_write, required=("items",), category="planning"),

    Tool("todo_read", "Прочитать текущий план работы.", {}, todo_read, category="planning"),

    Tool("update_plan", "Отметить пункт плана выполненным или изменить его статус.",
         {"item": {"type": "string", "description": "текст пункта (можно часть)"},
          "status": {"type": "string", "description": "done | doing | pending | skipped"}},
         update_plan, required=("item",), category="planning"),

    Tool("task", "Запустить под-агента для отдельной подзадачи (исследование, код, ревью, тесты). "
                 "Работает параллельно с основной задачей.",
         {"description": {"type": "string", "description": "подробное описание подзадачи"},
          "agent_type": {"type": "string", "description": "general | researcher | coder | reviewer | tester | planner"},
          "context": {"type": "string", "description": "контекст, который нужно передать под-агенту"},
          "tools": {"type": "array", "description": "ограничить набор инструментов",
                    "items": {"type": "string"}},
          "max_steps": {"type": "integer", "description": "лимит шагов под-агента"}},
         task, required=("description",), dangerous=True, category="planning"),

    Tool("ask_user", "Задать пользователю уточняющий вопрос (если без ответа нельзя двигаться).",
         {"question": {"type": "string", "description": "вопрос"},
          "options": {"type": "array", "description": "варианты ответа", "items": {"type": "string"}}},
         ask_user, required=("question",), category="planning"),

    Tool("finish", "Сообщить, что задача полностью выполнена (с кратким итогом для пользователя).",
         {"summary": {"type": "string", "description": "что именно сделано (2-5 строк)"},
          "files": {"type": "array", "description": "изменённые файлы", "items": {"type": "string"}}},
         finish, required=("summary",), category="planning"),
]

__all__ = ["AGENT_TYPES", "TOOLS", "ask_user", "finish", "task", "todo_read", "todo_write",
           "update_plan"]
