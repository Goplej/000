"""Цикл агента целиком: события, вызовы инструментов, нудж, лимиты, откат."""
from __future__ import annotations

from aiagent.config import AgentConfig
from aiagent.core import Agent
from aiagent.core.types import AgentEvent
from aiagent.ui import SilentUI


def collect(agent: Agent, message: str, **kwargs) -> list[AgentEvent]:
    return list(agent.run(message, **kwargs))


def test_agent_creates_file_with_scripted_mock(config, workspace, mock_script, tool_call_script):
    mock_script([
        tool_call_script("write_file", {"path": "new_module.py", "content": "VALUE = 42\n"},
                         text="Создаю файл. "),
        {"text": "Готово: файл создан."},
    ])
    agent = Agent(config, ui=SilentUI())
    result = agent.chat("создай new_module.py")

    assert result.stop_reason in {"end_turn", "finish"}
    assert (workspace / "new_module.py").read_text(encoding="utf-8") == "VALUE = 42\n"
    assert result.tool_calls >= 1
    assert "new_module.py" in result.files_changed or "new_module.py" in str(result.files_changed)


def test_events_sequence(config, workspace, mock_script, tool_call_script):
    mock_script([
        tool_call_script("list_dir", {"path": "."}),
        {"text": "Файлы перечислены."},
    ])
    agent = Agent(config, ui=SilentUI())
    events = collect(agent, "что в проекте?")
    kinds = [event.type for event in events]

    assert kinds[0] == "start"
    assert kinds[-1] == "done"
    assert "tool_start" in kinds and "tool_end" in kinds
    assert any(event.type == "text" and event.text for event in events)


def test_unknown_provider_reports_clearly(workspace):
    """Провайдер, которого нет, должен дать понятную ошибку, а не падать."""
    import pytest

    from aiagent.errors import ConfigError

    with pytest.raises(ConfigError) as error:
        AgentConfig.from_dict({"workspace": str(workspace), "provider": "нет-такого"})
    assert "провайдер" in error.value.human().lower()
    assert "anthropic" in error.value.human().lower()


def test_finish_stops_cycle(config, workspace, mock_script, tool_call_script):
    mock_script([
        tool_call_script("write_file", {"path": "done.txt", "content": "ок\n"}),
        tool_call_script("finish", {"summary": "Задача выполнена полностью"}),
    ])
    agent = Agent(config, ui=SilentUI())
    result = agent.chat("сделай и закончи")
    assert result.stop_reason == "finish"
    assert "выполнена" in result.answer.lower() or "выполнена" in result.answer


def test_max_steps_limit(config, workspace, mock_script, tool_call_script):
    mock_script([tool_call_script("list_dir", {"path": "."})])  # всегда один и тот же шаг
    agent = Agent(config, ui=SilentUI())
    result = agent.chat("крутись", max_steps=2)
    assert result.stop_reason == "max_steps"
    assert result.steps == 2


def test_unknown_tool_result_is_graceful(config, workspace, mock_script):
    mock_script([
        {"text": '<tool_call>{"name": "нет_такого", "arguments": {}}</tool_call>'},
        {"text": "Извините, инструмент недоступен."},
    ])
    agent = Agent(config, ui=SilentUI())
    result = agent.chat("сделай невозможное")
    assert result.stop_reason == "end_turn"
    assert "недоступен" in result.answer.lower() or "нет_такого" in result.answer.lower()


def test_agent_stops_on_command(config, workspace, mock_script, tool_call_script):
    mock_script([tool_call_script("list_dir", {"path": "."})])
    agent = Agent(config, ui=SilentUI())
    events = []
    for event in agent.run("работать", max_steps=6):
        events.append(event)
        if event.type == "step":
            agent.stop()          # имитируем Ctrl+C между шагами
    kinds = [event.type for event in events]
    assert "done" in kinds
    assert any("останов" in (event.text or "").lower() for event in events if event.type == "info")


def test_undo_restores_file(config, workspace, mock_script, tool_call_script):
    path = workspace / "to_change.txt"
    path.write_text("исходный текст\n", encoding="utf-8")

    mock_script([
        tool_call_script("write_file", {"path": "to_change.txt", "content": "новый текст\n"}),
        {"text": "готово"},
    ])
    agent = Agent(config, ui=SilentUI())
    agent.chat("перезапиши файл")
    assert path.read_text(encoding="utf-8") == "новый текст\n"

    answer = agent.checkpoints.undo(1)
    assert path.read_text(encoding="utf-8") == "исходный текст\n"
    assert "to_change.txt" in answer


def test_context_compaction_keeps_recent(config, workspace, mock_script):
    mock_script([{"text": "короткий ответ"}])
    config.context_window = 2000
    config.compact_at_ratio = 0.4
    agent = Agent(config, ui=SilentUI())
    for index in range(12):
        agent.chat(f"вопрос номер {index} " + "текст " * 200, max_steps=1)

    stats = agent.context.stats()
    assert stats.compactions >= 1
    assert len(agent.context.messages) <= 40


def test_health_and_stats(config):
    agent = Agent(config, ui=SilentUI())
    ok, message = agent.health()
    assert ok
    stats = agent.stats()
    assert stats["tools"] and stats["model"] == "mock-1"
