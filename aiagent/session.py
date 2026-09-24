"""Сессии: сохранение истории диалога, чтобы продолжать работу позже."""
from __future__ import annotations

import json
import re
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from .config import AgentConfig
from .errors import SessionError
from .paths import project_paths


@dataclass
class Session:
    name: str
    path: Path
    created: float = field(default_factory=time.time)
    updated: float = field(default_factory=time.time)
    meta: dict[str, Any] = field(default_factory=dict)
    messages: list[dict[str, Any]] = field(default_factory=list)

    # ------------------------------------------------------------------ сохранение
    def save(self) -> Path:
        self.updated = time.time()
        payload = {
            "name": self.name,
            "created": self.created,
            "updated": self.updated,
            "meta": self.meta,
            "messages": self.messages,
        }
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            self.path.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
        except OSError as e:
            raise SessionError(f"не удалось сохранить сессию: {e}") from e
        return self.path

    def add(self, role: str, content: str) -> None:
        self.messages.append({"role": role, "content": content, "at": time.time()})

    # ------------------------------------------------------------------- загрузка
    @classmethod
    def load(cls, path: Path) -> Session:
        try:
            data = json.loads(Path(path).read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as e:
            raise SessionError(f"не удалось прочитать сессию {path}: {e}") from e
        return cls(
            name=data.get("name", Path(path).stem),
            path=Path(path),
            created=float(data.get("created", time.time())),
            updated=float(data.get("updated", time.time())),
            meta=data.get("meta") or {},
            messages=data.get("messages") or [],
        )

    def summary(self) -> str:
        first = next((m["content"] for m in self.messages if m.get("role") == "user"), "")
        when = time.strftime("%Y-%m-%d %H:%M", time.localtime(self.updated))
        return f"{self.name} — {when}, сообщений {len(self.messages)}: {_short(first)}"


def slugify(text: str) -> str:
    clean = re.sub(r"[^\w\-\u0400-\u04FF ]+", "", text or "").strip().lower()
    clean = re.sub(r"\s+", "-", clean)[:40]
    return clean or time.strftime("session-%Y%m%d-%H%M%S")


def new_session(config: AgentConfig, name: str | None = None) -> Session:
    directory = project_paths(config.workspace).sessions_dir
    session_name = slugify(name or time.strftime("сессия-%Y%m%d-%H%M%S"))
    return Session(name=session_name, path=directory / f"{session_name}.json",
                   meta={"workspace": str(config.workspace), "model": config.resolved_model(),
                         "provider": config.resolved_provider()})


def list_sessions(config: AgentConfig) -> list[Session]:
    directory = project_paths(config.workspace).sessions_dir
    sessions: list[Session] = []
    for path in sorted(directory.glob("*.json"), key=lambda p: p.stat().st_mtime, reverse=True):
        try:
            sessions.append(Session.load(path))
        except SessionError:
            continue
    return sessions


def load_session(config: AgentConfig, name: str) -> Session:
    directory = project_paths(config.workspace).sessions_dir
    candidates = [Path(name), directory / name, directory / f"{name}.json"]
    for candidate in candidates:
        if candidate.is_file():
            return Session.load(candidate)
    raise SessionError(
        f"сессия «{name}» не найдена",
        hint=f"Посмотреть список: aia sessions (каталог {directory})",
    )


def latest_session(config: AgentConfig) -> Session | None:
    sessions = list_sessions(config)
    return sessions[0] if sessions else None


def _short(text: str, limit: int = 60) -> str:
    flat = " ".join(str(text).split())
    return flat if len(flat) <= limit else flat[: limit - 1] + "…"


__all__ = ["Session", "latest_session", "list_sessions", "load_session", "new_session", "slugify"]
