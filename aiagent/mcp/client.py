"""MCP-клиент: подключение внешних инструментов по протоколу Model Context Protocol.

Реализован JSON-RPC 2.0: initialize → tools/list → tools/call.
Работает с любыми MCP-серверами: filesystem, git, postgres, playwright и т.д.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any

from ..errors import MCPError
from ..utils.logging import get_logger
from .transport import Transport, build_transport

log = get_logger("mcp.client")
PROTOCOL_VERSION = "2025-06-18"


@dataclass
class McpTool:
    """Инструмент, предоставленный MCP-сервером."""

    name: str
    description: str = ""
    input_schema: dict[str, Any] = field(default_factory=dict)
    server: str = ""

    @property
    def full_name(self) -> str:
        return f"mcp__{self.server}__{self.name}"


class McpClient:
    """Один подключённый MCP-сервер."""

    def __init__(self, name: str, spec: dict[str, Any], cwd=None) -> None:
        self.name = name
        self.spec = spec or {}
        self.source = self.spec.get("_source", "")
        self.transport: Transport = build_transport(name, self.spec, cwd)
        self.tools: list[McpTool] = []
        self.server_info: dict[str, Any] = {}
        self._counter = 0
        self._initialized = False

    # ------------------------------------------------------------------ подключение
    def _next_id(self) -> int:
        self._counter += 1
        return self._counter

    def initialize(self) -> None:
        if self._initialized:
            return
        response = self.transport.send({
            "jsonrpc": "2.0",
            "id": self._next_id(),
            "method": "initialize",
            "params": {
                "protocolVersion": PROTOCOL_VERSION,
                "capabilities": {"roots": {"listChanged": False}, "sampling": {}},
                "clientInfo": {"name": "aiagent-studio", "version": "1.0.0"},
            },
        })
        if response and "error" in response:
            raise MCPError(f"MCP «{self.name}»: {response['error'].get('message', 'ошибка init')}")
        self.server_info = (response or {}).get("result", {}).get("serverInfo", {})
        self.transport.notify({"jsonrpc": "2.0", "method": "notifications/initialized"})
        self._initialized = True

    def list_tools(self) -> list[McpTool]:
        self.initialize()
        response = self.transport.send({
            "jsonrpc": "2.0", "id": self._next_id(), "method": "tools/list", "params": {},
        })
        if response is None:
            return []
        if "error" in response:
            raise MCPError(f"MCP «{self.name}»: {response['error'].get('message', 'ошибка tools/list')}")
        tools = []
        for item in (response.get("result") or {}).get("tools", []):
            tools.append(McpTool(
                name=item.get("name", ""),
                description=item.get("description", ""),
                input_schema=item.get("inputSchema") or {"type": "object", "properties": {}},
                server=self.name,
            ))
        self.tools = tools
        return tools

    def call_tool(self, tool_name: str, arguments: dict[str, Any]) -> tuple[bool, str]:
        self.initialize()
        response = self.transport.send({
            "jsonrpc": "2.0", "id": self._next_id(), "method": "tools/call",
            "params": {"name": tool_name, "arguments": arguments or {}},
        }, timeout=float(self.spec.get("timeout", 120)))
        if response is None:
            return False, "сервер не ответил"
        if "error" in response:
            error = response["error"]
            return False, str(error.get("message") or error)
        result = response.get("result") or {}
        texts: list[str] = []
        for item in result.get("content") or []:
            if item.get("type") == "text":
                texts.append(str(item.get("text", "")))
            elif item.get("type") == "resource":
                texts.append(f"[resource {item.get('resource', {}).get('uri', '')}]")
            else:
                texts.append(f"[{item.get('type', 'unknown')} содержимое]")
        body = "\n".join(texts) or "(пустой ответ)"
        return not bool(result.get("isError")), body

    def close(self) -> None:
        self.transport.close()

    def describe(self) -> str:
        info = self.server_info.get("name") or self.transport.kind
        return f"{self.name} ({info}, инструментов {len(self.tools)})"


__all__ = ["McpClient", "McpTool", "PROTOCOL_VERSION"]
