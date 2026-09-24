"""Командная строка AI Agent Studio (`aia`).

Команды:
    aia                     интерактивный режим в текущем проекте
    aia "задача"            выполнить задачу и выйти
    aia run "задача"        то же, явная форма
    aia doctor              проверить окружение, ключи, интернет, железо
    aia keys set|list       управление ключами API
    aia models [-p имя]     доступные модели и провайдеры
    aia pull [модель]       скачать локальную модель (Ollama)
    aia serve               HTTP API + веб-интерфейс
    aia init [папка]        подготовить проект (AIAGENT.md, .aiagent.json)
    aia sessions            сохранённые сессии
    aia undo [N]            откатить изменения
    aia mcp                 подключённые MCP-серверы и их инструменты
    aia tools               список инструментов агента
    aia config              показать/изменить настройки
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import textwrap
from pathlib import Path
from typing import Any

from . import __product__, __version__
from .commands import CommandRegistry, tools_footer
from .config import DEFAULT_MODELS, KEY_ENV, MODE_DESCRIPTIONS, PERMISSION_MODES, AgentConfig
from .errors import AgentError, AuthError, ConfigError, ProviderError
from .hardware import detect, doctor_text
from .paths import config_path, ensure_state_dir, keys_path, project_paths, state_dir
from .providers import REGISTRY, available_providers, create_provider
from .session import Session, latest_session, list_sessions, load_session, new_session
from .ui import Console
from .utils.logging import get_logger, log_dir, setup_logging
from .utils.text import plural_ru, preview, text_table

log = get_logger("cli")
COMMANDS = {"run", "doctor", "keys", "models", "pull", "serve", "init", "sessions", "undo",
            "mcp", "tools", "config", "version", "help", "chat"}


# --------------------------------------------------------------------------------------
# Точка входа
# --------------------------------------------------------------------------------------
def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)

    # Короткая форма: aia "задача" или aia задача
    if argv and argv[0] not in COMMANDS and not argv[0].startswith("-"):
        argv = ["run"] + argv
    elif len(argv) > 1 and argv[0].startswith("-"):
        argv = ["run"] + argv
    if not argv:
        argv = ["chat"]

    parser = build_parser()
    args = parser.parse_args(argv)

    if args.version:
        print(f"{__product__} {__version__}")
        return 0
    if args.help or args.command == "help":
        print(_help_text())
        return 0

    try:
        return _dispatch(args)
    except KeyboardInterrupt:
        print("\nПрервано.")
        return 130
    except AuthError as e:
        print(f"Нужен ключ API: {e.human()}", file=sys.stderr)
        return 3
    except (ConfigError, ProviderError, AgentError) as e:
        print(f"Ошибка: {e.human()}", file=sys.stderr)
        return 2
    except Exception as e:  # noqa: BLE001 — CLI не должен показывать трейсбек без нужды
        if os.environ.get("AIA_DEBUG"):
            raise
        print(f"Внутренняя ошибка: {type(e).__name__}: {e}\n"
              f"Повтори с AIA_DEBUG=1, чтобы увидеть трейсбек.", file=sys.stderr)
        return 1


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(add_help=False, prog="aia")
    parser.add_argument("command", nargs="?", default="chat")
    parser.add_argument("task", nargs="*")
    parser.add_argument("--provider", "-p", default="")
    parser.add_argument("--model", "-m", default="")
    parser.add_argument("--mode", default="")
    parser.add_argument("--workspace", "-w", default="")
    parser.add_argument("--max-steps", type=int, default=0)
    parser.add_argument("--temperature", type=float, default=None)
    parser.add_argument("--base-url", default="")
    parser.add_argument("--max-tokens", type=int, default=0)
    parser.add_argument("--thinking", type=int, default=0)
    parser.add_argument("--no-stream", action="store_true")
    parser.add_argument("--yolo", action="store_true")
    parser.add_argument("--quiet", "-q", action="store_true")
    parser.add_argument("--verbose", "-v", action="store_true")
    parser.add_argument("--no-color", action="store_true")
    parser.add_argument("--resume", nargs="?", const="__last__", default="")
    parser.add_argument("--session", default="")
    parser.add_argument("--tools", default="")
    parser.add_argument("--host", default="")
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--set", nargs="*", default=None)
    parser.add_argument("--show", action="store_true")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--version", action="store_true")
    parser.add_argument("--help", "-h", action="store_true")
    return parser


def _dispatch(args) -> int:
    handlers = {
        "chat": cmd_chat,
        "run": cmd_run,
        "doctor": cmd_doctor,
        "keys": cmd_keys,
        "models": cmd_models,
        "pull": cmd_pull,
        "serve": cmd_serve,
        "init": cmd_init,
        "sessions": cmd_sessions,
        "undo": cmd_undo,
        "mcp": cmd_mcp,
        "tools": cmd_tools,
        "config": cmd_config,
        "version": lambda a: (print(f"{__product__} {__version__}"), 0)[1],
    }
    handler = handlers.get(args.command)
    if handler is None:
        print(f"Неизвестная команда: {args.command}\n\n{_help_text()}", file=sys.stderr)
        return 2
    return handler(args) or 0


def build_config(args, workspace: str | None = None) -> AgentConfig:
    overrides: dict[str, Any] = {}
    if workspace:
        overrides["workspace"] = workspace
    if args.provider:
        overrides["provider"] = args.provider
    if args.model:
        overrides["model"] = args.model
    if args.mode:
        overrides["mode"] = args.mode
    if args.max_steps:
        overrides["max_steps"] = args.max_steps
    if args.temperature is not None:
        overrides["temperature"] = args.temperature
    if args.base_url:
        overrides["base_url"] = args.base_url
    if args.max_tokens:
        overrides["max_tokens"] = args.max_tokens
    if args.thinking:
        overrides["thinking_budget"] = args.thinking
    if args.no_stream:
        overrides["stream"] = False
    if args.yolo:
        overrides["mode"] = "yolo"
    if args.verbose:
        overrides["verbose"] = True
    if args.tools:
        overrides["enabled_tools"] = [name.strip() for name in args.tools.split(",") if name.strip()]
    if args.host:
        overrides["web_host"] = args.host
    if args.port:
        overrides["web_port"] = args.port
    cfg = AgentConfig.load(args.workspace or None, overrides)
    if args.quiet:
        cfg.verbose = False
    return cfg


def make_console(args, config: AgentConfig) -> Console:
    return Console(color=not args.no_color, verbose=config.verbose,
                   auto_approve=config.mode == "yolo", quiet=args.quiet)


# --------------------------------------------------------------------------------------
# Интерактивный режим
# --------------------------------------------------------------------------------------
def cmd_chat(args) -> int:
    from .core import Agent, parse_reply  # noqa: F401

    config = build_config(args)
    console = make_console(args, config)
    setup_logging(config.verbose, log_dir() / "agent.log")

    try:
        agent = Agent(config, ui=console)
    except AuthError as e:
        console.error(e.human())
        console.info("Подсказка: `aia keys set anthropic <ключ>` или запусти локально через Ollama.")
        return 3

    ok, message = agent.health()
    if not ok:
        console.warn(f"Провайдер отвечает с ошибкой: {message}")
        console.dim("Проверить окружение: aia doctor")
    elif message:
        console.dim(message)

    app = _ChatApp(config, agent, console)
    if args.resume:
        name = None if args.resume in {"", "__last__"} else args.resume
        try:
            session = load_session(config, name) if name else latest_session(config)
            if session:
                app.load_session(session)
                console.success(f"Продолжаем сессию «{session.name}» ({len(session.messages)} сообщений)")
            else:
                console.warn("Сохранённых сессий нет — начинаем новую.")
        except Exception as e:  # noqa: BLE001
            console.warn(f"Не удалось загрузить сессию: {e}")

    console.banner(model=config.resolved_model(), provider=agent.provider.display_name,
                   workspace=str(agent.workspace), tools=len(agent.tools),
                   mode=f"{config.mode} ({MODE_DESCRIPTIONS[config.mode].split('(')[0].strip().lower()})",
                   extra=f"провайдер: {agent.provider.name} · {tools_footer(agent)}")

    registry = CommandRegistry(agent, console, app)
    console.install_interrupt(agent.stop)

    while True:
        try:
            line = input(console.ansi.paint("\n› ", "\033[1;32m")).strip()
        except (EOFError, KeyboardInterrupt):
            console.info("\nВыход.")
            app.save_session()
            return 0
        if not line:
            continue
        if line.startswith("/"):
            if not registry.handle(line):
                app.save_session()
                return 0
            continue
        try:
            app.ask(line)
        except KeyboardInterrupt:
            agent.stop()
            console.warn("Прервано.")
        except (AuthError, ProviderError, AgentError) as e:
            console.error(e.human())


class _ChatApp:
    """Связка агента, сессии и интерфейса для интерактивного режима."""

    def __init__(self, config: AgentConfig, agent, console: Console) -> None:
        self.config = config
        self.agent = agent
        self.console = console
        self.session: Session = new_session(config, None)

    # ------------------------------------------------------------------ основной цикл
    def ask(self, text: str) -> None:
        self.session.add("user", text)
        answer = ""
        for event in self.agent.run(text):
            self._render(event)
            if event.type == "done":
                answer = event.text or ""
                self.console.final_report(event.meta or {})
        if answer:
            self.session.add("assistant", answer)
        self.session.save()

    def _render(self, event) -> None:
        console = self.console
        if event.type == "text":
            console.text(event.text)
        elif event.type == "thinking":
            console.thinking(event.text)
        elif event.type == "step":
            console.step(int(event.meta.get("step", 0)), int(event.meta.get("limit", 0)))
        elif event.type == "tool_start":
            console.tool_start(event.name, (event.call.arguments if event.call else {}))
        elif event.type == "tool_end":
            console.tool_end(event.name, event.ok, event.display, event.content,
                             float((event.meta or {}).get("seconds") or 0))
        elif event.type == "info":
            if event.text:
                console.info(event.text)
            elif event.meta:
                stats = event.meta.get("stats") or event.meta
                speed = stats.get("tok_per_s") if isinstance(stats, dict) else None
                if speed:
                    console.dim(f"  ↑ {speed} ток/с")
        elif event.type == "compact":
            console.info("Контекст сжат: " + preview(event.text, 200))
        elif event.type == "warn":
            console.warn(event.text)
        elif event.type == "error":
            console.error(event.text)
        elif event.type == "done":
            console.end_stream()

    # ------------------------------------------------------------------ сессии
    def save_session(self, name: str | None = None) -> Path:
        if name:
            self.session.name = name
            self.session.path = self.session.path.parent / f"{name}.json"
        self.session.meta = {
            "model": self.config.resolved_model(),
            "provider": self.config.resolved_provider(),
            "workspace": str(self.config.workspace),
            "tools": [tool.name for tool in self.agent.tools],
        }
        return self.session.save()

    def load_session(self, session: Session) -> None:
        self.session = session
        self.agent.load_session({"messages": [
            {"role": m.get("role", "user"), "content": m.get("content", "")}
            for m in session.messages if m.get("role") in {"user", "assistant"}
        ]})

    def list_sessions(self) -> list[dict[str, Any]]:
        out = []
        for session in list_sessions(self.config):
            first = next((m["content"] for m in session.messages if m.get("role") == "user"), "")
            out.append({
                "name": session.name,
                "saved": __import__("time").strftime("%Y-%m-%d %H:%M", __import__("time").localtime(session.updated)),
                "messages": len(session.messages),
                "first": first,
            })
        return out


# --------------------------------------------------------------------------------------
# Разовый запуск задачи
# --------------------------------------------------------------------------------------
def cmd_run(args) -> int:
    task = " ".join(args.task).strip()
    if not task:
        return cmd_chat(args)

    from .core import Agent

    config = build_config(args)
    console = make_console(args, config)
    setup_logging(config.verbose, log_dir() / "agent.log")

    try:
        agent = Agent(config, ui=console)
    except AuthError as e:
        console.error(e.human())
        return 3

    app = _ChatApp(config, agent, console)
    if args.resume:
        name = None if args.resume in {"", "__last__"} else args.resume
        session = load_session(config, name) if name else latest_session(config)
        if session:
            app.load_session(session)
            console.dim(f"продолжаю сессию «{session.name}»")

    console.header(f"{__product__}: {preview(task, 90)}")
    app.ask(task)
    if args.session:
        app.save_session(args.session)
    return 0


# --------------------------------------------------------------------------------------
# doctor
# --------------------------------------------------------------------------------------
def cmd_doctor(args) -> int:
    console = Console(color=not args.no_color, verbose=False, quiet=args.quiet)
    config = build_config(args)

    console.header(f"=== {__product__} {__version__}: проверка окружения ===")
    print(f"Python: {sys.version.split()[0]} ({sys.executable})")
    print(f"Состояние: {state_dir()}")
    print(f"Проект:    {config.workspace}")
    print(f"Провайдер: {config.resolved_provider()} → модель {config.resolved_model()}")
    print(f"Режим:     {config.mode}")

    print("\n" + doctor_text(detect()))

    # Ключи
    print("\n=== Ключи API ===")
    rows = []
    for name, info in available_providers().items():
        rows.append((name, info["title"], info["needs_key"], info["key_present"]))
    print(text_table(rows, ["провайдер", "название", "переменная окружения", "ключ"]))
    missing = [name for name, info in available_providers().items()
               if info["needs_key"] != "нет" and info["key_present"] == "нет"]
    if missing:
        print("\nБез ключей недоступны: " + ", ".join(missing))
        print("Установить: aia keys set anthropic sk-ant-…")
    print("Локально без ключей: ollama (aia pull) и демо-режим (--provider mock)")

    # Связь с провайдером
    print("\n=== Связь с моделью ===")
    try:
        provider = create_provider(config)
        ok, message = provider.health()
        print(("✓ " if ok else "✗ ") + provider.display_name + (f": {message}" if message else ""))
        if not ok:
            print("  Подсказка: проверь ключ/адрес или переключись: --provider mock / --provider ollama")
    except Exception as e:  # noqa: BLE001
        print(f"✗ не удалось создать провайдера: {e}")

    # Интернет
    print("\n=== Интернет ===")
    try:
        from .tools.search import search
        outcome = search("проверка связи", limit=1, timeout=10)
        print("✓ веб-поиск работает" + (f" (через {outcome.engine})" if outcome.engine else ""))
        if not outcome.ok:
            print("  " + "; ".join(outcome.errors[:3]))
    except Exception as e:  # noqa: BLE001
        print(f"✗ веб-поиск недоступен: {e}")

    # Инструменты и MCP
    print("\n=== Инструменты ===")
    from .tools import build_registry
    registry = build_registry()
    by_category: dict[str, list[str]] = {}
    for tool in registry.values():
        by_category.setdefault(tool.category, []).append(tool.name)
    for category, names in sorted(by_category.items()):
        print(f"  {category:<10} {', '.join(sorted(names))}")
    print(f"  всего: {len(registry)} {plural_ru(len(registry), 'инструмент', 'инструмента', 'инструментов')}")

    if args.show:
        print("\n=== Конфигурация ===")
        print(json.dumps(config.to_dict(), ensure_ascii=False, indent=2))

    print("\nГотово. Начни работу: aia  (или aia \"создай README для проекта\")")
    return 0


# --------------------------------------------------------------------------------------
# keys
# --------------------------------------------------------------------------------------
def cmd_keys(args) -> int:
    action = (args.task[0] if args.task else "list").lower()
    known = [name for name in REGISTRY if KEY_ENV.get(name)]

    if action in {"list", "ls"}:
        rows = []
        for name in known:
            envs = KEY_ENV[name]
            present = next((env for env in envs if os.environ.get(env)), "")
            rows.append((name, ", ".join(envs), "да" if present else "нет", present))
        print(text_table(rows, ["провайдер", "переменные", "ключ найден", "источник"]))
        print(f"\nФайл ключей: {keys_path()} (права 600, в git не попадёт)")
        return 0

    if action in {"set", "add"}:
        if len(args.task) < 3:
            print("Использование: aia keys set <провайдер> <ключ>\n"
                  "Например: aia keys set anthropic sk-ant-api03-…")
            return 2
        provider, key = args.task[1].lower(), args.task[2]
        if provider not in known:
            print(f"Неизвестный провайдер: {provider}. Доступны: {', '.join(known)}")
            return 2
        path = keys_path()
        ensure_state_dir()
        data: dict[str, str] = {}
        if path.is_file():
            try:
                data = json.loads(path.read_text(encoding="utf-8"))
            except json.JSONDecodeError:
                data = {}
        data[provider] = key
        path.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
        try:
            path.chmod(0o600)
        except OSError:
            pass
        print(f"Ключ для {provider} сохранён в {path}")
        print("Проверить: aia doctor")
        return 0

    if action in {"rm", "delete", "remove"}:
        if len(args.task) < 2:
            print("Использование: aia keys rm <провайдер>")
            return 2
        provider = args.task[1].lower()
        path = keys_path()
        if path.is_file():
            data = json.loads(path.read_text(encoding="utf-8"))
            data.pop(provider, None)
            path.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
        print(f"Ключ {provider} удалён.")
        return 0

    print("Использование: aia keys [list | set <провайдер> <ключ> | rm <провайдер>]")
    return 2


# --------------------------------------------------------------------------------------
# models / pull
# --------------------------------------------------------------------------------------
def cmd_models(args) -> int:
    config = build_config(args)
    try:
        provider = create_provider(config)
    except Exception as e:  # noqa: BLE001
        print(f"Не удалось создать провайдера: {e}")
        return 2

    print(f"Провайдер: {provider.display_name} ({provider.name})")
    print(f"Модель по умолчанию: {config.resolved_model()}\n")

    models = provider.list_models()
    if models:
        print(f"Доступные модели ({len(models)}):")
        for name in models[:60]:
            marker = " ← текущая" if name == config.resolved_model() else ""
            print(f"  {name}{marker}")
        if len(models) > 60:
            print(f"  … ещё {len(models) - 60}")
    else:
        print("Список моделей недоступен (проверь ключ или адрес сервера).")

    if args.json:
        print(json.dumps({"provider": provider.name, "models": models}, ensure_ascii=False, indent=2))
    return 0


def cmd_pull(args) -> int:
    config = build_config(args, )
    config.provider = "ollama"
    model = " ".join(args.task).strip() or config.model or DEFAULT_MODELS["ollama"]
    provider = create_provider(config, "ollama")

    if not shutil.which("ollama"):
        print("Ollama не установлен. Поставь с https://ollama.com/download\n"
              "   или работай через облако: aia keys set anthropic <ключ>")
        return 2

    print(f"Скачиваю модель {model} (это может занять время)…")
    try:
        proc = subprocess.run(["ollama", "pull", model], check=False)
    except OSError as e:
        print(f"Не удалось запустить ollama: {e}")
        return 2
    if proc.returncode != 0:
        print(f"ollama pull завершился с кодом {proc.returncode}")
        return proc.returncode

    ok, message = provider.health()
    print(("✓ " if ok else "✗ ") + message)
    print(f"\nЗапуск: aia --provider ollama --model {model}")
    return 0


# --------------------------------------------------------------------------------------
# serve / init / sessions / undo / mcp / tools / config
# --------------------------------------------------------------------------------------
def cmd_serve(args) -> int:
    from .server.http import serve

    config = build_config(args)
    host = args.host or config.web_host
    port = args.port or config.web_port
    return serve(config, host=host, port=port)


def cmd_init(args) -> int:
    target = Path(" ".join(args.task).strip() or args.workspace or ".").expanduser().resolve()
    target.mkdir(parents=True, exist_ok=True)
    created: list[str] = []

    rules = target / "AIAGENT.md"
    if not rules.exists():
        rules.write_text(
            "# Инструкции для ИИ-агента\n\n"
            "Этот файл агент читает перед каждой задачей. Опиши здесь:\n\n"
            "- стек и версии (например: Python 3.12, FastAPI, PostgreSQL);\n"
            "- как запускать проект и тесты (например: `make test`);\n"
            "- соглашения по коду и стилю;\n"
            "- чего делать НЕЛЬЗЯ (например: не трогать папку migrations/).\n\n"
            "## Пример\n"
            "- Тесты: `pytest -q`, новые тесты обязательны для нового кода.\n"
            "- Комментарии и документация — на русском.\n",
            encoding="utf-8",
        )
        created.append("AIAGENT.md")

    config_file = target / ".aiagent.json"
    if not config_file.exists():
        config_file.write_text(json.dumps({
            "mode": "auto",
            "max_steps": 60,
            "language": "ru",
            "permissions": {"deny": ["run_shell(rm -rf:*)", "run_shell(git push:*)"]},
        }, ensure_ascii=False, indent=2), encoding="utf-8")
        created.append(".aiagent.json")

    gitignore = target / ".gitignore"
    needed = [".aiagent/", ".aia/", "*.session.json"]
    existing = gitignore.read_text(encoding="utf-8") if gitignore.exists() else ""
    add = [line for line in needed if line not in existing]
    if add:
        with gitignore.open("a", encoding="utf-8") as handle:
            if existing and not existing.endswith("\n"):
                handle.write("\n")
            handle.write("# AI Agent Studio\n" + "\n".join(add) + "\n")
        created.append(".gitignore")

    print(f"Проект подготовлен: {target}")
    for name in created:
        print(f"  + {name}")
    print("\nДальше: aia (интерактивный режим) или aia \"твоя задача\"")
    return 0


def cmd_sessions(args) -> int:
    config = build_config(args)
    sessions = list_sessions(config)
    if not sessions:
        print("Сохранённых сессий нет.")
        return 0
    for session in sessions[:20]:
        print("  " + session.summary())
    print(f"\nКаталог: {project_paths(config.workspace).sessions_dir}")
    print("Продолжить: aia --resume <имя>")
    return 0


def cmd_undo(args) -> int:
    config = build_config(args)
    from .core.undo import CheckpointManager

    manager = CheckpointManager(config)
    index_file = project_paths(config.workspace).checkpoints_dir / "index.json"
    if index_file.is_file():
        try:
            from .core.undo import Checkpoint, FileChange

            data = json.loads(index_file.read_text(encoding="utf-8"))
            for item in data:
                checkpoint = Checkpoint(index=item.get("index", 0), title=item.get("title", ""),
                                        created=item.get("created", 0))
                checkpoint.changes = [FileChange(**change) for change in item.get("changes", [])]
                manager.checkpoints.append(checkpoint)
        except (OSError, json.JSONDecodeError, TypeError) as e:
            print(f"! Не удалось прочитать историю: {e}")

    if not manager.checkpoints:
        print("История изменений пуста — откатывать нечего.")
        return 0

    count = int(args.task[0]) if args.task and args.task[0].isdigit() else 1
    print("История (последние шаги):")
    print(manager.history(10))
    print("\nОткат:")
    print(manager.undo(count))
    return 0


def cmd_mcp(args) -> int:
    from .mcp.registry import describe_servers, load_servers

    config = build_config(args)
    servers = load_servers(config)
    if not servers:
        print("MCP-серверы не настроены.")
        print(f"Создай файл {config.workspace}/.mcp.json или {state_dir()}/mcp.json — пример ниже:\n")
        print(textwrap.dedent("""\
            {
              "mcpServers": {
                "filesystem": {
                  "command": "npx",
                  "args": ["-y", "@modelcontextprotocol/server-filesystem", "/путь/к/папке"]
                },
                "мой-http-сервер": {
                  "url": "http://127.0.0.1:9000/mcp"
                }
              }
            }"""))
        return 0

    print(describe_servers(config))
    if args.show:
        print("\nПодробности:")
        for server in servers:
            print(f"  {server.name}: {server.kind} — {server.source}")
    return 0


def cmd_tools(args) -> int:
    from .tools import build_registry

    registry = build_registry()
    rows = [(tool.name, tool.category, "да" if tool.dangerous else "",
             "да" if tool.writes else "", preview(tool.description, 60))
            for tool in sorted(registry.values(), key=lambda t: (t.category, t.name))]
    print(f"Инструментов: {len(rows)}\n")
    print(text_table(rows, ["имя", "категория", "опасный", "пишет", "назначение"]))
    if args.json:
        print(json.dumps({name: {"description": tool.description,
                                 "parameters": list(tool.parameters),
                                 "required": list(tool.required)}
                          for name, tool in registry.items()}, ensure_ascii=False, indent=2))
    return 0


def cmd_config(args) -> int:
    config = build_config(args)
    path = config_path()

    if args.set:
        data: dict[str, Any] = {}
        if path.is_file():
            try:
                data = json.loads(path.read_text(encoding="utf-8"))
            except json.JSONDecodeError:
                data = {}
        for pair in args.set:
            if "=" not in pair:
                print(f"Пропускаю {pair}: ожидаю key=value")
                continue
            key, value = pair.split("=", 1)
            data[key.strip()] = _coerce(value.strip())
        ensure_state_dir()
        path.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
        print(f"Сохранено в {path}: " + ", ".join(args.set))
        return 0

    if args.show or True:
        print(f"Файл настроек: {path}")
        print(f"Файл проекта:  {project_paths(config.workspace).workspace_config}")
        print()
        print(json.dumps(config.to_dict(), ensure_ascii=False, indent=2))
        print("\nИзменение: aia config --set mode=edits provider=anthropic model=claude-sonnet-4-5-20250929")
    return 0


def _coerce(value: str) -> Any:
    lowered = value.lower()
    if lowered in {"true", "false"}:
        return lowered == "true"
    try:
        return int(value)
    except ValueError:
        pass
    try:
        return float(value)
    except ValueError:
        pass
    return value


# --------------------------------------------------------------------------------------
# Справка
# --------------------------------------------------------------------------------------
def _help_text() -> str:
    providers = ", ".join(sorted(REGISTRY))
    return f"""{__product__} {__version__} — ИИ-агент уровня Claude Code

Использование:
  aia                          интерактивный режим (чат + инструменты)
  aia "создай бота на aiogram"  выполнить задачу и выйти
  aia run "задача"             то же самое, явная форма записи
  aia --provider anthropic     работать на Claude
  aia --provider ollama        работать локально (нужна Ollama)
  aia --provider mock          демо-режим без модели
  aia --mode yolo              без подтверждений (для изолированной среды)

Команды:
  aia doctor                   проверить окружение, ключи, железо, интернет
  aia keys set <провайдер> <ключ>   сохранить ключ API
  aia models                   список доступных моделей
  aia pull [модель]            скачать локальную модель через Ollama
  aia serve                    HTTP API + веб-интерфейс
  aia init [папка]             подготовить проект (AIAGENT.md, настройки)
  aia sessions / aia undo      сессии и откат изменений
  aia mcp                      подключённые MCP-серверы и их инструменты
  aia tools                    список инструментов агента
  aia config --show            настройки (можно менять через --set)

Провайдеры: {providers}
Режимы:    {", ".join(PERMISSION_MODES)}  (по умолчанию auto)
Ключи:     переменные окружения или файл {keys_path()}

В интерактивном режиме набери /help — там команды: /undo, /mode, /model, /context,
/compact, /todo, /notes, /sessions и другие.
"""


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main())
