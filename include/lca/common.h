// =============================================================================
//  lca/common.h  --  Local Claude Code Agent :: foundation types
// -----------------------------------------------------------------------------
//  Zero-dependency C++17 core: byte buffers, results, errors, small utilities.
//  Everything in this project is written against the POSIX + ISO C++17 standard
//  library only.  No third party runtime, no framework, no paid service.
// =============================================================================
#ifndef LCA_COMMON_H
#define LCA_COMMON_H

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lca {

// -----------------------------------------------------------------------------
// Version / identity
// -----------------------------------------------------------------------------
inline constexpr const char* kAppName    = "local-claude-code-agent";
inline constexpr const char* kAppVersion = "1.0.0";
inline constexpr const char* kAppCodename = "Ryzen";

// -----------------------------------------------------------------------------
// Byte buffer helpers
// -----------------------------------------------------------------------------
using Bytes  = std::vector<uint8_t>;
using String = std::string;

inline Bytes to_bytes(std::string_view s) {
    return Bytes(s.begin(), s.end());
}
inline String to_string(const Bytes& b) {
    return String(reinterpret_cast<const char*>(b.data()), b.size());
}
inline String to_string(const uint8_t* p, size_t n) {
    return n ? String(reinterpret_cast<const char*>(p), n) : String();
}

// Append helper used by every serializer in the project.
inline void append(Bytes& out, const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    out.insert(out.end(), p, p + n);
}
inline void append(Bytes& out, std::string_view s) { append(out, s.data(), s.size()); }
inline void append_u8 (Bytes& out, uint8_t v)  { out.push_back(v); }
inline void append_u16(Bytes& out, uint16_t v) { out.push_back(uint8_t(v >> 8)); out.push_back(uint8_t(v)); }
inline void append_u24(Bytes& out, uint32_t v) { out.push_back(uint8_t(v >> 16)); out.push_back(uint8_t(v >> 8)); out.push_back(uint8_t(v)); }
inline void append_u32(Bytes& out, uint32_t v) {
    out.push_back(uint8_t(v >> 24)); out.push_back(uint8_t(v >> 16));
    out.push_back(uint8_t(v >>  8)); out.push_back(uint8_t(v));
}
inline void append_u64(Bytes& out, uint64_t v) {
    for (int i = 7; i >= 0; --i) out.push_back(uint8_t(v >> (i * 8)));
}

inline uint16_t read_u16(const uint8_t* p) { return uint16_t((uint16_t(p[0]) << 8) | p[1]); }
inline uint32_t read_u24(const uint8_t* p) { return (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2]; }
inline uint32_t read_u32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
inline uint64_t read_u64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

// Constant-time comparison: never short-circuit on the first differing byte.
inline bool ct_equal(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t diff = 0;
    for (size_t i = 0; i < n; ++i) diff |= uint8_t(a[i] ^ b[i]);
    return diff == 0;
}
inline bool ct_equal(const Bytes& a, const Bytes& b) {
    return a.size() == b.size() && ct_equal(a.data(), b.data(), a.size());
}

// Erase secret material in a way the optimiser will not elide.
inline void secure_zero(void* p, size_t n) {
    volatile uint8_t* v = static_cast<volatile uint8_t*>(p);
    while (n--) *v++ = 0;
}
inline void secure_zero(Bytes& b) {
    if (!b.empty()) secure_zero(b.data(), b.size());
    b.clear();
}
template <size_t N>
inline void secure_zero(uint8_t (&arr)[N]) { secure_zero(arr, N); }

// -----------------------------------------------------------------------------
// Status / error propagation
// -----------------------------------------------------------------------------
enum class Code : int {
    Ok = 0,
    InvalidArgument,
    NotFound,
    PermissionDenied,
    IoError,
    NetworkError,
    TlsError,
    ProtocolError,
    Timeout,
    Cancelled,
    ResourceExhausted,
    Unsupported,
    ParseError,
    SandboxViolation,
    ModelError,
    Internal,
};

inline const char* code_name(Code c) {
    switch (c) {
        case Code::Ok:                return "Ok";
        case Code::InvalidArgument:   return "InvalidArgument";
        case Code::NotFound:          return "NotFound";
        case Code::PermissionDenied:  return "PermissionDenied";
        case Code::IoError:           return "IoError";
        case Code::NetworkError:      return "NetworkError";
        case Code::TlsError:          return "TlsError";
        case Code::ProtocolError:     return "ProtocolError";
        case Code::Timeout:           return "Timeout";
        case Code::Cancelled:         return "Cancelled";
        case Code::ResourceExhausted: return "ResourceExhausted";
        case Code::Unsupported:       return "Unsupported";
        case Code::ParseError:        return "ParseError";
        case Code::SandboxViolation:  return "SandboxViolation";
        case Code::ModelError:        return "ModelError";
        case Code::Internal:          return "Internal";
    }
    return "Unknown";
}

struct Error {
    Code        code{Code::Ok};
    std::string message;
    std::string where;   // "module:function" breadcrumb

    Error() = default;
    Error(Code c, std::string msg, std::string loc = {})
        : code(c), message(std::move(msg)), where(std::move(loc)) {}

    bool ok() const { return code == Code::Ok; }
    std::string str() const {
        std::string s = code_name(code);
        if (!where.empty())   { s += " @ "; s += where; }
        if (!message.empty()) { s += ": "; s += message; }
        return s;
    }
};

// A Result<T> that always carries either a value or a diagnostic Error.
template <typename T>
class Result {
public:
    Result(T v) : value_(std::move(v)) {}                 // NOLINT(google-explicit-constructor)
    Result(Error e) : error_(std::move(e)) {}             // NOLINT(google-explicit-constructor)
    static Result<T> ok(T v)            { return Result<T>(std::move(v)); }
    static Result<T> fail(Error e)      { return Result<T>(std::move(e)); }
    bool ok() const                     { return !error_.has_value(); }
    explicit operator bool() const      { return ok(); }
    const Error& error() const {
        static const Error kNoError;
        return error_ ? *error_ : kNoError;
    }
    T& value()                          { return *value_; }
    const T& value() const              { return *value_; }
    T value_or(T fallback) const        { return value_ ? *value_ : std::move(fallback); }
    T& operator*()                      { return *value_; }
    const T& operator*() const          { return *value_; }
    T* operator->()                     { return &*value_; }
private:
    std::optional<T>     value_;
    std::optional<Error> error_;
};

// Convenience macro that annotates an error with a location breadcrumb.
#define LCA_FAIL(code, msg) ::lca::Error((code), (msg), __FILE__ ":" LCA_STR(__LINE__))
#define LCA_STR2(x) #x
#define LCA_STR(x) LCA_STR2(x)
#define LCA_TRY(expr)                                                                  \
    do {                                                                               \
        auto&& _lca_r = (expr);                                                        \
        if (!_lca_r.ok()) return ::lca::Error(_lca_r.error());                          \
    } while (0)

// -----------------------------------------------------------------------------
// Time
// -----------------------------------------------------------------------------
inline int64_t now_millis() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
inline int64_t wall_millis() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}
inline int64_t now_micros() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

// Simple RAII scope timer.
class ScopeTimer {
public:
    explicit ScopeTimer(int64_t* sink) : sink_(sink), start_(now_micros()) {}
    ~ScopeTimer() { if (sink_) *sink_ = now_micros() - start_; }
    int64_t elapsed_us() const { return now_micros() - start_; }
private:
    int64_t* sink_;
    int64_t  start_;
};

// -----------------------------------------------------------------------------
// Small numeric helpers
// -----------------------------------------------------------------------------
template <typename T>
inline T clamp_val(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline size_t round_up(size_t v, size_t align) { return (v + align - 1) & ~(align - 1); }

// Human readable byte count, e.g. "4.00 GiB".
inline std::string human_bytes(uint64_t n) {
    static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double d = double(n);
    int u = 0;
    while (d >= 1024.0 && u < 4) { d /= 1024.0; ++u; }
    char buf[48];
    std::snprintf(buf, sizeof(buf), u == 0 ? "%.0f %s" : "%.2f %s", d, units[u]);
    return buf;
}

inline std::string human_duration(int64_t ms) {
    char buf[64];
    if (ms < 1000) std::snprintf(buf, sizeof(buf), "%lldms", (long long)ms);
    else if (ms < 60000) std::snprintf(buf, sizeof(buf), "%.2fs", double(ms) / 1000.0);
    else std::snprintf(buf, sizeof(buf), "%lldm%02llds", (long long)(ms / 60000), (long long)((ms % 60000) / 1000));
    return buf;
}

}  // namespace lca

#endif  // LCA_COMMON_H
