// =============================================================================
//  lca/search.h  --  web search and live page scraping
// -----------------------------------------------------------------------------
//  Search engines are queried the same way a browser would be: a raw HTTP(S)
//  request over the in-tree TLS 1.3 client, then HTML parsing in `buf`.  No
//  search API keys, no third-party SDK, no paid service of any kind.
//
//  The parsing half of this module is pure (string in, structures out) so it is
//  unit-testable without network access; only `web_search()` and `fetch_page()`
//  touch the network.
// =============================================================================
#ifndef LCA_SEARCH_H
#define LCA_SEARCH_H

#include "lca/buf.h"
#include "lca/common.h"
#include "lca/net.h"

namespace lca {

// -----------------------------------------------------------------------------
// Results
// -----------------------------------------------------------------------------
struct SearchResult {
    std::string title;
    std::string url;
    std::string snippet;
    std::string engine;
    int         rank{0};

    Json to_json() const;
    std::string to_text() const;
};

struct SearchRequest {
    std::string query;
    std::string engine{"duckduckgo"};     // duckduckgo | bing | google | wikipedia | auto
    int         max_results{8};
    int         timeout_ms{20000};
    bool        safe_search{true};
    std::string region;                   // e.g. "us-en"
    bool        fetch_snippets{false};    // fetch each result page for more context
    size_t      max_page_bytes{1u << 20};
};

struct SearchResponse {
    std::string              query;
    std::string              engine;
    std::vector<SearchResult> results;
    std::string              error;       // populated when the engine refused
    int                      http_status{0};
    int64_t                  elapsed_ms{0};
    std::vector<std::string> engines_tried;

    bool ok() const { return !results.empty(); }
    std::string to_text(size_t max_results = 0) const;
    Json        to_json() const;
};

// Performs a live search.  With engine == "auto" the engines are tried in order
// until one returns parseable results.
Result<SearchResponse> web_search(const SearchRequest& request);

// -----------------------------------------------------------------------------
// Page fetching / scraping
// -----------------------------------------------------------------------------
struct PageContent {
    std::string              url;
    std::string              final_url;
    std::string              title;
    std::string              text;             // readable text, scripts/styles removed
    std::string              html;             // raw body (possibly truncated)
    std::vector<std::string> links;            // absolute URLs
    std::vector<std::string> headings;
    std::map<std::string, std::string> meta;
    int                      status{0};
    size_t                   bytes{0};
    int64_t                  elapsed_ms{0};
    bool                     truncated{false};

    Json        to_json() const;
    std::string to_text(size_t max_chars = 20000) const;
};

struct FetchOptions {
    int         timeout_ms{20000};
    size_t      max_bytes{1u << 20};
    bool        keep_html{true};
    bool        only_text{false};
    bool        check_robots{true};
    std::string user_agent;
};

Result<PageContent> fetch_page(const std::string& url, const FetchOptions& options = {});

// Follows redirects-free raw fetch (the HTTP layer already follows redirects).
Result<PageContent> scrape_url(const std::string& url, const FetchOptions& options = {});

// -----------------------------------------------------------------------------
// Pure parsing helpers (offline, unit-tested)
// -----------------------------------------------------------------------------
std::string              html_to_text(const std::string& html);
std::string              html_unescape(const std::string& text);
std::string              html_extract_title(const std::string& html);
std::vector<std::string> html_extract_links(const std::string& html, const std::string& base_url);
std::vector<std::string> html_extract_headings(const std::string& html);
std::map<std::string, std::string> html_extract_meta(const std::string& html);

// Parses a results page of the given engine into structured results.
void parse_search_html(const std::string& html, const std::string& engine,
                       std::vector<SearchResult>* out);

// Builds the engine-specific query URL (exposed for tests and for the CLI).
std::string search_url_for(const std::string& engine, const std::string& query, int max_results,
                           bool safe_search, const std::string& region);

// Extracts `needle` occurrences with surrounding context from a text blob.
std::vector<std::string> find_snippets(const std::string& text, const std::string& needle,
                                       size_t context_chars = 120, size_t max_snippets = 5);

// -----------------------------------------------------------------------------
// Politeness
// -----------------------------------------------------------------------------
struct RobotsRules {
    bool                     fetched{false};
    bool                     allow_all{true};
    std::vector<std::string> disallow;
    std::vector<std::string> allow;
    int64_t                  crawl_delay_ms{0};

    bool allows(const std::string& path) const;
};

// Fetches and parses /robots.txt for the origin of `url` (cached per host).
Result<RobotsRules> robots_for(const std::string& url, int timeout_ms = 10000);

// Parses robots.txt content (pure).
RobotsRules parse_robots_txt(const std::string& body, const std::string& user_agent);

// Blocks so that consecutive requests to one host respect a minimum delay.
void polite_wait(const std::string& host, int64_t min_delay_ms = 1000);

}  // namespace lca

#endif  // LCA_SEARCH_H
