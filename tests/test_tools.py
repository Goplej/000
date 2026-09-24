"""Инструменты: файлы, безопасность путей, поиск, правки, память, патчи."""
from __future__ import annotations

from pathlib import Path

from aiagent.core import Agent
from aiagent.tools import READONLY_TOOL_NAMES, build_registry


def run(agent: Agent, name: str, arguments: dict):
    return agent.tool_map[name].run(arguments, agent.ctx)


# --------------------------------------------------------------------------------- файлы
def test_registry_has_expected_tools():
    registry = build_registry()
    assert len(registry) >= 35
    for name in ("read_file", "write_file", "edit_file", "multi_edit", "list_dir", "glob_search",
                 "grep_search", "run_shell", "web_search", "fetch_url", "todo_write", "task",
                 "finish", "apply_patch", "memory_write", "skills_list", "read_skill"):
        assert name in registry, f"инструмент {name} не зарегистрирован"
    for name in READONLY_TOOL_NAMES:
        assert name in registry or name == "finish", f"{name} объявлен только для чтения, но его нет"


def test_write_read_edit_cycle(agent: Agent, workspace: Path):
    result = run(agent, "write_file", {"path": "hello.py", "content": "print('привет')\n"})
    assert result.ok, result.content
    assert (workspace / "hello.py").is_file()

    reading = run(agent, "read_file", {"path": "hello.py"})
    assert reading.ok
    assert "привет" in reading.content
    assert "1|" in reading.content or "1\t" in reading.content  # нумерация строк

    edited = run(agent, "edit_file", {"path": "hello.py", "old_text": "привет", "new_text": "мир"})
    assert edited.ok, edited.content
    assert "мир" in (workspace / "hello.py").read_text(encoding="utf-8")


def test_write_creates_directories(agent: Agent, workspace: Path):
    result = run(agent, "write_file", {"path": "src/pkg/mod.py", "content": "x = 1\n"})
    assert result.ok
    assert (workspace / "src/pkg/mod.py").is_file()


def test_edit_requires_existing_fragment(agent: Agent):
    run(agent, "write_file", {"path": "a.txt", "content": "один\nдва\n"})
    result = run(agent, "edit_file", {"path": "a.txt", "old_text": "три", "new_text": "четыре"})
    assert not result.ok
    assert "не найден" in result.content.lower() or "не найдено" in result.content.lower()


def test_multi_edit_applies_all(agent: Agent, workspace: Path):
    run(agent, "write_file", {"path": "multi.txt", "content": "A\nB\nC\n"})
    result = run(agent, "multi_edit", {"path": "multi.txt", "edits": [
        {"old_text": "A", "new_text": "1"},
        {"old_text": "C", "new_text": "3"},
    ]})
    assert result.ok, result.content
    assert (workspace / "multi.txt").read_text(encoding="utf-8") == "1\nB\n3\n"


def test_safe_path_blocks_escape(agent: Agent):
    result = run(agent, "read_file", {"path": "../../etc/passwd"})
    assert not result.ok
    assert "вне" in result.content.lower() or "запрещ" in result.content.lower() \
        or "не разреш" in result.content.lower()


def test_glob_and_grep(agent: Agent, workspace: Path):
    (workspace / "sub").mkdir()
    (workspace / "sub" / "one.py").write_text("def find_me():\n    pass\n", encoding="utf-8")
    (workspace / "two.txt").write_text("find_me в тексте\n", encoding="utf-8")

    globbed = run(agent, "glob_search", {"pattern": "**/*.py"})
    assert globbed.ok and "one.py" in globbed.content

    found = run(agent, "grep_search", {"pattern": "find_me", "file_glob": "*.py"})
    assert found.ok and "one.py" in found.content
    assert "two.txt" not in found.content


def test_file_info_and_delete(agent: Agent, workspace: Path):
    run(agent, "write_file", {"path": "temp.txt", "content": "данные\n"})
    info = run(agent, "file_info", {"path": "temp.txt"})
    assert info.ok and "строк" in info.content.lower()

    removed = run(agent, "delete_file", {"path": "temp.txt"})
    assert removed.ok
    assert not (workspace / "temp.txt").exists()


# ----------------------------------------------------------------------------------- прочее
def test_todo_roundtrip(agent: Agent, workspace: Path):
    written = run(agent, "todo_write", {"items": ["[ ] сделать раз", "[ ] сделать два"]})
    assert written.ok
    todo_file = agent.paths.todo_file
    assert todo_file.is_file()
    assert "сделать раз" in todo_file.read_text(encoding="utf-8")

    read = run(agent, "todo_read", {})
    assert read.ok and "сделать два" in read.content


def test_memory_and_rules(agent: Agent, workspace: Path):
    assert run(agent, "memory_write", {"content": "## Решение\nИспользуем pytest"}).ok
    memory = agent.paths.memory_file.read_text(encoding="utf-8")
    assert "pytest" in memory
    assert run(agent, "memory_read", {}).ok

    assert run(agent, "project_rules_write", {"content": "Всегда пиши тесты."}).ok
    rules = (workspace / "AIAGENT.md").read_text(encoding="utf-8")
    assert "тесты" in rules
    assert run(agent, "project_rules_read", {}).ok


def test_apply_patch(agent: Agent, workspace: Path):
    (workspace / "code.py").write_text("def f():\n    return 1\n", encoding="utf-8")
    patch = "<<<<<<< SEARCH\n    return 1\n=======\n    return 42\n>>>>>>> REPLACE"
    result = run(agent, "apply_patch", {"path": "code.py", "patch": patch})
    assert result.ok, result.content
    assert "return 42" in (workspace / "code.py").read_text(encoding="utf-8")


def test_apply_patch_reports_missing_fragment(agent: Agent, workspace: Path):
    (workspace / "code.py").write_text("x = 1\n", encoding="utf-8")
    patch = "<<<<<<< SEARCH\nнет такого\n=======\nесть\n>>>>>>> REPLACE"
    result = run(agent, "apply_patch", {"path": "code.py", "patch": patch})
    assert not result.ok
    assert "не найден" in result.content.lower()


def test_preview_patch_does_not_change_file(agent: Agent, workspace: Path):
    path = workspace / "p.py"
    path.write_text("value = 1\n", encoding="utf-8")
    result = run(agent, "preview_patch", {"path": "p.py", "patch":
                                          "<<<<<<< SEARCH\nvalue = 1\n=======\nvalue = 2\n>>>>>>> REPLACE"})
    assert result.ok
    assert path.read_text(encoding="utf-8") == "value = 1\n"


def test_replace_in_files(agent: Agent, workspace: Path):
    (workspace / "a.py").write_text("old_name = 1\n", encoding="utf-8")
    (workspace / "b.py").write_text("print('old_name')\n", encoding="utf-8")
    result = run(agent, "replace_in_files", {"find": "old_name", "replace": "new_name",
                                             "file_glob": "*.py"})
    assert result.ok
    assert "new_name" in (workspace / "a.py").read_text(encoding="utf-8")
    assert "new_name" in (workspace / "b.py").read_text(encoding="utf-8")


def test_skills_tools(agent: Agent):
    listing = run(agent, "skills_list", {})
    assert listing.ok and "debug" in listing.content

    skill = run(agent, "read_skill", {"name": "debug"})
    assert skill.ok and "Отладка" in skill.content

    missing = run(agent, "read_skill", {"name": "такого-нет"})
    assert not missing.ok


def test_unknown_tool_message(agent: Agent):
    registry = build_registry()
    assert "неизвестный" not in registry
