"""Слэш-команды интерактивного режима: /help, /undo, /model, /compact и другие."""
from __future__ import annotations

import platform
import subprocess
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .config import MODE_DESCRIPTIONS, PERMISSION_MODES
from .providers import available_providers
from .utils.text import plural_ru, preview, text_table


@dataclass
class Command:
    name: str
    summary: str
    handler: Callable[[str, Any], bool]
    aliases: tuple[str, ...] = ()

    def help_line(self) -> str:
        names = ", ".join((self.name, *self.aliases))
        return f"  {names:<24} {self.summary}"


class CommandRegistry:
    """Набор команд, работающих с живым агентом и консолью."""

    def __init__(self, agent, console, app: Any = None) -> None:
        self.agent = agent
        self.console = console
        self.app = app
        self.commands: dict[str, Command] = {}
        self._register()

    # ------------------------------------------------------------------ регистрация
    def _add(self, name: str, summary: str, handler, aliases: tuple[str, ...] = ()) -> None:
        command = Command(name, summary, handler, aliases)
        self.commands[name] = command
        for alias in aliases:
            self.commands[alias] = command

    def _register(self) -> None:
        self._add("/help", "список команд", self.cmd_help, ("/h", "/?"))
        self._add("/exit", "выйти из агента", self.cmd_exit, ("/quit", "/q"))
        self._add("/tools", "какие инструменты доступны", self.cmd_tools)
        self._add("/model", "показать или сменить модель", self.cmd_model)
        self._add("/provider", "показать или сменить провайдера", self.cmd_provider)
        self._add("/mode", "режим разрешений: ask|edits|auto|plan|yolo", self.cmd_mode)
        self._add("/context", "заполнение контекстного окна", self.cmd_context)
        self._add("/compact", "сжать историю вручную", self.cmd_compact)
        self._add("/system", "показать системный промпт", self.cmd_system)
        self._add("/stats", "статистика сессии", self.cmd_stats, ("/status",))
        self._add("/undo", "откатить последние изменения (/undo 3)", self.cmd_undo)
        self._add("/history", "история изменений файлов", self.cmd_history)
        self._add("/todo", "текущий план работ", self.cmd_todo)
        self._add("/notes", "память проекта", self.cmd_notes, ("/memory",))
        self._add("/rules", "инструкции проекта", self.cmd_rules)
        self._add("/keys", "проверить ключи провайдеров", self.cmd_keys)
        self._add("/clear", "очистить экран", self.cmd_clear)
        self._add("/reset", "забыть историю диалога", self.cmd_reset)
        self._add("/save", "сохранить сессию", self.cmd_save)
        self._add("/load", "загрузить сессию", self.cmd_load)
        self._add("/sessions", "список сохранённых сессий", self.cmd_sessions)
        self._add("/verbose", "подробный вывод вкл/выкл", self.cmd_verbose)
        self._add("/edit", "открыть файл в редакторе (/edit path)", self.cmd_edit)
        self._add("/shell", "выполнить команду оболочки вручную (/shell ls)", self.cmd_shell)
        self._add("/diff", "показать незакоммиченные изменения (git diff)", self.cmd_diff)

    # ------------------------------------------------------------------ обработка
    def handle(self, line: str) -> bool:
        """Возвращает False, если нужно выйти из интерактивного режима."""
        name, _, argument = line.partition(" ")
        command = self.commands.get(name.lower())
        if command is None:
            self.console.warn(f"Неизвестная команда {name}. Список: /help")
            return True
        try:
            return command.handler(argument.strip(), self.agent)
        except Exception as e:  # noqa: BLE001 — команда не должна ронять сессию
            self.console.error(f"Команда {name} упала: {type(e).__name__}: {e}")
            return True

    # ------------------------------------------------------------------ команды
    def cmd_help(self, argument: str, agent) -> bool:
        seen = set()
        lines = ["Команды:"]
        for command in self.commands.values():
            if command.name in seen:
                continue
            seen.add(command.name)
            lines.append(command.help_line())
        lines += [
            "",
            "Просто напиши задачу обычными словами — агент выполнит её инструментами.",
            "Пример: «добавь в проект CLI-утилиту converter и тесты к ней».",
        ]
        self.console.header("\n".join(lines))
        return True

    def cmd_exit(self, argument: str, agent) -> bool:
        self.console.info("Выхожу. История изменений сохранена — вернуть можно командой aia undo.")
        return False

    def cmd_tools(self, argument: str, agent) -> bool:
        rows = [(tool.name, tool.category, preview(tool.description, 70)) for tool in agent.tools]
        self.console.header(f"Инструментов: {len(rows)} (режим {agent.config.mode})")
        self.console.header(text_table(rows, ["имя", "категория", "назначение"]))
        return True

    def cmd_model(self, argument: str, agent) -> bool:
        if not argument:
            self.console.info(f"Текущая модель: {agent.config.resolved_model()} "
                              f"({agent.provider.display_name})")
            models = agent.provider.list_models()
            if models:
                shown = ", ".join(models[:25])
                self.console.dim(f"Доступные: {shown}{' …' if len(models) > 25 else ''}")
            return True
        agent.config.model = argument
        agent.provider.config.model = argument
        agent.native_tools = bool(agent.provider.supports_tools(argument) and not agent._small_model())
        agent._refresh_prompt()
        self.console.success(f"Модель переключена: {argument}")
        return True

    def cmd_provider(self, argument: str, agent) -> bool:
        if not argument:
            rows = [(name, info["title"], info["local"], info["key_present"], info["default_model"])
                    for name, info in available_providers().items()]
            self.console.header(text_table(
                rows, ["провайдер", "название", "локальный", "ключ", "модель по умолчанию"]))
            return True
        from .providers import create_provider
        try:
            agent.config.provider = argument
            agent.provider = create_provider(agent.config, argument)
            agent._refresh_prompt()
            ok, message = agent.provider.health()
            (self.console.success if ok else self.console.warn)(
                f"Провайдер: {agent.provider.display_name}. {message}"
            )
        except Exception as e:  # noqa: BLE001
            self.console.error(str(e))
        return True

    def cmd_mode(self, argument: str, agent) -> bool:
        if not argument:
            lines = [f"Текущий режим: {agent.config.mode}"] + [
                f"  {mode:<6} {MODE_DESCRIPTIONS[mode]}" for mode in PERMISSION_MODES
            ]
            self.console.header("\n".join(lines))
            return True
        if argument not in PERMISSION_MODES:
            self.console.warn(f"Неизвестный режим. Доступны: {', '.join(PERMISSION_MODES)}")
            return True
        agent.config.mode = argument
        agent.tools = agent._choose_tools()
        agent.tool_map = {tool.name: tool for tool in agent.tools}
        agent._refresh_prompt()
        self.console.success(f"Режим: {argument} — {MODE_DESCRIPTIONS[argument]}")
        return True

    def cmd_context(self, argument: str, agent) -> bool:
        stats = agent.context.stats()
        self.console.header(agent.context.counter.describe(agent.context.messages))
        self.console.info(f"сообщений {stats.messages}, символов {stats.chars}, "
                          f"файлов затронуто {len(stats.files_touched)}")
        self.console.dim("сжатие истории: " + ("включено" if agent.config.auto_compact else "выключено")
                         + f" при {int(agent.config.compact_at_ratio * 100)}% заполнения")
        return True

    def cmd_compact(self, argument: str, agent) -> bool:
        before = len(agent.context.messages)
        summary = agent.context.compact(force=True)
        if not summary:
            self.console.info("Сжимать нечего — история короткая.")
            return True
        self.console.success(f"История сжата: было {before} сообщений, стало {len(agent.context.messages)}")
        self.console.dim(preview(summary, 400))
        return True

    def cmd_system(self, argument: str, agent) -> bool:
        self.console.header("Системный промпт:")
        print(agent.system_preview())
        return True

    def cmd_stats(self, argument: str, agent) -> bool:
        stats = agent.stats()
        lines = [
            f"провайдер:  {stats['provider']}",
            f"модель:     {stats['model']}",
            f"режим:      {stats['mode']}",
            f"инструментов: {len(stats['tools'])} (нативных вызовов: "
            f"{'да' if stats['native_tools'] else 'нет'})",
            f"контекст:   {stats['context']['tokens']} токенов, "
            f"{stats['context']['fill'] * 100:.0f}% окна",
            f"сжатий:     {stats['context']['compactions']}",
            f"чекпоинтов: {stats['checkpoints']}",
            f"сессия:     {int(stats['session_seconds'])} с",
        ]
        self.console.header("\n".join(lines))
        return True

    def cmd_undo(self, argument: str, agent) -> bool:
        count = int(argument) if argument.isdigit() else 1
        self.console.header("Откат изменений:")
        self.console.info(agent.checkpoints.undo(count) or "нечего откатывать")
        return True

    def cmd_history(self, argument: str, agent) -> bool:
        self.console.header(agent.checkpoints.history(int(argument) if argument.isdigit() else 20))
        return True

    def cmd_todo(self, argument: str, agent) -> bool:
        result = agent.tool_map["todo_read"].run({}, agent.ctx) if "todo_read" in agent.tool_map \
            else None
        if result is None:
            self.console.warn("Инструмент todo недоступен в текущем режиме.")
            return True
        self.console.header(result.content)
        return True

    def cmd_notes(self, argument: str, agent) -> bool:
        path = agent.memory_file
        if not path.is_file():
            self.console.info("Память проекта пуста. Агент заполнит её по ходу работы.")
            return True
        self.console.header(path.read_text(encoding="utf-8", errors="replace")[:4000])
        return True

    def cmd_rules(self, argument: str, agent) -> bool:
        files = [path for path in agent.paths.rules_files if path.is_file()]
        if not files:
            self.console.info("Инструкций проекта нет. Создать: aia init или файл AIAGENT.md.")
            return True
        for path in files:
            self.console.header(f"### {path}")
            print(path.read_text(encoding="utf-8", errors="replace")[:3000])
        return True

    def cmd_keys(self, argument: str, agent) -> bool:
        rows = []
        for name, info in available_providers().items():
            rows.append((name, info["title"], info["needs_key"], info["key_present"]))
        self.console.header(text_table(rows, ["провайдер", "название", "переменная", "ключ найден"]))
        self.console.dim("Установить ключ: aia keys set anthropic sk-ant-…")
        return True

    def cmd_clear(self, argument: str, agent) -> bool:
        import os
        os.system("cls" if platform.system() == "Windows" else "clear")
        return True

    def cmd_reset(self, argument: str, agent) -> bool:
        agent.reset()
        self.console.success("История диалога очищена (файлы и чекпоинты остались).")
        return True

    def cmd_save(self, argument: str, agent) -> bool:
        if self.app is None:
            self.console.warn("Сохранение доступно только в интерактивном режиме.")
            return True
        path = self.app.save_session(argument or None)
        self.console.success(f"Сессия сохранена: {path}")
        return True

    def cmd_load(self, argument: str, agent) -> bool:
        if self.app is None or not argument:
            self.console.warn("Укажи имя сессии: /load имя")
            return True
        try:
            self.app.load_session(argument)
            self.console.success(f"Сессия «{argument}» загружена.")
        except Exception as e:  # noqa: BLE001
            self.console.error(str(e))
        return True

    def cmd_sessions(self, argument: str, agent) -> bool:
        if self.app is None:
            return True
        sessions = self.app.list_sessions()
        if not sessions:
            self.console.info("Сохранённых сессий нет.")
            return True
        rows = [(item["name"], item["saved"], item["messages"], preview(item.get("first", ""), 40))
                for item in sessions[:20]]
        self.console.header(text_table(rows, ["сессия", "когда", "сообщений", "начало"]))
        return True

    def cmd_verbose(self, argument: str, agent) -> bool:
        self.console.verbose = not self.console.verbose
        self.console.success("Подробный вывод: " + ("включён" if self.console.verbose else "выключен"))
        return True

    def cmd_edit(self, argument: str, agent) -> bool:
        if not argument:
            self.console.warn("Укажи файл: /edit path/to/file.py")
            return True
        path = Path(agent.workspace) / argument
        if not path.is_file():
            self.console.error(f"Файл не найден: {argument}")
            return True
        import os
        editor = os.environ.get("EDITOR") or ("notepad" if platform.system() == "Windows" else "nano")
        try:
            subprocess.run([editor, str(path)], check=False)
        except OSError as e:
            self.console.error(f"Не удалось открыть редактор ({editor}): {e}")
        return True

    def cmd_shell(self, argument: str, agent) -> bool:
        if not argument:
            self.console.warn("Укажи команду: /shell git status")
            return True
        result = agent.tool_map["run_shell"].run({"command": argument}, agent.ctx)
        (self.console.success if result.ok else self.console.error)(result.content)
        return True

    def cmd_diff(self, argument: str, agent) -> bool:
        result = agent.tool_map["run_shell"].run(
            {"command": "git diff --stat" + (f" {argument}" if argument else "")}, agent.ctx)
        self.console.header(result.content)
        return True


def tools_footer(agent) -> str:
    count = len(agent.tools)
    return f"{count} {plural_ru(count, 'инструмент', 'инструмента', 'инструментов')}"


__all__ = ["Command", "CommandRegistry", "tools_footer"]
