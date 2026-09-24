"""Инструменты GitHub: успешные пути и аккуратные ошибки без сети."""
from __future__ import annotations

from pathlib import Path

import pytest

from aiagent.config import AgentConfig
from aiagent.core.permissions import PermissionManager
from aiagent.errors import ConfigError
from aiagent.integrations import github as gh
from aiagent.integrations.github import GitHubStatus
from aiagent.tools import ToolContext, ToolError, build_registry


@pytest.fixture
def ctx(config: AgentConfig, workspace: Path) -> ToolContext:
    return ToolContext(config=config, workspace=workspace, permissions=PermissionManager(config))


@pytest.fixture
def registry():
    return build_registry()


def test_github_tools_are_registered(registry):
    names = {name for name in registry if name.startswith("github")}
    assert names == {"github_status", "github_issues", "github_create_issue",
                     "github_create_pr", "github_repo_info"}
    assert registry["github_create_pr"].writes is True
    assert registry["github_issues"].writes is False


def test_status_reports_mode(registry, ctx, monkeypatch):
    monkeypatch.setattr(gh, "status", lambda: GitHubStatus(cli=True, token=True, user="octocat"))
    result = registry["github_status"].handler({}, ctx)
    assert result.ok
    assert "gh CLI" in result.content
    assert result.meta["cli"] is True


def test_issues_are_formatted(registry, ctx, monkeypatch):
    monkeypatch.setattr(gh, "list_issues",
                        lambda repo, *, state="open", limit=20, labels="": "#7 Баг в поиске  [bug]")
    result = registry["github_issues"].handler({"repo": "owner/repo", "state": "closed", "limit": 5}, ctx)
    assert result.ok and "#7" in result.content
    assert "owner/repo" in result.display


def test_issues_without_token_return_error_result(registry, ctx, monkeypatch):
    def boom(*args, **kwargs):
        raise ConfigError("нужен GITHUB_TOKEN или авторизация gh CLI")

    monkeypatch.setattr(gh, "list_issues", boom)
    result = registry["github_issues"].handler({"repo": "owner/repo"}, ctx)
    assert result.ok is False
    assert "GITHUB_TOKEN" in result.content


def test_repo_argument_is_validated(registry, ctx):
    with pytest.raises(ToolError):
        registry["github_issues"].handler({}, ctx)


def test_create_issue_requires_title(registry, ctx):
    with pytest.raises(ToolError):
        registry["github_create_issue"].handler({"repo": "owner/repo"}, ctx)


def test_create_pr_uses_current_branch(registry, ctx, monkeypatch):
    captured = {}

    def fake_create_pr(repo, title, body="", *, base="main", head=""):
        captured.update(repo=repo, title=title, base=base, head=head)
        return "Pull request: https://github.com/owner/repo/pull/1"

    monkeypatch.setattr(gh, "create_pr", fake_create_pr)
    result = registry["github_create_pr"].handler(
        {"repo": "https://github.com/owner/repo", "title": "Фикс", "base": "main"}, ctx)
    assert result.ok
    assert captured["title"] == "Фикс"
    assert "pull/1" in result.content


def test_repo_info(registry, ctx, monkeypatch):
    monkeypatch.setattr(gh, "repo_info", lambda repo: "owner/repo — «Описание», ★ 12, Python")
    result = registry["github_repo_info"].handler({"repo": "owner/repo"}, ctx)
    assert result.ok and "★ 12" in result.content


def test_readonly_names_include_read_tools():
    from aiagent.tools import READONLY_TOOL_NAMES

    assert {"github_status", "github_issues", "github_repo_info"} <= set(READONLY_TOOL_NAMES)
    assert "github_create_issue" not in READONLY_TOOL_NAMES
