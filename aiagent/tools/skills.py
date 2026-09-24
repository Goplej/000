"""Инструменты навыков: список методик и загрузка нужной по имени.

Навыки — готовые пошаговые инструкции (создание API, отладка, ревью, деплой…).
Агент видит их список в системном промпте и подгружает подробности инструментом
`read_skill` — так контекст не засоряется лишними деталями.
"""
from __future__ import annotations

from typing import Any

from .. import skills as skills_lib
from . import Tool, ToolContext, ToolError, ToolResult


def skills_list(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    available = skills_lib.discover(ctx.workspace)
    if not available:
        return ToolResult.done("Навыков не найдено.", display="skills_list → 0")
    lines = [f"Доступно навыков: {len(available)}", "-" * 50]
    for skill in available.values():
        tags = f"  [{', '.join(skill.tags)}]" if skill.tags else ""
        lines.append(f"{skill.name} ({skill.source})\n    {skill.description}{tags}")
    lines.append("\nПодробности: read_skill name=<имя>")
    return ToolResult.done("\n".join(lines), display=f"skills_list → {len(available)}")


def read_skill(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    name = str(args.get("name") or "").strip()
    if not name:
        raise ToolError("нужен параметр name (список: skills_list)")
    skill = skills_lib.load(name, ctx.workspace)
    if skill is None:
        available = ", ".join(sorted(skills_lib.discover(ctx.workspace))) or "нет"
        raise ToolError(f"навык «{name}» не найден. Доступны: {available}")

    return ToolResult.done(skill.render(), display=f"read_skill {skill.name} ({len(skill.body)} симв.)",
                           name=skill.name, source=skill.source)


TOOLS = [
    Tool(
        name="skills_list",
        description="Список доступных навыков (методик): краткое описание каждой.",
        parameters={},
        handler=skills_list,
        category="skills",
    ),
    Tool(
        name="read_skill",
        description=("Загрузить навык по имени: пошаговая методика решения типовой задачи "
                     "(API, отладка, тесты, ревью, деплой, бот и т.д.)."),
        parameters={"name": {"type": "string", "description": "имя навыка из skills_list"}},
        handler=read_skill,
        required=("name",),
        category="skills",
    ),
]

__all__ = ["TOOLS", "read_skill", "skills_list"]
