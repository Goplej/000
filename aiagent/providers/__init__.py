"""Реестр провайдеров и фабрика: выбирает реализацию по конфигурации."""
from __future__ import annotations

from ..config import DEFAULT_MODELS, KEY_ENV, AgentConfig
from ..errors import ConfigError
from .anthropic_provider import AnthropicProvider
from .base import BaseProvider
from .google_provider import GoogleProvider
from .mock_provider import MockProvider
from .ollama_provider import OllamaProvider
from .openai_provider import (
    DeepSeekProvider,
    GroqProvider,
    LocalServerProvider,
    MistralProvider,
    OpenAIProvider,
    OpenRouterProvider,
)

#: Имя провайдера → класс
REGISTRY: dict[str, type[BaseProvider]] = {
    "anthropic": AnthropicProvider,
    "openai": OpenAIProvider,
    "openrouter": OpenRouterProvider,
    "groq": GroqProvider,
    "deepseek": DeepSeekProvider,
    "mistral": MistralProvider,
    "google": GoogleProvider,
    "ollama": OllamaProvider,
    "local": LocalServerProvider,
    "mock": MockProvider,
}

#: Человеко-понятные названия для интерфейса и документации
DISPLAY = {
    "anthropic": "Anthropic Claude",
    "openai": "OpenAI",
    "openrouter": "OpenRouter",
    "groq": "Groq",
    "deepseek": "DeepSeek",
    "mistral": "Mistral",
    "google": "Google Gemini",
    "ollama": "Ollama (локально)",
    "local": "Локальный OpenAI-сервер",
    "mock": "Демо-режим",
}


def create_provider(config: AgentConfig, name: str = "") -> BaseProvider:
    """Создаёт провайдера по имени или по автоопределению из конфига."""
    provider_name = (name or config.resolved_provider()).lower()
    if provider_name not in REGISTRY:
        raise ConfigError(
            f"Неизвестный провайдер: {provider_name}",
            hint="Доступны: " + ", ".join(sorted(REGISTRY)),
        )
    provider = REGISTRY[provider_name](config)
    if not config.model:
        config.model = DEFAULT_MODELS.get(provider_name, config.model)
    return provider


def available_providers() -> dict[str, dict[str, str]]:
    """Список провайдеров с подсказками, какой ключ нужен."""
    import os

    out: dict[str, dict[str, str]] = {}
    for name, cls in REGISTRY.items():
        envs = KEY_ENV.get(name, ())
        key_present = any(os.environ.get(env) for env in envs) if envs else True
        out[name] = {
            "title": DISPLAY.get(name, name),
            "needs_key": "нет" if not envs else ", ".join(envs),
            "key_present": "да" if key_present else "нет",
            "local": "да" if cls.capabilities.local else "нет",
            "default_model": DEFAULT_MODELS.get(name, ""),
        }
    return out


def detect_best_provider(config: AgentConfig) -> tuple[str, str]:
    """
    Подбирает провайдера и модель, если пользователь ничего не указал.
    Возвращает (провайдер, модель) и не делает сетевых запросов.
    """
    if config.provider:
        return config.provider, config.resolved_model()
    provider_name = config.resolved_provider()
    return provider_name, DEFAULT_MODELS.get(provider_name, "")


__all__ = [
    "DISPLAY", "REGISTRY", "available_providers", "create_provider", "detect_best_provider",
]
