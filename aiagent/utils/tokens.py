"""Оценка токенов и контроль заполнения контекстного окна.

Модели токенизируют по-разному, но для управления контекстом достаточно
хорошей эвристики: русский текст ≈ 2.5 символа/токен, код и английский ≈ 4.
Считаем консервативно (завышаем), чтобы никогда не упасть в переполнение.
"""
from __future__ import annotations

import re
from dataclasses import dataclass

from ..core.types import Message

_CYRILLIC = re.compile(r"[а-яёА-ЯЁ]")
_CJK = re.compile(r"[\u4e00-\u9fff\u3040-\u30ff\uac00-\ud7af]")
_CODE = re.compile(r"[{}()\[\];=<>/\\|&*#@$`~^]")
_DIGITS = re.compile(r"\d")


def estimate_tokens(text: str) -> int:
    """Оценка числа токенов в строке (консервативная, с запасом ~10%)."""
    if not text:
        return 0
    length = len(text)
    cyrillic = len(_CYRILLIC.findall(text))
    cjk = len(_CJK.findall(text))
    code = len(_CODE.findall(text))
    digits = len(_DIGITS.findall(text))

    if cjk:
        # Иероглифы ≈ 1 токен на знак
        return int(cjk * 1.05 + (length - cjk) / 3.8)
    # База: 3.8 символа на токен, кириллица и код — плотнее
    tokens = length / 3.8
    tokens += cyrillic * (1 / 2.2 - 1 / 3.8)
    tokens += code * (1 / 3.0 - 1 / 3.8)
    tokens += digits * (1 / 2.8 - 1 / 3.8)
    return max(1, int(tokens * 1.1))


@dataclass
class TokenCounter:
    """Считает токены сообщений и следит за заполнением окна контекста."""

    context_window: int = 200_000
    system_tokens: int = 0

    def count_message(self, message: Message) -> int:
        # ~4 токена накладных расходов на сообщение (роль, служебные поля)
        total = estimate_tokens(message.content) + 4
        for call in message.tool_calls:
            total += estimate_tokens(call.name) + estimate_tokens(str(call.arguments)) + 6
        return total

    def count(self, messages: list[Message]) -> int:
        return self.system_tokens + sum(self.count_message(m) for m in messages)

    def fill_ratio(self, messages: list[Message]) -> float:
        if self.context_window <= 0:
            return 0.0
        return self.count(messages) / self.context_window

    def free_tokens(self, messages: list[Message]) -> int:
        return max(0, self.context_window - self.count(messages))

    def needs_compaction(self, messages: list[Message], ratio: float) -> bool:
        return self.fill_ratio(messages) >= ratio

    def describe(self, messages: list[Message]) -> str:
        used = self.count(messages)
        pct = self.fill_ratio(messages) * 100
        bar_len = 24
        filled = min(bar_len, int(bar_len * min(pct, 100) / 100))
        bar = "█" * filled + "░" * (bar_len - filled)
        return f"[{bar}] {pct:4.0f}%  ({used:,} / {self.context_window:,} токенов)".replace(",", " ")


#: Сколько токенов «стоит» одно изображение/большой файл по умолчанию
CHARS_PER_TOKEN = 3.2


def tokens_to_chars(tokens: int) -> int:
    return int(tokens * CHARS_PER_TOKEN)


__all__ = ["CHARS_PER_TOKEN", "TokenCounter", "estimate_tokens", "tokens_to_chars"]
