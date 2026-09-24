"""Ядро агента: цикл «запрос → инструменты → наблюдения → ответ».

Здесь сходится всё: провайдер модели, инструменты, разрешения, контекст,
чекпоинты, хуки, события для интерфейса и под-агенты.
"""
from __future__ import annotations

import json
import time
from collections.abc import Callable, Iterator
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Any

from ..config import AgentConfig
from ..errors import AgentError, ProviderError
from ..paths import project_paths
from ..prompts import build_system_prompt, minimal_prompt, nudge_text
from ..providers import create_provider
from ..providers.base import BaseProvider, ChatRequest, StreamEvent
from ..tools import (
    MINIMAL_TOOL_NAMES,
    READONLY_TOOL_NAMES,
    Tool,
    ToolContext,
    ToolResult,
    build_registry,
    select_tools,
)
from ..utils.logging import get_logger
from ..utils.text import clip, preview
from .context import ContextManager
from .hooks import HookRunner
from .parser import extract_thinking, parse_reply
from .types import AgentEvent, Message, ToolCall, TurnResult, Usage
from .undo import CheckpointManager

log = get_logger("agent")

MAX_NUDGES = 2
SMALL_MODEL_RATIO = 1.15          # во сколько раз слабее модель — настолько проще промпт


class Agent:
    """Автономный агент. Один агент = один проект + одна модель."""

    def __init__(
        self,
        config: AgentConfig,
        *,
        provider: BaseProvider | None = None,
        ui: Any = None,
        confirm: Callable[[str, str], bool] | None = None,
        workspace: str | Path | None = None,
        extra_instructions: str = "",
        is_subagent: bool = False,
        depth: int = 0,
    ) -> None:
        self.config = config
        self.workspace = Path(workspace or config.workspace).resolve()
        self.ui = ui
        self.confirm_override = confirm
        self.provider = provider or create_provider(config)
        self.paths = project_paths(self.workspace)
        self.depth = depth
        self.is_subagent = is_subagent

        self.registry = build_registry()
        self.mcp_clients: list[Any] = []
        self.mcp_notes: list[str] = []
        self._load_mcp_tools()
        self.tools = self._choose_tools()
        self.tool_map = {tool.name: tool for tool in self.tools}

        self.checkpoints = CheckpointManager(config, self.workspace)
        self.hooks = HookRunner(config)
        from .permissions import PermissionManager
        self.permissions = PermissionManager(config)
        self.ctx = ToolContext(
            config=config, workspace=self.workspace, permissions=self.permissions,
            ui=_ConfirmProxy(ui, confirm), checkpoints=self.checkpoints, hooks=self.hooks,
        )
        self.ctx.agent = self if not is_subagent else None

        self.native_tools = bool(
            self.provider.supports_tools(config.resolved_model()) and not self._small_model()
        )
        self.context = ContextManager(config)
        self.system_prompt = ""
        self._stopped = False
        self._session_started = time.time()
        self._extra_instructions = extra_instructions
        self._refresh_prompt()

    # ------------------------------------------------------------------ подготовка
    def _small_model(self) -> bool:
        """Слабые локальные модели: упрощаем промпт и набор инструментов."""
        model = (self.config.resolved_model() or "").lower()
        if any(marker in model for marker in (":1.5b", ":1b", ":0.5b", ":2b", ":3b", "3b-instruct")):
            return True
        return bool(self.config.base_url and "127.0.0.1" in self.config.base_url
                    and any(marker in model for marker in ("tiny", "mini", "small")))

    def _load_mcp_tools(self) -> None:
        """Подключает инструменты внешних MCP-серверов (если настроены)."""
        if not self.config.mcp_enabled or self.is_subagent:
            return
        try:
            from ..mcp.registry import mcp_tools
            extra, notes = mcp_tools(self.config)
        except Exception as e:  # noqa: BLE001 — MCP не должен мешать работе
            log.warning("MCP недоступен: %s", e)
            return
        for tool in extra:
            self.registry[tool.name] = tool
        self.mcp_notes = notes
        if extra and self.config.verbose:
            log.info("Подключены MCP-инструменты: %s", ", ".join(tool.name for tool in extra))

    def _choose_tools(self) -> list[Tool]:
        if self.config.mode == "plan":
            return select_tools(self.registry, list(READONLY_TOOL_NAMES), self.config.disabled_tools)
        if self.is_subagent:
            return select_tools(self.registry, ["read_file", "write_file", "edit_file",
                                                "list_dir", "glob_search", "grep_search",
                                                "run_shell", "web_search", "fetch_url", "finish"],
                                self.config.disabled_tools)
        if self._small_model() and not self.config.enabled_tools:
            return select_tools(self.registry, list(MINIMAL_TOOL_NAMES), self.config.disabled_tools)
        return select_tools(self.registry, self.config.enabled_tools or None,
                            self.config.disabled_tools)

    def _refresh_prompt(self) -> None:
        mcp_block = ""
        if getattr(self, "mcp_notes", None):
            mcp_block = ("ПОДКЛЮЧЁННЫЕ MCP-СЕРВЕРЫ (внешние инструменты):\n"
                         + "\n".join(self.mcp_notes))
        if self._small_model():
            self.system_prompt = minimal_prompt(self.config, self.tools)
            self.native_tools = False
            return
        self.system_prompt = build_system_prompt(
            self.config, self.tools, self.workspace,
            native_tools=self.native_tools,
            rules_files=self.paths.rules_files,
            memory_file=self.paths.memory_file,
            todo_file=self.paths.todo_file,
            extra="\n\n".join(part for part in (self._extra_instructions, mcp_block) if part),
        )

    @property
    def todo_file(self) -> Path:
        return self.paths.todo_file

    @property
    def memory_file(self) -> Path:
        return self.paths.memory_file

    # ------------------------------------------------------------------- интерфейс
    def stop(self) -> None:
        self._stopped = True

    def reset(self) -> None:
        self.context.reset()
        self._stopped = False

    def health(self) -> tuple[bool, str]:
        return self.provider.health()

    def stats(self) -> dict[str, Any]:
        context_stats = self.context.stats().to_dict()
        return {
            "provider": self.provider.name,
            "model": self.config.resolved_model(),
            "mode": self.config.mode,
            "tools": [tool.name for tool in self.tools],
            "native_tools": self.native_tools,
            "context": context_stats,
            "checkpoints": len(self.checkpoints.checkpoints),
            "session_seconds": round(time.time() - self._session_started, 1),
        }

    # ------------------------------------------------------------------ основной цикл
    def run(self, user_message: str, *, max_steps: int | None = None) -> Iterator[AgentEvent]:
        """Полный цикл работы над запросом пользователя. Отдаёт события для интерфейса."""
        limit = max_steps or self.config.max_steps
        self._stopped = False
        nudges = 0

        self.context.add_user(user_message)
        yield AgentEvent("start", meta=self.stats())

        step = 0
        total_calls = 0
        total_usage = Usage()
        answer = ""
        stop_reason = "end_turn"
        started = time.time()

        while step < limit:
            if self._stopped:
                stop_reason = "cancelled"
                yield AgentEvent("info", text="Остановлено пользователем.")
                break
            step += 1
            yield AgentEvent("step", meta={"step": step, "limit": limit})

            # 1. Сжатие контекста при необходимости
            if self.context.should_compact():
                if self.config.verbose:
                    yield AgentEvent("info", text="Контекст заполнен — сжимаю историю…")
                summary = self.context.summarize_with_model(self._complete_once) \
                    if self._can_call_model() else self.context.compact()
                if summary:
                    yield AgentEvent("compact", text=clip(summary, 2000),
                                     meta={"step": step})

            # 2. Запрос к модели
            text_parts: list[str] = []
            calls: list[ToolCall] = []
            usage = Usage()
            error: str | None = None

            try:
                for event in self._stream_model():
                    if event.kind == "text":
                        text_parts.append(event.text)
                        yield AgentEvent("text", text=event.text)
                    elif event.kind == "thinking":
                        yield AgentEvent("thinking", text=event.text)
                    elif event.kind == "tool_call" and event.tool_call is not None:
                        calls.append(event.tool_call)
                    elif event.kind == "usage" and event.usage is not None:
                        usage.add(event.usage)
                    elif event.kind == "meta" and event.meta:
                        yield AgentEvent("info", meta=event.meta)
                    elif event.kind == "error":
                        error = event.error
            except ProviderError as e:
                error = e.human()
            except AgentError as e:
                error = e.human()
            except KeyboardInterrupt:
                stop_reason = "cancelled"
                yield AgentEvent("warn", text="Прервано пользователем (Ctrl+C).")
                break

            total_usage.add(usage)

            if error:
                yield AgentEvent("error", text=error)
                stop_reason = "error"
                break

            raw_text = "".join(text_parts)
            if raw_text and "<think" in raw_text.lower():
                thinking, clean = extract_thinking(raw_text)
                if thinking:
                    yield AgentEvent("thinking", text=thinking)
                    raw_text = clean
            if not self.native_tools:
                parsed = parse_reply(raw_text, set(self.tool_map))
                if parsed.calls:
                    calls = parsed.calls
                visible_text = parsed.text
            else:
                visible_text = raw_text

            # 3. Инструменты
            if calls:
                self.context.add_assistant(visible_text, calls)
                if visible_text.strip():
                    yield AgentEvent("text", text="")  # разделитель для интерфейса
                total_calls += len(calls)
                finished, finish_text = yield from self._execute_calls(calls)
                if finished:
                    answer = finish_text or answer
                    stop_reason = "finish"
                    break
                continue

            # 4. Ответ без инструментов
            text = visible_text.strip()
            if text:
                answer = text
                self.context.add_assistant(text)

            if self._should_nudge(user_message, text, total_calls, nudges):
                nudges += 1
                yield AgentEvent("info", text="Модель отвечает текстом вместо действий — прошу выполнить задачу.")
                self.context.add_user(nudge_text(self.tools))
                continue

            stop_reason = "end_turn"
            break
        else:
            stop_reason = "max_steps"
            yield AgentEvent("warn", text=(
                f"Достигнут лимит шагов ({limit}). Скажи «продолжи» или увеличь --max-steps."
            ))

        # Итоговый отчёт
        usage_line = total_usage.summary() if total_usage.total else ""
        result = TurnResult(
            answer=answer, steps=step, tool_calls=total_calls, usage=total_usage,
            stop_reason=stop_reason, duration=time.time() - started,
            files_changed=list(self.context.files_touched),
        )
        if self.hooks.enabled:
            self.hooks.trigger("stop", {"stop_reason": stop_reason, "steps": step,
                                        "files": result.files_changed})
        yield AgentEvent("done", text=answer, meta={
            **result.to_dict(), "usage_line": usage_line,
            "context": self.context.stats().to_dict(),
        })

    def chat(self, user_message: str, **kwargs: Any) -> TurnResult:
        """Синхронный вариант: выполняет задачу и возвращает итог."""
        answer = ""
        meta: dict[str, Any] = {}
        for event in self.run(user_message, **kwargs):
            if event.type == "text":
                answer += event.text
            elif event.type == "done":
                meta = event.meta or {}
        return TurnResult(
            answer=meta.get("answer") or answer,
            steps=int(meta.get("steps") or 0),
            tool_calls=int(meta.get("tool_calls") or 0),
            stop_reason=str(meta.get("stop_reason") or "end_turn"),
            duration=float(meta.get("duration") or 0.0),
            files_changed=list(meta.get("files_changed") or []),
        )

    # ------------------------------------------------------------------ модель
    def _stream_model(self) -> Iterator[StreamEvent]:
        request = ChatRequest(
            messages=list(self.context.messages),
            system=self.system_prompt,
            tools=[tool.schema() for tool in self.tools] if self.native_tools else [],
            model=self.config.resolved_model(),
            max_tokens=self.config.max_tokens,
            temperature=self.config.temperature,
            stream=self.config.stream,
            thinking_budget=self.config.thinking_budget,
            reasoning_effort=self.config.reasoning_effort,
        )
        yield from self.provider.stream(request)

    def _can_call_model(self) -> bool:
        return self.provider.capabilities.streaming

    def _complete_once(self, messages: list[Message], prompt: str) -> str:
        """Один вызов модели без инструментов — для сводок и служебных текстов."""
        request = ChatRequest(
            messages=[*messages, Message.user(prompt)],
            system="Ты — аккуратный технический секретарь. Пиши кратко и по фактам.",
            model=self.config.resolved_model(),
            max_tokens=min(self.config.max_tokens, 2000),
            temperature=0.0,
            stream=False,
        )
        parts = []
        for event in self.provider.stream(request):
            if event.kind == "text":
                parts.append(event.text)
        return "".join(parts)

    # ------------------------------------------------------------------ инструменты
    def _execute_calls(self, calls: list[ToolCall]
                       ) -> Iterator[AgentEvent | tuple]:
        """Выполняет вызовы: независимые read-only — параллельно, остальные по порядку."""
        executable = [call for call in calls if call.name in self.tool_map]
        unknown = [call for call in calls if call.name not in self.tool_map]
        finished = False
        finish_text = ""

        for call in unknown:
            result = ToolResult.error(
                f"инструмент «{call.name}» не существует. Доступные: {', '.join(self.tool_map)}",
                unknown_tool=True,
            )
            yield AgentEvent("tool_start", name=call.name, call=call)
            yield AgentEvent("tool_end", name=call.name, call=call, ok=False,
                             display=f"неизвестный инструмент {call.name}", content=result.content)
            self.context.add_tool_result(call, result.to_message(self.config.tool_result_max_chars),
                                         is_error=True)

        # finish обрабатываем отдельно и сразу
        for call in executable:
            if call.name == "finish":
                yield AgentEvent("tool_start", name="finish", call=call)
                summary = str(call.arguments.get("summary") or call.arguments.get("message")
                              or "Задача выполнена.")
                files = call.arguments.get("files") or self.context.files_touched
                if files:
                    summary += "\nИзменённые файлы: " + ", ".join(str(f) for f in files)
                yield AgentEvent("tool_end", name="finish", call=call, ok=True,
                                 display="finish", content=summary)
                return True, summary

        parallel = self._can_run_parallel(executable)
        if parallel and len(executable) > 1:
            yield AgentEvent("info", text=f"Выполняю {len(executable)} инструментов параллельно…")
            with ThreadPoolExecutor(max_workers=min(self.config.max_parallel_tools, len(executable))) as pool:
                futures = []
                for call in executable:
                    yield AgentEvent("tool_start", name=call.name, call=call)
                    futures.append((call, pool.submit(self._run_tool, call)))
                for call, future in futures:
                    result = future.result()
                    yield AgentEvent("tool_end", name=call.name, call=call, ok=result.ok,
                                     display=result.display, content=result.content,
                                     meta={"seconds": result.seconds})
                    self.context.add_tool_result(
                        call, result.to_message(self.config.tool_result_max_chars),
                        is_error=not result.ok,
                    )
        else:
            for call in executable:
                yield AgentEvent("tool_start", name=call.name, call=call)
                result = self._run_tool(call)
                yield AgentEvent("tool_end", name=call.name, call=call, ok=result.ok,
                                 display=result.display, content=result.content,
                                 meta={"seconds": result.seconds})
                self.context.add_tool_result(
                    call, result.to_message(self.config.tool_result_max_chars),
                    is_error=not result.ok,
                )

        return finished, finish_text

    def _run_tool(self, call: ToolCall) -> ToolResult:
        tool = self.tool_map.get(call.name)
        if tool is None:
            return ToolResult.error(f"инструмент «{call.name}» недоступен")
        log.debug("Вызов %s(%s)", call.name, preview(json.dumps(call.arguments, ensure_ascii=False), 200))
        return tool.run(call.arguments, self.ctx)

    def _can_run_parallel(self, calls: list[ToolCall]) -> bool:
        if len(calls) < 2:
            return False
        if any(self.tool_map.get(call.name) is None for call in calls):
            return False
        # Пишущие и требующие подтверждения инструменты — только последовательно
        for call in calls:
            tool = self.tool_map[call.name]
            if tool.writes or tool.dangerous:
                return False
            if call.name in {"task", "ask_user", "finish"}:
                return False
        return all(call.name in READONLY_TOOL_NAMES for call in calls)

    # ------------------------------------------------------------------ эвристики
    def _should_nudge(self, user_input: str, text: str, tool_calls: int, nudges: int) -> bool:
        if nudges >= MAX_NUDGES or tool_calls > 0:
            return False
        if self._small_model():
            return False  # слабые модели путаются от лишних напоминаний
        if len(text) > 1200:
            return False
        haystack = (user_input or "").lower()
        action_words = (
            "создай", "сделай", "напиши", "добавь", "исправь", "почини", "удали", "перепиши",
            "запусти", "проверь", "найди", "поищи", "установи", "реализуй", "отрефакторь",
            "create", "write", "implement", "fix", "run", "add", "remove", "build", "refactor",
            "edit", "delete", "search", "find", "test",
        )
        if not any(word in haystack for word in action_words):
            return False
        lazy_markers = (
            "вот код", "here is the code", "```", "я бы ", "i would", "можно сделать так",
            "предлагаю", "вы можете", "you can", "вот пример", "например, так", "следует",
        )
        return any(marker in text.lower() for marker in lazy_markers) or len(text) < 60

    # ------------------------------------------------------------------ прочее
    def describe(self) -> str:
        return (f"{self.config.resolved_model()} · {self.provider.display_name} · "
                f"режим {self.config.mode} · инструментов {len(self.tools)}")

    def system_preview(self) -> str:
        return clip(self.system_prompt, 6000)

    def dump_session(self) -> dict[str, Any]:
        return {
            "provider": self.provider.name,
            "model": self.config.resolved_model(),
            "workspace": str(self.workspace),
            "started": self._session_started,
            "messages": self.context.dump(),
            "files": self.context.files_touched,
            "compactions": self.context.compactions,
        }

    def load_session(self, data: dict[str, Any]) -> None:
        self.context.load(data.get("messages") or [])
        self.context.files_touched = list(data.get("files") or [])


class _ConfirmProxy:
    """Прокси интерфейса, который умеет и подтверждать, и задавать вопросы."""

    def __init__(self, ui: Any, confirm: Callable[[str, str], bool] | None) -> None:
        self._ui = ui
        self._confirm = confirm

    def confirm(self, title: str, description: str) -> bool:
        if self._confirm is not None:
            return bool(self._confirm(title, description))
        if self._ui is not None and hasattr(self._ui, "confirm"):
            return bool(self._ui.confirm(title, description))
        return False

    def ask(self, prompt: str) -> str:
        if self._ui is not None and hasattr(self._ui, "ask"):
            return str(self._ui.ask(prompt))
        return ""

    def info(self, message: str) -> None:
        if self._ui is not None and hasattr(self._ui, "info"):
            self._ui.info(message)

    def warn(self, message: str) -> None:
        if self._ui is not None and hasattr(self._ui, "warn"):
            self._ui.warn(message)

    def error(self, message: str) -> None:
        if self._ui is not None and hasattr(self._ui, "error"):
            self._ui.error(message)


__all__ = ["Agent", "MAX_NUDGES"]
