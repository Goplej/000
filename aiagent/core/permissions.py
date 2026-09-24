"""Разрешения: решает, можно ли выполнить инструмент без вопроса.

Логика (по порядку):
  1. жёсткий блок-лист опасных команд — запрет всегда;
  2. deny-правила пользователя — запрет;
  3. allow-правила пользователя — разрешение без вопроса;
  4. режим (ask/edits/auto/plan/yolo) + тип инструмента;
  5. всё остальное — спросить.
"""
from __future__ import annotations

import fnmatch
import re
from dataclasses import dataclass
from pathlib import Path

from ..config import DANGEROUS_TOOLS, WRITE_TOOLS, AgentConfig
from ..utils.security import check_command, risk_reason


@dataclass
class Decision:
    allowed: bool
    needs_prompt: bool = False
    reason: str = ""
    source: str = "mode"          # deny | allow | mode | rule | blocked

    def __bool__(self) -> bool:
        return self.allowed


class PermissionManager:
    """Единая точка принятия решений по инструментам."""

    def __init__(self, config: AgentConfig) -> None:
        self.config = config

    # ------------------------------------------------------------------ основной вход
    def check(self, tool_name: str, arguments: dict, *, workspace: Path) -> Decision:
        mode = self.config.mode
        perms = self.config.permissions

        # 1. Жёсткие ограничения
        if tool_name in {"run_shell", "run_code"}:
            command = str(arguments.get("command") or arguments.get("code") or "")
            ok, reason = check_command(command)
            if not ok:
                return Decision(False, reason=f"Команда заблокирована: {reason}", source="blocked")

        if tool_name == "run_shell" and not self.config.allow_shell:
            return Decision(False, reason="Выполнение команд отключено (allow_shell=false)", source="config")
        if tool_name.startswith(("web_", "fetch_", "http_")) and not self.config.allow_web:
            return Decision(False, reason="Доступ в интернет отключён (allow_web=false)", source="config")
        if tool_name == "task" and not self.config.allow_subagents:
            return Decision(False, reason="Под-агенты отключены (allow_subagents=false)", source="config")

        # 2. Пользовательские правила
        deny = self._match_rules(perms.deny, tool_name, arguments)
        if deny:
            return Decision(False, reason=f"Запрещено правилом проекта: {deny}", source="deny")
        allow = self._match_rules(perms.allow, tool_name, arguments)
        if allow:
            return Decision(True, reason=f"Разрешено правилом: {allow}", source="allow")
        ask = self._match_rules(perms.ask, tool_name, arguments)
        if ask:
            return Decision(True, needs_prompt=True, reason=f"Требует подтверждения: {ask}", source="rule")

        # 3. Режимы
        if mode == "yolo":
            return Decision(True, reason="режим yolo", source="mode")
        if mode == "plan":
            if tool_name in WRITE_TOOLS or tool_name in DANGEROUS_TOOLS:
                return Decision(False,
                                reason="Режим plan: изменения запрещены, доступно только чтение и планирование",
                                source="mode")
            return Decision(True, reason="режим plan (только чтение)", source="mode")
        if mode == "auto":
            if tool_name in DANGEROUS_TOOLS:
                return Decision(True, needs_prompt=True,
                                reason="опасный инструмент в режиме auto", source="mode")
            if tool_name == "run_shell":
                return Decision(True, needs_prompt=True, reason="команда оболочки", source="mode")
            return Decision(True, reason="режим auto (в пределах проекта)", source="mode")
        if mode == "edits":
            if tool_name in WRITE_TOOLS:
                return Decision(True, reason="режим edits: правка файлов разрешена", source="mode")
            if tool_name in DANGEROUS_TOOLS:
                return Decision(True, needs_prompt=True, reason="опасный инструмент в режиме edits",
                                source="mode")
            return Decision(True, reason="чтение разрешено", source="mode")
        # mode == "ask"
        if tool_name in WRITE_TOOLS:
            return Decision(True, needs_prompt=self.config.confirm_writes or True,
                            reason="режим ask", source="mode")
        if tool_name in DANGEROUS_TOOLS:
            return Decision(True, needs_prompt=True, reason="режим ask", source="mode")
        return Decision(True, reason="чтение разрешено", source="mode")

    # ------------------------------------------------------------------------ риски
    def risk(self, tool_name: str, arguments: dict) -> str:
        """Короткое описание риска для карточки подтверждения."""
        if tool_name in {"run_shell", "run_code"}:
            command = str(arguments.get("command") or arguments.get("code") or "")
            reason = risk_reason(command)
            if reason:
                return reason
            return "выполнение команды в системе"
        if tool_name in WRITE_TOOLS:
            path = arguments.get("path") or arguments.get("file_path") or ""
            return f"изменение файла {path}" if path else "изменение файлов"
        if tool_name == "delete_file":
            return f"удаление {arguments.get('path', 'файла')}"
        if tool_name == "task":
            return "запуск под-агента (тратит токены и время)"
        if tool_name.startswith(("web_", "fetch_")):
            return "запрос во внешнюю сеть"
        return ""

    # ----------------------------------------------------------------------- правила
    @staticmethod
    def _match_rules(rules: list[str], tool_name: str, arguments: dict) -> str:
        """
        Правило выглядит как "tool" или "tool(шаблон)".
        Например: "run_shell(git status:*)" или "write_file(src/*)".
        """
        for rule in rules or []:
            rule = rule.strip()
            if not rule:
                continue
            match = re.match(r"^([\w.*]+)(?:\((.*)\))?$", rule)
            if not match:
                continue
            pattern_name, pattern_arg = match.group(1), match.group(2)
            if not fnmatch.fnmatch(tool_name, pattern_name):
                continue
            if pattern_arg is None:
                return rule
            value = _primary_argument(tool_name, arguments)
            if _arg_matches(pattern_arg, value):
                return rule
        return ""

    def describe_rules(self) -> str:
        perms = self.config.permissions
        lines = []
        if perms.allow:
            lines.append("разрешено: " + ", ".join(perms.allow))
        if perms.deny:
            lines.append("запрещено: " + ", ".join(perms.deny))
        if perms.ask:
            lines.append("спрашивать: " + ", ".join(perms.ask))
        return "; ".join(lines) or "особых правил нет"


def _primary_argument(tool_name: str, arguments: dict) -> str:
    for key in ("command", "path", "file_path", "url", "pattern", "code"):
        if key in arguments:
            return str(arguments[key])
    return " ".join(str(value) for value in arguments.values())


def _arg_matches(pattern: str, value: str) -> bool:
    if pattern in {"*", ""}:
        return True
    if fnmatch.fnmatch(value, pattern):
        return True
    if pattern.endswith(":*"):
        return value.startswith(pattern[:-2])
    # частичное совпадение для команд вида "git status"
    return pattern.rstrip("*") in value


__all__ = ["Decision", "PermissionManager"]
