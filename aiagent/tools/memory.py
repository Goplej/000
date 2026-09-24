"""Память агента: факты о проекте и заметки, которые переживают сессии.

Два уровня:
  • память проекта (~/.aiagent/projects/<проект>/memory.md) — «стек, команды, соглашения»;
  • правила проекта (AIAGENT.md в репозитории) — то, что можно коммитить команде.
"""
from __future__ import annotations

import time
from pathlib import Path
from typing import Any

from ..paths import project_paths
from ..utils.text import clip
from . import Tool, ToolContext, ToolError, ToolResult
from .files import rel, resolve

MEMORY_HEADER = "# Память проекта (ведёт ИИ-агент)\n\n"


def _memory_path(ctx: ToolContext) -> Path:
    return project_paths(ctx.workspace).memory_file


def memory_write(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    content = str(args.get("content") or args.get("fact") or "").strip()
    if not content:
        raise ToolError("нужен текст факта (content)")
    title = str(args.get("title") or "").strip()
    replace = bool(args.get("replace"))

    path = _memory_path(ctx)
    path.parent.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y-%m-%d %H:%M")

    if replace:
        path.write_text(MEMORY_HEADER + f"## {title or 'Факты'} ({stamp})\n{content}\n", encoding="utf-8")
    else:
        if not path.exists():
            path.write_text(MEMORY_HEADER, encoding="utf-8")
        with path.open("a", encoding="utf-8") as handle:
            handle.write(f"\n## {title or 'Заметка'} ({stamp})\n{content}\n")
    return ToolResult.done(f"Записано в память проекта ({path.name}).",
                           display=f"memory_write {clip(title or content, 40)}")


def memory_read(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    path = _memory_path(ctx)
    if not path.is_file():
        return ToolResult.done("Память проекта пока пуста.", display="memory_read (пусто)")
    text = path.read_text(encoding="utf-8", errors="replace")
    tail = int(args.get("last_chars") or 6000)
    if len(text) > tail:
        text = "…(начало опущено)\n" + text[-tail:]
    return ToolResult.done(text, display="memory_read")


def project_rules_write(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    """Дописывает правило в AIAGENT.md — файл инструкций, который читает агент."""
    rule = str(args.get("rule") or args.get("content") or "").strip()
    if not rule:
        raise ToolError("нужен текст правила (rule)")
    path = Path(ctx.workspace) / "AIAGENT.md"
    if not path.exists():
        path.write_text(
            "# Инструкции для ИИ-агента\n\n"
            "Этот файл агент читает в начале каждой сессии. Пиши сюда правила проекта:\n"
            "стек, соглашения, команды проверки.\n\n",
            encoding="utf-8",
        )
    with path.open("a", encoding="utf-8") as handle:
        handle.write(f"- {rule}\n")
    return ToolResult.done(f"Правило добавлено в AIAGENT.md: {clip(rule, 100)}",
                           display="project_rules_write")


def project_rules_read(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    paths = project_paths(ctx.workspace).rules_files
    found = [path for path in paths if path.is_file()]
    if not found:
        return ToolResult.done("Файлов с инструкциями проекта нет (можно создать AIAGENT.md).",
                               display="project_rules_read (нет)")
    chunks = []
    for path in found:
        try:
            body = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        chunks.append(f"### {rel(ctx, path)}\n{clip(body, 4000)}")
    return ToolResult.done("\n\n".join(chunks), display=f"project_rules_read ({len(found)})")


def gitignore_add(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    """Аккуратно добавляет шаблоны в .gitignore проекта."""
    patterns = args.get("patterns") or []
    if isinstance(patterns, str):
        patterns = [line.strip() for line in patterns.splitlines() if line.strip()]
    if not patterns:
        raise ToolError("нужен список patterns")
    path = resolve(ctx, ".gitignore")
    existing = path.read_text(encoding="utf-8") if path.exists() else ""
    lines = existing.splitlines()
    added = [pattern for pattern in patterns if pattern not in lines]
    if not added:
        return ToolResult.done("Всё уже в .gitignore.", display="gitignore_add (нечего добавлять)")
    if ctx.checkpoints is not None and path.exists():
        ctx.checkpoints.snapshot(path)
    block = ("\n# добавлено ИИ-агентом\n" if existing else "") + "\n".join(added) + "\n"
    path.write_text(existing + block, encoding="utf-8")
    return ToolResult.done(f"Добавлено в .gitignore: {', '.join(added)}", display="gitignore_add")


TOOLS = [
    Tool("memory_write", "Запомнить факт о проекте навсегда (стек, команды, соглашения). "
                         "Память переживает перезапуск и не тратит контекст.",
         {"content": {"type": "string", "description": "что запомнить"},
          "title": {"type": "string", "description": "краткий заголовок"},
          "replace": {"type": "boolean", "description": "перезаписать всю память"}},
         memory_write, required=("content",), category="memory"),

    Tool("memory_read", "Прочитать память проекта (факты, записанные ранее).",
         {"last_chars": {"type": "integer", "description": "сколько символов с конца"}},
         memory_read, category="memory"),

    Tool("project_rules_write", "Добавить правило в AIAGENT.md — инструкции проекта для агента.",
         {"rule": {"type": "string", "description": "текст правила"}},
         project_rules_write, required=(), writes=True, category="memory"),

    Tool("project_rules_read", "Прочитать инструкции проекта (AIAGENT.md / AGENT.md).",
         {}, project_rules_read, category="memory"),

    Tool("gitignore_add", "Добавить шаблоны в .gitignore (например, .venv/, *.log).",
         {"patterns": {"type": "array", "description": "шаблоны", "items": {"type": "string"}}},
         gitignore_add, required=("patterns",), writes=True, category="memory"),
]

__all__ = ["TOOLS", "memory_read", "memory_write", "project_rules_read", "project_rules_write"]
