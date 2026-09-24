"""Провайдер Ollama — локальные модели без интернета и без ключей.

Работает через /api/chat (NDJSON-стрим) и поддерживает нативные tools
для моделей, которые их умеют; остальным агент предлагает текстовый протокол.
"""
from __future__ import annotations

import json
import os
from collections.abc import Iterator
from typing import Any

from ..core.types import ToolCall, Usage
from ..errors import ProviderError
from ..utils.logging import get_logger
from .base import BaseProvider, ChatRequest, ProviderCapabilities, StreamEvent

log = get_logger("ollama")

#: Модели, у которых нативный tool calling работает стабильно.
NATIVE_TOOL_MODELS = (
    "qwen2.5", "qwen2.5-coder", "qwen3", "llama3.1", "llama3.2", "llama3.3",
    "mistral", "mistral-nemo", "firefunction", "command-r", "granite3",
    "devstral", "hermes3", "phi4", "deepseek-r1", "gpt-oss",
)


class OllamaProvider(BaseProvider):
    name = "ollama"
    display_name = "Ollama (локально)"
    default_base_url = "http://127.0.0.1:11434"
    capabilities = ProviderCapabilities(
        tools=True, streaming=True, vision=True, system_prompt=True,
        parallel_tools=True, local=True,
    )

    def __init__(self, config) -> None:
        super().__init__(config)
        env_url = os.environ.get("OLLAMA_HOST") or os.environ.get("AIA_OLLAMA_URL")
        if env_url and not config.base_url:
            self.base_url = env_url.rstrip("/")
            if not self.base_url.startswith("http"):
                self.base_url = f"http://{self.base_url}"
        self._tools_unsupported: set[str] = set()

    # ------------------------------------------------------------------------ запрос
    def stream(self, request: ChatRequest) -> Iterator[StreamEvent]:
        body = self._build_body(request)
        usage = Usage()
        stop_reason = "end_turn"
        stats: dict[str, Any] = {}

        try:
            for _, chunk in self.http.stream_sse(
                "POST", f"{self.base_url}/api/chat", json_body=body,
                headers=self._headers({"Accept": "application/x-ndjson"}),
                timeout=max(self.config.timeout, 600),   # локальные модели бывают медленными
            ):
                if chunk.get("error"):
                    message = str(chunk["error"])
                    if "does not support tools" in message.lower() and request.tools:
                        model = request.model
                        self._tools_unsupported.add(model)
                        log.warning("Модель %s не поддерживает tools — переключаюсь на текстовый протокол", model)
                        body.pop("tools", None)
                        fallback = self._build_body(request, tools=False)
                        yield from self._stream_once(fallback, request, usage)
                        return
                    raise ProviderError(f"Ollama: {message}", provider=self.name)

                message = chunk.get("message") or {}
                content = message.get("content") or ""
                if content:
                    yield StreamEvent.delta(content)
                thinking = message.get("thinking")
                if thinking:
                    yield StreamEvent.thinking(str(thinking))

                for call in message.get("tool_calls") or []:
                    function = call.get("function") or {}
                    arguments = function.get("arguments")
                    if isinstance(arguments, str):
                        arguments = _loads(arguments)
                    yield StreamEvent.call(ToolCall(
                        name=str(function.get("name", "")),
                        arguments=arguments if isinstance(arguments, dict) else {},
                        id=f"call_{abs(hash(str(function))) % 10 ** 12}",
                    ))

                if chunk.get("done"):
                    prompt_tokens = int(chunk.get("prompt_eval_count") or 0)
                    eval_tokens = int(chunk.get("eval_count") or 0)
                    usage.input_tokens += prompt_tokens
                    usage.output_tokens += eval_tokens
                    stop_reason = "tool_use" if message.get("tool_calls") else "end_turn"
                    duration = chunk.get("eval_duration") or 0
                    if eval_tokens and duration:
                        stats["tok_per_s"] = round(eval_tokens / (duration / 1e9), 1)
                    stats.update({
                        "load_ms": round((chunk.get("load_duration") or 0) / 1e6),
                        "prompt_tokens": prompt_tokens,
                        "output_tokens": eval_tokens,
                    })
                    break
        except ProviderError:
            raise
        except Exception as e:  # noqa: BLE001
            raise ProviderError(
                f"Ollama недоступен по {self.base_url} ({e})",
                provider=self.name,
                hint="Запусти Ollama (`ollama serve`) или укажи адрес: --url http://…",
                retryable=True,
            ) from e

        yield StreamEvent("usage", usage=usage)
        if stats:
            yield StreamEvent("meta", meta=stats)
        yield StreamEvent.stop(stop_reason)

    def supports_tools(self, model: str = "") -> bool:
        return self.supports_native_tools(model or self.config.resolved_model())

    def _stream_once(self, body: dict[str, Any], request: ChatRequest, usage: Usage) -> Iterator[StreamEvent]:
        for _, chunk in self.http.stream_sse(
            "POST", f"{self.base_url}/api/chat", json_body=body,
            headers=self._headers({"Accept": "application/x-ndjson"}),
            timeout=max(self.config.timeout, 600),
        ):
            message = chunk.get("message") or {}
            if message.get("content"):
                yield StreamEvent.delta(message["content"])
            if chunk.get("done"):
                usage.input_tokens += int(chunk.get("prompt_eval_count") or 0)
                usage.output_tokens += int(chunk.get("eval_count") or 0)
                break
        yield StreamEvent("usage", usage=usage)
        yield StreamEvent.stop("end_turn")

    # ------------------------------------------------------------------------- тело
    def _build_body(self, request: ChatRequest, tools: bool = True) -> dict[str, Any]:
        options: dict[str, Any] = {
            "temperature": request.temperature,
            "num_ctx": self._context_for(request.model),
            "num_predict": request.max_tokens,
        }
        body: dict[str, Any] = {
            "model": request.model,
            "messages": self._convert_messages(request),
            "stream": True,
            "options": options,
        }
        kv = os.environ.get("OLLAMA_KV_CACHE_TYPE")
        if kv:
            options["kv_cache_type"] = kv
        supports = self.supports_native_tools(request.model)
        if tools and request.tools and supports:
            body["tools"] = [{"type": "function", "function": tool} for tool in request.tools]
        return body

    def _convert_messages(self, request: ChatRequest) -> list[dict[str, Any]]:
        messages: list[dict[str, Any]] = []
        if request.system:
            messages.append({"role": "system", "content": request.system})
        for message in request.messages:
            if message.role == "tool":
                messages.append({
                    "role": "tool",
                    "content": message.content or "(пусто)",
                    "tool_name": message.tool_name,
                })
            elif message.role == "assistant" and message.tool_calls:
                messages.append({
                    "role": "assistant",
                    "content": message.content or "",
                    "tool_calls": [
                        {"function": {"name": call.name, "arguments": call.arguments}}
                        for call in message.tool_calls
                    ],
                })
            else:
                messages.append({"role": message.role, "content": message.content})
        return messages

    def _context_for(self, model: str) -> int:
        """Подбирает размер контекста: у 4 ГБ VRAM его нельзя раздувать."""
        from ..hardware import detect_gpus
        gpus = detect_gpus()
        vram = max([g.vram_gb for g in gpus], default=0.0)
        if vram and vram <= 4.5:
            return 8192
        if vram and vram <= 8.5:
            return 16384
        return min(self.config.context_window, 32768)

    def supports_native_tools(self, model: str) -> bool:
        if model in self._tools_unsupported:
            return False
        name = (model or "").lower()
        return any(key in name for key in NATIVE_TOOL_MODELS)

    # -------------------------------------------------------------------- служебное
    def list_models(self) -> list[str]:
        try:
            data = self.http.get_json(f"{self.base_url}/api/tags", retries=0, timeout=8)
            return [m.get("name", "") for m in data.get("models", []) if m.get("name")]
        except Exception:  # noqa: BLE001
            return []

    def running_models(self) -> list[dict[str, Any]]:
        try:
            data = self.http.get_json(f"{self.base_url}/api/ps", retries=0, timeout=8)
            return data.get("models", []) or []
        except Exception:  # noqa: BLE001
            return []

    def health(self) -> tuple[bool, str]:
        try:
            models = self.list_models()
        except Exception as e:  # noqa: BLE001
            return False, (f"Ollama недоступен по {self.base_url} ({e}).\n"
                           "  Запусти `ollama serve` или укажи --url http://хост:11434")
        if not models:
            return False, ("Ollama запущен, но моделей нет.\n"
                           "  Скачай модель: aia pull qwen2.5-coder:7b")
        return True, f"моделей: {len(models)}"


def _loads(raw: str) -> Any:
    try:
        return json.loads(raw)
    except (json.JSONDecodeError, TypeError):
        from ..utils.json_utils import loads_lenient
        return loads_lenient(raw) or {}


__all__ = ["NATIVE_TOOL_MODELS", "OllamaProvider"]
