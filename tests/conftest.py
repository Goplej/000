"""Общие фикстуры тестов: изолированный рабочий каталог и демо-провайдер."""
from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from aiagent.config import AgentConfig  # noqa: E402
from aiagent.core import Agent  # noqa: E402
from aiagent.ui import SilentUI  # noqa: E402


@pytest.fixture(autouse=True)
def isolated_home(tmp_path_factory, monkeypatch):
    """Состояние агента (~/.aiagent) — во временном каталоге, чтобы не трогать машину."""
    home = tmp_path_factory.mktemp("aia-home")
    monkeypatch.setenv("AIA_HOME", str(home))
    monkeypatch.delenv("ANTHROPIC_API_KEY", raising=False)
    monkeypatch.delenv("OPENAI_API_KEY", raising=False)
    monkeypatch.delenv("AIA_MOCK_SCRIPT", raising=False)
    yield home


@pytest.fixture
def workspace(tmp_path: Path) -> Path:
    """Пустой проект с парой файлов."""
    (tmp_path / "README.md").write_text("# Тестовый проект\n", encoding="utf-8")
    src = tmp_path / "app.py"
    src.write_text("def add(a, b):\n    return a + b\n", encoding="utf-8")
    return tmp_path


@pytest.fixture
def config(workspace: Path) -> AgentConfig:
    return AgentConfig.from_dict({
        "provider": "mock",
        "workspace": str(workspace),
        "mode": "yolo",          # в тестах подтверждения не нужны
        "max_steps": 8,
        "stream": True,
    })


@pytest.fixture
def agent(config: AgentConfig) -> Agent:
    return Agent(config, ui=SilentUI())


@pytest.fixture
def mock_script(tmp_path: Path, monkeypatch):
    """Позволяет задать сценарий ответов демо-провайдера списком словарей."""
    def _write(script: list[dict]) -> Path:
        path = tmp_path / "mock-script.json"
        path.write_text(json.dumps(script, ensure_ascii=False), encoding="utf-8")
        monkeypatch.setenv("AIA_MOCK_SCRIPT", str(path))
        return path

    return _write


@pytest.fixture
def tool_call_script():
    """Готовые сценарии вызовов инструментов в текстовом протоколе."""
    def _call(name: str, arguments: dict, text: str = "") -> dict:
        body = json.dumps({"name": name, "arguments": arguments}, ensure_ascii=False)
        return {"text": f"{text}<tool_call>{body}</tool_call>"}

    return _call
