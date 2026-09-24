"""Настройки: приоритеты, режимы, провайдеры и ключи."""
from __future__ import annotations

import json
from pathlib import Path

from aiagent.config import (
    DEFAULT_MODELS,
    KEY_ENV,
    MODE_DESCRIPTIONS,
    PERMISSION_MODES,
    AgentConfig,
    Permissions,
)


def test_defaults_are_sane():
    config = AgentConfig()
    assert config.mode in PERMISSION_MODES
    assert config.max_steps > 0
    assert config.timeout > 0
    assert config.temperature == 0.0
    assert set(MODE_DESCRIPTIONS) == set(PERMISSION_MODES)


def test_every_provider_has_default_model():
    for provider in ("anthropic", "openai", "google", "ollama", "mock"):
        assert DEFAULT_MODELS.get(provider), f"нет модели по умолчанию для {provider}"


def test_overrides_win(workspace: Path):
    config = AgentConfig.from_dict({"workspace": str(workspace), "mode": "plan", "max_steps": 3})
    assert config.mode == "plan"
    assert config.max_steps == 3
    assert Path(config.workspace) == workspace


def test_project_file_overrides_global(workspace: Path, isolated_home: Path):
    (isolated_home / "config.json").write_text(
        json.dumps({"mode": "edits", "language": "en"}), encoding="utf-8")
    (workspace / ".aiagent.json").write_text(
        json.dumps({"mode": "ask"}), encoding="utf-8")

    config = AgentConfig.load(str(workspace))
    assert config.mode == "ask"          # файл проекта важнее глобального
    assert config.language == "en"       # а глобальная настройка сохраняется


def test_env_overrides_files(workspace: Path, monkeypatch, isolated_home: Path):
    (isolated_home / "config.json").write_text(json.dumps({"mode": "ask"}), encoding="utf-8")
    monkeypatch.setenv("AIA_MODE", "yolo")
    monkeypatch.setenv("AIA_MAX_STEPS", "7")
    config = AgentConfig.load(str(workspace))
    assert config.mode == "yolo"
    assert config.max_steps == 7


def test_key_from_env_and_file(workspace: Path, monkeypatch, isolated_home: Path):
    monkeypatch.setenv("OPENAI_API_KEY", "sk-test-123")
    config = AgentConfig.from_dict({"provider": "openai", "workspace": str(workspace)})
    assert config.api_key() == "sk-test-123"

    monkeypatch.delenv("OPENAI_API_KEY")
    (isolated_home / "keys.json").write_text(
        json.dumps({"openai": "sk-from-file"}), encoding="utf-8")
    config = AgentConfig.from_dict({"provider": "openai", "workspace": str(workspace)})
    assert config.api_key() == "sk-from-file"


def test_resolved_provider_prefers_available_key(workspace: Path, monkeypatch):
    monkeypatch.setenv("GOOGLE_API_KEY", "AIza-test")
    config = AgentConfig.from_dict({"workspace": str(workspace), "provider": "anthropic"})
    # явный провайдер уважаем, но при отсутствии ключа и наличии другого — подсказываем
    assert config.resolved_provider() in {"anthropic", "google", "ollama", "mock"}
    assert config.provider in {"anthropic", "google"}


def test_permissions_rules_fnmatch():
    permissions = Permissions(
        deny=["run_shell(rm -rf:*)"],
        allow=["run_shell(git status:*)"],
        ask=["run_shell(git push:*)"],
        extra_dirs=["/tmp"],
    )
    from aiagent.core.permissions import PermissionManager

    config = AgentConfig.from_dict({"mode": "auto", "permissions": permissions.to_dict()})
    manager = PermissionManager(config)

    denied = manager.check("run_shell", {"command": "rm -rf tmp"}, workspace=Path("/tmp"))
    assert not denied.allowed
    assert denied.source == "deny"

    allowed_status = manager.check("run_shell", {"command": "git status"}, workspace=Path("/tmp"))
    assert allowed_status.allowed
    assert allowed_status.source == "allow"

    push = manager.check("run_shell", {"command": "git push origin main"}, workspace=Path("/tmp"))
    assert push.allowed is False or push.needs_prompt


def test_mode_plan_blocks_writes(workspace: Path):
    from aiagent.core.permissions import PermissionManager

    config = AgentConfig.from_dict({"mode": "plan"})
    manager = PermissionManager(config)
    decision = manager.check("write_file", {"path": "x.py", "content": "print(1)"}, workspace=Path("/tmp"))
    assert not decision.allowed


def test_env_key_map_covers_providers():
    for provider in ("anthropic", "openai", "google", "groq", "deepseek", "mistral", "openrouter"):
        assert provider in KEY_ENV, f"нет переменной окружения для {provider}"
    assert "ANTHROPIC_API_KEY" in KEY_ENV["anthropic"]
