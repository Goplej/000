"""Хуки: пользовательские команды до/после инструментов и по завершении.

Настройка в .aiagent.json:
    {
      "hooks": {
        "pre_tool":  ["python3 scripts/check_policy.py"],
        "post_tool": ["notify-send 'агент поработал'"],
        "stop":      ["git status --short"]
      }
    }

Команда получает JSON-событие в stdin и переменной окружения AIA_HOOK_PAYLOAD.
Код возврата 2 означает «запретить действие» (только для pre_tool), текст stderr
попадёт в сообщение модели.
"""
from __future__ import annotations

import json
import os
import shlex
import subprocess
from dataclasses import dataclass
from pathlib import Path

from ..config import AgentConfig
from ..utils.logging import get_logger

log = get_logger("hooks")
DEFAULT_TIMEOUT = 30


@dataclass
class HookResult:
    blocked: bool = False
    message: str = ""
    outputs: list[str] = None  # type: ignore[assignment]

    def __post_init__(self) -> None:
        if self.outputs is None:
            self.outputs = []


class HookRunner:
    """Выполняет команды хуков, аккуратно и с ограничением времени."""

    def __init__(self, config: AgentConfig) -> None:
        self.config = config
        self.workspace = Path(config.workspace)
        self.hooks: dict[str, list[str]] = {
            key: [cmd for cmd in (value or []) if cmd.strip()]
            for key, value in (config.hooks or {}).items()
        }

    @property
    def enabled(self) -> bool:
        return any(self.hooks.values())

    def run(self, event: str, payload: dict) -> HookResult:
        """Запускает все команды указанного события. Ошибки не ломают агента."""
        commands = self.hooks.get(event) or []
        result = HookResult()
        if not commands:
            return result

        payload = {**payload, "event": event, "workspace": str(self.workspace)}
        blob = json.dumps(payload, ensure_ascii=False)

        for command in commands:
            try:
                proc = subprocess.run(
                    command if os.name == "nt" else shlex.split(command),
                    shell=os.name == "nt",
                    cwd=str(self.workspace),
                    input=blob,
                    capture_output=True,
                    text=True,
                    encoding="utf-8",
                    errors="replace",
                    timeout=int(self.config.hooks.get("timeout", DEFAULT_TIMEOUT))
                    if isinstance(self.config.hooks.get("timeout"), (int, float)) else DEFAULT_TIMEOUT,
                    env={**os.environ, "AIA_HOOK_PAYLOAD": blob[:8000], "AIA_HOOK_EVENT": event},
                )
            except FileNotFoundError:
                log.warning("Хук не найден: %s", command)
                continue
            except subprocess.TimeoutExpired:
                log.warning("Хук превысил лимит времени: %s", command)
                continue
            except OSError as e:
                log.warning("Хук не запустился (%s): %s", command, e)
                continue

            output = (proc.stdout or "").strip()
            error = (proc.stderr or "").strip()
            if output:
                result.outputs.append(output[:2000])
            if proc.returncode == 2 and event == "pre_tool":
                result.blocked = True
                result.message = error or output or f"хук запретил действие: {command}"
                break
            if proc.returncode not in (0, 2):
                log.warning("Хук %s вернул код %s: %s", command, proc.returncode, error[:400])
        return result

    def trigger(self, event: str, payload: dict) -> None:
        """Запуск хука без ожидания реакции (post_tool, stop)."""
        if self.hooks.get(event):
            threading_run(self.run, event, payload)


def threading_run(func, *args) -> None:
    """Запуск в отдельном потоке, чтобы хук не тормозил агента."""
    import threading

    thread = threading.Thread(target=func, args=args, daemon=True)
    thread.start()


__all__ = ["HookResult", "HookRunner"]
