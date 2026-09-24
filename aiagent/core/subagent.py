"""Под-агенты: делегирование подзадач отдельным агентам.

Каждый под-агент получает чистый контекст, свою роль и ограниченный набор
инструментов. Так основная сессия не переполняется деталями, а несколько
подзадач можно решать параллельно (см. tool `task`).
"""
from __future__ import annotations

import time
from dataclasses import dataclass
from typing import Any

from ..config import AgentConfig
from ..utils.logging import get_logger

log = get_logger("subagent")

ROLE_PROMPTS = {
    "general": (
        "Ты — универсальный исполнитель подзадачи. Сделай работу и верни отчёт: "
        "что сделано, какие файлы затронуты, что проверено."
    ),
    "researcher": (
        "Ты — исследователь. Изучи проект инструментами (grep_search, read_file, list_dir), "
        "при необходимости поищи в интернете (web_search, fetch_url). Файлы не меняй. "
        "Верни структурированный отчёт: факты, пути файлов, цитаты кода, выводы, риски."
    ),
    "coder": (
        "Ты — программист. Реализуй подзадачу в коде: читай существующие файлы, пиши новые, "
        "правь точечно, запускай проверки. В конце верни список изменённых файлов и что именно изменено."
    ),
    "reviewer": (
        "Ты — ревьюер. Найди проблемы в коде: логические ошибки, уязвимости, утечки ресурсов, "
        "нарушения стиля проекта, отсутствие обработки ошибок. Ничего не меняй. "
        "Верни список замечаний с путями и приоритетами (критично/важно/мелочь)."
    ),
    "tester": (
        "Ты — тестировщик. Напиши или запусти тесты, воспроизведи проблему, проверь граничные случаи. "
        "Верни результат прогона и найденные дефекты с шагами воспроизведения."
    ),
    "planner": (
        "Ты — архитектор. Изучи задачу и код, составь пошаговый план реализации с оценкой "
        "сложности и рисками. Ничего не меняй. Верни нумерованный план."
    ),
}

#: Какие инструменты доступны ролям (пусто = общий набор под-агента).
ROLE_TOOLS = {
    "researcher": ["read_file", "list_dir", "glob_search", "grep_search", "web_search",
                   "fetch_url", "wikipedia", "todo_write", "finish"],
    "reviewer": ["read_file", "list_dir", "glob_search", "grep_search", "run_shell", "finish"],
    "planner": ["read_file", "list_dir", "glob_search", "grep_search", "todo_write", "finish"],
}


@dataclass
class SubagentReport:
    agent_type: str
    description: str
    text: str
    steps: int = 0
    tool_calls: int = 0
    seconds: float = 0.0
    ok: bool = True

    def render(self) -> str:
        head = f"[под-агент {self.agent_type}] {self.description[:120]}"
        foot = (f"\n\n(шагов: {self.steps}, вызовов инструментов: {self.tool_calls}, "
                f"время: {self.seconds:.1f} с)")
        return f"{head}\n{'-' * 60}\n{self.text}{foot}"


def run_subagent(
    parent: Any,
    description: str,
    *,
    agent_type: str = "general",
    max_steps: int = 25,
    tools_filter: list[str] | None = None,
    context: str = "",
) -> str:
    """
    Запускает под-агента и возвращает текстовый отчёт (его получит основная модель).
    При любой ошибке возвращает строку с описанием проблемы — агент не падает.
    """
    from .agent import Agent  # локальный импорт разрывает цикл

    if getattr(parent, "depth", 0) >= 1:
        return "[под-агент: ошибка] вложенные под-агенты запрещены — выполни задачу самостоятельно"

    config: AgentConfig = parent.config
    role = ROLE_PROMPTS.get(agent_type, ROLE_PROMPTS["general"])
    instructions = (
        f"{role}\n\nТвоя подзадача: {description}"
        + (f"\n\nКонтекст от основного агента:\n{context}" if context else "")
        + "\n\nНе задавай вопросов пользователю: работай автономно и верни итог одной порцией текста."
    )

    sub_config = AgentConfig.from_dict({**config.to_dict(), "enabled_tools": tools_filter or []})
    sub_config.max_steps = max_steps
    sub_config.verbose = False
    if tools_filter is None:
        sub_config.enabled_tools = list(ROLE_TOOLS.get(agent_type, []))

    started = time.time()
    try:
        agent = Agent(
            sub_config,
            provider=parent.provider,
            ui=None,
            confirm=None,
            workspace=parent.workspace,
            extra_instructions=instructions,
            is_subagent=True,
            depth=getattr(parent, "depth", 0) + 1,
        )
    except Exception as e:  # noqa: BLE001
        log.warning("Не удалось создать под-агента: %s", e)
        return f"[под-агент: ошибка] {type(e).__name__}: {e}"

    steps = 0
    tool_calls = 0
    answer = ""
    try:
        for event in agent.run(description, max_steps=max_steps):
            if event.type == "text":
                answer += event.text
            elif event.type == "done":
                steps = int((event.meta or {}).get("steps") or steps)
                tool_calls = int((event.meta or {}).get("tool_calls") or tool_calls)
                if (event.meta or {}).get("answer"):
                    answer = str(event.meta["answer"])
            elif event.type == "error":
                answer += f"\n[под-агент: ошибка] {event.text}"
    except Exception as e:  # noqa: BLE001
        return f"[под-агент: ошибка] {type(e).__name__}: {e}"

    report = SubagentReport(
        agent_type=agent_type, description=description,
        text=(answer.strip() or "Под-агент не сформулировал отчёт."),
        steps=steps, tool_calls=tool_calls, seconds=time.time() - started,
    )
    return report.render()


__all__ = ["ROLE_PROMPTS", "ROLE_TOOLS", "SubagentReport", "run_subagent"]
