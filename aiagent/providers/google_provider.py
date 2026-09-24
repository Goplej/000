"""Провайдер Google Gemini (generativelanguage API) со стримингом и function calling."""
from __future__ import annotations

from collections.abc import Iterator
from typing import Any

from ..core.types import Message, ToolCall, Usage
from ..errors import ProviderError
from ..utils.logging import get_logger
from .base import BaseProvider, ChatRequest, ProviderCapabilities, StreamEvent
from .pricing import compute_cost

log = get_logger("google")


class GoogleProvider(BaseProvider):
    name = "google"
    display_name = "Google Gemini"
    default_base_url = "https://generativelanguage.googleapis.com/v1beta"
    capabilities = ProviderCapabilities(
        tools=True, streaming=True, vision=True, system_prompt=True, parallel_tools=True,
    )

    # ------------------------------------------------------------------------ запрос
    def stream(self, request: ChatRequest) -> Iterator[StreamEvent]:
        self._check_key()
        model = (request.model or self.config.resolved_model()).replace("models/", "")
        url = f"{self.base_url}/models/{model}:streamGenerateContent"
        body = self._build_body(request)
        headers = self._headers({
            "x-goog-api-key": self.api_key,
            "Accept": "text/event-stream",
        })

        usage = Usage()
        stop_reason = "end_turn"
        buffer: list[dict[str, Any]] = []

        for _, chunk in self.http.stream_sse(
            "POST", url, json_body=body, headers=headers,
        ):
            if chunk.get("error"):
                error = chunk["error"]
                raise ProviderError(
                    f"Gemini: {error.get('message', 'ошибка')}", provider=self.name
                )
            meta = chunk.get("usageMetadata") or {}
            if meta:
                usage.input_tokens = int(meta.get("promptTokenCount") or usage.input_tokens)
                usage.output_tokens = int(meta.get("candidatesTokenCount") or usage.output_tokens)
                usage.cache_read_tokens = int(meta.get("cachedContentTokenCount") or 0)

            for candidate in chunk.get("candidates") or []:
                if candidate.get("finishReason"):
                    stop_reason = candidate["finishReason"]
                content = candidate.get("content") or {}
                for part in content.get("parts") or []:
                    if part.get("text"):
                        yield StreamEvent.delta(part["text"])
                    if part.get("thought"):
                        yield StreamEvent.thinking(part["text"] or "")
                    call = part.get("functionCall")
                    if call:
                        buffer.append(call)

        for call in buffer:
            yield StreamEvent.call(ToolCall(
                name=str(call.get("name", "")),
                arguments=call.get("args") or {},
                id=f"call_{abs(hash(str(call.get('name')) + str(call.get('args')))) % 10 ** 12}",
            ))

        compute_cost(request.model, usage)
        yield StreamEvent("usage", usage=usage)
        yield StreamEvent.stop(_map_finish(stop_reason))

    # ------------------------------------------------------------------------- тело
    def _build_body(self, request: ChatRequest) -> dict[str, Any]:
        contents = self._convert_messages(request.messages)
        body: dict[str, Any] = {
            "contents": contents,
            "generationConfig": {
                "temperature": request.temperature,
                "maxOutputTokens": request.max_tokens,
            },
        }
        if request.system:
            body["systemInstruction"] = {"parts": [{"text": request.system}]}
        if request.tools:
            body["tools"] = [{
                "functionDeclarations": [
                    {
                        "name": tool["name"],
                        "description": tool.get("description", ""),
                        "parameters": _clean_schema(tool.get("parameters") or {"type": "object",
                                                                              "properties": {}}),
                    }
                    for tool in request.tools
                ]
            }]
        if request.stop:
            body["generationConfig"]["stopSequences"] = request.stop
        return body

    def _convert_messages(self, messages: list[Message]) -> list[dict[str, Any]]:
        contents: list[dict[str, Any]] = []
        pending_responses: list[dict[str, Any]] = []

        def flush() -> None:
            if pending_responses:
                contents.append({"role": "user", "parts": list(pending_responses)})
                pending_responses.clear()

        for message in messages:
            if message.role == "system":
                continue
            if message.role == "tool":
                pending_responses.append({
                    "functionResponse": {
                        "name": message.tool_name or "tool",
                        "response": {
                            "content": message.content or "",
                            **({"error": True} if message.is_error else {}),
                        },
                    }
                })
                continue

            flush()
            if message.role == "user":
                contents.append({"role": "user", "parts": [{"text": message.content}]})
                continue

            parts: list[dict[str, Any]] = []
            if message.content:
                parts.append({"text": message.content})
            for call in message.tool_calls:
                parts.append({"functionCall": {"name": call.name, "args": call.arguments or {}}})
            if not parts:
                parts.append({"text": "(пустой ответ)"})
            contents.append({"role": "model", "parts": parts})

        flush()
        if not contents:
            contents.append({"role": "user", "parts": [{"text": "Начни работу."}]})
        return contents

    # -------------------------------------------------------------------- служебное
    def list_models(self) -> list[str]:
        if not self.api_key:
            return []
        try:
            data = self.http.get_json(
                f"{self.base_url}/models",
                headers=self._headers({"x-goog-api-key": self.api_key}),
                retries=0,
            )
            out = []
            for item in data.get("models", []):
                methods = item.get("supportedGenerationMethods") or []
                if "generateContent" in methods:
                    out.append(str(item.get("name", "")).replace("models/", ""))
            return out
        except Exception:  # noqa: BLE001
            return []

    def health(self) -> tuple[bool, str]:
        if not self.api_key:
            return False, "Не задан GOOGLE_API_KEY / GEMINI_API_KEY (aia keys set google <ключ>)"
        try:
            models = self.list_models()
            return (True, f"доступно моделей: {len(models)}") if models else (True, "API отвечает")
        except Exception as e:  # noqa: BLE001
            return False, str(e)


def _clean_schema(schema: dict[str, Any]) -> dict[str, Any]:
    """Gemini не понимает часть полей JSON Schema — вычищаем."""
    allowed = {"type", "description", "properties", "required", "items", "enum", "nullable", "format"}
    out: dict[str, Any] = {}
    for key, value in (schema or {}).items():
        if key not in allowed:
            continue
        if key == "properties" and isinstance(value, dict):
            out[key] = {k: _clean_schema(v) for k, v in value.items()}
        elif key == "items" and isinstance(value, dict):
            out[key] = _clean_schema(value)
        else:
            out[key] = value
    return out


def _map_finish(reason: str) -> str:
    return {
        "STOP": "end_turn",
        "MAX_TOKENS": "max_tokens",
        "SAFETY": "refusal",
        "RECITATION": "refusal",
        "OTHER": "end_turn",
    }.get(str(reason).upper(), "end_turn")


__all__ = ["GoogleProvider"]
