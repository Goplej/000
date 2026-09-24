"""Навыки: встроенные, пользовательские и проектные."""
from __future__ import annotations

from pathlib import Path

from aiagent import skills
from aiagent.core import Agent
from aiagent.ui import SilentUI


def test_builtin_skills_are_shipped():
    available = skills.discover(".")
    for name in ("debug", "rest-api", "tests", "code-review", "git-workflow",
                 "docker-deploy", "telegram-bot", "web-scraping"):
        assert name in available, f"нет встроенного навыка {name}"
    for skill in available.values():
        assert skill.body.strip(), f"у навыка {skill.name} пустое тело"
        assert skill.description, f"у навыка {skill.name} нет описания"


def test_project_skill_overrides_builtin(workspace: Path, isolated_home: Path):
    directory = workspace / ".aiagent" / "skills"
    directory.mkdir(parents=True)
    (directory / "debug.md").write_text(
        "---\nname: debug\ndescription: Наш собственный порядок отладки\ntags: свои\n---\n\n"
        "1. Сначала смотрим логи сервера.\n", encoding="utf-8")

    skill = skills.load("debug", workspace)
    assert skill is not None
    assert skill.source == "project"
    assert "логи сервера" in skill.body
    assert "Наш собственный" in skill.description


def test_user_skill_directory(workspace: Path, isolated_home: Path):
    directory = isolated_home / "skills"
    directory.mkdir(parents=True)
    (directory / "release.md").write_text(
        "---\nname: release\ndescription: Как выпускать версию\n---\n\nОбнови CHANGELOG.\n",
        encoding="utf-8")

    available = skills.discover(workspace)
    assert "release" in available
    assert available["release"].source == "user"


def test_front_matter_is_optional(workspace: Path, isolated_home: Path):
    directory = workspace / ".aiagent" / "skills"
    directory.mkdir(parents=True)
    (directory / "simple.md").write_text("# Просто навык\n\nДелай раз, два, три.\n",
                                         encoding="utf-8")
    skill = skills.load("simple", workspace)
    assert skill is not None
    assert skill.description.startswith("Просто навык")


def test_catalogue_lists_skills(workspace: Path):
    text = skills.catalogue(workspace)
    assert "ДОСТУПНЫЕ НАВЫКИ" in text
    assert "read_skill" in text


def test_prompt_contains_skill_catalogue(config, workspace: Path):
    agent = Agent(config, ui=SilentUI())
    assert "ДОСТУПНЫЕ НАВЫКИ" in agent.system_preview()


def test_unknown_skill_returns_none(workspace: Path):
    assert skills.load("выдуманный-навык", workspace) is None
