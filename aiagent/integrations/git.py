"""Работа с git: чтение состояния репозитория и безопасные операции."""
from __future__ import annotations

import subprocess
from dataclasses import dataclass
from pathlib import Path

from ..utils.logging import get_logger

log = get_logger("integrations.git")


@dataclass
class GitState:
    is_repo: bool
    branch: str = ""
    dirty: int = 0
    untracked: int = 0
    ahead: int = 0
    behind: int = 0
    last_commit: str = ""

    @property
    def summary(self) -> str:
        if not self.is_repo:
            return "git: не репозиторий"
        parts = [f"ветка {self.branch}"]
        if self.dirty:
            parts.append(f"изменено файлов: {self.dirty}")
        if self.untracked:
            parts.append(f"новых: {self.untracked}")
        if self.ahead or self.behind:
            parts.append(f"расхождение: +{self.ahead}/-{self.behind}")
        if self.last_commit:
            parts.append(f"последний коммит: {self.last_commit}")
        return "git: " + ", ".join(parts)


def run_git(workspace: str | Path, *args: str, timeout: int = 20) -> tuple[int, str]:
    """Запускает git и возвращает (код, вывод). Ошибки не бросает."""
    try:
        proc = subprocess.run(
            ["git", *args], cwd=str(workspace), capture_output=True, text=True,
            timeout=timeout, encoding="utf-8", errors="replace",
        )
    except (FileNotFoundError, OSError, subprocess.TimeoutExpired) as e:
        log.debug("git %s недоступен: %s", " ".join(args), e)
        return 127, str(e)
    return proc.returncode, (proc.stdout or proc.stderr).strip()


def detect(workspace: str | Path) -> GitState:
    code, _ = run_git(workspace, "rev-parse", "--is-inside-work-tree")
    if code != 0:
        return GitState(is_repo=False)

    state = GitState(is_repo=True)
    _, state.branch = run_git(workspace, "branch", "--show-current")
    if not state.branch:
        _, state.branch = run_git(workspace, "rev-parse", "--short", "HEAD")

    _, status = run_git(workspace, "status", "--porcelain")
    for line in status.splitlines():
        if line.startswith("??"):
            state.untracked += 1
        elif line.strip():
            state.dirty += 1

    _, counts = run_git(workspace, "rev-list", "--left-right", "--count", "@{upstream}...HEAD")
    if counts and len(counts.split()) == 2:
        behind, ahead = counts.split()
        state.behind, state.ahead = int(behind), int(ahead)

    _, last = run_git(workspace, "log", "-1", "--pretty=%h %s")
    state.last_commit = last[:120]
    return state


def context_text(workspace: str | Path) -> str:
    """Строка о git для системного промпта."""
    state = detect(workspace)
    if not state.is_repo:
        return ""
    lines = [state.summary]
    _, recent = run_git(workspace, "log", "-5", "--pretty=%h %ad %s", "--date=short")
    if recent:
        lines.append("Последние коммиты:\n" + "\n".join("  " + line for line in recent.splitlines()))
    _, diffstat = run_git(workspace, "diff", "--stat")
    if diffstat:
        lines.append("Незакоммиченные изменения:\n" + "\n".join(
            "  " + line for line in diffstat.splitlines()[-8:]))
    return "\n".join(lines)


def diff(workspace: str | Path, *, staged: bool = False, path: str = "") -> str:
    args = ["diff", "--unified=3"]
    if staged:
        args.append("--cached")
    if path:
        args += ["--", path]
    code, out = run_git(workspace, *args)
    return out if code == 0 else f"git diff недоступен: {out}"


def blame_line(workspace: str | Path, path: str, line: int) -> str:
    code, out = run_git(workspace, "blame", "-L", f"{line},{line}", "--porcelain", "--", path)
    if code != 0:
        return out
    author, when, subject = "", "", ""
    for row in out.splitlines():
        if row.startswith("author "):
            author = row[7:]
        elif row.startswith("author-time "):
            import time
            when = time.strftime("%Y-%m-%d", time.localtime(int(row[12:])))
        elif row.startswith("summary "):
            subject = row[8:]
    return f"{path}:{line} — {author}, {when}: {subject}"


def recent_commits(workspace: str | Path, count: int = 10) -> list[dict[str, str]]:
    code, out = run_git(workspace, "log", f"-{count}", "--pretty=%H%x1f%h%x1f%an%x1f%ad%x1f%s",
                        "--date=short")
    if code != 0:
        return []
    commits = []
    for line in out.splitlines():
        parts = line.split("\x1f")
        if len(parts) == 5:
            commits.append({"hash": parts[0], "short": parts[1], "author": parts[2],
                            "date": parts[3], "subject": parts[4]})
    return commits


__all__ = ["GitState", "blame_line", "context_text", "detect", "diff", "recent_commits", "run_git"]
