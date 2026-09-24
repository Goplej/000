// =============================================================================
//  lca/net.h  --  raw socket networking, HTTP/1.1 client, DEFLATE
// -----------------------------------------------------------------------------
//  The transport layer talks to the kernel directly (socket/connect/poll).
//  TLS is layered on top through the Stream interface so the HTTP client is
//  transport agnostic: it never inspects whether the bytes travelled in the
//  clear or inside a TLS 1.3 record layer.
// =============================================================================
#ifndef LCA_NET_H
#define LCA_NET_H

#include "lca/buf.h"

#include <map>

namespace lca {

// -----------------------------------------------------------------------------
// Stream abstraction
// -----------------------------------------------------------------------------
class Stream {
public:
    virtual ~Stream() = default;
    // Writes the whole buffer or fails.  `deadline_ms` is an absolute
    // steady-clock deadline; 0 means "no deadline".
    virtual Error write_all(const uint8_t* data, size_t n, int64_t deadline_ms) = 0;
    // Reads at least one byte (blocking up to the deadline).
    virtual Result<size_t> read_some(uint8_t* out, size_t cap, int64_t deadline_ms) = 0;
    virtual void  close() = 0;
    virtual bool  closed() const = 0;
    virtual std::string description() const = 0;

    // Convenience: read exactly n bytes.
    Error read_exact(uint8_t* out, size_t n, int64_t deadline_ms);
};

// -----------------------------------------------------------------------------
// TCP
// -----------------------------------------------------------------------------
struct DnsResult {
    std::string address;    // printable form
    int         family{0};  // AF_INET / AF_INET6
};
Result<std::vector<DnsResult>> dns_resolve(const std::string& host, uint16_t port);

class TcpStream : public Stream {
public:
    TcpStream() = default;
    ~TcpStream() override;
    TcpStream(const TcpStream&) = delete;
    TcpStream& operator=(const TcpStream&) = delete;

    // Tries every resolved address in parallel-ish (sequential with a per-address
    // timeout) and returns the first socket that connects.
    static Result<std::unique_ptr<TcpStream>> connect(const std::string& host, uint16_t port,
                                                     int connect_timeout_ms);
    Error write_all(const uint8_t* data, size_t n, int64_t deadline_ms) override;
    Result<size_t> read_some(uint8_t* out, size_t cap, int64_t deadline_ms) override;
    void  close() override;
    bool  closed() const override { return fd_ < 0; }
    std::string description() const override;

    int  fd() const { return fd_; }
    void set_read_timeout_ms(int ms)  { read_timeout_ms_ = ms; }
    void set_write_timeout_ms(int ms) { write_timeout_ms_ = ms; }

private:
    int fd_{-1};
    std::string peer_;
    int read_timeout_ms_{30000};
    int write_timeout_ms_{30000};
};

// -----------------------------------------------------------------------------
// HTTP
// -----------------------------------------------------------------------------
struct HttpRequest {
    std::string method{"GET"};
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    int  timeout_ms{15000};        // per phase (connect / first byte / idle)
    size_t max_response_bytes{8u * 1024 * 1024};
    bool follow_redirects{true};
    int  max_redirects{5};
    bool accept_gzip{true};
    std::string body_content_type;

    void set_header(const std::string& name, const std::string& value);
    const std::string* get_header(const std::string& name) const;
};

struct HttpResponse {
    int         status{0};
    std::string reason;
    std::string http_version;
    std::map<std::string, std::string> headers;    // keys are lower-case
    Bytes       body;
    std::string final_url;
    uint64_t    bytes_received{0};
    int64_t     elapsed_ms{0};
    int         redirects{0};

    bool ok() const { return status >= 200 && status < 300; }
    std::string body_string() const { return to_string(body); }
    std::string content_type() const;
    std::string header(const std::string& name) const;
};

// Performs a full HTTP(S) exchange: DNS, connect, optional TLS 1.3 handshake,
// request write, response parse, redirect following, content decoding.
Result<HttpResponse> http_request(const HttpRequest& req);

// Performs the exchange over an already-established stream (used by tests and by
// the local web UI's upstream fetches).
Result<HttpResponse> http_exchange(Stream& stream, const HttpRequest& req, const Url& url);

// -----------------------------------------------------------------------------
// Content encodings
// -----------------------------------------------------------------------------
// Raw DEFLATE (RFC 1951) and zlib-wrapped (RFC 1950) decompression.
bool inflate_bytes(const uint8_t* in, size_t n, Bytes& out);
bool gunzip_bytes(const uint8_t* in, size_t n, Bytes& out);   // gzip (RFC 1952)

}  // namespace lca

#endif  // LCA_NET_H
