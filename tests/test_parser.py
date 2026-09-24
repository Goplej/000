"""Разбор ответов модели: вызовы инструментов в разных форматах."""
from __future__ import annotations

from aiagent.core.parser import extract_thinking, parse_reply


def names(reply) -> list[str]:
    return [call.name for call in reply.calls]


def test_parses_tool_call_tag():
    text = ('Сейчас посмотрю файлы.\n'
            '<tool_call>{"name": "list_dir", "arguments": {"path": "."}}</tool_call>')
    reply = parse_reply(text, {"list_dir"})
    assert names(reply) == ["list_dir"]
    assert reply.calls[0].arguments == {"path": "."}
    assert "Сейчас посмотрю файлы." in reply.text
    assert "<tool_call>" not in reply.text


def test_parses_function_style_and_alt_tag():
    text = '<function>{"name": "read_file", "arguments": {"path": "app.py"}}</function>'
    reply = parse_reply(text, {"read_file"})
    assert names(reply) == ["read_file"]


def test_parses_json_code_block():
    text = 'Действую:\n```json\n{"name": "grep_search", "arguments": {"pattern": "TODO"}}\n```'
    reply = parse_reply(text, {"grep_search"})
    assert names(reply) == ["grep_search"]
    assert "```" not in reply.text


def test_parses_multiple_calls():
    text = ('<tool_call>{"name": "read_file", "arguments": {"path": "a.py"}}</tool_call>\n'
            '<tool_call>{"name": "read_file", "arguments": {"path": "b.py"}}</tool_call>')
    reply = parse_reply(text, {"read_file"})
    assert len(reply.calls) == 2
    assert [call.arguments["path"] for call in reply.calls] == ["a.py", "b.py"]


def test_parses_bare_json_object():
    text = '{"tool": "run_shell", "arguments": {"command": "pytest -q"}}'
    reply = parse_reply(text, {"run_shell"})
    assert names(reply) == ["run_shell"]
    assert reply.calls[0].arguments["command"] == "pytest -q"


def test_parses_react_style():
    text = ("Thought: нужно узнать размер файла\n"
            "Action: file_info\n"
            "Action Input: {\"path\": \"app.py\"}")
    reply = parse_reply(text, {"file_info"})
    assert names(reply) == ["file_info"]


def test_ignores_unknown_tools():
    text = '<tool_call>{"name": "телепорт", "arguments": {}}</tool_call>'
    reply = parse_reply(text, {"read_file", "write_file"})
    assert reply.calls == []


def test_plain_text_has_no_calls():
    reply = parse_reply("Просто ответ без вызовов.", {"read_file"})
    assert reply.calls == []
    assert "Просто ответ" in reply.text


def test_arguments_as_json_string():
    text = '<tool_call>{"name": "write_file", "arguments": "{\\"path\\": \\"x.py\\", \\"content\\": \\"1\\"}"}</tool_call>'
    reply = parse_reply(text, {"write_file"})
    assert reply.calls and reply.calls[0].arguments["path"] == "x.py"


def test_arguments_positional_fallback():
    text = '<tool_call>{"name": "read_file", "parameters": {"path": "main.py"}}</tool_call>'
    reply = parse_reply(text, {"read_file"})
    assert names(reply) == ["read_file"]
    assert reply.calls[0].arguments == {"path": "main.py"}


def test_extract_thinking():
    text = "Перед ответом подумаю.\n<thinking>Надо проверить тесты.</thinking>\nГотово."
    thinking, clean = extract_thinking(text)
    assert "проверить тесты" in thinking
    assert "<thinking>" not in clean
    thought = '<thinking>Проверю файл</thinking><tool_call>{"name": "read_file", "arguments": {"path": "a"}}</tool_call>'
    reply = parse_reply(thought, {"read_file"})
    assert names(reply) == ["read_file"]
    assert "Проверю файл" in reply.thinking
    assert "<thinking>" not in reply.text
