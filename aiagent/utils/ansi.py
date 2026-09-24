"""ANSI-цвета и оформление терминала (без внешних зависимостей)."""
from __future__ import annotations

import os
import re
import shutil
import sys

_RESET = "\033[0m"
_ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


class Ansi:
    """Цветной вывод. Автоматически выключается, если вывод не в терминал или задан NO_COLOR."""

    RESET = _RESET
    BOLD = "\033[1m"
    DIM = "\033[2m"
    ITALIC = "\033[3m"
    UNDERLINE = "\033[4m"

    BLACK = "\033[30m"
    RED = "\033[31m"
    GREEN = "\033[32m"
    YELLOW = "\033[33m"
    BLUE = "\033[34m"
    MAGENTA = "\033[35m"
    CYAN = "\033[36m"
    WHITE = "\033[37m"
    GRAY = "\033[90m"
    BRIGHT_RED = "\033[91m"
    BRIGHT_GREEN = "\033[92m"
    BRIGHT_YELLOW = "\033[93m"
    BRIGHT_BLUE = "\033[94m"
    BRIGHT_MAGENTA = "\033[95m"
    BRIGHT_CYAN = "\033[96m"

    def __init__(self, enabled: bool | None = None) -> None:
        if enabled is None:
            enabled = self._auto()
        self.enabled = bool(enabled)

    @staticmethod
    def _auto() -> bool:
        if os.environ.get("NO_COLOR"):
            return False
        if os.environ.get("FORCE_COLOR"):
            return True
        try:
            return sys.stdout.isatty()
        except Exception:  # noqa: BLE001 — в ноутбуках stdout бывает экзотическим
            return False

    def paint(self, text: str, *styles: str) -> str:
        if not self.enabled or not styles:
            return text
        return f"{''.join(styles)}{text}{_RESET}"

    # --- частые случаи -------------------------------------------------------------
    def ok(self, text: str) -> str:
        return self.paint(text, self.GREEN)

    def error(self, text: str) -> str:
        return self.paint(text, self.RED)

    def warn(self, text: str) -> str:
        return self.paint(text, self.YELLOW)

    def info(self, text: str) -> str:
        return self.paint(text, self.CYAN)

    def dim(self, text: str) -> str:
        return self.paint(text, self.GRAY)

    def bold(self, text: str) -> str:
        return self.paint(text, self.BOLD)

    def tool(self, text: str) -> str:
        return self.paint(text, self.BRIGHT_MAGENTA)

    def user(self, text: str) -> str:
        return self.paint(text, self.BRIGHT_GREEN)

    def assistant(self, text: str) -> str:
        return self.paint(text, self.BRIGHT_BLUE)

    def heading(self, text: str) -> str:
        return self.paint(text, self.BOLD, self.BRIGHT_CYAN)

    # --- служебное -----------------------------------------------------------------
    def visible_len(self, text: str) -> int:
        return len(_ANSI_RE.sub("", text))

    def truncate(self, text: str, width: int) -> str:
        plain = _ANSI_RE.sub("", text)
        if len(plain) <= width:
            return text
        return plain[: max(0, width - 1)] + "…"

    def box(self, title: str, body: str, width: int | None = None) -> str:
        width = width or min(shutil.get_terminal_size((90, 24)).columns, 100)
        inner = width - 4
        lines = []
        for raw in body.splitlines() or [""]:
            while len(raw) > inner:
                lines.append(raw[:inner])
                raw = raw[inner:]
            lines.append(raw)
        top = f"╭─ {title} " + "─" * max(0, width - len(title) - 5) + "╮"
        bottom = "╰" + "─" * (width - 2) + "╯"
        middle = "\n".join(f"│ {line.ljust(inner)} │" for line in lines)
        return f"{top}\n{middle}\n{bottom}"


def strip_ansi(text: str) -> str:
    return _ANSI_RE.sub("", text)


def terminal_width(default: int = 90) -> int:
    try:
        return shutil.get_terminal_size((default, 24)).columns
    except Exception:  # noqa: BLE001
        return default


__all__ = ["Ansi", "strip_ansi", "terminal_width"]
