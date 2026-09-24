"""Интеграция с GitHub: через CLI `gh`, если он установлен, иначе через REST API."""
from __future__ import annotations

import json
import shutil
import subprocess
from dataclasses import dataclass

from ..errors import AgentError
from ..utils.http import HttpClient
from ..utils.logging import get_logger

log = get_logger("integrations.github")
API = "https://api.github.com"


@dataclass
class GitHubStatus:
    cli: bool
    token: bool
    user: str = ""

    @property
    def summary(self) -> str:
        if self.cli:
            return f"github: gh CLI доступен{', как ' + self.user if self.user else ''}"
        if self.token:
            return "github: доступен по токену GITHUB_TOKEN (REST API)"
        return "github: не настроен (нужен gh CLI или GITHUB_TOKEN)"


def status() -> GitHubStatus:
    cli = shutil.which("gh") is not None
    token = bool(_env_token())
    user = ""
    if cli:
        code, out = _run("api", "user", "--jq", ".login")
        if code == 0:
            user = out.strip()
    return GitHubStatus(cli=cli, token=token, user=user)


def _env_token() -> str:
    import os

    return os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN") or ""


def _run(*args: str, timeout: int = 30) -> tuple[int, str]:
    try:
        proc = subprocess.run(["gh", *args], capture_output=True, text=True, timeout=timeout,
                              encoding="utf-8", errors="replace")
    except (FileNotFoundError, OSError, subprocess.TimeoutExpired) as e:
        return 127, str(e)
    return proc.returncode, (proc.stdout or proc.stderr).strip()


# --------------------------------------------------------------------------------------
# Через REST API (токен)
# --------------------------------------------------------------------------------------
def _api(method: str, path: str, body: dict | None = None) -> dict:
    token = _env_token()
    if not token:
        raise AgentError(
            "нет доступа к GitHub",
            hint="Установи gh CLI (`gh auth login`) или задай переменную GITHUB_TOKEN.",
        )
    client = HttpClient(timeout=30)
    response = client.request(
        method, f"{API}{path}",
        headers={
            "Authorization": f"Bearer {token}",
            "Accept": "application/vnd.github+json",
            "X-GitHub-Api-Version": "2022-11-28",
        },
        json_body=body,
    )
    try:
        return response.json()
    except ValueError:
        return {"raw": response.text[:2000]}


def parse_repo(url_or_slug: str) -> tuple[str, str]:
    """Принимает «owner/repo», URL репозитория или git@… — возвращает (owner, repo)."""
    value = url_or_slug.strip().rstrip("/")
    if value.startswith("git@"):
        value = value.split(":", 1)[-1]
    if "github.com" in value:
        value = value.split("github.com", 1)[1].lstrip("/:")
    value = value.removesuffix(".git")
    parts = [part for part in value.split("/") if part]
    if len(parts) < 2:
        raise AgentError(f"не понял репозиторий: {url_or_slug}",
                         hint="Ожидаю формат owner/repo или https://github.com/owner/repo")
    return parts[0], parts[1]


def list_issues(repo: str, *, state: str = "open", limit: int = 20, labels: str = "") -> str:
    owner, name = parse_repo(repo)
    query = f"?state={state}&per_page={min(limit, 100)}"
    if labels:
        query += f"&labels={labels}"
    if shutil.which("gh"):
        code, out = _run("issue", "list", "--repo", f"{owner}/{name}", "--state", state,
                         "--limit", str(limit), "--json",
                         "number,title,labels,author,createdAt")
        if code == 0:
            return _format_issues(json.loads(out or "[]"))
    issues = _api("GET", f"/repos/{owner}/{name}/issues{query}")
    if isinstance(issues, dict):
        return str(issues)
    return _format_issues(issues)


def _format_issues(issues: list[dict]) -> str:
    if not issues:
        return "Задач не найдено."
    lines = []
    for item in issues:
        labels = ", ".join(label.get("name", "") for label in item.get("labels", []))
        author = (item.get("user") or {}).get("login") or (item.get("author") or {}).get("login", "")
        lines.append(f"#{item.get('number')} {item.get('title')}"
                     + (f"  [{labels}]" if labels else "")
                     + (f"  — {author}" if author else ""))
    return "\n".join(lines)


def create_issue(repo: str, title: str, body: str = "", labels: list[str] | None = None) -> str:
    owner, name = parse_repo(repo)
    if shutil.which("gh"):
        args = ["issue", "create", "--repo", f"{owner}/{name}", "--title", title, "--body", body]
        for label in labels or []:
            args += ["--label", label]
        code, out = _run(*args)
        if code == 0:
            return f"Задача создана: {out.strip().splitlines()[-1]}"
        log.warning("gh issue create: %s", out)
    payload = {"title": title, "body": body, "labels": labels or []}
    result = _api("POST", f"/repos/{owner}/{name}/issues", payload)
    return f"Задача создана: {result.get('html_url') or result}"


def create_pr(repo: str, title: str, body: str = "", *, base: str = "main", head: str = "") -> str:
    owner, name = parse_repo(repo)
    head = head or _current_branch()
    if shutil.which("gh"):
        args = ["pr", "create", "--repo", f"{owner}/{name}", "--title", title, "--body", body,
                "--base", base, "--head", head]
        code, out = _run(*args)
        if code == 0:
            return f"Pull request: {out.strip().splitlines()[-1]}"
        log.warning("gh pr create: %s", out)
    result = _api("POST", f"/repos/{owner}/{name}/pulls",
                  {"title": title, "body": body, "base": base, "head": head})
    return f"Pull request: {result.get('html_url') or result}"


def _current_branch() -> str:
    try:
        proc = subprocess.run(["git", "branch", "--show-current"], capture_output=True, text=True,
                              timeout=10)
        return proc.stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return ""


def repo_info(repo: str) -> str:
    owner, name = parse_repo(repo)
    data = _api("GET", f"/repos/{owner}/{name}")
    if "full_name" not in data:
        return str(data)[:1500]
    return (f"{data['full_name']}: {data.get('description') or '—'}\n"
            f"звёзд: {data.get('stargazers_count')}, форков: {data.get('forks_count')}, "
            f"открытых задач: {data.get('open_issues_count')}\n"
            f"язык: {data.get('language')}, ветка по умолчанию: {data.get('default_branch')}")


__all__ = ["GitHubStatus", "create_issue", "create_pr", "list_issues", "parse_repo", "repo_info",
           "status"]
