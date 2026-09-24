// =============================================================================
//  lca/crypto.h  --  self-contained cryptography used by the TLS engine
// -----------------------------------------------------------------------------
//  Implemented from the specifications, no libcrypto / libssl / libsodium:
//    * SHA-256, SHA-384, SHA-512            (FIPS 180-4)
//    * HMAC                                 (RFC 2104)
//    * HKDF-Extract / HKDF-Expand-Label     (RFC 5869 / RFC 8446 s7.1)
//    * AES-128/192/256 block cipher + GCM   (FIPS 197 / NIST SP 800-38D)
//    * ChaCha20-Poly1305 AEAD               (RFC 8439)
//    * X25519 scalar multiplication         (RFC 7748)
//    * ECDSA verification over P-256/P-384  (FIPS 186-4) + fixed BigInt
//    * RSASSA-PKCS1-v1_5 and RSASSA-PSS     (RFC 8017)
//    * Base64 / Base64URL / hex, CSPRNG     (getrandom(2) w/ /dev/urandom fallback)
// =============================================================================
#ifndef LCA_CRYPTO_H
#define LCA_CRYPTO_H

#include "lca/common.h"

namespace lca {
namespace crypto {

// -----------------------------------------------------------------------------
// Hash / MAC / KDF
// -----------------------------------------------------------------------------
inline constexpr size_t kSha256Digest = 32;
inline constexpr size_t kSha384Digest = 48;
inline constexpr size_t kSha512Digest = 64;

struct Sha256 {
    Sha256() { init(); }
    void init();
    void update(const void* data, size_t n);
    void update(std::string_view s) { update(s.data(), s.size()); }
    void final(uint8_t out[kSha256Digest]);
    static Bytes one_shot(const void* data, size_t n);
    static Bytes one_shot(std::string_view s) { return one_shot(s.data(), s.size()); }
private:
    uint32_t h_[8];
    uint64_t len_;
    uint8_t  buf_[64];
    size_t   buf_len_;
};

struct Sha384 {
    Sha384() { init(); }
    void init();
    void update(const void* data, size_t n);
    void final(uint8_t out[kSha384Digest]);
    static Bytes one_shot(const void* data, size_t n);
private:
    uint64_t h_[8];
    uint64_t len_;
    uint8_t  buf_[128];
    size_t   buf_len_;
};

struct Sha512 {
    Sha512() { init(); }
    void init();
    void update(const void* data, size_t n);
    void final(uint8_t out[kSha512Digest]);
    static Bytes one_shot(const void* data, size_t n);
private:
    uint64_t h_[8];
    uint64_t len_;
    uint8_t  buf_[128];
    size_t   buf_len_;
};

Bytes hmac_sha256(const Bytes& key, const uint8_t* data, size_t n);
Bytes hmac_sha256(const Bytes& key, const Bytes& data);
Bytes hmac_sha256(const Bytes& key, std::string_view data);
Bytes hmac_sha384(const Bytes& key, const Bytes& data);
Bytes hmac_sha512(const Bytes& key, const Bytes& data);

// Hash-generic helpers (needed by the TLS 1.3 key schedule, which may run on
// either SHA-256 or SHA-384 depending on the negotiated cipher suite).
enum class HashKind { Sha256, Sha384 };
inline size_t hash_len(HashKind k) { return k == HashKind::Sha384 ? kSha384Digest : kSha256Digest; }
Bytes hash_bytes(HashKind kind, const uint8_t* data, size_t n);
inline Bytes hash_bytes(HashKind kind, const Bytes& b) { return hash_bytes(kind, b.data(), b.size()); }
Bytes hmac(HashKind kind, const Bytes& key, const uint8_t* data, size_t n);
inline Bytes hmac(HashKind kind, const Bytes& key, const Bytes& data) {
    return hmac(kind, key, data.data(), data.size());
}
Bytes hkdf_extract(HashKind kind, const Bytes& salt, const Bytes& ikm);
Bytes hkdf_expand(HashKind kind, const Bytes& prk, const Bytes& info, size_t out_len);
Bytes hkdf_expand_label(HashKind kind, const Bytes& secret, std::string_view label,
                        const Bytes& context, size_t out_len);

// HKDF-Extract (returns PRK, which is digest-sized).
Bytes hkdf_extract_sha256(const Bytes& salt, const Bytes& ikm);
// HKDF-Expand (RFC 5869).
Bytes hkdf_expand_sha256(const Bytes& prk, const Bytes& info, size_t out_len);
// TLS 1.3 HKDF-Expand-Label (RFC 8446 7.1).
Bytes hkdf_expand_label(const Bytes& secret, std::string_view label,
                        const Bytes& context, size_t out_len);

// -----------------------------------------------------------------------------
// AEAD
// -----------------------------------------------------------------------------
// AES-128-GCM with a 12 byte nonce.  Output ciphertext is `in` sized; the 16
// byte tag is written to `tag`.
void aes128_gcm_encrypt(const uint8_t key[16], const uint8_t nonce[12],
                        const uint8_t* aad, size_t aad_len,
                        const uint8_t* in, size_t in_len,
                        uint8_t* out, uint8_t tag[16]);
bool aes128_gcm_decrypt(const uint8_t key[16], const uint8_t nonce[12],
                        const uint8_t* aad, size_t aad_len,
                        const uint8_t* in, size_t in_len,
                        const uint8_t tag[16], uint8_t* out);
// Same API for AES-256 (32 byte key) used by several TLS 1.3 servers.
void aes256_gcm_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                        const uint8_t* aad, size_t aad_len,
                        const uint8_t* in, size_t in_len,
                        uint8_t* out, uint8_t tag[16]);
bool aes256_gcm_decrypt(const uint8_t key[32], const uint8_t nonce[12],
                        const uint8_t* aad, size_t aad_len,
                        const uint8_t* in, size_t in_len,
                        const uint8_t tag[16], uint8_t* out);

// Poly1305 one-time MAC (RFC 8439 s2.5), exported for tests and tooling.
void poly1305_mac(const uint8_t key[32], const uint8_t* msg, size_t n, uint8_t tag[16]);

// ChaCha20-Poly1305 AEAD (RFC 8439).
void chacha20_poly1305_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                               const uint8_t* aad, size_t aad_len,
                               const uint8_t* in, size_t in_len,
                               uint8_t* out, uint8_t tag[16]);
bool chacha20_poly1305_decrypt(const uint8_t key[32], const uint8_t nonce[12],
                               const uint8_t* aad, size_t aad_len,
                               const uint8_t* in, size_t in_len,
                               const uint8_t tag[16], uint8_t* out);

// CTAP-less AES-CTR (TLS 1.2 fallback helper, exported for tests).
void aes128_ctr(const uint8_t key[16], const uint8_t iv[16], const uint8_t* in, size_t n, uint8_t* out);

// -----------------------------------------------------------------------------
// X25519
// -----------------------------------------------------------------------------
inline constexpr size_t kX25519KeyLen = 32;
// Generates a private scalar; returns the corresponding public key.
void x25519_keypair(uint8_t priv[kX25519KeyLen], uint8_t pub[kX25519KeyLen]);
// RFC 7748 scalar multiplication.  Returns false for the all-zero output
// (small-order point rejection required by RFC 8446).
bool x25519(uint8_t out[kX25519KeyLen], const uint8_t scalar[kX25519KeyLen], const uint8_t point[kX25519KeyLen]);

// -----------------------------------------------------------------------------
// CSPRNG
// -----------------------------------------------------------------------------
void random_bytes(void* out, size_t n);
inline Bytes random_bytes(size_t n) { Bytes b(n); random_bytes(b.data(), n); return b; }

// -----------------------------------------------------------------------------
// Encodings
// -----------------------------------------------------------------------------
std::string base64_encode(const uint8_t* data, size_t n);
std::string base64_encode(const Bytes& b);
std::string base64url_encode(const Bytes& b);
bool base64_decode(std::string_view in, Bytes& out);
std::string hex_encode(const uint8_t* data, size_t n);
std::string hex_encode(const Bytes& b);
bool hex_decode(std::string_view in, Bytes& out);

// -----------------------------------------------------------------------------
// Fixed-width big integer (unsigned, little-endian 64-bit limbs)
// Enough for RSA moduli up to 8192 bits and all NIST prime curves.
// -----------------------------------------------------------------------------
class BigInt {
public:
    static constexpr size_t kMaxLimbs = 128;   // 128 * 64 = 8192 bits
    BigInt() { std::memset(limbs_, 0, sizeof(limbs_)); }
    explicit BigInt(uint64_t v) : BigInt() { limbs_[0] = v; }

    static BigInt from_bytes_be(const uint8_t* p, size_t n);
    static BigInt from_bytes_le(const uint8_t* p, size_t n);
    Bytes       to_bytes_be(size_t fixed_len = 0) const;

    bool is_zero() const;
    bool is_odd() const { return (limbs_[0] & 1u) != 0; }
    size_t bit_length() const;
    int  compare(const BigInt& o) const;

    bool bit(size_t i) const;
    void set_bit(size_t i);
    void shift_right_one();
    void shift_left_one();
    void shift_left_bits(size_t n);

    static BigInt add(const BigInt& a, const BigInt& b);
    static BigInt sub(const BigInt& a, const BigInt& b);   // assumes a >= b
    static BigInt mul(const BigInt& a, const BigInt& b);
    static void    divmod(const BigInt& a, const BigInt& b, BigInt& q, BigInt& r);
    static BigInt  mod(const BigInt& a, const BigInt& m);
    // Square-and-multiply modular exponentiation.
    static BigInt  mod_exp(const BigInt& base, const BigInt& exp, const BigInt& mod);
    // Modular inverse via the extended Euclidean algorithm.  Returns false when
    // the inverse does not exist.
    static bool    mod_inverse(const BigInt& a, const BigInt& m, BigInt& out);
    // Barrett-free single-limb helpers used by the curve code.
    static uint32_t add_limb(BigInt& a, uint64_t v);

    uint64_t limb(size_t i) const { return i < kMaxLimbs ? limbs_[i] : 0; }
    void     set_limb(size_t i, uint64_t v) { if (i < kMaxLimbs) limbs_[i] = v; }
    size_t   limb_count() const;
private:
    uint64_t limbs_[kMaxLimbs];
};

// -----------------------------------------------------------------------------
// ECDSA over NIST P-256 / P-384
// -----------------------------------------------------------------------------
enum class Curve { P256, P384 };

// Verifies `sig_der` (ASN.1 DER ECDSA-Sig-Value) over `digest` (already hashed,
// 32 or 48 bytes) with the given uncompressed point.
bool ecdsa_verify(Curve curve, const Bytes& pub_x, const Bytes& pub_y,
                  const Bytes& digest, const Bytes& sig_der);

// -----------------------------------------------------------------------------
// RSA signature verification
// -----------------------------------------------------------------------------
enum class RsaPadding { Pkcs1v15, Pss };

// `modulus`/`exponent` are big-endian byte strings taken from the certificate.
// `digest` is the raw hash of the signed data; `hash_oid` selects the DigestInfo
// prefix for PKCS#1 v1.5.
bool rsa_verify(RsaPadding padding, const Bytes& modulus, const Bytes& exponent,
                const Bytes& digest, const Bytes& sig, size_t hash_len);

}  // namespace crypto
}  // namespace lca

#endif  // LCA_CRYPTO_H
