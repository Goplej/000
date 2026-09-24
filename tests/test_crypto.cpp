// =============================================================================
//  tests/test_crypto.cpp  --  known-answer tests for the primitive layer
// -----------------------------------------------------------------------------
//  Every vector below is taken from a public specification:
//    * FIPS 180-4            (SHA-2)
//    * RFC 4231              (HMAC-SHA2)
//    * RFC 5869              (HKDF)
//    * NIST SP 800-38D       (AES-GCM)
//    * RFC 8439              (ChaCha20 / Poly1305 / AEAD)
//    * RFC 7748              (X25519, including the 1000-iteration chain)
// =============================================================================
#include "lca/crypto.h"

#include <cstdio>
#include <string>

using namespace lca;
using namespace lca::crypto;

namespace {

int g_pass = 0;
int g_fail = 0;

void check(const char* name, const std::string& got, const std::string& want, bool quiet = true) {
    if (got == want) {
        ++g_pass;
        std::printf("  [ok]   %s\n", name);
    } else {
        ++g_fail;
        std::printf("  [FAIL] %s\n", name);
        std::printf("         got  %s\n", got.c_str());
        std::printf("         want %s\n", want.c_str());
    }
    (void)quiet;
}

void check_true(const char* name, bool cond) {
    check(name, cond ? "true" : "false", "true");
}

void section(const char* title) { std::printf("\n== %s ==\n", title); }

}  // namespace

int main() {
    std::printf("local-claude-code-agent :: crypto known-answer tests\n");

    section("SHA-2 (FIPS 180-4)");
    check("sha256(\"abc\")", hex_encode(Sha256::one_shot("abc")),
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    check("sha512(\"abc\")", hex_encode(Sha512::one_shot("abc", 3)),
          "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
          "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    check("sha384(\"abc\")", hex_encode(Sha384::one_shot("abc", 3)),
          "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed"
          "8086072ba1e7cc2358baeca134c825a7");
    // Multi-block and padding-boundary cases.
    check("sha256(448-bit)", hex_encode(Sha256::one_shot(
              std::string("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))),
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    check("sha256(56 bytes)", hex_encode(Sha256::one_shot(std::string(56, 'a'))),
          "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");

    section("HMAC (RFC 4231)");
    {
        Bytes k(20, 0x0b);
        check("hmac-sha256 tc1", hex_encode(hmac_sha256(k, std::string_view("Hi There"))),
              "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
        Bytes k2(131, 0xaa);
        check("hmac-sha256 tc6", hex_encode(hmac_sha256(k2, std::string_view(
                  "Test Using Larger Than Block-Size Key - Hash Key First"))),
              "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
    }

    section("HKDF (RFC 5869)");
    {
        Bytes ikm(22, 0x0b), salt, info;
        hex_decode("000102030405060708090a0b0c", salt);
        hex_decode("f0f1f2f3f4f5f6f7f8f9", info);
        Bytes prk = hkdf_extract_sha256(salt, ikm);
        check("hkdf-extract", hex_encode(prk),
              "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5");
        check("hkdf-expand", hex_encode(hkdf_expand_sha256(prk, info, 42)),
              "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
              "34007208d5b887185865");
    }

    section("AES-GCM (NIST SP 800-38D)");
    {
        uint8_t zero16[16] = {0}, zero12[12] = {0}, tag[16], ct[64], back[64], pt[64] = {0};
        aes128_gcm_encrypt(zero16, zero12, nullptr, 0, nullptr, 0, ct, tag);
        check("aes128-gcm empty tag", hex_encode(tag, 16), "58e2fccefa7e3061367f1d57a4e7455a");
        aes128_gcm_encrypt(zero16, zero12, nullptr, 0, pt, 16, ct, tag);
        check("aes128-gcm ct", hex_encode(ct, 16), "0388dace60b6a392f328c2b971b2fe78");
        check("aes128-gcm tag", hex_encode(tag, 16), "ab6e47d42cec13bdf53a67b21257bddf");
        check_true("aes128-gcm decrypt", aes128_gcm_decrypt(zero16, zero12, nullptr, 0, ct, 16, tag, back) &&
                                          std::memcmp(back, pt, 16) == 0);
        tag[0] ^= 1;
        check_true("aes128-gcm rejects bad tag",
                   !aes128_gcm_decrypt(zero16, zero12, nullptr, 0, ct, 16, tag, back));
        uint8_t k32[32] = {0};
        aes256_gcm_encrypt(k32, zero12, nullptr, 0, nullptr, 0, ct, tag);
        check("aes256-gcm empty tag", hex_encode(tag, 16), "530f8afbc74536b9a963b4f1c4cb738b");
    }

    section("ChaCha20-Poly1305 (RFC 8439)");
    {
        Bytes key;
        hex_decode("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b", key);
        uint8_t tag[16];
        Bytes msg = to_bytes(std::string_view("Cryptographic Forum Research Group"));
        poly1305_mac(key.data(), msg.data(), msg.size(), tag);
        check("poly1305 mac", hex_encode(tag, 16), "a8061dc1305136c6c22b8baf0c0127a9");

        Bytes aead_key;
        hex_decode("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f", aead_key);
        uint8_t nonce[12] = {0x07, 0x00, 0x00, 0x00, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47};
        Bytes aad;
        hex_decode("50515253c0c1c2c3c4c5c6c7", aad);
        std::string plain =
            "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the "
            "future, sunscreen would be it.";
        Bytes out(plain.size()), round(plain.size());
        chacha20_poly1305_encrypt(aead_key.data(), nonce, aad.data(), aad.size(),
                                  reinterpret_cast<const uint8_t*>(plain.data()), plain.size(),
                                  out.data(), tag);
        check("chacha20-poly1305 ct", hex_encode(out),
              "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6"
              "3dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b36"
              "92ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc"
              "3ff4def08e4b7a9de576d26586cec64b6116");
        check("chacha20-poly1305 tag", hex_encode(tag, 16), "1ae10b594f09e26a7e902ecbd0600691");
        check_true("chacha20-poly1305 roundtrip",
                   chacha20_poly1305_decrypt(aead_key.data(), nonce, aad.data(), aad.size(),
                                             out.data(), out.size(), tag, round.data()) &&
                       to_string(round) == plain);
    }

    section("X25519 (RFC 7748)");
    {
        Bytes s1, s2;
        hex_decode("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", s1);
        hex_decode("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", s2);
        uint8_t base[32] = {9}, pub1[32], pub2[32], sh1[32], sh2[32];
        x25519(pub1, s1.data(), base);
        x25519(pub2, s2.data(), base);
        check("x25519 pubkey 1", hex_encode(pub1, 32),
              "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
        check("x25519 pubkey 2", hex_encode(pub2, 32),
              "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
        x25519(sh1, s1.data(), pub2);
        x25519(sh2, s2.data(), pub1);
        check("x25519 shared 1", hex_encode(sh1, 32),
              "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
        check("x25519 shared 2", hex_encode(sh2, 32), hex_encode(sh1, 32));

        Bytes k(32, 0), u(32, 0);
        k[0] = 9;
        u[0] = 9;
        uint8_t res[32];
        bool first_ok = false;
        for (int i = 0; i < 1000; ++i) {
            x25519(res, k.data(), u.data());
            u = k;
            k.assign(res, res + 32);
            if (i == 0) {
                first_ok = hex_encode(k) ==
                           "422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079";
            }
        }
        check_true("x25519 1 iteration", first_ok);
        check("x25519 1000 iterations", hex_encode(k),
              "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51");
    }

    section("TLS 1.3 key schedule (RFC 8448 traces)");
    {
        // RFC 8448 section 3: Simple 1-RTT Handshake.
        Bytes zero(32, 0);
        Bytes early = hkdf_extract_sha256(zero, zero);
        check("early secret", hex_encode(early),
              "33ad0a1c607ec03b09e6cd9893680ce210adf300aa1f2660e1b22e10f170f92a");
        Bytes derived = hkdf_expand_label(early, "derived", Sha256::one_shot(nullptr, 0), 32);
        check("derived secret", hex_encode(derived),
              "6f2615a108c702c5678f54fc9dbab69716c076189c48250cebeac3576c3611ba");
        Bytes shared;
        hex_decode("8bd4054fb55b9d63fdfbacf9f04b9f0d35e6d63f537563efd46272900f89492d", shared);
        Bytes handshake = hkdf_extract_sha256(derived, shared);
        check("handshake secret", hex_encode(handshake),
              "1dc826e93606aa6fdc0aadc12f741b01046aa6b99f691ed221a9f0ca043fbeac");
    }

    section("encodings and CSPRNG");
    {
        Bytes r1 = random_bytes(32), r2 = random_bytes(32);
        check_true("random_bytes differs", r1 != r2);
        Bytes enc = random_bytes(37);
        std::string b64 = base64_encode(enc);
        Bytes dec;
        check_true("base64 roundtrip", base64_decode(b64, dec) && dec == enc);
        std::string u64s = base64url_encode(enc);
        check_true("base64url has no padding",
                   u64s.find('=') == std::string::npos && u64s.find('+') == std::string::npos);
        check_true("hex roundtrip", hex_decode(hex_encode(enc), dec) && dec == enc);
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
