"""Ядро агента: цикл, контекст, разрешения, парсер, чекпоинты, хуки, под-агенты.

Экспорты ленивые (PEP 562): это разрывает круговые импорты между
`aiagent.tools`, `aiagent.prompts` и `aiagent.core`.
"""

from typing import Any

_LAZY: dict[str, str] = {
    "Agent": ".agent",
    "MAX_NUDGES": ".agent",
    "ContextManager": ".context",
    "ContextStats": ".context",
    "COMPACT_PROMPT": ".context",
    "HookResult": ".hooks",
    "HookRunner": ".hooks",
    "ParsedReply": ".parser",
    "parse_reply": ".parser",
    "extract_thinking": ".parser",
    "Decision": ".permissions",
    "PermissionManager": ".permissions",
    "ROLE_PROMPTS": ".subagent",
    "ROLE_TOOLS": ".subagent",
    "run_subagent": ".subagent",
    "AgentEvent": ".types",
    "Message": ".types",
    "ToolCall": ".types",
    "TurnResult": ".types",
    "Usage": ".types",
    "Checkpoint": ".undo",
    "CheckpointManager": ".undo",
    "FileChange": ".undo",
}

__all__ = list(_LAZY)


def __getattr__(name: str) -> Any:
    module_path = _LAZY.get(name)
    if module_path is None:
        raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
    import importlib

    module = importlib.import_module(module_path, __name__)
    value = getattr(module, name)
    globals()[name] = value          # кешируем, чтобы не искать повторно
    return value


def __dir__() -> list[str]:
    return sorted(__all__)
