#include "../include/naqx509.h"
#include "../include/crypto.h"
#include "sha512_tables.h"

/*
 * Certificates: reading them, and checking the signatures on them.
 * See naqx509.h for what this is for.
 *
 * Contents:
 *   SHA-384 / SHA-512
 *   arithmetic on large whole numbers (up to 4096 bits)
 *   RSA signatures (PKCS#1 v1.5 and PSS)
 *   ECDSA signatures on the curves P-256 and P-384
 *   reading certificates (DER) and checking a chain of them
 *
 * Nothing here needs to be secret (only public keys and signatures are
 * handled), so no care is taken to run in constant time.
 */

/* -------------------------------------------------------------------------- */
/* SHA-512 and SHA-384                                                         */
/* -------------------------------------------------------------------------- */

typedef struct sha512_ctx {
    u64 h[8];
    u8 block[128];
    u32 used;
    u64 total;
} sha512_ctx_t;

static u64 ror64(u64 v, u32 n) {
    return (v >> n) | (v << (64u - n));
}

static void sha512_block(sha512_ctx_t* c, const u8* p) {
    u64 w[80];
    u64 s[8];

    for (int i = 0; i < 16; i++) {
        u64 v = 0;

        for (int k = 0; k < 8; k++) {
            v = (v << 8) | p[i * 8 + k];
        }

        w[i] = v;
    }

    for (int i = 16; i < 80; i++) {
        u64 s0 = ror64(w[i - 15], 1) ^ ror64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        u64 s1 = ror64(w[i - 2], 19) ^ ror64(w[i - 2], 61) ^ (w[i - 2] >> 6);

        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    for (int i = 0; i < 8; i++) {
        s[i] = c->h[i];
    }

    for (int i = 0; i < 80; i++) {
        u64 e1 = ror64(s[4], 14) ^ ror64(s[4], 18) ^ ror64(s[4], 41);
        u64 ch = (s[4] & s[5]) ^ (~s[4] & s[6]);
        u64 t1 = s[7] + e1 + ch + g_sha512_k[i] + w[i];
        u64 e0 = ror64(s[0], 28) ^ ror64(s[0], 34) ^ ror64(s[0], 39);
        u64 maj = (s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]);
        u64 t2 = e0 + maj;

        s[7] = s[6]; s[6] = s[5]; s[5] = s[4]; s[4] = s[3] + t1;
        s[3] = s[2]; s[2] = s[1]; s[1] = s[0]; s[0] = t1 + t2;
    }

    for (int i = 0; i < 8; i++) {
        c->h[i] += s[i];
    }
}

static void sha512_run(const u64 start[8], const void* data, u32 length, u8* out, u32 out_len) {
    sha512_ctx_t c;
    const u8* p = (const u8*)data;

    for (int i = 0; i < 8; i++) {
        c.h[i] = start[i];
    }

    c.total = length;

    while (length >= 128u) {
        sha512_block(&c, p);
        p += 128;
        length -= 128u;
    }

    /* the tail: what is left, a 1 bit, zeros, and the length in bits */
    u8 tail[256];
    u32 n = 0;

    for (; n < length; n++) {
        tail[n] = p[n];
    }

    tail[n++] = 0x80;

    u32 total = (n <= 112u) ? 128u : 256u;

    while (n < total - 8u) {
        tail[n++] = 0;
    }

    u64 bits = c.total * 8u;

    for (int k = 7; k >= 0; k--) {
        tail[n++] = (u8)(bits >> (8 * k));
    }

    sha512_block(&c, tail);

    if (total == 256u) {
        sha512_block(&c, tail + 128);
    }

    for (u32 i = 0; i < out_len; i++) {
        out[i] = (u8)(c.h[i / 8u] >> (56u - 8u * (i % 8u)));
    }
}

void sha512(const void* data, u32 length, u8 out[64]) {
    sha512_run(g_sha512_start, data, length, out, 64);
}

void sha384(const void* data, u32 length, u8 out[48]) {
    sha512_run(g_sha384_start, data, length, out, 48);
}

/* One of the three hashes, by its size in bytes (32, 48 or 64). */
static void hash_any(u32 size, const void* data, u32 length, u8* out) {
    if (size == 32u) sha256(data, length, out);
    else if (size == 48u) sha384(data, length, out);
    else sha512(data, length, out);
}

/* -------------------------------------------------------------------------- */
/* Large numbers                                                               */
/* -------------------------------------------------------------------------- */

/* A number is an array of 32-bit "limbs", least significant first. */
#define BN_MAX 130u         /* enough for 4096 bits and a little */

static void bn_from_bytes(u32* out, u32 limbs, const u8* be, u32 length) {
    for (u32 i = 0; i < limbs; i++) {
        out[i] = 0;
    }

    for (u32 i = 0; i < length && i / 4u < limbs; i++) {
        out[i / 4u] |= (u32)be[length - 1u - i] << (8u * (i % 4u));
    }
}

static void bn_to_bytes(const u32* in, u32 limbs, u8* be, u32 length) {
    for (u32 i = 0; i < length; i++) {
        u32 limb = i / 4u;

        be[length - 1u - i] = limb < limbs ? (u8)(in[limb] >> (8u * (i % 4u))) : 0;
    }
}

static int bn_cmp(const u32* a, const u32* b, u32 n) {
    for (u32 i = n; i > 0; i--) {
        if (a[i - 1u] != b[i - 1u]) {
            return a[i - 1u] > b[i - 1u] ? 1 : -1;
        }
    }

    return 0;
}

static int bn_is_zero(const u32* a, u32 n) {
    for (u32 i = 0; i < n; i++) {
        if (a[i]) return 0;
    }

    return 1;
}

static u32 bn_add(u32* r, const u32* a, const u32* b, u32 n) {
    u64 carry = 0;

    for (u32 i = 0; i < n; i++) {
        u64 sum = (u64)a[i] + b[i] + carry;

        r[i] = (u32)sum;
        carry = sum >> 32;
    }

    return (u32)carry;
}

static u32 bn_sub(u32* r, const u32* a, const u32* b, u32 n) {
    u64 borrow = 0;

    for (u32 i = 0; i < n; i++) {
        u64 diff = (u64)a[i] - b[i] - borrow;

        r[i] = (u32)diff;
        borrow = (diff >> 32) & 1u;
    }

    return (u32)borrow;
}

/* out (2n limbs) = a * b */
static void bn_mul(u32* out, const u32* a, const u32* b, u32 n) {
    for (u32 i = 0; i < 2u * n; i++) {
        out[i] = 0;
    }

    for (u32 i = 0; i < n; i++) {
        u64 carry = 0;

        if (!a[i]) {
            continue;
        }

        for (u32 k = 0; k < n; k++) {
            u64 t = (u64)a[i] * b[k] + out[i + k] + carry;

            out[i + k] = (u32)t;
            carry = t >> 32;
        }

        out[i + n] = (u32)carry;
    }
}

/*
 * r (n limbs) = x (xn limbs) mod m (n limbs, top limb not zero).
 * Long division a limb at a time (Knuth's algorithm D).
 */
static void bn_mod(u32* r, const u32* x, u32 xn, const u32* m, u32 n) {
    static u32 u[2u * BN_MAX + 2u];
    static u32 v[BN_MAX];

    if (n == 1u) {
        u64 rem = 0;

        for (u32 i = xn; i > 0; i--) {
            rem = ((rem << 32) | x[i - 1u]) % m[0];
        }

        r[0] = (u32)rem;
        return;
    }

    if (xn < n) {
        for (u32 i = 0; i < n; i++) {
            r[i] = i < xn ? x[i] : 0;
        }

        return;
    }

    /* shift both so the divisor's top bit is set */
    u32 s = 0;

    while (!((m[n - 1u] << s) & 0x80000000u)) {
        s++;
    }

    for (u32 i = n - 1u; i > 0; i--) {
        v[i] = (m[i] << s) | (s ? m[i - 1u] >> (32u - s) : 0u);
    }

    v[0] = m[0] << s;

    u[xn] = s ? x[xn - 1u] >> (32u - s) : 0u;

    for (u32 i = xn - 1u; i > 0; i--) {
        u[i] = (x[i] << s) | (s ? x[i - 1u] >> (32u - s) : 0u);
    }

    u[0] = x[0] << s;

    for (u32 jj = xn - n + 1u; jj > 0; jj--) {
        u32 j = jj - 1u;
        u64 num = ((u64)u[j + n] << 32) | u[j + n - 1u];
        u64 qhat = num / v[n - 1u];
        u64 rhat = num % v[n - 1u];

        while (qhat >= 0x100000000ull || qhat * v[n - 2u] > ((rhat << 32) | u[j + n - 2u])) {
            qhat--;
            rhat += v[n - 1u];

            if (rhat >= 0x100000000ull) {
                break;
            }
        }

        /* u -= qhat * v, at position j */
        u64 carry = 0;
        u64 borrow = 0;

        for (u32 i = 0; i < n; i++) {
            u64 p = qhat * v[i] + carry;

            carry = p >> 32;

            u64 t = (u64)u[i + j] - (p & 0xFFFFFFFFull) - borrow;

            u[i + j] = (u32)t;
            borrow = (t >> 32) & 1u;
        }

        u64 t = (u64)u[j + n] - carry - borrow;

        u[j + n] = (u32)t;

        if ((t >> 32) & 1u) {
            /* one too many: add the divisor back */
            u64 c = 0;

            for (u32 i = 0; i < n; i++) {
                u64 sum = (u64)u[i + j] + v[i] + c;

                u[i + j] = (u32)sum;
                c = sum >> 32;
            }

            u[j + n] += (u32)c;
        }
    }

    for (u32 i = 0; i < n; i++) {
        r[i] = (u[i] >> s) | (s ? u[i + 1u] << (32u - s) : 0u);
    }
}

/* r = a * b mod m */
static void bn_modmul(u32* r, const u32* a, const u32* b, const u32* m, u32 n) {
    static u32 product[2u * BN_MAX];

    bn_mul(product, a, b, n);
    bn_mod(r, product, 2u * n, m, n);
}

/* r = base ^ exponent mod m; the exponent is given as big-endian bytes */
static void bn_modexp(u32* r, const u32* base, const u8* exponent, u32 exp_len, const u32* m, u32 n) {
    static u32 acc[BN_MAX];
    static u32 b[BN_MAX];
    int started = 0;

    for (u32 i = 0; i < n; i++) {
        b[i] = base[i];
        acc[i] = i == 0 ? 1u : 0u;
    }

    for (u32 i = 0; i < exp_len; i++) {
        for (int bit = 7; bit >= 0; bit--) {
            if (started) {
                bn_modmul(acc, acc, acc, m, n);
            }

            if ((exponent[i] >> bit) & 1u) {
                bn_modmul(acc, acc, b, m, n);
                started = 1;
            }
        }
    }

    for (u32 i = 0; i < n; i++) {
        r[i] = acc[i];
    }
}

/* -------------------------------------------------------------------------- */
/* RSA                                                                         */
/* -------------------------------------------------------------------------- */

/*
 * The public-key operation: signature ^ e mod n. Writes the result as
 * `*out_len` bytes (the size of the modulus). Returns 0 if the inputs are
 * not usable.
 */
static int rsa_public(const x509_key_t* key, const u8* sig, u32 sig_len, u8* out, u32* out_len) {
    static u32 m[BN_MAX];
    static u32 s[BN_MAX];
    static u32 result[BN_MAX];
    const u8* mod = key->n;
    u32 mod_len = key->n_len;
    const u8* e = key->e;
    u32 e_len = key->e_len;

    while (mod_len && mod[0] == 0) { mod++; mod_len--; }
    while (e_len && e[0] == 0) { e++; e_len--; }
    while (sig_len && sig[0] == 0) { sig++; sig_len--; }

    /* 2048 to 4096 bits, and an ordinary small exponent */
    if (mod_len < 250u || mod_len > 512u || e_len == 0 || e_len > 4u || sig_len > mod_len) {
        return 0;
    }

    u32 n = (mod_len + 3u) / 4u;

    bn_from_bytes(m, n, mod, mod_len);
    bn_from_bytes(s, n, sig, sig_len);

    if (bn_cmp(s, m, n) >= 0 || !(m[0] & 1u)) {
        return 0;
    }

    bn_modexp(result, s, e, e_len, m, n);
    bn_to_bytes(result, n, out, mod_len);
    *out_len = mod_len;

    return 1;
}

/* The fixed bytes that come before the hash in a PKCS#1 v1.5 signature. */
static const u8 g_digest_info_256[] = {
    0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01,
    0x05, 0x00, 0x04, 0x20
};
static const u8 g_digest_info_384[] = {
    0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02,
    0x05, 0x00, 0x04, 0x30
};
static const u8 g_digest_info_512[] = {
    0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03,
    0x05, 0x00, 0x04, 0x40
};

/* Old-style RSA signature: 00 01 FF ... FF 00, the hash's name, the hash. */
static int rsa_verify_pkcs1(const x509_key_t* key, u32 hash_size, const u8* msg, u32 msg_len,
                            const u8* sig, u32 sig_len) {
    static u8 em[512];
    u8 hash[64];
    u32 em_len = 0;
    const u8* info = hash_size == 32u ? g_digest_info_256
                   : (hash_size == 48u ? g_digest_info_384 : g_digest_info_512);
    u32 info_len = 19;

    if (!rsa_public(key, sig, sig_len, em, &em_len) || em_len < info_len + hash_size + 11u) {
        return 0;
    }

    hash_any(hash_size, msg, msg_len, hash);

    u32 pad_end = em_len - info_len - hash_size - 1u;
    u8 diff = (u8)(em[0] | (em[1] ^ 0x01u) | em[pad_end]);

    for (u32 i = 2; i < pad_end; i++) {
        diff |= (u8)(em[i] ^ 0xFFu);
    }

    for (u32 i = 0; i < info_len; i++) {
        diff |= (u8)(em[pad_end + 1u + i] ^ info[i]);
    }

    for (u32 i = 0; i < hash_size; i++) {
        diff |= (u8)(em[pad_end + 1u + info_len + i] ^ hash[i]);
    }

    return diff == 0;
}

/* Newer RSA signature (PSS), with a salt as long as the hash. */
static int rsa_verify_pss(const x509_key_t* key, u32 hash_size, const u8* msg, u32 msg_len,
                          const u8* sig, u32 sig_len) {
    static u8 em_buf[512];
    static u8 db[512];
    u8 hash[64];
    u8 block[8 + 64 + 64];
    u8 check[64];
    u32 em_len = 0;
    const u8* mod = key->n;
    u32 mod_len = key->n_len;

    if (!rsa_public(key, sig, sig_len, em_buf, &em_len)) {
        return 0;
    }

    while (mod_len && mod[0] == 0) { mod++; mod_len--; }

    /* the encoded message is one bit shorter than the modulus */
    u32 mod_bits = mod_len * 8u;

    for (u8 top = mod[0]; !(top & 0x80u); top = (u8)(top << 1)) {
        mod_bits--;
    }

    u32 em_bits = mod_bits - 1u;
    u32 need = (em_bits + 7u) / 8u;
    const u8* em = em_buf + (em_len - need);

    if (need < 2u * hash_size + 2u || em[need - 1u] != 0xBCu) {
        return 0;
    }

    /* anything in front of the encoded message must be zero */
    for (u32 i = 0; i < em_len - need; i++) {
        if (em_buf[i]) return 0;
    }

    u32 db_len = need - hash_size - 1u;
    const u8* h = em + db_len;
    u32 unused = 8u * need - em_bits;

    if (unused && (em[0] >> (8u - unused))) {
        return 0;
    }

    /* the mask: hash(h, counter) blocks, as many as needed */
    for (u32 counter = 0, at = 0; at < db_len; counter++) {
        u8 seed[64 + 4];
        u8 out[64];

        for (u32 i = 0; i < hash_size; i++) seed[i] = h[i];

        seed[hash_size] = (u8)(counter >> 24);
        seed[hash_size + 1u] = (u8)(counter >> 16);
        seed[hash_size + 2u] = (u8)(counter >> 8);
        seed[hash_size + 3u] = (u8)counter;
        hash_any(hash_size, seed, hash_size + 4u, out);

        for (u32 i = 0; i < hash_size && at < db_len; i++, at++) {
            db[at] = (u8)(em[at] ^ out[i]);
        }
    }

    if (unused) {
        db[0] &= (u8)(0xFFu >> unused);
    }

    /* zeros, then 01, then the salt */
    u32 salt_at = db_len - hash_size;

    for (u32 i = 0; i + 1u < salt_at; i++) {
        if (db[i]) return 0;
    }

    if (db[salt_at - 1u] != 0x01u) {
        return 0;
    }

    hash_any(hash_size, msg, msg_len, hash);

    for (u32 i = 0; i < 8u; i++) block[i] = 0;
    for (u32 i = 0; i < hash_size; i++) block[8u + i] = hash[i];
    for (u32 i = 0; i < hash_size; i++) block[8u + hash_size + i] = db[salt_at + i];

    hash_any(hash_size, block, 8u + 2u * hash_size, check);

    u8 diff = 0;

    for (u32 i = 0; i < hash_size; i++) {
        diff |= (u8)(check[i] ^ h[i]);
    }

    return diff == 0;
}

/* -------------------------------------------------------------------------- */
/* Elliptic curves P-256 and P-384                                             */
/* -------------------------------------------------------------------------- */

#define EC_MAX 12u      /* limbs: 384 bits */

typedef struct curve {
    u32 limbs;
    u32 p[EC_MAX];      /* the field: numbers are taken mod p */
    u32 b[EC_MAX];      /* the curve is y^2 = x^3 - 3x + b */
    u32 gx[EC_MAX];     /* the standard starting point G */
    u32 gy[EC_MAX];
    u32 order[EC_MAX];  /* how many points G generates */
} curve_t;

typedef struct point {
    u32 x[EC_MAX];
    u32 y[EC_MAX];
    u32 z[EC_MAX];      /* "projective" form: the real point is x/z^2, y/z^3 */
    int infinity;
} point_t;

static curve_t g_p256;
static curve_t g_p384;
static int g_curves_ready = 0;

static void from_hex(u32* out, u32 limbs, const char* hex) {
    u32 digits = 0;

    while (hex[digits]) {
        digits++;
    }

    for (u32 i = 0; i < limbs; i++) {
        out[i] = 0;
    }

    for (u32 i = 0; i < digits; i++) {
        char c = hex[digits - 1u - i];
        u32 v = (c >= '0' && c <= '9') ? (u32)(c - '0') : (u32)(c - 'A' + 10);

        out[i / 8u] |= v << (4u * (i % 8u));
    }
}

static void curves_init(void) {
    if (g_curves_ready) {
        return;
    }

    g_p256.limbs = 8;
    from_hex(g_p256.p, 8, "FFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFF");
    from_hex(g_p256.b, 8, "5AC635D8AA3A93E7B3EBBD55769886BC651D06B0CC53B0F63BCE3C3E27D2604B");
    from_hex(g_p256.gx, 8, "6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296");
    from_hex(g_p256.gy, 8, "4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5");
    from_hex(g_p256.order, 8, "FFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551");

    g_p384.limbs = 12;
    from_hex(g_p384.p, 12, "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFE"
                           "FFFFFFFF0000000000000000FFFFFFFF");
    from_hex(g_p384.b, 12, "B3312FA7E23EE7E4988E056BE3F82D19181D9C6EFE8141120314088F5013875A"
                           "C656398D8A2ED19D2A85C8EDD3EC2AEF");
    from_hex(g_p384.gx, 12, "AA87CA22BE8B05378EB1C71EF320AD746E1D3B628BA79B9859F741E082542A38"
                            "5502F25DBF55296C3A545E3872760AB7");
    from_hex(g_p384.gy, 12, "3617DE4A96262C6F5D9E98BF9292DC29F8F41DBD289A147CE9DA3113B5F0B8C0"
                            "0A60B1CE1D7E819D7A431D7C90EA0E5F");
    from_hex(g_p384.order, 12, "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFC7634D81F4372DDF"
                               "581A0DB248B0A77AECEC196ACCC52973");

    g_curves_ready = 1;
}

/* field arithmetic: all results are below p */

static void f_add(const curve_t* c, u32* r, const u32* a, const u32* b) {
    u32 carry = bn_add(r, a, b, c->limbs);

    if (carry || bn_cmp(r, c->p, c->limbs) >= 0) {
        bn_sub(r, r, c->p, c->limbs);
    }
}

static void f_sub(const curve_t* c, u32* r, const u32* a, const u32* b) {
    if (bn_sub(r, a, b, c->limbs)) {
        bn_add(r, r, c->p, c->limbs);
    }
}

static void f_mul(const curve_t* c, u32* r, const u32* a, const u32* b) {
    bn_modmul(r, a, b, c->p, c->limbs);
}

static void f_copy(const curve_t* c, u32* r, const u32* a) {
    for (u32 i = 0; i < c->limbs; i++) {
        r[i] = a[i];
    }
}

/* r = 1 / a mod m, by raising a to the power m - 2 (m is prime) */
static void bn_inverse(u32* r, const u32* a, const u32* m, u32 n) {
    u32 e[EC_MAX];
    u8 bytes[EC_MAX * 4u];
    u32 two[EC_MAX];

    for (u32 i = 0; i < n; i++) {
        two[i] = i == 0 ? 2u : 0u;
    }

    bn_sub(e, m, two, n);
    bn_to_bytes(e, n, bytes, n * 4u);
    bn_modexp(r, a, bytes, n * 4u, m, n);
}

static void point_double(const curve_t* c, point_t* r, const point_t* p) {
    u32 delta[EC_MAX], gamma[EC_MAX], beta[EC_MAX], alpha[EC_MAX];
    u32 t1[EC_MAX], t2[EC_MAX], x3[EC_MAX], y3[EC_MAX], z3[EC_MAX];

    if (p->infinity) {
        r->infinity = 1;
        return;
    }

    f_mul(c, delta, p->z, p->z);
    f_mul(c, gamma, p->y, p->y);
    f_mul(c, beta, p->x, gamma);

    /* alpha = 3 (x - delta)(x + delta) */
    f_sub(c, t1, p->x, delta);
    f_add(c, t2, p->x, delta);
    f_mul(c, alpha, t1, t2);
    f_add(c, t1, alpha, alpha);
    f_add(c, alpha, t1, alpha);

    /* x3 = alpha^2 - 8 beta */
    f_mul(c, x3, alpha, alpha);
    f_add(c, t1, beta, beta);       /* 2 beta */
    f_add(c, t1, t1, t1);           /* 4 beta */
    f_add(c, t2, t1, t1);           /* 8 beta */
    f_sub(c, x3, x3, t2);

    /* z3 = (y + z)^2 - gamma - delta */
    f_add(c, z3, p->y, p->z);
    f_mul(c, z3, z3, z3);
    f_sub(c, z3, z3, gamma);
    f_sub(c, z3, z3, delta);

    /* y3 = alpha (4 beta - x3) - 8 gamma^2 */
    f_sub(c, t1, t1, x3);
    f_mul(c, y3, alpha, t1);
    f_mul(c, t2, gamma, gamma);
    f_add(c, t2, t2, t2);
    f_add(c, t2, t2, t2);
    f_add(c, t2, t2, t2);
    f_sub(c, y3, y3, t2);

    f_copy(c, r->x, x3);
    f_copy(c, r->y, y3);
    f_copy(c, r->z, z3);
    r->infinity = 0;
}

/* r = p + (qx, qy), the second point being in ordinary form */
static void point_add(const curve_t* c, point_t* r, const point_t* p, const u32* qx, const u32* qy) {
    u32 z1z1[EC_MAX], u2[EC_MAX], s2[EC_MAX], h[EC_MAX], rr[EC_MAX];
    u32 hh[EC_MAX], hhh[EC_MAX], v[EC_MAX], t[EC_MAX], x3[EC_MAX], y3[EC_MAX], z3[EC_MAX];

    if (p->infinity) {
        f_copy(c, r->x, qx);
        f_copy(c, r->y, qy);

        for (u32 i = 0; i < c->limbs; i++) {
            r->z[i] = i == 0 ? 1u : 0u;
        }

        r->infinity = 0;
        return;
    }

    f_mul(c, z1z1, p->z, p->z);
    f_mul(c, u2, qx, z1z1);
    f_mul(c, s2, qy, p->z);
    f_mul(c, s2, s2, z1z1);
    f_sub(c, h, u2, p->x);
    f_sub(c, rr, s2, p->y);

    if (bn_is_zero(h, c->limbs)) {
        if (bn_is_zero(rr, c->limbs)) {
            point_double(c, r, p);      /* the same point twice */
        } else {
            r->infinity = 1;            /* opposite points cancel */
        }

        return;
    }

    f_mul(c, hh, h, h);
    f_mul(c, hhh, h, hh);
    f_mul(c, v, p->x, hh);

    f_mul(c, x3, rr, rr);
    f_sub(c, x3, x3, hhh);
    f_sub(c, x3, x3, v);
    f_sub(c, x3, x3, v);

    f_sub(c, t, v, x3);
    f_mul(c, y3, rr, t);
    f_mul(c, t, p->y, hhh);
    f_sub(c, y3, y3, t);

    f_mul(c, z3, p->z, h);

    f_copy(c, r->x, x3);
    f_copy(c, r->y, y3);
    f_copy(c, r->z, z3);
    r->infinity = 0;
}

/* r = k * (x, y) */
static void point_multiply(const curve_t* c, point_t* r, const u32* k, const u32* x, const u32* y) {
    r->infinity = 1;

    for (u32 bit = c->limbs * 32u; bit > 0; bit--) {
        point_double(c, r, r);

        if ((k[(bit - 1u) / 32u] >> ((bit - 1u) % 32u)) & 1u) {
            point_add(c, r, r, x, y);
        }
    }
}

/* Turns a point back into ordinary x, y. */
static void point_affine(const curve_t* c, const point_t* p, u32* x, u32* y) {
    u32 zi[EC_MAX], zi2[EC_MAX], zi3[EC_MAX];

    bn_inverse(zi, p->z, c->p, c->limbs);
    f_mul(c, zi2, zi, zi);
    f_mul(c, zi3, zi2, zi);
    f_mul(c, x, p->x, zi2);
    f_mul(c, y, p->y, zi3);
}

/* Reads the two numbers of an ECDSA signature: SEQUENCE { INTEGER r, INTEGER s }. */
static int ecdsa_parts(const u8* sig, u32 len, u32* r, u32* s, u32 limbs) {
    u32 at = 0;

    if (len < 8u || sig[at++] != 0x30u) {
        return 0;
    }

    if (sig[at] & 0x80u) {
        at += 1u + (sig[at] & 0x7Fu);
    } else {
        at++;
    }

    for (int which = 0; which < 2; which++) {
        if (at + 2u > len || sig[at] != 0x02u || (sig[at + 1u] & 0x80u)) {
            return 0;
        }

        u32 n = sig[at + 1u];
        const u8* v = sig + at + 2u;

        if (at + 2u + n > len) {
            return 0;
        }

        at += 2u + n;

        while (n && v[0] == 0) { v++; n--; }

        if (n > limbs * 4u) {
            return 0;
        }

        bn_from_bytes(which ? s : r, limbs, v, n);
    }

    return 1;
}

static int ecdsa_verify(const x509_key_t* key, u32 hash_size, const u8* msg, u32 msg_len,
                        const u8* sig, u32 sig_len) {
    curves_init();

    const curve_t* c = key->kind == X509_KEY_P256 ? &g_p256 : &g_p384;
    u32 n = c->limbs;
    u32 size = n * 4u;
    u32 r[EC_MAX], s[EC_MAX], e[EC_MAX], w[EC_MAX], u1[EC_MAX], u2[EC_MAX];
    u32 qx[EC_MAX], qy[EC_MAX], x[EC_MAX], y[EC_MAX], t[EC_MAX], lhs[EC_MAX];
    u8 hash[64];
    point_t a;
    point_t b;

    if (key->point_len != 1u + 2u * size || key->point[0] != 0x04u) {
        return 0;
    }

    bn_from_bytes(qx, n, key->point + 1, size);
    bn_from_bytes(qy, n, key->point + 1 + size, size);

    /* the key must be a point on the curve: y^2 = x^3 - 3x + b */
    if (bn_cmp(qx, c->p, n) >= 0 || bn_cmp(qy, c->p, n) >= 0) {
        return 0;
    }

    f_mul(c, lhs, qy, qy);
    f_mul(c, t, qx, qx);
    f_mul(c, t, t, qx);
    f_sub(c, t, t, qx);
    f_sub(c, t, t, qx);
    f_sub(c, t, t, qx);
    f_add(c, t, t, c->b);

    if (bn_cmp(lhs, t, n) != 0) {
        return 0;
    }

    if (!ecdsa_parts(sig, sig_len, r, s, n) || bn_is_zero(r, n) || bn_is_zero(s, n) ||
        bn_cmp(r, c->order, n) >= 0 || bn_cmp(s, c->order, n) >= 0) {
        return 0;
    }

    /* the hash, cut to the curve's size and taken as a number */
    hash_any(hash_size, msg, msg_len, hash);
    bn_from_bytes(e, n, hash, hash_size < size ? hash_size : size);

    if (bn_cmp(e, c->order, n) >= 0) {
        bn_sub(e, e, c->order, n);
    }

    bn_inverse(w, s, c->order, n);
    bn_modmul(u1, e, w, c->order, n);
    bn_modmul(u2, r, w, c->order, n);

    /* the point u1*G + u2*Q; its x must equal r */
    point_multiply(c, &b, u2, qx, qy);

    if (b.infinity) {
        point_multiply(c, &a, u1, c->gx, c->gy);
    } else {
        point_affine(c, &b, x, y);
        point_multiply(c, &a, u1, c->gx, c->gy);
        point_add(c, &a, &a, x, y);
    }

    if (a.infinity) {
        return 0;
    }

    point_affine(c, &a, x, y);

    if (bn_cmp(x, c->order, n) >= 0) {
        bn_sub(x, x, c->order, n);
    }

    return bn_cmp(x, r, n) == 0;
}

int x509_verify_signature(const x509_key_t* key, u32 scheme, const u8* message, u32 message_len,
                          const u8* signature, u32 signature_len) {
    switch (scheme) {
        case 0x0403: return key->kind == X509_KEY_P256 && ecdsa_verify(key, 32, message, message_len, signature, signature_len);
        case 0x0503: return key->kind == X509_KEY_P384 && ecdsa_verify(key, 48, message, message_len, signature, signature_len);
        case 0x0804: return key->kind == X509_KEY_RSA && rsa_verify_pss(key, 32, message, message_len, signature, signature_len);
        case 0x0805: return key->kind == X509_KEY_RSA && rsa_verify_pss(key, 48, message, message_len, signature, signature_len);
        case 0x0806: return key->kind == X509_KEY_RSA && rsa_verify_pss(key, 64, message, message_len, signature, signature_len);
        default:     return 0;
    }
}

/* -------------------------------------------------------------------------- */
/* Reading certificates                                                        */
/* -------------------------------------------------------------------------- */

/*
 * Certificates are written in DER: every item is a tag byte, a length,
 * and that many bytes of content, items nesting inside one another.
 */
typedef struct der {
    const u8* p;
    const u8* end;
} der_t;

/* Takes the next item. `whole` (optional) gets the item with its header. */
static int der_next(der_t* d, u8* tag, der_t* content, der_t* whole) {
    const u8* start = d->p;

    if (d->end - d->p < 2) {
        return 0;
    }

    *tag = d->p[0];

    u32 len = d->p[1];

    d->p += 2;

    if (len & 0x80u) {
        u32 count = len & 0x7Fu;

        if (count == 0 || count > 3u || (u32)(d->end - d->p) < count) {
            return 0;
        }

        len = 0;

        for (u32 i = 0; i < count; i++) {
            len = (len << 8) | *d->p++;
        }
    }

    if ((u32)(d->end - d->p) < len) {
        return 0;
    }

    content->p = d->p;
    content->end = d->p + len;
    d->p += len;

    if (whole) {
        whole->p = start;
        whole->end = d->p;
    }

    return 1;
}

static int der_is(const der_t* d, const u8* bytes, u32 length) {
    if ((u32)(d->end - d->p) != length) {
        return 0;
    }

    for (u32 i = 0; i < length; i++) {
        if (d->p[i] != bytes[i]) return 0;
    }

    return 1;
}

static const u8 OID_RSA[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01 };
static const u8 OID_RSA_SHA256[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0B };
static const u8 OID_RSA_SHA384[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0C };
static const u8 OID_RSA_SHA512[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0D };
static const u8 OID_EC_KEY[] = { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01 };
static const u8 OID_P256[] = { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07 };
static const u8 OID_P384[] = { 0x2B, 0x81, 0x04, 0x00, 0x22 };
static const u8 OID_ECDSA_SHA256[] = { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02 };
static const u8 OID_ECDSA_SHA384[] = { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x03 };
static const u8 OID_ECDSA_SHA512[] = { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x04 };
static const u8 OID_ALT_NAME[] = { 0x55, 0x1D, 0x11 };
static const u8 OID_BASIC[] = { 0x55, 0x1D, 0x13 };

#define SIG_UNKNOWN 0
#define SIG_RSA     1
#define SIG_ECDSA   2

typedef struct cert {
    der_t tbs;              /* the signed part, header included */
    der_t issuer;           /* names, header included, compared byte for byte */
    der_t subject;
    der_t key_info;         /* the whole public key structure */
    der_t alt_names;        /* contents of the "other names" extension */
    der_t signature;
    int sig_kind;
    u32 sig_hash;           /* 32, 48 or 64 */
    u64 not_before;         /* as yyyymmddhhmmss */
    u64 not_after;
    int is_ca;
    x509_key_t key;
} cert_t;

/* "250131120000Z" or "20250131120000Z" as the number 20250131120000. */
static u64 der_time(u8 tag, const der_t* d) {
    const u8* p = d->p;
    u32 n = (u32)(d->end - d->p);
    u64 v = 0;
    u32 digits = tag == 0x17u ? 12u : 14u;

    if (n < digits) {
        return 0;
    }

    for (u32 i = 0; i < digits; i++) {
        if (p[i] < '0' || p[i] > '9') return 0;
        v = v * 10u + (u64)(p[i] - '0');
    }

    if (tag == 0x17u) {
        /* two-digit year: 50..99 means 19xx */
        v += (v / 10000000000ull >= 50u) ? 19000000000000ull : 20000000000000ull;
    }

    return v;
}

static int cert_parse(const u8* data, u32 length, cert_t* c) {
    der_t all = { data, data + length };
    der_t outer, tbs, item, alg, sig, inner;
    u8 tag;

    memset(c, 0, sizeof(*c));

    if (!der_next(&all, &tag, &outer, 0) || tag != 0x30u) return 0;
    if (!der_next(&outer, &tag, &tbs, &c->tbs) || tag != 0x30u) return 0;
    if (!der_next(&outer, &tag, &alg, 0) || tag != 0x30u) return 0;
    if (!der_next(&outer, &tag, &sig, 0) || tag != 0x03u || sig.end - sig.p < 2) return 0;

    c->signature.p = sig.p + 1;     /* skip the "unused bits" byte */
    c->signature.end = sig.end;

    if (!der_next(&alg, &tag, &item, 0) || tag != 0x06u) return 0;

    if (der_is(&item, OID_RSA_SHA256, 9)) { c->sig_kind = SIG_RSA; c->sig_hash = 32; }
    else if (der_is(&item, OID_RSA_SHA384, 9)) { c->sig_kind = SIG_RSA; c->sig_hash = 48; }
    else if (der_is(&item, OID_RSA_SHA512, 9)) { c->sig_kind = SIG_RSA; c->sig_hash = 64; }
    else if (der_is(&item, OID_ECDSA_SHA256, 8)) { c->sig_kind = SIG_ECDSA; c->sig_hash = 32; }
    else if (der_is(&item, OID_ECDSA_SHA384, 8)) { c->sig_kind = SIG_ECDSA; c->sig_hash = 48; }
    else if (der_is(&item, OID_ECDSA_SHA512, 8)) { c->sig_kind = SIG_ECDSA; c->sig_hash = 64; }

    /* inside the signed part */
    if (!der_next(&tbs, &tag, &item, 0)) return 0;

    if (tag == 0xA0u) {
        /* the version; the serial number follows */
        if (!der_next(&tbs, &tag, &item, 0)) return 0;
    }

    if (tag != 0x02u) return 0;                                         /* serial number */
    if (!der_next(&tbs, &tag, &item, 0) || tag != 0x30u) return 0;      /* signature type again */
    if (!der_next(&tbs, &tag, &item, &c->issuer) || tag != 0x30u) return 0;

    if (!der_next(&tbs, &tag, &item, 0) || tag != 0x30u) return 0;      /* validity */

    if (!der_next(&item, &tag, &inner, 0)) return 0;
    c->not_before = der_time(tag, &inner);
    if (!der_next(&item, &tag, &inner, 0)) return 0;
    c->not_after = der_time(tag, &inner);

    if (!der_next(&tbs, &tag, &item, &c->subject) || tag != 0x30u) return 0;
    if (!der_next(&tbs, &tag, &item, &c->key_info) || tag != 0x30u) return 0;

    /* the public key: its kind (and curve), then the key bits */
    der_t key_alg, key_bits, oid;

    if (!der_next(&item, &tag, &key_alg, 0) || tag != 0x30u) return 0;
    if (!der_next(&item, &tag, &key_bits, 0) || tag != 0x03u || key_bits.end - key_bits.p < 2) return 0;
    if (!der_next(&key_alg, &tag, &oid, 0) || tag != 0x06u) return 0;

    key_bits.p++;

    if (der_is(&oid, OID_RSA, 9)) {
        der_t seq, n, e;

        if (!der_next(&key_bits, &tag, &seq, 0) || tag != 0x30u) return 0;
        if (!der_next(&seq, &tag, &n, 0) || tag != 0x02u) return 0;
        if (!der_next(&seq, &tag, &e, 0) || tag != 0x02u) return 0;

        c->key.kind = X509_KEY_RSA;
        c->key.n = n.p;
        c->key.n_len = (u32)(n.end - n.p);
        c->key.e = e.p;
        c->key.e_len = (u32)(e.end - e.p);
    } else if (der_is(&oid, OID_EC_KEY, 7)) {
        if (!der_next(&key_alg, &tag, &oid, 0) || tag != 0x06u) return 0;

        if (der_is(&oid, OID_P256, 8)) c->key.kind = X509_KEY_P256;
        else if (der_is(&oid, OID_P384, 5)) c->key.kind = X509_KEY_P384;

        c->key.point = key_bits.p;
        c->key.point_len = (u32)(key_bits.end - key_bits.p);
    }

    /* extensions, in [3] */
    while (der_next(&tbs, &tag, &item, 0)) {
        der_t list, ext, value;

        if (tag != 0xA3u) {
            continue;
        }

        if (!der_next(&item, &tag, &list, 0) || tag != 0x30u) return 0;

        while (der_next(&list, &tag, &ext, 0)) {
            if (tag != 0x30u || !der_next(&ext, &tag, &oid, 0) || tag != 0x06u) {
                continue;
            }

            /* an optional "critical" flag, then the value wrapped in an octet string */
            if (!der_next(&ext, &tag, &value, 0)) continue;
            if (tag == 0x01u && !der_next(&ext, &tag, &value, 0)) continue;
            if (tag != 0x04u) continue;

            if (der_is(&oid, OID_ALT_NAME, 3)) {
                if (der_next(&value, &tag, &inner, 0) && tag == 0x30u) {
                    c->alt_names = inner;
                }
            } else if (der_is(&oid, OID_BASIC, 3)) {
                der_t flag;

                if (der_next(&value, &tag, &inner, 0) && tag == 0x30u &&
                    der_next(&inner, &tag, &flag, 0) && tag == 0x01u && flag.end > flag.p) {
                    c->is_ca = flag.p[0] != 0;
                }
            }
        }
    }

    return 1;
}

static int same_bytes(const der_t* a, const der_t* b) {
    u32 n = (u32)(a->end - a->p);

    if (n != (u32)(b->end - b->p)) {
        return 0;
    }

    for (u32 i = 0; i < n; i++) {
        if (a->p[i] != b->p[i]) return 0;
    }

    return 1;
}

static char lower_char(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* Does a certificate name ("www.example.com" or "*.example.com") cover the host? */
static int name_covers(const u8* name, u32 name_len, const char* host) {
    u32 host_len = (u32)strlen(host);

    if (name_len > 2u && name[0] == '*' && name[1] == '.') {
        /* the star stands for exactly one label */
        u32 dot = 0;

        while (dot < host_len && host[dot] != '.') {
            dot++;
        }

        if (dot == 0 || dot >= host_len) {
            return 0;
        }

        host += dot;
        host_len -= dot;
        name += 1;
        name_len -= 1u;
    }

    if (name_len != host_len) {
        return 0;
    }

    for (u32 i = 0; i < name_len; i++) {
        if (lower_char((char)name[i]) != lower_char(host[i])) return 0;
    }

    return 1;
}

static int cert_names_host(const cert_t* c, const char* host) {
    der_t names = c->alt_names;
    der_t value;
    u8 tag;

    while (names.p && der_next(&names, &tag, &value, 0)) {
        /* [2]: a DNS name */
        if (tag == 0x82u && name_covers(value.p, (u32)(value.end - value.p), host)) {
            return 1;
        }
    }

    return 0;
}

/* Is `c` signed by the holder of `issuer_key`? */
static int cert_signed_by(const cert_t* c, const x509_key_t* issuer_key) {
    const u8* msg = c->tbs.p;
    u32 msg_len = (u32)(c->tbs.end - c->tbs.p);
    const u8* sig = c->signature.p;
    u32 sig_len = (u32)(c->signature.end - c->signature.p);

    if (c->sig_kind == SIG_RSA && issuer_key->kind == X509_KEY_RSA) {
        return rsa_verify_pkcs1(issuer_key, c->sig_hash, msg, msg_len, sig, sig_len);
    }

    if (c->sig_kind == SIG_ECDSA &&
        (issuer_key->kind == X509_KEY_P256 || issuer_key->kind == X509_KEY_P384)) {
        return ecdsa_verify(issuer_key, c->sig_hash, msg, msg_len, sig, sig_len);
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
/* The trusted roots                                                           */
/* -------------------------------------------------------------------------- */

#define ROOTS_PATH "/system/roots.bin"
#define ROOTS_MAX  (320u * 1024u)

static u8 g_roots[ROOTS_MAX];
static u32 g_roots_len = 0;
static int g_roots_tried = 0;

static void roots_load(void) {
    if (g_roots_tried) {
        return;
    }

    g_roots_tried = 1;

    i64 got = read_file(ROOTS_PATH, g_roots, sizeof(g_roots));

    g_roots_len = got > 0 ? (u32)got : 0u;
}

/* The date and time now, as yyyymmddhhmmss, or 0 if the clock cannot be read. */
static u64 now_stamp(void) {
    u64 c = clock_read();

    if (!c || (c >> 40) < 2024u) {
        return 0;
    }

    return (c >> 40) * 10000000000ull + ((c >> 32) & 255u) * 100000000ull +
           ((c >> 24) & 255u) * 1000000ull + ((c >> 16) & 255u) * 10000ull +
           ((c >> 8) & 255u) * 100ull + (c & 255u);
}

/* The PC's clock may be in local time: allow a day either way. */
static int in_date(const cert_t* c, u64 now) {
    if (!now) {
        return 1;
    }

    return now + 1000000u >= c->not_before && now <= c->not_after + 1000000u;
}

static void say(char* why, u32 size, const char* text) {
    u32 i = 0;

    for (; text[i] && i + 1u < size; i++) {
        why[i] = text[i];
    }

    if (size) {
        why[i] = 0;
    }
}

/*
 * Is `c` issued by one of the trusted roots (or is it one itself)?
 * 1 yes; 0 no root has that name; -1 a root has the name but the
 * signature or dates are wrong.
 */
static int trusted_issuer(const cert_t* c, u64 now) {
    int result = 0;

    roots_load();

    for (u32 at = 0; at + 2u <= g_roots_len; ) {
        u32 len = ((u32)g_roots[at] << 8) | g_roots[at + 1u];
        cert_t root;

        at += 2u;

        if (at + len > g_roots_len) {
            break;
        }

        const u8* der = g_roots + at;

        at += len;

        /* a quick look for the issuer's name before parsing the whole thing */
        if (!cert_parse(der, len, &root)) {
            continue;
        }

        /* the certificate is itself a trusted root */
        if (same_bytes(&c->subject, &root.subject) && same_bytes(&c->key_info, &root.key_info)) {
            return in_date(&root, now) ? 1 : -1;
        }

        if (!same_bytes(&c->issuer, &root.subject)) {
            continue;
        }

        if (in_date(&root, now) && cert_signed_by(c, &root.key)) {
            return 1;
        }

        result = -1;
    }

    return result;
}

int x509_verify_chain(const u8** certs, const u32* lengths, u32 count, const char* host,
                      x509_key_t* server_key, char* why, u32 why_size) {
    static cert_t chain[6];
    u64 now = now_stamp();

    if (server_key) {
        memset(server_key, 0, sizeof(*server_key));
    }

    if (count == 0) {
        say(why, why_size, "the server sent no certificate");
        return 0;
    }

    if (count > 6u) {
        count = 6u;
    }

    for (u32 i = 0; i < count; i++) {
        if (!cert_parse(certs[i], lengths[i], &chain[i])) {
            say(why, why_size, "a certificate could not be read");
            return 0;
        }
    }

    if (server_key) {
        *server_key = chain[0].key;
    }

    if (!cert_names_host(&chain[0], host)) {
        say(why, why_size, "the certificate is for a different site");
        return 0;
    }

    roots_load();

    if (!g_roots_len) {
        say(why, why_size, "the list of trusted authorities (" ROOTS_PATH ") is missing");
        return 0;
    }

    for (u32 i = 0; i < count; i++) {
        const cert_t* c = &chain[i];

        if (!in_date(c, now)) {
            say(why, why_size, now > c->not_after ? "a certificate has expired"
                                                  : "a certificate is not valid yet");
            return 0;
        }

        if (i > 0 && !c->is_ca) {
            say(why, why_size, "a certificate in the chain may not sign others");
            return 0;
        }

        if (c->sig_kind == SIG_UNKNOWN) {
            say(why, why_size, "a certificate is signed in a way this system cannot check");
            return 0;
        }

        /* does a trusted root vouch for this one? then the chain is good */
        int trusted = trusted_issuer(c, now);

        if (trusted == 1) {
            return 1;
        }

        if (trusted < 0 && i + 1u >= count) {
            say(why, why_size, "the signature of a trusted authority does not match");
            return 0;
        }

        /* otherwise the next certificate must have signed it */
        if (i + 1u >= count) {
            break;
        }

        if (!same_bytes(&c->issuer, &chain[i + 1u].subject)) {
            say(why, why_size, "the certificates sent do not form a chain");
            return 0;
        }

        if (chain[i + 1u].key.kind == 0) {
            say(why, why_size, "a certificate uses a kind of key this system cannot check");
            return 0;
        }

        if (!cert_signed_by(c, &chain[i + 1u].key)) {
            say(why, why_size, "a certificate's signature is wrong");
            return 0;
        }
    }

    say(why, why_size, "the certificate was not issued by a trusted authority");

    return 0;
}
