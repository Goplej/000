"""HTTP-клиент на стандартной библиотеке: JSON, SSE-стриминг, ретраи, таймауты.

Почему не requests/httpx: ядро агента должно ставиться одной командой
на любую машину без зависимостей. Всё, что нужно от HTTP, здесь есть.
"""
from __future__ import annotations

import json
import random
import ssl
import time
import urllib.error
import urllib.parse
import urllib.request
from collections.abc import Iterator
from dataclasses import dataclass, field
from typing import Any

from ..errors import ProviderError, RateLimitError

USER_AGENT = "AI-Agent-Studio/1.0 (+https://localhost)"


class HttpError(ProviderError):
    """Ошибка HTTP с сохранением тела ответа (модели часто пишут причину именно там)."""

    def __init__(self, message: str, *, body: str = "", **kwargs: Any) -> None:
        super().__init__(message, **kwargs)
        self.body = body

    def human(self) -> str:
        base = super().human()
        if self.body:
            return f"{base}\n  Ответ сервера: {self.body[:600]}"
        return base


@dataclass
class HttpResponse:
    status: int
    headers: dict[str, str] = field(default_factory=dict)
    body: bytes = b""

    @property
    def text(self) -> str:
        charset = "utf-8"
        ctype = self.headers.get("content-type", "")
        if "charset=" in ctype:
            charset = ctype.split("charset=", 1)[1].split(";")[0].strip() or "utf-8"
        return self.body.decode(charset, "replace")

    def json(self) -> Any:
        try:
            return json.loads(self.text)
        except json.JSONDecodeError as e:
            raise HttpError(f"Ответ не является JSON: {e}", body=self.text[:500]) from e


class HttpClient:
    """Мини-клиент с ретраями, поддержкой прокси из окружения и SSE."""

    def __init__(self, *, timeout: int = 120, connect_timeout: int = 15,
                 max_retries: int = 3, user_agent: str = USER_AGENT) -> None:
        self.timeout = timeout
        self.connect_timeout = connect_timeout
        self.max_retries = max_retries
        self.user_agent = user_agent
        self._ssl = ssl.create_default_context()

    # ------------------------------------------------------------------ публичный API
    def request(
        self,
        method: str,
        url: str,
        *,
        json_body: dict[str, Any] | None = None,
        data: bytes | None = None,
        headers: dict[str, str] | None = None,
        timeout: int | None = None,
        retries: int | None = None,
        stream: bool = False,
    ):
        """Выполняет запрос. stream=True возвращает открытый объект ответа (нужно закрыть)."""
        payload = data
        all_headers = {
            "User-Agent": self.user_agent,
            "Accept": "application/json",
        }
        if json_body is not None:
            payload = json.dumps(json_body, ensure_ascii=False).encode("utf-8")
            all_headers["Content-Type"] = "application/json; charset=utf-8"
        all_headers.update(headers or {})

        attempts = self.max_retries if retries is None else retries
        last_error: Exception | None = None

        for attempt in range(max(1, attempts + 1)):
            request = urllib.request.Request(url, data=payload, method=method.upper())
            for key, value in all_headers.items():
                request.add_header(key, value)
            try:
                response = urllib.request.urlopen(
                    request,
                    timeout=timeout or self.timeout,
                    context=self._ssl if url.startswith("https") else None,
                )
                if stream:
                    return response
                with response:
                    body = response.read()
                    return HttpResponse(
                        status=response.status,
                        headers={k.lower(): v for k, v in response.headers.items()},
                        body=body,
                    )
            except urllib.error.HTTPError as e:
                body = ""
                try:
                    body = e.read().decode("utf-8", "replace")[:2000]
                except Exception:  # noqa: BLE001
                    pass
                if e.code == 429:
                    retry_after = _retry_after(e.headers, attempt)
                    if attempt < attempts:
                        time.sleep(retry_after)
                        last_error = RateLimitError(
                            "Провайдер ограничил частоту запросов (429).",
                            retry_after=retry_after,
                        )
                        continue
                    raise RateLimitError(
                        "Провайдер ограничил частоту запросов. Подожди или смени модель.",
                        retry_after=retry_after,
                    ) from e
                if e.code in {500, 502, 503, 504, 529} and attempt < attempts:
                    time.sleep(_backoff(attempt))
                    last_error = _http_error(e, body)
                    continue
                raise _http_error(e, body) from e
            except urllib.error.URLError as e:
                reason = getattr(e, "reason", e)
                last_error = HttpError(
                    f"Не удалось соединиться: {reason}",
                    hint="Проверь интернет, VPN/прокси или адрес API.",
                    retryable=True,
                )
                if attempt < attempts:
                    time.sleep(_backoff(attempt))
                    continue
                raise last_error from e
            except TimeoutError as e:
                last_error = HttpError(
                    f"Таймаут запроса ({self.timeout} с)", retryable=True,
                    hint="Модель слишком долго отвечает.",
                )
                if attempt < attempts:
                    continue
                raise last_error from e

        raise last_error or HttpError("Неизвестная ошибка сети")

    def get_json(self, url: str, headers: dict[str, str] | None = None, **kwargs: Any) -> Any:
        return self.request("GET", url, headers=headers, **kwargs).json()

    def post_json(self, url: str, body: dict[str, Any], headers: dict[str, str] | None = None,
                  **kwargs: Any) -> Any:
        return self.request("POST", url, json_body=body, headers=headers, **kwargs).json()

    def stream_sse(
        self,
        method: str,
        url: str,
        *,
        json_body: dict[str, Any] | None = None,
        headers: dict[str, str] | None = None,
        timeout: int | None = None,
    ) -> Iterator[tuple[str, dict[str, Any]]]:
        """
        Читает поток Server-Sent Events и отдаёт пары (event, data).
        Понимает и SSE-формат OpenAI/Anthropic, и построчный JSON (Ollama).
        """
        response = self.request(
            method, url, json_body=json_body, headers=headers, timeout=timeout, stream=True
        )
        try:
            event_name = ""
            data_lines: list[str] = []
            for raw in response:
                line = raw.decode("utf-8", "replace").rstrip("\r\n")
                if not line:
                    if data_lines:
                        payload = "\n".join(data_lines)
                        if payload.strip() and payload.strip() != "[DONE]":
                            parsed = _safe_json(payload)
                            if parsed is not None:
                                yield event_name, parsed
                        event_name, data_lines = "", []
                    continue
                if line.startswith(":"):       # комментарий/keep-alive
                    continue
                if line.startswith("event:"):
                    event_name = line[6:].strip()
                    continue
                if line.startswith("data:"):
                    data_lines.append(line[5:].lstrip())
                    continue
                # Не-SSE поток (например, NDJSON от Ollama)
                parsed = _safe_json(line)
                if parsed is not None:
                    yield "", parsed
            if data_lines:
                payload = "\n".join(data_lines)
                parsed = _safe_json(payload)
                if parsed is not None:
                    yield event_name, parsed
        finally:
            try:
                response.close()
            except Exception:  # noqa: BLE001
                pass


def stream_lines(url: str, *, headers: dict[str, str] | None = None, timeout: int = 60) -> Iterator[str]:
    """Простой построчный стрим (для скачивания больших текстовых файлов)."""
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT, **(headers or {})})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        for raw in response:
            yield raw.decode("utf-8", "replace")


# --------------------------------------------------------------------------------------
# Внутреннее
# --------------------------------------------------------------------------------------
def _safe_json(text: str) -> Any:
    try:
        return json.loads(text)
    except (json.JSONDecodeError, ValueError):
        return None


def _http_error(e: urllib.error.HTTPError, body: str) -> HttpError:
    detail = _extract_message(body)
    mapping = {
        400: "Провайдер отклонил запрос (400)",
        401: "Ключ API не принят (401)",
        403: "Доступ запрещён (403)",
        404: "Модель или адрес не найдены (404)",
        413: "Запрос слишком большой (413)",
        422: "Некорректные параметры запроса (422)",
    }
    message = mapping.get(e.code, f"Ошибка HTTP {e.code}")
    if detail:
        message += f": {detail}"
    hint = ""
    if e.code in {401, 403}:
        hint = "Проверь ключ API: aia doctor --keys"
    elif e.code == 404:
        hint = "Возможно, неверное имя модели или базовый URL."
    return HttpError(message, status=e.code, body=body[:1000], hint=hint, retryable=e.code >= 500)


def _extract_message(body: str) -> str:
    data = _safe_json(body)
    if isinstance(data, dict):
        error = data.get("error")
        if isinstance(error, dict):
            return str(error.get("message") or error.get("type") or "")[:300]
        if isinstance(error, str):
            return error[:300]
        for key in ("message", "detail", "error_description"):
            if data.get(key):
                return str(data[key])[:300]
    return ""


def _backoff(attempt: int) -> float:
    return min(30.0, (2 ** attempt) + random.uniform(0, 1.0))


def _retry_after(headers: Any, attempt: int) -> float:
    try:
        value = headers.get("retry-after") if headers else None
        if value:
            return max(1.0, float(str(value).strip()))
    except (TypeError, ValueError):
        pass
    return _backoff(attempt)


def quote_url(url: str, params: dict[str, Any] | None = None) -> str:
    """Добавляет query-параметры к URL, корректно кодируя значения."""
    if not params:
        return url
    parts = urllib.parse.urlparse(url)
    query = dict(urllib.parse.parse_qsl(parts.query))
    for key, value in params.items():
        if value is not None:
            query[key] = str(value)
    return urllib.parse.urlunparse(parts._replace(query=urllib.parse.urlencode(query)))


__all__ = ["HttpClient", "HttpError", "HttpResponse", "quote_url", "stream_lines"]
