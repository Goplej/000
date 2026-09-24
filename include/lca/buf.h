// =============================================================================
//  lca/buf.h  --  byte cursors, URL handling, JSON, string utilities
// -----------------------------------------------------------------------------
//  A tiny, allocation-frugal toolkit shared by the network, TLS and agent
//  layers.  The JSON implementation is a complete recursive-descent parser and
//  serializer with stable key ordering (std::map) so that cached prompt prefixes
//  hash identically between runs, which keeps KV-cache reuse predictable.
// =============================================================================
#ifndef LCA_BUF_H
#define LCA_BUF_H

#include "lca/common.h"

#include <map>
#include <sstream>

namespace lca {

// -----------------------------------------------------------------------------
// ByteReader -- bounds-checked cursor over a byte range
// -----------------------------------------------------------------------------
class ByteReader {
public:
    ByteReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}
    explicit ByteReader(const Bytes& b) : data_(b.data()), size_(b.size()) {}

    size_t remaining() const { return size_ - pos_; }
    size_t position()  const { return pos_; }
    bool   empty()     const { return remaining() == 0; }
    const uint8_t* cursor() const { return data_ + pos_; }

    bool need(size_t n) const { return remaining() >= n; }
    bool skip(size_t n) { if (n > remaining()) return false; pos_ += n; return true; }
    void seek(size_t p) { pos_ = p <= size_ ? p : size_; }

    uint8_t  u8()  { return need(1)  ? data_[pos_++] : 0; }
    uint16_t u16() { if (!need(2)) return 0; uint16_t v = read_u16(data_ + pos_); pos_ += 2; return v; }
    uint32_t u24() { if (!need(3)) return 0; uint32_t v = read_u24(data_ + pos_); pos_ += 3; return v; }
    uint32_t u32() { if (!need(4)) return 0; uint32_t v = read_u32(data_ + pos_); pos_ += 4; return v; }
    uint64_t u64() { if (!need(8)) return 0; uint64_t v = read_u64(data_ + pos_); pos_ += 8; return v; }

    // TLS-style vectors
    Bytes vec8()  { size_t n = u8();  return take(n); }
    Bytes vec16() { size_t n = u16(); return take(n); }
    Bytes vec24() { size_t n = u24(); return take(n); }

    Bytes take(size_t n) {
        if (n > remaining()) { pos_ = size_; return Bytes(); }
        Bytes out(data_ + pos_, data_ + pos_ + n);
        pos_ += n;
        return out;
    }
    std::string take_str(size_t n) {
        if (n > remaining()) n = remaining();
        std::string s(reinterpret_cast<const char*>(data_ + pos_), n);
        pos_ += n;
        return s;
    }
    bool fail() const { return failed_; }
    void set_fail()   { failed_ = true; }

private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_{0};
    bool   failed_{false};
};

// -----------------------------------------------------------------------------
// String helpers
// -----------------------------------------------------------------------------
std::string trim(std::string_view s);
std::string ltrim(std::string_view s);
std::string rtrim(std::string_view s);
std::string lower(std::string_view s);
std::string upper(std::string_view s);
bool starts_with(std::string_view s, std::string_view prefix);
bool ends_with(std::string_view s, std::string_view suffix);
bool contains(std::string_view haystack, std::string_view needle);
std::vector<std::string> split(std::string_view s, char sep, bool keep_empty = false);
std::vector<std::string> split_lines(std::string_view s);
std::string join(const std::vector<std::string>& parts, std::string_view sep);
std::string replace_all(std::string_view s, std::string_view from, std::string_view to);
std::string repeat(std::string_view s, size_t n);
// Truncates to `limit` bytes on a UTF-8 boundary, appending "..." when cut.
std::string ellipsize(std::string_view s, size_t limit);
std::string pad_left(std::string_view s, size_t width);
bool iequals(std::string_view a, std::string_view b);
// Counts lines, treating both "\n" and "\r\n" correctly.
size_t count_lines(std::string_view s);
// True when `path` escapes `root` after normalising "." and "..".
bool path_escapes(std::string_view root, std::string_view path);
std::string normalise_path(std::string_view path);

// -----------------------------------------------------------------------------
// URL handling
// -----------------------------------------------------------------------------
struct Url {
    std::string scheme;
    std::string host;
    uint16_t    port{0};
    std::string path;        // includes the query string, always starts with '/'
    std::string query;       // without the leading '?'
    std::string userinfo;

    std::string origin() const;      // scheme://host[:port]
    std::string to_string() const;
    std::string request_target() const;   // path + "?" + query
};

// Parses absolute URLs and resolves relative references against a base.
Result<Url>        parse_url(std::string_view text);
Result<Url>        resolve_url(const Url& base, std::string_view reference);
std::string        url_encode(std::string_view s, bool keep_slashes = false);
std::string        url_decode(std::string_view s);

// -----------------------------------------------------------------------------
// JSON (RFC 8259 subset: objects, arrays, strings, numbers, bools, null)
// -----------------------------------------------------------------------------
class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool v) : type_(Type::Bool), bool_(v) {}
    Json(double v) : type_(Type::Number), num_(v) {}
    Json(int v) : type_(Type::Number), num_(double(v)) {}
    Json(int64_t v) : type_(Type::Number), num_(double(v)) {}
    Json(const char* s) : type_(Type::String), str_(s) {}
    Json(std::string s) : type_(Type::String), str_(std::move(s)) {}
    Json(std::string_view s) : type_(Type::String), str_(s) {}
    Json(std::vector<Json> v) : type_(Type::Array), arr_(std::move(v)) {}

    static Json array()                { Json j; j.type_ = Type::Array; return j; }
    static Json object()               { Json j; j.type_ = Type::Object; return j; }

    Type type() const                  { return type_; }
    bool is_null()   const             { return type_ == Type::Null; }
    bool is_bool()   const             { return type_ == Type::Bool; }
    bool is_number() const             { return type_ == Type::Number; }
    bool is_string() const             { return type_ == Type::String; }
    bool is_array()  const             { return type_ == Type::Array; }
    bool is_object() const             { return type_ == Type::Object; }

    bool          as_bool(bool d = false) const    { return type_ == Type::Bool ? bool_ : d; }
    double        as_number(double d = 0) const    { return type_ == Type::Number ? num_ : d; }
    int64_t       as_int(int64_t d = 0) const      { return type_ == Type::Number ? int64_t(num_) : d; }
    const std::string& as_string() const           { static const std::string empty; return type_ == Type::String ? str_ : empty; }
    std::string   as_string_or(std::string d) const { return type_ == Type::String ? str_ : std::move(d); }

    const std::vector<Json>& items() const         { static const std::vector<Json> empty; return type_ == Type::Array ? arr_ : empty; }
    std::vector<Json>&       items()               { if (type_ != Type::Array) { type_ = Type::Array; arr_.clear(); } return arr_; }
    const std::map<std::string, Json>& fields() const { static const std::map<std::string, Json> empty; return type_ == Type::Object ? obj_ : empty; }

    size_t size() const;
    bool   empty() const;

    // Object access (never throws; returns a null Json when absent).
    const Json& operator[](std::string_view key) const;
    Json&       operator[](std::string_view key);
    bool        has(std::string_view key) const;
    void        erase(std::string_view key);

    // Array helpers
    void push_back(Json v);
    const Json& at(size_t i) const;

    // Deep access helpers used by tool implementations.
    std::string   string_at(std::string_view path, std::string fallback = {}) const;
    int64_t       int_at(std::string_view path, int64_t fallback = 0) const;
    bool          bool_at(std::string_view path, bool fallback = false) const;

    std::string dump(int indent = 0) const;
    void        dump_to(std::string& out, int indent, int depth) const;
    static Result<Json> parse(std::string_view text);
    // Extracts the first complete JSON value from `text` (useful for streaming
    // model output that may be wrapped in prose).  `consumed` reports how many
    // bytes were used.
    static Result<Json> parse_prefix(std::string_view text, size_t* consumed = nullptr);
    static Json parse_or_null(std::string_view text);

private:
    Type        type_{Type::Null};
    bool        bool_{false};
    double      num_{0};
    std::string str_;
    std::vector<Json>                 arr_;
    std::map<std::string, Json>       obj_;
};

// -----------------------------------------------------------------------------
// Logging (stderr, level filtered).  Thread-safe via a single mutex.
// -----------------------------------------------------------------------------
enum class LogLevel { Trace = 0, Debug, Info, Warn, Error, Off };

void     log_set_level(LogLevel lvl);
LogLevel log_get_level();
void     log_set_json(bool enabled);
void     log_write(LogLevel lvl, std::string_view scope, std::string_view msg);
bool     log_enabled(LogLevel lvl);

#define LCA_LOG_TRACE(scope, msg) ::lca::log_write(::lca::LogLevel::Trace, scope, msg)
#define LCA_LOG_DEBUG(scope, msg) ::lca::log_write(::lca::LogLevel::Debug, scope, msg)
#define LCA_LOG_INFO(scope, msg)  ::lca::log_write(::lca::LogLevel::Info,  scope, msg)
#define LCA_LOG_WARN(scope, msg)  ::lca::log_write(::lca::LogLevel::Warn,  scope, msg)
#define LCA_LOG_ERROR(scope, msg) ::lca::log_write(::lca::LogLevel::Error, scope, msg)

}  // namespace lca

#endif  // LCA_BUF_H
