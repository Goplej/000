"""Инструменты GitHub: задачи, pull request'ы и информация о репозитории.

Работают двумя способами: через CLI `gh`, если он установлен и авторизован, иначе
через REST API api.github.com с токеном из GITHUB_TOKEN/GH_TOKEN или ~/.aiagent/keys.json.
"""
from __future__ import annotations

from typing import Any

from ..errors import AgentError
from ..integrations import github as gh
from . import Tool, ToolContext, ToolError, ToolResult


def _repo(args: dict[str, Any]) -> str:
    repo = str(args.get("repo") or "").strip()
    if not repo:
        raise ToolError("нужен параметр repo: owner/repo или https://github.com/owner/repo")
    return repo


def _guard(call) -> str | ToolResult:
    """Общая обёртка: превращает ошибки интеграции в понятный ToolResult."""
    try:
        return call()
    except AgentError as e:  # нет токена, нет репозитория, ошибка сети — это не падение агента
        return ToolResult(False, e.human(), display="github: ошибка")


def github_status(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    state = gh.status()
    return ToolResult.done(state.summary, display="github_status", cli=state.cli, token=state.token)


def github_issues(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    repo = _repo(args)
    state = str(args.get("state") or "open")
    limit = int(args.get("limit") or 20)
    labels = str(args.get("labels") or "")

    def call() -> str:
        return gh.list_issues(repo, state=state, limit=limit, labels=labels)

    result = _guard(call)
    if isinstance(result, ToolResult):
        return result
    return ToolResult.done(result, display=f"github_issues {repo} ({state})")


def github_create_issue(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    repo = _repo(args)
    title = str(args.get("title") or "").strip()
    if not title:
        raise ToolError("нужен параметр title — заголовок задачи")
    body = str(args.get("body") or "")
    labels = [str(label) for label in (args.get("labels") or [])]

    def call() -> str:
        return gh.create_issue(repo, title, body, labels)

    result = _guard(call)
    if isinstance(result, ToolResult):
        return result
    return ToolResult.done(result, display=f"github_create_issue {repo}")


def github_create_pr(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    repo = _repo(args)
    title = str(args.get("title") or "").strip()
    if not title:
        raise ToolError("нужен параметр title — заголовок pull request")
    body = str(args.get("body") or "")
    base = str(args.get("base") or "main")
    head = str(args.get("head") or "")

    def call() -> str:
        return gh.create_pr(repo, title, body, base=base, head=head)

    result = _guard(call)
    if isinstance(result, ToolResult):
        return result
    return ToolResult.done(result, display=f"github_create_pr {repo} → {base}")


def github_repo_info(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    repo = _repo(args)

    def call() -> str:
        return gh.repo_info(repo)

    result = _guard(call)
    if isinstance(result, ToolResult):
        return result
    return ToolResult.done(result, display=f"github_repo_info {repo}")


TOOLS = [
    Tool(
        name="github_status",
        description="Проверить доступность GitHub: установлен ли gh CLI и есть ли токен.",
        parameters={},
        handler=github_status,
        category="github",
    ),
    Tool(
        name="github_issues",
        description="Список задач (issues) репозитория GitHub.",
        parameters={
            "repo": {"type": "string", "description": "owner/repo или ссылка на репозиторий"},
            "state": {"type": "string", "description": "open | closed | all", "default": "open"},
            "limit": {"type": "integer", "description": "сколько задач показать", "default": 20},
            "labels": {"type": "string", "description": "метки через запятую"},
        },
        handler=github_issues,
        required=("repo",),
        category="github",
    ),
    Tool(
        name="github_create_issue",
        description="Создать задачу (issue) в репозитории GitHub.",
        parameters={
            "repo": {"type": "string", "description": "owner/repo или ссылка"},
            "title": {"type": "string", "description": "заголовок задачи"},
            "body": {"type": "string", "description": "описание (Markdown)"},
            "labels": {"type": "array", "items": {"type": "string"}, "description": "метки"},
        },
        handler=github_create_issue,
        required=("repo", "title"),
        writes=True,
        category="github",
    ),
    Tool(
        name="github_create_pr",
        description=("Создать pull request из текущей (или указанной) ветки в базовую. "
                     "Сначала убедись, что изменения отправлены: git push."),
        parameters={
            "repo": {"type": "string", "description": "owner/repo или ссылка"},
            "title": {"type": "string", "description": "заголовок pull request"},
            "body": {"type": "string", "description": "описание (Markdown)"},
            "base": {"type": "string", "description": "базовая ветка", "default": "main"},
            "head": {"type": "string", "description": "ветка с изменениями (по умолчанию текущая)"},
        },
        handler=github_create_pr,
        required=("repo", "title"),
        writes=True,
        category="github",
    ),
    Tool(
        name="github_repo_info",
        description="Краткая информация о репозитории GitHub: описание, звёзды, язык, ссылка.",
        parameters={"repo": {"type": "string", "description": "owner/repo или ссылка"}},
        handler=github_repo_info,
        required=("repo",),
        category="github",
    ),
]

__all__ = [
    "TOOLS",
    "github_create_issue",
    "github_create_pr",
    "github_issues",
    "github_repo_info",
    "github_status",
]
