"""Исключения платформы — единая иерархия с понятными русскими сообщениями."""
from __future__ import annotations


class AgentError(Exception):
    """Базовая ошибка агента."""

    def __init__(self, message: str, *, hint: str = "", details: dict | None = None) -> None:
        super().__init__(message)
        self.message = message
        self.hint = hint
        self.details = details or {}

    def human(self) -> str:
        return f"{self.message}\n  Подсказка: {self.hint}" if self.hint else self.message


class ConfigError(AgentError):
    """Неверная конфигурация."""


class ProviderError(AgentError):
    """Ошибка обращения к модели (сеть, ключ, лимит)."""

    def __init__(self, message: str, *, status: int | None = None, provider: str = "",
                 retryable: bool = False, **kwargs) -> None:
        super().__init__(message, **kwargs)
        self.status = status
        self.provider = provider
        self.retryable = retryable


class AuthError(ProviderError):
    """Нет ключа или ключ не принят."""


class RateLimitError(ProviderError):
    """Превышен лимит запросов — стоит подождать."""

    def __init__(self, message: str, *, retry_after: float = 5.0, **kwargs) -> None:
        super().__init__(message, retryable=True, **kwargs)
        self.retry_after = retry_after


class ToolError(AgentError):
    """Инструмент завершился ошибкой."""


class PermissionDenied(ToolError):
    """Пользователь (или правила) запретил действие."""


class ContextOverflow(AgentError):
    """Контекст не влезает даже после сжатия."""


class SessionError(AgentError):
    """Проблема с сохранением/загрузкой сессии."""


class MCPError(AgentError):
    """Ошибка MCP-сервера."""


__all__ = [
    "AgentError", "AuthError", "ConfigError", "ContextOverflow", "MCPError",
    "PermissionDenied", "ProviderError", "RateLimitError", "SessionError", "ToolError",
]
