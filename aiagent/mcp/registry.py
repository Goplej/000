"""Загрузка MCP-серверов и превращение их инструментов в инструменты агента."""
from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from ..config import AgentConfig
from ..errors import MCPError
from ..paths import mcp_path
from ..utils.logging import get_logger
from .client import McpClient, McpTool

log = get_logger("mcp")

CONFIG_NAMES = (".mcp.json", "mcp.json", ".aiagent/mcp.json")


def load_specs(config: AgentConfig) -> dict[str, dict[str, Any]]:
    """
    Собирает описания серверов из трёх мест (позже перекрывает раньше):
      ~/.aiagent/mcp.json  →  <проект>/.mcp.json
    Формат: {"mcpServers": {"имя": {"command": ..., "args": [...]} | {"url": ...}}}
    Поддерживается и плоский формат {"servers": [...]}.
    """
    specs: dict[str, dict[str, Any]] = {}
    candidates: list[Path] = [mcp_path()]
    for name in CONFIG_NAMES:
        candidates.append(Path(config.workspace) / name)

    for path in candidates:
        try:
            if not path.is_file():
                continue
            data = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as e:
            log.warning("Не удалось прочитать %s: %s", path, e)
            continue

        entries = data.get("mcpServers") or data.get("servers") or {}
        if isinstance(entries, list):
            entries = {item.get("name", f"server{index}"): item
                       for index, item in enumerate(entries)}
        for name, spec in entries.items():
            if not isinstance(spec, dict):
                continue
            spec = {**spec, "_source": str(path)}
            if spec.get("disabled"):
                continue
            specs[name] = spec
    return specs


def load_servers(config: AgentConfig) -> list[McpClient]:
    """Создаёт клиентов (без подключения — подключение ленивое, при первом вызове)."""
    servers: list[McpClient] = []
    for name, spec in load_specs(config).items():
        try:
            servers.append(McpClient(name, spec, cwd=config.workspace))
        except MCPError as e:
            log.warning("%s", e)
    return servers


def mcp_tools(config: AgentConfig) -> tuple[list[Any], list[str]]:
    """
    Возвращает (инструменты агента, пояснения для промпта).
    Инструменты именуются mcp__<сервер>__<инструмент>, чтобы не конфликтовать.
    """
    from ..tools import Tool, ToolContext, ToolResult

    agents_tools: list[Tool] = []
    notes: list[str] = []

    for server in load_servers(config):
        try:
            tools = server.list_tools()
        except MCPError as e:
            notes.append(f"- {server.name}: не подключился ({e.message})")
            continue
        if not tools:
            notes.append(f"- {server.name}: инструментов не найдено")
            continue

        names = []
        for mcp_tool in tools:
            names.append(mcp_tool.name)
            agents_tools.append(_wrap_tool(server, mcp_tool, Tool, ToolResult, ToolContext))
        notes.append(f"- {server.name} ({server.transport.kind}): {', '.join(names)}")

    return agents_tools, notes


def _wrap_tool(server: McpClient, mcp_tool: McpTool, Tool, ToolResult, ToolContext) -> Any:
    """Оборачивает MCP-инструмент в обычный инструмент агента."""

    def handler(arguments: dict[str, Any], ctx: ToolContext) -> ToolResult:
        ok, content = server.call_tool(mcp_tool.name, arguments)
        return ToolResult(ok, content, display=f"{mcp_tool.full_name}",
                          meta={"server": server.name})

    schema = mcp_tool.input_schema or {"type": "object", "properties": {}}
    parameters = schema.get("properties") or {}
    required = tuple(schema.get("required") or ())

    return Tool(
        name=mcp_tool.full_name,
        description=(mcp_tool.description or f"Инструмент MCP-сервера {server.name}") +
                    f" (внешний сервер «{server.name}»)",
        parameters=parameters,
        handler=handler,
        required=required,
        dangerous=True,          # внешние серверы непредсказуемы: подтверждаем
        category="mcp",
    )


def describe_servers(config: AgentConfig) -> str:
    servers = load_servers(config)
    if not servers:
        return "MCP-серверы не настроены."
    lines = []
    for server in servers:
        try:
            tools = server.list_tools()
            listing = ", ".join(tool.name for tool in tools) or "—"
            info = server.server_info.get("name") or server.transport.kind
            lines.append(f"{server.name} [{info}]\n  источник: {server.source}\n  инструменты: {listing}")
        except MCPError as e:
            lines.append(f"{server.name} — ошибка: {e.message}")
    return "\n\n".join(lines)


def close_all(servers: list[McpClient]) -> None:
    for server in servers:
        server.close()


def mcp_config_template() -> str:
    return json.dumps({
        "mcpServers": {
            "filesystem": {
                "command": "npx",
                "args": ["-y", "@modelcontextprotocol/server-filesystem", "."],
            },
            "git": {"command": "uvx", "args": ["mcp-server-git", "--repository", "."]},
            "мой-http": {"url": "http://127.0.0.1:9000/mcp", "timeout": 60},
        }
    }, ensure_ascii=False, indent=2)


__all__ = ["CONFIG_NAMES", "close_all", "describe_servers", "load_servers", "load_specs",
           "mcp_config_template", "mcp_tools"]
