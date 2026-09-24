// =============================================================================
//  src/net.cpp  --  sockets, HTTP/1.1, DEFLATE / gzip
// =============================================================================
#include "lca/net.h"
#include "lca/crypto.h"
#include "lca/tls.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>

namespace lca {

namespace {
constexpr const char* kScope = "net";

int64_t deadline_from(int timeout_ms) {
    return timeout_ms <= 0 ? 0 : now_millis() + timeout_ms;
}
int poll_timeout(int64_t deadline_ms) {
    if (deadline_ms <= 0) return 30000;
    int64_t left = deadline_ms - now_millis();
    if (left < 0) return 0;
    return int(left > 60000 ? 60000 : left);
}
}  // namespace

// =============================================================================
// Stream
// =============================================================================
Error Stream::read_exact(uint8_t* out, size_t n, int64_t deadline_ms) {
    size_t got = 0;
    while (got < n) {
        Result<size_t> r = read_some(out + got, n - got, deadline_ms);
        if (!r.ok()) return r.error();
        if (*r == 0) return LCA_FAIL(Code::NetworkError, "connection closed mid-record");
        got += *r;
    }
    return {};
}

// =============================================================================
// DNS
// =============================================================================
Result<std::vector<DnsResult>> dns_resolve(const std::string& host, uint16_t port) {
    // Numeric literals short-circuit the resolver.
    struct in_addr a4 {};
    struct in6_addr a6 {};
    if (::inet_pton(AF_INET, host.c_str(), &a4) == 1 || ::inet_pton(AF_INET6, host.c_str(), &a6) == 1) {
        return std::vector<DnsResult>{{host, ::inet_pton(AF_INET, host.c_str(), &a4) == 1 ? AF_INET : AF_INET6}};
    }
    struct addrinfo hints {};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    struct addrinfo* res = nullptr;
    std::string port_str = std::to_string(port);
    int rc = ::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res);
    if (rc != 0 || res == nullptr) {
        return LCA_FAIL(Code::NetworkError,
                        "DNS lookup failed for " + host + ": " + std::string(::gai_strerror(rc)));
    }
    std::vector<DnsResult> out;
    for (struct addrinfo* it = res; it != nullptr; it = it->ai_next) {
        char buf[INET6_ADDRSTRLEN] = {0};
        void* addr = nullptr;
        int family = it->ai_family;
        if (family == AF_INET) addr = &reinterpret_cast<struct sockaddr_in*>(it->ai_addr)->sin_addr;
        else if (family == AF_INET6) addr = &reinterpret_cast<struct sockaddr_in6*>(it->ai_addr)->sin6_addr;
        else continue;
        if (::inet_ntop(family, addr, buf, sizeof(buf)) == nullptr) continue;
        DnsResult d;
        d.address = buf;
        d.family  = family;
        bool dup = false;
        for (const auto& e : out) if (e.address == d.address) dup = true;
        if (!dup) out.push_back(d);
    }
    ::freeaddrinfo(res);
    if (out.empty()) return LCA_FAIL(Code::NetworkError, "DNS returned no usable address for " + host);
    return out;
}

// =============================================================================
// TcpStream
// =============================================================================
TcpStream::~TcpStream() { close(); }

void TcpStream::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

std::string TcpStream::description() const { return "tcp://" + peer_; }

Result<std::unique_ptr<TcpStream>> TcpStream::connect(const std::string& host, uint16_t port,
                                                     int connect_timeout_ms) {
    Result<std::vector<DnsResult>> addrs = dns_resolve(host, port);
    if (!addrs.ok()) return addrs.error();

    Error last = LCA_FAIL(Code::NetworkError, "no address to try");
    for (const DnsResult& a : *addrs) {
        int fd = ::socket(a.family, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
        if (fd < 0) {
            last = LCA_FAIL(Code::NetworkError, std::string("socket(): ") + std::strerror(errno));
            continue;
        }
        // Non-blocking connect so we can enforce a deadline.
        int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        struct sockaddr_storage ss {};
        socklen_t sslen = 0;
        if (a.family == AF_INET) {
            auto* sin = reinterpret_cast<struct sockaddr_in*>(&ss);
            sin->sin_family = AF_INET;
            sin->sin_port = htons(port);
            ::inet_pton(AF_INET, a.address.c_str(), &sin->sin_addr);
            sslen = sizeof(*sin);
        } else {
            auto* sin6 = reinterpret_cast<struct sockaddr_in6*>(&ss);
            sin6->sin6_family = AF_INET6;
            sin6->sin6_port = htons(port);
            ::inet_pton(AF_INET6, a.address.c_str(), &sin6->sin6_addr);
            sslen = sizeof(*sin6);
        }

        int rc = ::connect(fd, reinterpret_cast<struct sockaddr*>(&ss), sslen);
        if (rc == 0) {
            ::fcntl(fd, F_SETFL, flags);      // back to blocking mode
            auto stream = std::unique_ptr<TcpStream>(new TcpStream());
            stream->fd_ = fd;
            stream->peer_ = host + ":" + std::to_string(port);
            return stream;
        }
        if (errno != EINPROGRESS) {
            last = LCA_FAIL(Code::NetworkError,
                            "connect(" + a.address + ":" + std::to_string(port) + "): " +
                                std::strerror(errno));
            ::close(fd);
            continue;
        }
        struct pollfd pfd {fd, POLLOUT, 0};
        int prc = ::poll(&pfd, 1, connect_timeout_ms);
        if (prc <= 0) {
            last = LCA_FAIL(Code::Timeout, "connect timeout to " + a.address + ":" + std::to_string(port));
            ::close(fd);
            continue;
        }
        int soerr = 0;
        socklen_t len = sizeof(soerr);
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len);
        if (soerr != 0) {
            last = LCA_FAIL(Code::NetworkError,
                            "connect(" + a.address + "): " + std::string(std::strerror(soerr)));
            ::close(fd);
            continue;
        }
        ::fcntl(fd, F_SETFL, flags);
        auto stream = std::unique_ptr<TcpStream>(new TcpStream());
        stream->fd_ = fd;
        stream->peer_ = host + ":" + std::to_string(port);
        return stream;
    }
    return last;
}

Error TcpStream::write_all(const uint8_t* data, size_t n, int64_t deadline_ms) {
    if (fd_ < 0) return LCA_FAIL(Code::NetworkError, "socket is closed");
    size_t off = 0;
    while (off < n) {
        ssize_t w = ::send(fd_, data + off, n - off, MSG_NOSIGNAL);
        if (w > 0) { off += size_t(w); continue; }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd {fd_, POLLOUT, 0};
            if (::poll(&pfd, 1, poll_timeout(deadline_ms ? deadline_ms
                                                         : deadline_from(write_timeout_ms_))) <= 0) {
                return LCA_FAIL(Code::Timeout, "write timeout");
            }
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        return LCA_FAIL(Code::NetworkError, std::string("send(): ") + std::strerror(errno));
    }
    return {};
}

Result<size_t> TcpStream::read_some(uint8_t* out, size_t cap, int64_t deadline_ms) {
    if (fd_ < 0) return LCA_FAIL(Code::NetworkError, "socket is closed");
    int64_t dl = deadline_ms ? deadline_ms : deadline_from(read_timeout_ms_);
    while (true) {
        ssize_t r = ::recv(fd_, out, cap, 0);
        if (r > 0) return size_t(r);
        if (r == 0) return size_t(0);
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            struct pollfd pfd {fd_, POLLIN, 0};
            int prc = ::poll(&pfd, 1, poll_timeout(dl));
            if (prc == 0) return LCA_FAIL(Code::Timeout, "read timeout");
            if (prc < 0) return LCA_FAIL(Code::NetworkError, std::string("poll(): ") + std::strerror(errno));
            continue;
        }
        return LCA_FAIL(Code::NetworkError, std::string("recv(): ") + std::strerror(errno));
    }
}

// =============================================================================
// DEFLATE (RFC 1951) / zlib (RFC 1950) / gzip (RFC 1952)
// =============================================================================
namespace {

class BitReader {
public:
    BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}
    bool get(uint32_t count, uint32_t& out) {
        while (bits_ < count) {
            if (pos_ >= size_) return false;
            buf_ |= uint64_t(data_[pos_++]) << bits_;
            bits_ += 8;
        }
        out = uint32_t(buf_ & ((uint64_t(1) << count) - 1));
        buf_ >>= count;
        bits_ -= count;
        return true;
    }
    void align() { buf_ >>= (bits_ & 7); bits_ -= (bits_ & 7); }
    size_t byte_position() const { return pos_ - bits_ / 8; }

private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_{0};
    uint64_t buf_{0};
    uint32_t bits_{0};
};

struct Huffman {
    // Canonical Huffman decoder: counts per length plus sorted symbol table.
    std::vector<uint16_t> counts;   // index = bit length (1..15)
    std::vector<uint16_t> symbols;

    void build(const uint8_t* lengths, size_t n) {
        counts.assign(16, 0);
        for (size_t i = 0; i < n; ++i) counts[lengths[i]]++;
        counts[0] = 0;
        std::vector<uint16_t> offsets(16, 0);
        for (int i = 1; i < 16; ++i) offsets[i] = uint16_t(offsets[i - 1] + counts[i - 1]);
        symbols.assign(n, 0);
        for (size_t i = 0; i < n; ++i) {
            if (lengths[i]) symbols[offsets[lengths[i]]++] = uint16_t(i);
        }
    }
    bool decode(BitReader& br, uint16_t& symbol) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 15; ++len) {
            uint32_t bit = 0;
            if (!br.get(1, bit)) return false;
            code |= int(bit);
            int count = counts[size_t(len)];
            if (code - first < count) {
                symbol = symbols[size_t(index + (code - first))];
                return true;
            }
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        return false;
    }
};

const uint16_t kLenBase[29]   = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
const uint8_t  kLenExtra[29]  = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
const uint16_t kDistBase[30]  = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
const uint8_t  kDistExtra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

bool inflate_block_body(BitReader& br, Huffman& lit, Huffman& dist, Bytes& out, size_t max_out) {
    while (true) {
        uint16_t sym = 0;
        if (!lit.decode(br, sym)) return false;
        if (sym < 256) {
            if (out.size() >= max_out) return false;
            out.push_back(uint8_t(sym));
            continue;
        }
        if (sym == 256) return true;
        size_t lidx = size_t(sym - 257);
        if (lidx >= 29) return false;
        uint32_t extra = 0;
        if (kLenExtra[lidx] && !br.get(kLenExtra[lidx], extra)) return false;
        size_t length = kLenBase[lidx] + extra;

        uint16_t dsym = 0;
        if (!dist.decode(br, dsym)) return false;
        if (dsym >= 30) return false;
        uint32_t dextra = 0;
        if (kDistExtra[dsym] && !br.get(kDistExtra[dsym], dextra)) return false;
        size_t distance = kDistBase[dsym] + dextra;
        if (distance == 0 || distance > out.size()) return false;
        size_t start = out.size() - distance;
        for (size_t i = 0; i < length; ++i) {
            if (out.size() >= max_out) return false;
            out.push_back(out[start + i]);
        }
    }
}

bool inflate_impl(const uint8_t* in, size_t n, Bytes& out, size_t max_out) {
    BitReader br(in, n);
    out.clear();
    while (true) {
        uint32_t final_block = 0, type = 0;
        if (!br.get(1, final_block) || !br.get(2, type)) return false;
        if (type == 0) {                                   // stored
            br.align();
            size_t pos = br.byte_position();
            if (pos + 4 > n) return false;
            uint16_t len = uint16_t(in[pos] | (in[pos + 1] << 8));
            // uint16_t nlen = in[pos+2] | (in[pos+3] << 8);  (redundancy check omitted)
            pos += 4;
            if (pos + len > n) return false;
            if (out.size() + len > max_out) return false;
            out.insert(out.end(), in + pos, in + pos + len);
            // Re-seed the bit reader on the byte boundary after the stored block.
            br = BitReader(in + pos + len, n - pos - len);
        } else if (type == 1) {                            // fixed Huffman
            uint8_t lit_lengths[288];
            for (int i = 0; i < 144; ++i) lit_lengths[i] = 8;
            for (int i = 144; i < 256; ++i) lit_lengths[i] = 9;
            for (int i = 256; i < 280; ++i) lit_lengths[i] = 7;
            for (int i = 280; i < 288; ++i) lit_lengths[i] = 8;
            uint8_t dist_lengths[30];
            for (int i = 0; i < 30; ++i) dist_lengths[i] = 5;
            Huffman lit, dist;
            lit.build(lit_lengths, 288);
            dist.build(dist_lengths, 30);
            if (!inflate_block_body(br, lit, dist, out, max_out)) return false;
        } else if (type == 2) {                            // dynamic Huffman
            uint32_t hlit = 0, hdist = 0, hclen = 0;
            if (!br.get(5, hlit) || !br.get(5, hdist) || !br.get(4, hclen)) return false;
            size_t nlit = hlit + 257, ndist = hdist + 1, nclen = hclen + 4;
            static const uint8_t order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
            uint8_t clen_lengths[19] = {0};
            for (size_t i = 0; i < nclen; ++i) {
                uint32_t v = 0;
                if (!br.get(3, v)) return false;
                clen_lengths[order[i]] = uint8_t(v);
            }
            Huffman clen;
            clen.build(clen_lengths, 19);
            std::vector<uint8_t> lengths;
            lengths.reserve(nlit + ndist);
            while (lengths.size() < nlit + ndist) {
                uint16_t sym = 0;
                if (!clen.decode(br, sym)) return false;
                if (sym < 16) {
                    lengths.push_back(uint8_t(sym));
                } else if (sym == 16) {
                    if (lengths.empty()) return false;
                    uint32_t rep = 0;
                    if (!br.get(2, rep)) return false;
                    uint8_t prev = lengths.back();
                    for (uint32_t i = 0; i < rep + 3; ++i) lengths.push_back(prev);
                } else if (sym == 17) {
                    uint32_t rep = 0;
                    if (!br.get(3, rep)) return false;
                    for (uint32_t i = 0; i < rep + 3; ++i) lengths.push_back(0);
                } else {
                    uint32_t rep = 0;
                    if (!br.get(7, rep)) return false;
                    for (uint32_t i = 0; i < rep + 11; ++i) lengths.push_back(0);
                }
            }
            if (lengths.size() != nlit + ndist) return false;
            Huffman lit, dist;
            lit.build(lengths.data(), nlit);
            dist.build(lengths.data() + nlit, ndist);
            if (!inflate_block_body(br, lit, dist, out, max_out)) return false;
        } else {
            return false;
        }
        if (final_block) break;
        if (out.size() > max_out) return false;
    }
    return true;
}

uint32_t adler32(const uint8_t* data, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; ++i) {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

}  // namespace

bool inflate_bytes(const uint8_t* in, size_t n, Bytes& out) {
    constexpr size_t kMaxInflate = 256u * 1024 * 1024;
    if (n >= 2 && (in[0] & 0x0F) == 0x08 && ((uint16_t(in[0]) << 8) | in[1]) % 31 == 0) {
        // zlib wrapper: 2 byte header, deflate payload, 4 byte Adler-32 trailer.
        if (!inflate_impl(in + 2, n - 2, out, kMaxInflate)) return false;
        if (n >= 6) {
            uint32_t want = read_u32(in + n - 4);
            if (adler32(out.data(), out.size()) != want) return false;
        }
        return true;
    }
    return inflate_impl(in, n, out, kMaxInflate);
}

bool gunzip_bytes(const uint8_t* in, size_t n, Bytes& out) {
    if (n < 18 || in[0] != 0x1F || in[1] != 0x8B || in[2] != 8) return false;
    uint8_t flags = in[3];
    size_t pos = 10;
    if (flags & 0x04) {                       // FEXTRA
        if (pos + 2 > n) return false;
        size_t xlen = size_t(in[pos]) | (size_t(in[pos + 1]) << 8);
        pos += 2 + xlen;
    }
    auto skip_zstring = [&]() -> bool {
        while (pos < n && in[pos] != 0) ++pos;
        if (pos >= n) return false;
        ++pos;
        return true;
    };
    if (flags & 0x08) if (!skip_zstring()) return false;   // FNAME
    if (flags & 0x10) if (!skip_zstring()) return false;   // FCOMMENT
    if (flags & 0x02) pos += 2;                            // FHCRC
    if (pos >= n) return false;
    return inflate_impl(in + pos, n - pos, out, 256u * 1024 * 1024);
}

// =============================================================================
// HttpRequest / HttpResponse
// =============================================================================
void HttpRequest::set_header(const std::string& name, const std::string& value) {
    for (auto& kv : headers) {
        if (iequals(kv.first, name)) { kv.second = value; return; }
    }
    headers.emplace_back(name, value);
}
const std::string* HttpRequest::get_header(const std::string& name) const {
    for (const auto& kv : headers) if (iequals(kv.first, name)) return &kv.second;
    return nullptr;
}
std::string HttpResponse::header(const std::string& name) const {
    auto it = headers.find(lower(name));
    return it == headers.end() ? std::string() : it->second;
}
std::string HttpResponse::content_type() const {
    std::string ct = header("content-type");
    size_t semi = ct.find(';');
    return trim(semi == std::string::npos ? ct : ct.substr(0, semi));
}

namespace {

// Reads from `stream` until the header terminator, then parses the response head.
Result<size_t> read_headers(Stream& stream, Bytes& buffer, size_t max_bytes, int64_t deadline) {
    const std::string term = "\r\n\r\n";
    while (true) {
        std::string hay(reinterpret_cast<const char*>(buffer.data()), buffer.size());
        size_t hit = hay.find(term);
        if (hit != std::string::npos) return hit + term.size();
        if (buffer.size() > max_bytes) return LCA_FAIL(Code::ProtocolError, "response headers too large");
        uint8_t chunk[8192];
        Result<size_t> r = stream.read_some(chunk, sizeof(chunk), deadline);
        if (!r.ok()) return r.error();
        if (*r == 0) {
            if (buffer.empty()) return LCA_FAIL(Code::NetworkError, "server closed connection");
            return buffer.size();
        }
        buffer.insert(buffer.end(), chunk, chunk + *r);
    }
}

int status_from_line(const std::string& line, std::string& version, std::string& reason) {
    // "HTTP/1.1 200 OK"
    size_t sp1 = line.find(' ');
    if (sp1 == std::string::npos) return -1;
    version = line.substr(0, sp1);
    size_t sp2 = line.find(' ', sp1 + 1);
    std::string code = line.substr(sp1 + 1, sp2 == std::string::npos ? std::string::npos
                                                                    : sp2 - sp1 - 1);
    reason = sp2 == std::string::npos ? "" : line.substr(sp2 + 1);
    for (char c : code) if (c < '0' || c > '9') return -1;
    return std::atoi(code.c_str());
}

}  // namespace

Result<HttpResponse> http_exchange(Stream& stream, const HttpRequest& req, const Url& url) {
    constexpr size_t kHeaderCap = 256 * 1024;
    int64_t start = now_millis();

    // ---- build request -------------------------------------------------
    std::string head;
    head += req.method;
    head += ' ';
    head += url.request_target();
    head += " HTTP/1.1\r\n";
    head += "Host: " + url.host + (url.port == 443 || url.port == 80 ? "" : ":" + std::to_string(url.port)) + "\r\n";
    head += "User-Agent: local-claude-code-agent/1.0 (+https://github.com)\r\n";
    head += "Accept: */*\r\n";
    head += "Connection: close\r\n";
    bool has_accept_encoding = false, has_content_length = false, has_content_type = false;
    for (const auto& kv : req.headers) {
        head += kv.first + ": " + kv.second + "\r\n";
        if (iequals(kv.first, "accept-encoding")) has_accept_encoding = true;
        if (iequals(kv.first, "content-length")) has_content_length = true;
        if (iequals(kv.first, "content-type")) has_content_type = true;
    }
    if (req.accept_gzip && !has_accept_encoding) head += "Accept-Encoding: gzip, deflate\r\n";
    if (!req.body.empty() || req.method == "POST" || req.method == "PUT" || req.method == "PATCH") {
        if (!has_content_length) head += "Content-Length: " + std::to_string(req.body.size()) + "\r\n";
        if (!has_content_type && !req.body_content_type.empty())
            head += "Content-Type: " + req.body_content_type + "\r\n";
    }
    head += "\r\n";

    Bytes wire(head.begin(), head.end());
    wire.insert(wire.end(), req.body.begin(), req.body.end());

    int64_t write_deadline = now_millis() + req.timeout_ms;
    Error werr = stream.write_all(wire.data(), wire.size(), write_deadline);
    if (!werr.ok()) return werr;

    // ---- read head -----------------------------------------------------
    Bytes buffer;
    buffer.reserve(16384);
    int64_t head_deadline = now_millis() + req.timeout_ms;
    Result<size_t> head_len = read_headers(stream, buffer, kHeaderCap, head_deadline);
    if (!head_len.ok()) return head_len.error();
    std::string head_text(reinterpret_cast<const char*>(buffer.data()), *head_len);

    HttpResponse resp;
    resp.final_url = url.to_string();
    std::vector<std::string> lines = split_lines(head_text);
    if (lines.empty()) return LCA_FAIL(Code::ProtocolError, "empty response");
    resp.status = status_from_line(lines[0], resp.http_version, resp.reason);
    if (resp.status < 0) return LCA_FAIL(Code::ProtocolError, "malformed status line: " + lines[0]);
    for (size_t i = 1; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        if (line.empty()) break;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = lower(trim(line.substr(0, colon)));
        std::string value = trim(line.substr(colon + 1));
        auto it = resp.headers.find(name);
        if (it == resp.headers.end()) resp.headers[name] = value;
        else it->second += ", " + value;
    }

    Bytes body;
    body.reserve(8192);
    size_t header_bytes = *head_len;
    body.insert(body.end(), buffer.begin() + header_bytes, buffer.end());
    resp.bytes_received = buffer.size() - header_bytes;

    bool chunked = contains(lower(resp.header("transfer-encoding")), "chunked");
    long long content_length = -1;
    {
        std::string cl = resp.header("content-length");
        if (!cl.empty()) {
            try { content_length = std::stoll(cl); } catch (...) { content_length = -1; }
        }
    }

    // Per RFC 9110 a response to HEAD carries no body, and 204/304 are always
    // bodyless, no matter what Content-Length claims.
    const bool bodyless = req.method == "HEAD" || resp.status == 204 || resp.status == 304 ||
                          resp.status == 100;
    if (bodyless) {
        body.clear();
        resp.bytes_received = 0;
    }

    auto fill_to = [&](size_t target) -> Error {
        while (body.size() < target) {
            uint8_t chunk[16384];
            size_t want = std::min(sizeof(chunk), target - body.size());
            int64_t dl = now_millis() + req.timeout_ms;
            Result<size_t> r = stream.read_some(chunk, want, dl);
            if (!r.ok()) return r.error();
            if (*r == 0) return LCA_FAIL(Code::NetworkError, "connection closed early");
            body.insert(body.end(), chunk, chunk + *r);
            resp.bytes_received += *r;
            if (resp.bytes_received > req.max_response_bytes)
                return LCA_FAIL(Code::ResourceExhausted, "response exceeded max_response_bytes");
        }
        return {};
    };

    if (bodyless) {
        // nothing to read: fall through to content decoding / return
    } else if (chunked) {
        // Incremental de-chunking: consume from `body`, refilling as needed.
        Bytes decoded;
        size_t pos = 0;
        while (true) {
            // Ensure we have a full size line.
            while (true) {
                std::string_view view(reinterpret_cast<const char*>(body.data()) + pos, body.size() - pos);
                size_t nl = view.find("\r\n");
                if (nl != std::string_view::npos) break;
                Error e = fill_to(body.size() + 1);
                if (!e.ok()) return e;
            }
            std::string_view view(reinterpret_cast<const char*>(body.data()) + pos, body.size() - pos);
            size_t nl = view.find("\r\n");
            std::string size_line(view.substr(0, nl));
            size_t semi = size_line.find(';');
            if (semi != std::string::npos) size_line = size_line.substr(0, semi);
            long long chunk_size = 0;
            try { chunk_size = std::stoll(trim(size_line), nullptr, 16); } catch (...) { chunk_size = -1; }
            if (chunk_size < 0) return LCA_FAIL(Code::ProtocolError, "bad chunk size");
            pos += nl + 2;
            if (chunk_size == 0) break;
            Error e = fill_to(pos + size_t(chunk_size) + 2);
            if (!e.ok()) return e;
            decoded.insert(decoded.end(), body.begin() + pos, body.begin() + pos + chunk_size);
            pos += size_t(chunk_size) + 2;   // skip trailing CRLF
            if (decoded.size() > req.max_response_bytes)
                return LCA_FAIL(Code::ResourceExhausted, "response exceeded max_response_bytes");
        }
        body = std::move(decoded);
    } else if (content_length >= 0) {
        Error e = fill_to(size_t(content_length));
        if (!e.ok()) return e;
        body.resize(size_t(content_length));
    } else {
        // Read until EOF (Connection: close).
        while (true) {
            uint8_t chunk[16384];
            int64_t dl = now_millis() + req.timeout_ms;
            Result<size_t> r = stream.read_some(chunk, sizeof(chunk), dl);
            if (!r.ok()) {
                if (r.error().code == Code::Timeout) break;
                return r.error();
            }
            if (*r == 0) break;
            body.insert(body.end(), chunk, chunk + *r);
            resp.bytes_received += *r;
            if (body.size() > req.max_response_bytes)
                return LCA_FAIL(Code::ResourceExhausted, "response exceeded max_response_bytes");
        }
    }

    // ---- content decoding ---------------------------------------------
    std::string encoding = lower(resp.header("content-encoding"));
    if (bodyless) encoding.clear();
    if (!body.empty() && (contains(encoding, "gzip") || contains(encoding, "x-gzip"))) {
        Bytes inflated;
        if (gunzip_bytes(body.data(), body.size(), inflated)) body = std::move(inflated);
    } else if (!body.empty() && contains(encoding, "deflate")) {
        Bytes inflated;
        if (inflate_bytes(body.data(), body.size(), inflated)) body = std::move(inflated);
    }
    resp.body = std::move(body);
    resp.elapsed_ms = now_millis() - start;
    return resp;
}

Result<HttpResponse> http_request(const HttpRequest& req_in) {
    HttpRequest req = req_in;
    Url url;
    {
        Result<Url> parsed = parse_url(req.url);
        if (!parsed.ok()) return parsed.error();
        url = *parsed;
    }
    int redirects = 0;
    while (true) {
        std::unique_ptr<Stream> stream;
        if (url.scheme == "https") {
            auto conn = TcpStream::connect(url.host, url.port, req.timeout_ms);
            if (!conn.ok()) return conn.error();
            TlsClient tls;
            TlsConfig cfg;
            cfg.server_name = url.host;
            cfg.timeout_ms = req.timeout_ms;
            Error herr = tls.handshake(std::move(*conn), cfg);
            if (!herr.ok()) return herr;
            stream = tls.take_stream();
        } else {
            auto conn = TcpStream::connect(url.host, url.port, req.timeout_ms);
            if (!conn.ok()) return conn.error();
            stream = std::move(*conn);
        }

        Result<HttpResponse> resp = http_exchange(*stream, req, url);
        if (!resp.ok()) return resp;

        bool is_redirect = resp->status == 301 || resp->status == 302 || resp->status == 303 ||
                           resp->status == 307 || resp->status == 308;
        if (!is_redirect || !req.follow_redirects || redirects >= req.max_redirects) {
            resp->redirects = redirects;
            return resp;
        }
        std::string location = resp->header("location");
        if (location.empty()) { resp->redirects = redirects; return resp; }
        Result<Url> next = resolve_url(url, location);
        if (!next.ok()) return next.error();
        url = *next;
        ++redirects;
        if (resp->status == 303 || ((resp->status == 301 || resp->status == 302) && req.method == "POST")) {
            req.method = "GET";
            req.body.clear();
            for (auto it = req.headers.begin(); it != req.headers.end();) {
                if (iequals(it->first, "content-length") || iequals(it->first, "content-type"))
                    it = req.headers.erase(it);
                else ++it;
            }
        }
        LCA_LOG_DEBUG(kScope, "redirect " + std::to_string(resp->status) + " -> " + url.to_string());
    }
}

}  // namespace lca
