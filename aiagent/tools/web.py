"""Интернет: веб-поиск, загрузка страниц, произвольные HTTP-запросы к API."""
from __future__ import annotations

import html
import json
import re
import urllib.error
import urllib.parse
import urllib.request
from html.parser import HTMLParser
from typing import Any

from ..utils.text import clip
from . import Tool, ToolContext, ToolError, ToolResult
from .search import format_results, search, wikipedia_page

USER_AGENT = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"
)
MAX_PAGE_CHARS = 40_000


# --------------------------------------------------------------------------------------
# HTML → текст
# --------------------------------------------------------------------------------------
class _Extractor(HTMLParser):
    SKIP = {"script", "style", "noscript", "svg", "head", "template", "iframe", "nav", "footer"}
    BLOCK = {"p", "div", "br", "li", "tr", "h1", "h2", "h3", "h4", "h5", "h6", "section",
             "article", "blockquote", "pre", "table", "ul", "ol", "header", "form"}

    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.parts: list[str] = []
        self._skip = 0
        self.title = ""
        self._in_title = False
        self.links: list[tuple[str, str]] = []
        self._link_href = ""

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        attributes = dict(attrs)
        if tag in self.SKIP:
            self._skip += 1
        if tag == "title":
            self._in_title = True
        if tag in self.BLOCK:
            self.parts.append("\n")
        if tag == "a":
            href = attributes.get("href") or ""
            if href.startswith("http"):
                self._link_href = href
                self.parts.append(" [ссылка: ")
        if tag == "img" and attributes.get("alt"):
            self.parts.append(f"[изображение: {attributes['alt']}] ")

    def handle_endtag(self, tag: str) -> None:
        if tag in self.SKIP and self._skip:
            self._skip -= 1
        if tag == "title":
            self._in_title = False
        if tag in self.BLOCK:
            self.parts.append("\n")
        if tag == "a" and self._link_href:
            self.parts.append(f"{self._link_href}] ")
            self.links.append((self._link_href, ""))
            self._link_href = ""

    def handle_data(self, data: str) -> None:
        if self._skip:
            return
        text = data.strip()
        if not text:
            return
        if self._in_title:
            self.title += text + " "
            return
        self.parts.append(text + " ")

    def text(self) -> str:
        raw = html.unescape("".join(self.parts))
        raw = re.sub(r"[ \t\r\f\v]+", " ", raw)
        raw = re.sub(r" *\n *", "\n", raw)
        return re.sub(r"\n{3,}", "\n\n", raw).strip()


def html_to_text(markup: str) -> tuple[str, str]:
    parser = _Extractor()
    try:
        parser.feed(markup)
        parser.close()
    except Exception:  # noqa: BLE001 — битая разметка не должна ломать чтение
        pass
    return parser.text(), parser.title.strip()


def fetch_page(url: str, timeout: int = 25, max_bytes: int = 3_000_000) -> tuple[int, str, str]:
    """Возвращает (статус, content-type, тело)."""
    request = urllib.request.Request(url, headers={
        "User-Agent": USER_AGENT,
        "Accept": "text/html,application/json,text/plain;q=0.9,*/*;q=0.8",
        "Accept-Language": "ru,en;q=0.8",
    })
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.status, response.headers.get("Content-Type", ""), response.read(max_bytes).decode(
            "utf-8", "replace"
        )


# --------------------------------------------------------------------------------------
# Инструменты
# --------------------------------------------------------------------------------------
def web_search(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    if not ctx.config.allow_web and not ctx.config.permissions.allow_web:
        raise ToolError("доступ в интернет отключён (allow_web=false)")
    query = str(args.get("query") or "").strip()
    if not query:
        raise ToolError("пустой поисковый запрос")
    limit = max(1, min(int(args.get("max_results") or 5), 15))

    outcome = search(query, limit=limit, timeout=ctx.config.timeout, engine=str(args.get("engine") or ""))
    body = format_results(outcome, query)
    ok = outcome.ok
    return ToolResult(ok, body,
                      display=f"web_search «{clip(query, 50)}» → {len(outcome.results)}" +
                              (f" ({outcome.engine})" if outcome.engine else ""),
                      meta={"engine": outcome.engine, "count": len(outcome.results),
                            "errors": outcome.errors})


def fetch_url(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    url = str(args.get("url") or "").strip()
    if not url:
        raise ToolError("нужен параметр url")
    if not url.startswith(("http://", "https://")):
        url = "https://" + url

    max_chars = max(500, min(int(args.get("max_chars") or MAX_PAGE_CHARS), 200_000))
    start = max(0, int(args.get("start") or 0))
    want_json = bool(args.get("json"))

    try:
        status, content_type, body = fetch_page(url, timeout=ctx.config.timeout)
    except urllib.error.HTTPError as e:
        return ToolResult.error(f"{url} → HTTP {e.code} {e.reason}")
    except urllib.error.URLError as e:
        return ToolResult.error(f"не удалось открыть {url}: {e.reason}")
    except (OSError, ValueError) as e:
        return ToolResult.error(f"не удалось открыть {url}: {e}")

    title = ""
    if want_json or "json" in content_type:
        try:
            text = json.dumps(json.loads(body), ensure_ascii=False, indent=2)
        except json.JSONDecodeError:
            text = body
    elif "html" in content_type or "<html" in body[:800].lower():
        text, title = html_to_text(body)
    else:
        text = body

    total = len(text)
    chunk = text[start:start + max_chars]
    tail = ""
    if start + max_chars < total:
        tail = (f"\n\n[показано {start}–{start + max_chars} из {total} символов. "
                f"Продолжение: fetch_url url={url} start={start + max_chars}]")
    head = f"{title}\n" if args.get("show_title", True) and title else ""
    return ToolResult.done(
        f"{head}Источник: {url} (HTTP {status})\n{'-' * 60}\n{chunk}{tail}",
        display=f"fetch_url {clip(url, 60)} ({total} симв.)",
        url=url, chars=total, status=status,
    )


def http_request(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    """Прямой вызов API: GET/POST/PUT/DELETE с заголовками и телом."""
    url = str(args.get("url") or "").strip()
    if not url:
        raise ToolError("нужен параметр url")
    if not url.startswith(("http://", "https://")):
        url = "https://" + url
    method = str(args.get("method") or "GET").upper()
    headers = {str(k): str(v) for k, v in (args.get("headers") or {}).items()}
    headers.setdefault("User-Agent", USER_AGENT)
    headers.setdefault("Accept", "application/json")

    payload: bytes | None = None
    if args.get("json_body") is not None:
        payload = json.dumps(args["json_body"], ensure_ascii=False).encode("utf-8")
        headers.setdefault("Content-Type", "application/json")
    elif args.get("data") is not None:
        payload = str(args["data"]).encode("utf-8")

    if args.get("params"):
        url += ("&" if "?" in url else "?") + urllib.parse.urlencode(args["params"])

    request = urllib.request.Request(url, data=payload, method=method)
    for key, value in headers.items():
        request.add_header(key, value)

    try:
        with urllib.request.urlopen(request, timeout=int(args.get("timeout") or ctx.config.timeout)) as response:
            status, body = response.status, response.read(2_000_000).decode("utf-8", "replace")
            content_type = response.headers.get("Content-Type", "")
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", "replace") if hasattr(e, "read") else ""
        return ToolResult.error(f"{method} {url} → HTTP {e.code}\n{clip(body, 2000)}")
    except (urllib.error.URLError, OSError, ValueError) as e:
        return ToolResult.error(f"{method} {url} → {e}")

    pretty = body
    if "json" in content_type:
        try:
            pretty = json.dumps(json.loads(body), ensure_ascii=False, indent=2)
        except json.JSONDecodeError:
            pass
    return ToolResult.done(f"{method} {url} → HTTP {status}\n{'-' * 40}\n{clip(pretty, 20000)}",
                           display=f"http_request {method} {clip(url, 50)} → {status}",
                           status=status)


def wikipedia_lookup(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    query = str(args.get("query") or "").strip()
    if not query:
        raise ToolError("нужен параметр query")
    language = str(args.get("language") or "")

    if args.get("full") or args.get("title"):
        text = wikipedia_page(str(args.get("title") or query), timeout=ctx.config.timeout,
                              language=language)
        if text:
            return ToolResult.done(clip(text, 12_000), display=f"wikipedia {clip(query, 40)}")
    result = search(query, limit=int(args.get("max_results") or 3),
                    timeout=ctx.config.timeout, engine="wikipedia")
    if not result.ok:
        return ToolResult.error(f"Wikipedia недоступна: {'; '.join(result.errors[:2])}")
    parts = []
    for item in result.results:
        page = wikipedia_page(item.title, timeout=ctx.config.timeout, language=language)
        parts.append(page or f"{item.title}\n{item.url}\n{item.snippet}")
    return ToolResult.done("\n\n".join(parts)[:20_000], display=f"wikipedia {clip(query, 40)}")


def site_links(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
    """Собирает ссылки со страницы — полезно для исследования сайтов."""
    url = str(args.get("url") or "").strip()
    if not url.startswith(("http://", "https://")):
        url = "https://" + url
    try:
        _, content_type, body = fetch_page(url, timeout=ctx.config.timeout)
    except (urllib.error.URLError, OSError, ValueError) as e:
        return ToolResult.error(f"не удалось открыть {url}: {e}")
    if "html" not in content_type:
        return ToolResult.error("страница не HTML — ссылки не собрать")
    parser = _Extractor()
    parser.feed(body)
    parser.close()
    urls = []
    for href, _ in parser.links[:400]:
        if args.get("same_domain", True):
            if urllib.parse.urlparse(href).netloc != urllib.parse.urlparse(url).netloc:
                continue
        if href not in urls:
            urls.append(href)
    limit = int(args.get("max_results") or 80)
    body_text = "\n".join(urls[:limit]) or "(ссылок не найдено)"
    return ToolResult.done(f"ссылки с {url}:\n{body_text}", display=f"site_links {clip(url, 50)} ({len(urls)})")


TOOLS = [
    Tool("web_search", "Найти информацию в интернете (DuckDuckGo/SearX/StackOverflow) без API-ключей.",
         {"query": {"type": "string", "description": "поисковый запрос"},
          "max_results": {"type": "integer", "description": "сколько результатов (1-15)"},
          "engine": {"type": "string", "description": "конкретный движок: duckduckgo, searx, stackoverflow, wikipedia, hackernews"}},
         web_search, required=("query",), category="web"),

    Tool("fetch_url", "Открыть веб-страницу и прочитать её содержимое как текст/JSON.",
         {"url": {"type": "string", "description": "адрес страницы"},
          "max_chars": {"type": "integer", "description": "сколько символов вернуть"},
          "start": {"type": "integer", "description": "с какого символа начать (для длинных страниц)"},
          "json": {"type": "boolean", "description": "вернуть JSON-ответ целиком"}},
         fetch_url, required=("url",), category="web"),

    Tool("http_request", "Прямой HTTP-запрос к API (GET/POST/PUT/DELETE) с заголовками и JSON.",
         {"url": {"type": "string", "description": "адрес"},
          "method": {"type": "string", "description": "HTTP-метод"},
          "headers": {"type": "object", "description": "заголовки запроса"},
          "json_body": {"type": "object", "description": "тело запроса в JSON"},
          "data": {"type": "string", "description": "тело запроса текстом"},
          "params": {"type": "object", "description": "query-параметры"},
          "timeout": {"type": "integer", "description": "таймаут в секундах"}},
         http_request, required=("url",), dangerous=True, category="web"),

    Tool("wikipedia", "Справка из Wikipedia: краткая или полная статья по термину.",
         {"query": {"type": "string", "description": "термин или тема"},
          "title": {"type": "string", "description": "точное название статьи"},
          "full": {"type": "boolean", "description": "вернуть полный текст статьи"},
          "language": {"type": "string", "description": "ru или en"}},
         wikipedia_lookup, required=("query",), category="web"),

    Tool("site_links", "Собрать ссылки со страницы (исследование сайта или документации).",
         {"url": {"type": "string", "description": "адрес страницы"},
          "same_domain": {"type": "boolean", "description": "только ссылки того же домена"},
          "max_results": {"type": "integer", "description": "максимум ссылок"}},
         site_links, required=("url",), category="web"),
]

__all__ = ["TOOLS", "fetch_page", "fetch_url", "html_to_text", "http_request", "web_search"]
