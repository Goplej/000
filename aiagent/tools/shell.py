"""Терминал: выполнение команд, фоновые процессы, быстрый запуск кода."""
from __future__ import annotations

import os
import platform
import shlex
import shutil
import signal
import subprocess
import time
from pathlib import Path
from typing import Any

from ..utils.security import check_command, env_without_secrets, risk_reason
from ..utils.text import clip, human_duration
from . import Tool, ToolContext, ToolError, ToolResult

MAX_OUTPUT = 24_000
_background: dict[int, subprocess.Popen] = {}


def _launch(command: str, cwd: Path, timeout: int, env: dict[str, str] | None = None):
    """Запускает команду, учитывая особенности Windows."""
    if platform.system() == "Windows":
        shell_command = ["cmd.exe", "/c", command]
    else:
        shell_command = [os.environ.get("SHELL") or "/bin/bash", "-lc", command]
    return subprocess.run(
        shell_command,
        cwd=str(cwd),
        capture_output=True,
        timeout=timeout,
        text=True,
        encoding="utf-8",
        errors="replace",
        env=env or {**env_without_secrets(), "PYTHONIOENCODING": "utf-8", "PYTHONUNBUFFERED": "1"},
    )


def run_shell(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    command = str(args.get("command") or "").strip()
    if not command:
        raise ToolError("пустая команда")
    if not ctx.config.allow_shell:
        raise ToolError("выполнение команд отключено (allow_shell=false)")

    ok, reason = check_command(command)
    if not ok:
        return ToolResult.error(
            f"команда заблокирована политикой безопасности: {reason}. "
            "Если она действительно нужна — выполни её вручную в терминале.",
            blocked=True,
        )

    timeout = max(1, min(int(args.get("timeout") or ctx.config.shell_timeout), 1800))
    workdir = Path(args.get("cwd") or ".")
    if not workdir.is_absolute():
        workdir = Path(ctx.workspace) / workdir
    if not workdir.is_dir():
        workdir = Path(ctx.workspace)

    env = {**env_without_secrets(), "PYTHONIOENCODING": "utf-8", "PYTHONUNBUFFERED": "1"}
    started = time.time()
    try:
        proc = _launch(command, workdir, timeout, env)
    except subprocess.TimeoutExpired:
        return ToolResult.error(
            f"команда не завершилась за {human_duration(timeout)} и была прервана. "
            "Увеличь timeout или разбей работу на части."
        )
    except FileNotFoundError as e:
        return ToolResult.error(f"команда не найдена: {e}")
    except OSError as e:
        return ToolResult.error(f"не удалось запустить команду: {e}")

    elapsed = time.time() - started
    stdout = (proc.stdout or "").rstrip()
    stderr = (proc.stderr or "").rstrip()

    parts = [f"$ {command}", f"рабочий каталог: {workdir}", f"код возврата: {proc.returncode}",
             f"время: {human_duration(elapsed)}"]
    if stdout:
        parts.append("stdout:\n" + clip(stdout, MAX_OUTPUT // 2))
    if stderr:
        parts.append("stderr:\n" + clip(stderr, MAX_OUTPUT // 2, head_ratio=0.4))
    if not stdout and not stderr:
        parts.append("(пустой вывод)")

    output = "\n".join(parts)
    if proc.returncode == 0:
        return ToolResult.done(output, display=f"run_shell {clip(command, 70)}", returncode=0)
    hint = ""
    if "command not found" in stderr or "не является внутренней" in stderr:
        hint = "\n\nПодсказка: команда не найдена — проверь название и PATH."
    elif "No such file or directory" in stderr:
        hint = "\n\nПодсказка: файл или каталог отсутствуют — проверь пути (list_dir)."
    return ToolResult(False, output + hint,
                      display=f"run_shell {clip(command, 70)} → код {proc.returncode}",
                      meta={"returncode": proc.returncode})


def run_code(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    """Быстрый запуск фрагмента кода: удобно для проверки идеи."""
    code = str(args.get("code") or "")
    language = str(args.get("language") or "python").lower()
    if not code.strip():
        raise ToolError("пустой код")

    runners = {
        "python": (".py", [os.sys.executable or "python3"]),
        "py": (".py", [os.sys.executable or "python3"]),
        "node": (".js", ["node"]),
        "javascript": (".js", ["node"]),
        "js": (".js", ["node"]),
        "bash": (".sh", ["bash"]),
        "sh": (".sh", ["bash"]),
        "ruby": (".rb", ["ruby"]),
        "php": (".php", ["php"]),
        "go": (".go", ["go", "run"]),
    }
    if language not in runners:
        raise ToolError(f"поддерживаются: {', '.join(sorted(runners))}")
    extension, interpreter = runners[language]
    if interpreter[0] not in {"bash"} and not shutil.which(interpreter[0]):
        raise ToolError(f"интерпретатор {interpreter[0]} не найден в системе")

    tmp_dir = Path(ctx.workspace) / ".aia" / "tmp"
    tmp_dir.mkdir(parents=True, exist_ok=True)
    script = tmp_dir / f"snippet-{int(time.time() * 1000)}{extension}"
    script.write_text(code, encoding="utf-8")

    command = " ".join([*(shlex.quote(part) for part in interpreter), shlex.quote(str(script))])
    result = run_shell({**args, "command": command, "timeout": args.get("timeout") or 60}, ctx)
    result.display = f"run_code {language}"
    return result


def shell_start(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    """Запускает долгоживущий процесс (сервер, watcher) в фоне."""
    command = str(args.get("command") or "").strip()
    if not command:
        raise ToolError("пустая команда")
    ok, reason = check_command(command)
    if not ok:
        return ToolResult.error(f"команда заблокирована: {reason}")

    workdir = Path(ctx.workspace) / str(args.get("cwd") or ".")
    if not workdir.is_dir():
        workdir = Path(ctx.workspace)

    if platform.system() == "Windows":
        shell_command = ["cmd.exe", "/c", command]
    else:
        shell_command = [os.environ.get("SHELL") or "/bin/bash", "-lc", command]

    log_path = Path(ctx.workspace) / ".aia" / "logs"
    log_path.mkdir(parents=True, exist_ok=True)
    log_file = log_path / f"shell-{int(time.time())}.log"

    handle = log_file.open("w", encoding="utf-8")
    try:
        proc = subprocess.Popen(
            shell_command, cwd=str(workdir), stdout=handle, stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL, text=True, start_new_session=(platform.system() != "Windows"),
        )
    except OSError as e:
        handle.close()
        raise ToolError(f"не удалось запустить процесс: {e}") from e

    _background[proc.pid] = proc
    time.sleep(float(args.get("wait") or 1.5))
    status = "работает" if proc.poll() is None else f"завершился с кодом {proc.returncode}"
    log_tail = clip(log_file.read_text(encoding="utf-8", errors="replace"), 3000) or "(пока без вывода)"
    return ToolResult.done(
        f"PID {proc.pid}: {command}\nсостояние: {status}\nлог: {log_file}\n\n--- вывод ---\n{log_tail}",
        display=f"shell_start PID {proc.pid} ({status})", pid=proc.pid, log=str(log_file),
    )


def shell_output(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    """Читает накопленный вывод фонового процесса."""
    pid = int(args["pid"])
    log_hint = args.get("log")
    log_file = Path(str(log_hint)) if log_hint else _find_log(ctx, pid)
    if log_file is None or not log_file.is_file():
        raise ToolError(f"лог для PID {pid} не найден")
    tail = int(args.get("lines") or 120)
    lines = log_file.read_text(encoding="utf-8", errors="replace").splitlines()
    body = "\n".join(lines[-tail:]) or "(вывод пуст)"
    proc = _background.get(pid)
    status = "нет данных о процессе"
    if proc is not None:
        status = "работает" if proc.poll() is None else f"завершён (код {proc.returncode})"
    return ToolResult.done(f"PID {pid}: {status}\n\n{body}", display=f"shell_output PID {pid}")


def shell_kill(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    pid = int(args["pid"])
    proc = _background.pop(pid, None)
    if proc is None:
        if platform.system() == "Windows":
            result = run_shell({"command": f"taskkill /PID {pid} /T /F"}, ctx)
        else:
            result = run_shell({"command": f"kill -TERM {pid}"}, ctx)
        return ToolResult(result.ok, f"процесс {pid}: попытка остановки через систему\n{result.content}",
                          display=f"shell_kill {pid}")
    try:
        if platform.system() == "Windows":
            proc.terminate()
        else:
            os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
        time.sleep(0.5)
        if proc.poll() is None:
            proc.kill()
    except OSError as e:
        return ToolResult.error(f"не удалось остановить процесс {pid}: {e}")
    return ToolResult.done(f"процесс {pid} остановлен", display=f"shell_kill {pid}")


def shell_list(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    alive = []
    for pid, proc in list(_background.items()):
        if proc.poll() is None:
            alive.append(f"PID {pid}: работает")
        else:
            alive.append(f"PID {pid}: завершён (код {proc.returncode})")
            _background.pop(pid, None)
    return ToolResult.done("\n".join(alive) if alive else "фоновых процессов нет",
                           display=f"shell_list ({len(alive)})")


def _find_log(ctx: ToolContext, pid: int) -> Path | None:
    logs = Path(ctx.workspace) / ".aia" / "logs"
    if not logs.is_dir():
        return None
    candidates = sorted(logs.glob("shell-*.log"), key=lambda p: p.stat().st_mtime, reverse=True)
    return candidates[0] if candidates else None


def risk_of(command: str) -> str:
    return risk_reason(command)


TOOLS = [
    Tool("run_shell", "Выполнить команду в терминале (тесты, сборка, git, скрипты).",
         {"command": {"type": "string", "description": "команда оболочки"},
          "cwd": {"type": "string", "description": "рабочий каталог (по умолчанию корень проекта)"},
          "timeout": {"type": "integer", "description": "лимит времени в секундах"}},
         run_shell, required=("command",), dangerous=True, category="shell"),

    Tool("run_code", "Быстро выполнить короткий фрагмент кода (python/node/bash) и увидеть вывод.",
         {"code": {"type": "string", "description": "исходный код"},
          "language": {"type": "string", "description": "python | node | bash | ruby | php | go"},
          "timeout": {"type": "integer", "description": "лимит времени"}},
         run_code, required=("code",), dangerous=True, category="shell"),

    Tool("shell_start", "Запустить долгоживущий процесс в фоне (сервер, watcher, сборка).",
         {"command": {"type": "string", "description": "команда"},
          "cwd": {"type": "string", "description": "рабочий каталог"},
          "wait": {"type": "number", "description": "сколько секунд подождать перед отчётом"}},
         shell_start, required=("command",), dangerous=True, category="shell"),

    Tool("shell_output", "Прочитать вывод фонового процесса.",
         {"pid": {"type": "integer", "description": "идентификатор процесса"},
          "lines": {"type": "integer", "description": "сколько последних строк"},
          "log": {"type": "string", "description": "путь к лог-файлу (если известен)"}},
         shell_output, required=("pid",), category="shell"),

    Tool("shell_kill", "Остановить фоновый процесс.",
         {"pid": {"type": "integer", "description": "идентификатор процесса"}},
         shell_kill, required=("pid",), dangerous=True, category="shell"),

    Tool("shell_list", "Показать запущенные агентом фоновые процессы.", {}, shell_list,
         category="shell"),
]

__all__ = ["TOOLS", "run_shell", "run_code", "shell_start", "shell_output", "shell_kill",
           "shell_list"]
