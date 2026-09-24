"""Цены моделей за 1M токенов — чтобы агент показывал реальную стоимость работы.

Значения ориентировочные; при изменении тарифов правь только этот файл.
Формат: модель-префикс → (вход, выход, кэш-чтение, кэш-запись) в USD за 1M токенов.
"""
from __future__ import annotations

from ..core.types import Usage

PRICES: dict[str, tuple[float, float, float, float]] = {
    # --- Anthropic ---
    "claude-opus-4": (15.0, 75.0, 1.5, 18.75),
    "claude-sonnet-4": (3.0, 15.0, 0.3, 3.75),
    "claude-3-7-sonnet": (3.0, 15.0, 0.3, 3.75),
    "claude-3-5-sonnet": (3.0, 15.0, 0.3, 3.75),
    "claude-3-5-haiku": (0.8, 4.0, 0.08, 1.0),
    "claude-3-haiku": (0.25, 1.25, 0.03, 0.3),
    # --- OpenAI ---
    "gpt-4o-mini": (0.15, 0.6, 0.075, 0.0),
    "gpt-4o": (2.5, 10.0, 1.25, 0.0),
    "gpt-4.1-mini": (0.4, 1.6, 0.1, 0.0),
    "gpt-4.1": (2.0, 8.0, 0.5, 0.0),
    "o3-mini": (1.1, 4.4, 0.55, 0.0),
    "o1": (15.0, 60.0, 7.5, 0.0),
    # --- Google ---
    "gemini-2.5-pro": (1.25, 10.0, 0.31, 0.0),
    "gemini-2.5-flash": (0.3, 2.5, 0.075, 0.0),
    "gemini-2.0-flash": (0.1, 0.4, 0.025, 0.0),
    # --- Прочие облака ---
    "deepseek-chat": (0.27, 1.1, 0.07, 0.0),
    "deepseek-reasoner": (0.55, 2.19, 0.14, 0.0),
    "llama-3.3-70b": (0.59, 0.79, 0.0, 0.0),
    "mixtral": (0.24, 0.24, 0.0, 0.0),
    "mistral-large": (2.0, 6.0, 0.0, 0.0),
    # --- Локальные: бесплатно ---
    "qwen2.5-coder": (0.0, 0.0, 0.0, 0.0),
    "llama3": (0.0, 0.0, 0.0, 0.0),
    "mock": (0.0, 0.0, 0.0, 0.0),
}


def price_for(model: str) -> tuple[float, float, float, float]:
    """Возвращает тариф модели (сначала более длинные совпадения)."""
    name = (model or "").lower()
    best: tuple[float, float, float, float] | None = None
    best_len = 0
    for prefix, price in PRICES.items():
        if prefix in name and len(prefix) > best_len:
            best, best_len = price, len(prefix)
    return best or (0.0, 0.0, 0.0, 0.0)


def compute_cost(model: str, usage: Usage) -> float:
    """Считает стоимость использования в долларах."""
    inp, out, cache_read, cache_write = price_for(model)
    if not any((inp, out, cache_read, cache_write)):
        return 0.0
    cost = (
        usage.input_tokens * inp
        + usage.output_tokens * out
        + usage.cache_read_tokens * cache_read
        + usage.cache_write_tokens * cache_write
    ) / 1_000_000
    usage.cost_usd = round(cost, 6)
    return usage.cost_usd


def is_free(model: str) -> bool:
    return not any(price_for(model))


__all__ = ["PRICES", "compute_cost", "is_free", "price_for"]
