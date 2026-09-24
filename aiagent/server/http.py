"""HTTP API и веб-интерфейс агента (без внешних зависимостей).

Запуск: aia serve [--host 127.0.0.1] [--port 8787]

Основное:
  GET  /                  веб-интерфейс (чат + панель проекта)
  GET  /api/health        состояние провайдера, модели, железа
  GET  /api/config        текущие настройки и список провайдеров
  POST /api/config        изменить настройки
  GET  /api/tools         список инструментов
  GET  /api/sessions      сохранённые сессии
  POST /api/chat          выполнить задачу со стримингом событий (SSE)
  POST /api/stop          остановить работу
  POST /api/undo          откатить изменения
  GET  /api/mcp           состояние MCP-серверов
"""
from __future__ import annotations

import json
import mimetypes
import queue
import threading
import time
import traceback
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any

from .. import __product__, __version__
from ..config import MODE_DESCRIPTIONS, PERMISSION_MODES, AgentConfig
from ..core import Agent
from ..errors import AgentError, AuthError
from ..hardware import detect, recommend
from ..paths import project_paths, web_dir
from ..providers import available_providers
from ..session import list_sessions, load_session, new_session
from ..ui import SilentUI
from ..utils.logging import get_logger

log = get_logger("server")


class ServerState:
    """Общее состояние сервера: конфиг, агент, очередь подтверждений."""

    def __init__(self, config: AgentConfig) -> None:
        self.config = config
        self.lock = threading.Lock()
        self.agent: Agent | None = None
        self.busy = False
        self.pending_confirm: dict[str, Any] = {}
        self.confirm_queue: queue.Queue[tuple[str, bool]] = queue.Queue()
        self.last_events: list[dict[str, Any]] = []
        self.session = new_session(config, None)

    # ------------------------------------------------------------------ агент
    def get_agent(self) -> Agent:
        if self.agent is None:
            self.agent = Agent(self.config, ui=SilentUI())
        return self.agent

    def rebuild_agent(self) -> Agent:
        if self.agent is not None:
            self.agent.stop()
        self.agent = Agent(self.config, ui=SilentUI())
        return self.agent

    def confirm(self, title: str, description: str) -> bool:
        """Ждёт ответа из веб-интерфейса (или отклоняет по таймауту)."""
        token = f"c{int(time.time() * 1000)}"
        self.pending_confirm = {"id": token, "title": title, "description": description}
        log.info("Ожидаю подтверждения: %s", title)
        try:
            while True:
                answer_id, allowed = self.confirm_queue.get(timeout=300)
                if answer_id == token:
                    return allowed
        except queue.Empty:
            self.pending_confirm = {}
            return False
        finally:
            self.pending_confirm = {}

    def answer_confirm(self, token: str, allowed: bool) -> bool:
        self.confirm_queue.put((token, allowed))
        return True


class Handler(BaseHTTPRequestHandler):
    server_version = f"AIAgentStudio/{__version__}"
    protocol_version = "HTTP/1.1"
    state: ServerState  # задаётся при создании класса

    # ------------------------------------------------------------------ служебное
    def log_message(self, fmt: str, *args: Any) -> None:  # noqa: A003
        log.debug("%s - %s", self.address_string(), fmt % args)

    def _headers(self, status: int, content_type: str, length: int) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(length))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.end_headers()

    def _json(self, payload: Any, status: int = 200) -> None:
        body = json.dumps(payload, ensure_ascii=False, default=str).encode("utf-8")
        self._headers(status, "application/json; charset=utf-8", len(body))
        self.wfile.write(body)

    def _read_json(self) -> dict[str, Any]:
        length = int(self.headers.get("Content-Length") or 0)
        if not length:
            return {}
        try:
            return json.loads(self.rfile.read(length).decode("utf-8"))
        except (json.JSONDecodeError, UnicodeDecodeError):
            return {}

    def _static(self, relative: str) -> None:
        root = web_dir()
        target = (root / relative.lstrip("/")).resolve()
        if root not in target.parents and target != root:
            self._json({"error": "недопустимый путь"}, 400)
            return
        if not target.is_file():
            self._json({"error": f"файл не найден: {relative}"}, 404)
            return
        body = target.read_bytes()
        content_type = mimetypes.guess_type(str(target))[0] or "application/octet-stream"
        if target.suffix in {".html", ".js", ".css"}:
            content_type += "; charset=utf-8"
        self._headers(200, content_type, len(body))
        self.wfile.write(body)

    # ------------------------------------------------------------------ маршруты
    def do_OPTIONS(self) -> None:  # noqa: N802
        self._headers(204, "text/plain", 0)

    def do_GET(self) -> None:  # noqa: N802
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        query = urllib.parse.parse_qs(parsed.query)
        _ = query  # зарезервировано под будущие GET-параметры

        try:
            if path in {"/", "/index.html"}:
                self._static("index.html")
            elif path == "/api/health":
                self._health()
            elif path == "/api/config":
                self._json({
                    "config": self.state.config.to_dict(),
                    "modes": MODE_DESCRIPTIONS,
                    "providers": available_providers(),
                    "permission_modes": list(PERMISSION_MODES),
                    "hardware": detect().to_dict(),
                    "recommendation": recommend(detect()),
                })
            elif path == "/api/tools":
                agent = self.state.get_agent()
                self._json({"tools": [
                    {"name": tool.name, "description": tool.description,
                     "category": tool.category, "dangerous": tool.dangerous,
                     "writes": tool.writes, "parameters": list(tool.parameters),
                     "required": list(tool.required)}
                    for tool in agent.tools
                ]})
            elif path == "/api/sessions":
                sessions = list_sessions(self.state.config)
                self._json({"sessions": [
                    {"name": s.name, "messages": len(s.messages), "updated": s.updated,
                     "first": next((m["content"] for m in s.messages if m.get("role") == "user"), "")[:120]}
                    for s in sessions
                ]})
            elif path == "/api/history":
                agent = self.state.get_agent()
                self._json({"history": agent.checkpoints.history(30),
                            "summary": agent.checkpoints.summary()})
            elif path == "/api/mcp":
                from ..mcp.registry import describe_servers
                self._json({"servers": describe_servers(self.state.config)})
            elif path == "/api/pending":
                self._json({"pending": self.state.pending_confirm})
            elif path.startswith("/static/") or path.endswith((".js", ".css", ".png", ".svg",
                                                              ".ico", ".woff2")):
                self._static(path.lstrip("/"))
            else:
                self._json({"error": "маршрут не найден", "path": path}, 404)
        except Exception as e:  # noqa: BLE001
            log.error("GET %s: %s", path, traceback.format_exc(limit=3))
            self._json({"error": f"{type(e).__name__}: {e}"}, 500)

    def do_POST(self) -> None:  # noqa: N802
        path = urllib.parse.urlparse(self.path).path
        data = self._read_json()
        try:
            if path == "/api/chat":
                self._chat(data)
            elif path == "/api/stop":
                if self.state.agent:
                    self.state.agent.stop()
                self._json({"ok": True, "message": "Останавливаю агента"})
            elif path == "/api/undo":
                agent = self.state.get_agent()
                count = int(data.get("count") or 1)
                self._json({"ok": True, "result": agent.checkpoints.undo(count)})
            elif path == "/api/confirm":
                token = str(data.get("id") or (self.state.pending_confirm or {}).get("id") or "")
                allowed = bool(data.get("allow"))
                self._json({"ok": self.state.answer_confirm(token, allowed)})
            elif path == "/api/config":
                self._apply_config(data)
                self._json({"ok": True, "config": self.state.config.to_dict()})
            elif path == "/api/session":
                action = data.get("action")
                if action == "save":
                    self.state.session.meta = {"model": self.state.config.resolved_model()}
                    path_saved = self.state.session.save()
                    self._json({"ok": True, "path": str(path_saved)})
                elif action == "load":
                    session = load_session(self.state.config, str(data.get("name") or ""))
                    self.state.session = session
                    agent = self.state.rebuild_agent()
                    agent.load_session({"messages": session.messages})
                    self._json({"ok": True, "messages": len(session.messages)})
                else:
                    self._json({"error": "неизвестное действие"}, 400)
            elif path == "/api/reset":
                agent = self.state.rebuild_agent()
                agent.reset()
                self._json({"ok": True})
            else:
                self._json({"error": "маршрут не найден"}, 404)
        except Exception as e:  # noqa: BLE001
            log.error("POST %s: %s", path, traceback.format_exc(limit=3))
            self._json({"error": f"{type(e).__name__}: {e}"}, 500)

    # ------------------------------------------------------------------ хендлеры
    def _health(self) -> None:
        agent = self.state.get_agent()
        ok, message = agent.health()
        self._json({
            "ok": ok, "message": message, "version": __version__, "product": __product__,
            "provider": agent.provider.name, "provider_title": agent.provider.display_name,
            "model": self.state.config.resolved_model(),
            "mode": self.state.config.mode,
            "tools": len(agent.tools),
            "workspace": str(agent.workspace),
            "busy": self.state.busy,
        })

    def _apply_config(self, data: dict[str, Any]) -> None:
        config = self.state.config
        changed_model = False
        for key, value in data.items():
            if key in {"provider", "model"} and value:
                setattr(config, key, value)
                changed_model = True
            elif key == "mode" and value in PERMISSION_MODES:
                config.mode = value
                changed_model = True
            elif key in {"max_steps", "temperature", "max_tokens", "allow_web", "allow_shell",
                         "auto_compact", "language", "thinking_budget"}:
                setattr(config, key, value)
        if data.get("save"):
            path = project_paths(config.workspace).workspace_config
            path.write_text(json.dumps(config.to_dict(), ensure_ascii=False, indent=2),
                            encoding="utf-8")
        if changed_model or data.get("rebuild"):
            self.state.rebuild_agent()

    def _chat(self, data: dict[str, Any]) -> None:
        message = str(data.get("message") or "").strip()
        if not message:
            self._json({"error": "пустое сообщение"}, 400)
            return
        if self.state.busy:
            self._json({"error": "агент уже занят другой задачей"}, 409)
            return

        if data.get("mode") in PERMISSION_MODES:
            self.state.config.mode = data["mode"]

        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.send_header("X-Accel-Buffering", "no")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()

        state = self.state
        state.busy = True
        self.state.session.add("user", message)

        def send(event: dict[str, Any]) -> None:
            try:
                payload = f"data: {json.dumps(event, ensure_ascii=False, default=str)}\n\n"
                self.wfile.write(payload.encode("utf-8"))
                self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                if state.agent:
                    state.agent.stop()

        answer = ""
        try:
            agent = state.rebuild_agent() if data.get("rebuild") else state.get_agent()
            send({"type": "start", "model": state.config.resolved_model(),
                  "provider": agent.provider.name, "tools": len(agent.tools)})
            for event in agent.run(message, max_steps=int(data.get("max_steps") or 0) or None):
                payload = event.to_dict()
                payload["pending"] = state.pending_confirm
                if event.type == "text":
                    answer += event.text
                if event.type == "done":
                    payload["answer"] = event.text or answer
                    if answer:
                        state.session.add("assistant", answer)
                    state.session.save()
                send(payload)
        except AuthError as e:
            send({"type": "error", "text": e.human()})
        except AgentError as e:
            send({"type": "error", "text": e.human()})
        except Exception as e:  # noqa: BLE001
            log.error("Ошибка в чате: %s", traceback.format_exc(limit=5))
            send({"type": "error", "text": f"{type(e).__name__}: {e}"})
        finally:
            state.busy = False
            send({"type": "end"})


def create_server(config: AgentConfig, host: str | None = None, port: int | None = None) -> ThreadingHTTPServer:
    state = ServerState(config)
    handler = type("BoundHandler", (Handler,), {"state": state})
    server = ThreadingHTTPServer(
        (host or config.web_host, config.web_port if port is None else port), handler)
    server.daemon_threads = True
    server.state = state  # type: ignore[attr-defined]
    return server


def serve(config: AgentConfig, host: str = "127.0.0.1", port: int = 8787) -> int:
    server = create_server(config, host, port)
    address = f"http://{host}:{server.server_address[1]}"
    print(f"{__product__} — веб-интерфейс: {address}")
    print(f"Проект: {config.workspace}")
    print(f"Провайдер: {config.resolved_provider()} · модель {config.resolved_model()}")
    if not (web_dir() / "index.html").is_file():
        print(f"! Не найден {web_dir() / 'index.html'} — интерфейс не откроется, но API работает.")
    print("Остановить: Ctrl+C")
    try:
        server.serve_forever(poll_interval=0.3)
    except KeyboardInterrupt:
        print("\nОстанавливаю сервер…")
    finally:
        if getattr(server, "state", None) and server.state.agent:
            server.state.agent.stop()
        server.shutdown()
    return 0


def start_background(config: AgentConfig, host: str = "127.0.0.1",
                     port: int = 8787) -> ThreadingHTTPServer:
    """Запускает сервер в фоновом потоке (для тестов и ноутбуков)."""
    server = create_server(config, host, port)
    thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.2}, daemon=True)
    thread.start()
    time.sleep(0.2)
    return server


__all__ = ["Handler", "ServerState", "create_server", "serve", "start_background"]
