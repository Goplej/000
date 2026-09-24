// =============================================================================
//  src/tls.cpp  --  TLS 1.3 client implementation (RFC 8446)
// =============================================================================
#include "lca/tls.h"

#include <cstring>

namespace lca {

namespace {

constexpr const char* kScope = "tls";

// Record content types
constexpr uint8_t kCtChangeCipherSpec = 20;
constexpr uint8_t kCtAlert            = 21;
constexpr uint8_t kCtHandshake        = 22;
constexpr uint8_t kCtApplicationData  = 23;

// Handshake message types
constexpr uint8_t kHsClientHello       = 1;
constexpr uint8_t kHsServerHello       = 2;
constexpr uint8_t kHsNewSessionTicket  = 4;
constexpr uint8_t kHsEncryptedExtensions = 8;
constexpr uint8_t kHsCertificate       = 11;
constexpr uint8_t kHsCertificateVerify = 15;
constexpr uint8_t kHsFinished          = 20;
constexpr uint8_t kHsKeyUpdate         = 24;

// Extension types
constexpr uint16_t kExtServerName        = 0;
constexpr uint16_t kExtStatusRequest     = 5;
constexpr uint16_t kExtSupportedGroups   = 10;
constexpr uint16_t kExtEcPointFormats    = 11;
constexpr uint16_t kExtSignatureAlgs     = 13;
constexpr uint16_t kExtAlpn              = 16;
constexpr uint16_t kExtSupportedVersions = 43;
constexpr uint16_t kExtPskModes          = 45;
constexpr uint16_t kExtKeyShare          = 51;
constexpr uint16_t kExtSignatureAlgsCert = 50;

constexpr uint16_t kGroupX25519    = 0x001D;
constexpr uint16_t kGroupSecp256r1 = 0x0017;

constexpr uint16_t kSuiteAes128GcmSha256   = 0x1301;
constexpr uint16_t kSuiteAes256GcmSha384   = 0x1302;
constexpr uint16_t kSuiteChaCha20Sha256    = 0x1303;

constexpr size_t kMaxHandshakeMessage = 128 * 1024;

const char* alert_name(uint8_t desc) {
    switch (desc) {
        case 0:  return "close_notify";
        case 10: return "unexpected_message";
        case 20: return "bad_record_mac";
        case 22: return "record_overflow";
        case 40: return "handshake_failure";
        case 42: return "bad_certificate";
        case 43: return "unsupported_certificate";
        case 44: return "certificate_revoked";
        case 45: return "certificate_expired";
        case 46: return "certificate_unknown";
        case 47: return "illegal_parameter";
        case 48: return "unknown_ca";
        case 49: return "access_denied";
        case 50: return "decode_error";
        case 51: return "decrypt_error";
        case 70: return "protocol_version";
        case 71: return "insufficient_security";
        case 80: return "internal_error";
        case 90: return "user_canceled";
        case 109: return "missing_extension";
        case 110: return "unsupported_extension";
        case 112: return "unrecognized_name";
        case 116: return "certificate_required";
        case 120: return "no_application_protocol";
        default:  return "unknown_alert";
    }
}

// ---------------------------------------------------------------------------
// Hash abstraction: the two TLS 1.3 suites we support use SHA-256 / SHA-384.
// ---------------------------------------------------------------------------
struct SuiteInfo {
    uint16_t id;
    const char* name;
    size_t key_len;
    size_t hash_len;
    int    aead;      // 0 = AES-128-GCM, 1 = AES-256-GCM, 2 = ChaCha20-Poly1305
    int    hash;      // 0 = SHA-256, 1 = SHA-384
};

const SuiteInfo* suite_info(uint16_t id) {
    static const SuiteInfo suites[] = {
        {kSuiteAes128GcmSha256, "TLS_AES_128_GCM_SHA256",       16, 32, 0, 0},
        {kSuiteAes256GcmSha384, "TLS_AES_256_GCM_SHA384",       32, 48, 1, 1},
        {kSuiteChaCha20Sha256,  "TLS_CHACHA20_POLY1305_SHA256", 32, 32, 2, 0},
    };
    for (const SuiteInfo& s : suites) if (s.id == id) return &s;
    return nullptr;
}

inline crypto::HashKind hash_kind(int which) {
    return which == 1 ? crypto::HashKind::Sha384 : crypto::HashKind::Sha256;
}
Bytes hash_bytes(int which, const uint8_t* data, size_t n) {
    return crypto::hash_bytes(hash_kind(which), data, n);
}
Bytes hkdf_label(int which, const Bytes& secret, const char* label, const Bytes& ctx, size_t len) {
    return crypto::hkdf_expand_label(hash_kind(which), secret, label, ctx, len);
}

struct Aead {
    int    kind{0};
    size_t key_len{16};

    void seal(const uint8_t* key, const uint8_t* nonce, const uint8_t* aad, size_t aad_len,
              const uint8_t* in, size_t n, uint8_t* out, uint8_t tag[16]) const {
        if (kind == 0)      crypto::aes128_gcm_encrypt(key, nonce, aad, aad_len, in, n, out, tag);
        else if (kind == 1) crypto::aes256_gcm_encrypt(key, nonce, aad, aad_len, in, n, out, tag);
        else                crypto::chacha20_poly1305_encrypt(key, nonce, aad, aad_len, in, n, out, tag);
    }
    bool open(const uint8_t* key, const uint8_t* nonce, const uint8_t* aad, size_t aad_len,
              const uint8_t* in, size_t n, const uint8_t tag[16], uint8_t* out) const {
        if (kind == 0)      return crypto::aes128_gcm_decrypt(key, nonce, aad, aad_len, in, n, tag, out);
        if (kind == 1)      return crypto::aes256_gcm_decrypt(key, nonce, aad, aad_len, in, n, tag, out);
        return crypto::chacha20_poly1305_decrypt(key, nonce, aad, aad_len, in, n, tag, out);
    }
};

void build_nonce(const Bytes& iv, uint64_t seq, uint8_t nonce[12]) {
    std::memcpy(nonce, iv.data(), 12);
    for (int i = 0; i < 8; ++i) nonce[11 - i] ^= uint8_t(seq >> (i * 8));
}

// ---------------------------------------------------------------------------
// Handshake message helpers
// ---------------------------------------------------------------------------
void append_handshake(Bytes& out, uint8_t type, const Bytes& body) {
    append_u8(out, type);
    append_u24(out, uint32_t(body.size()));
    out.insert(out.end(), body.begin(), body.end());
}

void append_extension(Bytes& out, uint16_t type, const Bytes& body) {
    append_u16(out, type);
    append_u16(out, uint16_t(body.size()));
    out.insert(out.end(), body.begin(), body.end());
}

Bytes make_vec8(const Bytes& b)  { Bytes out; append_u8(out, uint8_t(b.size()));  out.insert(out.end(), b.begin(), b.end()); return out; }
Bytes make_vec16(const Bytes& b) { Bytes out; append_u16(out, uint16_t(b.size())); out.insert(out.end(), b.begin(), b.end()); return out; }

struct RecordHeader {
    uint8_t type{0};
    uint16_t length{0};
    uint8_t version[2]{0, 0};
};

}  // namespace

const char* tls_cipher_name(uint16_t suite) {
    const SuiteInfo* s = suite_info(suite);
    return s ? s->name : "unknown";
}

// =============================================================================
// TlsStream -- record layer
// =============================================================================
TlsStream::~TlsStream() { close(); }

std::string TlsStream::description() const {
    std::string base = inner_ ? inner_->description() : "closed";
    return "tls://" + base + " [" + tls_cipher_name(info_.cipher_suite) + "]";
}

void TlsStream::close() {
    if (closed_) return;
    if (inner_ && !inner_->closed() && !sent_close_notify_ && !write_keys_.key.empty())
        send_alert(1, 0, now_millis() + 2000);
    closed_ = true;
    if (inner_) inner_->close();
}

Error TlsStream::send_alert(uint8_t level, uint8_t description, int64_t deadline_ms) {
    uint8_t plain[2] = {level, description};
    uint8_t nonce[12];
    build_nonce(write_keys_.iv, write_keys_.seq, nonce);
    Bytes inner(plain, plain + 2);
    inner.push_back(kCtAlert);

    Bytes record;
    uint16_t len = uint16_t(inner.size() + 16);
    append_u8(record, kCtApplicationData);
    append_u16(record, 0x0303);
    append_u16(record, len);
    size_t header_len = record.size();
    record.resize(header_len + inner.size() + 16);
    uint8_t tag[16];
    const Aead aead{info_.cipher_suite == kSuiteAes256GcmSha384 ? 1
                 : info_.cipher_suite == kSuiteChaCha20Sha256  ? 2
                                                               : 0,
                    write_keys_.key.size()};
    aead.seal(write_keys_.key.data(), nonce, record.data(), header_len, inner.data(), inner.size(),
              record.data() + header_len, tag);
    std::memcpy(record.data() + header_len + inner.size(), tag, 16);
    write_keys_.seq++;
    if (description == 0) sent_close_notify_ = true;
    if (!inner_) return {};
    return inner_->write_all(record.data(), record.size(), deadline_ms);
}

Error TlsStream::read_record(Bytes& content_type_out, Bytes& plaintext_out, int64_t deadline_ms) {
    plaintext_out.clear();
    while (true) {
        uint8_t header[5];
        Error err = inner_->read_exact(header, 5, deadline_ms);
        if (!err.ok()) return err;
        uint8_t type = header[0];
        uint16_t length = read_u16(header + 3);
        if (length == 0 || length > 16640) {
            return LCA_FAIL(Code::TlsError, "invalid record length " + std::to_string(length));
        }
        Bytes payload(length);
        err = inner_->read_exact(payload.data(), length, deadline_ms);
        if (!err.ok()) return err;

        if (type == kCtChangeCipherSpec) continue;            // middlebox compatibility
        if (type == kCtAlert && length >= 2) {
            uint8_t desc = payload[1];
            if (desc == 0) {
                peer_close_notify_ = true;
                return LCA_FAIL(Code::NetworkError, "peer sent close_notify");
            }
            return LCA_FAIL(Code::TlsError, std::string("peer alert: ") + alert_name(desc));
        }

        if (read_keys_.key.empty()) {
            // Handshake record that is still in the clear (pre key derivation).
            content_type_out.assign(1, type);
            plaintext_out = std::move(payload);
            return {};
        }
        if (type != kCtApplicationData) {
            // Plaintext record after keys were installed: TLS 1.3 forbids this.
            return LCA_FAIL(Code::TlsError, "unexpected plaintext record type " +
                                                std::to_string(int(type)));
        }
        if (length < 17) return LCA_FAIL(Code::TlsError, "encrypted record too short");

        uint8_t nonce[12];
        build_nonce(read_keys_.iv, read_keys_.seq, nonce);
        Bytes plain(length - 16);
        const Aead aead{info_.cipher_suite == kSuiteAes256GcmSha384 ? 1
                     : info_.cipher_suite == kSuiteChaCha20Sha256  ? 2
                                                                   : 0,
                        read_keys_.key.size()};
        if (!aead.open(read_keys_.key.data(), nonce, header, 5, payload.data(), length - 16,
                       payload.data() + length - 16, plain.data())) {
            return LCA_FAIL(Code::TlsError, "record authentication failed (bad MAC)");
        }
        read_keys_.seq++;

        // Strip zero padding; the last non-zero byte is the real content type.
        size_t end = plain.size();
        while (end > 0 && plain[end - 1] == 0) --end;
        if (end == 0) return LCA_FAIL(Code::TlsError, "empty inner plaintext");
        uint8_t inner_type = plain[end - 1];
        plain.resize(end - 1);

        if (inner_type == kCtAlert) {
            uint8_t desc = plain.size() >= 2 ? plain[1] : 255;
            if (desc == 0) {
                peer_close_notify_ = true;
                return LCA_FAIL(Code::NetworkError, "peer sent close_notify");
            }
            return LCA_FAIL(Code::TlsError, std::string("peer alert: ") + alert_name(desc));
        }
        if (inner_type == kCtHandshake) {
            if (!handshake_complete_) {
                // Part of the handshake flight: hand it to the driver untouched.
                content_type_out.assign(1, inner_type);
                plaintext_out = std::move(plain);
                return {};
            }
            // Post-handshake message (session ticket / key update).
            Error e = handle_post_handshake(plain, end);
            if (!e.ok()) return e;
            continue;   // carries no application data
        }
        if (inner_type != kCtApplicationData) {
            return LCA_FAIL(Code::TlsError, "unexpected inner content type " + std::to_string(int(inner_type)));
        }
        content_type_out.assign(1, inner_type);
        plaintext_out = std::move(plain);
        return {};
    }
}

Error TlsStream::handle_post_handshake(Bytes& plaintext, size_t& consumed) {
    consumed = 0;
    handshake_buffer_.insert(handshake_buffer_.end(), plaintext.begin(), plaintext.end());
    while (handshake_buffer_.size() >= 4) {
        uint8_t type = handshake_buffer_[0];
        uint32_t len = read_u24(handshake_buffer_.data() + 1);
        if (len > kMaxHandshakeMessage)
            return LCA_FAIL(Code::TlsError, "post-handshake message too large");
        if (handshake_buffer_.size() < 4 + len) break;
        Bytes body(handshake_buffer_.begin() + 4, handshake_buffer_.begin() + 4 + len);
        handshake_buffer_.erase(handshake_buffer_.begin(), handshake_buffer_.begin() + 4 + len);

        if (type == kHsNewSessionTicket) {
            // ticket_nonce(1) ticket(2) ... -- keep the raw ticket for the caller.
            ByteReader r(body);
            uint8_t nonce_len = r.u8();
            if (!r.skip(nonce_len)) continue;
            info_.session_ticket = r.vec16();
            LCA_LOG_DEBUG(kScope, "received NewSessionTicket (" +
                                      std::to_string(info_.session_ticket.size()) + " bytes)");
            continue;
        }
        if (type == kHsKeyUpdate) {
            LCA_LOG_DEBUG(kScope, "peer requested a key update");
            return LCA_FAIL(Code::Unsupported, "peer-initiated TLS key update is not supported");
        }
        LCA_LOG_DEBUG(kScope, "ignoring post-handshake message type " + std::to_string(int(type)));
    }
    consumed = plaintext.size();
    return {};
}

Error TlsStream::write_all(const uint8_t* data, size_t n, int64_t deadline_ms) {
    return write_records(kCtApplicationData, data, n, deadline_ms);
}

Error TlsStream::write_handshake(const uint8_t* data, size_t n, int64_t deadline_ms) {
    return write_records(kCtHandshake, data, n, deadline_ms);
}

Error TlsStream::write_records(uint8_t inner_type, const uint8_t* data, size_t n, int64_t deadline_ms) {
    if (closed_) return LCA_FAIL(Code::TlsError, "stream is closed");
    size_t off = 0;
    while (off < n) {
        size_t chunk = std::min(n - off, max_plaintext_);
        Bytes inner(data + off, data + off + chunk);
        inner.push_back(inner_type);

        uint8_t nonce[12];
        build_nonce(write_keys_.iv, write_keys_.seq, nonce);
        Bytes record;
        append_u8(record, kCtApplicationData);
        append_u16(record, 0x0303);
        append_u16(record, uint16_t(inner.size() + 16));
        size_t header_len = record.size();
        record.resize(header_len + inner.size() + 16);
        uint8_t tag[16];
        const Aead aead{info_.cipher_suite == kSuiteAes256GcmSha384 ? 1
                     : info_.cipher_suite == kSuiteChaCha20Sha256  ? 2
                                                                   : 0,
                        write_keys_.key.size()};
        aead.seal(write_keys_.key.data(), nonce, record.data(), header_len, inner.data(),
                  inner.size(), record.data() + header_len, tag);
        std::memcpy(record.data() + header_len + inner.size(), tag, 16);
        write_keys_.seq++;

        Error err = inner_->write_all(record.data(), record.size(), deadline_ms);
        if (!err.ok()) return err;
        off += chunk;
    }
    return {};
}

Result<size_t> TlsStream::read_some(uint8_t* out, size_t cap, int64_t deadline_ms) {
    if (!read_buffer_.empty()) {
        size_t take = std::min(cap, read_buffer_.size());
        std::memcpy(out, read_buffer_.data(), take);
        read_buffer_.erase(read_buffer_.begin(), read_buffer_.begin() + long(take));
        return take;
    }
    if (closed_ || peer_close_notify_) return LCA_FAIL(Code::NetworkError, "connection closed");
    Bytes type, plain;
    Error err = read_record(type, plain, deadline_ms);
    if (!err.ok()) {
        if (err.code == Code::NetworkError && peer_close_notify_) return size_t(0);
        return err;
    }
    if (plain.empty()) return read_some(out, cap, deadline_ms);
    size_t take = std::min(cap, plain.size());
    std::memcpy(out, plain.data(), take);
    if (take < plain.size()) {
        read_buffer_.assign(plain.begin() + long(take), plain.end());
    }
    return take;
}

// =============================================================================
// TlsClient::handshake
// =============================================================================
namespace {

struct HandshakeMaterial {
    Bytes transcript;                 // raw concatenated handshake messages
    Bytes client_random;
    Bytes server_random;
    Bytes session_id;
    Bytes client_priv, client_pub;
    Bytes server_pub;
    Bytes shared_secret;
    const SuiteInfo* suite{nullptr};

    Bytes handshake_secret, master_secret;
    Bytes client_hs_traffic, server_hs_traffic;
    Bytes client_ap_traffic, server_ap_traffic;

    Bytes transcript_hash() const { return hash_bytes(suite->hash, transcript.data(), transcript.size()); }
};

TlsStream::Keys derive_keys(const SuiteInfo* suite, const Bytes& secret) {
    TlsStream::Keys keys;
    keys.key = hkdf_label(suite->hash, secret, "key", Bytes(), suite->key_len);
    keys.iv  = hkdf_label(suite->hash, secret, "iv", Bytes(), 12);
    keys.seq = 0;
    return keys;
}

Bytes finished_key(const SuiteInfo* suite, const Bytes& traffic_secret) {
    return hkdf_label(suite->hash, traffic_secret, "finished", Bytes(), suite->hash_len);
}

Bytes compute_finished(const SuiteInfo* suite, const Bytes& traffic_secret, const Bytes& transcript_hash) {
    Bytes key = finished_key(suite, traffic_secret);
    return crypto::hmac(hash_kind(suite->hash), key, transcript_hash.data(), transcript_hash.size());
}

}  // namespace

Error TlsClient::handshake(std::unique_ptr<Stream> transport, const TlsConfig& cfg) {
    int64_t start = now_millis();
    if (!transport) return LCA_FAIL(Code::InvalidArgument, "TLS transport is null");
    Stream* raw_transport = transport.get();   // still valid after the move below
    if (cfg.server_name.empty()) return LCA_FAIL(Code::InvalidArgument, "TLS server_name is required");

    HandshakeMaterial hs;
    hs.client_random = cfg.client_random_override.empty()
                           ? crypto::random_bytes(32)
                           : Bytes([&] {
                                 Bytes out;
                                 crypto::hex_decode(cfg.client_random_override, out);
                                 out.resize(32, 0);
                                 return out;
                             }());
    hs.session_id = crypto::random_bytes(32);
    hs.client_priv.assign(32, 0);
    hs.client_pub.assign(32, 0);
    crypto::x25519_keypair(hs.client_priv.data(), hs.client_pub.data());

    // ---- ClientHello ----------------------------------------------------
    {
        Bytes body;
        append_u16(body, 0x0303);                       // legacy_version
        body.insert(body.end(), hs.client_random.begin(), hs.client_random.end());
        Bytes sid = make_vec8(hs.session_id);
        body.insert(body.end(), sid.begin(), sid.end());

        Bytes suites;
        for (uint16_t s : {kSuiteAes128GcmSha256, kSuiteAes256GcmSha384, kSuiteChaCha20Sha256})
            append_u16(suites, s);
        Bytes suites_vec = make_vec16(suites);
        body.insert(body.end(), suites_vec.begin(), suites_vec.end());
        append_u8(body, 1);                             // compression: null
        append_u8(body, 0);

        Bytes ext;
        {   // server_name
            Bytes name = to_bytes(cfg.server_name);
            Bytes list;
            append_u8(list, 0);                         // host_name
            Bytes nv = make_vec16(name);
            list.insert(list.end(), nv.begin(), nv.end());
            Bytes se = make_vec16(list);
            append_extension(ext, kExtServerName, se);
        }
        {   // supported_groups
            Bytes groups;
            for (uint16_t g : {kGroupX25519, kGroupSecp256r1}) append_u16(groups, g);
            Bytes gv = make_vec16(groups);
            append_extension(ext, kExtSupportedGroups, gv);
        }
        {   // ec_point_formats (uncompressed)
            Bytes pf; append_u8(pf, 1); append_u8(pf, 0);
            append_extension(ext, kExtEcPointFormats, pf);
        }
        {   // signature_algorithms
            Bytes algs;
            for (uint16_t a : {0x0403, 0x0503, 0x0804, 0x0805, 0x0806, 0x0401, 0x0501}) append_u16(algs, a);
            Bytes av = make_vec16(algs);
            append_extension(ext, kExtSignatureAlgs, av);
            append_extension(ext, kExtSignatureAlgsCert, av);
        }
        {   // supported_versions: one byte length prefix, then the version list
            Bytes versions;
            append_u16(versions, 0x0304);          // TLS 1.3 only
            Bytes v;
            append_u8(v, uint8_t(versions.size()));
            v.insert(v.end(), versions.begin(), versions.end());
            append_extension(ext, kExtSupportedVersions, v);
        }
        {   // key_share
            Bytes share;
            append_u16(share, kGroupX25519);
            Bytes pub = make_vec16(hs.client_pub);
            share.insert(share.end(), pub.begin(), pub.end());
            Bytes sv = make_vec16(share);
            append_extension(ext, kExtKeyShare, sv);
        }
        {   // psk_key_exchange_modes (no PSK offered, but keeps servers happy)
            Bytes m; append_u8(m, 1); append_u8(m, 1);
            append_extension(ext, kExtPskModes, m);
        }
        if (!cfg.alpn.empty()) {
            Bytes list;
            for (const std::string& proto : cfg.alpn) {
                append_u8(list, uint8_t(proto.size()));
                list.insert(list.end(), proto.begin(), proto.end());
            }
            Bytes lv = make_vec16(list);
            append_extension(ext, kExtAlpn, lv);
        }
        Bytes ev = make_vec16(ext);
        body.insert(body.end(), ev.begin(), ev.end());

        Bytes message;
        append_handshake(message, kHsClientHello, body);

        Bytes record;
        append_u8(record, kCtHandshake);
        append_u16(record, 0x0301);
        append_u16(record, uint16_t(message.size()));
        record.insert(record.end(), message.begin(), message.end());
        Error err = transport->write_all(record.data(), record.size(), now_millis() + cfg.timeout_ms);
        if (!err.ok()) return err;
        hs.transcript.insert(hs.transcript.end(), message.begin(), message.end());
    }

    // ---- Read ServerHello (plaintext) -----------------------------------
    auto read_plain_record = [&](uint8_t& type_out, Bytes& payload) -> Error {
        return [&]() -> Error {
            uint8_t header[5];
            Error err = transport->read_exact(header, 5, now_millis() + cfg.timeout_ms);
            if (!err.ok()) return err;
            type_out = header[0];
            uint16_t length = read_u16(header + 3);
            if (length == 0 || length > 16640)
                return LCA_FAIL(Code::TlsError, "bad record length in handshake");
            payload.resize(length);
            return transport->read_exact(payload.data(), length, now_millis() + cfg.timeout_ms);
        }();
    };

    uint8_t rec_type = 0;
    Bytes payload;
    while (true) {
        Error err = read_plain_record(rec_type, payload);
        if (!err.ok()) return err;
        if (rec_type == kCtChangeCipherSpec) continue;
        if (rec_type == kCtAlert && payload.size() >= 2)
            return LCA_FAIL(Code::TlsError, std::string("server alert: ") + alert_name(payload[1]));
        if (rec_type != kCtHandshake)
            return LCA_FAIL(Code::TlsError, "expected ServerHello, got record type " + std::to_string(int(rec_type)));
        break;
    }
    if (payload.size() < 4) return LCA_FAIL(Code::TlsError, "truncated ServerHello");
    uint8_t msg_type = payload[0];
    uint32_t msg_len = read_u24(payload.data() + 1);
    if (msg_type == 6) {
        return LCA_FAIL(Code::Unsupported,
                        "server sent HelloRetryRequest; only X25519 key shares are supported");
    }
    if (msg_type != kHsServerHello) {
        return LCA_FAIL(Code::TlsError, "expected ServerHello, got handshake type " +
                                            std::to_string(int(msg_type)));
    }
    if (payload.size() < 4 + msg_len) return LCA_FAIL(Code::TlsError, "truncated ServerHello body");
    Bytes server_hello(payload.begin() + 4, payload.begin() + 4 + msg_len);
    hs.transcript.insert(hs.transcript.end(), payload.begin(), payload.begin() + 4 + msg_len);

    {
        ByteReader r(server_hello);
        r.u16();                                              // legacy_version
        Bytes server_random = r.take(32);
        hs.server_random = server_random;
        Bytes sid_echo = r.vec8();
        uint16_t suite_id = r.u16();
        r.u8();                                               // compression
        hs.suite = suite_info(suite_id);
        if (!hs.suite) {
            return LCA_FAIL(Code::Unsupported, "server selected unsupported cipher suite 0x" +
                                                   crypto::hex_encode(Bytes{uint8_t(suite_id >> 8),
                                                                            uint8_t(suite_id)}));
        }
        info_.cipher_suite = suite_id;
        info_.cipher_name  = hs.suite->name;
        info_.hash_len     = hs.suite->hash_len;
        info_.group_name   = "X25519";

        Bytes exts = r.vec16();
        ByteReader er(exts);
        bool version_ok = false;
        while (er.remaining() >= 4) {
            uint16_t etype = er.u16();
            Bytes ebody = er.vec16();
            if (etype == kExtSupportedVersions) {
                ByteReader vr(ebody);
                uint16_t v = vr.u16();
                if (v != 0x0304) {
                    return LCA_FAIL(Code::Unsupported,
                                    "server negotiated TLS version 0x" + crypto::hex_encode(Bytes{uint8_t(v >> 8), uint8_t(v)}) +
                                        " (this client speaks TLS 1.3 only)");
                }
                version_ok = true;
            } else if (etype == kExtKeyShare) {
                ByteReader kr(ebody);
                uint16_t group = kr.u16();
                Bytes peer = kr.vec16();
                if (group != kGroupX25519) {
                    return LCA_FAIL(Code::Unsupported,
                                    "server chose key share group " + std::to_string(group) +
                                        "; only X25519 is implemented");
                }
                hs.server_pub = peer;
            } else if (etype == kExtAlpn) {
                ByteReader ar(ebody);
                Bytes list = ar.vec16();
                if (!list.empty()) info_.alpn = std::string(list.begin() + 1, list.end());
            } else if (etype == kExtServerName) {
                // empty acknowledgement
            }
        }
        if (!version_ok) return LCA_FAIL(Code::TlsError, "server did not confirm TLS 1.3");
        if (hs.server_pub.size() != 32) return LCA_FAIL(Code::TlsError, "missing or bad X25519 key share");
    }

    // ---- ECDHE + handshake secrets --------------------------------------
    {
        uint8_t shared[32];
        if (!crypto::x25519(shared, hs.client_priv.data(), hs.server_pub.data())) {
            return LCA_FAIL(Code::TlsError, "X25519 produced a small-order shared secret");
        }
        hs.shared_secret.assign(shared, shared + 32);
        secure_zero(shared);
    }
    {
        Bytes zeros(hs.suite->hash_len, 0);
        Bytes early_secret = crypto::hkdf_extract(hash_kind(hs.suite->hash), zeros, zeros);
        Bytes derived = hkdf_label(hs.suite->hash, early_secret, "derived",
                                   hash_bytes(hs.suite->hash, nullptr, 0), hs.suite->hash_len);
        hs.handshake_secret = crypto::hkdf_extract(hash_kind(hs.suite->hash), derived, hs.shared_secret);
        Bytes th = hs.transcript_hash();
        hs.client_hs_traffic = hkdf_label(hs.suite->hash, hs.handshake_secret, "c hs traffic", th, hs.suite->hash_len);
        hs.server_hs_traffic = hkdf_label(hs.suite->hash, hs.handshake_secret, "s hs traffic", th, hs.suite->hash_len);
    }

    // Install the handshake read keys so subsequent records decrypt.
    auto stream = std::unique_ptr<TlsStream>(new TlsStream(std::move(transport), info_));
    stream->max_plaintext_ = cfg.max_record_plaintext;
    stream->timeout_ms_ = cfg.timeout_ms;
    stream->set_read_keys(derive_keys(hs.suite, hs.server_hs_traffic));

    // ---- Collect EncryptedExtensions, Certificate, CertificateVerify, Finished
    std::vector<X509Cert> presented;
    bool got_server_finished = false;
    bool cert_verified = false;
    std::string cert_error;

    auto process_handshake_message = [&](uint8_t type, const Bytes& body) -> Error {
        switch (type) {
            case kHsEncryptedExtensions: {
                ByteReader r(body);
                Bytes exts = r.vec16();
                ByteReader er(exts);
                while (er.remaining() >= 4) {
                    uint16_t etype = er.u16();
                    Bytes ebody = er.vec16();
                    if (etype == kExtAlpn) {
                        ByteReader ar(ebody);
                        Bytes list = ar.vec16();
                        if (!list.empty()) info_.alpn = std::string(list.begin() + 1, list.end());
                    }
                }
                break;
            }
            case kHsCertificate: {
                ByteReader r(body);
                Bytes context = r.vec8();
                Bytes list = r.vec24();
                ByteReader lr(list);
                while (lr.remaining() > 3) {
                    Bytes der = lr.vec24();
                    if (der.empty()) break;
                    Result<X509Cert> cert = x509_parse(der);
                    if (!cert.ok()) return LCA_FAIL(Code::TlsError, "cannot parse server certificate: " +
                                                                        cert.error().message);
                    presented.push_back(std::move(*cert));
                    // Skip the per-entry extensions vector.
                    if (lr.remaining() >= 2) {
                        size_t save = lr.position();
                        uint16_t ext_len = lr.u16();
                        if (!lr.skip(ext_len)) { lr.seek(save); break; }
                    }
                }
                if (presented.empty()) return LCA_FAIL(Code::TlsError, "server sent an empty certificate list");

                if (cfg.verify_peer) {
                    TrustStore store;
                    bool store_ok = false;
                    if (!cfg.ca_bundle_path.empty()) {
                        Result<size_t> lr2 = store.load_pem_file(cfg.ca_bundle_path);
                        store_ok = lr2.ok();
                        if (!store_ok) cert_error = lr2.error().message;
                    } else {
                        for (const std::string& path : TrustStore::default_bundle_paths()) {
                            Result<size_t> lr2 = store.load_pem_file(path);
                            if (lr2.ok() && *lr2 > 0) { store_ok = true; break; }
                        }
                        if (!store_ok) cert_error = "no CA bundle found on this system";
                    }
                    if (!store_ok) {
                        if (!cfg.allow_untrusted_self_signed) {
                            return LCA_FAIL(Code::TlsError, "cannot verify certificate: " + cert_error);
                        }
                        info_.verified = false;
                        info_.chain_length = presented.size();
                        info_.peer_subject = presented.front().subject_cn;
                        info_.peer_issuer = presented.front().issuer_cn;
                        info_.peer_valid_to = presented.front().not_after_string();
                        info_.peer_key = presented.front().key_name();
                        info_.chain_summary = "WARNING: " + cert_error +
                                              "; peer not verified (" +
                                              std::to_string(presented.size()) +
                                              " certificate(s) presented)";
                    } else {
                        Result<ChainResult> chain = x509_validate_chain(
                            presented, store, cfg.server_name, cfg.allow_untrusted_self_signed);
                        if (!chain.ok()) return Error(chain.error());
                        info_.verified = true;
                        info_.chain_length = chain->chain.size();
                        info_.chain_summary = chain->summary;
                        const X509Cert& leaf = chain->chain.front();
                        info_.peer_subject = leaf.subject_cn;
                        info_.peer_issuer = leaf.issuer_cn;
                        info_.peer_valid_to = leaf.not_after_string();
                        info_.peer_key = leaf.key_name();
                    }
                    cert_verified = true;
                } else {
                    info_.verified = false;
                    info_.chain_length = presented.size();
                    info_.chain_summary = "peer verification disabled by configuration (" +
                                          std::to_string(presented.size()) +
                                          " certificate(s) presented)";
                    info_.peer_subject = presented.front().subject_cn;
                    info_.peer_issuer = presented.front().issuer_cn;
                    info_.peer_valid_to = presented.front().not_after_string();
                    info_.peer_key = presented.front().key_name();
                }
                break;
            }
            case kHsCertificateVerify: {
                if (presented.empty()) return LCA_FAIL(Code::TlsError, "CertificateVerify without Certificate");
                ByteReader r(body);
                uint16_t scheme = r.u16();
                Bytes signature = r.vec16();
                Bytes th = hs.transcript_hash();
                const std::string context = "TLS 1.3, server CertificateVerify";
                Bytes message(64, 0x20);
                message.insert(message.end(), context.begin(), context.end());
                message.push_back(0);
                message.insert(message.end(), th.begin(), th.end());
                std::string detail;
                if (!x509_verify_message(presented.front(), message, signature, scheme, &detail)) {
                    return LCA_FAIL(Code::TlsError,
                                    "server CertificateVerify failed (" +
                                        std::string(signature_scheme_name(scheme)) + "): " + detail);
                }
                LCA_LOG_DEBUG(kScope, "server key proof verified with " +
                                          std::string(signature_scheme_name(scheme)));
                break;
            }
            case kHsFinished: {
                Bytes th = hs.transcript_hash();
                Bytes expected = compute_finished(hs.suite, hs.server_hs_traffic, th);
                if (expected.size() != body.size() ||
                    !ct_equal(expected.data(), body.data(), body.size())) {
                    return LCA_FAIL(Code::TlsError, "server Finished verification failed");
                }
                if (cfg.verify_peer && !cert_verified)
                    return LCA_FAIL(Code::TlsError, "server Finished arrived before certificate validation");
                got_server_finished = true;
                break;
            }
            default:
                LCA_LOG_DEBUG(kScope, "ignoring handshake message type " + std::to_string(int(type)));
                break;
        }
        return {};
    };

    // Handshake messages may be fragmented or coalesced across records.
    Bytes hs_buffer;
    stream->set_write_keys(TlsStream::Keys());   // no write key yet

    while (!got_server_finished) {
        Bytes type, plain;
        Error err = stream->read_record_bytes(type, plain, now_millis() + cfg.timeout_ms);
        if (!err.ok()) return err;
        hs_buffer.insert(hs_buffer.end(), plain.begin(), plain.end());
        while (hs_buffer.size() >= 4) {
            uint8_t mtype = hs_buffer[0];
            uint32_t mlen = read_u24(hs_buffer.data() + 1);
            if (mlen > kMaxHandshakeMessage)
                return LCA_FAIL(Code::TlsError, "handshake message too large");
            if (hs_buffer.size() < 4 + mlen) break;
            Bytes message(hs_buffer.begin(), hs_buffer.begin() + 4 + mlen);
            Bytes body(hs_buffer.begin() + 4, hs_buffer.begin() + 4 + mlen);
            hs_buffer.erase(hs_buffer.begin(), hs_buffer.begin() + 4 + mlen);
            // Process first, then extend the transcript: CertificateVerify and
            // Finished are signed over the transcript *excluding* themselves.
            Error perr = process_handshake_message(mtype, body);
            if (!perr.ok()) return perr;
            hs.transcript.insert(hs.transcript.end(), message.begin(), message.end());
            if (got_server_finished) break;
        }
    }

    // ---- Application secrets --------------------------------------------
    {
        Bytes derived = hkdf_label(hs.suite->hash, hs.handshake_secret, "derived",
                                   hash_bytes(hs.suite->hash, nullptr, 0), hs.suite->hash_len);
        Bytes zeros(hs.suite->hash_len, 0);
        hs.master_secret = crypto::hkdf_extract(hash_kind(hs.suite->hash), derived, zeros);
        Bytes th = hs.transcript_hash();
        hs.client_ap_traffic = hkdf_label(hs.suite->hash, hs.master_secret, "c ap traffic", th, hs.suite->hash_len);
        hs.server_ap_traffic = hkdf_label(hs.suite->hash, hs.master_secret, "s ap traffic", th, hs.suite->hash_len);
    }

    // ---- Client Finished ------------------------------------------------
    {
        Bytes th = hs.transcript_hash();
        Bytes verify = compute_finished(hs.suite, hs.client_hs_traffic, th);
        Bytes message;
        append_handshake(message, kHsFinished, verify);

        // Middlebox compatibility: a dummy CCS before the first encrypted record.
        Bytes ccs;
        append_u8(ccs, kCtChangeCipherSpec);
        append_u16(ccs, 0x0303);
        append_u16(ccs, 1);
        append_u8(ccs, 1);
        Error err = raw_transport->write_all(ccs.data(), ccs.size(), now_millis() + cfg.timeout_ms);
        if (!err.ok()) return err;

        stream->set_write_keys(derive_keys(hs.suite, hs.client_hs_traffic));
        err = stream->write_handshake(message.data(), message.size(), now_millis() + cfg.timeout_ms);
        if (!err.ok()) return err;
        hs.transcript.insert(hs.transcript.end(), message.begin(), message.end());
    }

    // ---- Switch to application traffic keys ------------------------------
    stream->set_read_keys(derive_keys(hs.suite, hs.server_ap_traffic));
    stream->set_write_keys(derive_keys(hs.suite, hs.client_ap_traffic));
    stream->set_handshake_complete();
    info_.version = TlsVersion::Tls13;
    info_.handshake_ms = now_millis() - start;
    // `verified` reports the outcome of chain validation only.  Handshakes that
    // skipped validation keep verified == false so callers can tell the two apart.
    stream->set_info(info_);

    // Wipe intermediate key material.
    secure_zero(hs.shared_secret);
    secure_zero(hs.handshake_secret);
    secure_zero(hs.master_secret);
    secure_zero(hs.client_hs_traffic);
    secure_zero(hs.server_hs_traffic);

    LCA_LOG_DEBUG(kScope, std::string(info_.cipher_name) + " handshake with " + cfg.server_name +
                              " in " + std::to_string(info_.handshake_ms) + "ms; alpn=" +
                              (info_.alpn.empty() ? "(none)" : info_.alpn) + "; " + info_.chain_summary);

    stream_ = std::move(stream);
    return {};
}

}  // namespace lca
