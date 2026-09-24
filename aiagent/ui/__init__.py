"""Интерфейсы вывода: консоль, поток событий для веб-интерфейса, тихий режим."""
from __future__ import annotations

from typing import Any

from .console import Console


class SilentUI:
    """Ничего не печатает: нужен для тестов, серверных вызовов и под-агентов."""

    verbose = False
    auto_approve = True

    def text(self, chunk: str) -> None:
        pass

    def info(self, message: str) -> None:
        pass

    def warn(self, message: str) -> None:
        pass

    def error(self, message: str) -> None:
        pass

    def tool_start(self, name: str, arguments: dict[str, Any]) -> None:
        pass

    def tool_end(self, name: str, ok: bool, display: str, content: str = "",
                 seconds: float = 0.0) -> None:
        pass

    def confirm(self, title: str, description: str) -> bool:
        return True

    def ask(self, prompt: str) -> str:
        return ""

    def end_stream(self) -> None:
        pass


class EventUI:
    """
    Интерфейс для программного использования: превращает события в колбэки.
    Например, веб-сервер подписывается и рассылает их в браузер через SSE.
    """

    verbose = False

    def __init__(self, on_event=None, on_confirm=None, auto_approve: bool = False) -> None:
        self.on_event = on_event
        self.on_confirm = on_confirm
        self.auto_approve = auto_approve

    def _emit(self, kind: str, **payload: Any) -> None:
        if self.on_event:
            self.on_event({"kind": kind, **payload})

    def text(self, chunk: str) -> None:
        self._emit("text", text=chunk)

    def info(self, message: str) -> None:
        self._emit("info", text=message)

    def warn(self, message: str) -> None:
        self._emit("warn", text=message)

    def error(self, message: str) -> None:
        self._emit("error", text=message)

    def tool_start(self, name: str, arguments: dict[str, Any]) -> None:
        self._emit("tool_start", name=name, arguments=arguments)

    def tool_end(self, name: str, ok: bool, display: str, content: str = "",
                 seconds: float = 0.0) -> None:
        self._emit("tool_end", name=name, ok=ok, display=display, content=content,
                   seconds=seconds)

    def confirm(self, title: str, description: str) -> bool:
        if self.auto_approve:
            return True
        if self.on_confirm is None:
            return False
        return bool(self.on_confirm(title, description))

    def ask(self, prompt: str) -> str:
        self._emit("ask", text=prompt)
        return ""

    def end_stream(self) -> None:
        self._emit("end_stream")


__all__ = ["Console", "EventUI", "SilentUI"]
