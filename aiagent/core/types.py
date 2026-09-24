"""Модель данных агента: сообщения, вызовы инструментов, события, использование токенов."""
from __future__ import annotations

import json
import time
import uuid
from collections.abc import Iterator
from dataclasses import dataclass, field
from typing import Any, Literal

Role = Literal["system", "user", "assistant", "tool"]

# --------------------------------------------------------------------------------------
# Сообщения
# --------------------------------------------------------------------------------------
@dataclass
class ToolCall:
    """Запрос модели на выполнение инструмента."""

    name: str
    arguments: dict[str, Any] = field(default_factory=dict)
    id: str = field(default_factory=lambda: f"call_{uuid.uuid4().hex[:12]}")
    raw: str = ""

    def to_dict(self) -> dict[str, Any]:
        return {"id": self.id, "name": self.name, "arguments": self.arguments}

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> ToolCall:
        return cls(
            name=data.get("name", ""),
            arguments=data.get("arguments") or {},
            id=data.get("id") or f"call_{uuid.uuid4().hex[:12]}",
            raw=data.get("raw", ""),
        )

    def preview(self, limit: int = 160) -> str:
        parts = []
        for key, value in self.arguments.items():
            if isinstance(value, str):
                shown = value if len(value) <= limit else value[:limit] + "…"
                parts.append(f"{key}={shown!r}")
            else:
                parts.append(f"{key}={value}")
        line = ", ".join(parts)
        return line if len(line) <= limit * 2 else line[: limit * 2] + "…"


@dataclass
class Message:
    role: Role
    content: str = ""
    tool_calls: list[ToolCall] = field(default_factory=list)
    tool_call_id: str = ""              # для role="tool"
    tool_name: str = ""
    is_error: bool = False
    timestamp: float = field(default_factory=time.time)
    usage: Usage | None = None
    meta: dict[str, Any] = field(default_factory=dict)

    # ---------------------------------------------------------------- удобные фабрики
    @classmethod
    def user(cls, text: str, **meta: Any) -> Message:
        return cls("user", text, meta=meta)

    @classmethod
    def assistant(cls, text: str, tool_calls: list[ToolCall] | None = None, **meta: Any) -> Message:
        return cls("assistant", text, tool_calls=list(tool_calls or []), meta=meta)

    @classmethod
    def tool_result(cls, call: ToolCall, content: str, is_error: bool = False) -> Message:
        return cls("tool", content, tool_call_id=call.id, tool_name=call.name, is_error=is_error)

    # ---------------------------------------------------------------------- сериализация
    def to_dict(self) -> dict[str, Any]:
        data: dict[str, Any] = {"role": self.role, "content": self.content}
        if self.tool_calls:
            data["tool_calls"] = [c.to_dict() for c in self.tool_calls]
        if self.tool_call_id:
            data["tool_call_id"] = self.tool_call_id
        if self.tool_name:
            data["tool_name"] = self.tool_name
        if self.is_error:
            data["is_error"] = True
        if self.meta:
            data["meta"] = self.meta
        if self.usage:
            data["usage"] = self.usage.to_dict()
        return data

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> Message:
        return cls(
            role=data.get("role", "user"),
            content=data.get("content", "") or "",
            tool_calls=[ToolCall.from_dict(c) for c in data.get("tool_calls", [])],
            tool_call_id=data.get("tool_call_id", ""),
            tool_name=data.get("tool_name", ""),
            is_error=bool(data.get("is_error")),
            usage=Usage.from_dict(data["usage"]) if data.get("usage") else None,
            meta=data.get("meta") or {},
        )

    @property
    def chars(self) -> int:
        extra = sum(len(json.dumps(c.arguments, ensure_ascii=False)) for c in self.tool_calls)
        return len(self.content) + extra


# --------------------------------------------------------------------------------------
# Токены и стоимость
# --------------------------------------------------------------------------------------
@dataclass
class Usage:
    input_tokens: int = 0
    output_tokens: int = 0
    cache_read_tokens: int = 0
    cache_write_tokens: int = 0
    cost_usd: float = 0.0

    def add(self, other: Usage | None) -> Usage:
        if other is None:
            return self
        self.input_tokens += other.input_tokens
        self.output_tokens += other.output_tokens
        self.cache_read_tokens += other.cache_read_tokens
        self.cache_write_tokens += other.cache_write_tokens
        self.cost_usd += other.cost_usd
        return self

    @property
    def total(self) -> int:
        return self.input_tokens + self.output_tokens

    def to_dict(self) -> dict[str, Any]:
        return {
            "input_tokens": self.input_tokens,
            "output_tokens": self.output_tokens,
            "cache_read_tokens": self.cache_read_tokens,
            "cache_write_tokens": self.cache_write_tokens,
            "cost_usd": round(self.cost_usd, 6),
        }

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> Usage:
        return cls(
            input_tokens=int(data.get("input_tokens", 0)),
            output_tokens=int(data.get("output_tokens", 0)),
            cache_read_tokens=int(data.get("cache_read_tokens", 0)),
            cache_write_tokens=int(data.get("cache_write_tokens", 0)),
            cost_usd=float(data.get("cost_usd", 0.0)),
        )

    def summary(self) -> str:
        parts = [f"вход {self.input_tokens:,} → выход {self.output_tokens:,} токенов".replace(",", " ")]
        if self.cache_read_tokens:
            parts.append(f"из кэша {self.cache_read_tokens:,}".replace(",", " "))
        if self.cost_usd:
            parts.append(f"≈${self.cost_usd:.4f}")
        return ", ".join(parts)


# --------------------------------------------------------------------------------------
# События агентного цикла (то, что видит интерфейс)
# --------------------------------------------------------------------------------------
@dataclass
class AgentEvent:
    type: str
    # start | text | thinking | tool_start | tool_end | step | usage | info | warn |
    # error | compact | done | permission
    text: str = ""
    name: str = ""
    call: ToolCall | None = None
    ok: bool = True
    display: str = ""
    content: str = ""
    seconds: float = 0.0
    meta: dict[str, Any] = field(default_factory=dict)

    def to_dict(self) -> dict[str, Any]:
        data: dict[str, Any] = {"type": self.type}
        if self.text:
            data["text"] = self.text
        if self.name:
            data["name"] = self.name
        if self.call is not None:
            data["call"] = self.call.to_dict()
        if self.type == "tool_end":
            data["ok"] = self.ok
            data["display"] = self.display
            data["content"] = self.content[:20000]
            data["seconds"] = round(self.seconds, 2)
        if self.meta:
            data["meta"] = self.meta
        return data


@dataclass
class TurnResult:
    """Итог одного запроса пользователя."""

    answer: str = ""
    steps: int = 0
    tool_calls: int = 0
    usage: Usage = field(default_factory=Usage)
    stop_reason: str = "end_turn"
    duration: float = 0.0
    files_changed: list[str] = field(default_factory=list)

    def to_dict(self) -> dict[str, Any]:
        return {
            "answer": self.answer,
            "steps": self.steps,
            "tool_calls": self.tool_calls,
            "usage": self.usage.to_dict(),
            "stop_reason": self.stop_reason,
            "duration": round(self.duration, 2),
            "files_changed": self.files_changed,
        }


def iter_chunks(text: str, size: int = 48) -> Iterator[str]:
    for index in range(0, len(text), size):
        yield text[index:index + size]


__all__ = ["AgentEvent", "Message", "Role", "ToolCall", "TurnResult", "Usage", "iter_chunks"]
