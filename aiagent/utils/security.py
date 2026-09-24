"""Безопасность: блок-лист команд, защита секретов, песочница путей."""
from __future__ import annotations

import os
import re
from pathlib import Path

#: Категорически запрещённые команды (агент их не выполнит никогда, даже в yolo-режиме).
BLOCKED_COMMANDS: list[tuple[str, str]] = [
    (r"\brm\s+(-[a-zA-Z]*\s+)*-[a-zA-Z]*[rf][a-zA-Z]*\s+/(\s|$)", "удаление корня файловой системы"),
    (r"\brm\s+-rf\s+~(/\s*)?(\s|$)", "удаление домашнего каталога"),
    (r"\brm\s+(-[a-zA-Z]*\s+)*-[a-zA-Z]*[rf][a-zA-Z]*\s+/(bin|etc|usr|var|boot|System)(\s|$)", "удаление системных каталогов"),
    (r"\bmkfs(\.\w+)?\b", "форматирование раздела"),
    (r"\bdd\b[^\n]*\bof=/dev/(sd|hd|nvme|mmcblk|disk)", "запись напрямую на устройство"),
    (r">\s*/dev/(sd|hd|nvme|mmcblk|disk)", "перезапись устройства"),
    (r":\(\)\s*\{.*\}\s*;\s*:", "fork-бомба"),
    (r"\b(shutdown|reboot|halt|poweroff|init\s+0)\b", "выключение компьютера"),
    (r"\bchmod\s+(-R\s+)?(777|666)\s+/(\s|$)", "открытие прав на корень"),
    (r"\bchown\s+-R\b[^\n]*\s/(\s|$)", "смена владельца системных файлов"),
    (r"\b(format|diskpart|bcdedit|reg\s+delete)\b", "изменение системы Windows"),
    (r"\bdel\s+/[fsq]\s+[a-zA-Z]:", "удаление диска Windows"),
    (r"\brmdir\s+/s\s+/q\s+[a-zA-Z]:\\?\s*$", "удаление диска Windows"),
    (r"(curl|wget|iwr|invoke-webrequest)\b[^\n|]*\|\s*(ba|z|fi|k)?sh\b", "выполнение скачанного скрипта"),
    (r"\b(nc|ncat|netcat)\b[^\n]*\s-e\s", "обратная оболочка"),
    (r"\b(base64|echo)\b[^\n]*\|\s*(base64|sh|bash)[^\n]*\b-d\b", "скрытое выполнение кода"),
    (r"\bhistory\s+-c\b", "стирание истории команд"),
    (r"\bgit\s+push\s+.*--force\b[^\n]*\b(main|master)\b", "принудительная перезапись главной ветки"),
    (r"\b(sudo|doas|runas)\s+rm\b", "удаление с повышением прав"),
    (r"\bnpm\s+publish\b|\bpip\s+upload\b", "публикация пакета"),
    (r"\bkill\s+-9\s+1\b|\bkillall\b", "массовое убийство процессов"),
]

#: Команды, требующие подтверждения даже в автономном режиме.
RISKY_PATTERNS: list[tuple[str, str]] = [
    (r"\bgit\s+(reset\s+--hard|clean\s+-[a-z]*f)", "потеря незакоммиченных изменений"),
    (r"\bgit\s+push\b", "публикация изменений в удалённый репозиторий"),
    (r"\brm\s+-[a-zA-Z]*r[a-zA-Z]*\b", "рекурсивное удаление"),
    (r"\bsudo\b|\bdoas\b", "повышение прав"),
    (r"\bpip\s+install\b|\bnpm\s+install\s+-g\b|\bapt(-get)?\s+install\b", "установка пакетов"),
    (r"\bdocker\b|\bdocker-compose\b", "управление контейнерами"),
    (r"\bsystemctl\b|\bservice\b", "управление службами"),
    (r"\bcurl\b|\bwget\b", "сетевой запрос"),
    (r"[>]{1,2}\s*\S+", "перезапись файла через перенаправление"),
    (r"\bchmod\b|\bchown\b", "изменение прав"),
    (r"\bmv\b\s+\S+\s+/", "перемещение в системный каталог"),
]

_SECRET_PATTERNS: list[tuple[re.Pattern[str], str]] = [
    (re.compile(r"\b(sk-ant-[A-Za-z0-9_\-]{20,})"), "[ANTHROPIC_KEY]"),
    (re.compile(r"\b(sk-proj-[A-Za-z0-9_\-]{20,})"), "[OPENAI_KEY]"),
    (re.compile(r"\bsk-[A-Za-z0-9]{20,}\b"), "[API_KEY]"),
    (re.compile(r"\b(AIza[0-9A-Za-z_\-]{30,})"), "[GOOGLE_KEY]"),
    (re.compile(r"\b(gsk_[A-Za-z0-9]{20,})"), "[GROQ_KEY]"),
    (re.compile(r"\b(gh[pousr]_[A-Za-z0-9]{20,})"), "[GITHUB_TOKEN]"),
    (re.compile(r"\b(AKIA[0-9A-Z]{16})"), "[AWS_KEY]"),
    (re.compile(r"\b(xox[baprs]-[A-Za-z0-9\-]{10,})"), "[SLACK_TOKEN]"),
    (re.compile(r"-----BEGIN [A-Z ]*PRIVATE KEY-----[\s\S]*?-----END [A-Z ]*PRIVATE KEY-----"),
     "[PRIVATE_KEY]"),
    (re.compile(r"(?i)\b(password|passwd|pwd)\s*[:=]\s*['\"]?([^\s'\"]{6,})"), r"\1=[HIDDEN]"),
    (re.compile(r"(?i)\b(api[_-]?key|secret|token|access[_-]?key)\b\s*[:=]\s*['\"]?([\w\-.]{12,})"),
     r"\1=[HIDDEN]"),
    (re.compile(r"(?i)(postgres|mysql|mongodb|redis)(\+\w+)?://[^\s:@/]+:[^\s@/]+@"), r"\1://[HIDDEN]@"),
]


class Secrets:
    """Прячет секреты в текстах, которые уходят в модель или в логи."""

    def __init__(self, extra: list[str] | None = None) -> None:
        self.patterns = list(_SECRET_PATTERNS)
        for value in extra or []:
            if value and len(value) >= 8:
                self.patterns.append((re.compile(re.escape(value)), "[SECRET]"))

    def redact(self, text: str) -> str:
        if not text:
            return text
        for pattern, replacement in self.patterns:
            text = pattern.sub(replacement, text)
        return text

    def contains_secret(self, text: str) -> bool:
        return any(pattern.search(text) for pattern, _ in self.patterns)


def redact(text: str) -> str:
    """Быстрая очистка текста от типовых секретов."""
    return Secrets().redact(text)


def check_command(command: str) -> tuple[bool, str]:
    """
    Проверяет команду перед запуском.
    Возвращает (можно_выполнять, причина_запрета).
    """
    flat = " ".join(command.split())
    for pattern, reason in BLOCKED_COMMANDS:
        if re.search(pattern, flat, re.IGNORECASE):
            return False, reason
    return True, ""


def risk_reason(command: str) -> str:
    """Причина, по которой команду стоит подтвердить (пустая строка — не рискованная)."""
    flat = " ".join(command.split())
    for pattern, reason in RISKY_PATTERNS:
        if re.search(pattern, flat, re.IGNORECASE):
            return reason
    return ""


def is_inside(path: Path, root: Path) -> bool:
    """Находится ли путь внутри каталога (после разрешения симлинков)."""
    try:
        path.resolve().relative_to(root.resolve())
        return True
    except (ValueError, OSError):
        return False


def safe_path(raw: str, workspace: Path, extra_dirs: list[str] | None = None,
              *, must_exist: bool = False) -> Path:
    """Приводит путь к безопасному абсолютному внутри разрешённых каталогов."""
    path = Path(raw or ".").expanduser()
    if not path.is_absolute():
        path = workspace / path
    path = path.resolve()
    allowed = [workspace.resolve()] + [Path(d).expanduser().resolve() for d in (extra_dirs or [])]
    if not any(is_inside(path, root) or path == root for root in allowed):
        raise PermissionError(
            f"Путь вне рабочей папки: {path}\nРазрешено: {', '.join(str(a) for a in allowed)}"
        )
    if must_exist and not path.exists():
        raise FileNotFoundError(f"Файл не найден: {raw}")
    return path


def env_without_secrets() -> dict[str, str]:
    """Окружение для дочерних процессов без API-ключей."""
    hidden = {
        "ANTHROPIC_API_KEY", "OPENAI_API_KEY", "OPENROUTER_API_KEY", "GOOGLE_API_KEY",
        "GEMINI_API_KEY", "GROQ_API_KEY", "MISTRAL_API_KEY", "DEEPSEEK_API_KEY",
        "AIA_ANTHROPIC_KEY", "AIA_OPENAI_KEY", "AIA_OPENROUTER_KEY", "AIA_GOOGLE_KEY",
        "AIA_GROQ_KEY", "AWS_SECRET_ACCESS_KEY", "AWS_ACCESS_KEY_ID",
    }
    return {k: v for k, v in os.environ.items() if k not in hidden}


__all__ = [
    "BLOCKED_COMMANDS", "RISKY_PATTERNS", "Secrets", "check_command", "env_without_secrets",
    "is_inside", "redact", "risk_reason", "safe_path",
]
