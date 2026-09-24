// =============================================================================
//  src/crypto.cpp  --  symmetric primitives, X25519, CSPRNG, encodings
// =============================================================================
#include "lca/crypto.h"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <sys/random.h>
#include <sys/time.h>

namespace lca {
namespace crypto {

// =============================================================================
// SHA-256  (FIPS 180-4)
// =============================================================================
namespace {

inline uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
inline uint64_t rotr64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

const uint32_t kSha256K[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};

void sha256_compress(uint32_t h[8], const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t(block[i*4]) << 24) | (uint32_t(block[i*4+1]) << 16) |
               (uint32_t(block[i*4+2]) << 8) | uint32_t(block[i*4+3]);
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr32(w[i-15], 7) ^ rotr32(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = rotr32(w[i-2], 17) ^ rotr32(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = rotr32(e,6) ^ rotr32(e,11) ^ rotr32(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + kSha256K[i] + w[i];
        uint32_t S0 = rotr32(a,2) ^ rotr32(a,13) ^ rotr32(a,22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
}

// SHA-512 family constants
const uint64_t kSha512K[80] = {
    0x428a2f98d728ae22ull,0x7137449123ef65cdull,0xb5c0fbcfec4d3b2full,0xe9b5dba58189dbbcull,
    0x3956c25bf348b538ull,0x59f111f1b605d019ull,0x923f82a4af194f9bull,0xab1c5ed5da6d8118ull,
    0xd807aa98a3030242ull,0x12835b0145706fbeull,0x243185be4ee4b28cull,0x550c7dc3d5ffb4e2ull,
    0x72be5d74f27b896full,0x80deb1fe3b1696b1ull,0x9bdc06a725c71235ull,0xc19bf174cf692694ull,
    0xe49b69c19ef14ad2ull,0xefbe4786384f25e3ull,0x0fc19dc68b8cd5b5ull,0x240ca1cc77ac9c65ull,
    0x2de92c6f592b0275ull,0x4a7484aa6ea6e483ull,0x5cb0a9dcbd41fbd4ull,0x76f988da831153b5ull,
    0x983e5152ee66dfabull,0xa831c66d2db43210ull,0xb00327c898fb213full,0xbf597fc7beef0ee4ull,
    0xc6e00bf33da88fc2ull,0xd5a79147930aa725ull,0x06ca6351e003826full,0x142929670a0e6e70ull,
    0x27b70a8546d22ffcull,0x2e1b21385c26c926ull,0x4d2c6dfc5ac42aedull,0x53380d139d95b3dfull,
    0x650a73548baf63deull,0x766a0abb3c77b2a8ull,0x81c2c92e47edaee6ull,0x92722c851482353bull,
    0xa2bfe8a14cf10364ull,0xa81a664bbc423001ull,0xc24b8b70d0f89791ull,0xc76c51a30654be30ull,
    0xd192e819d6ef5218ull,0xd69906245565a910ull,0xf40e35855771202aull,0x106aa07032bbd1b8ull,
    0x19a4c116b8d2d0c8ull,0x1e376c085141ab53ull,0x2748774cdf8eeb99ull,0x34b0bcb5e19b48a8ull,
    0x391c0cb3c5c95a63ull,0x4ed8aa4ae3418acbull,0x5b9cca4f7763e373ull,0x682e6ff3d6b2b8a3ull,
    0x748f82ee5defb2fcull,0x78a5636f43172f60ull,0x84c87814a1f0ab72ull,0x8cc702081a6439ecull,
    0x90befffa23631e28ull,0xa4506cebde82bde9ull,0xbef9a3f7b2c67915ull,0xc67178f2e372532bull,
    0xca273eceea26619cull,0xd186b8c721c0c207ull,0xeada7dd6cde0eb1eull,0xf57d4f7fee6ed178ull,
    0x06f067aa72176fbaull,0x0a637dc5a2c898a6ull,0x113f9804bef90daeull,0x1b710b35131c471bull,
    0x28db77f523047d84ull,0x32caab7b40c72493ull,0x3c9ebe0a15c9bebcull,0x431d67c49c100d4cull,
    0x4cc5d4becb3e42b6ull,0x597f299cfc657e2aull,0x5fcb6fab3ad6faecull,0x6c44198c4a475817ull};

void sha512_compress(uint64_t h[8], const uint8_t block[128]) {
    uint64_t w[80];
    for (int i = 0; i < 16; ++i) {
        uint64_t v = 0;
        for (int j = 0; j < 8; ++j) v = (v << 8) | block[i*8+j];
        w[i] = v;
    }
    for (int i = 16; i < 80; ++i) {
        uint64_t s0 = rotr64(w[i-15],1) ^ rotr64(w[i-15],8) ^ (w[i-15] >> 7);
        uint64_t s1 = rotr64(w[i-2],19) ^ rotr64(w[i-2],61) ^ (w[i-2] >> 6);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint64_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
    for (int i = 0; i < 80; ++i) {
        uint64_t S1 = rotr64(e,14) ^ rotr64(e,18) ^ rotr64(e,41);
        uint64_t ch = (e & f) ^ (~e & g);
        uint64_t t1 = hh + S1 + ch + kSha512K[i] + w[i];
        uint64_t S0 = rotr64(a,28) ^ rotr64(a,34) ^ rotr64(a,39);
        uint64_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint64_t t2 = S0 + mj;
        hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
}

}  // namespace

void Sha256::init() {
    h_[0]=0x6a09e667u; h_[1]=0xbb67ae85u; h_[2]=0x3c6ef372u; h_[3]=0xa54ff53au;
    h_[4]=0x510e527fu; h_[5]=0x9b05688cu; h_[6]=0x1f83d9abu; h_[7]=0x5be0cd19u;
    len_ = 0; buf_len_ = 0;
}

void Sha256::update(const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    len_ += n;
    if (buf_len_) {
        size_t need = 64 - buf_len_;
        size_t take = n < need ? n : need;
        std::memcpy(buf_ + buf_len_, p, take);
        buf_len_ += take; p += take; n -= take;
        if (buf_len_ == 64) { sha256_compress(h_, buf_); buf_len_ = 0; }
    }
    while (n >= 64) { sha256_compress(h_, p); p += 64; n -= 64; }
    if (n) { std::memcpy(buf_, p, n); buf_len_ = n; }
}

void Sha256::final(uint8_t out[32]) {
    uint64_t bits = len_ * 8;
    uint8_t pad = 0x80;
    update(&pad, 1);
    uint8_t zero = 0;
    while (buf_len_ != 56) update(&zero, 1);
    uint8_t lenbuf[8];
    for (int i = 0; i < 8; ++i) lenbuf[i] = uint8_t(bits >> (56 - i*8));
    update(lenbuf, 8);
    for (int i = 0; i < 8; ++i) {
        out[i*4]   = uint8_t(h_[i] >> 24); out[i*4+1] = uint8_t(h_[i] >> 16);
        out[i*4+2] = uint8_t(h_[i] >> 8);  out[i*4+3] = uint8_t(h_[i]);
    }
}

Bytes Sha256::one_shot(const void* data, size_t n) {
    Sha256 s; s.update(data, n);
    Bytes out(kSha256Digest); s.final(out.data());
    return out;
}

void Sha512::init() {
    h_[0]=0x6a09e667f3bcc908ull; h_[1]=0xbb67ae8584caa73bull; h_[2]=0x3c6ef372fe94f82bull;
    h_[3]=0xa54ff53a5f1d36f1ull; h_[4]=0x510e527fade682d1ull; h_[5]=0x9b05688c2b3e6c1full;
    h_[6]=0x1f83d9abfb41bd6bull; h_[7]=0x5be0cd19137e2179ull;
    len_ = 0; buf_len_ = 0;
}

void Sha512::update(const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    len_ += n;
    if (buf_len_) {
        size_t need = 128 - buf_len_;
        size_t take = n < need ? n : need;
        std::memcpy(buf_ + buf_len_, p, take);
        buf_len_ += take; p += take; n -= take;
        if (buf_len_ == 128) { sha512_compress(h_, buf_); buf_len_ = 0; }
    }
    while (n >= 128) { sha512_compress(h_, p); p += 128; n -= 128; }
    if (n) { std::memcpy(buf_, p, n); buf_len_ = n; }
}

void Sha512::final(uint8_t out[64]) {
    uint64_t bits = len_ * 8;
    uint8_t pad = 0x80; update(&pad, 1);
    uint8_t zero = 0;
    while (buf_len_ != 112) update(&zero, 1);
    uint8_t lenbuf[16];
    std::memset(lenbuf, 0, 8);
    for (int i = 0; i < 8; ++i) lenbuf[8 + i] = uint8_t(bits >> (56 - i*8));
    update(lenbuf, 16);
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 8; ++j) out[i*8+j] = uint8_t(h_[i] >> (56 - j*8));
}

Bytes Sha512::one_shot(const void* data, size_t n) {
    Sha512 s; s.update(data, n);
    Bytes out(kSha512Digest); s.final(out.data());
    return out;
}

void Sha384::init() {
    h_[0]=0xcbbb9d5dc1059ed8ull; h_[1]=0x629a292a367cd507ull; h_[2]=0x9159015a3070dd17ull;
    h_[3]=0x152fecd8f70e5939ull; h_[4]=0x67332667ffc00b31ull; h_[5]=0x8eb44a8768581511ull;
    h_[6]=0xdb0c2e0d64f98fa7ull; h_[7]=0x47b5481dbefa4fa4ull;
    len_ = 0; buf_len_ = 0;
}

void Sha384::update(const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    len_ += n;
    if (buf_len_) {
        size_t need = 128 - buf_len_;
        size_t take = n < need ? n : need;
        std::memcpy(buf_ + buf_len_, p, take);
        buf_len_ += take; p += take; n -= take;
        if (buf_len_ == 128) { sha512_compress(h_, buf_); buf_len_ = 0; }
    }
    while (n >= 128) { sha512_compress(h_, p); p += 128; n -= 128; }
    if (n) { std::memcpy(buf_, p, n); buf_len_ = n; }
}

void Sha384::final(uint8_t out[48]) {
    uint64_t bits = len_ * 8;
    uint8_t pad = 0x80; update(&pad, 1);
    uint8_t zero = 0;
    while (buf_len_ != 112) update(&zero, 1);
    uint8_t lenbuf[16];
    std::memset(lenbuf, 0, 8);
    for (int i = 0; i < 8; ++i) lenbuf[8 + i] = uint8_t(bits >> (56 - i*8));
    update(lenbuf, 16);
    for (int i = 0; i < 6; ++i)
        for (int j = 0; j < 8; ++j) out[i*8+j] = uint8_t(h_[i] >> (56 - j*8));
}

Bytes Sha384::one_shot(const void* data, size_t n) {
    Sha384 s; s.update(data, n);
    Bytes out(kSha384Digest); s.final(out.data());
    return out;
}

// =============================================================================
// HMAC + HKDF
// =============================================================================
Bytes hmac_sha256(const Bytes& key, const uint8_t* data, size_t n) {
    constexpr size_t B = 64;
    Bytes k = key;
    if (k.size() > B) k = Sha256::one_shot(k.data(), k.size());
    k.resize(B, 0);
    Bytes ipad(B), opad(B);
    for (size_t i = 0; i < B; ++i) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }

    Sha256 inner;
    inner.update(ipad.data(), B);
    inner.update(data, n);
    uint8_t ih[kSha256Digest];
    inner.final(ih);

    Sha256 outer;
    outer.update(opad.data(), B);
    outer.update(ih, kSha256Digest);
    Bytes out(kSha256Digest);
    outer.final(out.data());
    secure_zero(ipad); secure_zero(opad); secure_zero(ih);
    return out;
}

Bytes hmac_sha256(const Bytes& key, const Bytes& data) {
    return hmac_sha256(key, data.data(), data.size());
}

Bytes hmac_sha256(const Bytes& key, std::string_view data) {
    return hmac_sha256(key, reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

Bytes hmac_sha384(const Bytes& key, const Bytes& data) {
    constexpr size_t B = 128;
    Bytes k = key;
    if (k.size() > B) k = Sha384::one_shot(k.data(), k.size());
    k.resize(B, 0);
    Bytes ipad(B), opad(B);
    for (size_t i = 0; i < B; ++i) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    Sha384 inner; inner.update(ipad.data(), B); inner.update(data.data(), data.size());
    uint8_t ih[kSha384Digest]; inner.final(ih);
    Sha384 outer; outer.update(opad.data(), B); outer.update(ih, kSha384Digest);
    Bytes out(kSha384Digest); outer.final(out.data());
    secure_zero(ipad); secure_zero(opad); secure_zero(ih);
    return out;
}

Bytes hmac_sha512(const Bytes& key, const Bytes& data) {
    constexpr size_t B = 128;
    Bytes k = key;
    if (k.size() > B) k = Sha512::one_shot(k.data(), k.size());
    k.resize(B, 0);
    Bytes ipad(B), opad(B);
    for (size_t i = 0; i < B; ++i) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    Sha512 inner; inner.update(ipad.data(), B); inner.update(data.data(), data.size());
    uint8_t ih[kSha512Digest]; inner.final(ih);
    Sha512 outer; outer.update(opad.data(), B); outer.update(ih, kSha512Digest);
    Bytes out(kSha512Digest); outer.final(out.data());
    secure_zero(ipad); secure_zero(opad); secure_zero(ih);
    return out;
}

Bytes hkdf_extract_sha256(const Bytes& salt, const Bytes& ikm) {
    return hmac_sha256(salt, ikm.data(), ikm.size());
}

Bytes hkdf_expand_sha256(const Bytes& prk, const Bytes& info, size_t out_len) {
    Bytes out;
    out.reserve(out_len);
    Bytes t;
    uint8_t counter = 1;
    while (out.size() < out_len) {
        Bytes msg = t;
        msg.insert(msg.end(), info.begin(), info.end());
        msg.push_back(counter++);
        t = hmac_sha256(prk, msg.data(), msg.size());
        size_t take = std::min(t.size(), out_len - out.size());
        out.insert(out.end(), t.begin(), t.begin() + take);
    }
    return out;
}

Bytes hash_bytes(HashKind kind, const uint8_t* data, size_t n) {
    if (kind == HashKind::Sha384) return Sha384::one_shot(data, n);
    return Sha256::one_shot(data, n);
}

Bytes hmac(HashKind kind, const Bytes& key, const uint8_t* data, size_t n) {
    if (kind == HashKind::Sha384) {
        // hmac_sha384 only exposes a Bytes overload; wrap the pointer form.
        Bytes msg(data, data + n);
        return hmac_sha384(key, msg);
    }
    return hmac_sha256(key, data, n);
}

Bytes hkdf_extract(HashKind kind, const Bytes& salt, const Bytes& ikm) {
    return hmac(kind, salt, ikm.data(), ikm.size());
}

Bytes hkdf_expand(HashKind kind, const Bytes& prk, const Bytes& info, size_t out_len) {
    Bytes out;
    out.reserve(out_len);
    Bytes t;
    uint8_t counter = 1;
    const size_t dlen = hash_len(kind);
    while (out.size() < out_len) {
        Bytes msg = t;
        msg.insert(msg.end(), info.begin(), info.end());
        msg.push_back(counter++);
        t = hmac(kind, prk, msg.data(), msg.size());
        size_t take = std::min(t.size(), out_len - out.size());
        out.insert(out.end(), t.begin(), t.begin() + take);
    }
    (void)dlen;
    return out;
}

Bytes hkdf_expand_label(HashKind kind, const Bytes& secret, std::string_view label,
                        const Bytes& context, size_t out_len) {
    static const std::string prefix = "tls13 ";
    Bytes info;
    append_u16(info, uint16_t(out_len));
    std::string full = prefix + std::string(label);
    append_u8(info, uint8_t(full.size()));
    append(info, full);
    append_u8(info, uint8_t(context.size()));
    info.insert(info.end(), context.begin(), context.end());
    return hkdf_expand(kind, secret, info, out_len);
}

Bytes hkdf_expand_label(const Bytes& secret, std::string_view label,
                        const Bytes& context, size_t out_len) {
    // HkdfLabel ::= struct { uint16 length; opaque label<7..255>;
    //                         opaque context<0..255>; }
    static const std::string prefix = "tls13 ";
    Bytes info;
    append_u16(info, uint16_t(out_len));
    std::string full = prefix + std::string(label);
    append_u8(info, uint8_t(full.size()));
    append(info, full);
    append_u8(info, uint8_t(context.size()));
    info.insert(info.end(), context.begin(), context.end());
    return hkdf_expand_sha256(secret, info, out_len);
}

// =============================================================================
// AES  (FIPS 197)  -- compact byte oriented implementation
// =============================================================================
namespace {

const uint8_t kSbox[256] = {
0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16};

const uint8_t kInvSbox[256] = {
0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d};

inline uint8_t xtime(uint8_t x) { return uint8_t((x << 1) ^ ((x >> 7) * 0x1b)); }
inline uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    for (int i = 0; i < 8; ++i) {
        if (b & 1) r ^= a;
        b >>= 1;
        a = xtime(a);
    }
    return r;
}

// AES context supporting 128/192/256 bit keys.
struct AesKey {
    uint8_t  rk[240];
    int      nr;
    int      nk;
};

void aes_expand(const uint8_t* key, int key_bits, AesKey& ctx) {
    ctx.nk = key_bits / 32;
    ctx.nr = ctx.nk + 6;
    int total = 4 * (ctx.nr + 1);
    std::memcpy(ctx.rk, key, size_t(ctx.nk) * 4);
    uint8_t rcon = 1;
    for (int i = ctx.nk; i < total; ++i) {
        uint8_t t[4];
        std::memcpy(t, ctx.rk + (i - 1) * 4, 4);
        if (i % ctx.nk == 0) {
            uint8_t tmp = t[0];
            t[0] = uint8_t(kSbox[t[1]] ^ rcon); t[1] = kSbox[t[2]];
            t[2] = kSbox[t[3]];                 t[3] = kSbox[tmp];
            rcon = xtime(rcon);
        } else if (ctx.nk > 6 && (i % ctx.nk) == 4) {
            for (int j = 0; j < 4; ++j) t[j] = kSbox[t[j]];
        }
        for (int j = 0; j < 4; ++j)
            ctx.rk[i*4 + j] = uint8_t(ctx.rk[(i - ctx.nk)*4 + j] ^ t[j]);
    }
}

inline void add_round_key(uint8_t s[16], const uint8_t* rk) {
    for (int i = 0; i < 16; ++i) s[i] ^= rk[i];
}

void aes_encrypt_block(const AesKey& ctx, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    std::memcpy(s, in, 16);
    add_round_key(s, ctx.rk);
    for (int round = 1; round <= ctx.nr; ++round) {
        // SubBytes + ShiftRows
        uint8_t t[16];
        t[0]  = kSbox[s[0]];  t[1]  = kSbox[s[5]];  t[2]  = kSbox[s[10]]; t[3]  = kSbox[s[15]];
        t[4]  = kSbox[s[4]];  t[5]  = kSbox[s[9]];  t[6]  = kSbox[s[14]]; t[7]  = kSbox[s[3]];
        t[8]  = kSbox[s[8]];  t[9]  = kSbox[s[13]]; t[10] = kSbox[s[2]];  t[11] = kSbox[s[7]];
        t[12] = kSbox[s[12]]; t[13] = kSbox[s[1]];  t[14] = kSbox[s[6]];  t[15] = kSbox[s[11]];
        if (round != ctx.nr) {
            for (int c = 0; c < 4; ++c) {
                uint8_t* p = t + c*4;
                uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
                uint8_t m0 = uint8_t(gmul(a0,2) ^ gmul(a1,3) ^ a2 ^ a3);
                uint8_t m1 = uint8_t(a0 ^ gmul(a1,2) ^ gmul(a2,3) ^ a3);
                uint8_t m2 = uint8_t(a0 ^ a1 ^ gmul(a2,2) ^ gmul(a3,3));
                uint8_t m3 = uint8_t(gmul(a0,3) ^ a1 ^ a2 ^ gmul(a3,2));
                p[0]=m0; p[1]=m1; p[2]=m2; p[3]=m3;
            }
        }
        std::memcpy(s, t, 16);
        add_round_key(s, ctx.rk + round * 16);
    }
    std::memcpy(out, s, 16);
}

   // =============================================================================
// GHASH (NIST SP 800-38D 6.4)  --  128-bit carry-less multiplication
// =============================================================================
struct GHashKey { uint64_t h_lo, h_hi; };

inline void gf128_mul(uint64_t x_lo, uint64_t x_hi,
                      uint64_t h_lo, uint64_t h_hi,
                      uint64_t& z_lo, uint64_t& z_hi) {
    // Bitwise shift-and-add over GF(2^128) with the GCM reduction polynomial.
    uint64_t v_lo = h_lo, v_hi = h_hi;
    uint64_t r_lo = 0, r_hi = 0;
    // NIST SP 800-38D walks the block most-significant bit first (bit 0 of the
    // algorithm is the MSB of the 128-bit string).
    for (int i = 0; i < 128; ++i) {
        uint64_t bit = (i < 64) ? ((x_hi >> (63 - i)) & 1u) : ((x_lo >> (127 - i)) & 1u);
        uint64_t mask = uint64_t(0) - bit;
        r_lo ^= v_lo & mask;
        r_hi ^= v_hi & mask;
        uint64_t lsb = v_lo & 1u;
        uint64_t new_lo = (v_lo >> 1) | (v_hi << 63);
        uint64_t new_hi = v_hi >> 1;
        uint64_t red = uint64_t(0) - lsb;              // 0 or ~0
        new_hi ^= 0xe100000000000000ull & red;
        v_lo = new_lo; v_hi = new_hi;
    }
    z_lo = r_lo; z_hi = r_hi;
}

struct GcmCtx {
    uint64_t h_lo, h_hi;
    uint64_t y_lo, y_hi;
    uint8_t  ej0[16];      // E(K, J0) for tag computation
};

void gcm_init(GcmCtx& g, const AesKey& aes, const uint8_t nonce[12]) {
    uint8_t h[16];
    static const uint8_t zero[16] = {0};
    aes_encrypt_block(aes, zero, h);
    g.h_lo = 0; g.h_hi = 0;
    for (int i = 0; i < 8; ++i) g.h_hi = (g.h_hi << 8) | h[i];
    for (int i = 8; i < 16; ++i) g.h_lo = (g.h_lo << 8) | h[i];
    g.y_lo = 0; g.y_hi = 0;

    uint8_t j0[16];
    std::memcpy(j0, nonce, 12);
    j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;
    aes_encrypt_block(aes, j0, g.ej0);
}

// Absorbs `len` bytes, zero-padding the final partial block as GCM requires.
void ghash_bytes(GcmCtx& g, const uint8_t* data, size_t len) {
    size_t off = 0;
    while (off + 16 <= len) {
        uint64_t x_hi = 0, x_lo = 0;
        for (int i = 0; i < 8; ++i) x_hi = (x_hi << 8) | data[off + i];
        for (int i = 8; i < 16; ++i) x_lo = (x_lo << 8) | data[off + i];
        x_hi ^= g.y_hi; x_lo ^= g.y_lo;
        gf128_mul(x_lo, x_hi, g.h_lo, g.h_hi, g.y_lo, g.y_hi);
        off += 16;
    }
    if (off < len) {
        uint8_t block[16] = {0};
        std::memcpy(block, data + off, len - off);
        uint64_t x_hi = 0, x_lo = 0;
        for (int i = 0; i < 8; ++i) x_hi = (x_hi << 8) | block[i];
        for (int i = 8; i < 16; ++i) x_lo = (x_lo << 8) | block[i];
        x_hi ^= g.y_hi; x_lo ^= g.y_lo;
        gf128_mul(x_lo, x_hi, g.h_lo, g.h_hi, g.y_lo, g.y_hi);
    }
}

void gcm_ctr_xor(const AesKey& aes, const uint8_t j0[16], const uint8_t* in,
                 size_t len, uint8_t* out) {
    uint8_t ctr[16];
    std::memcpy(ctr, j0, 16);
    size_t off = 0;
    while (off < len) {
        ctr[15] = uint8_t(ctr[15] + 1);
        if (ctr[15] == 0) {
            ctr[14] = uint8_t(ctr[14] + 1);
            if (ctr[14] == 0) { ctr[13] = uint8_t(ctr[13] + 1); if (ctr[13] == 0) ctr[12] = uint8_t(ctr[12] + 1); }
        }
        uint8_t ks[16];
        aes_encrypt_block(aes, ctr, ks);
        size_t n = std::min<size_t>(16, len - off);
        for (size_t i = 0; i < n; ++i) out[off + i] = uint8_t(in[off + i] ^ ks[i]);
        off += n;
    }
}

void gcm_tag(GcmCtx& g, const AesKey& aes, size_t aad_len, size_t ct_len,
             const uint8_t j0[16], uint8_t tag[16]) {
    uint8_t lens[16];
    uint64_t aad_bits = uint64_t(aad_len) * 8;
    uint64_t ct_bits  = uint64_t(ct_len)  * 8;
    for (int i = 0; i < 8; ++i) lens[i]     = uint8_t(aad_bits >> (56 - i*8));
    for (int i = 0; i < 8; ++i) lens[8 + i] = uint8_t(ct_bits  >> (56 - i*8));
    ghash_bytes(g, lens, 16);           // GHASH(A || 0* || C || 0* || len(A) || len(C))

    uint8_t ek[16];
    aes_encrypt_block(aes, j0, ek);
    uint8_t yb[16];
    for (int i = 0; i < 8; ++i) yb[i]     = uint8_t(g.y_hi >> (56 - i*8));
    for (int i = 0; i < 8; ++i) yb[8 + i] = uint8_t(g.y_lo >> (56 - i*8));
    for (int i = 0; i < 16; ++i) tag[i] = uint8_t(ek[i] ^ yb[i]);
}

}  // namespace

void aes128_ctr(const uint8_t key[16], const uint8_t iv[16], const uint8_t* in, size_t n, uint8_t* out) {
    AesKey ctx; aes_expand(key, 128, ctx);
    uint8_t ctr[16];
    std::memcpy(ctr, iv, 16);
    size_t off = 0;
    while (off < n) {
        uint8_t ks[16];
        aes_encrypt_block(ctx, ctr, ks);
        size_t m = std::min<size_t>(16, n - off);
        for (size_t i = 0; i < m; ++i) out[off + i] = uint8_t(in[off + i] ^ ks[i]);
        off += m;
        for (int i = 15; i >= 0; --i) { if (++ctr[i]) break; }
    }
    secure_zero(ctr);
}

namespace {

void gcm_encrypt_impl(const uint8_t* key, int key_bits, const uint8_t nonce[12],
                      const uint8_t* aad, size_t aad_len,
                      const uint8_t* in, size_t in_len, uint8_t* out, uint8_t tag[16]) {
    AesKey aes; aes_expand(key, key_bits, aes);
    GcmCtx g; gcm_init(g, aes, nonce);
    uint8_t j0[16];
    std::memcpy(j0, nonce, 12); j0[12]=0; j0[13]=0; j0[14]=0; j0[15]=1;
    if (aad_len) ghash_bytes(g, aad, aad_len);
    gcm_ctr_xor(aes, j0, in, in_len, out);
    if (in_len) ghash_bytes(g, out, in_len);
    gcm_tag(g, aes, aad_len, in_len, j0, tag);
    secure_zero(j0);
    secure_zero(aes.rk);
}

bool gcm_decrypt_impl(const uint8_t* key, int key_bits, const uint8_t nonce[12],
                      const uint8_t* aad, size_t aad_len,
                      const uint8_t* in, size_t in_len, const uint8_t tag[16],
                      uint8_t* out) {
    AesKey aes; aes_expand(key, key_bits, aes);
    GcmCtx g; gcm_init(g, aes, nonce);
    uint8_t j0[16];
    std::memcpy(j0, nonce, 12); j0[12]=0; j0[13]=0; j0[14]=0; j0[15]=1;
    if (aad_len) ghash_bytes(g, aad, aad_len);
    if (in_len)  ghash_bytes(g, in, in_len);
    uint8_t expected[16];
    gcm_tag(g, aes, aad_len, in_len, j0, expected);
    bool ok = ct_equal(expected, tag, 16);
    if (ok) gcm_ctr_xor(aes, j0, in, in_len, out);
    secure_zero(expected);
    secure_zero(aes.rk);
    return ok;
}

}  // namespace

void aes128_gcm_encrypt(const uint8_t key[16], const uint8_t nonce[12],
                        const uint8_t* aad, size_t aad_len,
                        const uint8_t* in, size_t in_len, uint8_t* out, uint8_t tag[16]) {
    gcm_encrypt_impl(key, 128, nonce, aad, aad_len, in, in_len, out, tag);
}

bool aes128_gcm_decrypt(const uint8_t key[16], const uint8_t nonce[12],
                        const uint8_t* aad, size_t aad_len,
                        const uint8_t* in, size_t in_len, const uint8_t tag[16], uint8_t* out) {
    return gcm_decrypt_impl(key, 128, nonce, aad, aad_len, in, in_len, tag, out);
}

void aes256_gcm_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                        const uint8_t* aad, size_t aad_len,
                        const uint8_t* in, size_t in_len, uint8_t* out, uint8_t tag[16]) {
    gcm_encrypt_impl(key, 256, nonce, aad, aad_len, in, in_len, out, tag);
}

bool aes256_gcm_decrypt(const uint8_t key[32], const uint8_t nonce[12],
                        const uint8_t* aad, size_t aad_len,
                        const uint8_t* in, size_t in_len, const uint8_t tag[16], uint8_t* out) {
    return gcm_decrypt_impl(key, 256, nonce, aad, aad_len, in, in_len, tag, out);
}

// =============================================================================
// ChaCha20 + Poly1305  (RFC 8439)
// =============================================================================
namespace {

inline uint32_t rotl32(uint32_t v, int c) { return (v << c) | (v >> (32 - c)); }

void chacha20_block(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t out[64]) {
    uint32_t st[16];
    st[0]=0x61707865u; st[1]=0x3320646eu; st[2]=0x79622d32u; st[3]=0x6b206574u;
    for (int i = 0; i < 8; ++i)
        st[4+i] = (uint32_t(key[i*4])) | (uint32_t(key[i*4+1]) << 8) |
                  (uint32_t(key[i*4+2]) << 16) | (uint32_t(key[i*4+3]) << 24);
    st[12] = counter;
    for (int i = 0; i < 3; ++i)
        st[13+i] = (uint32_t(nonce[i*4])) | (uint32_t(nonce[i*4+1]) << 8) |
                   (uint32_t(nonce[i*4+2]) << 16) | (uint32_t(nonce[i*4+3]) << 24);
    uint32_t x[16];
    std::memcpy(x, st, sizeof(x));
#define QR(a,b,c,d)                        \
    x[a]+=x[b]; x[d]^=x[a]; x[d]=rotl32(x[d],16); \
    x[c]+=x[d]; x[b]^=x[c]; x[b]=rotl32(x[b],12); \
    x[a]+=x[b]; x[d]^=x[a]; x[d]=rotl32(x[d], 8); \
    x[c]+=x[d]; x[b]^=x[c]; x[b]=rotl32(x[b], 7);
    for (int i = 0; i < 10; ++i) {
        QR(0,4,8,12)  QR(1,5,9,13)  QR(2,6,10,14) QR(3,7,11,15)
        QR(0,5,10,15) QR(1,6,11,12) QR(2,7,8,13)  QR(3,4,9,14)
    }
#undef QR
    for (int i = 0; i < 16; ++i) {
        uint32_t v = x[i] + st[i];
        out[i*4]   = uint8_t(v);       out[i*4+1] = uint8_t(v >> 8);
        out[i*4+2] = uint8_t(v >> 16); out[i*4+3] = uint8_t(v >> 24);
    }
}

// Poly1305 one-shot over a contiguous message.
void poly1305(const uint8_t key[32], const uint8_t* msg, size_t len, uint8_t tag[16]) {
    uint32_t r[5];
    r[0] = (uint32_t(key[0]) | (uint32_t(key[1]) << 8) | (uint32_t(key[2]) << 16) | (uint32_t(key[3]) << 24)) & 0x3ffffffu;
    r[1] = ((uint32_t(key[3]) >> 2) | (uint32_t(key[4]) << 6) | (uint32_t(key[5]) << 14) | (uint32_t(key[6]) << 22)) & 0x3ffff03u;
    r[2] = ((uint32_t(key[6]) >> 4) | (uint32_t(key[7]) << 4) | (uint32_t(key[8]) << 12) | (uint32_t(key[9]) << 20)) & 0x3ffc0ffu;
    r[3] = ((uint32_t(key[9]) >> 6) | (uint32_t(key[10]) << 2) | (uint32_t(key[11]) << 10) | (uint32_t(key[12]) << 18)) & 0x3f03fffu;
    r[4] = ((uint32_t(key[12]) >> 8) | (uint32_t(key[13]) << 0) | (uint32_t(key[14]) << 8) | (uint32_t(key[15]) << 16)) & 0x00fffffu;
    uint32_t s[4];
    for (int i = 0; i < 4; ++i)
        s[i] = uint32_t(key[16+i*4]) | (uint32_t(key[17+i*4]) << 8) |
               (uint32_t(key[18+i*4]) << 16) | (uint32_t(key[19+i*4]) << 24);
    uint32_t h[5] = {0,0,0,0,0};
    const uint32_t r1 = r[1], r2 = r[2], r3 = r[3], r4 = r[4];
    const uint32_t s1 = r1*5, s2 = r2*5, s3 = r3*5, s4 = r4*5;
    const uint32_t hib = 1u << 24;

    size_t i = 0;
    for (; i + 16 <= len; i += 16) {
        h[0] += (uint32_t(msg[i]) | (uint32_t(msg[i+1]) << 8) | (uint32_t(msg[i+2]) << 16) | (uint32_t(msg[i+3]) << 24)) & 0x3ffffffu;
        h[1] += ((uint32_t(msg[i+3]) >> 2) | (uint32_t(msg[i+4]) << 6) | (uint32_t(msg[i+5]) << 14) | (uint32_t(msg[i+6]) << 22)) & 0x3ffffffu;
        h[2] += ((uint32_t(msg[i+6]) >> 4) | (uint32_t(msg[i+7]) << 4) | (uint32_t(msg[i+8]) << 12) | (uint32_t(msg[i+9]) << 20)) & 0x3ffffffu;
        h[3] += ((uint32_t(msg[i+9]) >> 6) | (uint32_t(msg[i+10]) << 2) | (uint32_t(msg[i+11]) << 10) | (uint32_t(msg[i+12]) << 18)) & 0x3ffffffu;
        h[4] += ((uint32_t(msg[i+12]) >> 8) | (uint32_t(msg[i+13]) << 0) | (uint32_t(msg[i+14]) << 8) | (uint32_t(msg[i+15]) << 16)) | hib;

        uint64_t d0 = uint64_t(h[0])*r[0] + uint64_t(h[1])*s4 + uint64_t(h[2])*s3 + uint64_t(h[3])*s2 + uint64_t(h[4])*s1;
        uint64_t d1 = uint64_t(h[0])*r1 + uint64_t(h[1])*r[0] + uint64_t(h[2])*s4 + uint64_t(h[3])*s3 + uint64_t(h[4])*s2;
        uint64_t d2 = uint64_t(h[0])*r2 + uint64_t(h[1])*r1 + uint64_t(h[2])*r[0]+ uint64_t(h[3])*s4 + uint64_t(h[4])*s3;
        uint64_t d3 = uint64_t(h[0])*r3 + uint64_t(h[1])*r2 + uint64_t(h[2])*r1 + uint64_t(h[3])*r[0]+ uint64_t(h[4])*s4;
        uint64_t d4 = uint64_t(h[0])*r4 + uint64_t(h[1])*r3 + uint64_t(h[2])*r2 + uint64_t(h[3])*r1 + uint64_t(h[4])*r[0];
        uint32_t c = uint32_t(d0 >> 26); h[0] = uint32_t(d0) & 0x3ffffffu;
        d1 += c; c = uint32_t(d1 >> 26); h[1] = uint32_t(d1) & 0x3ffffffu;
        d2 += c; c = uint32_t(d2 >> 26); h[2] = uint32_t(d2) & 0x3ffffffu;
        d3 += c; c = uint32_t(d3 >> 26); h[3] = uint32_t(d3) & 0x3ffffffu;
        d4 += c; c = uint32_t(d4 >> 26); h[4] = uint32_t(d4) & 0x3ffffffu;
        h[0] += c * 5;
        c = h[0] >> 26; h[0] &= 0x3ffffffu;
        h[1] += c;
    }
    if (i < len) {
        uint8_t blk[16] = {0};
        std::memcpy(blk, msg + i, len - i);
        blk[len - i] = 1;
        h[0] += (uint32_t(blk[0]) | (uint32_t(blk[1]) << 8) | (uint32_t(blk[2]) << 16) | (uint32_t(blk[3]) << 24)) & 0x3ffffffu;
        h[1] += ((uint32_t(blk[3]) >> 2) | (uint32_t(blk[4]) << 6) | (uint32_t(blk[5]) << 14) | (uint32_t(blk[6]) << 22)) & 0x3ffffffu;
        h[2] += ((uint32_t(blk[6]) >> 4) | (uint32_t(blk[7]) << 4) | (uint32_t(blk[8]) << 12) | (uint32_t(blk[9]) << 20)) & 0x3ffffffu;
        h[3] += ((uint32_t(blk[9]) >> 6) | (uint32_t(blk[10]) << 2) | (uint32_t(blk[11]) << 10) | (uint32_t(blk[12]) << 18)) & 0x3ffffffu;
        h[4] += ((uint32_t(blk[12]) >> 8) | (uint32_t(blk[13]) << 0) | (uint32_t(blk[14]) << 8) | (uint32_t(blk[15]) << 16));
        uint64_t d0 = uint64_t(h[0])*r[0] + uint64_t(h[1])*s4 + uint64_t(h[2])*s3 + uint64_t(h[3])*s2 + uint64_t(h[4])*s1;
        uint64_t d1 = uint64_t(h[0])*r1 + uint64_t(h[1])*r[0] + uint64_t(h[2])*s4 + uint64_t(h[3])*s3 + uint64_t(h[4])*s2;
        uint64_t d2 = uint64_t(h[0])*r2 + uint64_t(h[1])*r1 + uint64_t(h[2])*r[0]+ uint64_t(h[3])*s4 + uint64_t(h[4])*s3;
        uint64_t d3 = uint64_t(h[0])*r3 + uint64_t(h[1])*r2 + uint64_t(h[2])*r1 + uint64_t(h[3])*r[0]+ uint64_t(h[4])*s4;
        uint64_t d4 = uint64_t(h[0])*r4 + uint64_t(h[1])*r3 + uint64_t(h[2])*r2 + uint64_t(h[3])*r1 + uint64_t(h[4])*r[0];
        uint32_t c = uint32_t(d0 >> 26); h[0] = uint32_t(d0) & 0x3ffffffu;
        d1 += c; c = uint32_t(d1 >> 26); h[1] = uint32_t(d1) & 0x3ffffffu;
        d2 += c; c = uint32_t(d2 >> 26); h[2] = uint32_t(d2) & 0x3ffffffu;
        d3 += c; c = uint32_t(d3 >> 26); h[3] = uint32_t(d3) & 0x3ffffffu;
        d4 += c; c = uint32_t(d4 >> 26); h[4] = uint32_t(d4) & 0x3ffffffu;
        h[0] += c * 5;
        c = h[0] >> 26; h[0] &= 0x3ffffffu;
        h[1] += c;
    }
    // Final carry + conditional subtraction of 2^130-5.
    uint32_t c = h[1] >> 26; h[1] &= 0x3ffffffu; h[2] += c;
    c = h[2] >> 26; h[2] &= 0x3ffffffu; h[3] += c;
    c = h[3] >> 26; h[3] &= 0x3ffffffu; h[4] += c;
    c = h[4] >> 26; h[4] &= 0x3ffffffu; h[0] += c * 5;
    c = h[0] >> 26; h[0] &= 0x3ffffffu; h[1] += c;

    uint32_t g0 = h[0] + 5;            c = g0 >> 26; g0 &= 0x3ffffffu;
    uint32_t g1 = h[1] + c;            c = g1 >> 26; g1 &= 0x3ffffffu;
    uint32_t g2 = h[2] + c;            c = g2 >> 26; g2 &= 0x3ffffffu;
    uint32_t g3 = h[3] + c;            c = g3 >> 26; g3 &= 0x3ffffffu;
    uint32_t g4 = h[4] + c - (1u << 26);
    uint32_t mask = (g4 >> 31) - 1u;   // all ones when h < 2^130-5
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h[0] = (h[0] & mask) | g0; h[1] = (h[1] & mask) | g1; h[2] = (h[2] & mask) | g2;
    h[3] = (h[3] & mask) | g3; h[4] = (h[4] & mask) | g4;

    // Repack the 130-bit accumulator into four 32-bit words and add the
    // encrypted nonce half of the key (mod 2^128).
    uint64_t f0 = (uint64_t(h[0]) | (uint64_t(h[1]) << 26)) & 0xffffffffull;
    uint64_t f1 = ((uint64_t(h[1]) >> 6)  | (uint64_t(h[2]) << 20)) & 0xffffffffull;
    uint64_t f2 = ((uint64_t(h[2]) >> 12) | (uint64_t(h[3]) << 14)) & 0xffffffffull;
    uint64_t f3 = ((uint64_t(h[3]) >> 18) | (uint64_t(h[4]) << 8))  & 0xffffffffull;
    f0 += s[0];
    f1 += s[1] + (f0 >> 32);
    f2 += s[2] + (f1 >> 32);
    f3 += s[3] + (f2 >> 32);
    const uint64_t words[4] = {f0 & 0xffffffffull, f1 & 0xffffffffull,
                               f2 & 0xffffffffull, f3 & 0xffffffffull};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) tag[i*4 + j] = uint8_t(words[i] >> (j * 8));
}

}  // namespace

void poly1305_mac(const uint8_t key[32], const uint8_t* msg, size_t n, uint8_t tag[16]) {
    poly1305(key, msg, n, tag);
}

void chacha20_poly1305_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                               const uint8_t* aad, size_t aad_len,
                               const uint8_t* in, size_t in_len,
                               uint8_t* out, uint8_t tag[16]) {
    uint8_t block0[64];
    chacha20_block(key, 0, nonce, block0);
    uint8_t polykey[32];
    std::memcpy(polykey, block0, 32);
    // Encrypt payload with counter starting at 1.
    size_t off = 0;
    uint32_t ctr = 1;
    while (off < in_len) {
        uint8_t ks[64];
        chacha20_block(key, ctr++, nonce, ks);
        size_t n = std::min<size_t>(64, in_len - off);
        for (size_t i = 0; i < n; ++i) out[off + i] = uint8_t(in[off + i] ^ ks[i]);
        off += n;
    }
    // Poly1305 over: aad || pad16 || ct || pad16 || len(aad) || len(ct)
    Bytes mac;
    mac.reserve(aad_len + in_len + 64);
    mac.insert(mac.end(), aad, aad + aad_len);
    while (mac.size() % 16) mac.push_back(0);
    mac.insert(mac.end(), out, out + in_len);
    while (mac.size() % 16) mac.push_back(0);
    uint8_t lens[16];
    for (int i = 0; i < 8; ++i) lens[i]     = uint8_t(uint64_t(aad_len) >> (i * 8));   // little-endian
    for (int i = 0; i < 8; ++i) lens[8 + i] = uint8_t(uint64_t(in_len)  >> (i * 8));
    mac.insert(mac.end(), lens, lens + 16);
    poly1305(polykey, mac.data(), mac.size(), tag);
    secure_zero(polykey); secure_zero(block0);
}

bool chacha20_poly1305_decrypt(const uint8_t key[32], const uint8_t nonce[12],
                               const uint8_t* aad, size_t aad_len,
                               const uint8_t* in, size_t in_len,
                               const uint8_t tag[16], uint8_t* out) {
    uint8_t block0[64];
    chacha20_block(key, 0, nonce, block0);
    uint8_t polykey[32];
    std::memcpy(polykey, block0, 32);
    Bytes mac;
    mac.reserve(aad_len + in_len + 64);
    mac.insert(mac.end(), aad, aad + aad_len);
    while (mac.size() % 16) mac.push_back(0);
    mac.insert(mac.end(), in, in + in_len);
    while (mac.size() % 16) mac.push_back(0);
    uint8_t lens[16];
    for (int i = 0; i < 8; ++i) lens[i]     = uint8_t(uint64_t(aad_len) >> (i * 8));   // little-endian
    for (int i = 0; i < 8; ++i) lens[8 + i] = uint8_t(uint64_t(in_len)  >> (i * 8));
    mac.insert(mac.end(), lens, lens + 16);
    uint8_t expected[16];
    poly1305(polykey, mac.data(), mac.size(), expected);
    bool ok = ct_equal(expected, tag, 16);
    if (ok) {
        size_t off = 0; uint32_t ctr = 1;
        while (off < in_len) {
            uint8_t ks[64];
            chacha20_block(key, ctr++, nonce, ks);
            size_t n = std::min<size_t>(64, in_len - off);
            for (size_t i = 0; i < n; ++i) out[off + i] = uint8_t(in[off + i] ^ ks[i]);
            off += n;
        }
    }
    secure_zero(polykey); secure_zero(block0); secure_zero(expected);
    return ok;
}

// =============================================================================
// X25519  (RFC 7748) -- 4x64 limb field arithmetic modulo 2^255-19
// =============================================================================
namespace {

typedef uint64_t u64;
typedef unsigned __int128 u128;

// Field element as four little-endian 64-bit limbs.
struct Fe { u64 v[4]; };

const u64 kLimbMask = ~u64(0);

inline void fe_zero(Fe& r) { r.v[0]=r.v[1]=r.v[2]=r.v[3]=0; }
inline void fe_one (Fe& r) { r.v[0]=1; r.v[1]=r.v[2]=r.v[3]=0; }

inline u64 adc(u64 a, u64 b, u64& carry) {
    u128 s = u128(a) + b + carry;
    carry = u64(s >> 64);
    return u64(s);
}
inline u64 sbb(u64 a, u64 b, u64& borrow) {
    u128 d = u128(a) - b - borrow;
    borrow = u64((d >> 64) & 1);
    return u64(d);
}

// Fold a 5-limb value (2^320 range) back under 2^255-19.  The a24/121665 era
// ladder never produces more than a couple of fold iterations.
inline void fe_reduce5(u64 t[5]) {
    int guard = 0;
    while (t[4] != 0 && guard++ < 8) {
        u64 hi = t[4];
        t[4] = 0;
        u128 add = u128(hi) * 38;                 // 2^256 === 38 (mod 2^255-19)
        u128 cur = u128(t[0]) + u64(add);
        u64 carry = u64(cur >> 64) + u64(add >> 64);
        t[0] = u64(cur);
        for (int i = 1; i < 4 && carry; ++i) {
            u128 s = u128(t[i]) + carry;
            t[i] = u64(s);
            carry = u64(s >> 64);
        }
        t[4] += carry;
    }
}

// Full reduction to the canonical representative in [0, p).
// p = 2^255 - 19 has the 64-bit limbs {2^64-19, 2^64-1, 2^64-1, 2^63-1}.
inline void fe_canonical(Fe& r) {
    // Fold the 2^255 bit back in: 2^255 === 19 (mod p).  Two passes are enough
    // for every value this implementation can produce, and running both passes
    // unconditionally keeps the routine free of secret-dependent branches.
    for (int iter = 0; iter < 2; ++iter) {
        u64 top = r.v[3] >> 63;
        r.v[3] &= (u64(1) << 63) - 1;
        u64 carry = 0;
        r.v[0] = adc(r.v[0], top * 19, carry);
        r.v[1] = adc(r.v[1], 0, carry);
        r.v[2] = adc(r.v[2], 0, carry);
        r.v[3] = adc(r.v[3], 0, carry);
    }
    // Conditional subtraction of p (r < 2p here, so a single pass suffices).
    u64 b = 0;
    u64 t0 = sbb(r.v[0], u64(0) - 19, b);
    u64 t1 = sbb(r.v[1], kLimbMask, b);
    u64 t2 = sbb(r.v[2], kLimbMask, b);
    u64 t3 = sbb(r.v[3], (u64(1) << 63) - 1, b);
    u64 mask = u64(0) - (b ^ 1);            // borrow clear => r >= p => keep t
    r.v[0] = (t0 & mask) | (r.v[0] & ~mask);
    r.v[1] = (t1 & mask) | (r.v[1] & ~mask);
    r.v[2] = (t2 & mask) | (r.v[2] & ~mask);
    r.v[3] = (t3 & mask) | (r.v[3] & ~mask);
}

inline void fe_add(Fe& r, const Fe& a, const Fe& b) {
    u64 c = 0;
    r.v[0] = adc(a.v[0], b.v[0], c);
    r.v[1] = adc(a.v[1], b.v[1], c);
    r.v[2] = adc(a.v[2], b.v[2], c);
    r.v[3] = adc(a.v[3], b.v[3], c);
    if (c) {                                  // fold 2^256 -> 38
        u64 c2 = 0;
        r.v[0] = adc(r.v[0], 38, c2);
        r.v[1] = adc(r.v[1], 0, c2);
        r.v[2] = adc(r.v[2], 0, c2);
        r.v[3] = adc(r.v[3], 0, c2);
        if (c2) { u64 c3 = 0; r.v[0] = adc(r.v[0], 38, c3); r.v[1] = adc(r.v[1], 0, c3); }
    }
    fe_canonical(r);
}

inline void fe_sub(Fe& r, const Fe& a, const Fe& b) {
    // Borrow-subtract, then add p back (mod 2^256) when the difference went
    // negative.  Both inputs are canonical, so a single correction is exact.
    u64 borrow = 0;
    u64 t0 = sbb(a.v[0], b.v[0], borrow);
    u64 t1 = sbb(a.v[1], b.v[1], borrow);
    u64 t2 = sbb(a.v[2], b.v[2], borrow);
    u64 t3 = sbb(a.v[3], b.v[3], borrow);
    u64 m = u64(0) - borrow;                  // all ones when a < b
    u64 carry = 0;
    r.v[0] = adc(t0, (u64(0) - 19) & m, carry);
    r.v[1] = adc(t1, kLimbMask & m, carry);
    r.v[2] = adc(t2, kLimbMask & m, carry);
    r.v[3] = adc(t3, ((u64(1) << 63) - 1) & m, carry);
    fe_canonical(r);
}

// Schoolbook 4x4 multiply followed by pseudo-Mersenne reduction.
inline void fe_mul(Fe& r, const Fe& a, const Fe& b) {
    u64 t[8] = {0,0,0,0,0,0,0,0};
    for (int i = 0; i < 4; ++i) {
        u64 carry = 0;
        for (int j = 0; j < 4; ++j) {
            u128 cur = u128(t[i + j]) + u128(a.v[i]) * b.v[j] + carry;
            t[i + j] = u64(cur);
            carry = u64(cur >> 64);
        }
        for (int k = i + 4; k < 8 && carry; ++k) {   // full carry propagation
            u128 s = u128(t[k]) + carry;
            t[k] = u64(s);
            carry = u64(s >> 64);
        }
    }
    // N = L + 2^256 * H  ==>  N mod p = L + 38 * H
    u64 hp[5] = {0,0,0,0,0};
    u128 c = 0;
    for (int i = 0; i < 4; ++i) {
        u128 cur = u128(t[4 + i]) * 38 + c;
        hp[i] = u64(cur);
        c = cur >> 64;
    }
    hp[4] = u64(c);
    u64 s[5];
    u128 c2 = 0;
    for (int i = 0; i < 4; ++i) {
        u128 cur = u128(hp[i]) + t[i] + c2;
        s[i] = u64(cur);
        c2 = cur >> 64;
    }
    s[4] = u64(u128(hp[4]) + c2);
    fe_reduce5(s);
    r.v[0]=s[0]; r.v[1]=s[1]; r.v[2]=s[2]; r.v[3]=s[3];
    fe_canonical(r);
}

inline void fe_sq(Fe& r, const Fe& a) { fe_mul(r, a, a); }

inline void fe_mul_small(Fe& r, const Fe& a, u64 s) {
    u64 t[5] = {0,0,0,0,0};
    u64 carry = 0;
    for (int i = 0; i < 4; ++i) {
        u128 cur = u128(a.v[i]) * s + carry;
        t[i] = u64(cur);
        carry = u64(cur >> 64);
    }
    t[4] = carry;
    fe_reduce5(t);
    r.v[0]=t[0]; r.v[1]=t[1]; r.v[2]=t[2]; r.v[3]=t[3];
    fe_canonical(r);
}

inline void fe_cswap(Fe& a, Fe& b, u64 swap) {
    u64 mask = u64(0) - swap;
    for (int i = 0; i < 4; ++i) {
        u64 x = mask & (a.v[i] ^ b.v[i]);
        a.v[i] ^= x;
        b.v[i] ^= x;
    }
}

inline void fe_from_bytes(Fe& r, const uint8_t s[32]) {
    for (int i = 0; i < 4; ++i) {
        u64 v = 0;
        for (int j = 7; j >= 0; --j) v = (v << 8) | s[i*8 + j];
        r.v[i] = v;
    }
    r.v[3] &= (u64(1) << 63) - 1;   // clear the unused top bit
    fe_canonical(r);
}

inline void fe_to_bytes(uint8_t out[32], const Fe& a) {
    Fe t = a;
    fe_canonical(t);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 8; ++j) out[i*8 + j] = uint8_t(t.v[i] >> (j * 8));
}

// Exponentiation by a fixed public big-endian exponent (not secret dependent).
void fe_pow(Fe& out, const Fe& base, const uint8_t* exp_be, size_t exp_bytes) {
    Fe result; fe_one(result);
    Fe b = base;
    for (size_t i = 0; i < exp_bytes; ++i) {
        for (int bit = 7; bit >= 0; --bit) {
            Fe tmp; fe_sq(tmp, result); result = tmp;
            if ((exp_be[i] >> bit) & 1) { Fe t2; fe_mul(t2, result, b); result = t2; }
        }
    }
    out = result;
}

void fe_invert(Fe& out, const Fe& a) {
    // p - 2 == 2^255 - 21, encoded big-endian.
    uint8_t be[32];
    std::memset(be, 0xff, sizeof(be));
    be[0]  = 0x7f;
    be[31] = 0xeb;
    fe_pow(out, a, be, sizeof(be));
}

}  // namespace

void x25519_keypair(uint8_t priv[32], uint8_t pub[32]) {
    random_bytes(priv, 32);
    priv[0]  &= 248;
    priv[31] &= 127;
    priv[31] |= 64;
    static const uint8_t kBase[32] = {9};
    x25519(pub, priv, kBase);
}

bool x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    // RFC 7748 decodeScalar25519: clamp a local copy of the scalar.  Callers
    // may pass unclamped material (the private key or an RFC test vector).
    uint8_t k[32];
    std::memcpy(k, scalar, 32);
    k[0]  &= 248;
    k[31] &= 127;
    k[31] |= 64;

    Fe x1, x2, z2, x3, z3;
    fe_from_bytes(x1, point);
    fe_one(x2);
    fe_zero(z2);
    x3 = x1;
    fe_one(z3);

    u64 swap = 0;
    for (int t = 254; t >= 0; --t) {
        u64 kt = (k[t >> 3] >> (t & 7)) & 1;
        swap ^= kt;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = kt;

        Fe A, AA, B, BB, E, C, D, DA, CB, t0, t1;
        fe_add(A, x2, z2);
        fe_sub(B, x2, z2);
        fe_sq(AA, A);
        fe_sq(BB, B);
        fe_sub(E, AA, BB);
        fe_add(C, x3, z3);
        fe_sub(D, x3, z3);
        fe_mul(DA, D, A);
        fe_mul(CB, C, B);
        fe_add(t0, DA, CB);
        fe_sq(x3, t0);
        fe_sub(t1, DA, CB);
        fe_sq(t1, t1);
        fe_mul(z3, t1, x1);
        fe_mul(x2, AA, BB);
        Fe a24e;
        fe_mul_small(a24e, E, 121665);
        fe_add(t0, AA, a24e);
        fe_mul(z2, E, t0);
    }
    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);

    Fe zinv, res;
    fe_invert(zinv, z2);
    fe_mul(res, x2, zinv);
    fe_to_bytes(out, res);

    // Reject the all-zero result (small order point) per RFC 8446.
    uint8_t acc = 0;
    for (int i = 0; i < 32; ++i) acc |= out[i];
    secure_zero(k);
    return acc != 0;
}

// =============================================================================
// CSPRNG
// =============================================================================
void random_bytes(void* out, size_t n) {
    uint8_t* p = static_cast<uint8_t*>(out);
    size_t got = 0;
    while (got < n) {
        ssize_t r = ::getrandom(p + got, n - got, 0);
        if (r > 0) { got += size_t(r); continue; }
        if (r < 0 && errno == EINTR) continue;
        break;
    }
    if (got == n) return;
    // Fallback: /dev/urandom, then a time/PID/stack mixed fallback.
    int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        while (got < n) {
            ssize_t r = ::read(fd, p + got, n - got);
            if (r <= 0) break;
            got += size_t(r);
        }
        ::close(fd);
        if (got == n) return;
    }
    uint64_t seed = uint64_t(wall_millis()) ^ (uint64_t(now_micros()) << 17) ^ uint64_t(::getpid());
    uint64_t s = seed | 1;
    while (got < n) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        size_t chunk = std::min<size_t>(8, n - got);
        for (size_t i = 0; i < chunk; ++i) p[got + i] = uint8_t(s >> (i * 8));
        got += chunk;
    }
}

// =============================================================================
// Encodings
// =============================================================================
namespace {
const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
const char kB64Url[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
}  // namespace

std::string base64_encode(const uint8_t* data, size_t n) {
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    size_t i = 0;
    while (i + 2 < n) {
        uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i+1]) << 8) | data[i+2];
        out.push_back(kB64[(v >> 18) & 63]); out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(kB64[(v >> 6) & 63]);  out.push_back(kB64[v & 63]);
        i += 3;
    }
    if (i + 1 == n) {
        uint32_t v = uint32_t(data[i]) << 16;
        out.push_back(kB64[(v >> 18) & 63]); out.push_back(kB64[(v >> 12) & 63]);
        out.push_back('='); out.push_back('=');
    } else if (i + 2 == n) {
        uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i+1]) << 8);
        out.push_back(kB64[(v >> 18) & 63]); out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(kB64[(v >> 6) & 63]);  out.push_back('=');
    }
    return out;
}

std::string base64_encode(const Bytes& b) { return base64_encode(b.data(), b.size()); }

std::string base64url_encode(const Bytes& b) {
    std::string s = base64_encode(b);
    for (char& c : s) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    while (!s.empty() && s.back() == '=') s.pop_back();
    return s;
}

bool base64_decode(std::string_view in, Bytes& out) {
    static int8_t table[256];
    static bool init = false;
    if (!init) {
        std::memset(table, -1, sizeof(table));
        for (int i = 0; i < 64; ++i) table[uint8_t(kB64[i])] = int8_t(i);
        table[uint8_t('-')] = 62; table[uint8_t('_')] = 63;
        init = true;
    }
    out.clear();
    uint32_t buf = 0;
    int bits = 0;
    for (char ch : in) {
        if (ch == '=' || ch == '\n' || ch == '\r' || ch == ' ' || ch == '\t') continue;
        int8_t v = table[uint8_t(ch)];
        if (v < 0) return false;
        buf = (buf << 6) | uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(uint8_t(buf >> bits));
        }
    }
    return true;
}

std::string hex_encode(const uint8_t* data, size_t n) {
    static const char* hx = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) { s.push_back(hx[data[i] >> 4]); s.push_back(hx[data[i] & 15]); }
    return s;
}
std::string hex_encode(const Bytes& b) { return hex_encode(b.data(), b.size()); }

bool hex_decode(std::string_view in, Bytes& out) {
    out.clear();
    if (in.size() % 2) return false;
    auto val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < in.size(); i += 2) {
        int hi = val(in[i]), lo = val(in[i+1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(uint8_t((hi << 4) | lo));
    }
    return true;
}

}  // namespace crypto
}  // namespace lca
