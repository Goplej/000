"""Поисковые движки без API-ключей: DuckDuckGo, SearXNG, Wikipedia, StackExchange.

Ключи не нужны, поэтому поиск работает «из коробки» на любой машине.
Если один движок недоступен — автоматически пробуем следующий.
"""
from __future__ import annotations

import html
import json
import re
import urllib.error
import urllib.parse
import urllib.request
from collections.abc import Callable
from dataclasses import dataclass, field

USER_AGENT = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"
)


@dataclass
class SearchResult:
    title: str = ""
    url: str = ""
    snippet: str = ""
    source: str = ""

    def to_dict(self) -> dict[str, str]:
        return {"title": self.title, "url": self.url, "snippet": self.snippet,
                "source": self.source}


@dataclass
class SearchOutcome:
    results: list[SearchResult] = field(default_factory=list)
    engine: str = ""
    errors: list[str] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return bool(self.results)


def _fetch(url: str, *, data: bytes | None = None, timeout: int = 20,
           headers: dict[str, str] | None = None) -> str:
    request = urllib.request.Request(
        url, data=data,
        headers={"User-Agent": USER_AGENT, "Accept-Language": "ru,en;q=0.8", **(headers or {})},
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        charset = "utf-8"
        content_type = response.headers.get("Content-Type", "")
        match = re.search(r"charset=([\w-]+)", content_type, re.IGNORECASE)
        if match:
            charset = match.group(1)
        return response.read(2_000_000).decode(charset, "replace")


def _clean(text: str) -> str:
    return html.unescape(re.sub(r"<[^>]+>", "", text or "")).strip()


# --------------------------------------------------------------------------------------
# DuckDuckGo
# --------------------------------------------------------------------------------------
def duckduckgo(query: str, limit: int = 5, timeout: int = 20) -> list[SearchResult]:
    payload = urllib.parse.urlencode({"q": query, "kl": "ru-ru"}).encode()
    page = _fetch(
        "https://html.duckduckgo.com/html/", data=payload, timeout=timeout,
        headers={"Content-Type": "application/x-www-form-urlencoded"},
    )
    results: list[SearchResult] = []
    blocks = re.findall(
        r'<div[^>]+class="[^"]*result[^"]*results_links[^"]*"[\s\S]*?(?=<div[^>]+class="[^"]*result|$)',
        page,
    )
    for block in blocks:
        link = re.search(r'class="result__a"[^>]*href="([^"]+)"[^>]*>([\s\S]*?)</a>', block)
        if not link:
            continue
        snippet_match = re.search(r'class="result__(?:snippet|body)"[^>]*>([\s\S]*?)</a>', block)
        results.append(SearchResult(
            title=_clean(link.group(2)),
            url=_unwrap(html.unescape(link.group(1))),
            snippet=_clean(snippet_match.group(1)) if snippet_match else "",
            source="duckduckgo",
        ))
        if len(results) >= limit:
            break
    return results


def duckduckgo_lite(query: str, limit: int = 5, timeout: int = 20) -> list[SearchResult]:
    payload = urllib.parse.urlencode({"q": query}).encode()
    page = _fetch("https://lite.duckduckgo.com/lite/", data=payload, timeout=timeout,
                  headers={"Content-Type": "application/x-www-form-urlencoded"})
    links = re.findall(r'<a[^>]+class="result-link"[^>]*href="([^"]+)"[^>]*>([\s\S]*?)</a>', page)
    snippets = re.findall(r'class="result-snippet"[^>]*>([\s\S]*?)</td>', page)
    results = []
    for index, (url, title) in enumerate(links[:limit]):
        results.append(SearchResult(
            title=_clean(title), url=_unwrap(html.unescape(url)),
            snippet=_clean(snippets[index]) if index < len(snippets) else "",
            source="duckduckgo-lite",
        ))
    return results


def _unwrap(url: str) -> str:
    if url.startswith("//"):
        url = "https:" + url
    if "duckduckgo.com/l/" in url:
        query = urllib.parse.parse_qs(urllib.parse.urlparse(url).query)
        if query.get("uddg"):
            return query["uddg"][0]
    return url


# --------------------------------------------------------------------------------------
# SearXNG (свой или публичный инстанс)
# --------------------------------------------------------------------------------------
def searx(query: str, limit: int = 5, timeout: int = 20, base: str = "https://searx.be") -> list[SearchResult]:
    url = f"{base.rstrip('/')}/search?" + urllib.parse.urlencode(
        {"q": query, "format": "json", "language": "ru", "safesearch": 1}
    )
    body = _fetch(url, timeout=timeout, headers={"Accept": "application/json"})
    data = json.loads(body)
    return [
        SearchResult(title=item.get("title", ""), url=item.get("url", ""),
                     snippet=_clean(item.get("content", "")), source="searx")
        for item in (data.get("results") or [])[:limit]
    ]


# --------------------------------------------------------------------------------------
# Wikipedia
# --------------------------------------------------------------------------------------
def wikipedia(query: str, limit: int = 5, timeout: int = 20, language: str = "") -> list[SearchResult]:
    languages = [language] if language else ["ru", "en"]
    for lang in languages:
        try:
            url = f"https://{lang}.wikipedia.org/w/api.php?" + urllib.parse.urlencode(
                {"action": "query", "list": "search", "srsearch": query, "format": "json",
                 "srlimit": limit, "utf8": 1}
            )
            data = json.loads(_fetch(url, timeout=timeout, headers={"Accept": "application/json"}))
        except (urllib.error.URLError, OSError, json.JSONDecodeError, ValueError):
            continue
        items = ((data.get("query") or {}).get("search")) or []
        results = [
            SearchResult(
                title=item.get("title", ""),
                url=f"https://{lang}.wikipedia.org/wiki/" + urllib.parse.quote(str(item.get("title", "")).replace(" ", "_")),
                snippet=_clean(item.get("snippet", "")),
                source=f"wikipedia-{lang}",
            )
            for item in items
        ]
        if results:
            return results
    return []


def wikipedia_page(title: str, timeout: int = 20, language: str = "") -> str:
    """Полный текст статьи — удобно для фактических справок."""
    for lang in ([language] if language else ["ru", "en"]):
        url = f"https://{lang}.wikipedia.org/api/rest_v1/page/summary/" + urllib.parse.quote(title)
        try:
            data = json.loads(_fetch(url, timeout=timeout, headers={"Accept": "application/json"}))
        except (urllib.error.URLError, OSError, json.JSONDecodeError, ValueError):
            continue
        extract = data.get("extract") or ""
        if extract:
            return f"{data.get('title', title)}\n{data.get('content_urls', {}).get('desktop', {}).get('page', '')}\n\n{extract}"
    return ""


# --------------------------------------------------------------------------------------
# StackExchange (вопросы по программированию)
# --------------------------------------------------------------------------------------
def stackexchange(query: str, limit: int = 5, timeout: int = 20) -> list[SearchResult]:
    url = "https://api.stackexchange.com/2.3/search/advanced?" + urllib.parse.urlencode({
        "order": "desc", "sort": "relevance", "q": query, "site": "stackoverflow",
        "pagesize": limit, "filter": "withbody",
    })
    data = json.loads(_fetch(url, timeout=timeout, headers={"Accept": "application/json"}))
    results = []
    for item in data.get("items", [])[:limit]:
        results.append(SearchResult(
            title=_clean(item.get("title", "")),
            url=item.get("link", ""),
            snippet=_clean(re.sub(r"<[^>]+>", " ", item.get("body", "")))[:400],
            source="stackoverflow",
        ))
    return results


def hackernews(query: str, limit: int = 5, timeout: int = 20) -> list[SearchResult]:
    """Поиск через Algolia API — свежие технические ссылки."""
    url = "https://hn.algolia.com/api/v1/search?" + urllib.parse.urlencode(
        {"query": query, "hitsPerPage": limit}
    )
    data = json.loads(_fetch(url, timeout=timeout, headers={"Accept": "application/json"}))
    results = []
    for hit in data.get("hits", [])[:limit]:
        title = hit.get("title") or hit.get("story_title") or ""
        if not title:
            continue
        results.append(SearchResult(
            title=_clean(title),
            url=hit.get("url") or f"https://news.ycombinator.com/item?id={hit.get('objectID')}",
            snippet=_clean(hit.get("story_text") or "")[:300],
            source="hackernews",
        ))
    return results


# --------------------------------------------------------------------------------------
# Каскад: пробуем движки по очереди
# --------------------------------------------------------------------------------------
ENGINES: dict[str, Callable[..., list[SearchResult]]] = {
    "duckduckgo": duckduckgo,
    "duckduckgo-lite": duckduckgo_lite,
    "searx": searx,
    "wikipedia": wikipedia,
    "stackoverflow": stackexchange,
    "hackernews": hackernews,
}

DEFAULT_ORDER = ("duckduckgo", "duckduckgo-lite", "searx", "stackoverflow", "wikipedia")


def search(query: str, limit: int = 5, timeout: int = 20, engine: str = "",
           searx_url: str = "https://searx.be") -> SearchOutcome:
    """Ищет в интернете, перебирая движки. Возвращает результаты и причины неудач."""
    outcome = SearchOutcome()
    order = [engine] if engine else list(DEFAULT_ORDER)

    for name in order:
        function = ENGINES.get(name)
        if function is None:
            outcome.errors.append(f"{name}: неизвестный движок")
            continue
        try:
            if name == "searx":
                results = function(query, limit, timeout, base=searx_url)
            else:
                results = function(query, limit, timeout)
        except (urllib.error.URLError, urllib.error.HTTPError, OSError, ValueError,
                json.JSONDecodeError) as e:
            outcome.errors.append(f"{name}: {type(e).__name__}: {e}")
            continue
        if results:
            outcome.results = results[:limit]
            outcome.engine = name
            return outcome
        outcome.errors.append(f"{name}: пусто")

    return outcome


def dedupe(results: list[SearchResult]) -> list[SearchResult]:
    seen: set[str] = set()
    out: list[SearchResult] = []
    for item in results:
        key = urllib.parse.urlparse(item.url).netloc + urllib.parse.urlparse(item.url).path.rstrip("/")
        if key in seen:
            continue
        seen.add(key)
        out.append(item)
    return out


def format_results(outcome: SearchOutcome, query: str) -> str:
    if not outcome.results:
        return (f"Поиск по запросу «{query}» ничего не дал.\n"
                f"Причины: {'; '.join(outcome.errors[:4]) or 'неизвестно'}\n"
                "Попробуй другой запрос или используй fetch_url с известным адресом.")
    lines = [f"Найдено ({outcome.engine}) по запросу «{query}»:"]
    for index, item in enumerate(dedupe(outcome.results), 1):
        lines.append(f"{index}. {item.title}\n   {item.url}\n   {item.snippet[:280]}")
    lines.append("\nЧтобы прочитать страницу целиком — вызови fetch_url с адресом.")
    return "\n".join(lines)


__all__ = [
    "DEFAULT_ORDER", "ENGINES", "SearchOutcome", "SearchResult", "dedupe", "duckduckgo",
    "duckduckgo_lite", "format_results", "hackernews", "search", "searx", "stackexchange",
    "wikipedia", "wikipedia_page",
]
