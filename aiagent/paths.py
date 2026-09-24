"""Пути и окружение: где хранить настройки, сессии, историю, заметки.

Всё состояние — в одном каталоге (по умолчанию ~/.aiagent), чтобы агент
не разбрасывал файлы по диску и его легко было удалить целиком.
"""
from __future__ import annotations

import hashlib
import os
import platform
import shutil
from dataclasses import dataclass
from pathlib import Path

APP_DIR_NAME = "aiagent"
STATE_ENV = "AIA_HOME"
WORKSPACE_ENV = "AIA_WORKSPACE"


def state_dir() -> Path:
    """Корень состояния агента: ~/.aiagent (или $AIA_HOME)."""
    override = os.environ.get(STATE_ENV)
    if override:
        return Path(override).expanduser().resolve()
    return Path.home() / f".{APP_DIR_NAME}"


def config_path() -> Path:
    return state_dir() / "config.json"


def mcp_path() -> Path:
    return state_dir() / "mcp.json"


def keys_path() -> Path:
    """Файл с ключами (права 600). Можно не использовать — переменные окружения надёжнее."""
    return state_dir() / "keys.json"


def web_dir() -> Path:
    return Path(__file__).resolve().parent / "web"


def ensure_state_dir() -> Path:
    path = state_dir()
    path.mkdir(parents=True, exist_ok=True)
    try:
        path.chmod(0o700)
    except OSError:
        pass
    return path


@dataclass(frozen=True)
class ProjectPaths:
    workspace: Path
    state: Path

    @property
    def project_key(self) -> str:
        """Короткий стабильный идентификатор проекта (путь + имя)."""
        digest = hashlib.sha1(str(self.workspace).encode("utf-8")).hexdigest()[:10]
        name = self.workspace.name or "root"
        safe = "".join(ch for ch in name if ch.isalnum() or ch in "-_")[:24] or "project"
        return f"{safe}-{digest}"

    @property
    def project_dir(self) -> Path:
        path = self.state / "projects" / self.project_key
        path.mkdir(parents=True, exist_ok=True)
        return path

    @property
    def sessions_dir(self) -> Path:
        path = self.project_dir / "sessions"
        path.mkdir(parents=True, exist_ok=True)
        return path

    @property
    def checkpoints_dir(self) -> Path:
        path = self.project_dir / "checkpoints"
        path.mkdir(parents=True, exist_ok=True)
        return path

    @property
    def logs_dir(self) -> Path:
        path = self.state / "logs"
        path.mkdir(parents=True, exist_ok=True)
        return path

    @property
    def memory_file(self) -> Path:
        return self.project_dir / "memory.md"

    @property
    def todo_file(self) -> Path:
        return self.project_dir / "todo.json"

    @property
    def workspace_config(self) -> Path:
        return self.workspace / ".aiagent.json"

    @property
    def rules_files(self) -> list[Path]:
        """Файлы правил проекта — как CLAUDE.md в Claude Code."""
        return [
            self.workspace / "AIAGENT.md",
            self.workspace / ".aiagent" / "AIAGENT.md",
            self.workspace / "AGENT.md",
            self.project_dir / "AIAGENT.md",
        ]


def project_paths(workspace: str | Path | None = None) -> ProjectPaths:
    ws = Path(workspace or os.environ.get(WORKSPACE_ENV) or Path.cwd()).expanduser().resolve()
    return ProjectPaths(workspace=ws, state=ensure_state_dir())


def shell_name() -> str:
    if platform.system() == "Windows":
        return "powershell" if shutil.which("pwsh") else "cmd"
    return os.environ.get("SHELL", "/bin/bash").rsplit("/", 1)[-1]


def os_label() -> str:
    system = platform.system()
    if system == "Darwin":
        return f"macOS {platform.mac_ver()[0]}"
    if system == "Windows":
        return f"Windows {platform.release()}"
    return f"{system} {platform.release()}"


__all__ = [
    "APP_DIR_NAME", "ProjectPaths", "STATE_ENV", "config_path", "ensure_state_dir",
    "keys_path", "mcp_path", "os_label", "project_paths", "shell_name",
    "state_dir", "web_dir", "WORKSPACE_ENV",
]
