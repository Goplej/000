"""Оффлайн-провайдер: демонстрация агента без ключей и без модели.

Полезен для тестов, показа интерфейса и проверки инструментов на «сухом» стенде.
Сценарии задаются переменной AIA_MOCK_SCRIPT (файл JSON со списком ответов):
    [{"text": "<tool_call>…</tool_call>"}, {"text": "готово"}]
"""
from __future__ import annotations

import json
import os
import time
from collections.abc import Iterator
from typing import Any

from ..core.types import ToolCall, Usage
from ..utils.tokens import estimate_tokens
from .base import BaseProvider, ChatRequest, ProviderCapabilities, StreamEvent


class MockProvider(BaseProvider):
    name = "mock"
    display_name = "Демо-режим (без модели)"
    default_base_url = "mock://local"
    capabilities = ProviderCapabilities(
        tools=True, streaming=True, system_prompt=True, local=True, parallel_tools=False,
    )

    def __init__(self, config) -> None:
        super().__init__(config)
        self._turn = 0
        self._script: list[dict[str, Any]] = []
        path = os.environ.get("AIA_MOCK_SCRIPT")
        if path and os.path.isfile(path):
            try:
                raw = json.loads(open(path, encoding="utf-8").read())
                self._script = [item if isinstance(item, dict) else {"text": str(item)} for item in raw]
            except (OSError, json.JSONDecodeError):
                self._script = []
        self.delay = float(os.environ.get("AIA_MOCK_DELAY", "0.02"))

    # ------------------------------------------------------------------------ запрос
    def stream(self, request: ChatRequest) -> Iterator[StreamEvent]:
        text, calls = self._next(request)
        for piece in _chunks(text, 24):
            yield StreamEvent.delta(piece)
            if self.delay:
                time.sleep(self.delay)
        for call in calls:
            yield StreamEvent.call(call)
        usage = Usage(
            input_tokens=sum(estimate_tokens(m.content) for m in request.messages),
            output_tokens=estimate_tokens(text),
        )
        yield StreamEvent("usage", usage=usage)
        yield StreamEvent("meta", meta={"tok_per_s": 120.0, "mock": True})
        yield StreamEvent.stop("tool_use" if calls else "end_turn")

    # ---------------------------------------------------------------------- генерация
    def _next(self, request: ChatRequest) -> tuple[str, list[ToolCall]]:
        if self._script:
            index = min(self._turn, len(self._script) - 1)
            self._turn += 1
            item = self._script[index]
            calls = []
            for raw in item.get("tool_calls") or []:
                function = raw.get("function", raw)
                calls.append(ToolCall(name=function.get("name", ""),
                                      arguments=function.get("arguments") or {}))
            text = str(item.get("text", ""))
            if not calls and "<tool_call" in text:
                # Сценарии удобно писать «как видит модель» — разберём их настоящим парсером.
                from ..core.parser import parse_reply

                parsed = parse_reply(text)
                return parsed.text, parsed.calls
            return text, calls
        return self._demo(request)

    def _demo(self, request: ChatRequest) -> tuple[str, list[ToolCall]]:
        """Разумное поведение по умолчанию: показать, как агент вызывает инструменты."""
        last_user = next((m for m in reversed(request.messages) if m.role == "user"), None)
        task = (last_user.content if last_user else "").strip()
        results = [
            m for m in request.messages
            if m.role == "tool" or (m.role == "user" and m.content.startswith("[результат"))
        ]

        if results:
            return ("Готово. Демо-режим завершил работу — вот что вернули инструменты:\n"
                    + results[-1].content[:600] + "\n\n"
                    "Для реальных ответов подключи модель: `aia keys set anthropic <ключ>` "
                    "или запусти Ollama и используй профиль `local`.", [])

        if not task:
            return ("Привет! Это демонстрационный режим AI Agent Studio. Модель не подключена, "
                    "но весь инструментарий работает. Напиши задачу — покажу карточки вызовов.", [])

        low = task.lower()
        if any(word in low for word in ("файл", "создай", "напиши", "file", "create", "скрипт")):
            path = "demo_agent.py"
            code = (
                '"""Файл создан агентом в демо-режиме."""\n\n'
                'from __future__ import annotations\n\n\n'
                'def greet(name: str = "мир") -> str:\n'
                '    return f"Привет, {name}!"\n\n\n'
                'if __name__ == "__main__":\n'
                '    print(greet())\n'
            )
            call = ToolCall(name="write_file",
                            arguments={"path": path, "content": code, "reason": "демонстрация"})
            payload = json.dumps({"name": call.name, "arguments": call.arguments}, ensure_ascii=False)
            return (f"Создаю файл {path}.\n<tool_call>{payload}</tool_call>", [call])

        if any(word in low for word in ("файлы", "структур", "список", "папк", "list")):
            call = ToolCall(name="list_dir", arguments={"path": ".", "depth": 2})
            payload = json.dumps({"name": call.name, "arguments": call.arguments}, ensure_ascii=False)
            return (f"Смотрю содержимое рабочей папки.\n<tool_call>{payload}</tool_call>", [call])

        if any(word in low for word in ("интернет", "поиск", "найди", "погода", "новости", "search")):
            call = ToolCall(name="web_search", arguments={"query": task[:120], "max_results": 5})
            payload = json.dumps({"name": call.name, "arguments": call.arguments}, ensure_ascii=False)
            return (f"Ищу в интернете: «{task[:80]}»\n<tool_call>{payload}</tool_call>", [call])

        if any(word in low for word in ("запусти", "тест", "run", "build", "сборка")):
            call = ToolCall(name="run_shell", arguments={"command": "echo демонстрация работы агента"})
            payload = json.dumps({"name": call.name, "arguments": call.arguments}, ensure_ascii=False)
            return (f"Выполняю команду.\n<tool_call>{payload}</tool_call>", [call])

        return (f"Демо-режим: ты написал «{task[:200]}».\n"
                "Настоящая модель не подключена, поэтому отвечаю по шаблону. Весь остальной "
                "функционал (инструменты, интерфейс, сессии) работает по-настоящему.", [])

    # -------------------------------------------------------------------- служебное
    def list_models(self) -> list[str]:
        return ["mock-1", "mock-coder", "mock-long-context"]

    def health(self) -> tuple[bool, str]:
        return True, "Демо-режим: модель не подключена, но инструменты работают"


def _chunks(text: str, size: int) -> Iterator[str]:
    for index in range(0, len(text), size):
        yield text[index:index + size]


__all__ = ["MockProvider"]
