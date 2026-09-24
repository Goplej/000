"""Веб-сервер: HTTP API, статика и стриминг событий (SSE)."""
from __future__ import annotations

import json
import threading
import time
import urllib.error
import urllib.request
from http.client import HTTPConnection

import pytest

from aiagent.config import AgentConfig
from aiagent.server.http import ServerState, create_server


@pytest.fixture
def server(config: AgentConfig, mock_script):
    mock_script([{"text": "Ответ из веб-теста."}])
    httpd = create_server(config, "127.0.0.1", 0)
    thread = threading.Thread(target=httpd.serve_forever, kwargs={"poll_interval": 0.1}, daemon=True)
    thread.start()
    time.sleep(0.2)
    yield httpd
    httpd.shutdown()
    httpd.server_close()


def get(server, path: str, timeout: float = 10) -> tuple[int, dict]:
    host, port = server.server_address[:2]
    request = urllib.request.Request(f"http://{host}:{port}{path}")
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            body = response.read().decode("utf-8")
            return response.status, json.loads(body) if body.startswith("{") else {"raw": body}
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read().decode("utf-8") or "{}")


def post(server, path: str, payload: dict, timeout: float = 30) -> tuple[int, str]:
    host, port = server.server_address[:2]
    request = urllib.request.Request(
        f"http://{host}:{port}{path}", data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"}, method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return response.status, response.read().decode("utf-8")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8")


def test_health_and_config(server):
    status, health = get(server, "/api/health")
    assert status == 200
    assert health["ok"] is True
    assert health["provider"] == "mock"
    assert health["tools"] >= 30

    status, config = get(server, "/api/config")
    assert status == 200
    assert config["config"]["mode"] in {"ask", "edits", "auto", "plan", "yolo"}
    assert "anthropic" in config["providers"]


def test_tools_and_history(server):
    status, tools = get(server, "/api/tools")
    assert status == 200
    names = {tool["name"] for tool in tools["tools"]}
    assert {"read_file", "write_file", "run_shell"} <= names

    status, history = get(server, "/api/history")
    assert status == 200 and "history" in history


def test_index_page_is_served(server):
    host, port = server.server_address[:2]
    with urllib.request.urlopen(f"http://{host}:{port}/", timeout=10) as response:
        html = response.read().decode("utf-8")
    assert response.status == 200
    assert "AI Agent Studio" in html
    assert "/app.js" in html


def test_chat_streams_events(server):
    host, port = server.server_address[:2]
    connection = HTTPConnection(host, port, timeout=30)
    connection.request("POST", "/api/chat", json.dumps({"message": "привет"}),
                       {"Content-Type": "application/json"})
    response = connection.getresponse()
    assert response.status == 200
    assert response.getheader("Content-Type", "").startswith("text/event-stream")

    events = []
    buffer = ""
    deadline = time.time() + 25
    while time.time() < deadline:
        chunk = response.read1(4096).decode("utf-8") if hasattr(response, "read1") else response.read(4096).decode("utf-8")
        if not chunk:
            break
        buffer += chunk
        while "\n\n" in buffer:
            raw, buffer = buffer.split("\n\n", 1)
            raw = raw.strip()
            if raw.startswith("data:"):
                events.append(json.loads(raw[5:].strip()))
        if any(event.get("type") == "end" for event in events):
            break
    connection.close()

    kinds = [event["type"] for event in events]
    assert "start" in kinds and "done" in kinds
    text = "".join(event.get("text", "") for event in events if event["type"] == "text")
    assert "Ответ из веб-теста." in text


def test_stop_and_undo_endpoints(server):
    status, payload = post(server, "/api/stop", {})
    assert status == 200 and payload

    status, payload = post(server, "/api/undo", {"count": 1})
    assert status == 200
    assert "result" in json.loads(payload)


def test_confirm_endpoint_without_pending(server):
    status, _ = post(server, "/api/confirm", {"id": "нет", "allow": True})
    assert status == 200


def test_busy_agent_rejects_second_task(server):
    state: ServerState = server.state
    state.busy = True
    try:
        status, body = post(server, "/api/chat", {"message": "ещё задача"})
        assert status == 409
        assert "занят" in body
    finally:
        state.busy = False
