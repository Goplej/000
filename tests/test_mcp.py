"""MCP: транспорты, клиент и подключение внешних инструментов к агенту."""
from __future__ import annotations

import json
from pathlib import Path

import pytest

from aiagent.config import AgentConfig
from aiagent.core import Agent
from aiagent.errors import MCPError
from aiagent.mcp.client import McpClient
from aiagent.mcp.registry import load_specs, mcp_tools
from aiagent.mcp.transport import StdioTransport, build_transport
from aiagent.ui import SilentUI

FAKE_SERVER = '''\
"""Мини-MCP-сервер для тестов: отвечает по JSON-RPC на stdin/stdout."""
import json
import sys

TOOLS = [{
    "name": "echo",
    "description": "Повторяет переданный текст",
    "inputSchema": {
        "type": "object",
        "properties": {"text": {"type": "string", "description": "что повторить"}},
        "required": ["text"],
    },
}]


def main() -> None:
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        message = json.loads(line)
        method = message.get("method")
        if message.get("id") is None:
            continue                      # уведомление — отвечать не нужно
        if method == "initialize":
            result = {"protocolVersion": "2025-06-18",
                      "serverInfo": {"name": "fake-server", "version": "0.1"},
                      "capabilities": {"tools": {}}}
        elif method == "tools/list":
            result = {"tools": TOOLS}
        elif method == "tools/call":
            arguments = (message.get("params") or {}).get("arguments") or {}
            result = {"content": [{"type": "text",
                                   "text": "эхо: " + str(arguments.get("text", ""))}]}
        else:
            print(json.dumps({"jsonrpc": "2.0", "id": message["id"],
                              "error": {"code": -32601, "message": "нет метода"}}), flush=True)
            continue
        print(json.dumps({"jsonrpc": "2.0", "id": message["id"], "result": result}), flush=True)


if __name__ == "__main__":
    main()
'''


@pytest.fixture
def fake_server(tmp_path: Path) -> Path:
    path = tmp_path / "fake_mcp.py"
    path.write_text(FAKE_SERVER, encoding="utf-8")
    return path


@pytest.fixture
def mcp_workspace(workspace: Path, fake_server: Path) -> Path:
    import sys

    (workspace / ".mcp.json").write_text(json.dumps({
        "mcpServers": {"fake": {"command": sys.executable, "args": [str(fake_server)]}}
    }, ensure_ascii=False), encoding="utf-8")
    return workspace


def test_build_transport_types(tmp_path: Path):
    stdio = build_transport("s", {"command": "python3", "args": ["-c", "pass"]}, tmp_path)
    assert isinstance(stdio, StdioTransport)

    http = build_transport("h", {"url": "http://127.0.0.1:9/mcp"})
    assert http.kind == "http"

    with pytest.raises(MCPError):
        build_transport("broken", {})


def test_stdio_client_lists_and_calls(mcp_workspace: Path, fake_server: Path):
    import sys

    client = McpClient("fake", {"command": sys.executable, "args": [str(fake_server)]},
                       cwd=mcp_workspace)
    try:
        client.initialize()
        assert client.server_info.get("name") == "fake-server"

        tools = client.list_tools()
        assert [tool.name for tool in tools] == ["echo"]
        assert tools[0].full_name == "mcp__fake__echo"

        ok, text = client.call_tool("echo", {"text": "привет"})
        assert ok and text == "эхо: привет"
    finally:
        client.close()


def test_missing_command_reports_clearly(tmp_path: Path):
    client = McpClient("нет-такого", {"command": "совсем-нет-такой-команды-12345"}, cwd=tmp_path)
    with pytest.raises(MCPError) as error:
        client.list_tools()
    assert "не найдена" in error.value.message or "не запустился" in error.value.message


def test_load_specs_from_project_file(mcp_workspace: Path):
    config = AgentConfig.from_dict({"provider": "mock", "workspace": str(mcp_workspace)})
    specs = load_specs(config)
    assert "fake" in specs
    assert specs["fake"]["_source"].endswith(".mcp.json")


def test_mcp_tools_wrap_into_agent_tools(mcp_workspace: Path):
    config = AgentConfig.from_dict({"provider": "mock", "workspace": str(mcp_workspace)})
    tools, notes = mcp_tools(config)
    names = [tool.name for tool in tools]
    assert "mcp__fake__echo" in names
    assert any("fake" in note for note in notes)

    echo = tools[names.index("mcp__fake__echo")]
    assert echo.dangerous is True
    assert echo.category == "mcp"
    assert "text" in echo.parameters


def test_agent_uses_mcp_tools(mcp_workspace: Path, mock_script, tool_call_script):
    config = AgentConfig.from_dict({"provider": "mock", "workspace": str(mcp_workspace),
                                    "mode": "yolo"})
    mock_script([
        tool_call_script("mcp__fake__echo", {"text": "проверка"}),
        {"text": "Готово."},
    ])
    agent = Agent(config, ui=SilentUI())
    assert "mcp__fake__echo" in agent.tool_map

    result = agent.chat("повтори текст")
    assert result.stop_reason in {"end_turn", "finish"}
    assert result.tool_calls >= 1
    result_text = "".join(str(event) for event in [result.answer])
    assert "эхо: проверка" in result_text or "Готово" in result_text


def test_mcp_disabled_by_config(mcp_workspace: Path):
    config = AgentConfig.from_dict({"provider": "mock", "workspace": str(mcp_workspace),
                                    "mode": "yolo", "mcp_enabled": False})
    agent = Agent(config, ui=SilentUI())
    assert not any(name.startswith("mcp__") for name in agent.tool_map)


def test_description_of_servers(mcp_workspace: Path):
    from aiagent.mcp.registry import describe_servers

    config = AgentConfig.from_dict({"provider": "mock", "workspace": str(mcp_workspace)})
    text = describe_servers(config)
    assert "fake" in text and "echo" in text
