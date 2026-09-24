"""Системные подсказки агента: главный «характер» и правила поведения.

Промпт собирается из блоков и адаптируется под ситуацию: режим разрешений,
доступные инструменты, платформу, наличие git, инструкции проекта, наличие
нативного tool calling у модели.
"""
from __future__ import annotations

import platform
import subprocess
import time
from pathlib import Path

from .config import AgentConfig
from .tools import Tool, tools_prompt

LANGUAGES = {
    "ru": "русском", "en": "English", "uk": "українській", "de": "Deutsch",
    "es": "español", "fr": "français", "zh": "中文",
}

IDENTITY = """Ты — {product}, автономный ИИ-агент уровня Claude Code, работающий прямо на компьютере пользователя.

Ты не чат-бот: у тебя есть инструменты, и ты обязан ими пользоваться, а не рассказывать, что нужно сделать.
Пользователь доверил тебе свой проект — работай как опытный senior-разработчик: аккуратно, проверяя результат.
"""

RULES = """ПРАВИЛА РАБОТЫ:
1. ДЕЙСТВУЙ, а не описывай. Нужен файл — создай его инструментом write_file. Нужно проверить — запусти run_shell.
2. Перед правкой файла СНАЧАЛА прочитай его (read_file). Никогда не угадывай содержимое.
3. Для правок используй точечный edit_file (или multi_edit/apply_patch) — не перезаписывай файл целиком без причины.
4. Ищи перед тем, как спрашивать: grep_search/glob_search по проекту, затем web_search в интернете. Пользователя беспокоят только те вопросы, на которые нельзя ответить самому.
5. Проверяй свою работу: запусти тесты или хотя бы сам скрипт. Если тесты падают — исправь, а не сообщай об успехе.
6. Длинную задачу разбей на шаги и веди план через todo_write. Отмечай выполненные пункты через update_plan.
7. Сложные независимые подзадачи делегируй под-агентам (task) — они работают параллельно и не засоряют твой контекст.
8. Не выдумывай пути, команды и факты. Не знаешь — проверь инструментом.
9. Ошибку инструмента читай внимательно и исправляй причину; не повторяй один и тот же вызов дважды.
10. Когда задача выполнена — вызови finish с коротким итогом: что сделано, какие файлы созданы/изменены, что проверено.
11. Отвечай кратко и по делу, на {language} языке. Без извинений, без воды, без пересказа задания.
12. Никогда не выводи секреты (ключи, пароли, токены) в ответах и не коммить их в файлы.
"""

STYLE = """СТИЛЬ ОБЩЕНИЯ:
- Пиши так, как пишет инженер коллеге: коротко, по существу, с конкретикой (пути файлов, команды, результаты).
- Не хвали себя и не подводи итоги после каждого шага — только финальный отчёт по завершении.
- Если пользователь просит объяснить — объясняй понятно и с примерами из его проекта.
- Если задача неоднозначна — сделай разумное предположение, отметь его одной строкой и продолжай.
"""

TOOL_PROTOCOL_TEXT = """ФОРМАТ ВЫЗОВА ИНСТРУМЕНТОВ:
Вызывай инструмент отдельным блоком, ровно так:

<tool_call>{"name": "имя_инструмента", "arguments": {"параметр": "значение"}}</tool_call>

Требования:
- JSON строго валидный: двойные кавычки, без комментариев, без переносов внутри строк.
- Можно вызвать несколько инструментов в одном ответе — каждый своим блоком.
- Не пиши несколько разных вызовов в одном JSON-объекте.
- Если действия не нужны (простой вопрос) — просто отвечай текстом без блоков.
- Результат придёт тебе как «[результат ok] …» — используй его в следующем шаге.
"""

TOOL_PROTOCOL_NATIVE = """ИНСТРУМЕНТЫ:
У тебя есть инструменты с описанными параметрами — вызывай их через механизм инструментов,
а не текстом. Если задача требует нескольких шагов, делай их последовательно, анализируя
результат каждого. Не выводи JSON вызова инструмента в обычном тексте ответа.
"""


def git_context(workspace: Path) -> str:
    """Краткий контекст репозитория: ветка, последние коммиты, изменения."""
    if not (workspace / ".git").exists():
        return ""
    try:
        branch = subprocess.run(
            ["git", "rev-parse", "--abbrev-ref", "HEAD"], cwd=str(workspace),
            capture_output=True, text=True, timeout=5, check=False,
        ).stdout.strip()
        status = subprocess.run(
            ["git", "status", "--short"], cwd=str(workspace),
            capture_output=True, text=True, timeout=5, check=False,
        ).stdout.strip()
        log = subprocess.run(
            ["git", "log", "--oneline", "-5"], cwd=str(workspace),
            capture_output=True, text=True, timeout=5, check=False,
        ).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return ""

    parts = [f"- git-ветка: {branch or 'неизвестно'}"]
    if log:
        parts.append("- последние коммиты:\n" + "\n".join(f"  {line}" for line in log.splitlines()))
    if status:
        lines = status.splitlines()
        shown = lines[:20]
        parts.append("- незакоммиченные изменения:\n" + "\n".join(f"  {line}" for line in shown)
                     + (f"\n  … ещё {len(lines) - len(shown)}" if len(lines) > len(shown) else ""))
    else:
        parts.append("- рабочее дерево чистое")
    return "\n".join(parts)


def project_instructions(workspace: Path, rules_files: list[Path]) -> str:
    chunks: list[str] = []
    for path in rules_files:
        try:
            if path.is_file():
                body = path.read_text(encoding="utf-8", errors="replace").strip()
                if body:
                    chunks.append(f"### {path.name}\n{body[:6000]}")
        except OSError:
            continue
    if not chunks:
        return ""
    return "ИНСТРУКЦИИ ПРОЕКТА (обязательны к исполнению):\n" + "\n\n".join(chunks)


def memory_block(memory_file: Path) -> str:
    try:
        if memory_file.is_file():
            body = memory_file.read_text(encoding="utf-8", errors="replace").strip()
            if body:
                return "ПАМЯТЬ ПРОЕКТА (записано ранее):\n" + body[:4000]
    except OSError:
        pass
    return ""


def todo_block(todo_file: Path) -> str:
    import json
    try:
        if not todo_file.is_file():
            return ""
        data = json.loads(todo_file.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return ""
    marks = {"done": "[x]", "doing": "[~]", "skipped": "[-]", "pending": "[ ]"}
    items = data.get("items") or []
    if not items:
        return ""
    lines = [f"{marks.get(item.get('status', 'pending'), '[ ]')} {item.get('text', '')}"
             for item in items]
    return "ТЕКУЩИЙ ПЛАН:\n" + "\n".join(lines)


def environment_block(config: AgentConfig, workspace: Path, extra: dict | None = None) -> str:
    from .hardware import detect

    hw = detect()
    lines = [
        "ОКРУЖЕНИЕ:",
        f"- рабочая папка: {workspace}",
        f"- система: {platform.system()} {platform.release()} ({platform.machine()})",
        f"- оболочка: {'powershell/cmd' if platform.system() == 'Windows' else 'bash/sh'}",
        f"- сегодня: {time.strftime('%Y-%m-%d %H:%M')}",
        f"- железо: {hw.summary()}",
        f"- провайдер модели: {config.resolved_provider()} / {config.resolved_model()}",
        f"- режим разрешений: {config.mode}",
    ]
    try:
        from .integrations.project import detect as detect_project

        stack = detect_project(workspace).summary()
        if stack:
            lines.append("ПРОЕКТ:\n" + "\n".join("  " + row for row in stack.splitlines()))
    except Exception:  # noqa: BLE001 — определение проекта не должно мешать работе
        pass

    git = git_context(workspace)
    if git:
        lines.append(git)
    if extra:
        for key, value in extra.items():
            lines.append(f"- {key}: {value}")
    return "\n".join(lines)


def mode_block(config: AgentConfig) -> str:
    from .config import MODE_DESCRIPTIONS

    notes = {
        "ask": "Сейчас режим «ask»: каждое изменение файлов и команд требует подтверждения. "
               "Предлагай действия чётко, чтобы пользователю было понятно, что он разрешает.",
        "edits": "Сейчас режим «edits»: файлы можно менять свободно, команды — с подтверждением.",
        "auto": "Сейчас режим «auto»: работай автономно внутри проекта; опасные команды спросят подтверждение.",
        "plan": "Сейчас режим «plan»: ЗАПРЕЩЕНО менять файлы и запускать команды. Составь подробный план "
                "и покажи его пользователю.",
        "yolo": "Сейчас режим «yolo»: подтверждений нет. Будь особенно осторожен и не делай необратимых вещей.",
    }
    return f"РЕЖИМ РАБОТЫ: {MODE_DESCRIPTIONS.get(config.mode, '')}\n{notes.get(config.mode, '')}"


def build_system_prompt(
    config: AgentConfig,
    tools: list[Tool],
    workspace: Path,
    *,
    native_tools: bool,
    rules_files: list[Path] | None = None,
    memory_file: Path | None = None,
    todo_file: Path | None = None,
    extra: str = "",
) -> str:
    """Собирает полный системный промпт из блоков."""
    from . import __product__

    language = LANGUAGES.get(config.language, config.language)
    blocks = [
        IDENTITY.format(product=f"{__product__} (v1.0)"),
        RULES.format(language=language),
        STYLE,
        mode_block(config),
    ]

    if rules_files:
        instructions = project_instructions(workspace, rules_files)
        if instructions:
            blocks.append(instructions)
    if memory_file is not None:
        memory = memory_block(memory_file)
        if memory:
            blocks.append(memory)
    if todo_file is not None:
        todo = todo_block(todo_file)
        if todo:
            blocks.append(todo)

    try:  # навыки: короткий каталог, подробности — инструментом read_skill
        from .skills import catalogue

        skills_catalogue = catalogue(workspace)
        if skills_catalogue:
            blocks.append(skills_catalogue)
    except Exception:  # noqa: BLE001 — отсутствие навыков не должно мешать работе
        pass

    blocks.append(environment_block(config, workspace))

    if native_tools and tools:
        listing = "\n".join(f"- {tool.name}: {tool.description}" for tool in tools)
        blocks.append(TOOL_PROTOCOL_NATIVE + "\nДоступные инструменты:\n" + listing)
    elif tools:
        blocks.append(TOOL_PROTOCOL_TEXT + "\n" + tools_prompt(tools, examples=4))

    if config.system_prompt_extra:
        blocks.append(config.system_prompt_extra)
    if extra:
        blocks.append(extra)
    return "\n\n".join(block for block in blocks if block.strip())


def minimal_prompt(config: AgentConfig, tools: list[Tool]) -> str:
    """Короткая подсказка для слабых локальных моделей (1.5B–3B)."""
    language = LANGUAGES.get(config.language, config.language)
    listing = "\n".join(f"- {tool.name}({', '.join(tool.parameters)}): {tool.description}"
                        for tool in tools)
    return f"""Ты — ИИ-агент на компьютере пользователя. Папка проекта: {config.workspace}
Отвечай на {language}. Чтобы выполнить действие, напиши РОВНО это и ничего больше:

<tool_call>{{"name": "имя", "arguments": {{"параметр": "значение"}}}}</tool_call>

Инструменты:
{listing}

Примеры:
<tool_call>{{"name": "read_file", "arguments": {{"path": "main.py"}}}}</tool_call>
<tool_call>{{"name": "write_file", "arguments": {{"path": "hello.py", "content": "print('привет')"}}}}</tool_call>

Правила: меняешь файл — вызывай инструмент, а не пиши текст. Закончил — вызови finish.
"""


def nudge_text(tools: list[Tool]) -> str:
    """Напоминание модели, что нужно действовать, а не описывать."""
    names = ", ".join(tool.name for tool in tools[:10])
    sample = tools[0].example() if tools else ""
    return (
        "Ты ответил текстом, но задача требует действий. Не описывай код словами — ВЫПОЛНИ его.\n"
        "Ответь блоком такого вида:\n" + (sample or
        '<tool_call>{"name": "write_file", "arguments": {"path": "файл.py", "content": "..."}}</tool_call>') +
        f"\n\nДоступные инструменты: {names}.\n"
        "Если работа действительно закончена — вызови finish с итогом."
    )


def continuation_text() -> str:
    """Мягкое продолжение после обрыва по лимиту шагов."""
    return ("Продолжи работу с того места, где остановился. Не повторяй уже сделанное, "
            "проверь текущее состояние инструментами и заверши задачу.")


def summarize_prompt() -> str:
    return ("Кратко перескажи пользователю, что было сделано в этой сессии: какие файлы созданы "
            "или изменены (с путями), какие команды запускались и с каким результатом, что осталось.")


__all__ = [
    "LANGUAGES", "build_system_prompt", "continuation_text", "environment_block",
    "git_context", "memory_block", "minimal_prompt", "mode_block", "nudge_text",
    "project_instructions", "summarize_prompt", "todo_block",
]
