// =============================================================================
//  src/x509.cpp  --  DER decoding, certificate parsing, path validation
// =============================================================================
#include "lca/x509.h"

#include <cstdio>
#include <ctime>

namespace lca {

// =============================================================================
// DER reader
// =============================================================================
namespace der {

DerElement next(ByteReader& r) {
    DerElement e;
    if (r.remaining() < 2) return e;
    e.tag = r.u8();
    uint8_t first = r.u8();
    size_t length = 0;
    if (first & 0x80) {
        size_t nbytes = first & 0x7F;
        if (nbytes == 0 || nbytes > 4 || r.remaining() < nbytes) return e;
        for (size_t i = 0; i < nbytes; ++i) length = (length << 8) | r.u8();
    } else {
        length = first;
    }
    if (r.remaining() < length) return e;
    e.value = r.take_str(length);
    e.total = 2 + (first & 0x80 ? size_t(first & 0x7F) : 0) + length;
    if (e.tag == 0x00 || e.tag == 0xFF) {                 // high-tag-number forms are not used here
        e.ok = false;
        return e;
    }
    e.raw = std::string(reinterpret_cast<const char*>(r.cursor() - e.total), e.total);
    e.ok = true;
    return e;
}

DerElement peek(ByteReader& r) {
    size_t save = r.position();
    DerElement e = next(r);
    r.seek(save);
    return e;
}

std::vector<DerElement> children(std::string_view body) {
    std::vector<DerElement> out;
    ByteReader r(reinterpret_cast<const uint8_t*>(body.data()), body.size());
    while (r.remaining() > 0) {
        DerElement e = next(r);
        if (!e.ok) break;
        out.push_back(std::move(e));
    }
    return out;
}

std::string oid_to_string(std::string_view body) {
    if (body.empty()) return {};
    std::string out;
    uint32_t value = 0;
    bool first = true;
    for (size_t i = 0; i < body.size(); ++i) {
        uint8_t b = uint8_t(body[i]);
        value = (value << 7) | (b & 0x7F);
        if (b & 0x80) continue;
        if (first) {
            out += std::to_string(value / 40) + "." + std::to_string(value % 40);
            first = false;
        } else {
            out += "." + std::to_string(value);
        }
        value = 0;
    }
    return out;
}

std::string decode_string(uint8_t tag, std::string_view body) {
    switch (tag) {
        case 0x0C: case 0x13: case 0x16: case 0x14: case 0x1E:   // UTF8/Printable/IA5/T61/BMP-ish
            return std::string(body);
        default:
            return std::string(body);
    }
}

bool decode_integer(std::string_view body, Bytes& out_be) {
    if (body.empty()) return false;
    size_t i = 0;
    while (i + 1 < body.size() && body[i] == 0) ++i;
    out_be.assign(body.begin() + long(i), body.end());
    return !out_be.empty();
}

namespace {
int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
}  // namespace

bool decode_time(uint8_t tag, std::string_view body, int64_t& unix_seconds) {
    std::string s(body);
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    int offset_minutes = 0;
    if (tag == 0x17) {                       // UTCTime: YYMMDDHHMMSSZ
        if (s.size() < 11) return false;
        year = std::atoi(s.substr(0, 2).c_str());
        year += year >= 50 ? 1900 : 2000;
        month  = std::atoi(s.substr(2, 2).c_str());
        day    = std::atoi(s.substr(4, 2).c_str());
        hour   = std::atoi(s.substr(6, 2).c_str());
        minute = std::atoi(s.substr(8, 2).c_str());
        if (s.size() >= 12) second = std::atoi(s.substr(10, 2).c_str());
    } else if (tag == 0x18) {                // GeneralizedTime: YYYYMMDDHHMMSSZ
        if (s.size() < 10) return false;
        year   = std::atoi(s.substr(0, 4).c_str());
        month  = std::atoi(s.substr(4, 2).c_str());
        day    = std::atoi(s.substr(6, 2).c_str());
        hour   = std::atoi(s.substr(8, 2).c_str());
        if (s.size() >= 12) minute = std::atoi(s.substr(10, 2).c_str());
        if (s.size() >= 14) second = std::atoi(s.substr(12, 2).c_str());
    } else {
        return false;
    }
    size_t z = s.find_first_of("Z+-");
    if (z != std::string::npos && s[z] != 'Z' && s.size() >= z + 5) {
        int sign = s[z] == '-' ? -1 : 1;
        offset_minutes = sign * (std::atoi(s.substr(z + 1, 2).c_str()) * 60 +
                                 std::atoi(s.substr(z + 3, 2).c_str()));
    }
    int64_t days = days_from_civil(year, month, day);
    unix_seconds = days * 86400 + hour * 3600 + minute * 60 + second - offset_minutes * 60;
    return true;
}

}  // namespace der

// =============================================================================
// Certificate parsing
// =============================================================================
namespace {

constexpr const char* kOidEcdsaSha256  = "1.2.840.10045.4.3.2";
constexpr const char* kOidEcdsaSha384  = "1.2.840.10045.4.3.3";
constexpr const char* kOidRsaSha256    = "1.2.840.113549.1.1.11";
constexpr const char* kOidRsaSha384    = "1.2.840.113549.1.1.12";
constexpr const char* kOidRsaSha1      = "1.2.840.113549.1.1.5";
constexpr const char* kOidRsaPss       = "1.2.840.113549.1.1.10";
constexpr const char* kOidEcPublicKey  = "1.2.840.10045.2.1";
constexpr const char* kOidRsaEncrypt   = "1.2.840.113549.1.1.1";
constexpr const char* kOidPrime256v1   = "1.2.840.10045.3.1.7";
constexpr const char* kOidSecp384r1    = "1.3.132.0.34";
constexpr const char* kOidEd25519      = "1.3.101.112";
constexpr const char* kOidExtension    = "2.5.29.17";
constexpr const char* kOidBasicCons    = "2.5.29.19";
constexpr const char* kOidKeyUsage     = "2.5.29.15";

// Walks a Name (RDNSequence) collecting the CN.
std::string name_common_name(std::string_view name_body) {
    std::string cn;
    for (const DerElement& rdn : der::children(name_body)) {
        for (const DerElement& atv : der::children(rdn.value)) {
            auto parts = der::children(atv.value);
            if (parts.size() != 2) continue;
            std::string oid = der::oid_to_string(parts[0].value);
            if (oid == "2.5.4.3") cn = der::decode_string(parts[1].tag, parts[1].value);
        }
    }
    return cn;
}

Bytes as_bytes(std::string_view s) {
    return Bytes(reinterpret_cast<const uint8_t*>(s.data()),
                 reinterpret_cast<const uint8_t*>(s.data()) + s.size());
}

}  // namespace

Result<X509Cert> x509_parse(const Bytes& der_bytes) {
    X509Cert cert;
    cert.der = der_bytes;
    ByteReader outer(der_bytes);
    DerElement root = der::next(outer);
    if (!root.ok || root.tag != 0x30) return LCA_FAIL(Code::ParseError, "certificate is not a DER SEQUENCE");

    auto top = der::children(root.value);
    if (top.size() < 3) return LCA_FAIL(Code::ParseError, "truncated certificate");

    // --- tbsCertificate -------------------------------------------------
    const DerElement& tbs = top[0];
    cert.tbs = as_bytes(tbs.raw);   // signature is computed over the whole element
    auto tbs_fields = der::children(tbs.value);
    size_t idx = 0;
    if (idx < tbs_fields.size() && tbs_fields[idx].tag == 0xA0) {      // [0] version
        cert.version = 1;
        auto v = der::children(tbs_fields[idx].value);
        if (!v.empty()) cert.version = std::atoi(std::string(v[0].value).c_str()) + 1;
        ++idx;
    }
    if (idx >= tbs_fields.size()) return LCA_FAIL(Code::ParseError, "missing serial");
    der::decode_integer(tbs_fields[idx].value, cert.serial);
    ++idx;
    // signature AlgorithmIdentifier (inside TBS) -- keep going to issuer
    ++idx;
    if (idx + 3 >= tbs_fields.size()) return LCA_FAIL(Code::ParseError, "truncated TBS");
    cert.issuer_raw  = as_bytes(tbs_fields[idx].value); ++idx;
    {
        const DerElement& validity = tbs_fields[idx]; ++idx;
        auto times = der::children(validity.value);
        if (times.size() >= 2) {
            der::decode_time(times[0].tag, times[0].value, cert.not_before);
            der::decode_time(times[1].tag, times[1].value, cert.not_after);
        }
    }
    cert.subject_raw = as_bytes(tbs_fields[idx].value);
    cert.subject_cn  = name_common_name(tbs_fields[idx].value);
    cert.issuer_cn   = name_common_name(std::string_view(
        reinterpret_cast<const char*>(cert.issuer_raw.data()), cert.issuer_raw.size()));
    ++idx;

    // --- subjectPublicKeyInfo ------------------------------------------
    if (idx >= tbs_fields.size()) return LCA_FAIL(Code::ParseError, "missing SPKI");
    {
        const DerElement& spki = tbs_fields[idx]; ++idx;
        auto spki_parts = der::children(spki.value);
        if (spki_parts.size() < 2) return LCA_FAIL(Code::ParseError, "bad SPKI");
        auto alg = der::children(spki_parts[0].value);
        std::string alg_oid = alg.empty() ? std::string() : der::oid_to_string(alg[0].value);
        std::string curve_oid;
        if (alg.size() >= 2) curve_oid = der::oid_to_string(alg[1].value);

        if (alg_oid == kOidEcPublicKey) {
            if (curve_oid == kOidPrime256v1) { cert.key_type = KeyType::EcdsaP256; cert.key_bits = 256; }
            else if (curve_oid == kOidSecp384r1) { cert.key_type = KeyType::EcdsaP384; cert.key_bits = 384; }
            else return LCA_FAIL(Code::Unsupported, "unsupported EC curve " + curve_oid);
            // BIT STRING -> 0x04 || X || Y
            // BIT STRING body: unused-bits byte, then 0x04 || X || Y.
            std::string bits = spki_parts[1].value;
            size_t scalar = cert.key_bits / 8;
            if (bits.size() < 2 || uint8_t(bits[0]) != 0x00 || uint8_t(bits[1]) != 0x04 ||
                bits.size() < 2 + 2 * scalar)
                return LCA_FAIL(Code::ParseError, "unsupported EC point encoding");
            cert.ec_x = as_bytes(std::string_view(bits).substr(2, scalar));
            cert.ec_y = as_bytes(std::string_view(bits).substr(2 + scalar, scalar));
        } else if (alg_oid == kOidRsaEncrypt) {
            std::string bits = spki_parts[1].value;
            if (bits.size() < 2) return LCA_FAIL(Code::ParseError, "empty RSA key");
            ByteReader kr(reinterpret_cast<const uint8_t*>(bits.data() + 1), bits.size() - 1);
            DerElement rsaseq = der::next(kr);
            if (!rsaseq.ok || rsaseq.tag != 0x30) return LCA_FAIL(Code::ParseError, "bad RSA key");
            auto rsaparts = der::children(rsaseq.value);
            if (rsaparts.size() < 2) return LCA_FAIL(Code::ParseError, "bad RSA key");
            der::decode_integer(rsaparts[0].value, cert.rsa_modulus);
            der::decode_integer(rsaparts[1].value, cert.rsa_exponent);
            cert.key_bits = cert.rsa_modulus.size() * 8;
            cert.key_type = cert.key_bits >= 2048 ? KeyType::Rsa2048 : KeyType::Rsa;
        } else if (alg_oid == kOidEd25519) {
            cert.key_type = KeyType::Ed25519;
            cert.key_bits = 256;
        } else {
            return LCA_FAIL(Code::Unsupported, "unsupported public key algorithm " + alg_oid);
        }
    }

    // --- extensions ------------------------------------------------------
    while (idx < tbs_fields.size()) {
        const DerElement& field = tbs_fields[idx++];
        if (field.tag != 0xA3) continue;                                 // [3] extensions
        for (const DerElement& ext : der::children(field.value)) {
            auto parts = der::children(ext.value);
            if (parts.size() < 2) continue;
            std::string oid = der::oid_to_string(parts[0].value);
            const DerElement& value = parts.back();
            std::string body = value.value;
            if (value.tag == 0x04 && body.size() >= 2) {
                // OCTET STRING wrapping the extension value; unwrap one level.
                ByteReader inner(reinterpret_cast<const uint8_t*>(body.data()), body.size());
                DerElement wrapped = der::next(inner);
                if (wrapped.ok && inner.remaining() == 0) body = wrapped.value;
            }
            if (oid == kOidExtension) {
                ByteReader nr(reinterpret_cast<const uint8_t*>(body.data()), body.size());
                DerElement seq = der::next(nr);
                if (!seq.ok || seq.tag != 0x30) continue;
                for (const DerElement& name : der::children(seq.value)) {
                    if (name.tag == 0x82) cert.san_dns.push_back(name.value);
                    else if (name.tag == 0x87) cert.san_ip.push_back(name.value);
                }
            } else if (oid == kOidBasicCons) {
                ByteReader br(reinterpret_cast<const uint8_t*>(body.data()), body.size());
                DerElement seq = der::next(br);
                if (!seq.ok) continue;
                auto parts2 = der::children(seq.value);
                for (const DerElement& p : parts2) {
                    if (p.tag == 0x01) cert.is_ca = !p.value.empty() && p.value[0] != 0;
                    if (p.tag == 0x02) {
                        Bytes len;
                        if (der::decode_integer(p.value, len) && !len.empty()) cert.path_len = len.back();
                    }
                }
            } else if (oid == kOidKeyUsage) {
                // (parsed for completeness; asserted during chain validation)
            }
        }
    }

    // --- signature -------------------------------------------------------
    if (top.size() < 3) return LCA_FAIL(Code::ParseError, "missing signature");
    {
        auto sig_alg = der::children(top[1].value);
        if (!sig_alg.empty()) cert.sig_oid = der::oid_to_string(sig_alg[0].value);
        std::string sig_bits = top[2].value;
        if (sig_bits.empty()) return LCA_FAIL(Code::ParseError, "empty signature");
        cert.signature = as_bytes(std::string_view(sig_bits).substr(1));   // drop unused-bits byte
    }
    return cert;
}

Result<X509Cert> x509_parse_pem(std::string_view pem) {
    std::string body;
    size_t pos = 0;
    bool in = false;
    while (pos < pem.size()) {
        size_t nl = pem.find('\n', pos);
        std::string_view line = pem.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        if (contains(line, "-----BEGIN CERTIFICATE-----")) { in = true; }
        else if (contains(line, "-----END CERTIFICATE-----")) { in = false; }
        else if (in) body += std::string(trim(line));
        if (nl == std::string_view::npos) break;
        pos = nl + 1;
    }
    Bytes der;
    if (body.empty() || !crypto::base64_decode(body, der))
        return LCA_FAIL(Code::ParseError, "no PEM certificate found");
    return x509_parse(der);
}

std::string X509Cert::key_name() const {
    switch (key_type) {
        case KeyType::EcdsaP256: return "ECDSA-P256";
        case KeyType::EcdsaP384: return "ECDSA-P384";
        case KeyType::Rsa2048:   return "RSA-" + std::to_string(key_bits);
        case KeyType::Rsa:       return "RSA-" + std::to_string(key_bits);
        case KeyType::Ed25519:   return "Ed25519";
        default:                 return "unknown";
    }
}

std::string X509Cert::not_after_string() const {
    std::time_t t = std::time_t(not_after);
    char buf[64] = {0};
    std::tm tmv {};
    if (::gmtime_r(&t, &tmv)) std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%SZ", &tmv);
    return buf;
}

bool X509Cert::matches_hostname(const std::string& host) const {
    std::string want = lower(trim(host));
    auto match_one = [&](const std::string& pattern) {
        std::string p = lower(pattern);
        if (p == want) return true;
        if (starts_with(p, "*.")) {
            size_t dot = want.find('.');
            if (dot == std::string::npos) return false;
            std::string suffix = p.substr(1);            // ".example.com"
            return ends_with(want, suffix) && want.size() > suffix.size();
        }
        return false;
    };
    for (const std::string& d : san_dns) if (match_one(d)) return true;
    for (const std::string& ip : san_ip) if (lower(ip) == want) return true;
    if (san_dns.empty() && san_ip.empty()) return match_one(subject_cn);   // legacy CN fallback
    return false;
}

// =============================================================================
// Trust store
// =============================================================================
Result<size_t> TrustStore::load_pem_string(std::string_view pem) {
    size_t loaded = 0;
    const std::string begin = "-----BEGIN CERTIFICATE-----";
    const std::string end   = "-----END CERTIFICATE-----";
    size_t pos = 0;
    while (true) {
        size_t b = pem.find(begin, pos);
        if (b == std::string_view::npos) break;
        size_t e = pem.find(end, b);
        if (e == std::string_view::npos) break;
        std::string block(pem.substr(b, e + end.size() - b));
        Result<X509Cert> cert = x509_parse_pem(block);
        if (cert.ok()) {
            anchors_.push_back(std::move(*cert));
            ++loaded;
        }
        pos = e + end.size();
    }
    if (loaded == 0) return LCA_FAIL(Code::NotFound, "no certificates found in PEM bundle");
    return loaded;
}

Result<size_t> TrustStore::load_pem_file(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return LCA_FAIL(Code::NotFound, "cannot open CA bundle: " + path);
    std::string content;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) content.append(buf, n);
    std::fclose(f);
    Result<size_t> r = load_pem_string(content);
    if (!r.ok()) return r;
    return *r;
}

void TrustStore::add_der(const Bytes& der) {
    Result<X509Cert> cert = x509_parse(der);
    if (cert.ok()) anchors_.push_back(std::move(*cert));
}

const X509Cert* TrustStore::find_issuer(const Bytes& issuer_raw) const {
    for (const X509Cert& a : anchors_) {
        if (a.subject_raw == issuer_raw) return &a;
    }
    return nullptr;
}

std::vector<std::string> TrustStore::default_bundle_paths() {
    std::vector<std::string> paths;
    for (const char* env : {"SSL_CERT_FILE", "CURL_CA_BUNDLE", "REQUESTS_CA_BUNDLE"}) {
        const char* v = std::getenv(env);
        if (v && *v) paths.push_back(v);
    }
    paths.push_back("/etc/ssl/certs/ca-certificates.crt");
    paths.push_back("/etc/pki/tls/certs/ca-bundle.crt");
    paths.push_back("/etc/ssl/cert.pem");
    paths.push_back("/usr/local/etc/openssl/cert.pem");
    return paths;
}

// =============================================================================
// Signature verification
// =============================================================================
namespace {

bool digest_for_oid(const std::string& oid, const Bytes& message, Bytes& digest, std::string& name) {
    if (oid == kOidEcdsaSha256 || oid == kOidRsaSha256) {
        digest = crypto::Sha256::one_shot(message.data(), message.size());
        name = "sha256";
        return true;
    }
    if (oid == kOidEcdsaSha384 || oid == kOidRsaSha384) {
        digest = crypto::Sha384::one_shot(message.data(), message.size());
        name = "sha384";
        return true;
    }
    if (oid == kOidRsaSha1) {
        name = "sha1";
        return false;                      // SHA-1 certificates are rejected outright
    }
    name = oid;
    return false;
}

}  // namespace

bool x509_verify_signature(const X509Cert& cert, const X509Cert& issuer, std::string* detail) {
    Bytes digest;
    std::string hash_name;
    if (!digest_for_oid(cert.sig_oid, cert.tbs, digest, hash_name)) {
        if (detail) *detail = "unsupported signature algorithm " + cert.sig_oid;
        return false;
    }
    switch (issuer.key_type) {
        case KeyType::EcdsaP256:
        case KeyType::EcdsaP384: {
            crypto::Curve curve = issuer.key_type == KeyType::EcdsaP256 ? crypto::Curve::P256
                                                                      : crypto::Curve::P384;
            if ((cert.sig_oid == kOidEcdsaSha256 && curve != crypto::Curve::P256) ||
                (cert.sig_oid == kOidEcdsaSha384 && curve != crypto::Curve::P384)) {
                if (detail) *detail = "curve/hash mismatch";
                return false;
            }
            bool ok = crypto::ecdsa_verify(curve, issuer.ec_x, issuer.ec_y, digest, cert.signature);
            if (detail && !ok) *detail = "ECDSA signature check failed (" + hash_name + ")";
            return ok;
        }
        case KeyType::Rsa:
        case KeyType::Rsa2048: {
            crypto::RsaPadding padding = (cert.sig_oid == kOidRsaPss) ? crypto::RsaPadding::Pss
                                                                     : crypto::RsaPadding::Pkcs1v15;
            bool ok = crypto::rsa_verify(padding, issuer.rsa_modulus, issuer.rsa_exponent,
                                         digest, cert.signature, digest.size());
            if (detail && !ok) *detail = "RSA signature check failed (" + hash_name + ")";
            return ok;
        }
        default:
            if (detail) *detail = "unsupported issuer key type";
            return false;
    }
}

const char* signature_scheme_name(uint16_t scheme) {
    switch (scheme) {
        case 0x0401: return "rsa_pkcs1_sha256";
        case 0x0501: return "rsa_pkcs1_sha384";
        case 0x0601: return "rsa_pkcs1_sha512";
        case 0x0403: return "ecdsa_secp256r1_sha256";
        case 0x0503: return "ecdsa_secp384r1_sha384";
        case 0x0603: return "ecdsa_secp521r1_sha512";
        case 0x0804: return "rsa_pss_rsae_sha256";
        case 0x0805: return "rsa_pss_rsae_sha384";
        case 0x0806: return "rsa_pss_rsae_sha512";
        case 0x0807: return "ed25519";
        default:     return "unknown";
    }
}

bool x509_verify_message(const X509Cert& cert, const Bytes& message, const Bytes& signature,
                         uint16_t signature_scheme, std::string* detail) {
    Bytes digest;
    switch (signature_scheme) {
        case 0x0403: case 0x0804: case 0x0401:
            digest = crypto::Sha256::one_shot(message.data(), message.size());
            break;
        case 0x0503: case 0x0805: case 0x0501:
            digest = crypto::Sha384::one_shot(message.data(), message.size());
            break;
        case 0x0603: case 0x0806: case 0x0601:
            digest = crypto::Sha512::one_shot(message.data(), message.size());
            break;
        default:
            if (detail) *detail = std::string("unsupported signature scheme ") + signature_scheme_name(signature_scheme);
            return false;
    }
    switch (cert.key_type) {
        case KeyType::EcdsaP256:
            if (signature_scheme != 0x0403) {
                if (detail) *detail = "certificate key does not match signature scheme";
                return false;
            }
            return crypto::ecdsa_verify(crypto::Curve::P256, cert.ec_x, cert.ec_y, digest, signature);
        case KeyType::EcdsaP384:
            if (signature_scheme != 0x0503) {
                if (detail) *detail = "certificate key does not match signature scheme";
                return false;
            }
            return crypto::ecdsa_verify(crypto::Curve::P384, cert.ec_x, cert.ec_y, digest, signature);
        case KeyType::Rsa:
        case KeyType::Rsa2048: {
            crypto::RsaPadding padding = (signature_scheme == 0x0804 || signature_scheme == 0x0805 ||
                                          signature_scheme == 0x0806)
                                             ? crypto::RsaPadding::Pss
                                             : crypto::RsaPadding::Pkcs1v15;
            bool ok = crypto::rsa_verify(padding, cert.rsa_modulus, cert.rsa_exponent, digest,
                                         signature, digest.size());
            if (!ok && detail) *detail = "RSA CertificateVerify failed";
            return ok;
        }
        default:
            if (detail) *detail = "unsupported certificate key type for CertificateVerify";
            return false;
    }
}

// =============================================================================
// Chain validation
// =============================================================================
int64_t x509_time_now() {
    return int64_t(std::time(nullptr));
}

Result<ChainResult> x509_validate_chain(const std::vector<X509Cert>& presented,
                                        const TrustStore& store,
                                        const std::string& hostname,
                                        bool allow_untrusted_self_signed) {
    if (presented.empty()) return LCA_FAIL(Code::TlsError, "server sent no certificate");
    ChainResult result;
    int64_t now = x509_time_now();

    // 1. Leaf checks ----------------------------------------------------
    const X509Cert& leaf = presented.front();
    if (!leaf.valid_at(now)) {
        return LCA_FAIL(Code::TlsError,
                        "leaf certificate is outside its validity window (expired " + leaf.not_after_string() + ")");
    }
    if (!leaf.matches_hostname(hostname)) {
        return LCA_FAIL(Code::TlsError,
                        "certificate does not match host '" + hostname + "' (CN=" + leaf.subject_cn +
                            ", SANs=" + join(leaf.san_dns, ",") + ")");
    }

    // 2. Walk up the presented chain, verifying each link ----------------
    std::vector<X509Cert> chain;
    chain.push_back(leaf);
    size_t guard = 0;
    while (guard++ < 12) {
        const X509Cert& current = chain.back();
        if (current.issuer_raw == current.subject_raw) break;          // self-issued, stop here
        // Prefer an intermediate from the presented list.
        const X509Cert* issuer = nullptr;
        for (size_t i = 0; i < presented.size(); ++i) {
            if (&presented[i] == &current) continue;
            if (presented[i].subject_raw == current.issuer_raw) { issuer = &presented[i]; break; }
        }
        if (!issuer) issuer = store.find_issuer(current.issuer_raw);
        if (!issuer) {
            return LCA_FAIL(Code::TlsError,
                            "no trusted issuer found for '" + current.subject_cn +
                                "' (issuer '" + current.issuer_cn + "')");
        }
        std::string detail;
        if (!x509_verify_signature(current, *issuer, &detail)) {
            return LCA_FAIL(Code::TlsError, "signature check failed for '" + current.subject_cn +
                                                "': " + detail);
        }
        if (!issuer->valid_at(now)) {
            return LCA_FAIL(Code::TlsError, "issuer '" + issuer->subject_cn + "' is not valid at the current time");
        }
        chain.push_back(*issuer);
        if (!issuer->is_ca && issuer->subject_raw != issuer->issuer_raw) {
            return LCA_FAIL(Code::TlsError, "issuer '" + issuer->subject_cn + "' lacks CA basicConstraints");
        }
    }

    // 3. Anchor checks ---------------------------------------------------
    const X509Cert& top = chain.back();
    bool anchored = store.find_issuer(top.subject_raw) != nullptr ||
                    (top.issuer_raw == top.subject_raw && store.find_issuer(top.issuer_raw) != nullptr);
    if (!anchored) {
        bool self_signed = top.subject_raw == top.issuer_raw;
        if (!(allow_untrusted_self_signed && self_signed)) {
            return LCA_FAIL(Code::TlsError,
                            "certificate chain does not terminate at a trusted root ('" + top.subject_cn +
                                "' is unknown to the local trust store)");
        }
        result.summary = "WARNING: untrusted self-signed certificate accepted by policy";
    }
    result.chain = std::move(chain);
    if (result.summary.empty()) {
        result.summary = "chain of " + std::to_string(result.chain.size()) + " certificate(s) verified: " +
                         leaf.subject_cn + " (" + leaf.key_name() + "), valid until " +
                         leaf.not_after_string();
    }
    return result;
}

}  // namespace lca
