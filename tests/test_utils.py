"""Утилиты: терпимый JSON, текст, токены, безопасность, diff, анимация отсутствует."""
from __future__ import annotations

from aiagent.utils.diff import apply_patch_blocks, parse_patch, unified_diff
from aiagent.utils.json_utils import (
    dumps_compact,
    extract_json_after,
    find_balanced,
    loads_lenient,
    parse_json_objects,
)
from aiagent.utils.security import check_command, is_inside, redact, safe_path
from aiagent.utils.text import clip, human_duration, human_size, plural_ru, preview, text_table
from aiagent.utils.tokens import TokenCounter, estimate_tokens


# ------------------------------------------------------------------------------- JSON
def test_loads_lenient_handles_sloppy_json():
    assert loads_lenient("{'name': 'read_file'}") == {"name": "read_file"}
    assert loads_lenient('{"name": "x", /* комментарий */ "args": {}}') == {"name": "x", "args": {}}
    assert loads_lenient('{"a": 1, "b": 2,}') == {"a": 1, "b": 2}
    assert loads_lenient('{"text": "он сказал \\"да\\"", "ok": True}')["ok"] is True
    assert loads_lenient("не json") is None


def test_parse_json_objects_finds_objects_in_text():
    text = 'Начинаю. {"name": "read_file", "arguments": {"path": "a.py"}} Готово.'
    objects = list(parse_json_objects(text))
    assert objects and objects[0][1]["name"] == "read_file"


def test_find_balanced_and_extract():
    text = 'префикс {"a": {"b": [1, 2, "}"]}} суффикс'
    start = text.index("{")
    end = find_balanced(text, start)
    assert end > start and text[start:end + 1].endswith("}}")
    assert extract_json_after('{"arguments": {"path": "a.py"}}', "arguments") == {"path": "a.py"}


def test_dumps_compact_is_valid_json():
    import json

    payload = {"имя": "тест", "числа": [1, 2, 3]}
    assert json.loads(dumps_compact(payload)) == payload


# ------------------------------------------------------------------------------- текст
def test_text_helpers():
    assert len(clip("abcdefghij", 4)) <= 4
    assert "обрезано" in clip("x" * 200, 60)
    assert preview("строка\nвторая", 100).startswith("строка")
    assert human_size(1536).endswith("КБ")
    assert "мин" in human_duration(125) or "с" in human_duration(125)
    assert plural_ru(1, "файл", "файла", "файлов") == "файл"
    assert plural_ru(3, "файл", "файла", "файлов") == "файла"
    assert plural_ru(7, "файл", "файла", "файлов") == "файлов"

    table = text_table([("a", "b"), ("длинное-значение", "c")], ["первая", "вторая"])
    assert "первая" in table and "длинное-значение" in table


# ------------------------------------------------------------------------------- токены
def test_token_counter_reports_fill():
    from aiagent.core.types import Message

    counter = TokenCounter(context_window=1000)
    messages = [Message(role="user", content="привет " * 200)]
    assert counter.count(messages) > 100
    assert counter.fill_ratio(messages) > 0.1
    described = counter.describe(messages)
    assert "%" in described and "токен" in described.lower()
    assert counter.needs_compaction(messages, ratio=0.05)
    assert estimate_tokens("") == 0


# ----------------------------------------------------------------------------- безопасность
def test_redacts_secrets():
    text = "ключ sk-ant-api03-abcdefghijklmnop и токен ghp_ABCDEFGHIJKLMNOPQRSTUVWXYZ012345"
    cleaned = redact(text)
    assert "sk-ant-api03-abcdefghijklmnop" not in cleaned
    assert "ghp_ABCDEFGHIJKLMNOPQRSTUVWXYZ012345" not in cleaned


def test_blocks_dangerous_commands():
    for command in ("rm -rf /", ":(){ :|:& };:", "mkfs.ext4 /dev/sda1", "dd if=/dev/zero of=/dev/sda"):
        ok, reason = check_command(command)
        assert not ok, f"команда должна блокироваться: {command}"
        assert reason


def test_allows_normal_commands():
    for command in ("pytest -q", "git status", "python app.py", "npm run build"):
        ok, _ = check_command(command)
        assert ok, f"обычная команда не должна блокироваться: {command}"


def test_safe_path(workspace):
    inside = safe_path("sub/file.py", workspace)
    assert is_inside(inside, workspace)

    import pytest

    with pytest.raises(PermissionError):
        safe_path("../../etc/passwd", workspace)


# ------------------------------------------------------------------------------- diff
def test_unified_diff_and_patch_blocks():
    before = "один\nдва\nтри\n"
    after = "один\nдва изменён\nтри\nчетыре\n"
    diff = unified_diff(before, after, filename="file.txt")
    assert "два изменён" in diff and diff.startswith("---")

    patch_text = ("<<<<<<< SEARCH\n"
                  "два\n"
                  "=======\n"
                  "два изменён\n"
                  ">>>>>>> REPLACE")
    blocks = parse_patch(patch_text)
    assert len(blocks) == 1
    updated, problems = apply_patch_blocks(before, blocks)   # (текст, список проблем)
    assert problems == []
    assert "два изменён" in updated
