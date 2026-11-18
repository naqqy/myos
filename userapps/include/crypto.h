#ifndef NAQ_CRYPTO_H
#define NAQ_CRYPTO_H

#include "naqos.h"

/*
 * The cryptographic building blocks TLS 1.3 needs.
 *
 * These are straightforward implementations written for clarity. They
 * have not been reviewed or hardened against side channels, so treat the
 * result as a learning-grade TLS, not something to protect real secrets.
 */

/*
 * SHA-256: a 32-byte fingerprint of any amount of data.
 */
typedef struct sha256_ctx {
    u32 state[8];
    u64 length;
    u8 block[64];
    u32 block_len;
} sha256_ctx_t;

void sha256_init(sha256_ctx_t* ctx);
void sha256_update(sha256_ctx_t* ctx, const void* data, u32 length);
void sha256_final(sha256_ctx_t* ctx, u8 out[32]);
void sha256(const void* data, u32 length, u8 out[32]);

/*
 * HMAC-SHA256: a keyed fingerprint. HKDF builds key derivation on it.
 */
void hmac_sha256(
    const u8* key,
    u32 key_len,
    const u8* data,
    u32 data_len,
    u8 out[32]
);

void hkdf_extract(const u8* salt, u32 salt_len, const u8* ikm, u32 ikm_len, u8 out[32]);

/*
 * TLS 1.3's HKDF-Expand-Label. out_len must be at most 32.
 */
void hkdf_expand_label(
    const u8 secret[32],
    const char* label,
    const u8* context,
    u32 context_len,
    u8* out,
    u32 out_len
);

/*
 * X25519 key agreement: out = scalar * point on Curve25519.
 * x25519_base() multiplies the standard base point (makes a public key).
 */
void x25519(u8 out[32], const u8 scalar[32], const u8 point[32]);
void x25519_base(u8 out[32], const u8 scalar[32]);

/*
 * ChaCha20-Poly1305 authenticated encryption (RFC 8439).
 * seal writes `length` bytes of ciphertext and a 16-byte tag.
 * open returns 1 and the plaintext if the tag is right, 0 otherwise.
 * length must be at most 16640.
 */
void chacha20poly1305_seal(
    const u8 key[32],
    const u8 nonce[12],
    const u8* aad,
    u32 aad_len,
    const u8* plain,
    u32 length,
    u8* cipher,
    u8 tag[16]
);

int chacha20poly1305_open(
    const u8 key[32],
    const u8 nonce[12],
    const u8* aad,
    u32 aad_len,
    const u8* cipher,
    u32 length,
    const u8 tag[16],
    u8* plain
);

/*
 * AES-128 in Galois/Counter Mode: the other authenticated cipher TLS 1.3
 * uses (and the one every TLS 1.3 server must offer). Same interface as
 * the ChaCha20-Poly1305 functions above.
 */
void aes128_encrypt_block(const u8 key[16], const u8 in[16], u8 out[16]);

void aes128gcm_seal(
    const u8 key[16],
    const u8 nonce[12],
    const u8* aad,
    u32 aad_len,
    const u8* plain,
    u32 length,
    u8* cipher,
    u8 tag[16]
);

int aes128gcm_open(
    const u8 key[16],
    const u8 nonce[12],
    const u8* aad,
    u32 aad_len,
    const u8* cipher,
    u32 length,
    const u8 tag[16],
    u8* plain
);

/*
 * Unpredictable bytes, from the CPU's random number instruction where it
 * has one, mixed with timing.
 */
void random_bytes(u8* out, u32 length);

#endif
