// =============================================================================
//  lca/x509.h  --  DER/ASN.1 decoding and certificate path validation
// -----------------------------------------------------------------------------
//  Implements just enough of X.509 (RFC 5280) for TLS server authentication:
//  certificate decoding, SPKI extraction, hostname matching against SAN/CN,
//  validity windows, basic constraints, and signature chaining up to a trust
//  anchor loaded from a PEM bundle.
// =============================================================================
#ifndef LCA_X509_H
#define LCA_X509_H

#include "lca/buf.h"
#include "lca/crypto.h"

namespace lca {

// -----------------------------------------------------------------------------
// Minimal DER reader
// -----------------------------------------------------------------------------
struct DerElement {
    uint8_t     tag{0};
    std::string value;      // element body (no tag/length)
    std::string raw;        // complete TLV bytes (tag + length + body)
    size_t      total{0};   // tag + length + body
    bool ok{false};
};

namespace der {
DerElement next(ByteReader& r);
DerElement peek(ByteReader& r);
std::vector<DerElement> children(std::string_view body);
std::string oid_to_string(std::string_view body);
std::string decode_string(uint8_t tag, std::string_view body);   // UTF8String/PrintableString/...
bool        decode_integer(std::string_view body, Bytes& out_be);
bool        decode_time(uint8_t tag, std::string_view body, int64_t& unix_seconds);
}  // namespace der

// -----------------------------------------------------------------------------
// Certificate
// -----------------------------------------------------------------------------
enum class KeyType { Unknown, EcdsaP256, EcdsaP384, Rsa2048, Rsa, Ed25519 };

struct X509Cert {
    Bytes  der;
    Bytes  tbs;                 // raw bytes of tbsCertificate (what gets signed)
    int    version{1};
    Bytes  serial;
    std::string sig_oid;        // signatureAlgorithm OID
    Bytes  issuer_raw;          // DER of issuer Name
    Bytes  subject_raw;         // DER of subject Name
    std::string subject_cn;
    std::string issuer_cn;
    int64_t not_before{0};
    int64_t not_after{0};

    KeyType key_type{KeyType::Unknown};
    size_t  key_bits{0};
    Bytes   rsa_modulus, rsa_exponent;
    Bytes   ec_x, ec_y;         // P-256 / P-384 affine coordinates (big endian)

    std::vector<std::string> san_dns;
    std::vector<std::string> san_ip;
    bool is_ca{false};
    int  path_len{-1};

    Bytes signature;            // BIT STRING contents

    bool valid_at(int64_t unix_time) const { return unix_time >= not_before && unix_time <= not_after; }
    bool matches_hostname(const std::string& host) const;
    std::string key_name() const;
    std::string not_after_string() const;
};

Result<X509Cert> x509_parse(const Bytes& der);
Result<X509Cert> x509_parse_pem(std::string_view pem);

// -----------------------------------------------------------------------------
// Trust anchors
// -----------------------------------------------------------------------------
class TrustStore {
public:
    // Loads every CERTIFICATE block from a PEM bundle.  Returns the number of
    // anchors loaded.
    Result<size_t> load_pem_file(const std::string& path);
    Result<size_t> load_pem_string(std::string_view pem);
    void add_der(const Bytes& der);

    // Looks up an anchor whose subject DER equals `issuer_raw`.
    const X509Cert* find_issuer(const Bytes& issuer_raw) const;
    size_t size() const { return anchors_.size(); }
    // The trust bundles this build looks for by default, in priority order.
    static std::vector<std::string> default_bundle_paths();

private:
    std::vector<X509Cert> anchors_;
};

// -----------------------------------------------------------------------------
// Verification
// -----------------------------------------------------------------------------
// Verifies `cert.signature` over `cert.tbs` using `issuer`'s public key and the
// digest implied by cert.sig_oid.  `cert.sig_oid`/`issuer` must be consistent.
// `digest_out` optionally receives the message digest that was verified.
bool x509_verify_signature(const X509Cert& cert, const X509Cert& issuer,
                           std::string* detail = nullptr);

// Verifies a signature over an arbitrary message (used by TLS CertificateVerify).
bool x509_verify_message(const X509Cert& cert, const Bytes& message, const Bytes& signature,
                         uint16_t signature_scheme, std::string* detail = nullptr);
const char* signature_scheme_name(uint16_t scheme);

struct ChainResult {
    std::vector<X509Cert> chain;   // leaf first, anchor last
    std::string summary;
};

// Builds and validates a chain from the presented certificates up to a trust
// anchor, checking signatures, validity, key usage and the server hostname.
Result<ChainResult> x509_validate_chain(const std::vector<X509Cert>& presented,
                                        const TrustStore& store,
                                        const std::string& hostname,
                                        bool allow_untrusted_self_signed = false);

int64_t x509_time_now();   // seconds since epoch

}  // namespace lca

#endif  // LCA_X509_H
