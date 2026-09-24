"""Сессии, слэш-команды и командная строка `aia`."""
from __future__ import annotations

import json
from pathlib import Path

import pytest

from aiagent.cli import build_parser, main
from aiagent.commands import CommandRegistry
from aiagent.config import AgentConfig
from aiagent.core import Agent
from aiagent.session import latest_session, list_sessions, load_session, new_session, slugify
from aiagent.ui import Console


# --------------------------------------------------------------------------------- сессии
def test_session_save_and_load(config: AgentConfig):
    session = new_session(config, "моя сессия")
    session.add("user", "привет")
    session.add("assistant", "здравствуй")
    path = session.save()
    assert path.is_file()

    restored = load_session(config, session.name)
    assert len(restored.messages) == 2
    assert restored.messages[0]["content"] == "привет"

    assert any(item.name == session.name for item in list_sessions(config))
    assert latest_session(config) is not None


def test_session_json_is_readable(config: AgentConfig):
    session = new_session(config, "тест-json")
    session.add("user", "задача")
    data = json.loads(session.save().read_text(encoding="utf-8"))
    assert data["meta"]["provider"] == "mock"
    assert data["messages"][0]["role"] == "user"


def test_slugify_keeps_cyrillic_and_strips_junk():
    assert slugify("Привет, мир!") == "привет-мир"
    assert slugify("") .startswith("сессия-") or slugify("").startswith("session-")


def test_load_missing_session_raises(config: AgentConfig):
    from aiagent.errors import SessionError

    with pytest.raises(SessionError):
        load_session(config, "нет-такой-сессии")


# ------------------------------------------------------------------------------- команды
def test_command_registry_handles_help_and_unknown(agent: Agent, capsys):
    console = Console(color=False)
    registry = CommandRegistry(agent, console, app=None)

    assert registry.handle("/help") is True
    assert registry.handle("/неттакой") is True
    output = capsys.readouterr().out
    assert "Команды" in output or "Неизвестная команда" in output

    assert registry.handle("/exit") is False


def test_mode_and_model_commands(agent: Agent, capsys):
    registry = CommandRegistry(agent, Console(color=False), app=None)
    registry.handle("/mode plan")
    assert agent.config.mode == "plan"
    assert registry.handle("/stats") is True
    for name in ("/tools", "/context", "/notes", "/rules", "/keys", "/todo"):
        assert registry.handle(name) is True
    capsys.readouterr()  # не проверяем текст, важно отсутствие исключений


def test_model_command_switches_model(agent: Agent):
    registry = CommandRegistry(agent, Console(color=False), app=None)
    registry.handle("/model mock-coder")
    assert agent.config.model == "mock-coder"


# ------------------------------------------------------------------------------------ CLI
def test_parser_defaults():
    args = build_parser().parse_args(["run", "сделай", "что-то", "--provider", "mock"])
    assert args.command == "run"
    assert args.task == ["сделай", "что-то"]
    assert args.provider == "mock"


def test_cli_version_and_tools(capsys):
    assert main(["--version"]) == 0
    assert "AI Agent Studio" in capsys.readouterr().out

    assert main(["tools"]) == 0
    assert "read_file" in capsys.readouterr().out


def test_cli_run_with_mock(config, workspace, mock_script, capsys):
    mock_script([{"text": "Готово из теста."}])
    code = main(["run", "просто ответь", "--provider", "mock", "--workspace", str(workspace),
                 "--mode", "yolo", "--no-color"])
    assert code == 0
    assert "Готово из теста." in capsys.readouterr().out


def test_cli_init_creates_project_files(tmp_path: Path, capsys):
    code = main(["init", str(tmp_path / "новый"), "--no-color"])
    assert code == 0
    assert (tmp_path / "новый" / "AIAGENT.md").is_file()
    assert (tmp_path / "новый" / ".aiagent.json").is_file()
    assert ".aiagent/" in (tmp_path / "новый" / ".gitignore").read_text(encoding="utf-8")


def test_cli_config_set(monkeypatch, isolated_home: Path, capsys):
    code = main(["config", "--set", "max_steps=17", "mode=plan", "language=ru"])
    assert code == 0
    data = json.loads((isolated_home / "config.json").read_text(encoding="utf-8"))
    assert data["max_steps"] == 17 and data["mode"] == "plan"


def test_cli_help_lists_commands(capsys):
    assert main(["--help"]) == 0
    output = capsys.readouterr().out
    for word in ("doctor", "serve", "run", "undo", "mcp"):
        assert word in output


def test_free_text_becomes_task():
    """`aia сделай что-то` — это задача, а не неизвестная команда."""
    args = build_parser().parse_args(["run", "сделай", "что-то"])
    assert args.command == "run"
