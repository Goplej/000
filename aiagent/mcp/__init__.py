"""Клиент MCP (Model Context Protocol): подключение внешних инструментов.

Позволяет агенту использовать любые MCP-серверы — файловые системы, базы данных,
браузер, git и тысячи готовых интеграций, — как свои собственные инструменты.
"""

from .client import PROTOCOL_VERSION, McpClient, McpTool
from .registry import close_all, describe_servers, load_servers, load_specs, mcp_tools
from .transport import HttpTransport, StdioTransport, Transport, build_transport

__all__ = [
    "HttpTransport", "McpClient", "McpTool", "PROTOCOL_VERSION", "StdioTransport",
    "Transport", "build_transport", "close_all", "describe_servers", "load_servers",
    "load_specs", "mcp_tools",
]
