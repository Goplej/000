"""Мини-рендер Markdown для терминала (без библиотек) и подсветка кода."""
from __future__ import annotations

import re

from ..utils.ansi import Ansi

KEYWORDS = {
    "py": ("def", "class", "return", "import", "from", "if", "else", "elif", "for", "while",
           "try", "except", "finally", "with", "as", "lambda", "yield", "async", "await",
           "None", "True", "False", "and", "or", "not", "in", "is", "pass", "raise"),
    "js": ("function", "const", "let", "var", "return", "if", "else", "for", "while", "class",
           "import", "export", "from", "async", "await", "try", "catch", "new", "this"),
    "sh": ("cd", "ls", "echo", "export", "if", "then", "fi", "for", "do", "done", "sudo",
           "git", "npm", "pip", "python"),
    "sql": ("select", "from", "where", "insert", "update", "delete", "join", "group", "order"),
}

LANG_BY_EXT = {
    ".py": "py", ".pyw": "py", ".js": "js", ".mjs": "js", ".ts": "js", ".tsx": "js",
    ".jsx": "js", ".sh": "sh", ".bash": "sh", ".zsh": "sh", ".sql": "sql", ".json": "json",
}


def language_for(path: str) -> str:
    for extension, language in LANG_BY_EXT.items():
        if str(path).endswith(extension):
            return language
    return ""


def highlight(code: str, language: str, ansi: Ansi) -> str:
    """Простая подсветка: строки, комментарии, числа, ключевые слова."""
    if not ansi.enabled or language == "json":
        return code

    lines = []
    for line in code.splitlines():
        out = line
        # Комментарии
        out = re.sub(r"(#.*$|//.*$)", lambda m: ansi.dim(m.group(1)), out)
        # Строки
        out = re.sub(r"('([^'\\]|\\.)*'|\"([^\"\\]|\\.)*\")",
                     lambda m: ansi.paint(m.group(0), Ansi.BRIGHT_YELLOW), out)
        # Числа
        out = re.sub(r"\b(\d+(\.\d+)?)\b", lambda m: ansi.paint(m.group(1), Ansi.BRIGHT_CYAN), out)
        # Ключевые слова
        for keyword in KEYWORDS.get(language, ()):
            out = re.sub(rf"\b({re.escape(keyword)})\b",
                         lambda m: ansi.paint(m.group(1), Ansi.BRIGHT_MAGENTA), out)
        lines.append(out)
    return "\n".join(lines)


def markdown_to_terminal(text: str, ansi: Ansi, width: int = 100) -> str:
    """
    Упрощённый Markdown для терминала: заголовки, списки, код, жирный текст.
    Полноценный рендер не нужен — важно, чтобы вывод был читаемым.
    """
    lines: list[str] = []
    in_code = False
    language = ""

    for raw in text.splitlines():
        if raw.strip().startswith("```"):
            if in_code:
                in_code = False
                language = ""
                continue
            in_code = True
            language = raw.strip().strip("`").strip()
            if language:
                lines.append(ansi.dim(f"┄ {language} " + "┄" * max(0, 20 - len(language))))
            continue

        if in_code:
            lines.append("  " + highlight(raw, language, ansi) if ansi.enabled
                         else ansi.dim("  " + raw))
            continue

        line = raw
        if line.startswith("### "):
            line = ansi.heading(line[4:])
        elif line.startswith("## "):
            line = ansi.bold(ansi.heading(line[3:]))
        elif line.startswith("# "):
            line = ansi.bold(ansi.heading(line[2:]))
        else:
            line = re.sub(r"\*\*([^*]+)\*\*", lambda m: ansi.bold(m.group(1)), line)
            line = re.sub(r"`([^`]+)`", lambda m: ansi.paint(m.group(1), Ansi.BRIGHT_CYAN), line)
            line = re.sub(r"^\s*[-*]\s+", lambda m: m.group(0).replace("-", "•").replace("*", "•"), line)
        lines.append(line)

    return "\n".join(lines)


def code_with_line_numbers(code: str, start: int = 1) -> str:
    lines = code.splitlines()
    width = len(str(start + len(lines)))
    return "\n".join(f"{index + start:>{width}}│ {line}" for index, line in enumerate(lines))


__all__ = ["code_with_line_numbers", "highlight", "language_for", "markdown_to_terminal"]
