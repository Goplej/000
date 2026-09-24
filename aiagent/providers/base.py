"""Базовый интерфейс провайдеров моделей.

Все провайдеры приводят свой протокол к единому потоку StreamEvent,
поэтому агентный цикл ничего не знает об Anthropic/OpenAI/Gemini/Ollama.
"""
from __future__ import annotations

import time
from abc import ABC, abstractmethod
from collections.abc import Iterator
from dataclasses import dataclass, field
from typing import Any

from ..config import AgentConfig
from ..core.types import Message, ToolCall, Usage
from ..utils.http import HttpClient
from ..utils.tokens import estimate_tokens


@dataclass
class ProviderCapabilities:
    tools: bool = True            # умеет вызывать инструменты
    streaming: bool = True
    vision: bool = False          # понимает изображения
    thinking: bool = False        # расширенное «мышление»
    system_prompt: bool = True
    parallel_tools: bool = True   # можно вернуть несколько вызовов в одном ответе
    cache: bool = False           # кэш промпта
    local: bool = False           # работает без интернета


@dataclass
class ChatRequest:
    messages: list[Message]
    system: str = ""
    tools: list[dict[str, Any]] = field(default_factory=list)
    model: str = ""
    max_tokens: int = 4096
    temperature: float = 0.0
    stream: bool = True
    thinking_budget: int = 0
    reasoning_effort: str = ""
    stop: list[str] = field(default_factory=list)


@dataclass
class StreamEvent:
    """Событие потока от модели."""

    kind: str                 # text | thinking | tool_call | usage | stop | error | meta
    text: str = ""
    tool_call: ToolCall | None = None
    usage: Usage | None = None
    stop_reason: str = ""
    error: str = ""
    meta: dict[str, Any] = field(default_factory=dict)

    @staticmethod
    def delta(text: str) -> StreamEvent:
        return StreamEvent("text", text=text)

    @staticmethod
    def thinking(text: str) -> StreamEvent:
        return StreamEvent("thinking", text=text)

    @staticmethod
    def call(call: ToolCall) -> StreamEvent:
        return StreamEvent("tool_call", tool_call=call)

    @staticmethod
    def stop(reason: str = "end_turn") -> StreamEvent:
        return StreamEvent("stop", stop_reason=reason)


class BaseProvider(ABC):
    """Общий предок всех провайдеров."""

    name = "base"
    display_name = "Base"
    default_base_url = ""
    capabilities = ProviderCapabilities()

    def __init__(self, config: AgentConfig) -> None:
        self.config = config
        connect, read = config.timeouts()
        self.http = HttpClient(timeout=read, connect_timeout=connect,
                               max_retries=config.max_retries)
        self.api_key = config.api_key(self.name)
        self.base_url = (config.base_url or self.default_base_url).rstrip("/")

    # ------------------------------------------------------------------ обязательное
    @abstractmethod
    def stream(self, request: ChatRequest) -> Iterator[StreamEvent]:
        """Отдаёт события по мере генерации."""

    # ------------------------------------------------------------------- необязательное
    def list_models(self) -> list[str]:
        return []

    def health(self) -> tuple[bool, str]:
        return True, ""

    def supports_tools(self, model: str = "") -> bool:
        """Умеет ли модель вызывать инструменты (у Ollama зависит от модели)."""
        return bool(self.capabilities.tools)

    def count_tokens(self, messages: list[Message], system: str = "") -> int:
        """Оценка размера запроса: точная только у Anthropic, у остальных — эвристика."""
        return estimate_tokens(system) + sum(estimate_tokens(m.content) for m in messages)

    # ------------------------------------------------------------------ удобные обёртки
    def complete(self, request: ChatRequest) -> tuple[Message, Usage]:
        """Собирает поток в одно сообщение (для простых вызовов и под-агентов)."""
        text_parts: list[str] = []
        calls: list[ToolCall] = []
        usage = Usage()
        stop_reason = "end_turn"
        for event in self.stream(request):
            if event.kind == "text":
                text_parts.append(event.text)
            elif event.kind == "tool_call" and event.tool_call is not None:
                calls.append(event.tool_call)
            elif event.kind == "usage" and event.usage is not None:
                usage = event.usage
            elif event.kind == "stop":
                stop_reason = event.stop_reason or stop_reason
            elif event.kind == "error":
                raise RuntimeError(event.error)
        message = Message.assistant("".join(text_parts), calls)
        message.usage = usage
        return message, usage

    # ------------------------------------------------------------------------ утилиты
    @property
    def is_local(self) -> bool:
        return self.capabilities.local

    def describe(self, model: str = "") -> str:
        return f"{model or self.config.resolved_model()} ({self.display_name})"

    def _headers(self, extra: dict[str, str] | None = None) -> dict[str, str]:
        headers = {"Accept": "application/json"}
        headers.update(extra or {})
        return headers

    def _check_key(self) -> None:
        from ..errors import AuthError
        if not self.api_key and not self.capabilities.local:
            from ..config import KEY_ENV
            envs = ", ".join(KEY_ENV.get(self.name, ()))
            raise AuthError(
                f"Не задан ключ API для провайдера {self.display_name}.",
                provider=self.name,
                hint=f"Экспортируй {envs} или выполни: aia keys set {self.name} <ключ>",
            )


class TimedStream:
    """Обёртка, замеряющая время до первого токена и общую длительность."""

    def __init__(self) -> None:
        self.start = time.time()
        self.first_token: float | None = None
        self.end: float | None = None

    def token(self) -> None:
        if self.first_token is None:
            self.first_token = time.time()

    def finish(self) -> None:
        self.end = time.time()

    @property
    def ttft(self) -> float:
        return (self.first_token or self.end or time.time()) - self.start

    @property
    def duration(self) -> float:
        return (self.end or time.time()) - self.start

    def tokens_per_second(self, tokens: int) -> float:
        elapsed = max(1e-6, (self.end or time.time()) - (self.first_token or self.start))
        return tokens / elapsed


__all__ = ["BaseProvider", "ChatRequest", "ProviderCapabilities", "StreamEvent", "TimedStream"]
