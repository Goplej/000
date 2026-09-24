"""Транспорты MCP: stdio (подпроцесс) и HTTP (Streamable HTTP / SSE).

Спецификация MCP — JSON-RPC 2.0 поверх одного из транспортов.
Здесь нет зависимостей: только subprocess и urllib.
"""
from __future__ import annotations

import json
import os
import subprocess
import threading
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any

from ..errors import MCPError
from ..utils.logging import get_logger

log = get_logger("mcp.transport")


class Transport:
    """Базовый транспорт: send(request) → ответ (dict) или None для уведомлений."""

    kind = "base"

    def __init__(self, name: str) -> None:
        self.name = name
        self.closed = False

    def send(self, message: dict[str, Any], *, timeout: float = 60.0) -> dict[str, Any] | None:
        raise NotImplementedError

    def notify(self, message: dict[str, Any]) -> None:
        try:
            self.send(message, timeout=5.0)
        except Exception as e:  # noqa: BLE001 — уведомления не критичны
            log.debug("Уведомление не доставлено: %s", e)

    def close(self) -> None:
        self.closed = True


class StdioTransport(Transport):
    """Запускает сервер как подпроцесс и общается построчно (JSON-RPC)."""

    kind = "stdio"

    def __init__(self, name: str, command: list[str], *, cwd: str | Path | None = None,
                 env: dict[str, str] | None = None) -> None:
        super().__init__(name)
        self.command = command
        self.cwd = str(cwd) if cwd else None
        self.env = {**os.environ, **(env or {})}
        self.process: subprocess.Popen | None = None
        self._lock = threading.Lock()

    def start(self) -> None:
        if self.process is not None and self.process.poll() is None:
            return
        try:
            self.process = subprocess.Popen(
                self.command, cwd=self.cwd, env=self.env,
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, encoding="utf-8", errors="replace", bufsize=1,
            )
        except FileNotFoundError as e:
            raise MCPError(
                f"MCP-сервер «{self.name}»: команда не найдена: {self.command[0]}",
                hint="Проверь, установлен ли сервер (например, Node.js для npx).",
            ) from e
        except OSError as e:
            raise MCPError(f"MCP-сервер «{self.name}» не запустился: {e}") from e

    def send(self, message: dict[str, Any], *, timeout: float = 60.0) -> dict[str, Any] | None:
        self.start()
        assert self.process and self.process.stdin and self.process.stdout

        with self._lock:
            try:
                self.process.stdin.write(json.dumps(message, ensure_ascii=False) + "\n")
                self.process.stdin.flush()
            except (BrokenPipeError, OSError) as e:
                raise MCPError(f"MCP-сервер «{self.name}» разорвал соединение: {e}") from e

            if "id" not in message:
                return None

            line = self.process.stdout.readline()
            if not line:
                error = ""
                if self.process.stderr:
                    error = self.process.stderr.read()[:500]
                raise MCPError(
                    f"MCP-сервер «{self.name}» закрылся без ответа. {error}".strip()
                )
            try:
                return json.loads(line)
            except json.JSONDecodeError as e:
                raise MCPError(
                    f"MCP-сервер «{self.name}» вернул не JSON: {line[:200]}"
                ) from e

    def close(self) -> None:
        super().close()
        if self.process and self.process.poll() is None:
            try:
                self.process.terminate()
                self.process.wait(timeout=3)
            except (subprocess.TimeoutExpired, OSError):
                try:
                    self.process.kill()
                except OSError:
                    pass


class HttpTransport(Transport):
    """Streamable HTTP транспорт: POST JSON-RPC на указанный URL."""

    kind = "http"

    def __init__(self, name: str, url: str, headers: dict[str, str] | None = None) -> None:
        super().__init__(name)
        self.url = url
        self.headers = {"Content-Type": "application/json", "Accept": "application/json",
                        **(headers or {})}
        self.session_id = ""

    def send(self, message: dict[str, Any], *, timeout: float = 60.0) -> dict[str, Any] | None:
        payload = json.dumps(message, ensure_ascii=False).encode("utf-8")
        headers = dict(self.headers)
        if self.session_id:
            headers["Mcp-Session-Id"] = self.session_id
        request = urllib.request.Request(self.url, data=payload, headers=headers, method="POST")
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                session = response.headers.get("Mcp-Session-Id")
                if session:
                    self.session_id = session
                body = response.read().decode("utf-8", "replace").strip()
        except urllib.error.HTTPError as e:
            raise MCPError(f"MCP-сервер «{self.name}»: HTTP {e.code}") from e
        except (urllib.error.URLError, OSError) as e:
            raise MCPError(f"MCP-сервер «{self.name}» недоступен: {e}") from e

        if not body:
            return None
        # Сервер может ответить событием SSE — вытаскиваем первый data:
        for line in body.splitlines():
            line = line.strip()
            if line.startswith("data:"):
                line = line[5:].strip()
            if line.startswith("{"):
                try:
                    return json.loads(line)
                except json.JSONDecodeError:
                    continue
        raise MCPError(f"MCP-сервер «{self.name}» вернул неожиданный ответ")


def build_transport(name: str, spec: dict[str, Any], cwd: Path | None = None) -> Transport:
    """Создаёт транспорт по описанию сервера из .mcp.json."""
    if "command" in spec:
        command = spec["command"]
        args = spec.get("args") or []
        if isinstance(command, str):
            command = [command, *args]
        return StdioTransport(name, list(command), cwd=spec.get("cwd") or cwd,
                              env=spec.get("env"))
    if "url" in spec:
        return HttpTransport(name, spec["url"], spec.get("headers"))
    raise MCPError(f"MCP-сервер «{name}»: нужен либо command, либо url")


__all__ = ["HttpTransport", "StdioTransport", "Transport", "build_transport"]
