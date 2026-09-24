"""Управление контекстом: сборка запроса, сжатие истории, статистика окна."""
from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass, field
from typing import Any

from ..config import AgentConfig
from ..utils.text import clip, preview
from ..utils.tokens import TokenCounter
from .types import Message, ToolCall, Usage

COMPACT_PROMPT = (
    "Сожми историю работы над задачей в подробную сводку для продолжения работы.\n"
    "Обязательно сохрани: цель пользователя, что уже сделано, какие файлы созданы/изменены "
    "(с путями), какие команды запускались и что вернули, какие ошибки возникли и как исправлены, "
    "что осталось сделать. Пиши по-русски, компактно, без воды."
)


@dataclass
class ContextStats:
    messages: int = 0
    chars: int = 0
    tokens: int = 0
    fill: float = 0.0
    compactions: int = 0
    files_touched: list[str] = field(default_factory=list)

    def to_dict(self) -> dict[str, Any]:
        return {
            "messages": self.messages, "chars": self.chars, "tokens": self.tokens,
            "fill": round(self.fill, 3), "compactions": self.compactions,
            "files_touched": self.files_touched[-20:],
        }


class ContextManager:
    """Держит историю сообщений, следит за окном и умеет сжимать её."""

    def __init__(self, config: AgentConfig, system_prompt: str = "") -> None:
        self.config = config
        self.system_prompt = system_prompt
        self.messages: list[Message] = []
        self.counter = TokenCounter(context_window=config.context_window)
        self.compactions = 0
        self.files_touched: list[str] = []

    # --------------------------------------------------------------------- мутации
    def add_user(self, text: str, **meta: Any) -> Message:
        message = Message.user(text, **meta)
        self.messages.append(message)
        return message

    def add_assistant(self, text: str, calls: list[ToolCall] | None = None, **meta: Any) -> Message:
        message = Message.assistant(text, calls, **meta)
        self.messages.append(message)
        return message

    def add_tool_result(self, call: ToolCall, content: str, is_error: bool = False) -> Message:
        message = Message.tool_result(call, content, is_error)
        self.messages.append(message)
        if not is_error:
            self._track_file(call)
        return message

    def add_system_note(self, text: str) -> Message:
        """Служебная запись, которая уходит модели как user-сообщение."""
        message = Message.user(f"[системная заметка] {text}")
        self.messages.append(message)
        return message

    def reset(self) -> None:
        self.messages.clear()
        self.compactions = 0
        self.files_touched.clear()

    def _track_file(self, call: ToolCall) -> None:
        if call.name not in {"write_file", "edit_file", "multi_edit", "delete_file"}:
            return
        path = call.arguments.get("path") or call.arguments.get("file_path")
        if path and str(path) not in self.files_touched:
            self.files_touched.append(str(path))

    # ------------------------------------------------------------------ статистика
    def stats(self) -> ContextStats:
        chars = sum(len(m.content) for m in self.messages)
        tokens = self.counter.count(self.messages)
        return ContextStats(
            messages=len(self.messages),
            chars=chars,
            tokens=tokens,
            fill=self.counter.fill_ratio(self.messages),
            compactions=self.compactions,
            files_touched=list(self.files_touched),
        )

    def fill_ratio(self, provider_tokens: int | None = None) -> float:
        if provider_tokens is not None and self.config.context_window:
            return provider_tokens / self.config.context_window
        return self.counter.fill_ratio(self.messages)

    def should_compact(self, provider_tokens: int | None = None) -> bool:
        if not self.config.auto_compact:
            return False
        return self.fill_ratio(provider_tokens) >= self.config.compact_at_ratio

    # --------------------------------------------------------------------- сжатие
    def compact(self, usage: Usage | None = None, force: bool = False) -> str:
        """
        Сжимает историю: оставляет системный промпт, свежие сообщения и краткую сводку.
        Возвращает текст сводки (для показа пользователю).
        """
        keep = max(2, self.config.keep_recent_messages)
        if len(self.messages) <= keep + 1 and not force:
            return ""

        recent = self.messages[-keep:]
        older = self.messages[:-keep]
        if not older:
            return ""

        summary = self._summarize_locally(older)
        note = Message.user(
            "[сжатие контекста] Краткая сводка предыдущей работы:\n" + summary
        )
        self.messages = [note, *recent]
        self.compactions += 1
        return summary

    def _summarize_locally(self, messages: list[Message]) -> str:
        """Эвристическая сводка без обращения к модели: быстро и бесплатно."""
        files: list[str] = []
        commands: list[str] = []
        tasks: list[str] = []
        errors: list[str] = []

        for message in messages:
            if message.role == "user" and not message.meta.get("tool_result"):
                first_line = message.content.strip().splitlines()[0] if message.content.strip() else ""
                if first_line and not first_line.startswith("[результат"):
                    tasks.append(clip(first_line, 140))
            if message.role == "assistant":
                for call in message.tool_calls:
                    if call.name in {"write_file", "edit_file", "multi_edit", "delete_file"}:
                        path = call.arguments.get("path") or call.arguments.get("file_path")
                        if path:
                            files.append(str(path))
                    elif call.name in {"run_shell", "run_code"}:
                        command = call.arguments.get("command") or "фрагмент кода"
                        commands.append(clip(str(command), 120))
            if message.role == "tool" and message.is_error:
                errors.append(clip(message.content, 160))
            elif message.role == "user" and message.content.startswith("[результат"):
                if "ошибка" in message.content[:60].lower():
                    errors.append(clip(message.content, 160))

        parts = []
        if tasks:
            parts.append("Задачи пользователя: " + "; ".join(dict.fromkeys(tasks[-6:])))
        if files:
            parts.append("Файлы, с которыми работали: " + ", ".join(dict.fromkeys(files[-15:])))
        if commands:
            parts.append("Запускались команды: " + "; ".join(dict.fromkeys(commands[-8:])))
        if errors:
            parts.append("Были ошибки: " + " | ".join(dict.fromkeys(errors[-4:])))
        parts.append(f"Всего сообщений сжато: {len(messages)}")
        return "\n".join(f"- {part}" for part in parts)

    def summarize_with_model(self, complete: Callable[[list[Message], str], str]) -> str:
        """
        Сводка силами модели (точнее, чем эвристика). complete(messages, prompt) -> текст.
        При любой ошибке возвращает эвристическую сводку.
        """
        keep = max(2, self.config.keep_recent_messages)
        older = self.messages[:-keep]
        if not older:
            return ""
        try:
            text = complete(older, COMPACT_PROMPT)
        except Exception:  # noqa: BLE001 — сводка не должна ломать работу
            text = ""
        if not text.strip():
            return self.compact(force=True)
        note = Message.user("[сжатие контекста] Сводка предыдущей работы:\n" + text.strip())
        self.messages = [note, *self.messages[-keep:]]
        self.compactions += 1
        return text.strip()

    # ----------------------------------------------------------------- сборка запроса
    def request_tools(self) -> list[dict[str, Any]]:
        return []

    def describe(self) -> str:
        stats = self.stats()
        return (f"сообщений {stats.messages}, ≈{stats.tokens} токенов "
                f"({stats.fill * 100:.0f}% окна), сжатий: {stats.compactions}")

    def dump(self) -> list[dict[str, Any]]:
        return [m.to_dict() for m in self.messages]

    def load(self, data: list[dict[str, Any]]) -> None:
        self.messages = [Message.from_dict(item) for item in data]

    def last_assistant_text(self) -> str:
        for message in reversed(self.messages):
            if message.role == "assistant" and message.content.strip():
                return message.content.strip()
        return ""

    def tool_call_count(self) -> int:
        return sum(len(m.tool_calls) for m in self.messages if m.role == "assistant")

    def preview_start(self) -> str:
        for message in self.messages:
            if message.role == "user" and not message.content.startswith("["):
                return preview(message.content, 80)
        return "(новый диалог)"


__all__ = ["COMPACT_PROMPT", "ContextManager", "ContextStats"]
