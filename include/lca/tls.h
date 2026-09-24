// =============================================================================
//  lca/tls.h  --  TLS 1.3 client (RFC 8446) built on the local crypto library
// -----------------------------------------------------------------------------
//  Supported negotiation:
//    * Key exchange : X25519 (RFC 7748)
//    * Cipher suites: TLS_AES_128_GCM_SHA256, TLS_AES_256_GCM_SHA384,
//                     TLS_CHACHA20_POLY1305_SHA256
//    * Auth         : ECDSA P-256/P-384 and RSA (PSS + PKCS#1 v1.5) server certs
//    * ALPN         : h2 is NOT offered (HTTP/1.1 only), http/1.1 by default
//  Certificate paths are validated against the system trust store with the
//  X.509 code in x509.cpp; no OpenSSL, no external TLS runtime.
// =============================================================================
#ifndef LCA_TLS_H
#define LCA_TLS_H

#include "lca/net.h"
#include "lca/x509.h"

namespace lca {

enum class TlsVersion : uint16_t {
    Tls12 = 0x0303,
    Tls13 = 0x0304,
};

struct TlsConfig {
    std::string server_name;                  // SNI + hostname verification
    bool verify_peer{true};
    bool allow_untrusted_self_signed{false};
    std::string ca_bundle_path;               // empty => search default locations
    int  timeout_ms{15000};
    std::vector<std::string> alpn{"http/1.1"};
    size_t max_record_plaintext{16384};
    bool enable_tls12_fallback{true};
    std::string client_random_override;       // test hook (hex, 32 bytes)
};

struct TlsSessionInfo {
    TlsVersion version{TlsVersion::Tls13};
    uint16_t   cipher_suite{0};
    std::string cipher_name;
    std::string group_name;
    size_t     hash_len{32};
    bool       verified{false};
    bool       resumed{false};
    std::string alpn;
    std::string peer_subject;
    std::string peer_issuer;
    std::string peer_valid_to;
    std::string peer_key;
    std::string chain_summary;
    size_t     chain_length{0};
    Bytes      session_ticket;
    int64_t    handshake_ms{0};
};

const char* tls_cipher_name(uint16_t suite);

// -----------------------------------------------------------------------------
// Record-layer stream (encrypts writes, decrypts reads)
// -----------------------------------------------------------------------------
class TlsStream : public Stream {
public:
    // Configuration of the record layer (public so the handshake driver can
    // size records and deadlines before the stream is handed to the caller).
    size_t max_plaintext_{16384};
    int    timeout_ms_{30000};

    TlsStream(std::unique_ptr<Stream> inner, TlsSessionInfo info)
        : inner_(std::move(inner)), info_(std::move(info)) {}
    ~TlsStream() override;

    Error write_all(const uint8_t* data, size_t n, int64_t deadline_ms) override;
    Result<size_t> read_some(uint8_t* out, size_t cap, int64_t deadline_ms) override;
    void  close() override;
    bool  closed() const override { return closed_; }
    std::string description() const override;

    const TlsSessionInfo& info() const { return info_; }
    // True when the peer sent a close_notify alert.
    bool peer_closed_cleanly() const { return peer_close_notify_; }

    // Internal: used by TlsClient while completing the handshake.
    struct Keys {
        Bytes key, iv;
        uint64_t seq{0};
    };
    // Record layer entry point used by the handshake driver.
    Error read_record_bytes(Bytes& content_type_out, Bytes& plaintext_out, int64_t deadline_ms) {
        return read_record(content_type_out, plaintext_out, deadline_ms);
    }
    // Sends a handshake message in an encrypted record (inner type 0x16).
    // Needed for the client Finished, which is sent before the record layer
    // switches to application data.
    Error write_handshake(const uint8_t* data, size_t n, int64_t deadline_ms);
    Error write_records(uint8_t inner_type, const uint8_t* data, size_t n, int64_t deadline_ms);
    // Marks the record layer as ready for post-handshake bookkeeping (session
    // tickets, key updates).  Before this is set, handshake records are handed
    // to the handshake driver instead of being consumed internally.
    void set_handshake_complete() { handshake_complete_ = true; }
    void set_read_keys(Keys k)  { read_keys_ = std::move(k); }
    void set_write_keys(Keys k) { write_keys_ = std::move(k); }
    Keys& read_keys()  { return read_keys_; }
    Keys& write_keys() { return write_keys_; }
    void set_info(TlsSessionInfo info) { info_ = std::move(info); }

private:
    Error send_alert(uint8_t level, uint8_t description, int64_t deadline_ms);
    Error read_record(Bytes& content_type_out, Bytes& plaintext_out, int64_t deadline_ms);
    Error handle_post_handshake(Bytes& plaintext, size_t& consumed);

    std::unique_ptr<Stream> inner_;
    TlsSessionInfo info_;
    Keys read_keys_, write_keys_;
    Bytes read_buffer_;          // decrypted application data not yet consumed
    Bytes handshake_buffer_;     // reassembly buffer for post-handshake messages
    bool closed_{false};
    bool peer_close_notify_{false};
    bool sent_close_notify_{false};
    bool handshake_complete_{false};
};

// -----------------------------------------------------------------------------
// Client handshake driver
// -----------------------------------------------------------------------------
class TlsClient {
public:
    // Performs the full handshake.  The transport is taken by value so the
    // resulting encrypted stream owns the socket for its whole lifetime.
    Error handshake(std::unique_ptr<Stream> transport, const TlsConfig& cfg);
    std::unique_ptr<Stream> take_stream() { return std::move(stream_); }
    const TlsSessionInfo& info() const { return info_; }

private:
    std::unique_ptr<TlsStream> stream_;
    TlsSessionInfo info_;
};

}  // namespace lca

#endif  // LCA_TLS_H
