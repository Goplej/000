"""Провайдер Anthropic (Claude) — Messages API со стримингом и инструментами.

Поддерживает: системный промпт, tool use, расширенное мышление (thinking),
кэширование промпта, подсчёт токенов через официальный endpoint.
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

log = get_logger("anthropic")
ANTHROPIC_VERSION = "2023-06-01"


class AnthropicProvider(BaseProvider):
    name = "anthropic"
    display_name = "Anthropic Claude"
    default_base_url = "https://api.anthropic.com"
    capabilities = ProviderCapabilities(
        tools=True, streaming=True, vision=True, thinking=True,
        system_prompt=True, parallel_tools=True, cache=True,
    )
    max_thinking_tokens = 32_000

    # ------------------------------------------------------------------------ запрос
    def stream(self, request: ChatRequest) -> Iterator[StreamEvent]:
        self._check_key()
        body = self._build_body(request)
        headers = self._headers({
            "x-api-key": self.api_key,
            "anthropic-version": ANTHROPIC_VERSION,
            "anthropic-beta": "prompt-caching-2024-07-31",
            "Accept": "text/event-stream",
        })

        tool_buffers: dict[int, dict[str, Any]] = {}
        usage = Usage()
        stop_reason = "end_turn"

        try:
            for event_name, data in self.http.stream_sse(
                "POST", f"{self.base_url}/v1/messages", json_body=body, headers=headers
            ):
                kind = data.get("type") or event_name

                if kind == "error":
                    error = data.get("error") or {}
                    raise ProviderError(
                        f"Anthropic: {error.get('message', 'неизвестная ошибка')}",
                        provider=self.name, status=error.get("type") == "overloaded_error" and 529 or None,
                        retryable=True,
                    )

                if kind == "message_start":
                    info = (data.get("message") or {}).get("usage") or {}
                    usage.input_tokens += int(info.get("input_tokens") or 0)
                    usage.cache_read_tokens += int(info.get("cache_read_input_tokens") or 0)
                    usage.cache_write_tokens += int(info.get("cache_creation_input_tokens") or 0)

                elif kind == "content_block_start":
                    block = data.get("content_block") or {}
                    if block.get("type") == "tool_use":
                        tool_buffers[data.get("index", 0)] = {
                            "id": block.get("id", ""),
                            "name": block.get("name", ""),
                            "json": "",
                        }

                elif kind == "content_block_delta":
                    delta = data.get("delta") or {}
                    delta_type = delta.get("type")
                    if delta_type == "text_delta":
                        yield StreamEvent.delta(delta.get("text", ""))
                    elif delta_type == "thinking_delta":
                        yield StreamEvent.thinking(delta.get("thinking", ""))
                    elif delta_type == "input_json_delta":
                        slot = tool_buffers.setdefault(data.get("index", 0),
                                                       {"id": "", "name": "", "json": ""})
                        slot["json"] += delta.get("partial_json", "")

                elif kind == "content_block_stop":
                    slot = tool_buffers.pop(data.get("index", 0), None)
                    if slot:
                        yield StreamEvent.call(ToolCall(
                            name=slot["name"],
                            arguments=_loads(slot["json"]),
                            id=slot["id"] or f"call_{abs(hash(slot['name'])) % 10 ** 10}",
                            raw=slot["json"],
                        ))

                elif kind == "message_delta":
                    stop_reason = (data.get("delta") or {}).get("stop_reason") or stop_reason
                    info = data.get("usage") or {}
                    usage.output_tokens += int(info.get("output_tokens") or 0)

                elif kind == "message_stop":
                    break

        except ProviderError:
            raise
        except Exception as e:  # noqa: BLE001 — любые сетевые сбои показываем понятно
            raise ProviderError(f"Anthropic: сбой соединения ({e})", provider=self.name,
                                retryable=True) from e

        compute_cost(request.model, usage)
        yield StreamEvent("usage", usage=usage)
        yield StreamEvent.stop(_map_stop(stop_reason))

    # ---------------------------------------------------------------------- разбор
    def _build_body(self, request: ChatRequest) -> dict[str, Any]:
        body: dict[str, Any] = {
            "model": request.model,
            "max_tokens": min(request.max_tokens, 64_000),
            "messages": self._convert_messages(request.messages),
            "stream": True,
        }
        if request.temperature is not None:
            body["temperature"] = min(max(request.temperature, 0.0), 1.0)
        if request.stop:
            body["stop_sequences"] = request.stop

        system_blocks: list[dict[str, Any]] = []
        if request.system:
            block: dict[str, Any] = {"type": "text", "text": request.system}
            if self.capabilities.cache:
                block["cache_control"] = {"type": "ephemeral"}
            system_blocks.append(block)
        if system_blocks:
            body["system"] = system_blocks

        if request.tools:
            body["tools"] = [
                {
                    "name": tool["name"],
                    "description": tool.get("description", ""),
                    "input_schema": tool.get("parameters") or {"type": "object", "properties": {}},
                }
                for tool in request.tools
            ]

        budget = request.thinking_budget or self.config.thinking_budget
        if budget > 0 and self.capabilities.thinking:
            budget = min(budget, self.max_thinking_tokens, max(1024, request.max_tokens - 1024))
            body["thinking"] = {"type": "enabled", "budget_tokens": budget}
            body.pop("temperature", None)  # Anthropic не разрешает temperature с thinking
        return body

    def _convert_messages(self, messages: list[Message]) -> list[dict[str, Any]]:
        """Приводит внутренние сообщения к формату Anthropic (блоки контента)."""
        out: list[dict[str, Any]] = []
        pending_tool_results: list[dict[str, Any]] = []

        def flush_tool_results() -> None:
            if pending_tool_results:
                out.append({"role": "user", "content": list(pending_tool_results)})
                pending_tool_results.clear()

        for message in messages:
            if message.role == "system":
                continue
            if message.role == "tool":
                pending_tool_results.append({
                    "type": "tool_result",
                    "tool_use_id": message.tool_call_id,
                    "content": message.content or "(пустой ответ инструмента)",
                    **({"is_error": True} if message.is_error else {}),
                })
                continue

            flush_tool_results()

            if message.role == "user":
                out.append({"role": "user", "content": [{"type": "text", "text": message.content}]})
                continue

            blocks: list[dict[str, Any]] = []
            if message.content:
                blocks.append({"type": "text", "text": message.content})
            for call in message.tool_calls:
                blocks.append({
                    "type": "tool_use",
                    "id": call.id,
                    "name": call.name,
                    "input": call.arguments or {},
                })
            if not blocks:
                blocks.append({"type": "text", "text": "(пустой ответ)"})
            out.append({"role": "assistant", "content": blocks})

        flush_tool_results()

        if not out:
            out.append({"role": "user", "content": [{"type": "text", "text": "Начни работу."}]})
        # Anthropic требует, чтобы первый блок был от пользователя
        if out[0]["role"] != "user":
            out.insert(0, {"role": "user", "content": [{"type": "text", "text": "(продолжение)"}]})
        return out

    # -------------------------------------------------------------------- служебное
    def count_tokens(self, messages: list[Message], system: str = "") -> int:
        """Точный подсчёт через API; при ошибке — эвристика."""
        if not self.api_key:
            return super().count_tokens(messages, system)
        try:
            payload = {
                "model": self.config.resolved_model(),
                "messages": self._convert_messages([m for m in messages if m.role != "system"]),
            }
            if system:
                payload["system"] = [{"type": "text", "text": system}]
            data = self.http.post_json(
                f"{self.base_url}/v1/messages/count_tokens",
                payload,
                headers=self._headers({"x-api-key": self.api_key,
                                       "anthropic-version": ANTHROPIC_VERSION}),
                retries=0,
            )
            return int(data.get("input_tokens") or 0)
        except Exception as e:  # noqa: BLE001
            log.debug("count_tokens недоступен: %s", e)
            return super().count_tokens(messages, system)

    def list_models(self) -> list[str]:
        if not self.api_key:
            return []
        try:
            data = self.http.get_json(
                f"{self.base_url}/v1/models",
                headers=self._headers({"x-api-key": self.api_key,
                                       "anthropic-version": ANTHROPIC_VERSION}),
                retries=0,
            )
            return [m.get("id", "") for m in data.get("data", []) if m.get("id")]
        except Exception:  # noqa: BLE001
            return []

    def health(self) -> tuple[bool, str]:
        if not self.api_key:
            return False, "Не задан ANTHROPIC_API_KEY (aia keys set anthropic <ключ>)"
        try:
            self.list_models()
            return True, ""
        except Exception as e:  # noqa: BLE001
            return False, str(e)


def _loads(raw: str) -> dict[str, Any]:
    if not raw.strip():
        return {}
    try:
        data = json.loads(raw)
        return data if isinstance(data, dict) else {"value": data}
    except json.JSONDecodeError:
        return {"__raw__": raw}


def _map_stop(reason: str) -> str:
    return {
        "end_turn": "end_turn",
        "tool_use": "tool_use",
        "max_tokens": "max_tokens",
        "stop_sequence": "stop_sequence",
        "refusal": "refusal",
    }.get(reason, reason or "end_turn")


__all__ = ["AnthropicProvider"]
