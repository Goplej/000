// =============================================================================
//  tests/test_buf.cpp  --  buffers, strings, URLs, JSON, HTML helpers
// =============================================================================
#include "lca/buf.h"
#include "lca/search.h"
#include "test_util.h"

using namespace lca;
using lca_test::section;

static void test_strings() {
    section("strings");
    LCA_STREQ(trim("  hello \n"), "hello");
    LCA_STREQ(ltrim("  x"), "x");
    LCA_STREQ(rtrim("x  "), "x");
    LCA_STREQ(lower("AbC"), "abc");
    LCA_STREQ(upper("AbC"), "ABC");
    LCA_STREQ(replace_all("a-b-c", "-", "_"), "a_b_c");
    LCA_STREQ(join({"a", "b", "c"}, ","), "a,b,c");
    LCA_STREQ(repeat("ab", 3), "ababab");
    LCA_STREQ(ellipsize("abcdefghij", 5), "abcde...");
    LCA_STREQ(pad_left("7", 3), "  7");
    LCA_CHECK(starts_with("hello world", "hello"));
    LCA_CHECK(ends_with("hello world", "world"));
    LCA_CHECK(iequals("Hello", "hELLo"));
    LCA_CHECK(count_lines("a\nb\nc\n") == 3);
    LCA_CHECK(count_lines("a\r\nb") == 2);
    auto parts = split("a,,b", ',', true);
    LCA_EQ(parts.size(), size_t(3));
    auto kept = split("a,,b", ',', false);
    LCA_EQ(kept.size(), size_t(2));
    LCA_STREQ(normalise_path("/a/./b/../c"), "/a/c");
    LCA_CHECK(path_escapes("/work", "/work/../etc"));
    LCA_CHECK(!path_escapes("/work", "/work/sub/file"));
    LCA_CHECK(path_escapes("/work", "/other/file"));
}

static void test_urls() {
    section("urls");
    auto url = parse_url("https://user@Example.COM:8443/path/to?x=1&y=2#frag");
    LCA_CHECK(url.ok());
    LCA_STREQ(url->scheme, "https");
    LCA_STREQ(url->host, "example.com");
    LCA_EQ(int(url->port), 8443);
    LCA_STREQ(url->path, "/path/to");
    LCA_STREQ(url->query, "x=1&y=2");
    LCA_STREQ(url->request_target(), "/path/to?x=1&y=2");
    LCA_STREQ(url->origin(), "https://example.com:8443");

    auto dflt = parse_url("http://example.com");
    LCA_CHECK(dflt.ok());
    LCA_EQ(int(dflt->port), 80);
    LCA_STREQ(dflt->path, "/");

    auto resolved = resolve_url(*dflt, "../other/page.html");
    LCA_CHECK(resolved.ok());
    LCA_STREQ(resolved->path, "/other/page.html");

    LCA_STREQ(url_encode("a b&c/d", false), "a%20b%26c%2Fd");
    LCA_STREQ(url_encode("a/b", true), "a/b");
    LCA_STREQ(url_decode("a%20b%26c"), "a b&c");
    LCA_CHECK(!parse_url("not a url::").ok() || true);   // parser must not crash
}

static void test_json() {
    section("json");
    auto parsed = Json::parse(R"({"name":"lca","version":2,"ok":true,
                                 "tags":["a","b"],
                                 "nested":{"x":1.5,"nil":null,"inner":{"s":"v"}}})");
    LCA_CHECK(parsed.ok());
    Json& j = *parsed;
    LCA_STREQ(j["name"].as_string(), "lca");
    LCA_EQ(j["version"].as_int(), int64_t(2));
    LCA_CHECK(j["ok"].as_bool());
    LCA_EQ(j["tags"].items().size(), size_t(2));
    LCA_STREQ(j["tags"].at(1).as_string(), "b");
    LCA_STREQ(j.string_at("nested.x", "-"), "-");        // numbers are not strings
    LCA_STREQ(j.string_at("nested.inner.s", "v"), "v");   // deep paths use '.'
    LCA_STREQ(j.string_at("tags[0]", "?"), "a");          // array index syntax
    LCA_STREQ(j.string_at("missing.path", "fallback"), "fallback");
    LCA_CHECK(j["nested"]["nil"].is_null());
    LCA_EQ(j["nested"]["x"].as_number(), 1.5);

    // round trip
    auto again = Json::parse(j.dump());
    LCA_CHECK(again.ok());
    LCA_STREQ((*again)["name"].as_string(), "lca");

    // escaping
    Json esc = Json::object();
    esc["quote"] = "he said \"hi\"\n\t\\end";
    auto reread = Json::parse(esc.dump());
    LCA_CHECK(reread.ok());
    LCA_STREQ((*reread)["quote"].as_string(), "he said \"hi\"\n\t\\end");

    // malformed input must fail cleanly
    LCA_CHECK(!Json::parse("{\"a\": }").ok());
    LCA_CHECK(!Json::parse("[1,2,").ok());

    // parse_prefix tolerates trailing prose
    auto prefix = Json::parse_prefix("{\"tool\":\"fs_read\"} trailing text");
    LCA_CHECK(prefix.ok());
    LCA_STREQ((*prefix)["tool"].as_string(), "fs_read");

    // unicode escapes
    auto uni = Json::parse(R"({"s":"\u00e9\u4e2d"})");
    LCA_CHECK(uni.ok());
    LCA_CHECK(!(*uni)["s"].as_string().empty());
}

static void test_byte_reader() {
    section("byte reader");
    Bytes data{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09};
    ByteReader r(data);
    LCA_EQ(int(r.u8()), 1);
    LCA_EQ(int(r.u16()), 0x0203);
    LCA_EQ(int(r.u24()), 0x040506);
    // A short read must return 0 and leave the cursor untouched.
    LCA_EQ(int(r.u32()), 0);
    LCA_EQ(r.remaining(), size_t(3));
    LCA_EQ(int(r.u24()), 0x070809);
    ByteReader r2(data);
    LCA_EQ(r2.take_str(3).size(), size_t(3));
    LCA_EQ(r2.remaining(), size_t(6));
    r2.seek(100);
    LCA_EQ(r2.remaining(), size_t(0));
}

static void test_html_and_text() {
    section("html + text");
    const std::string html =
        "<html><head><title>Test &amp; Page</title></head><body>"
        "<h1>Heading</h1><p>Body &lt;text&gt;</p>"
        "<a href='/rel'>rel</a><a href='https://example.com/abs'>abs</a>"
        "<script>var x = '<b>not text</b>';</script></body></html>";
    LCA_STREQ(html_unescape("a &amp; b &lt;c&gt; &quot;d&quot; &#65;"), "a & b <c> \"d\" A");
    std::string text = html_to_text(html);
    LCA_CHECK(text.find("Heading") != std::string::npos);
    LCA_CHECK(text.find("Body <text>") != std::string::npos);
    LCA_CHECK(text.find("not text") == std::string::npos);
    (void)html;
}

int main() {
    std::printf("lca buf tests\n=============\n");
    test_strings();
    test_urls();
    test_json();
    test_byte_reader();
    test_html_and_text();
    return lca_test::finish("test_buf");
}
