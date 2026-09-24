"""Консольный интерфейс: живой вывод, карточки инструментов, вопросы пользователю."""
from __future__ import annotations

import sys
import threading
from typing import Any

from ..utils.ansi import Ansi, terminal_width
from ..utils.text import clip, preview


class Console:
    """
    Отображает события агента в терминале:
      • текст модели — потоком, по мере генерации;
      • вызовы инструментов — компактной карточкой с результатом;
      • вопросы подтверждения — интерактивно.
    """

    def __init__(self, *, color: bool = True, verbose: bool = False,
                 auto_approve: bool = False, quiet: bool = False) -> None:
        self.ansi = Ansi(color)
        self.verbose = verbose
        self.auto_approve = auto_approve
        self.quiet = quiet
        self._streaming = False
        self._thinking = False
        self._stop_requested = False
        self._on_interrupt = None
        self._lock = threading.Lock()

    # ------------------------------------------------------------------ вывод текста
    def text(self, chunk: str) -> None:
        if not chunk:
            return
        with self._lock:
            if self._thinking and not self._streaming:
                sys.stdout.write("\n")
                self._thinking = False
            if not self._streaming:
                sys.stdout.write("\n")
            self._streaming = True
            sys.stdout.write(chunk)
            sys.stdout.flush()

    def thinking(self, chunk: str) -> None:
        if not chunk or self.quiet:
            return
        with self._lock:
            if not self._thinking and not self._streaming:
                sys.stdout.write(self.ansi.dim("\n💭 размышляю: "))
                self._thinking = True
            sys.stdout.write(self.ansi.dim(chunk))
            sys.stdout.flush()

    def end_stream(self) -> None:
        with self._lock:
            if self._streaming or self._thinking:
                sys.stdout.write("\n")
                sys.stdout.flush()
            self._streaming = False
            self._thinking = False

    # ------------------------------------------------------------------ сообщения
    def info(self, message: str) -> None:
        self.end_stream()
        print(self.ansi.info(f"  {message}"))

    def warn(self, message: str) -> None:
        self.end_stream()
        print(self.ansi.warn(f"⚠ {message}"))

    def error(self, message: str) -> None:
        self.end_stream()
        print(self.ansi.error(f"✖ {message}"))

    def success(self, message: str) -> None:
        self.end_stream()
        print(self.ansi.ok(f"✔ {message}"))

    def dim(self, message: str) -> None:
        if not self.quiet:
            print(self.ansi.dim(message))

    def step(self, step: int, limit: int) -> None:
        self.end_stream()
        if not self.quiet:
            print(self.ansi.dim(f"── шаг {step}/{limit} ──"))

    def header(self, title: str) -> None:
        print(self.ansi.heading(title))

    def rule(self, width: int | None = None) -> None:
        print(self.ansi.dim("─" * (width or min(terminal_width(), 70))))

    def banner(self, model: str, provider: str, workspace: str, tools: int,
               mode: str, extra: str = "") -> None:
        print(self.ansi.heading("AI Agent Studio") + self.ansi.dim("  v1.0"))
        print(f"  модель:   {self.ansi.bold(model)} {self.ansi.dim('(' + provider + ')')}")
        print(f"  проект:   {workspace}")
        print(f"  режим:    {mode}   инструментов: {tools}")
        if extra:
            print(self.ansi.dim(f"  {extra}"))
        print(self.ansi.dim("  /help — команды, /exit — выход\n"))

    # ------------------------------------------------------------------ инструменты
    def tool_start(self, name: str, arguments: dict[str, Any]) -> None:
        self.end_stream()
        if self.quiet:
            return
        args = preview(_format_args(arguments), 110)
        print(self.ansi.tool(f"\n🔧 {name}") + (f" {self.ansi.dim(args)}" if args else ""))

    def tool_end(self, name: str, ok: bool, display: str, content: str = "",
                 seconds: float = 0.0) -> None:
        mark = self.ansi.ok("  ✔") if ok else self.ansi.error("  ✘")
        note = f" ({seconds:.1f} с)" if seconds else ""
        print(f"{mark} {display or name}{self.ansi.dim(note)}")
        if self.verbose and content:
            body = clip(content, 1500)
            print(self.ansi.dim("\n".join("     " + line for line in body.splitlines())))

    # ------------------------------------------------------------------ подтверждения
    def confirm(self, title: str, description: str) -> bool:
        self.end_stream()
        if self.auto_approve:
            print(self.ansi.dim(f"  (авто) {title}"))
            return True
        print(self.ansi.warn(f"\n{title}"))
        body = clip(description, 2000)
        print(self.ansi.dim("\n".join("   " + line for line in body.splitlines())))
        try:
            answer = input(self.ansi.bold("Выполнить? [д/н] (Enter — да): ")).strip().lower()
        except (EOFError, KeyboardInterrupt):
            print()
            return False
        return answer in {"", "д", "да", "y", "yes", "1"}

    def ask(self, prompt: str) -> str:
        self.end_stream()
        try:
            return input(prompt).strip()
        except (EOFError, KeyboardInterrupt):
            print()
            return ""

    # ------------------------------------------------------------------ прерывание
    def install_interrupt(self, callback) -> None:
        """Ctrl+C прерывает работу агента, а не выходит из программы."""
        self._on_interrupt = callback
        try:
            import signal

            def handler(signum, frame):  # noqa: ARG001
                self._stop_requested = True
                self.warn("Останавливаю… (Ctrl+C ещё раз — выйти)")
                if callback:
                    callback()

            signal.signal(signal.SIGINT, handler)
        except (ImportError, ValueError, OSError):
            pass

    def final_report(self, meta: dict[str, Any]) -> None:
        """Итоговая строка после выполнения задачи."""
        self.end_stream()
        usage = meta.get("usage_line") or ""
        files = meta.get("files_changed") or []
        parts = []
        if meta.get("steps"):
            parts.append(f"шагов {meta['steps']}")
        if meta.get("tool_calls"):
            parts.append(f"инструментов {meta['tool_calls']}")
        if meta.get("duration"):
            parts.append(f"{float(meta['duration']):.1f} с")
        if usage:
            parts.append(usage)
        if parts:
            print(self.ansi.dim("\n" + " · ".join(parts)))
        if files:
            print(self.ansi.dim("изменённые файлы: " + ", ".join(map(str, files[:10]))))


def _format_args(arguments: dict[str, Any]) -> str:
    import json

    if not arguments:
        return ""
    try:
        return json.dumps(arguments, ensure_ascii=False, separators=(", ", ": "))
    except (TypeError, ValueError):
        return str(arguments)


__all__ = ["Console"]
