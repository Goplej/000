// =============================================================================
//  src/crypto_asym.cpp  --  BigInt, ECDSA (P-256/P-384), RSA signature checks
// -----------------------------------------------------------------------------
//  Written against FIPS 186-4 / SEC1 / RFC 8017 with no external bignum or
//  crypto dependency.  Verification only: this code proves that a peer holds a
//  private key, it never signs with one.
// =============================================================================
#include "lca/crypto.h"

#include <algorithm>

namespace lca {
namespace crypto {

// =============================================================================
// BigInt -- fixed capacity 128 x 64 bit limbs (8192 bits)
// =============================================================================
BigInt BigInt::from_bytes_be(const uint8_t* p, size_t n) {
    BigInt r;
    size_t limbs = (n + 7) / 8;
    for (size_t i = 0; i < limbs && i < kMaxLimbs; ++i) {
        size_t end = n - i * 8;                  // exclusive, counted from the right
        size_t begin = end > 8 ? end - 8 : 0;
        uint64_t v = 0;
        for (size_t j = begin; j < end; ++j) v = (v << 8) | p[j];
        r.limbs_[i] = v;
    }
    return r;
}

BigInt BigInt::from_bytes_le(const uint8_t* p, size_t n) {
    BigInt r;
    for (size_t i = 0; i < n && i / 8 < kMaxLimbs; ++i) {
        r.limbs_[i / 8] |= uint64_t(p[i]) << ((i % 8) * 8);
    }
    return r;
}

Bytes BigInt::to_bytes_be(size_t fixed_len) const {
    size_t bytes = fixed_len ? fixed_len : (bit_length() + 7) / 8;
    Bytes out(bytes, 0);
    for (size_t i = 0; i < bytes; ++i) {
        size_t base_bit = (bytes - 1 - i) * 8;
        uint8_t v = 0;
        for (int b = 0; b < 8; ++b) {
            if (bit(base_bit + size_t(b))) v |= uint8_t(1u << b);
        }
        out[i] = v;
    }
    return out;
}

size_t BigInt::limb_count() const {
    for (size_t i = kMaxLimbs; i-- > 0;) if (limbs_[i]) return i + 1;
    return 0;
}
bool   BigInt::is_zero() const { return limb_count() == 0; }
size_t BigInt::bit_length() const {
    size_t n = limb_count();
    if (n == 0) return 0;
    uint64_t top = limbs_[n - 1];
    size_t bits = 0;
    while (top) { ++bits; top >>= 1; }
    return (n - 1) * 64 + bits;
}
bool BigInt::bit(size_t i) const {
    return i / 64 < kMaxLimbs && ((limbs_[i / 64] >> (i % 64)) & 1u) != 0;
}
void BigInt::set_bit(size_t i) {
    if (i / 64 < kMaxLimbs) limbs_[i / 64] |= uint64_t(1) << (i % 64);
}
int BigInt::compare(const BigInt& o) const {
    size_t an = limb_count(), bn = o.limb_count();
    if (an != bn) return an < bn ? -1 : 1;
    for (size_t i = an; i-- > 0;) {
        if (limbs_[i] != o.limbs_[i]) return limbs_[i] < o.limbs_[i] ? -1 : 1;
    }
    return 0;
}
void BigInt::shift_right_one() {
    uint64_t carry = 0;
    for (size_t i = kMaxLimbs; i-- > 0;) {
        uint64_t next = limbs_[i] & 1u;
        limbs_[i] = (limbs_[i] >> 1) | (carry << 63);
        carry = next;
    }
}
void BigInt::shift_left_one() {
    uint64_t carry = 0;
    for (size_t i = 0; i < kMaxLimbs; ++i) {
        uint64_t next = limbs_[i] >> 63;
        limbs_[i] = (limbs_[i] << 1) | carry;
        carry = next;
    }
}
void BigInt::shift_left_bits(size_t n) {
    while (n >= 64) { shift_left_bits(64); n -= 64; }
    if (!n) return;
    uint64_t carry = 0;
    for (size_t i = 0; i < kMaxLimbs; ++i) {
        uint64_t next = limbs_[i] >> (64 - n);
        limbs_[i] = (limbs_[i] << n) | carry;
        carry = next;
    }
}
BigInt BigInt::add(const BigInt& a, const BigInt& b) {
    BigInt r;
    uint64_t carry = 0;
    for (size_t i = 0; i < kMaxLimbs; ++i) {
        unsigned __int128 s = (unsigned __int128)a.limbs_[i] + b.limbs_[i] + carry;
        r.limbs_[i] = uint64_t(s);
        carry = uint64_t(s >> 64);
    }
    return r;
}
BigInt BigInt::sub(const BigInt& a, const BigInt& b) {
    BigInt r;
    uint64_t borrow = 0;
    for (size_t i = 0; i < kMaxLimbs; ++i) {
        unsigned __int128 d = (unsigned __int128)a.limbs_[i] - b.limbs_[i] - borrow;
        r.limbs_[i] = uint64_t(d);
        borrow = uint64_t((d >> 64) & 1);
    }
    return r;
}
BigInt BigInt::mul(const BigInt& a, const BigInt& b) {
    BigInt r;
    size_t an = a.limb_count(), bn = b.limb_count();
    if (an == 0 || bn == 0) return r;
    if (an + bn > kMaxLimbs) {
        // Keep the low half; callers always reduce mod p/n before multiplying.
        an = std::min(an, kMaxLimbs);
        bn = std::min(bn, kMaxLimbs - an + 1);
    }
    for (size_t i = 0; i < an; ++i) {
        uint64_t carry = 0;
        for (size_t j = 0; j < bn && i + j < kMaxLimbs; ++j) {
            unsigned __int128 cur = (unsigned __int128)a.limbs_[i] * b.limbs_[j] +
                                    r.limbs_[i + j] + carry;
            r.limbs_[i + j] = uint64_t(cur);
            carry = uint64_t(cur >> 64);
        }
        for (size_t k = i + bn; carry && k < kMaxLimbs; ++k) {
            unsigned __int128 s = (unsigned __int128)r.limbs_[k] + carry;
            r.limbs_[k] = uint64_t(s);
            carry = uint64_t(s >> 64);
        }
    }
    return r;
}
void BigInt::divmod(const BigInt& a, const BigInt& b, BigInt& q, BigInt& r) {
    q = BigInt();
    r = BigInt();
    if (b.is_zero()) return;
    const size_t b_bits = b.bit_length();
    for (size_t i = a.bit_length(); i-- > 0;) {
        // Fast path: no shift is necessary while the accumulator is still
        // smaller than the divisor's minimum possible shifted value.
        if (r.limb_count() * 64 + 64 < b_bits && !a.bit(i)) { r.shift_left_one(); continue; }
        r.shift_left_one();
        if (a.bit(i)) r.limbs_[0] |= 1u;
        if (r.compare(b) >= 0) {
            r = sub(r, b);
            q.set_bit(i);
        }
    }
}
BigInt BigInt::mod(const BigInt& a, const BigInt& m) {
    BigInt q, r;
    divmod(a, m, q, r);
    return r;
}
BigInt BigInt::mod_exp(const BigInt& base, const BigInt& exp, const BigInt& mod) {
    BigInt result = BigInt::mod(BigInt(uint64_t(1)), mod);
    if (mod.limb_count() == 1 && mod.limbs_[0] == 1) return BigInt();
    BigInt b = BigInt::mod(base, mod);
    for (size_t i = exp.bit_length(); i-- > 0;) {
        result = BigInt::mod(mul(result, result), mod);
        if (exp.bit(i)) result = BigInt::mod(mul(result, b), mod);
    }
    return result;
}
uint32_t BigInt::add_limb(BigInt& a, uint64_t v) {
    unsigned __int128 s = (unsigned __int128)a.limbs_[0] + v;
    a.limbs_[0] = uint64_t(s);
    uint64_t carry = uint64_t(s >> 64);
    for (size_t i = 1; carry && i < kMaxLimbs; ++i) {
        unsigned __int128 t = (unsigned __int128)a.limbs_[i] + carry;
        a.limbs_[i] = uint64_t(t);
        carry = uint64_t(t >> 64);
    }
    return uint32_t(carry);
}
bool BigInt::mod_inverse(const BigInt& a, const BigInt& m, BigInt& out) {
    // Binary extended GCD (HAC 14.61); requires an odd modulus, true for every
    // NIST prime curve and every RSA modulus handled here.
    if (m.is_zero() || (m.limbs_[0] & 1u) == 0) return false;
    BigInt u = mod(a, m);
    if (u.is_zero()) return false;
    BigInt v = m;
    BigInt x1(uint64_t(1)), x2;
    auto is_one = [](const BigInt& x) { return x.limb_count() == 1 && x.limbs_[0] == 1u; };
    long long guard = 0;
    while (!is_one(u) && !is_one(v)) {
        if (++guard > 4ll * static_cast<long long>(kMaxLimbs) * 64) return false;
        while ((u.limbs_[0] & 1u) == 0) {
            u.shift_right_one();
            if ((x1.limbs_[0] & 1u) == 0) x1.shift_right_one();
            else { x1 = add(x1, m); x1.shift_right_one(); }
        }
        while ((v.limbs_[0] & 1u) == 0) {
            v.shift_right_one();
            if ((x2.limbs_[0] & 1u) == 0) x2.shift_right_one();
            else { x2 = add(x2, m); x2.shift_right_one(); }
        }
        if (u.compare(v) >= 0) {
            u = sub(u, v);
            x1 = x1.compare(x2) >= 0 ? sub(x1, x2) : sub(add(x1, m), x2);
        } else {
            v = sub(v, u);
            x2 = x2.compare(x1) >= 0 ? sub(x2, x1) : sub(add(x2, m), x1);
        }
        if (u.is_zero() || v.is_zero()) return false;
    }
    out = is_one(u) ? mod(x1, m) : mod(x2, m);
    return true;
}

// =============================================================================
// Prime field curves (short Weierstrass, a = -3)
// =============================================================================
namespace {

constexpr const char* kP256p  = "ffffffff00000001000000000000000000000000ffffffffffffffffffffffff";
constexpr const char* kP256b  = "5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b";
constexpr const char* kP256n  = "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551";
constexpr const char* kP256gx = "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296";
constexpr const char* kP256gy = "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5";

constexpr const char* kP384p  = "fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff";
constexpr const char* kP384b  = "b3312fa7e23ee7e4988e056be3f82d19181d9c6efe8141120314088f5013875ac656398d8a2ed19d2a85c8edd3ec2aef";
constexpr const char* kP384n  = "ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973";
constexpr const char* kP384gx = "aa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a385502f25dbf55296c3a545e3872760ab7";
constexpr const char* kP384gy = "3617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da3113b5f0b8c00a60b1ce1d7e819d7a431d7c90ea0e5f";

struct CurveData {
    size_t bits;
    BigInt p, b, n, gx, gy;
};

BigInt big_from_hex(const char* hex) {
    Bytes b;
    hex_decode(hex, b);
    return BigInt::from_bytes_be(b.data(), b.size());
}

const CurveData& curve_of(Curve which) {
    static const CurveData p256{256, big_from_hex(kP256p), big_from_hex(kP256b), big_from_hex(kP256n),
                                big_from_hex(kP256gx), big_from_hex(kP256gy)};
    static const CurveData p384{384, big_from_hex(kP384p), big_from_hex(kP384b), big_from_hex(kP384n),
                                big_from_hex(kP384gx), big_from_hex(kP384gy)};
    return which == Curve::P256 ? p256 : p384;
}

struct Jacobian { BigInt X, Y, Z; };

Jacobian jac_infinity() { return {BigInt(uint64_t(0)), BigInt(uint64_t(1)), BigInt(uint64_t(0))}; }
bool jac_is_infinity(const Jacobian& p) { return p.Z.is_zero(); }

// dbl-2001-b (a = -3)
// Modular subtraction: (a - b) mod m.  BigInt::sub wraps at 2^(64*kMaxLimbs),
// so a bare sub() must never be fed to mod() when a < b.
static BigInt mod_sub(const BigInt& a, const BigInt& b, const BigInt& m) {
    if (a.compare(b) >= 0) return BigInt::sub(a, b);
    return BigInt::sub(BigInt::add(a, m), b);
}

Jacobian jac_double(const CurveData& c, const Jacobian& pt) {
    if (jac_is_infinity(pt) || pt.Y.is_zero()) return jac_infinity();
    // dbl-2007-bl
    BigInt delta = BigInt::mod(BigInt::mul(pt.Z, pt.Z), c.p);            // Z^2
    BigInt gamma = BigInt::mod(BigInt::mul(pt.Y, pt.Y), c.p);            // Y^2
    BigInt beta  = BigInt::mod(BigInt::mul(pt.X, gamma), c.p);           // X*Y^2
    BigInt t1 = mod_sub(pt.X, delta, c.p);
    BigInt t2 = BigInt::mod(BigInt::add(pt.X, delta), c.p);
    BigInt alpha = BigInt::mod(BigInt::mul(BigInt::add(BigInt::add(t1, t1), t1), t2), c.p);
    BigInt eight_beta = BigInt::mod(BigInt::mul(beta, BigInt(uint64_t(8))), c.p);
    BigInt X3 = BigInt::mod(mod_sub(BigInt::mod(BigInt::mul(alpha, alpha), c.p), eight_beta, c.p), c.p);
    BigInt yz = BigInt::mod(BigInt::add(pt.Y, pt.Z), c.p);
    BigInt Z3 = BigInt::mod(mod_sub(mod_sub(BigInt::mod(BigInt::mul(yz, yz), c.p), gamma, c.p),
                                   delta, c.p), c.p);
    BigInt four_beta = BigInt::mod(BigInt::mul(beta, BigInt(uint64_t(4))), c.p);
    BigInt t = mod_sub(four_beta, X3, c.p);
    BigInt g2 = BigInt::mod(BigInt::mul(gamma, gamma), c.p);
    BigInt eight_g2 = BigInt::mod(BigInt::mul(g2, BigInt(uint64_t(8))), c.p);
    BigInt Y3 = BigInt::mod(mod_sub(BigInt::mod(BigInt::mul(alpha, t), c.p), eight_g2, c.p), c.p);
    return {X3, Y3, Z3};
}

// add-2007-bl
Jacobian jac_add(const CurveData& c, const Jacobian& a, const Jacobian& b) {
    if (jac_is_infinity(a)) return b;
    if (jac_is_infinity(b)) return a;
    auto mm = [&](const BigInt& x, const BigInt& y) { return BigInt::mod(BigInt::mul(x, y), c.p); };
    auto aa = [&](const BigInt& x, const BigInt& y) { return BigInt::mod(BigInt::add(x, y), c.p); };
    auto ss = [&](const BigInt& x, const BigInt& y) { return mod_sub(x, y, c.p); };
    BigInt Z1Z1 = mm(a.Z, a.Z);
    BigInt Z2Z2 = mm(b.Z, b.Z);
    BigInt U1 = mm(a.X, Z2Z2);
    BigInt U2 = mm(b.X, Z1Z1);
    BigInt S1 = mm(a.Y, mm(b.Z, Z2Z2));
    BigInt S2 = mm(b.Y, mm(a.Z, Z1Z1));
    if (U1.compare(U2) == 0) {
        if (S1.compare(S2) == 0) return jac_double(c, a);
        return jac_infinity();
    }
    BigInt H = ss(U2, U1);
    BigInt twoH = aa(H, H);
    BigInt I = mm(twoH, twoH);
    BigInt J = mm(H, I);
    BigInt r = aa(ss(S2, S1), ss(S2, S1));
    BigInt V = mm(U1, I);
    BigInt X3 = ss(ss(mm(r, r), J), aa(V, V));
    BigInt s1j = mm(S1, J);
    BigInt Y3 = ss(mm(r, ss(V, X3)), aa(s1j, s1j));
    BigInt zs = aa(a.Z, b.Z);
    BigInt Z3 = mm(ss(ss(mm(zs, zs), Z1Z1), Z2Z2), H);
    return {X3, Y3, Z3};
}

Jacobian jac_mul(const CurveData& c, const Jacobian& pt, const BigInt& k) {
    Jacobian result = jac_infinity();
    Jacobian base = pt;
    size_t bits = k.bit_length();
    for (size_t i = 0; i < bits; ++i) {
        if (k.bit(i)) result = jac_add(c, result, base);
        base = jac_double(c, base);
    }
    return result;
}

bool jac_to_affine(const CurveData& c, const Jacobian& pt, BigInt& x, BigInt& y) {
    if (jac_is_infinity(pt)) return false;
    BigInt zinv, zinv2, zinv3;
    if (!BigInt::mod_inverse(pt.Z, c.p, zinv)) return false;
    zinv2 = BigInt::mod(BigInt::mul(zinv, zinv), c.p);
    zinv3 = BigInt::mod(BigInt::mul(zinv2, zinv), c.p);
    x = BigInt::mod(BigInt::mul(pt.X, zinv2), c.p);
    y = BigInt::mod(BigInt::mul(pt.Y, zinv3), c.p);
    return true;
}

// MGF1 (RFC 8017 appendix B.2.1)
Bytes mgf1(size_t hash_len, const Bytes& seed, size_t mask_len) {
    Bytes mask;
    mask.reserve(mask_len + hash_len);
    for (uint32_t counter = 0; mask.size() < mask_len; ++counter) {
        Bytes input = seed;
        append_u32(input, counter);
        Bytes block;
        if (hash_len == 48)      block = Sha384::one_shot(input.data(), input.size());
        else if (hash_len == 64) block = Sha512::one_shot(input.data(), input.size());
        else                     block = Sha256::one_shot(input.data(), input.size());
        mask.insert(mask.end(), block.begin(), block.end());
    }
    mask.resize(mask_len);
    return mask;
}

Bytes hash_with_len(size_t hash_len, const Bytes& data) {
    if (hash_len == 48) return Sha384::one_shot(data.data(), data.size());
    if (hash_len == 64) return Sha512::one_shot(data.data(), data.size());
    return Sha256::one_shot(data.data(), data.size());
}

// DigestInfo prefixes from RFC 8017 section 9.2.
const char* pkcs1_prefix(size_t hash_len) {
    switch (hash_len) {
        case 32: return "3031300d060960864801650304020105000420";     // SHA-256
        case 48: return "3041300d060960864801650304020205000430";     // SHA-384
        case 64: return "3051300d060960864801650304020305000440";     // SHA-512
        case 20: return "3021300906052b0e03021a05000414";              // SHA-1
        default: return nullptr;
    }
}

// Minimal DER walk used for the ECDSA-Sig-Value structure.
bool read_der_integer(const uint8_t* data, size_t size, size_t& pos, Bytes& out) {
    if (pos + 2 > size || data[pos] != 0x02) return false;
    size_t len = data[pos + 1];
    size_t header = 2;
    if (len & 0x80) {
        size_t nbytes = len & 0x7F;
        if (nbytes == 0 || nbytes > 2 || pos + 2 + nbytes > size) return false;
        len = 0;
        for (size_t i = 0; i < nbytes; ++i) len = (len << 8) | data[pos + 2 + i];
        header = 2 + nbytes;
    }
    if (pos + header + len > size) return false;
    const uint8_t* body = data + pos + header;
    size_t skip = 0;
    while (skip + 1 < len && body[skip] == 0x00) ++skip;      // strip leading zeros
    out.assign(body + skip, body + len);
    pos += header + len;
    return !out.empty();
}

}  // namespace

// =============================================================================
// ECDSA verification (FIPS 186-4 section 6.4.2)
// =============================================================================
bool ecdsa_verify(Curve curve, const Bytes& pub_x, const Bytes& pub_y,
                  const Bytes& digest, const Bytes& sig_der) {
    const CurveData& c = curve_of(curve);
    const size_t field_bytes = c.bits / 8;
    if (pub_x.empty() || pub_y.empty() || pub_x.size() > field_bytes || pub_y.size() > field_bytes)
        return false;
    if (digest.empty()) return false;

    // --- decode DER ECDSA-Sig-Value: SEQUENCE { INTEGER r, INTEGER s } ----
    size_t pos = 0;
    if (sig_der.size() < 8 || sig_der[0] != 0x30) return false;
    size_t seq_len = sig_der[1];
    size_t seq_header = 2;
    if (seq_len & 0x80) {
        size_t nbytes = seq_len & 0x7F;
        if (nbytes == 0 || nbytes > 2 || sig_der.size() < 2 + nbytes) return false;
        seq_len = 0;
        for (size_t i = 0; i < nbytes; ++i) seq_len = (seq_len << 8) | sig_der[2 + i];
        seq_header = 2 + nbytes;
    }
    if (sig_der.size() < seq_header + seq_len) return false;
    pos = seq_header;
    Bytes r_bytes, s_bytes;
    const uint8_t* p = sig_der.data();
    if (!read_der_integer(p, seq_header + seq_len, pos, r_bytes)) return false;
    if (!read_der_integer(p, seq_header + seq_len, pos, s_bytes)) return false;

    BigInt r = BigInt::from_bytes_be(r_bytes.data(), r_bytes.size());
    BigInt s = BigInt::from_bytes_be(s_bytes.data(), s_bytes.size());
    if (r.is_zero() || s.is_zero() || r.compare(c.n) >= 0 || s.compare(c.n) >= 0) return false;

    BigInt Qx = BigInt::from_bytes_be(pub_x.data(), pub_x.size());
    BigInt Qy = BigInt::from_bytes_be(pub_y.data(), pub_y.size());
    if (Qx.compare(c.p) >= 0 || Qy.compare(c.p) >= 0) return false;

    // Reject points that are not on the curve: y^2 == x^3 - 3x + b (mod p).
    {
        BigInt y2 = BigInt::mod(BigInt::mul(Qy, Qy), c.p);
        BigInt x2 = BigInt::mod(BigInt::mul(Qx, Qx), c.p);
        BigInt x3 = BigInt::mod(BigInt::mul(x2, Qx), c.p);
        BigInt three_x = BigInt::mod(BigInt::mul(Qx, BigInt(uint64_t(3))), c.p);
        BigInt rhs = mod_sub(x3, three_x, c.p);
        rhs = BigInt::mod(BigInt::add(rhs, c.b), c.p);
        if (y2.compare(rhs) != 0) return false;
    }

    // e = leftmost n bits of the digest
    BigInt e;
    {
        Bytes mantissa = digest;
        if (mantissa.size() * 8 > c.bits) {
            size_t keep = (c.bits + 7) / 8;
            mantissa.resize(keep);
            uint8_t extra = uint8_t(mantissa.size() * 8 - c.bits);
            if (extra) mantissa[0] >>= extra;
        }
        e = BigInt::from_bytes_be(mantissa.data(), mantissa.size());
    }

    BigInt w;
    if (!BigInt::mod_inverse(s, c.n, w)) return false;
    BigInt u1 = BigInt::mod(BigInt::mul(e, w), c.n);
    BigInt u2 = BigInt::mod(BigInt::mul(r, w), c.n);

    Jacobian g{BigInt::from_bytes_be(c.gx.to_bytes_be(field_bytes).data(), field_bytes),
               BigInt::from_bytes_be(c.gy.to_bytes_be(field_bytes).data(), field_bytes),
               BigInt(uint64_t(1))};
    Jacobian q{Qx, Qy, BigInt(uint64_t(1))};
    Jacobian point = jac_add(c, jac_mul(c, g, u1), jac_mul(c, q, u2));
    BigInt x, y;
    if (!jac_to_affine(c, point, x, y)) return false;
    BigInt v = BigInt::mod(x, c.n);
    return v.compare(r) == 0;
}

// =============================================================================
// RSA signature verification (RFC 8017)
// =============================================================================
bool rsa_verify(RsaPadding padding, const Bytes& modulus, const Bytes& exponent,
                const Bytes& digest, const Bytes& sig, size_t hash_len) {
    if (modulus.empty() || exponent.empty() || sig.empty() || digest.empty()) return false;
    if (sig.size() > modulus.size()) return false;
    size_t mod_bytes = modulus.size();
    BigInt n = BigInt::from_bytes_be(modulus.data(), modulus.size());
    BigInt e = BigInt::from_bytes_be(exponent.data(), exponent.size());
    BigInt m = BigInt::from_bytes_be(sig.data(), sig.size());
    if (m.compare(n) >= 0) return false;
    BigInt s = BigInt::mod_exp(m, e, n);
    Bytes em = s.to_bytes_be(mod_bytes);
    if (em.size() != mod_bytes) return false;

    if (padding == RsaPadding::Pkcs1v15) {
        const char* prefix_hex = pkcs1_prefix(hash_len);
        if (!prefix_hex) return false;
        Bytes prefix;
        hex_decode(prefix_hex, prefix);
        if (em.size() < 2 + prefix.size() + digest.size()) return false;
        if (em[0] != 0x00 || em[1] != 0x01) return false;
        size_t i = 2;
        while (i < em.size() && em[i] == 0xFF) ++i;
        if (i - 2 < 8) return false;                       // at least 8 padding bytes
        if (i >= em.size() || em[i] != 0x00) return false;
        ++i;
        Bytes t(em.begin() + long(i), em.end());
        Bytes expected = prefix;
        expected.insert(expected.end(), digest.begin(), digest.end());
        return ct_equal(t, expected);
    }

    // EMSA-PSS-VERIFY (RFC 8017 section 9.1.2)
    size_t mod_bits = 0;
    {
        size_t idx = 0;
        while (idx < modulus.size() && modulus[idx] == 0) ++idx;
        if (idx >= modulus.size()) return false;
        mod_bits = (modulus.size() - idx - 1) * 8;
        uint8_t top = modulus[idx];
        while (top) { ++mod_bits; top >>= 1; }
    }
    size_t em_bits = mod_bits - 1;
    size_t em_len = (em_bits + 7) / 8;
    if (mod_bytes < em_len) return false;
    em.erase(em.begin(), em.begin() + long(mod_bytes - em_len));
    if (em.size() < hash_len + 2) return false;
    if (em.back() != 0xBC) return false;

    Bytes h(em.end() - long(hash_len) - 1, em.end() - 1);
    size_t db_len = em_len - hash_len - 1;
    Bytes masked_db(em.begin(), em.begin() + long(db_len));
    Bytes db_mask = mgf1(hash_len, h, db_len);
    Bytes db(db_len);
    for (size_t i = 0; i < db_len; ++i) db[i] = uint8_t(masked_db[i] ^ db_mask[i]);
    size_t bits_to_clear = 8 * em_len - em_bits;
    if (bits_to_clear > 0 && bits_to_clear < 8) db[0] &= uint8_t(0xFF >> bits_to_clear);
    else if (bits_to_clear >= 8) db[0] = 0;

    size_t sep = 0;
    while (sep < db.size() && db[sep] == 0x00) ++sep;
    if (sep >= db.size() || db[sep] != 0x01) return false;
    Bytes salt(db.begin() + long(sep + 1), db.end());
    Bytes m_prime(8, 0x00);
    m_prime.insert(m_prime.end(), digest.begin(), digest.end());
    m_prime.insert(m_prime.end(), salt.begin(), salt.end());
    Bytes h_prime = hash_with_len(hash_len, m_prime);
    return ct_equal(h_prime, h);
}


}  // namespace crypto
}  // namespace lca
