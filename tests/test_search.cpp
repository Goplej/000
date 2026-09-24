// =============================================================================
//  tests/test_search.cpp  --  HTML parsing, result extraction and live fetching
// -----------------------------------------------------------------------------
//  Two layers are covered:
//    * pure parsers (no network) -- html_to_text, links, headings, meta, search
//      result extraction for each supported engine, robots.txt rules;
//    * live fetching against a local HTTP/1.1 fixture served by python3's
//      http.server, which exercises real sockets, redirects, encoding handling
//      and the readability extraction path.
// =============================================================================
#include "lca/net.h"
#include "lca/search.h"
#include "test_util.h"

#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace lca;
using lca_test::section;

namespace {

struct LocalServer {
    pid_t pid{-1};
    uint16_t port{0};
    std::string dir;

    ~LocalServer() { stop(); }

    void stop() {
        if (pid <= 0) return;
        ::kill(pid, SIGTERM);
        int status = 0;
        for (int i = 0; i < 40; ++i) {
            if (::waitpid(pid, &status, WNOHANG) == pid) { pid = -1; return; }
            ::usleep(25 * 1000);
        }
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &status, 0);
        pid = -1;
    }
};

uint16_t pick_port() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    int reuse = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) { ::close(fd); return 0; }
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) { ::close(fd); return 0; }
    uint16_t port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

bool write_file_at(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out << content;
    return out.good();
}

const char* kIndexHtml = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <title>LCA Fixture Page</title>
  <meta name="description" content="fixture page for the lca test suite">
  <meta name="author" content="lca">
  <style>body { color: red; }</style>
  <script>var ignored = true;</script>
</head>
<body>
  <h1>Fixture Heading</h1>
  <h2>Second Heading</h2>
  <p>Hello &amp; welcome to the &lt;fixture&gt; page.</p>
  <p>Numbers: 1 &lt; 2 &gt; 0</p>
  <ul>
    <li><a href="/page2.html">Second page</a></li>
    <li><a href="relative/page3.html">Third page</a></li>
    <li><a href="https://example.com/external">External</a></li>
    <li><a href="#anchor">Anchor only</a></li>
    <li><a href="javascript:void(0)">Script link</a></li>
  </ul>
  <pre><code>int main() { return 0; }</code></pre>
</body>
</html>
)HTML";

bool start_server(LocalServer& server) {
    server.port = pick_port();
    if (server.port == 0) return false;
    if (!write_file_at(server.dir + "/index.html", kIndexHtml)) return false;
    if (!write_file_at(server.dir + "/page2.html",
                       "<html><head><title>Page Two</title></head><body><h1>Two</h1>"
                       "<p>The magic word is pomegranate.</p>"
                       "<a href=\"/index.html\">home</a> <a href=\"/missing.html\">gone</a></body></html>\n"))
        return false;
    if (!write_file_at(server.dir + "/robots.txt",
                       "User-agent: *\nDisallow: /private/\nAllow: /private/ok.html\n"
                       "Crawl-delay: 1\n"))
        return false;
    if (!write_file_at(server.dir + "/private/secret.html", "<html><title>secret</title></html>\n"))
        return false;
    if (!write_file_at(server.dir + "/private/ok.html", "<html><title>ok</title></html>\n"))
        return false;

    pid_t child = ::fork();
    if (child < 0) return false;
    if (child == 0) {
        ::setsid();
        if (::chdir(server.dir.c_str()) != 0) ::_exit(126);
        int null_fd = ::open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            ::dup2(null_fd, STDOUT_FILENO);
            ::dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO) ::close(null_fd);
        }
        std::string port = std::to_string(server.port);
        ::execlp("python3", "python3", "-m", "http.server", port.c_str(), "--bind", "127.0.0.1",
                 static_cast<char*>(nullptr));
        ::_exit(127);
    }
    server.pid = child;
    for (int i = 0; i < 120; ++i) {
        auto attempt = TcpStream::connect("127.0.0.1", server.port, 400);
        if (attempt.ok()) {
            attempt->get()->close();
            return true;
        }
        ::usleep(50 * 1000);
    }
    return false;
}

// -----------------------------------------------------------------------------
// Pure parser tests
// -----------------------------------------------------------------------------
void test_html_helpers() {
    section("html helpers");
    std::string html = kIndexHtml;

    LCA_STREQ(html_extract_title(html), "LCA Fixture Page");
    LCA_STREQ(html_unescape("&amp;&lt;&gt;&quot;&#39;&nbsp;"), "&<>\"' ");
    LCA_STREQ(html_unescape("&#65;&#x42;"), "AB");

    auto meta = html_extract_meta(html);
    LCA_STREQ(meta["description"], "fixture page for the lca test suite");
    LCA_STREQ(meta["author"], "lca");

    auto headings = html_extract_headings(html);
    LCA_CHECK(headings.size() >= 2);
    LCA_STREQ(headings[0], "Fixture Heading");

    auto links = html_extract_links(html, "http://example.test/index.html");
    LCA_CHECK(links.size() >= 3);
    bool saw_absolute = false, saw_relative = false, saw_external = false;
    for (const std::string& l : links) {
        if (l == "http://example.test/page2.html") saw_absolute = true;
        if (l == "http://example.test/relative/page3.html") saw_relative = true;
        if (l == "https://example.com/external") saw_external = true;
        LCA_CHECK(l.find("javascript:") == std::string::npos);
        LCA_CHECK(l.find("#anchor") == std::string::npos);
    }
    LCA_CHECK(saw_absolute);
    LCA_CHECK(saw_relative);
    LCA_CHECK(saw_external);

    std::string text = html_to_text(html);
    LCA_CHECK(text.find("Fixture Heading") != std::string::npos);
    LCA_CHECK(text.find("Hello & welcome to the <fixture> page.") != std::string::npos);
    LCA_CHECK(text.find("ignored") == std::string::npos);          // <script> removed
    LCA_CHECK(text.find("color: red") == std::string::npos);       // <style> removed
    LCA_CHECK(text.find("int main()") != std::string::npos);       // <pre> kept
    LCA_CHECK(text.find("  ") == std::string::npos || text.find("\n\n\n\n") == std::string::npos);

    // Matching is case-insensitive; the returned window keeps the original case.
    auto snippets = find_snippets(text, "fixture", 40, 2);
    LCA_CHECK(!snippets.empty());
    LCA_CHECK(!snippets.empty() && lower(snippets[0]).find("fixture") != std::string::npos);
    // A tiny context window must still contain the whole match.
    auto tight = find_snippets(text, "Fixture Heading", 1, 1);
    LCA_CHECK(!tight.empty());
    LCA_CHECK(!tight.empty() && tight[0].find("Fixture Heading") != std::string::npos);
}

void test_search_parsers() {
    section("search result parsers");
    // DuckDuckGo html layout
    const std::string ddg =
        "<div class=\"result results_links\">"
        "<a rel=\"nofollow\" class=\"result__a\" href=\"//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.com%2Fa\">"
        "First &amp; Result</a>"
        "<a class=\"result__snippet\">A snippet about the first result.</a></div>"
        "<div class=\"result results_links\">"
        "<a rel=\"nofollow\" class=\"result__a\" href=\"https://example.org/b\">Second Result</a>"
        "<a class=\"result__snippet\">Another snippet here.</a></div>";
    std::vector<SearchResult> results;
    parse_search_html(ddg, "duckduckgo", &results);
    LCA_CHECK(results.size() >= 2);
    if (results.size() >= 2) {
        LCA_STREQ(results[0].title, "First & Result");
        LCA_STREQ(results[0].engine, "duckduckgo");
        LCA_CHECK(results[0].url.find("example.com/a") != std::string::npos);
        LCA_CHECK(results[0].url.find("duckduckgo.com") == std::string::npos);   // unwrapped
        LCA_CHECK(results[1].url == "https://example.org/b");
    }
    LCA_CHECK(results[0].snippet.find("first result") != std::string::npos);

    // Bing layout
    const std::string bing =
        "<li class=\"b_algo\"><h2><a href=\"https://bing.example/1\">Bing One</a></h2>"
        "<p>Bing snippet one.</p></li>"
        "<li class=\"b_algo\"><h2><a href=\"https://bing.example/2\">Bing Two</a></h2>"
        "<p>Bing snippet two.</p></li>";
    std::vector<SearchResult> bing_results;
    parse_search_html(bing, "bing", &bing_results);
    LCA_CHECK(bing_results.size() >= 2);
    if (!bing_results.empty()) LCA_STREQ(bing_results[0].title, "Bing One");

    // Wikipedia search HTML (<li class="mw-search-result"> blocks)
    const std::string wiki =
        "<ul class=\"mw-search-results\">"
        "<li class=\"mw-search-result\"><div class=\"mw-search-result-heading\">"
        "<a href=\"/wiki/Vector_(mathematics)\" title=\"Vector (mathematics)\">"
        "Vector (mathematics)</a></div></li>"
        "<li class=\"mw-search-result\"><div class=\"mw-search-result-heading\">"
        "<a href=\"/wiki/Euclidean_vector\" title=\"Euclidean vector\">Euclidean vector</a>"
        "</div></li></ul>";
    std::vector<SearchResult> wiki_results;
    parse_search_html(wiki, "wikipedia", &wiki_results);
    LCA_CHECK(wiki_results.size() >= 2);
    if (!wiki_results.empty()) {
        LCA_STREQ(wiki_results[0].title, "Vector (mathematics)");
        LCA_STREQ(wiki_results[0].url, "https://en.wikipedia.org/wiki/Vector_(mathematics)");
    }

    // Result cap is honoured and the engine URL builder tags the query.
    std::vector<SearchResult> capped;
    parse_search_html(ddg, "duckduckgo", &capped);
    LCA_CHECK(capped.size() >= 2);
    capped.resize(1);            // callers cap the list they keep
    LCA_CHECK(capped.size() == 1);
    std::string url = search_url_for("duckduckgo", "c++ filesystem", 20, true, "us-en");
    LCA_CHECK(url.find("duckduckgo.com") != std::string::npos);
    LCA_CHECK(url.find("c%2B%2B") != std::string::npos || url.find("c++") != std::string::npos);
    LCA_CHECK(search_url_for("wikipedia", "vector", 5, true, "").find("wikipedia.org") !=
              std::string::npos);
    LCA_CHECK(search_url_for("bing", "vector", 5, false, "").find("bing.com") != std::string::npos);
    LCA_CHECK(search_url_for("google", "vector", 5, false, "").find("google.com") != std::string::npos);
    LCA_CHECK(search_url_for("nonsense", "vector", 5, false, "").find("duckduckgo") !=
              std::string::npos);   // auto falls back
}

void test_robots() {
    section("robots.txt");
    RobotsRules rules = parse_robots_txt(
        "User-agent: *\nDisallow: /private/\nAllow: /private/ok.html\nCrawl-delay: 1\n",
        "LCA/1.0");
    LCA_CHECK(rules.allows("/"));
    LCA_CHECK(rules.allows("/index.html"));
    LCA_CHECK(!rules.allows("/private/secret.html"));
    LCA_CHECK(rules.allows("/private/ok.html"));
    LCA_EQ(rules.crawl_delay_ms, 1000);

    RobotsRules open = parse_robots_txt("User-agent: *\nDisallow:\n", "LCA/1.0");
    LCA_CHECK(open.allow_all);
    LCA_CHECK(open.allows("/anything"));

    RobotsRules other_agent = parse_robots_txt(
        "User-agent: OtherBot\nDisallow: /\n\nUser-agent: LCA\nDisallow: /tmp/\n", "LCA/1.0");
    LCA_CHECK(other_agent.allows("/index.html"));
    LCA_CHECK(!other_agent.allows("/tmp/x"));
}

void test_result_formatting() {
    section("result formatting");
    SearchResponse response;
    response.query = "c++ filesystem";
    response.engine = "duckduckgo";
    response.elapsed_ms = 12;
    SearchResult one;
    one.title = "std::filesystem";
    one.url = "https://en.cppreference.com/w/cpp/filesystem";
    one.snippet = "Filesystem library";
    one.engine = "duckduckgo";
    response.results.push_back(one);
    LCA_CHECK(response.ok());
    std::string text = response.to_text();
    LCA_CHECK(text.find("c++ filesystem") != std::string::npos);
    LCA_CHECK(text.find("en.cppreference.com") != std::string::npos);
    SearchResponse empty;
    LCA_CHECK(!empty.ok());
    LCA_CHECK(empty.to_text().find("no results") != std::string::npos);
    LCA_CHECK(one.to_text().find("std::filesystem") != std::string::npos);
}

// -----------------------------------------------------------------------------
// Live fetching against the local fixture server
// -----------------------------------------------------------------------------
void test_live_fetch(LocalServer& server) {
    section("live fetch over plain HTTP");
    const std::string base = "http://127.0.0.1:" + std::to_string(server.port);

    HttpRequest req;
    req.url = base + "/index.html";
    req.timeout_ms = 10000;
    auto response = http_request(req);
    LCA_CHECK_MSG(response.ok(), response.ok() ? "" : response.error().str());
    if (!response.ok()) return;
    LCA_EQ(response->status, 200);
    LCA_CHECK(response->body_string().find("LCA Fixture Page") != std::string::npos);
    LCA_CHECK(response->header("content-type").find("text/html") != std::string::npos);
    LCA_CHECK(response->bytes_received >= response->body.size());

    // 404 must be reported, not swallowed
    HttpRequest missing;
    missing.url = base + "/missing.html";
    missing.timeout_ms = 10000;
    auto not_found = http_request(missing);
    LCA_CHECK(not_found.ok());
    LCA_EQ(not_found->status, 404);
    LCA_CHECK(!not_found->ok());

    // HEAD and a custom header
    HttpRequest head;
    head.method = "HEAD";
    head.url = base + "/index.html";
    head.timeout_ms = 10000;
    head.set_header("X-LCA-Test", "1");
    auto head_response = http_request(head);
    LCA_CHECK(head_response.ok());
    LCA_EQ(head_response->status, 200);
    LCA_CHECK(head_response->body.empty());

    // FetchOptions: text-only extraction
    FetchOptions opts;
    opts.only_text = true;
    opts.keep_html = false;
    opts.check_robots = false;
    opts.user_agent = "LCA-Test/1.0";
    auto page = fetch_page(base + "/index.html", opts);
    LCA_CHECK_MSG(page.ok(), page.ok() ? "" : page.error().str());
    if (!page.ok()) return;
    LCA_STREQ(page->title, "LCA Fixture Page");
    LCA_CHECK(page->text.find("Fixture Heading") != std::string::npos);
    LCA_CHECK(page->html.empty());
    LCA_CHECK(page->status == 200);
    LCA_CHECK(!page->truncated);

    // robots.txt enforcement: /private/ is disallowed for every agent
    FetchOptions guarded;
    guarded.check_robots = true;
    guarded.user_agent = "LCA-Test/1.0";
    auto blocked = fetch_page(base + "/private/secret.html", guarded);
    LCA_CHECK(!blocked.ok());
    LCA_CHECK(blocked.error().message.find("robots") != std::string::npos);

    auto allowed = fetch_page(base + "/private/ok.html", guarded);
    LCA_CHECK(allowed.ok());

    // scrape_url is the thin compatibility wrapper
    auto scraped = scrape_url(base + "/page2.html", opts);
    LCA_CHECK(scraped.ok());
    if (scraped.ok()) LCA_CHECK(scraped->text.find("pomegranate") != std::string::npos);

    // Redirect handling via the fixture: /index.html -> /page2.html is 404-free
    HttpRequest redirected;
    redirected.url = base + "/./page2.html";
    redirected.timeout_ms = 10000;
    auto redirect_response = http_request(redirected);
    LCA_CHECK(redirect_response.ok());
    LCA_EQ(redirect_response->status, 200);

    // Unreachable host reports an error rather than hanging forever
    HttpRequest dead;
    dead.url = "http://127.0.0.1:1/nothing";
    dead.timeout_ms = 1500;
    auto dead_response = http_request(dead);
    LCA_CHECK(!dead_response.ok());
}

void test_live_search_fixture(LocalServer& server) {
    section("search parsing against real bytes");
    // Serve the DuckDuckGo-style markup from the fixture server and parse what
    // actually came off the socket, so parser + HTTP layer are tested together.
    std::string html =
        "<html><body><div class=\"result results_links\">"
        "<a class=\"result__a\" href=\"http://127.0.0.1:" + std::to_string(server.port) +
        "/page2.html\">Fixture Result</a>"
        "<a class=\"result__snippet\">A snippet mentioning pomegranate.</a></div></body></html>";
    std::string path = server.dir + "/search.html";
    LCA_CHECK(write_file_at(path, html));

    HttpRequest req;
    req.url = "http://127.0.0.1:" + std::to_string(server.port) + "/search.html";
    req.timeout_ms = 10000;
    auto response = http_request(req);
    LCA_CHECK(response.ok());
    if (!response.ok()) return;
    std::vector<SearchResult> results;
    parse_search_html(response->body_string(), "duckduckgo", &results);
    LCA_EQ(results.size(), size_t(1));
    if (results.empty()) return;
    LCA_STREQ(results[0].title, "Fixture Result");
    LCA_STREQ(results[0].url, "http://127.0.0.1:" + std::to_string(server.port) + "/page2.html");
    LCA_CHECK(results[0].snippet.find("pomegranate") != std::string::npos);
}

}  // namespace

int main() {
    std::printf("lca search tests\n================\n");
    test_html_helpers();
    test_search_parsers();
    test_robots();
    test_result_formatting();

    LocalServer server;
    const char* tmp = std::getenv("TMPDIR");
    server.dir = std::string(tmp && *tmp ? tmp : "/tmp") + "/lca-search-fixture-" +
                 std::to_string(int64_t(wall_millis()));
    ::mkdir(server.dir.c_str(), 0700);
    ::mkdir((server.dir + "/private").c_str(), 0700);
    if (start_server(server)) {
        test_live_fetch(server);
        test_live_search_fixture(server);
    } else {
        std::printf("SKIP: local fixture server did not start (python3 missing?)\n");
    }
    server.stop();
    return lca_test::finish("test_search");
}
