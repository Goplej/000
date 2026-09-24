// =============================================================================
//  src/buf.cpp  --  strings, URLs, JSON, logging
// =============================================================================
#include "lca/buf.h"
#include "lca/crypto.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <mutex>

namespace lca {

// =============================================================================
// Strings
// =============================================================================
namespace {
inline bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}
}  // namespace

std::string ltrim(std::string_view s) {
    size_t i = 0;
    while (i < s.size() && is_space(s[i])) ++i;
    return std::string(s.substr(i));
}
std::string rtrim(std::string_view s) {
    size_t n = s.size();
    while (n > 0 && is_space(s[n - 1])) --n;
    return std::string(s.substr(0, n));
}
std::string trim(std::string_view s) { return rtrim(ltrim(s)); }

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
std::string upper(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = char(std::toupper(static_cast<unsigned char>(c)));
    return out;
}
bool starts_with(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}
bool ends_with(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}
bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}
bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && lower(a) == lower(b);
}

std::vector<std::string> split(std::string_view s, char sep, bool keep_empty) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == sep) {
            std::string piece(s.substr(start, i - start));
            if (keep_empty || !piece.empty()) out.push_back(std::move(piece));
            start = i + 1;
        }
    }
    return out;
}

std::vector<std::string> split_lines(std::string_view s) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '\n') {
            std::string_view line = s.substr(start, i - start);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            out.emplace_back(line);
            start = i + 1;
        }
    }
    if (!out.empty() && out.back().empty() && !s.empty() && s.back() == '\n') out.pop_back();
    return out;
}

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

std::string replace_all(std::string_view s, std::string_view from, std::string_view to) {
    if (from.empty()) return std::string(s);
    std::string out;
    out.reserve(s.size());
    size_t pos = 0;
    while (true) {
        size_t hit = s.find(from, pos);
        if (hit == std::string_view::npos) {
            out.append(s.substr(pos));
            break;
        }
        out.append(s.substr(pos, hit - pos));
        out.append(to);
        pos = hit + from.size();
    }
    return out;
}

std::string repeat(std::string_view s, size_t n) {
    std::string out;
    out.reserve(s.size() * n);
    for (size_t i = 0; i < n; ++i) out.append(s);
    return out;
}

std::string ellipsize(std::string_view s, size_t limit) {
    if (s.size() <= limit) return std::string(s);
    size_t cut = limit;
    // Walk back off a continuation byte so the result stays valid UTF-8.
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    std::string out(s.substr(0, cut));
    out += "...";
    return out;
}

std::string pad_left(std::string_view s, size_t width) {
    if (s.size() >= width) return std::string(s);
    return std::string(width - s.size(), ' ') + std::string(s);
}

size_t count_lines(std::string_view s) {
    if (s.empty()) return 0;
    // Count terminators; a file that ends with a newline does not gain an extra
    // empty line, so the count matches split_lines() for normal text files.
    size_t n = 0;
    for (char c : s) if (c == '\n') ++n;
    if (s.back() != '\n') ++n;
    return n;
}

std::string normalise_path(std::string_view path) {
    std::vector<std::string> parts;
    bool absolute = !path.empty() && path[0] == '/';
    for (const std::string& seg : split(path, '/')) {
        if (seg == ".") continue;
        if (seg == "..") {
            if (!parts.empty() && parts.back() != "..") parts.pop_back();
            else if (!absolute) parts.push_back("..");
            continue;
        }
        parts.push_back(seg);
    }
    std::string out = absolute ? "/" : "";
    out += join(parts, "/");
    if (out.empty()) out = absolute ? "/" : ".";
    return out;
}

bool path_escapes(std::string_view root, std::string_view path) {
    std::string norm_root = normalise_path(root);
    std::string norm_path = normalise_path(path);
    if (norm_root == "." || norm_root.empty()) norm_root = "/";
    if (norm_path == norm_root) return false;
    if (!starts_with(norm_path, norm_root)) return true;
    if (norm_root == "/") return false;
    return norm_path.size() > norm_root.size() && norm_path[norm_root.size()] != '/';
}

// =============================================================================
// URLs
// =============================================================================
std::string Url::origin() const {
    std::string out = scheme + "://" + host;
    bool default_port = (scheme == "https" && port == 443) || (scheme == "http" && port == 80);
    if (!default_port) out += ":" + std::to_string(port);
    return out;
}
std::string Url::request_target() const {
    std::string target = path.empty() ? "/" : path;
    if (!query.empty()) target += "?" + query;
    return target;
}
std::string Url::to_string() const { return origin() + request_target(); }

Result<Url> parse_url(std::string_view text) {
    std::string s = trim(text);
    Url url;
    size_t scheme_end = s.find("://");
    if (scheme_end == std::string::npos) {
        // Bare host forms: "example.com/path" -- assume https.
        if (starts_with(s, "//")) s = "https:" + s;
        else s = "https://" + s;
        scheme_end = s.find("://");
    }
    url.scheme = lower(s.substr(0, scheme_end));
    if (url.scheme != "http" && url.scheme != "https") {
        return Error(Code::InvalidArgument, "unsupported URL scheme: " + url.scheme, "parse_url");
    }
    size_t authority_start = scheme_end + 3;
    size_t authority_end = s.find_first_of("/?#", authority_start);
    std::string authority = s.substr(authority_start,
                                     authority_end == std::string::npos
                                         ? std::string::npos
                                         : authority_end - authority_start);
    if (authority.empty()) return Error(Code::InvalidArgument, "URL has no host", "parse_url");

    size_t at = authority.rfind('@');
    if (at != std::string::npos) {
        url.userinfo = authority.substr(0, at);
        authority = authority.substr(at + 1);
    }
    if (!authority.empty() && authority.front() == '[') {          // IPv6 literal
        size_t close = authority.find(']');
        if (close == std::string::npos) return Error(Code::InvalidArgument, "bad IPv6 host", "parse_url");
        url.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size() && authority[close + 1] == ':')
            url.port = uint16_t(std::atoi(authority.c_str() + close + 2));
    } else {
        size_t colon = authority.rfind(':');
        if (colon != std::string::npos) {
            url.host = authority.substr(0, colon);
            url.port = uint16_t(std::atoi(authority.c_str() + colon + 1));
        } else {
            url.host = authority;
        }
    }
    if (url.host.empty()) return Error(Code::InvalidArgument, "URL has no host", "parse_url");
    url.host = lower(url.host);
    if (url.port == 0) url.port = (url.scheme == "https") ? 443 : 80;

    std::string rest = authority_end == std::string::npos ? std::string()
                                                          : s.substr(authority_end);
    size_t hash = rest.find('#');
    if (hash != std::string::npos) rest = rest.substr(0, hash);    // fragments are client side
    size_t q = rest.find('?');
    if (q != std::string::npos) {
        url.path  = rest.substr(0, q);
        url.query = rest.substr(q + 1);
    } else {
        url.path = rest;
    }
    if (url.path.empty()) url.path = "/";
    return url;
}

Result<Url> resolve_url(const Url& base, std::string_view reference) {
    std::string ref = trim(reference);
    if (ref.empty()) return base;
    if (contains(ref, "://")) return parse_url(ref);
    if (starts_with(ref, "//")) return parse_url(base.scheme + ":" + ref);

    Url out = base;
    out.query.clear();
    if (!ref.empty() && ref[0] == '/') {
        size_t q = ref.find('?');
        out.path = q == std::string::npos ? std::string(ref) : std::string(ref.substr(0, q));
        if (q != std::string::npos) out.query = std::string(ref.substr(q + 1));
        return out;
    }
    if (!ref.empty() && ref[0] == '?') {
        out.query = std::string(ref.substr(1));
        return out;
    }
    // Relative reference: merge against the base directory.
    std::string dir = base.path;
    size_t slash = dir.rfind('/');
    dir = slash == std::string::npos ? std::string("/") : dir.substr(0, slash + 1);
    std::string merged = dir + std::string(ref);
    size_t q = merged.find('?');
    if (q != std::string::npos) {
        out.query = merged.substr(q + 1);
        merged = merged.substr(0, q);
    }
    out.path = normalise_path(merged);
    return out;
}

std::string url_encode(std::string_view s, bool keep_slashes) {
    static const char* hexd = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved || (keep_slashes && c == '/')) {
            out.push_back(char(c));
        } else {
            out.push_back('%');
            out.push_back(hexd[c >> 4]);
            out.push_back(hexd[c & 15]);
        }
    }
    return out;
}

std::string url_decode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    auto hexval = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = hexval(s[i + 1]), lo = hexval(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(char((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        if (s[i] == '+') { out.push_back(' '); continue; }
        out.push_back(s[i]);
    }
    return out;
}

// =============================================================================
// JSON
// =============================================================================
size_t Json::size() const {
    if (type_ == Type::Array)  return arr_.size();
    if (type_ == Type::Object) return obj_.size();
    if (type_ == Type::String) return str_.size();
    return 0;
}
bool Json::empty() const { return size() == 0; }

const Json& Json::operator[](std::string_view key) const {
    static const Json null_value;
    if (type_ != Type::Object) return null_value;
    auto it = obj_.find(std::string(key));
    return it == obj_.end() ? null_value : it->second;
}
Json& Json::operator[](std::string_view key) {
    if (type_ != Type::Object) { type_ = Type::Object; obj_.clear(); }
    return obj_[std::string(key)];
}
bool Json::has(std::string_view key) const {
    return type_ == Type::Object && obj_.find(std::string(key)) != obj_.end();
}
void Json::erase(std::string_view key) {
    if (type_ == Type::Object) obj_.erase(std::string(key));
}
void Json::push_back(Json v) {
    if (type_ != Type::Array) { type_ = Type::Array; arr_.clear(); }
    arr_.push_back(std::move(v));
}
const Json& Json::at(size_t i) const {
    static const Json null_value;
    return (type_ == Type::Array && i < arr_.size()) ? arr_[i] : null_value;
}

namespace {
// Walks a "a.b[3].c" path.
const Json& walk(const Json& root, std::string_view path) {
    const Json* cur = &root;
    for (size_t i = 0; i < path.size();) {
        if (path[i] == '.') { ++i; continue; }
        if (path[i] == '[') {
            size_t close = path.find(']', i);
            if (close == std::string_view::npos) break;
            size_t idx = size_t(std::atoll(std::string(path.substr(i + 1, close - i - 1)).c_str()));
            cur = &cur->at(idx);
            i = close + 1;
            continue;
        }
        size_t end = i;
        while (end < path.size() && path[end] != '.' && path[end] != '[') ++end;
        cur = &(*cur)[path.substr(i, end - i)];
        i = end;
    }
    return *cur;
}
}  // namespace

std::string Json::string_at(std::string_view path, std::string fallback) const {
    const Json& v = walk(*this, path);
    return v.is_string() ? v.as_string() : std::move(fallback);
}
int64_t Json::int_at(std::string_view path, int64_t fallback) const {
    const Json& v = walk(*this, path);
    if (v.is_number()) return v.as_int();
    if (v.is_string()) {
        try { return std::stoll(v.as_string()); } catch (...) { return fallback; }
    }
    return fallback;
}
bool Json::bool_at(std::string_view path, bool fallback) const {
    const Json& v = walk(*this, path);
    if (v.is_bool()) return v.as_bool();
    if (v.is_number()) return v.as_number() != 0;
    if (v.is_string()) return v.as_string() == "true";
    return fallback;
}

void Json::dump_to(std::string& out, int indent, int depth) const {
    auto nl = [&](int d) {
        if (indent <= 0) return;
        out.push_back('\n');
        out.append(size_t(indent * d), ' ');
    };
    switch (type_) {
        case Type::Null:   out += "null"; break;
        case Type::Bool:   out += bool_ ? "true" : "false"; break;
        case Type::Number: {
            double v = num_;
            if (v == double(int64_t(v)) && std::abs(v) < 9.0e15) {
                out += std::to_string(int64_t(v));
            } else {
                char buf[40];
                std::snprintf(buf, sizeof(buf), "%.17g", v);
                out += buf;
            }
            break;
        }
        case Type::String: {
            out.push_back('"');
            for (unsigned char c : str_) {
                switch (c) {
                    case '"':  out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\n': out += "\\n";  break;
                    case '\r': out += "\\r";  break;
                    case '\t': out += "\\t";  break;
                    case '\b': out += "\\b";  break;
                    case '\f': out += "\\f";  break;
                    default:
                        if (c < 0x20) {
                            char buf[8];
                            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                            out += buf;
                        } else {
                            out.push_back(char(c));
                        }
                }
            }
            out.push_back('"');
            break;
        }
        case Type::Array: {
            if (arr_.empty()) { out += "[]"; break; }
            out.push_back('[');
            for (size_t i = 0; i < arr_.size(); ++i) {
                if (i) out.push_back(',');
                nl(depth + 1);
                arr_[i].dump_to(out, indent, depth + 1);
            }
            nl(depth);
            out.push_back(']');
            break;
        }
        case Type::Object: {
            if (obj_.empty()) { out += "{}"; break; }
            out.push_back('{');
            bool first = true;
            for (const auto& kv : obj_) {
                if (!first) out.push_back(',');
                first = false;
                nl(depth + 1);
                Json(kv.first).dump_to(out, indent, depth + 1);
                out.push_back(':');
                if (indent > 0) out.push_back(' ');
                kv.second.dump_to(out, indent, depth + 1);
            }
            nl(depth);
            out.push_back('}');
            break;
        }
    }
}

std::string Json::dump(int indent) const {
    std::string out;
    out.reserve(256);
    dump_to(out, indent, 0);
    return out;
}

namespace {

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : s_(text) {}

    Result<Json> parse() {
        skip_ws();
        Result<Json> v = value(0);
        if (!v.ok()) return v;
        skip_ws();
        return v;
    }

    size_t pos() const { return pos_; }

private:
    void skip_ws() {
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { ++pos_; continue; }
            break;
        }
    }
    bool eat(char c) {
        if (pos_ < s_.size() && s_[pos_] == c) { ++pos_; return true; }
        return false;
    }
    bool eat_lit(std::string_view lit) {
        if (s_.compare(pos_, lit.size(), lit) == 0) { pos_ += lit.size(); return true; }
        return false;
    }

    Result<Json> value(int depth) {
        if (depth > 128) return Error(Code::ParseError, "JSON nesting too deep", "json");
        skip_ws();
        if (pos_ >= s_.size()) return Error(Code::ParseError, "unexpected end of JSON", "json");
        char c = s_[pos_];
        switch (c) {
            case '{': return object(depth);
            case '[': return array(depth);
            case '"': return string();
            case 't': if (eat_lit("true"))  return Json(true);  break;
            case 'f': if (eat_lit("false")) return Json(false); break;
            case 'n': if (eat_lit("null"))  return Json();      break;
            default:  break;
        }
        if (c == '-' || (c >= '0' && c <= '9')) return number();
        return Error(Code::ParseError, std::string("unexpected character '") + c + "'", "json");
    }

    Result<Json> object(int depth) {
        ++pos_;  // consume '{'
        Json out = Json::object();
        skip_ws();
        if (eat('}')) return out;
        while (true) {
            skip_ws();
            if (pos_ >= s_.size() || s_[pos_] != '"')
                return Error(Code::ParseError, "expected object key", "json");
            Result<Json> key = string();
            if (!key.ok()) return key;
            skip_ws();
            if (!eat(':')) return Error(Code::ParseError, "expected ':'", "json");
            Result<Json> val = value(depth + 1);
            if (!val.ok()) return val;
            out[key->as_string()] = std::move(*val);
            skip_ws();
            if (eat(',')) continue;
            if (eat('}')) break;
            return Error(Code::ParseError, "expected ',' or '}'", "json");
        }
        return out;
    }

    Result<Json> array(int depth) {
        ++pos_;  // consume '['
        Json out = Json::array();
        skip_ws();
        if (eat(']')) return out;
        while (true) {
            Result<Json> val = value(depth + 1);
            if (!val.ok()) return val;
            out.push_back(std::move(*val));
            skip_ws();
            if (eat(',')) continue;
            if (eat(']')) break;
            return Error(Code::ParseError, "expected ',' or ']'", "json");
        }
        return out;
    }

    Result<Json> string() {
        ++pos_;  // consume opening quote
        std::string out;
        while (pos_ < s_.size()) {
            unsigned char c = static_cast<unsigned char>(s_[pos_++]);
            if (c == '"') return Json(std::move(out));
            if (c != '\\') {
                out.push_back(char(c));
                continue;
            }
            if (pos_ >= s_.size()) break;
            char esc = s_[pos_++];
            switch (esc) {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    if (pos_ + 4 > s_.size()) return Error(Code::ParseError, "bad \\u escape", "json");
                    uint32_t cp = 0;
                    for (int i = 0; i < 4; ++i) {
                        char h = s_[pos_++];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= uint32_t(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= uint32_t(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= uint32_t(h - 'A' + 10);
                        else return Error(Code::ParseError, "bad \\u escape", "json");
                    }
                    if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 6 <= s_.size() &&
                        s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                        uint32_t lo = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = s_[pos_ + 2 + i];
                            lo <<= 4;
                            if (h >= '0' && h <= '9') lo |= uint32_t(h - '0');
                            else if (h >= 'a' && h <= 'f') lo |= uint32_t(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') lo |= uint32_t(h - 'A' + 10);
                            else { lo = 0xFFFFFFFFu; break; }
                        }
                        if (lo != 0xFFFFFFFFu && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            pos_ += 6;
                        }
                    }
                    // UTF-8 encode
                    if (cp < 0x80) {
                        out.push_back(char(cp));
                    } else if (cp < 0x800) {
                        out.push_back(char(0xC0 | (cp >> 6)));
                        out.push_back(char(0x80 | (cp & 0x3F)));
                    } else if (cp < 0x10000) {
                        out.push_back(char(0xE0 | (cp >> 12)));
                        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
                        out.push_back(char(0x80 | (cp & 0x3F)));
                    } else {
                        out.push_back(char(0xF0 | (cp >> 18)));
                        out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
                        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
                        out.push_back(char(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default: return Error(Code::ParseError, "unknown escape", "json");
            }
        }
        return Error(Code::ParseError, "unterminated string", "json");
    }

    Result<Json> number() {
        size_t start = pos_;
        if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+')) ++pos_;
        while (pos_ < s_.size() && ((s_[pos_] >= '0' && s_[pos_] <= '9') || s_[pos_] == '.' ||
                                    s_[pos_] == 'e' || s_[pos_] == 'E' || s_[pos_] == '-' ||
                                    s_[pos_] == '+')) {
            ++pos_;
        }
        std::string text(s_.substr(start, pos_ - start));
        try {
            return Json(std::stod(text));
        } catch (...) {
            return Error(Code::ParseError, "bad number: " + text, "json");
        }
    }

    std::string_view s_;
    size_t pos_{0};
};

}  // namespace

Result<Json> Json::parse(std::string_view text) {
    JsonParser p(text);
    Result<Json> v = p.parse();
    if (!v.ok()) return v;
    return v;
}

Result<Json> Json::parse_prefix(std::string_view text, size_t* consumed) {
    size_t start = 0;
    while (start < text.size() && text[start] != '{' && text[start] != '[') ++start;
    if (start >= text.size()) return Error(Code::ParseError, "no JSON value found", "json");
    char open = text[start];
    char close = open == '{' ? '}' : ']';
    int depth = 0;
    bool in_string = false, escape = false;
    for (size_t i = start; i < text.size(); ++i) {
        char c = text[i];
        if (in_string) {
            if (escape) { escape = false; continue; }
            if (c == '\\') { escape = true; continue; }
            if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') { in_string = true; continue; }
        if (c == open) ++depth;
        else if (c == close) {
            if (--depth == 0) {
                if (consumed) *consumed = i + 1;
                JsonParser p(text.substr(start, i + 1 - start));
                return p.parse();
            }
        }
    }
    return Error(Code::ParseError, "unbalanced JSON value", "json");
}

Json Json::parse_or_null(std::string_view text) {
    size_t consumed = 0;
    Result<Json> v = parse_prefix(text, &consumed);
    return v.ok() ? *v : Json();
}

// =============================================================================
// Logging
// =============================================================================
namespace {
LogLevel  g_log_level = LogLevel::Info;
bool      g_log_json  = false;
std::mutex g_log_mutex;

const char* level_name(LogLevel l) {
    switch (l) {
        case LogLevel::Trace: return "trace";
        case LogLevel::Debug: return "debug";
        case LogLevel::Info:  return "info";
        case LogLevel::Warn:  return "warn";
        case LogLevel::Error: return "error";
        case LogLevel::Off:   return "off";
    }
    return "?";
}
}  // namespace

void log_set_level(LogLevel lvl) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    g_log_level = lvl;
}
LogLevel log_get_level() {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    return g_log_level;
}
void log_set_json(bool enabled) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    g_log_json = enabled;
}
bool log_enabled(LogLevel lvl) { return int(lvl) >= int(log_get_level()); }

void log_write(LogLevel lvl, std::string_view scope, std::string_view msg) {
    if (!log_enabled(lvl)) return;
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log_json) {
        // Compact single-line JSON so log collectors can consume it directly.
        std::string esc;
        for (char c : std::string(msg)) {
            if (c == '"' || c == '\\') { esc.push_back('\\'); esc.push_back(c); }
            else if (c == '\n') esc += "\\n";
            else if (c == '\r') esc += "\\r";
            else if (c == '\t') esc += "\\t";
            else if (static_cast<unsigned char>(c) < 0x20) esc.push_back(' ');
            else esc.push_back(c);
        }
        std::fprintf(stderr, "{\"ts\":%lld,\"level\":\"%s\",\"scope\":\"%s\",\"msg\":\"%s\"}\n",
                     (long long)wall_millis(), level_name(lvl), std::string(scope).c_str(),
                     esc.c_str());
    } else {
        std::fprintf(stderr, "[%s] %-12s %.*s\n", level_name(lvl),
                     std::string(scope).c_str(), int(msg.size()), msg.data());
    }
    std::fflush(stderr);
}

}  // namespace lca
