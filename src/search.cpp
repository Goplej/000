// =============================================================================
//  lca/search.cpp  --  search engines + page scraping
// =============================================================================
#include "lca/search.h"
#include "lca/fs_engine.h"
#include "lca/mem.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <thread>

namespace lca {
namespace {

constexpr const char* kScope = "search";
constexpr const char* kDefaultUserAgent =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/124.0 Safari/537.36 lca/1.0";

// -----------------------------------------------------------------------------
// HTML tokeniser helpers
// -----------------------------------------------------------------------------
struct Tag {
    std::string name;
    std::string attrs;
    bool        closing{false};
    bool        self_closing{false};
};

std::string unwrap_redirect(const std::string& url);

bool parse_tag(const std::string& html, size_t start, Tag* tag, size_t* end_index) {
    if (start >= html.size() || html[start] != '<') return false;
    size_t i = start + 1;
    if (i < html.size() && html[i] == '/') { tag->closing = true; ++i; }
    else if (i < html.size() && (html[i] == '!' || html[i] == '?')) {
        // comment / doctype / processing instruction
        size_t close = html.find(tag->closing ? '>' : '>', i);
        if (close == std::string::npos) return false;
        *end_index = close + 1;
        tag->name = "";
        return true;
    }
    size_t name_start = i;
    while (i < html.size() && (std::isalnum((unsigned char)html[i]) || html[i] == '-' ||
                               html[i] == ':' || html[i] == '_'))
        ++i;
    tag->name = lower(html.substr(name_start, i - name_start));
    size_t attr_start = i;
    // Scan to the end of the tag, honouring quotes.
    bool in_quote = false;
    char quote = 0;
    while (i < html.size()) {
        char c = html[i];
        if (in_quote) {
            if (c == quote) in_quote = false;
        } else if (c == '"' || c == '\'') {
            in_quote = true;
            quote = c;
        } else if (c == '>') {
            break;
        }
        ++i;
    }
    if (i >= html.size()) return false;
    tag->attrs = html.substr(attr_start, i - attr_start);
    tag->self_closing = i > start && html[i - 1] == '/';
    *end_index = i + 1;
    return true;
}

std::string attr_value(const std::string& attrs, const std::string& name) {
    std::string needle = name + "=";
    size_t pos = 0;
    while ((pos = lower(attrs).find(needle, pos)) != std::string::npos) {
        // Ensure the match starts at a token boundary.
        if (pos > 0) {
            char prev = attrs[pos - 1];
            if (std::isalnum((unsigned char)prev) || prev == '-' || prev == '_' || prev == ':') {
                pos += needle.size();
                continue;
            }
        }
        size_t v = pos + needle.size();
        if (v >= attrs.size()) return {};
        char quote = attrs[v];
        if (quote == '"' || quote == '\'') {
            size_t end = attrs.find(quote, v + 1);
            if (end == std::string::npos) return {};
            return attrs.substr(v + 1, end - v - 1);
        }
        size_t end = v;
        while (end < attrs.size() && !std::isspace((unsigned char)attrs[end]) && attrs[end] != '>') ++end;
        return attrs.substr(v, end - v);
    }
    return {};
}

std::string collapse_ws(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool space = false;
    for (char c : s) {
        if (std::isspace((unsigned char)c)) {
            space = true;
            continue;
        }
        if (space && !out.empty()) out.push_back(' ');
        space = false;
        out.push_back(c);
    }
    return out;
}

// Removes tracking redirect wrappers (DuckDuckGo uses /l/?uddg=<encoded>).
std::string unwrap_redirect(const std::string& url) {
    Result<Url> parsed = parse_url(url);
    if (!parsed.ok()) return url;
    if (parsed->query.empty()) return url;
    for (const std::string& pair : split(parsed->query, '&')) {
        size_t eq = pair.find('=');
        if (eq == std::string::npos) continue;
        std::string key = pair.substr(0, eq);
        if (key == "uddg" || key == "url" || key == "u" || key == "q") {
            std::string value = url_decode(pair.substr(eq + 1));
            if (starts_with(value, "http://") || starts_with(value, "https://")) return value;
        }
    }
    return url;
}

std::string strip_tags(const std::string& html) {
    std::string out;
    size_t i = 0;
    while (i < html.size()) {
        if (html[i] == '<') {
            size_t close = html.find('>', i);
            if (close == std::string::npos) break;
            i = close + 1;
            continue;
        }
        out.push_back(html[i++]);
    }
    return collapse_ws(html_unescape(out));
}

}  // namespace

// -----------------------------------------------------------------------------
// HTML entity decoding
// -----------------------------------------------------------------------------
std::string html_unescape(const std::string& text) {
    static const std::map<std::string, std::string> kNamed = {
        {"amp", "&"},   {"lt", "<"},    {"gt", ">"},    {"quot", "\""}, {"apos", "'"},
        {"nbsp", " "},  {"hellip", "…"}, {"mdash", "—"}, {"ndash", "–"},
        {"lsquo", "'"}, {"rsquo", "'"}, {"ldquo", "\""}, {"rdquo", "\""},
        {"laquo", "«"}, {"raquo", "»"}, {"times", "×"},  {"divide", "÷"},
        {"copy", "©"},  {"reg", "®"},   {"trade", "™"},  {"deg", "°"},
        {"euro", "€"},  {"pound", "£"}, {"yen", "¥"},    {"cent", "¢"},
        {"middot", "·"},{"bull", "•"},  {"dagger", "†"}, {"sect", "§"},
        {"para", "¶"},  {"plusmn", "±"},{"frac12", "½"}, {"sup2", "²"}, {"sup3", "³"},
    };
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        if (text[i] != '&') { out.push_back(text[i++]); continue; }
        size_t semi = text.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 12) { out.push_back(text[i++]); continue; }
        std::string entity = text.substr(i + 1, semi - i - 1);
        if (entity.empty()) { out.push_back(text[i++]); continue; }
        if (entity[0] == '#') {
            unsigned long code = 0;
            if (entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X'))
                code = std::strtoul(entity.c_str() + 2, nullptr, 16);
            else
                code = std::strtoul(entity.c_str() + 1, nullptr, 10);
            if (code != 0 && code < 0x110000) {
                // UTF-8 encode
                if (code < 0x80) {
                    out.push_back(char(code));
                } else if (code < 0x800) {
                    out.push_back(char(0xC0 | (code >> 6)));
                    out.push_back(char(0x80 | (code & 0x3F)));
                } else if (code < 0x10000) {
                    out.push_back(char(0xE0 | (code >> 12)));
                    out.push_back(char(0x80 | ((code >> 6) & 0x3F)));
                    out.push_back(char(0x80 | (code & 0x3F)));
                } else {
                    out.push_back(char(0xF0 | (code >> 18)));
                    out.push_back(char(0x80 | ((code >> 12) & 0x3F)));
                    out.push_back(char(0x80 | ((code >> 6) & 0x3F)));
                    out.push_back(char(0x80 | (code & 0x3F)));
                }
                i = semi + 1;
                continue;
            }
        } else {
            auto it = kNamed.find(entity);
            if (it != kNamed.end()) {
                out += it->second;
                i = semi + 1;
                continue;
            }
        }
        out.push_back(text[i++]);
    }
    return out;
}

// -----------------------------------------------------------------------------
// HTML -> text
// -----------------------------------------------------------------------------
std::string html_to_text(const std::string& html) {
    static const std::vector<std::string> kBlock = {
        "p", "div", "li", "tr", "td", "th", "h1", "h2", "h3", "h4", "h5", "h6",
        "section", "article", "header", "footer", "nav", "aside", "main", "ul", "ol",
        "table", "thead", "tbody", "pre", "blockquote", "figure", "figcaption", "form",
        "br", "hr", "dl", "dt", "dd"};
    auto is_block = [&](const std::string& name) {
        for (const std::string& b : kBlock) if (b == name) return true;
        return false;
    };

    std::string out;
    out.reserve(html.size() / 2);
    size_t i = 0;
    std::string skip_tag;
    while (i < html.size()) {
        if (html[i] == '<') {
            Tag tag;
            size_t end = i;
            if (parse_tag(html, i, &tag, &end)) {
                if (!skip_tag.empty()) {
                    if (tag.closing && tag.name == skip_tag) skip_tag.clear();
                } else if (!tag.closing &&
                           (tag.name == "script" || tag.name == "style" || tag.name == "noscript" ||
                            tag.name == "svg" || tag.name == "template" || tag.name == "head")) {
                    skip_tag = tag.name;
                } else if (is_block(tag.name)) {
                    // A newline around block level structure, never inside a word.
                    if (!out.empty() && out.back() != '\n') out.push_back('\n');
                }
                i = end;
                continue;
            }
            ++i;
            continue;
        }
        if (skip_tag.empty()) out.push_back(html[i]);
        ++i;
    }
    out = html_unescape(out);

    // Collapse runs of spaces per line and drop empty lines.
    std::string cleaned;
    cleaned.reserve(out.size());
    std::string line;
    bool pending_space = false;
    auto flush = [&]() {
        std::string t = collapse_ws(line);
        line.clear();
        if (t.empty()) return;
        if (!cleaned.empty()) cleaned.push_back('\n');
        cleaned += t;
    };
    for (char c : out) {
        if (c == '\n' || c == '\r') { flush(); pending_space = false; continue; }
        if (c == ' ' || c == '\t') { pending_space = true; continue; }
        if (pending_space && !line.empty()) line.push_back(' ');
        pending_space = false;
        line.push_back(c);
    }
    flush();
    return cleaned;
}

std::string html_extract_title(const std::string& html) {
    std::string low = lower(html);
    size_t start = low.find("<title");
    if (start == std::string::npos) return {};
    size_t open_end = low.find('>', start);
    if (open_end == std::string::npos) return {};
    size_t close = low.find("</title", open_end);
    if (close == std::string::npos) return {};
    return collapse_ws(html_unescape(html.substr(open_end + 1, close - open_end - 1)));
}

std::vector<std::string> html_extract_links(const std::string& html, const std::string& base_url) {
    std::vector<std::string> links;
    Result<Url> base = parse_url(base_url);
    size_t i = 0;
    while (i < html.size()) {
        if (html[i] != '<') { ++i; continue; }
        Tag tag;
        size_t end = i;
        if (!parse_tag(html, i, &tag, &end)) { ++i; continue; }
        if (!tag.closing && (tag.name == "a" || tag.name == "area")) {
            std::string href = attr_value(tag.attrs, "href");
            if (!href.empty() && href[0] != '#' && !starts_with(href, "javascript:") &&
                !starts_with(href, "mailto:") && !starts_with(href, "data:")) {
                if (base.ok()) {
                    Result<Url> resolved = resolve_url(*base, href);
                    if (resolved.ok()) links.push_back(unwrap_redirect(resolved->to_string()));
                } else {
                    links.push_back(unwrap_redirect(href));
                }
            }
        }
        i = end;
    }
    std::sort(links.begin(), links.end());
    links.erase(std::unique(links.begin(), links.end()), links.end());
    return links;
}

std::vector<std::string> html_extract_headings(const std::string& html) {
    std::vector<std::string> out;
    size_t i = 0;
    std::string current;
    std::string current_tag;
    while (i < html.size()) {
        if (html[i] == '<') {
            Tag tag;
            size_t end = i;
            if (parse_tag(html, i, &tag, &end)) {
                if (!tag.closing && tag.name.size() == 2 && tag.name[0] == 'h' &&
                    tag.name[1] >= '1' && tag.name[1] <= '6') {
                    current.clear();
                    current_tag = tag.name;
                } else if (tag.closing && tag.name == current_tag && !current_tag.empty()) {
                    std::string text = collapse_ws(html_unescape(current));
                    if (!text.empty()) out.push_back(text);
                    current.clear();
                    current_tag.clear();
                }
                i = end;
                continue;
            }
        }
        if (!current_tag.empty()) current.push_back(html[i]);
        ++i;
    }
    return out;
}

std::map<std::string, std::string> html_extract_meta(const std::string& html) {
    std::map<std::string, std::string> meta;
    size_t i = 0;
    while (i < html.size()) {
        if (html[i] != '<') { ++i; continue; }
        Tag tag;
        size_t end = i;
        if (!parse_tag(html, i, &tag, &end)) { ++i; continue; }
        if (!tag.closing && tag.name == "meta") {
            std::string name = attr_value(tag.attrs, "name");
            if (name.empty()) name = attr_value(tag.attrs, "property");
            std::string content = attr_value(tag.attrs, "content");
            if (!name.empty() && !content.empty())
                meta[lower(name)] = html_unescape(content);
        }
        i = end;
    }
    return meta;
}

// -----------------------------------------------------------------------------
// Engine URLs and result parsing
// -----------------------------------------------------------------------------
std::string search_url_for(const std::string& engine, const std::string& query, int max_results,
                           bool safe_search, const std::string& region) {
    std::string q = url_encode(query);
    if (engine == "bing") {
        std::string url = "https://www.bing.com/search?q=" + q + "&count=" +
                          std::to_string(std::max(10, max_results * 2));
        if (safe_search) url += "&safeSearch=Moderate";
        if (!region.empty()) url += "&mkt=" + url_encode(region);
        return url;
    }
    if (engine == "google") {
        std::string url = "https://www.google.com/search?q=" + q + "&num=" +
                          std::to_string(std::max(10, max_results * 2)) + "&hl=en";
        if (safe_search) url += "&safe=active";
        return url;
    }
    if (engine == "wikipedia") {
        return "https://en.wikipedia.org/w/index.php?search=" + q + "&title=Special:Search&fulltext=1";
    }
    if (engine == "mojeek") {
        return "https://www.mojeek.com/search?q=" + q + (safe_search ? "&safe=1" : "");
    }
    // DuckDuckGo HTML endpoint (no JavaScript required).
    std::string url = "https://html.duckduckgo.com/html/?q=" + q;
    if (safe_search) url += "&kp=1";
    if (!region.empty()) url += "&kl=" + url_encode(region);
    return url;
}

namespace {

}  // namespace

void parse_search_html(const std::string& html, const std::string& engine,
                       std::vector<SearchResult>* out) {
    if (!out) return;
    std::string low = lower(html);

    auto push = [&](const std::string& title, const std::string& url, const std::string& snippet) {
        if (title.empty() || url.empty()) return;
        for (const SearchResult& existing : *out)
            if (existing.url == url) return;             // de-duplicate
        SearchResult r;
        r.title = collapse_ws(html_unescape(title));
        r.url = unwrap_redirect(html_unescape(url));
        r.snippet = collapse_ws(html_unescape(strip_tags(snippet)));
        r.engine = engine;
        r.rank = int(out->size()) + 1;
        if (!r.url.empty()) out->push_back(r);
    };

    if (engine == "duckduckgo" || engine == "auto") {
        // <a class="result__a" href="...">Title</a> ... <a class="result__snippet">…</a>
        size_t pos = 0;
        while ((pos = low.find("result__a", pos)) != std::string::npos) {
            size_t tag_start = html.rfind('<', pos);
            size_t tag_end = html.find('>', pos);
            if (tag_start == std::string::npos || tag_end == std::string::npos) break;
            Tag tag;
            size_t dummy = 0;
            if (!parse_tag(html, tag_start, &tag, &dummy)) { pos = tag_end + 1; continue; }
            std::string href = attr_value(tag.attrs, "href");
            size_t title_end = low.find("</a>", tag_end);
            std::string title = title_end == std::string::npos
                                    ? std::string()
                                    : html.substr(tag_end + 1, title_end - tag_end - 1);
            // Snippet: the next result__snippet occurrence.
            size_t snip_pos = low.find("result__snippet", tag_end);
            std::string snippet;
            if (snip_pos != std::string::npos) {
                size_t snip_end_tag = html.find('>', snip_pos);
                size_t snip_close = snip_end_tag == std::string::npos
                                        ? std::string::npos : low.find("</a>", snip_end_tag);
                if (snip_end_tag != std::string::npos && snip_close != std::string::npos)
                    snippet = html.substr(snip_end_tag + 1, snip_close - snip_end_tag - 1);
            }
            push(title, href, snippet);
            pos = tag_end + 1;
        }
        return;
    }

    if (engine == "bing") {
        // <li class="b_algo"><h2><a href="…">Title</a></h2><p>Snippet</p>
        size_t pos = 0;
        while ((pos = low.find("b_algo", pos)) != std::string::npos) {
            size_t block_end = low.find("</li>", pos);
            size_t h2 = low.find("<h2", pos);
            if (h2 == std::string::npos || (block_end != std::string::npos && h2 > block_end)) {
                pos += 6;
                continue;
            }
            size_t a = low.find("<a", h2);
            size_t a_end = a == std::string::npos ? std::string::npos : low.find('>', a);
            if (a == std::string::npos || a_end == std::string::npos) { pos = h2 + 3; continue; }
            Tag tag;
            size_t dummy = 0;
            parse_tag(html, a, &tag, &dummy);
            std::string href = attr_value(tag.attrs, "href");
            size_t title_end = low.find("</a>", a_end);
            std::string title = title_end == std::string::npos
                                    ? std::string() : html.substr(a_end + 1, title_end - a_end - 1);
            size_t p = low.find("<p", title_end == std::string::npos ? a_end : title_end);
            std::string snippet;
            if (p != std::string::npos && (block_end == std::string::npos || p < block_end)) {
                size_t p_end = low.find('>', p);
                size_t p_close = low.find("</p>", p_end);
                if (p_end != std::string::npos && p_close != std::string::npos)
                    snippet = html.substr(p_end + 1, p_close - p_end - 1);
            }
            push(title, href, snippet);
            pos = title_end == std::string::npos ? pos + 6 : title_end;
        }
        return;
    }

    if (engine == "google") {
        // Result links appear as <a href="/url?q=..."> or <a href="https://…">
        size_t pos = 0;
        while ((pos = low.find("<a ", pos)) != std::string::npos) {
            size_t tag_end = low.find('>', pos);
            if (tag_end == std::string::npos) break;
            Tag tag;
            size_t dummy = 0;
            if (!parse_tag(html, pos, &tag, &dummy)) { pos += 3; continue; }
            std::string href = attr_value(tag.attrs, "href");
            std::string cls = lower(attr_value(tag.attrs, "class"));
            if (!href.empty() && (starts_with(href, "/url?") || starts_with(href, "http"))) {
                if (!contains(cls, "fl") && !contains(cls, "translate") && !contains(href, "google.")) {
                    size_t title_end = low.find("</a>", tag_end);
                    std::string title = title_end == std::string::npos
                                            ? std::string()
                                            : html.substr(tag_end + 1, title_end - tag_end - 1);
                    if (starts_with(href, "/url?")) href = "https://www.google.com" + href;
                    if (!strip_tags(title).empty()) push(title, href, "");
                }
            }
            pos = tag_end;
        }
        return;
    }

    if (engine == "mojeek") {
        size_t pos = 0;
        while ((pos = low.find("class=\"t\"", pos)) != std::string::npos) {
            size_t a = low.find("<a", pos);
            size_t a_end = a == std::string::npos ? std::string::npos : low.find('>', a);
            if (a == std::string::npos || a_end == std::string::npos) break;
            Tag tag;
            size_t dummy = 0;
            parse_tag(html, a, &tag, &dummy);
            std::string href = attr_value(tag.attrs, "href");
            size_t title_end = low.find("</a>", a_end);
            std::string title = title_end == std::string::npos
                                    ? std::string() : html.substr(a_end + 1, title_end - a_end - 1);
            push(title, href, "");
            pos = title_end == std::string::npos ? pos + 8 : title_end;
        }
        return;
    }

    if (engine == "wikipedia") {
        // Search results: <li class="mw-search-result">…<a href="/wiki/…" title="…">Title</a>
        size_t pos = 0;
        while ((pos = low.find("mw-search-result", pos)) != std::string::npos) {
            size_t a = low.find("<a", pos);
            size_t a_end = a == std::string::npos ? std::string::npos : low.find('>', a);
            if (a == std::string::npos || a_end == std::string::npos) break;
            Tag tag;
            size_t dummy = 0;
            parse_tag(html, a, &tag, &dummy);
            std::string href = attr_value(tag.attrs, "href");
            std::string title = attr_value(tag.attrs, "title");
            if (title.empty()) {
                size_t title_end = low.find("</a>", a_end);
                if (title_end != std::string::npos)
                    title = html.substr(a_end + 1, title_end - a_end - 1);
            }
            if (!href.empty() && href[0] == '/') href = "https://en.wikipedia.org" + href;
            push(title, href, "");
            pos = a_end;
        }
        return;
    }

    // Unknown engine: fall back to the generic "first link of each block" scan.
    size_t pos = 0;
    while ((pos = low.find("<a ", pos)) != std::string::npos) {
        size_t tag_end = low.find('>', pos);
        if (tag_end == std::string::npos) break;
        Tag tag;
        size_t dummy = 0;
        parse_tag(html, pos, &tag, &dummy);
        std::string href = attr_value(tag.attrs, "href");
        if (starts_with(href, "http")) {
            size_t title_end = low.find("</a>", tag_end);
            if (title_end != std::string::npos)
                push(html.substr(tag_end + 1, title_end - tag_end - 1), href, "");
        }
        pos = tag_end;
    }
}

// -----------------------------------------------------------------------------
// SearchResult / SearchResponse formatting
// -----------------------------------------------------------------------------
Json SearchResult::to_json() const {
    Json j = Json::object();
    j["title"] = title;
    j["url"] = url;
    j["snippet"] = snippet;
    j["engine"] = engine;
    j["rank"] = int64_t(rank);
    return j;
}

std::string SearchResult::to_text() const {
    std::string s = std::to_string(rank) + ". " + title + "\n   " + url + "\n";
    if (!snippet.empty()) s += "   " + snippet + "\n";
    return s;
}

std::string SearchResponse::to_text(size_t max_results) const {
    std::string out = "query: " + query + " (" + engine + ", " +
                      human_duration(elapsed_ms) + ")\n\n";
    size_t shown = 0;
    for (const SearchResult& r : results) {
        if (max_results && shown >= max_results) break;
        out += r.to_text();
        ++shown;
    }
    if (results.empty()) {
        out += "no results";
        if (!error.empty()) out += ": " + error;
        out += "\n";
    }
    return out;
}

Json SearchResponse::to_json() const {
    Json j = Json::object();
    j["query"] = query;
    j["engine"] = engine;
    j["http_status"] = int64_t(http_status);
    j["elapsed_ms"] = int64_t(elapsed_ms);
    j["error"] = error;
    Json arr = Json::array();
    for (const SearchResult& r : results) arr.push_back(r.to_json());
    j["results"] = arr;
    Json tried = Json::array();
    for (const std::string& e : engines_tried) tried.push_back(Json(e));
    j["engines_tried"] = tried;
    return j;
}

// -----------------------------------------------------------------------------
// Live search
// -----------------------------------------------------------------------------
Result<SearchResponse> web_search(const SearchRequest& request) {
    if (trim(request.query).empty())
        return LCA_FAIL(Code::InvalidArgument, "empty search query");

    std::vector<std::string> engines;
    if (request.engine == "auto" || request.engine.empty())
        engines = {"duckduckgo", "mojeek", "bing", "wikipedia"};
    else
        engines = {request.engine};

    SearchResponse response;
    response.query = request.query;
    int64_t started = now_millis();
    std::string last_error;

    for (const std::string& engine : engines) {
        response.engines_tried.push_back(engine);
        std::string url = search_url_for(engine, request.query, request.max_results,
                                         request.safe_search, request.region);
        HttpRequest req;
        req.url = url;
        req.timeout_ms = request.timeout_ms;
        req.max_response_bytes = request.max_page_bytes;
        req.set_header("User-Agent", kDefaultUserAgent);
        req.set_header("Accept", "text/html,application/xhtml+xml");
        req.set_header("Accept-Language", "en-US,en;q=0.9");
        req.set_header("Connection", "close");

        polite_wait(parse_url(url).ok() ? parse_url(url)->host : "");
        Result<HttpResponse> http = http_request(req);
        if (!http.ok()) {
            last_error = "HTTP request failed: " + http.error().str();
            LCA_LOG_WARN(kScope, "search via " + engine + " failed: " + last_error);
            continue;
        }
        response.http_status = http->status;
        if (!http->ok()) {
            last_error = "engine returned HTTP " + std::to_string(http->status);
            continue;
        }
        response.results.clear();
        parse_search_html(http->body_string(), engine, &response.results);
        if (!response.results.empty()) {
            if (int(response.results.size()) > request.max_results)
                response.results.resize(size_t(request.max_results));
            response.engine = engine;
            response.elapsed_ms = now_millis() - started;

            if (request.fetch_snippets) {
                for (SearchResult& r : response.results) {
                    if (!r.snippet.empty()) continue;
                    Result<PageContent> page = fetch_page(r.url, FetchOptions{});
                    if (page.ok()) r.snippet = ellipsize(page->text, 240);
                }
            }
            return response;
        }
        last_error = "no results parsed from " + engine + " response";
    }

    response.elapsed_ms = now_millis() - started;
    response.error = last_error.empty() ? "no search engine returned results" : last_error;
    return response;   // not an error: the caller may want the diagnostics
}

// -----------------------------------------------------------------------------
// Page fetching
// -----------------------------------------------------------------------------
Result<PageContent> fetch_page(const std::string& url, const FetchOptions& options) {
    FetchOptions opts = options;
    if (opts.user_agent.empty()) opts.user_agent = kDefaultUserAgent;

    PageContent page;
    page.url = url;
    int64_t started = now_millis();

    Result<Url> parsed = parse_url(url);
    if (!parsed.ok()) return Error(parsed.error());
    if (opts.check_robots) {
        Result<RobotsRules> robots = robots_for(url, std::min(opts.timeout_ms, 10000));
        if (robots.ok() && !robots->allows(parsed->path)) {
            return LCA_FAIL(Code::PermissionDenied,
                            "robots.txt disallows " + parsed->path + " on " + parsed->host);
        }
    }

    HttpRequest req;
    req.url = url;
    req.timeout_ms = opts.timeout_ms;
    req.max_response_bytes = opts.max_bytes;
    req.set_header("User-Agent", opts.user_agent);
    req.set_header("Accept", "text/html,application/xhtml+xml,text/plain;q=0.9,*/*;q=0.8");
    req.set_header("Accept-Language", "en-US,en;q=0.9");
    req.set_header("Connection", "close");

    polite_wait(parsed->host);
    Result<HttpResponse> http = http_request(req);
    if (!http.ok()) return Error(http.error());

    page.status = http->status;
    page.final_url = http->final_url;
    page.bytes = http->bytes_received;
    page.elapsed_ms = now_millis() - started;
    Result<Url> final = parse_url(http->final_url.empty() ? url : http->final_url);
    std::string base = final.ok() ? final->to_string() : url;

    std::string content_type = lower(http->content_type());
    bool html = contains(content_type, "html") || content_type.empty();
    if (html) {
        page.html = http->body_string();
        page.title = html_extract_title(page.html);
        page.meta = html_extract_meta(page.html);
        page.links = html_extract_links(page.html, base);
        page.headings = html_extract_headings(page.html);
        page.text = html_to_text(page.html);
        if (opts.only_text) page.html.clear();
    } else {
        page.text = http->body_string();
        if (opts.keep_html) page.html = page.text;
    }
    page.truncated = http->bytes_received >= opts.max_bytes;
    return page;
}

Result<PageContent> scrape_url(const std::string& url, const FetchOptions& options) {
    return fetch_page(url, options);
}

std::string PageContent::to_text(size_t max_chars) const {
    std::string out;
    if (!title.empty()) out += title + "\n" + std::string(title.size(), '=') + "\n";
    out += "url: " + (final_url.empty() ? url : final_url) + "  (HTTP " +
           std::to_string(status) + ", " + human_duration(elapsed_ms) + ")\n\n";
    if (!headings.empty()) {
        out += "headings:\n";
        size_t shown = 0;
        for (const std::string& h : headings) {
            if (shown++ >= 12) break;
            out += "  - " + h + "\n";
        }
        out += "\n";
    }
    out += text.size() > max_chars ? text.substr(0, max_chars) + "\n...[truncated]" : text;
    return out;
}

Json PageContent::to_json() const {
    Json j = Json::object();
    j["url"] = url;
    j["final_url"] = final_url;
    j["title"] = title;
    j["text"] = text;
    j["status"] = int64_t(status);
    j["bytes"] = int64_t(bytes);
    j["elapsed_ms"] = int64_t(elapsed_ms);
    Json hrefs = Json::array();
    for (size_t i = 0; i < links.size() && i < 200; ++i) hrefs.push_back(Json(links[i]));
    j["links"] = hrefs;
    Json heads = Json::array();
    for (size_t i = 0; i < headings.size() && i < 200; ++i) heads.push_back(Json(headings[i]));
    j["headings"] = heads;
    Json meta_json = Json::object();
    for (const auto& kv : meta) meta_json[kv.first] = kv.second;
    j["meta"] = meta_json;
    return j;
}

// -----------------------------------------------------------------------------
// Snippets
// -----------------------------------------------------------------------------
std::vector<std::string> find_snippets(const std::string& text, const std::string& needle,
                                       size_t context_chars, size_t max_snippets) {
    std::vector<std::string> out;
    if (needle.empty()) return out;
    std::string hay = lower(text);
    std::string low_needle = lower(needle);
    size_t pos = 0;
    while (out.size() < max_snippets) {
        size_t at = hay.find(low_needle, pos);
        if (at == std::string::npos) break;
        size_t start = at > context_chars ? at - context_chars : 0;
        size_t end = std::min(text.size(), at + needle.size() + context_chars);
        std::string piece = text.substr(start, end - start);
        // Trim to a word boundary, but never cut into the match itself.
        if (start > 0) {
            size_t space = piece.find(' ');
            if (space != std::string::npos && space + 1 < piece.size() &&
                lower(piece.substr(space + 1)).find(low_needle) != std::string::npos)
                piece = piece.substr(space + 1);
        }
        out.push_back((start > 0 ? "... " : "") + collapse_ws(piece) + (end < text.size() ? " ..." : ""));
        pos = at + needle.size();
    }
    return out;
}

// -----------------------------------------------------------------------------
// robots.txt + politeness
// -----------------------------------------------------------------------------
bool RobotsRules::allows(const std::string& path) const {
    if (allow_all) return true;
    // Longest-match rule wins (the common interpretation).
    size_t best_len = 0;
    bool allowed = true;
    for (const std::string& d : disallow) {
        if (!d.empty() && starts_with(path, d) && d.size() >= best_len) {
            best_len = d.size();
            allowed = false;
        }
    }
    for (const std::string& a : allow) {
        if (!a.empty() && starts_with(path, a) && a.size() >= best_len) {
            best_len = a.size();
            allowed = true;
        }
    }
    return allowed;
}

RobotsRules parse_robots_txt(const std::string& body, const std::string& user_agent) {
    RobotsRules rules;
    rules.fetched = true;
    std::string target = lower(user_agent);
    size_t slash = target.find('/');
    if (slash != std::string::npos) target.resize(slash);

    bool in_star = false, in_specific = false, matched_any = false;
    for (const std::string& raw : split_lines(body)) {
        std::string line = raw;
        size_t comment = line.find('#');
        if (comment != std::string::npos) line.resize(comment);
        line = trim(line);
        if (line.empty()) continue;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = lower(trim(line.substr(0, colon)));
        std::string value = trim(line.substr(colon + 1));

        if (key == "user-agent") {
            std::string agent = lower(value);
            in_star = (agent == "*");
            in_specific = (!agent.empty() && agent != "*" && !target.empty() &&
                           target.find(agent) != std::string::npos);
            if (in_specific) matched_any = true;
            continue;
        }
        bool applies = in_star || in_specific;
        if (!applies) continue;
        if (key == "disallow") {
            if (!value.empty()) {
                rules.disallow.push_back(value);
                rules.allow_all = false;
            }
        } else if (key == "allow") {
            if (!value.empty() && !in_specific) rules.allow.push_back(value);
            else if (!value.empty()) rules.allow.push_back(value);
        } else if (key == "crawl-delay") {
            double seconds = std::strtod(value.c_str(), nullptr);
            if (seconds > 0) rules.crawl_delay_ms = int64_t(seconds * 1000.0);
        }
    }
    if (!matched_any && rules.disallow.empty()) rules.allow_all = true;
    return rules;
}

Result<RobotsRules> robots_for(const std::string& url, int timeout_ms) {
    static std::mutex mu;
    static std::map<std::string, RobotsRules> cache;

    Result<Url> parsed = parse_url(url);
    if (!parsed.ok()) return Error(parsed.error());
    std::string origin = parsed->origin();
    {
        std::lock_guard<std::mutex> lock(mu);
        auto it = cache.find(origin);
        if (it != cache.end()) return it->second;
    }

    HttpRequest req;
    req.url = origin + "/robots.txt";
    req.timeout_ms = timeout_ms;
    req.max_response_bytes = 256 * 1024;
    req.set_header("User-Agent", kDefaultUserAgent);
    req.set_header("Connection", "close");
    Result<HttpResponse> http = http_request(req);

    RobotsRules rules;
    rules.fetched = false;
    if (http.ok() && http->status == 200) {
        rules = parse_robots_txt(http->body_string(), kDefaultUserAgent);
    } else {
        rules.allow_all = true;      // missing robots.txt means "no restrictions"
    }
    {
        std::lock_guard<std::mutex> lock(mu);
        cache[origin] = rules;
    }
    return rules;
}

void polite_wait(const std::string& host, int64_t min_delay_ms) {
    if (host.empty() || min_delay_ms <= 0) return;
    static std::mutex mu;
    static std::map<std::string, int64_t> last_hit;
    int64_t wait_ms = 0;
    {
        std::lock_guard<std::mutex> lock(mu);
        int64_t now = now_millis();
        auto it = last_hit.find(host);
        if (it != last_hit.end()) {
            int64_t elapsed = now - it->second;
            if (elapsed < min_delay_ms) wait_ms = min_delay_ms - elapsed;
        }
        last_hit[host] = now + wait_ms;
    }
    if (wait_ms > 0) {
        LCA_LOG_TRACE(kScope, "politeness delay " + std::to_string(wait_ms) + "ms for " + host);
        std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
    }
}

}  // namespace lca
