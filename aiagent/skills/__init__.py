"""Навыки (skills): markdown-инструкции, которые агент подгружает по мере надобности.

Навык — это файл `*.md` с YAML-подобной «шапкой» (name/description/tags) и телом:
пошаговая методика решения типовой задачи (создание API, отладка, ревью, релиз…).

Ищутся в трёх местах (все совпадения доступны агенту):
  1. <пакет>/skills/builtin/*.md      — поставляемые навыки;
  2. ~/.aiagent/skills/*.md           — личные навыки пользователя;
  3. <проект>/.aiagent/skills/*.md    — навыки конкретного проекта.
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path

from ..paths import state_dir
from ..utils.logging import get_logger

log = get_logger("skills")

BUILTIN_DIR = Path(__file__).resolve().parent / "builtin"


@dataclass
class Skill:
    name: str
    description: str
    body: str
    path: Path
    tags: list[str] = field(default_factory=list)
    source: str = "builtin"

    def render(self) -> str:
        return (f"# Навык: {self.name}\n{self.description}\n\n{self.body.strip()}\n\n"
                f"(источник: {self.path})")

    def summary_line(self) -> str:
        return f"- {self.name}: {self.description}"


def _parse_front_matter(text: str) -> tuple[dict[str, str], str]:
    """Простейший разбор «шапки» между --- --- (без зависимостей)."""
    if not text.startswith("---"):
        return {}, text
    end = text.find("\n---", 3)
    if end == -1:
        return {}, text
    head, body = text[3:end], text[end + 4:]
    meta: dict[str, str] = {}
    for line in head.splitlines():
        if ":" in line:
            key, value = line.split(":", 1)
            meta[key.strip().lower()] = value.strip().strip('"\'')
    return meta, body.lstrip("\n")


def _load_file(path: Path, source: str) -> Skill | None:
    try:
        raw = path.read_text(encoding="utf-8")
    except OSError as e:
        log.warning("Не удалось прочитать навык %s: %s", path, e)
        return None
    meta, body = _parse_front_matter(raw)
    name = meta.get("name") or path.stem
    description = meta.get("description") or _first_line(body) or name
    tags = [tag.strip() for tag in re.split(r"[,\s]+", meta.get("tags", "")) if tag.strip()]
    return Skill(name=name, description=description, body=body, path=path, tags=tags, source=source)


def _first_line(text: str) -> str:
    for line in text.splitlines():
        clean = line.strip().lstrip("#").strip()
        if clean:
            return clean[:160]
    return ""


def discover(workspace: str | Path = ".") -> dict[str, Skill]:
    """Все доступные навыки; проект и пользователь перекрывают встроенные."""
    skills: dict[str, Skill] = {}
    locations: list[tuple[Path, str]] = [
        (BUILTIN_DIR, "builtin"),
        (state_dir() / "skills", "user"),
        (Path(workspace) / ".aiagent" / "skills", "project"),
    ]
    for directory, source in locations:
        if not directory.is_dir():
            continue
        for path in sorted(directory.glob("*.md")):
            skill = _load_file(path, source)
            if skill:
                skills[skill.name.lower()] = skill
    return skills


def load(name: str, workspace: str | Path = ".") -> Skill | None:
    return discover(workspace).get(name.strip().lower())


def catalogue(workspace: str | Path = ".") -> str:
    """Короткий список навыков — вставляется в системный промпт."""
    skills = discover(workspace)
    if not skills:
        return ""
    lines = ["ДОСТУПНЫЕ НАВЫКИ (подробности — инструментом read_skill):"]
    lines += [skill.summary_line() for skill in skills.values()]
    return "\n".join(lines)


__all__ = ["BUILTIN_DIR", "Skill", "catalogue", "discover", "load"]
