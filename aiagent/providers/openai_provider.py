"""Провайдеры, совместимые с OpenAI Chat Completions API.

Одна реализация закрывает: OpenAI, OpenRouter, Groq, DeepSeek, Mistral,
а также любые локальные серверы (LM Studio, llama.cpp, vLLM, LocalAI, Together).
"""
from __future__ import annotations

import json
from collections.abc import Iterator
from typing import Any

from ..core.types import Message, ToolCall, Usage
from ..errors import ProviderError
from ..utils.logging import get_logger
from .base import BaseProvider, ChatRequest, ProviderCapabilities, StreamEvent
from .pricing import compute_cost

log = get_logger("openai-compat")


class OpenAICompatibleProvider(BaseProvider):
    """Универсальный клиент /chat/completions со стримингом и tool calls."""

    name = "openai"
    display_name = "OpenAI-совместимый API"
    default_base_url = "https://api.openai.com/v1"
    capabilities = ProviderCapabilities(
        tools=True, streaming=True, vision=True, system_prompt=True,
        parallel_tools=True, cache=True,
    )

    def __init__(self, config) -> None:
        super().__init__(config)
        # Разные сервисы по-разному называют поле с «размышлениями»
        self.reasoning_fields = ("reasoning_content", "reasoning", "thinking")

    # ------------------------------------------------------------------------ запрос
    def stream(self, request: ChatRequest) -> Iterator[StreamEvent]:
        if self.name != "ollama" and not self.capabilities.local:
            self._check_key()

        body = self._build_body(request)
        headers = self._headers({
            **({"Authorization": f"Bearer {self.api_key}"} if self.api_key else {}),
            "Accept": "text/event-stream",
        })
        if self.name == "openrouter":
            headers.setdefault("HTTP-Referer", "https://localhost/aiagent")
            headers.setdefault("X-Title", "AI Agent Studio")

        tool_slots: dict[int, dict[str, Any]] = {}
        usage = Usage()
        stop_reason = "end_turn"
        raw_usage: dict[str, Any] = {}

        for _, chunk in self.http.stream_sse(
            "POST", f"{self.base_url}/chat/completions", json_body=body, headers=headers
        ):
            if "error" in chunk and not chunk.get("choices"):
                error = chunk["error"]
                message = error.get("message") if isinstance(error, dict) else str(error)
                raise ProviderError(f"{self.display_name}: {message}", provider=self.name)

            if chunk.get("usage"):
                raw_usage = chunk["usage"]
            choices = chunk.get("choices") or []
            if not choices:
                continue
            choice = choices[0]
            delta = choice.get("delta") or {}
            content = delta.get("content")
            if isinstance(content, list):  # некоторые сервисы шлют массив блоков
                content = "".join(part.get("text", "") for part in content if isinstance(part, dict))
            if content:
                yield StreamEvent.delta(content)

            for field in self.reasoning_fields:
                thought = delta.get(field)
                if thought:
                    yield StreamEvent.thinking(str(thought))
                    break

            for call in delta.get("tool_calls") or []:
                index = call.get("index", 0)
                slot = tool_slots.setdefault(index, {"id": "", "name": "", "arguments": ""})
                if call.get("id"):
                    slot["id"] = call["id"]
                function = call.get("function") or {}
                if function.get("name"):
                    slot["name"] += function["name"]
                if function.get("arguments"):
                    slot["arguments"] += function["arguments"]
                if call.get("type") == "function" and not function:
                    log.debug("пустой tool_call от %s", self.name)

            finish = choice.get("finish_reason")
            if finish:
                stop_reason = finish

        for index in sorted(tool_slots):
            slot = tool_slots[index]
            if not slot["name"]:
                continue
            yield StreamEvent.call(ToolCall(
                name=slot["name"],
                arguments=_loads(slot["arguments"]),
                id=slot["id"] or f"call_{index}_{abs(hash(slot['name'])) % 10 ** 8}",
                raw=slot["arguments"],
            ))

        if raw_usage:
            usage.input_tokens = int(raw_usage.get("prompt_tokens") or 0)
            usage.output_tokens = int(raw_usage.get("completion_tokens") or 0)
            details = raw_usage.get("prompt_tokens_details") or {}
            usage.cache_read_tokens = int(details.get("cached_tokens") or 0)
        compute_cost(request.model, usage)
        yield StreamEvent("usage", usage=usage)
        yield StreamEvent.stop(_map_finish(stop_reason))

    # ------------------------------------------------------------------------- тело
    def _build_body(self, request: ChatRequest) -> dict[str, Any]:
        messages: list[dict[str, Any]] = []
        if request.system:
            messages.append({"role": "system", "content": request.system})
        for message in request.messages:
            messages.append(self._convert_message(message))

        body: dict[str, Any] = {
            "model": request.model,
            "messages": messages,
            "stream": True,
        }
        # Локальные и часть облачных моделей капризны к параметрам
        if self.capabilities.local:
            body["temperature"] = request.temperature
            body["stream_options"] = {"include_usage": True}
        else:
            body["temperature"] = request.temperature
            body["stream_options"] = {"include_usage": True}
        if request.max_tokens:
            # новые модели OpenAI требуют max_completion_tokens
            if self.name == "openai" and request.model.startswith(("o1", "o3", "o4", "gpt-5")):
                body["max_completion_tokens"] = request.max_tokens
                body.pop("temperature", None)
                if request.reasoning_effort:
                    body["reasoning_effort"] = request.reasoning_effort
            else:
                body["max_tokens"] = request.max_tokens
        if request.stop:
            body["stop"] = request.stop
        if request.tools:
            body["tools"] = [{"type": "function", "function": tool} for tool in request.tools]
            body["tool_choice"] = "auto"
        return body

    def _convert_message(self, message: Message) -> dict[str, Any]:
        if message.role == "tool":
            return {
                "role": "tool",
                "tool_call_id": message.tool_call_id,
                "content": message.content or "(пустой результат)",
            }
        if message.role == "assistant" and message.tool_calls:
            return {
                "role": "assistant",
                "content": message.content or None,
                "tool_calls": [
                    {
                        "id": call.id,
                        "type": "function",
                        "function": {
                            "name": call.name,
                            "arguments": json.dumps(call.arguments or {}, ensure_ascii=False),
                        },
                    }
                    for call in message.tool_calls
                ],
            }
        return {"role": message.role, "content": message.content}

    # -------------------------------------------------------------------- служебное
    def list_models(self) -> list[str]:
        try:
            headers = self._headers({"Authorization": f"Bearer {self.api_key}"} if self.api_key else {})
            data = self.http.get_json(f"{self.base_url}/models", headers=headers, retries=0)
            items = data.get("data") if isinstance(data, dict) else data
            return sorted({m.get("id", "") for m in (items or []) if m.get("id")})
        except Exception:  # noqa: BLE001 — сервис может не поддерживать /models
            return []

    def health(self) -> tuple[bool, str]:
        if not self.api_key and not self.capabilities.local:
            return False, f"Не задан ключ для {self.display_name} (aia keys set {self.name} <ключ>)"
        try:
            models = self.list_models()
            if models:
                return True, f"доступно моделей: {len(models)}"
            return True, "API отвечает (список моделей недоступен)"
        except Exception as e:  # noqa: BLE001
            return False, str(e)


# --------------------------------------------------------------------------------------
# Конкретные сервисы: отличаются адресом и поведением
# --------------------------------------------------------------------------------------
class OpenAIProvider(OpenAICompatibleProvider):
    name = "openai"
    display_name = "OpenAI"
    default_base_url = "https://api.openai.com/v1"


class OpenRouterProvider(OpenAICompatibleProvider):
    name = "openrouter"
    display_name = "OpenRouter"
    default_base_url = "https://openrouter.ai/api/v1"

    def list_models(self) -> list[str]:
        models = super().list_models()
        # Отфильтровываем явно «мёртвые» записи вида :free при недоступности
        return models


class GroqProvider(OpenAICompatibleProvider):
    name = "groq"
    display_name = "Groq (быстрый инференс)"
    default_base_url = "https://api.groq.com/openai/v1"


class DeepSeekProvider(OpenAICompatibleProvider):
    name = "deepseek"
    display_name = "DeepSeek"
    default_base_url = "https://api.deepseek.com/v1"


class MistralProvider(OpenAICompatibleProvider):
    name = "mistral"
    display_name = "Mistral"
    default_base_url = "https://api.mistral.ai/v1"


class LocalServerProvider(OpenAICompatibleProvider):
    """LM Studio / llama.cpp server / vLLM / LocalAI — ключ не нужен."""

    name = "local"
    display_name = "Локальный OpenAI-сервер"
    default_base_url = "http://127.0.0.1:1234/v1"
    capabilities = ProviderCapabilities(
        tools=True, streaming=True, system_prompt=True, parallel_tools=False, local=True,
    )

    def __init__(self, config) -> None:
        super().__init__(config)
        self.api_key = "local"


def _loads(raw: str) -> dict[str, Any]:
    if not raw or not raw.strip():
        return {}
    try:
        data = json.loads(raw)
    except json.JSONDecodeError:
        from ..utils.json_utils import loads_lenient
        data = loads_lenient(raw)
        if data is None:
            return {"__raw__": raw}
    if isinstance(data, dict):
        return {k: v for k, v in data.items() if k != "__raw__"} or data
    return {"value": data}


def _map_finish(reason: str) -> str:
    return {
        "stop": "end_turn",
        "length": "max_tokens",
        "tool_calls": "tool_use",
        "function_call": "tool_use",
        "content_filter": "refusal",
    }.get(reason, reason or "end_turn")


__all__ = [
    "DeepSeekProvider", "GroqProvider", "LocalServerProvider", "MistralProvider",
    "OpenAICompatibleProvider", "OpenAIProvider", "OpenRouterProvider",
]
