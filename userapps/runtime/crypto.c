#include "../include/crypto.h"

/*
 * Cryptographic primitives for TLS 1.3. See crypto.h for what to expect
 * of them.
 */

/* -------------------------------------------------------------------------- */
/* SHA-256                                                                     */
/* -------------------------------------------------------------------------- */

static const u32 g_sha_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static u32 rotr(u32 x, u32 n) {
    return (x >> n) | (x << (32u - n));
}

static void sha256_block(sha256_ctx_t* ctx, const u8* p) {
    u32 w[64];

    for (u32 i = 0; i < 16; i++) {
        w[i] = ((u32)p[i * 4] << 24) | ((u32)p[i * 4 + 1] << 16) |
               ((u32)p[i * 4 + 2] << 8) | (u32)p[i * 4 + 3];
    }

    for (u32 i = 16; i < 64; i++) {
        u32 s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);

        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    u32 a = ctx->state[0];
    u32 b = ctx->state[1];
    u32 c = ctx->state[2];
    u32 d = ctx->state[3];
    u32 e = ctx->state[4];
    u32 f = ctx->state[5];
    u32 g = ctx->state[6];
    u32 h = ctx->state[7];

    for (u32 i = 0; i < 64; i++) {
        u32 s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        u32 ch = (e & f) ^ (~e & g);
        u32 t1 = h + s1 + ch + g_sha_k[i] + w[i];
        u32 s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        u32 maj = (a & b) ^ (a & c) ^ (b & c);
        u32 t2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

void sha256_init(sha256_ctx_t* ctx) {
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
    ctx->length = 0;
    ctx->block_len = 0;
}

void sha256_update(sha256_ctx_t* ctx, const void* data, u32 length) {
    const u8* p = (const u8*)data;

    ctx->length += length;

    while (length > 0) {
        u32 take = 64u - ctx->block_len;

        if (take > length) {
            take = length;
        }

        for (u32 i = 0; i < take; i++) {
            ctx->block[ctx->block_len + i] = p[i];
        }

        ctx->block_len += take;
        p += take;
        length -= take;

        if (ctx->block_len == 64u) {
            sha256_block(ctx, ctx->block);
            ctx->block_len = 0;
        }
    }
}

void sha256_final(sha256_ctx_t* ctx, u8 out[32]) {
    u64 bits = ctx->length * 8u;
    u8 pad[72];
    u32 n = 0;

    /*
     * A 1 bit, zeros up to 56 bytes into a block, then the length in bits.
     */
    pad[n++] = 0x80;

    while ((ctx->block_len + n) % 64u != 56u) {
        pad[n++] = 0;
    }

    for (int i = 7; i >= 0; i--) {
        pad[n++] = (u8)(bits >> (i * 8));
    }

    sha256_update(ctx, pad, n);

    for (u32 i = 0; i < 8; i++) {
        out[i * 4] = (u8)(ctx->state[i] >> 24);
        out[i * 4 + 1] = (u8)(ctx->state[i] >> 16);
        out[i * 4 + 2] = (u8)(ctx->state[i] >> 8);
        out[i * 4 + 3] = (u8)ctx->state[i];
    }
}

void sha256(const void* data, u32 length, u8 out[32]) {
    sha256_ctx_t ctx;

    sha256_init(&ctx);
    sha256_update(&ctx, data, length);
    sha256_final(&ctx, out);
}

/* -------------------------------------------------------------------------- */
/* HMAC and HKDF                                                               */
/* -------------------------------------------------------------------------- */

void hmac_sha256(
    const u8* key,
    u32 key_len,
    const u8* data,
    u32 data_len,
    u8 out[32]
) {
    u8 block[64];
    u8 inner[32];
    sha256_ctx_t ctx;

    for (u32 i = 0; i < 64; i++) {
        block[i] = 0;
    }

    if (key_len > 64u) {
        sha256(key, key_len, block);
    } else {
        for (u32 i = 0; i < key_len; i++) {
            block[i] = key[i];
        }
    }

    /*
     * H((key ^ opad) || H((key ^ ipad) || data))
     */
    for (u32 i = 0; i < 64; i++) {
        block[i] ^= 0x36;
    }

    sha256_init(&ctx);
    sha256_update(&ctx, block, 64);
    sha256_update(&ctx, data, data_len);
    sha256_final(&ctx, inner);

    for (u32 i = 0; i < 64; i++) {
        block[i] ^= 0x36 ^ 0x5c;
    }

    sha256_init(&ctx);
    sha256_update(&ctx, block, 64);
    sha256_update(&ctx, inner, 32);
    sha256_final(&ctx, out);
}

void hkdf_extract(const u8* salt, u32 salt_len, const u8* ikm, u32 ikm_len, u8 out[32]) {
    hmac_sha256(salt, salt_len, ikm, ikm_len, out);
}

void hkdf_expand_label(
    const u8 secret[32],
    const char* label,
    const u8* context,
    u32 context_len,
    u8* out,
    u32 out_len
) {
    /*
     * info = length (2 bytes), "tls13 " + label (length-prefixed),
     * context (length-prefixed). With at most 32 bytes wanted, HKDF-Expand
     * is a single HMAC over info followed by the byte 1.
     */
    u8 info[128];
    u8 block[32];
    u32 n = 0;
    u32 label_len = (u32)strlen(label);

    info[n++] = (u8)(out_len >> 8);
    info[n++] = (u8)out_len;
    info[n++] = (u8)(6u + label_len);

    info[n++] = 't';
    info[n++] = 'l';
    info[n++] = 's';
    info[n++] = '1';
    info[n++] = '3';
    info[n++] = ' ';

    for (u32 i = 0; i < label_len; i++) {
        info[n++] = (u8)label[i];
    }

    info[n++] = (u8)context_len;

    for (u32 i = 0; i < context_len; i++) {
        info[n++] = context[i];
    }

    info[n++] = 1;

    hmac_sha256(secret, 32, info, n, block);

    for (u32 i = 0; i < out_len && i < 32u; i++) {
        out[i] = block[i];
    }
}

/* -------------------------------------------------------------------------- */
/* X25519                                                                      */
/* -------------------------------------------------------------------------- */

/*
 * Arithmetic modulo 2^255 - 19, with a number held as 16 limbs of 16 bits
 * each in 64-bit integers (so products and sums have room to grow before
 * carries are propagated). This is the well-known compact formulation
 * from TweetNaCl.
 */
typedef i64 gf[16];

static const gf g_121665 = { 0xDB41, 1 };

static void gf_carry(gf o) {
    for (int i = 0; i < 16; i++) {
        o[i] += (1LL << 16);

        i64 c = o[i] >> 16;

        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

/*
 * Swaps p and q if b is 1, in constant time.
 */
static void gf_select(gf p, gf q, int b) {
    i64 mask = ~((i64)b - 1);

    for (int i = 0; i < 16; i++) {
        i64 t = mask & (p[i] ^ q[i]);

        p[i] ^= t;
        q[i] ^= t;
    }
}

static void gf_pack(u8* o, const gf n) {
    gf m;
    gf t;

    for (int i = 0; i < 16; i++) {
        t[i] = n[i];
    }

    gf_carry(t);
    gf_carry(t);
    gf_carry(t);

    /*
     * Subtract the modulus up to twice, keeping the result only if it did
     * not go negative.
     */
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;

        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }

        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);

        int b = (int)((m[15] >> 16) & 1);

        m[14] &= 0xffff;

        gf_select(t, m, 1 - b);
    }

    for (int i = 0; i < 16; i++) {
        o[2 * i] = (u8)(t[i] & 0xff);
        o[2 * i + 1] = (u8)(t[i] >> 8);
    }
}

static void gf_unpack(gf o, const u8* n) {
    for (int i = 0; i < 16; i++) {
        o[i] = n[2 * i] + ((i64)n[2 * i + 1] << 8);
    }

    o[15] &= 0x7fff;
}

static void gf_add(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; i++) {
        o[i] = a[i] + b[i];
    }
}

static void gf_sub(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; i++) {
        o[i] = a[i] - b[i];
    }
}

static void gf_mul(gf o, const gf a, const gf b) {
    i64 t[31];

    for (int i = 0; i < 31; i++) {
        t[i] = 0;
    }

    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 16; j++) {
            t[i + j] += a[i] * b[j];
        }
    }

    /*
     * 2^256 = 38 (mod 2^255 - 19): fold the high half back in.
     */
    for (int i = 0; i < 15; i++) {
        t[i] += 38 * t[i + 16];
    }

    for (int i = 0; i < 16; i++) {
        o[i] = t[i];
    }

    gf_carry(o);
    gf_carry(o);
}

static void gf_square(gf o, const gf a) {
    gf_mul(o, a, a);
}

/*
 * o = 1 / in, by raising to the power p - 2.
 */
static void gf_invert(gf o, const gf in) {
    gf c;

    for (int i = 0; i < 16; i++) {
        c[i] = in[i];
    }

    for (int a = 253; a >= 0; a--) {
        gf_square(c, c);

        if (a != 2 && a != 4) {
            gf_mul(c, c, in);
        }
    }

    for (int i = 0; i < 16; i++) {
        o[i] = c[i];
    }
}

void x25519(u8 out[32], const u8 scalar[32], const u8 point[32]) {
    u8 z[32];
    gf x;
    gf a;
    gf b;
    gf c;
    gf d;
    gf e;
    gf f;

    for (int i = 0; i < 32; i++) {
        z[i] = scalar[i];
    }

    /*
     * "Clamp" the scalar as the standard requires.
     */
    z[31] = (u8)((scalar[31] & 127) | 64);
    z[0] &= 248;

    gf_unpack(x, point);

    for (int i = 0; i < 16; i++) {
        b[i] = x[i];
        d[i] = 0;
        a[i] = 0;
        c[i] = 0;
    }

    a[0] = 1;
    d[0] = 1;

    /*
     * Montgomery ladder: one step per bit of the scalar, the same work
     * whatever the bit is.
     */
    for (int i = 254; i >= 0; i--) {
        int r = (z[i >> 3] >> (i & 7)) & 1;

        gf_select(a, b, r);
        gf_select(c, d, r);

        gf_add(e, a, c);
        gf_sub(a, a, c);
        gf_add(c, b, d);
        gf_sub(b, b, d);
        gf_square(d, e);
        gf_square(f, a);
        gf_mul(a, c, a);
        gf_mul(c, b, e);
        gf_add(e, a, c);
        gf_sub(a, a, c);
        gf_square(b, a);
        gf_sub(c, d, f);
        gf_mul(a, c, g_121665);
        gf_add(a, a, d);
        gf_mul(c, c, a);
        gf_mul(a, d, f);
        gf_mul(d, b, x);
        gf_square(b, e);

        gf_select(a, b, r);
        gf_select(c, d, r);
    }

    gf_invert(c, c);
    gf_mul(a, a, c);
    gf_pack(out, a);
}

void x25519_base(u8 out[32], const u8 scalar[32]) {
    u8 base[32];

    for (int i = 0; i < 32; i++) {
        base[i] = 0;
    }

    base[0] = 9;

    x25519(out, scalar, base);
}

/* -------------------------------------------------------------------------- */
/* ChaCha20                                                                    */
/* -------------------------------------------------------------------------- */

static u32 rotl(u32 x, u32 n) {
    return (x << n) | (x >> (32u - n));
}

static u32 load32(const u8* p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

#define QUARTER(a, b, c, d) \
    a += b; d ^= a; d = rotl(d, 16); \
    c += d; b ^= c; b = rotl(b, 12); \
    a += b; d ^= a; d = rotl(d, 8);  \
    c += d; b ^= c; b = rotl(b, 7);

/*
 * One 64-byte block of key stream.
 */
static void chacha20_block(const u8 key[32], u32 counter, const u8 nonce[12], u8 out[64]) {
    u32 s[16];
    u32 x[16];

    s[0] = 0x61707865u;
    s[1] = 0x3320646eu;
    s[2] = 0x79622d32u;
    s[3] = 0x6b206574u;

    for (int i = 0; i < 8; i++) {
        s[4 + i] = load32(key + i * 4);
    }

    s[12] = counter;
    s[13] = load32(nonce);
    s[14] = load32(nonce + 4);
    s[15] = load32(nonce + 8);

    for (int i = 0; i < 16; i++) {
        x[i] = s[i];
    }

    for (int round = 0; round < 10; round++) {
        QUARTER(x[0], x[4], x[8], x[12])
        QUARTER(x[1], x[5], x[9], x[13])
        QUARTER(x[2], x[6], x[10], x[14])
        QUARTER(x[3], x[7], x[11], x[15])

        QUARTER(x[0], x[5], x[10], x[15])
        QUARTER(x[1], x[6], x[11], x[12])
        QUARTER(x[2], x[7], x[8], x[13])
        QUARTER(x[3], x[4], x[9], x[14])
    }

    for (int i = 0; i < 16; i++) {
        u32 v = x[i] + s[i];

        out[i * 4] = (u8)v;
        out[i * 4 + 1] = (u8)(v >> 8);
        out[i * 4 + 2] = (u8)(v >> 16);
        out[i * 4 + 3] = (u8)(v >> 24);
    }
}

static void chacha20_xor(
    const u8 key[32],
    u32 counter,
    const u8 nonce[12],
    const u8* in,
    u8* out,
    u32 length
) {
    u8 stream[64];

    for (u32 done = 0; done < length; done += 64u) {
        chacha20_block(key, counter++, nonce, stream);

        u32 take = length - done < 64u ? length - done : 64u;

        for (u32 i = 0; i < take; i++) {
            out[done + i] = in[done + i] ^ stream[i];
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Poly1305                                                                    */
/* -------------------------------------------------------------------------- */

/*
 * Poly1305 treats the message as coefficients of a polynomial, evaluates
 * it at a secret point r modulo 2^130 - 5, and adds a second secret s.
 * Numbers are 17 limbs of 8 bits (TweetNaCl's formulation).
 */
static void poly_add(u32* h, const u32* c) {
    u32 u = 0;

    for (int j = 0; j < 17; j++) {
        u += h[j] + c[j];
        h[j] = u & 255;
        u >>= 8;
    }
}

static void poly1305(u8 out[16], const u8* m, u32 n, const u8 key[32]) {
    static const u32 minus_p[17] = {
        5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 252
    };

    u32 x[17];
    u32 r[17];
    u32 h[17];
    u32 c[17];
    u32 g[17];

    for (int j = 0; j < 17; j++) {
        r[j] = 0;
        h[j] = 0;
    }

    for (int j = 0; j < 16; j++) {
        r[j] = key[j];
    }

    r[3] &= 15;
    r[4] &= 252;
    r[7] &= 15;
    r[8] &= 252;
    r[11] &= 15;
    r[12] &= 252;
    r[15] &= 15;

    while (n > 0) {
        u32 j;

        for (j = 0; j < 17; j++) {
            c[j] = 0;
        }

        for (j = 0; j < 16 && j < n; j++) {
            c[j] = m[j];
        }

        c[j] = 1;
        m += j;
        n -= j;

        poly_add(h, c);

        for (u32 i = 0; i < 17; i++) {
            x[i] = 0;

            for (j = 0; j < 17; j++) {
                x[i] += h[j] * ((j <= i) ? r[i - j] : 320 * r[i + 17 - j]);
            }
        }

        for (u32 i = 0; i < 17; i++) {
            h[i] = x[i];
        }

        u32 u = 0;

        for (j = 0; j < 16; j++) {
            u += h[j];
            h[j] = u & 255;
            u >>= 8;
        }

        u += h[16];
        h[16] = u & 3;
        u = 5 * (u >> 2);

        for (j = 0; j < 16; j++) {
            u += h[j];
            h[j] = u & 255;
            u >>= 8;
        }

        u += h[16];
        h[16] = u;
    }

    for (int j = 0; j < 17; j++) {
        g[j] = h[j];
    }

    poly_add(h, minus_p);

    u32 s = (u32)(-(i32)(h[16] >> 7));

    for (int j = 0; j < 17; j++) {
        h[j] ^= s & (g[j] ^ h[j]);
    }

    for (int j = 0; j < 16; j++) {
        c[j] = key[j + 16];
    }

    c[16] = 0;

    poly_add(h, c);

    for (int j = 0; j < 16; j++) {
        out[j] = (u8)h[j];
    }
}

/* -------------------------------------------------------------------------- */
/* ChaCha20-Poly1305                                                           */
/* -------------------------------------------------------------------------- */

#define AEAD_MAX 16640u

/*
 * The tag covers: associated data, padded to 16 bytes; ciphertext, padded
 * to 16 bytes; then both lengths as 64-bit little-endian numbers.
 */
static u8 g_mac_input[AEAD_MAX + 64u];

static void aead_tag(
    const u8 key[32],
    const u8 nonce[12],
    const u8* aad,
    u32 aad_len,
    const u8* cipher,
    u32 length,
    u8 tag[16]
) {
    u8 block[64];
    u32 n = 0;

    /*
     * The Poly1305 key is the first half of key-stream block 0.
     */
    chacha20_block(key, 0, nonce, block);

    for (u32 i = 0; i < aad_len; i++) {
        g_mac_input[n++] = aad[i];
    }

    while (n % 16u) {
        g_mac_input[n++] = 0;
    }

    for (u32 i = 0; i < length; i++) {
        g_mac_input[n++] = cipher[i];
    }

    while (n % 16u) {
        g_mac_input[n++] = 0;
    }

    for (u32 i = 0; i < 8; i++) {
        g_mac_input[n++] = (u8)(i < 4 ? aad_len >> (i * 8) : 0);
    }

    for (u32 i = 0; i < 8; i++) {
        g_mac_input[n++] = (u8)(i < 4 ? length >> (i * 8) : 0);
    }

    poly1305(tag, g_mac_input, n, block);
}

void chacha20poly1305_seal(
    const u8 key[32],
    const u8 nonce[12],
    const u8* aad,
    u32 aad_len,
    const u8* plain,
    u32 length,
    u8* cipher,
    u8 tag[16]
) {
    if (length > AEAD_MAX || aad_len > 32u) {
        return;
    }

    chacha20_xor(key, 1, nonce, plain, cipher, length);
    aead_tag(key, nonce, aad, aad_len, cipher, length, tag);
}

int chacha20poly1305_open(
    const u8 key[32],
    const u8 nonce[12],
    const u8* aad,
    u32 aad_len,
    const u8* cipher,
    u32 length,
    const u8 tag[16],
    u8* plain
) {
    u8 expected[16];
    u8 diff = 0;

    if (length > AEAD_MAX || aad_len > 32u) {
        return 0;
    }

    aead_tag(key, nonce, aad, aad_len, cipher, length, expected);

    for (int i = 0; i < 16; i++) {
        diff |= (u8)(expected[i] ^ tag[i]);
    }

    if (diff) {
        return 0;
    }

    chacha20_xor(key, 1, nonce, cipher, plain, length);

    return 1;
}

/* -------------------------------------------------------------------------- */
/* AES-128                                                                     */
/* -------------------------------------------------------------------------- */

/*
 * AES scrambles a 16-byte block through ten rounds of: substitute every
 * byte through a fixed table (the S-box), rotate the rows, mix the
 * columns, and XOR in a round key derived from the key.
 *
 * The S-box is computed rather than typed in: each entry is the inverse
 * of the byte in the field GF(2^8), followed by a fixed bit shuffle.
 */
static u8 g_sbox[256];
static int g_sbox_ready = 0;

static u8 rotl8(u8 x, u32 n) {
    return (u8)((x << n) | (x >> (8u - n)));
}

static void aes_build_sbox(void) {
    u8 p = 1;
    u8 q = 1;

    /*
     * p runs through every non-zero byte as powers of 3; q is kept equal
     * to 1/p.
     */
    do {
        p = (u8)(p ^ (u8)(p << 1) ^ ((p & 0x80u) ? 0x1Bu : 0u));

        q ^= (u8)(q << 1);
        q ^= (u8)(q << 2);
        q ^= (u8)(q << 4);

        if (q & 0x80u) {
            q ^= 0x09u;
        }

        g_sbox[p] = (u8)(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4) ^ 0x63u);
    } while (p != 1);

    g_sbox[0] = 0x63;
    g_sbox_ready = 1;
}

/*
 * The 11 round keys (176 bytes) for a 128-bit key.
 */
static void aes_expand_key(const u8 key[16], u8 round_keys[176]) {
    static const u8 rcon[10] = {
        0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36
    };

    if (!g_sbox_ready) {
        aes_build_sbox();
    }

    for (int i = 0; i < 16; i++) {
        round_keys[i] = key[i];
    }

    for (int i = 16; i < 176; i += 4) {
        u8 t[4];

        t[0] = round_keys[i - 4];
        t[1] = round_keys[i - 3];
        t[2] = round_keys[i - 2];
        t[3] = round_keys[i - 1];

        if (i % 16 == 0) {
            u8 first = t[0];

            t[0] = (u8)(g_sbox[t[1]] ^ rcon[i / 16 - 1]);
            t[1] = g_sbox[t[2]];
            t[2] = g_sbox[t[3]];
            t[3] = g_sbox[first];
        }

        for (int k = 0; k < 4; k++) {
            round_keys[i + k] = (u8)(round_keys[i - 16 + k] ^ t[k]);
        }
    }
}

static u8 xtime(u8 x) {
    return (u8)((x << 1) ^ ((x & 0x80u) ? 0x1Bu : 0u));
}

static void aes_encrypt(const u8 round_keys[176], const u8 in[16], u8 out[16]) {
    u8 s[16];

    for (int i = 0; i < 16; i++) {
        s[i] = (u8)(in[i] ^ round_keys[i]);
    }

    for (int round = 1; round <= 10; round++) {
        u8 t[16];

        /*
         * Substitute, and rotate row r left by r (bytes are stored column
         * by column, so row r is indices r, r+4, r+8, r+12).
         */
        for (int col = 0; col < 4; col++) {
            for (int row = 0; row < 4; row++) {
                t[col * 4 + row] = g_sbox[s[((col + row) % 4) * 4 + row]];
            }
        }

        if (round < 10) {
            for (int col = 0; col < 4; col++) {
                u8* c = t + col * 4;
                u8 all = (u8)(c[0] ^ c[1] ^ c[2] ^ c[3]);
                u8 first = c[0];

                c[0] ^= (u8)(all ^ xtime((u8)(c[0] ^ c[1])));
                c[1] ^= (u8)(all ^ xtime((u8)(c[1] ^ c[2])));
                c[2] ^= (u8)(all ^ xtime((u8)(c[2] ^ c[3])));
                c[3] ^= (u8)(all ^ xtime((u8)(c[3] ^ first)));
            }
        }

        for (int i = 0; i < 16; i++) {
            s[i] = (u8)(t[i] ^ round_keys[round * 16 + i]);
        }
    }

    for (int i = 0; i < 16; i++) {
        out[i] = s[i];
    }
}

void aes128_encrypt_block(const u8 key[16], const u8 in[16], u8 out[16]) {
    u8 round_keys[176];

    aes_expand_key(key, round_keys);
    aes_encrypt(round_keys, in, out);
}

/* -------------------------------------------------------------------------- */
/* GCM                                                                         */
/* -------------------------------------------------------------------------- */

/*
 * x = x * h in the field GF(2^128), in GCM's bit order (the first bit of
 * the first byte is the lowest power).
 */
static void gf128_multiply(u8 x[16], const u8 h[16]) {
    u8 z[16];
    u8 v[16];

    for (int i = 0; i < 16; i++) {
        z[i] = 0;
        v[i] = h[i];
    }

    for (int bit = 0; bit < 128; bit++) {
        if (x[bit >> 3] & (0x80u >> (bit & 7))) {
            for (int i = 0; i < 16; i++) {
                z[i] ^= v[i];
            }
        }

        int carry = v[15] & 1;

        for (int i = 15; i > 0; i--) {
            v[i] = (u8)((v[i] >> 1) | (v[i - 1] << 7));
        }

        v[0] >>= 1;

        if (carry) {
            v[0] ^= 0xE1;
        }
    }

    for (int i = 0; i < 16; i++) {
        x[i] = z[i];
    }
}

static void ghash_blocks(u8 y[16], const u8 h[16], const u8* data, u32 length) {
    while (length > 0) {
        u32 take = length < 16u ? length : 16u;

        for (u32 i = 0; i < take; i++) {
            y[i] ^= data[i];
        }

        gf128_multiply(y, h);

        data += take;
        length -= take;
    }
}

/*
 * Counter-mode key stream: block n is AES(nonce || counter).
 */
static void gcm_ctr(
    const u8 round_keys[176],
    const u8 nonce[12],
    u32 counter,
    const u8* in,
    u8* out,
    u32 length
) {
    u8 block[16];
    u8 stream[16];

    for (int i = 0; i < 12; i++) {
        block[i] = nonce[i];
    }

    for (u32 done = 0; done < length; done += 16u) {
        block[12] = (u8)(counter >> 24);
        block[13] = (u8)(counter >> 16);
        block[14] = (u8)(counter >> 8);
        block[15] = (u8)counter;
        counter++;

        aes_encrypt(round_keys, block, stream);

        u32 take = length - done < 16u ? length - done : 16u;

        for (u32 i = 0; i < take; i++) {
            out[done + i] = in[done + i] ^ stream[i];
        }
    }
}

static void gcm_tag(
    const u8 round_keys[176],
    const u8 nonce[12],
    const u8* aad,
    u32 aad_len,
    const u8* cipher,
    u32 length,
    u8 tag[16]
) {
    u8 h[16];
    u8 y[16];
    u8 lengths[16];
    u8 first[16];

    for (int i = 0; i < 16; i++) {
        h[i] = 0;
        y[i] = 0;
        lengths[i] = 0;
    }

    /*
     * The hash key is the encryption of an all-zero block.
     */
    aes_encrypt(round_keys, h, h);

    ghash_blocks(y, h, aad, aad_len);
    ghash_blocks(y, h, cipher, length);

    /*
     * Both lengths in bits, big-endian.
     */
    u64 aad_bits = (u64)aad_len * 8u;
    u64 cipher_bits = (u64)length * 8u;

    for (int i = 0; i < 8; i++) {
        lengths[7 - i] = (u8)(aad_bits >> (i * 8));
        lengths[15 - i] = (u8)(cipher_bits >> (i * 8));
    }

    ghash_blocks(y, h, lengths, 16);

    /*
     * Masked with the key stream block for counter 1.
     */
    for (int i = 0; i < 16; i++) {
        first[i] = 0;
    }

    gcm_ctr(round_keys, nonce, 1, first, first, 16);

    for (int i = 0; i < 16; i++) {
        tag[i] = (u8)(y[i] ^ first[i]);
    }
}

void aes128gcm_seal(
    const u8 key[16],
    const u8 nonce[12],
    const u8* aad,
    u32 aad_len,
    const u8* plain,
    u32 length,
    u8* cipher,
    u8 tag[16]
) {
    u8 round_keys[176];

    aes_expand_key(key, round_keys);
    gcm_ctr(round_keys, nonce, 2, plain, cipher, length);
    gcm_tag(round_keys, nonce, aad, aad_len, cipher, length, tag);
}

int aes128gcm_open(
    const u8 key[16],
    const u8 nonce[12],
    const u8* aad,
    u32 aad_len,
    const u8* cipher,
    u32 length,
    const u8 tag[16],
    u8* plain
) {
    u8 round_keys[176];
    u8 expected[16];
    u8 diff = 0;

    aes_expand_key(key, round_keys);
    gcm_tag(round_keys, nonce, aad, aad_len, cipher, length, expected);

    for (int i = 0; i < 16; i++) {
        diff |= (u8)(expected[i] ^ tag[i]);
    }

    if (diff) {
        return 0;
    }

    gcm_ctr(round_keys, nonce, 2, cipher, plain, length);

    return 1;
}

/* -------------------------------------------------------------------------- */
/* Random numbers                                                              */
/* -------------------------------------------------------------------------- */

static u64 read_tsc(void) {
    u32 lo;
    u32 hi;

    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));

    return ((u64)hi << 32) | lo;
}

static int cpu_has_rdrand(void) {
    u32 eax = 1;
    u32 ebx = 0;
    u32 ecx = 0;
    u32 edx = 0;

    __asm__ volatile ("cpuid" : "+a"(eax), "+b"(ebx), "+c"(ecx), "+d"(edx));

    return (ecx >> 30) & 1u;
}

static int rdrand64(u64* out) {
    unsigned char ok;

    __asm__ volatile ("rdrand %0; setc %1" : "=r"(*out), "=qm"(ok));

    return ok;
}

/*
 * A pool that every call stirs: the CPU's hardware random numbers when it
 * has them, plus the exact timing of this code, hashed together.
 */
static u8 g_pool[32];
static u64 g_pool_counter = 0;

void random_bytes(u8* out, u32 length) {
    int hardware = cpu_has_rdrand();

    while (length > 0) {
        sha256_ctx_t ctx;
        u8 block[32];
        u64 sample;

        sha256_init(&ctx);
        sha256_update(&ctx, g_pool, 32);

        g_pool_counter++;
        sha256_update(&ctx, &g_pool_counter, 8);

        for (int i = 0; i < 16; i++) {
            sample = read_tsc();
            sha256_update(&ctx, &sample, 8);

            if (hardware && rdrand64(&sample)) {
                sha256_update(&ctx, &sample, 8);
            }

            sample = get_ticks();
            sha256_update(&ctx, &sample, 8);
        }

        sha256_final(&ctx, block);

        /*
         * Half of the result goes back into the pool and is never given
         * out; the other half is the output.
         */
        for (int i = 0; i < 16; i++) {
            g_pool[i] ^= block[i];
            g_pool[16 + i] ^= block[i] + (u8)i;
        }

        u32 take = length < 16u ? length : 16u;

        for (u32 i = 0; i < take; i++) {
            out[i] = block[16 + i];
        }

        out += take;
        length -= take;
    }
}
