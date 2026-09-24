"""Инструменты агента: единый интерфейс, реестр и сборка набора под задачу.

Каждый инструмент — это:
  • JSON-схема (её видит модель при нативном tool calling);
  • текстовое описание (для моделей, которые пишут вызовы руками);
  • обработчик, который никогда не бросает исключение наружу.

Именно инструменты превращают «чат с моделью» в агента, который реально
создаёт файлы, запускает тесты, ищет в интернете и помнит проект.
"""
from __future__ import annotations

import time
from collections.abc import Callable
from dataclasses import dataclass, field
from typing import Any

from ..config import AgentConfig
from ..core.permissions import Decision, PermissionManager
from ..utils.text import clip, preview

MAX_RESULT_CHARS = 60_000


# --------------------------------------------------------------------------------------
# Результат работы инструмента
# --------------------------------------------------------------------------------------
@dataclass
class ToolResult:
    ok: bool
    content: str = ""
    display: str = ""                 # короткая строка для интерфейса
    meta: dict[str, Any] = field(default_factory=dict)
    seconds: float = 0.0

    def to_message(self, limit: int = 8000) -> str:
        status = "ok" if self.ok else "ошибка"
        body = self.content if len(self.content) <= limit else clip(self.content, limit)
        return f"[результат {status}]\n{body}"

    @classmethod
    def error(cls, message: str, **meta: Any) -> ToolResult:
        return cls(False, message, display=preview(message, 100), meta=meta)

    @classmethod
    def done(cls, content: str, display: str = "", **meta: Any) -> ToolResult:
        return cls(True, content, display=display or preview(content, 100), meta=meta)


class ToolError(Exception):
    """Ожидаемая ошибка инструмента (попадает в текст результата, а не в трейсбек)."""


# --------------------------------------------------------------------------------------
# Контекст исполнения
# --------------------------------------------------------------------------------------
@dataclass
class ToolContext:
    config: AgentConfig
    workspace: Any                       # pathlib.Path
    permissions: PermissionManager
    ui: Any = None                       # интерфейс (для вопросов и подсказок)
    checkpoints: Any = None              # CheckpointManager
    hooks: Any = None                    # HookRunner
    agent: Any = None                    # ссылка на агента (для task/под-агентов)
    extra: dict[str, Any] = field(default_factory=dict)

    def ask(self, title: str, description: str, risk: str = "") -> bool:
        """Спрашивает подтверждение у пользователя (если интерфейс это умеет)."""
        if self.ui is None:
            return False
        body = description if not risk else f"{description}\n\nРиск: {risk}"
        return bool(self.ui.confirm(title, body))

    def note(self, text: str) -> None:
        if self.ui is not None and hasattr(self.ui, "info"):
            self.ui.info(text)


# --------------------------------------------------------------------------------------
# Описание инструмента
# --------------------------------------------------------------------------------------
@dataclass
class Tool:
    name: str
    description: str
    parameters: dict[str, Any]
    handler: Callable[[dict[str, Any], ToolContext], ToolResult]
    required: tuple[str, ...] = ()
    writes: bool = False          # меняет файлы
    dangerous: bool = False       # может навредить (шелл, сети, под-агенты)
    category: str = "general"

    def schema(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "description": self.description,
            "parameters": {
                "type": "object",
                "properties": self.parameters,
                "required": list(self.required),
                "additionalProperties": False,
            },
        }

    def text_spec(self) -> str:
        args = ", ".join(self.parameters) or ""
        required = f" (обязательные: {', '.join(self.required)})" if self.required else ""
        return f"- {self.name}({args}){required}: {self.description}"

    def example(self) -> str:
        sample = {}
        for key in self.required[:2]:
            schema = self.parameters.get(key, {})
            kind = schema.get("type", "string")
            if kind == "integer":
                sample[key] = 1
            elif kind == "boolean":
                sample[key] = True
            elif kind == "array":
                sample[key] = ["пример"]
            elif key in {"path", "file_path"}:
                sample[key] = "example.py"
            elif key == "command":
                sample[key] = "pytest -q"
            elif key == "query":
                sample[key] = "синтаксис python match"
            else:
                sample[key] = "значение"
        import json
        return f'<tool_call>{json.dumps({"name": self.name, "arguments": sample}, ensure_ascii=False)}</tool_call>'

    # ------------------------------------------------------------------- исполнение
    def validate(self, arguments: dict[str, Any]) -> str:
        missing = [key for key in self.required if arguments.get(key) in (None, "")]
        if missing:
            return f"не хватает обязательных параметров: {', '.join(missing)}"
        return ""

    def run(self, arguments: dict[str, Any], ctx: ToolContext) -> ToolResult:
        started = time.time()
        problem = self.validate(arguments or {})
        if problem:
            return ToolResult.error(f"{self.name}: {problem}")

        decision: Decision = ctx.permissions.check(self.name, arguments, workspace=ctx.workspace)
        if not decision.allowed:
            return ToolResult.error(f"{self.name}: действие запрещено ({decision.reason})",
                                    blocked=True)

        if decision.needs_prompt:
            risk = ctx.permissions.risk(self.name, arguments)
            if not ctx.ask(f"Разрешить {self.name}?", _describe_call(self, arguments), risk):
                return ToolResult.error(
                    f"{self.name}: пользователь не разрешил выполнение. "
                    "Предложи другой способ или спроси, что делать дальше.",
                    denied=True,
                )

        if self.writes and ctx.checkpoints is not None:
            ctx.checkpoints.begin(f"{self.name}: {arguments.get('path') or arguments.get('file_path') or ''}")

        hooks_payload = {"tool": self.name, "arguments": _shorten(arguments)}
        if ctx.hooks is not None and ctx.hooks.enabled:
            hook = ctx.hooks.run("pre_tool", hooks_payload)
            if hook.blocked:
                return ToolResult.error(f"{self.name}: заблокировано хуком — {hook.message}")

        try:
            result = self.handler(arguments or {}, ctx)
        except ToolError as e:
            result = ToolResult.error(str(e))
        except PermissionError as e:
            result = ToolResult.error(f"нет доступа: {e}")
        except KeyboardInterrupt:
            raise
        except Exception as e:  # noqa: BLE001 — агент не должен падать из-за инструмента
            result = ToolResult.error(f"{type(e).__name__}: {e}")

        if self.writes and ctx.checkpoints is not None:
            if result.ok:
                ctx.checkpoints.commit(f"{self.name} {_short_path(arguments)}".strip())
            else:
                ctx.checkpoints.discard()

        result.seconds = round(time.time() - started, 2)
        if len(result.content) > MAX_RESULT_CHARS:
            result.content = clip(result.content, MAX_RESULT_CHARS)
        if ctx.hooks is not None and ctx.hooks.enabled:
            ctx.hooks.trigger("post_tool", {**hooks_payload, "ok": result.ok,
                                            "display": result.display})
        return result


def _describe_call(tool: Tool, arguments: dict[str, Any]) -> str:
    import json
    body = json.dumps(_shorten(arguments), ensure_ascii=False, indent=2)
    return clip(body, 1500)


def _shorten(arguments: dict[str, Any], limit: int = 400) -> dict[str, Any]:
    out = {}
    for key, value in (arguments or {}).items():
        out[key] = clip(value, limit) if isinstance(value, str) and len(value) > limit else value
    return out


def _short_path(arguments: dict[str, Any]) -> str:
    return str(arguments.get("path") or arguments.get("file_path") or "")


# --------------------------------------------------------------------------------------
# Реестр и наборы инструментов
# --------------------------------------------------------------------------------------
def build_registry() -> dict[str, Tool]:
    """Собирает все доступные инструменты."""
    from . import files, github, memory, notebook, patch, shell, skills, tasks, web

    tools: list[Tool] = []
    tools += files.TOOLS
    tools += shell.TOOLS
    tools += web.TOOLS
    tools += tasks.TOOLS
    tools += memory.TOOLS
    tools += notebook.TOOLS
    tools += patch.TOOLS
    tools += skills.TOOLS
    tools += github.TOOLS
    return {tool.name: tool for tool in tools}


#: Быстрый старт: то, что нужно 90% задач.
CORE_TOOL_NAMES = (
    "read_file", "write_file", "edit_file", "multi_edit", "list_dir", "glob_search",
    "grep_search", "run_shell", "web_search", "fetch_url", "todo_write", "task", "finish",
)

#: Минимальный набор для слабых моделей.
MINIMAL_TOOL_NAMES = ("read_file", "write_file", "edit_file", "run_shell", "finish")

#: Набор только для чтения (режим plan).
READONLY_TOOL_NAMES = (
    "read_file", "list_dir", "glob_search", "grep_search", "file_info", "web_search",
    "fetch_url", "wikipedia", "site_links", "todo_read", "memory_read", "project_rules_read",
    "notebook_read", "skills_list", "read_skill", "preview_patch",
    "github_status", "github_issues", "github_repo_info", "finish",
)


def select_tools(registry: dict[str, Tool], names: list[str] | None = None,
                 disabled: list[str] | None = None) -> list[Tool]:
    """Возвращает список инструментов согласно конфигурации.

    names — явный перечень (обычно из конфига);
    иначе: сначала основные инструменты, затем остальные.
    """
    if names:
        chosen = [registry[name] for name in names if name in registry]
    else:
        chosen = sorted(registry.values(), key=lambda tool: (tool.name not in CORE_TOOL_NAMES, tool.name))
    if disabled:
        chosen = [tool for tool in chosen if tool.name not in disabled]
    return chosen


def tools_prompt(tools: list[Tool], *, examples: int = 3) -> str:
    """Текстовая инструкция по инструментам (для моделей без нативного tool calling)."""
    lines = [tool.text_spec() for tool in tools]
    samples = "\n".join(tool.example() for tool in tools[:examples])
    return (
        "Доступные инструменты:\n" + "\n".join(lines) +
        "\n\nВызов инструмента — ровно один JSON-объект в теге:\n"
        '<tool_call>{"name": "имя_инструмента", "arguments": {"параметр": "значение"}}</tool_call>\n\n'
        f"Примеры:\n{samples}"
    )


__all__ = [
    "CORE_TOOL_NAMES", "MINIMAL_TOOL_NAMES", "READONLY_TOOL_NAMES", "Tool", "ToolContext",
    "ToolError", "ToolResult", "build_registry", "select_tools", "tools_prompt",
]
