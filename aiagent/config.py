"""Конфигурация агента: модели, провайдеры, разрешения, поведение.

Приоритет (позже перекрывает раньше):
    1. значения по умолчанию
    2. ~/.aiagent/config.json            (глобальные настройки пользователя)
    3. <проект>/.aiagent.json            (настройки проекта, можно коммитить)
    4. переменные окружения              (AIA_MODEL, ANTHROPIC_API_KEY, ...)
    5. аргументы командной строки
"""
from __future__ import annotations

import json
import os
from dataclasses import asdict, dataclass, field, fields
from pathlib import Path
from typing import Any

from .errors import ConfigError
from .paths import config_path, project_paths

# --------------------------------------------------------------------------------------
# Модели по умолчанию для каждого провайдера
# --------------------------------------------------------------------------------------
DEFAULT_MODELS = {
    "anthropic": "claude-sonnet-4-5-20250929",
    "openai": "gpt-4o",
    "openrouter": "anthropic/claude-sonnet-4.5",
    "google": "gemini-2.5-flash",
    "groq": "llama-3.3-70b-versatile",
    "mistral": "mistral-large-latest",
    "deepseek": "deepseek-chat",
    "ollama": "qwen2.5-coder:7b",
    "mock": "mock-1",
}

#: Переменные окружения, в которых ищем ключ для каждого провайдера.
KEY_ENV = {
    "anthropic": ("ANTHROPIC_API_KEY", "AIA_ANTHROPIC_KEY"),
    "openai": ("OPENAI_API_KEY", "AIA_OPENAI_KEY"),
    "openrouter": ("OPENROUTER_API_KEY", "AIA_OPENROUTER_KEY"),
    "google": ("GOOGLE_API_KEY", "GEMINI_API_KEY", "AIA_GOOGLE_KEY"),
    "groq": ("GROQ_API_KEY", "AIA_GROQ_KEY"),
    "mistral": ("MISTRAL_API_KEY", "AIA_MISTRAL_KEY"),
    "deepseek": ("DEEPSEEK_API_KEY", "AIA_DEEPSEEK_KEY"),
    "ollama": (),
    "mock": (),
}

#: Режимы разрешений (чем дальше, тем автономнее).
PERMISSION_MODES = ("ask", "edits", "auto", "plan", "yolo")

MODE_DESCRIPTIONS = {
    "ask": "Спрашивать разрешение на каждое действие (самый безопасный).",
    "edits": "Разрешить правку файлов автоматически, команды — с подтверждением.",
    "auto": "Разрешить всё в пределах рабочей папки, опасное — с подтверждением.",
    "plan": "Только чтение и планирование: файлы не меняются, команды не запускаются.",
    "yolo": "Без вопросов вообще. Только в изолированной среде!",
}

#: Инструменты, которые считаются «изменяющими» (для режимов ask/edits/plan).
WRITE_TOOLS = {"write_file", "edit_file", "multi_edit", "delete_file", "apply_patch", "notebook_edit"}
DANGEROUS_TOOLS = {"run_shell", "run_code", "task", "kill_shell"}


@dataclass
class Permissions:
    """Правила доступа: списки разрешённого/запрещённого/спрашиваемого."""

    allow: list[str] = field(default_factory=list)   # напр. "run_shell(git status:*)"
    deny: list[str] = field(default_factory=list)
    ask: list[str] = field(default_factory=list)
    extra_dirs: list[str] = field(default_factory=list)   # папки вне проекта, куда можно писать
    allow_web: bool = True

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)

    @classmethod
    def from_dict(cls, data: dict[str, Any] | None) -> Permissions:
        data = data or {}
        allowed = {f.name for f in fields(cls)}
        return cls(**{k: v for k, v in data.items() if k in allowed})


@dataclass
class AgentConfig:
    # --- модель ---
    provider: str = ""                        # пусто => определить по ключам/профилю
    model: str = ""
    temperature: float = 0.0
    max_tokens: int = 8192
    thinking_budget: int = 0                  # расширенное мышление (Anthropic), токенов
    reasoning_effort: str = ""                # OpenAI: low|medium|high
    base_url: str = ""                        # свой адрес API (прокси, локальный сервер)

    # --- поведение агента ---
    mode: str = "auto"                        # см. PERMISSION_MODES
    max_steps: int = 60                       # максимум итераций «мысль→инструмент»
    max_parallel_tools: int = 4               # одновременных инструментов (в одном шаге)
    auto_compact: bool = True                 # автосжатие контекста
    compact_at_ratio: float = 0.75            # доля окна, при которой сжимаем
    keep_recent_messages: int = 8
    tool_result_max_chars: int = 8000
    stream: bool = True
    language: str = "ru"                      # язык ответов агента
    verbose: bool = False
    color: bool = True
    timeout: int = 120                        # таймаут одного запроса к модели, сек
    max_retries: int = 3

    # --- инструменты ---
    enabled_tools: list[str] = field(default_factory=list)    # пусто = все
    disabled_tools: list[str] = field(default_factory=list)
    shell_timeout: int = 180
    allow_shell: bool = True
    allow_web: bool = True
    allow_subagents: bool = True
    mcp_enabled: bool = True
    extra_dirs: list[str] = field(default_factory=list)

    # --- контекст модели (грубо, для оценки заполнения окна) ---
    context_window: int = 200_000

    # --- интерфейс/сервер ---
    web_host: str = "127.0.0.1"
    web_port: int = 8787

    # --- служебное ---
    permissions: Permissions = field(default_factory=Permissions)
    hooks: dict[str, list[str]] = field(default_factory=dict)   # напр. {"pre_tool": ["echo ..."]}
    system_prompt_extra: str = ""
    workspace: str = "."

    # ------------------------------------------------------------------ работа с файлами
    @classmethod
    def load(cls, workspace: str | Path | None = None,
             overrides: dict[str, Any] | None = None) -> AgentConfig:
        paths = project_paths(workspace)
        cfg = cls()
        for path in (config_path(), paths.workspace_config):
            cfg = _merge(cfg, _read_json(path))
        cfg.workspace = str(paths.workspace)
        cfg = _merge(cfg, _from_env())
        cfg = _merge(cfg, overrides or {})
        cfg.validate()
        return cfg

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> AgentConfig:
        cfg = _merge(cls(), data)
        cfg.validate()
        return cfg

    def validate(self) -> None:
        if self.mode not in PERMISSION_MODES:
            raise ConfigError(
                f"Неизвестный режим разрешений: {self.mode}",
                hint=f"Доступны: {', '.join(PERMISSION_MODES)}",
            )
        if self.max_steps < 1:
            raise ConfigError("max_steps должен быть ≥ 1")
        if not (0.1 <= self.compact_at_ratio <= 0.99):
            raise ConfigError("compact_at_ratio должен быть в диапазоне 0.1–0.99")
        if self.provider and self.provider not in DEFAULT_MODELS:
            raise ConfigError(
                f"Неизвестный провайдер: {self.provider}",
                hint="Доступны: " + ", ".join(DEFAULT_MODELS),
            )

    def resolved_provider(self) -> str:
        """Какой провайдер реально будет использован (с учётом наличия ключей)."""
        if self.provider:
            return self.provider
        for name in ("anthropic", "openai", "openrouter", "google", "groq", "deepseek", "mistral"):
            if any(os.environ.get(env) for env in KEY_ENV[name]):
                return name
        return "ollama"

    def resolved_model(self) -> str:
        return self.model or DEFAULT_MODELS.get(self.resolved_provider(), "")

    def save(self, path: str | Path | None = None) -> Path:
        target = Path(path) if path else config_path()
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(json.dumps(self.to_dict(), ensure_ascii=False, indent=2), encoding="utf-8")
        return target

    def to_dict(self) -> dict[str, Any]:
        data = asdict(self)
        data["permissions"] = self.permissions.to_dict()
        return data

    # ------------------------------------------------------------------------ утилиты
    def api_key(self, provider: str | None = None) -> str:
        name = provider or self.resolved_provider()
        for env in KEY_ENV.get(name, ()):
            value = os.environ.get(env)
            if value:
                return value.strip()
        # запасной вариант — файл ключей с правами 600
        from .paths import keys_path
        data = _read_json(keys_path())
        if isinstance(data, dict):
            return str(data.get(name, "") or "").strip()
        return ""

    def has_credentials(self, provider: str | None = None) -> bool:
        name = provider or self.resolved_provider()
        if name in {"ollama", "mock"}:
            return True
        return bool(self.api_key(name))

    def timeouts(self) -> tuple[int, int]:
        """(таймаут соединения, таймаут чтения) с учётом длинных ответов модели."""
        return (15, max(30, self.timeout))

    def describe(self, provider: str = "", model: str = "") -> str:
        return (f"{model or self.resolved_model()} · {provider or self.resolved_provider()} "
                f"· режим {self.mode}")


def _from_env() -> dict[str, Any]:
    env_map: dict[str, tuple[str, type]] = {
        "AIA_PROVIDER": ("provider", str),
        "AIA_MODEL": ("model", str),
        "AIA_MODE": ("mode", str),
        "AIA_MAX_STEPS": ("max_steps", int),
        "AIA_TEMPERATURE": ("temperature", float),
        "AIA_MAX_TOKENS": ("max_tokens", int),
        "AIA_BASE_URL": ("base_url", str),
        "AIA_LANGUAGE": ("language", str),
        "AIA_WEB_PORT": ("web_port", int),
        "AIA_WEB_HOST": ("web_host", str),
        "AIA_CONTEXT_WINDOW": ("context_window", int),
        "AIA_SHELL_TIMEOUT": ("shell_timeout", int),
        "AIA_VERBOSE": ("verbose", bool),
        "AIA_NO_WEB": ("allow_web", bool),
        "AIA_NO_SHELL": ("allow_shell", bool),
    }
    out: dict[str, Any] = {}
    for env, (attr, kind) in env_map.items():
        raw = os.environ.get(env)
        if raw is None or raw == "":
            continue
        try:
            if kind is bool:
                out[attr] = raw.strip().lower() in {"1", "true", "yes", "on", "да"}
            elif kind is int:
                out[attr] = int(raw)
            elif kind is float:
                out[attr] = float(raw)
            else:
                out[attr] = raw
        except ValueError:
            continue
    if "AIA_NO_WEB" in os.environ:
        out["allow_web"] = not out.pop("allow_web", False)
    if "AIA_NO_SHELL" in os.environ:
        out["allow_shell"] = not out.pop("allow_shell", False)
    return out


def _merge(cfg: AgentConfig, data: dict[str, Any] | None) -> AgentConfig:
    if not data:
        return cfg
    allowed = {f.name for f in fields(AgentConfig)}
    for key, value in data.items():
        if key == "permissions":
            cfg.permissions = Permissions.from_dict(
                value if isinstance(value, dict) else None
            ) if not isinstance(value, Permissions) else value
            continue
        if key in allowed and value is not None:
            setattr(cfg, key, value)
    if isinstance(cfg.permissions, dict):  # на случай данных из JSON
        cfg.permissions = Permissions.from_dict(cfg.permissions)
    return cfg


def _read_json(path: Path) -> dict[str, Any] | None:
    try:
        if Path(path).is_file():
            data = json.loads(Path(path).read_text(encoding="utf-8"))
            return data if isinstance(data, dict) else None
    except (OSError, json.JSONDecodeError):
        return None
    return None


__all__ = [
    "AgentConfig", "DANGEROUS_TOOLS", "DEFAULT_MODELS", "KEY_ENV", "MODE_DESCRIPTIONS",
    "PERMISSION_MODES", "Permissions", "WRITE_TOOLS",
]
