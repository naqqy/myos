#include "include/naqnet.h"
#include "include/crypto.h"

/*
 * TLSTEST.ELF
 *
 * Self-test for the networking and TLS libraries. Run it from the shell:
 *
 *   run tlstest
 *
 * It checks the crypto against published test vectors, then fetches a
 * page over HTTPS and prints the start of the answer.
 */

static tls_conn_t g_conn;
static u8 g_buffer[2048];

static void print_number(u64 value) {
    char digits[24];
    int n = 0;

    do {
        digits[n++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value);

    while (n > 0) {
        putchar(digits[--n]);
    }
}

static int from_hex(const char* hex, u8* out, u32 count) {
    for (u32 i = 0; i < count; i++) {
        u32 value = 0;

        for (int k = 0; k < 2; k++) {
            char c = hex[i * 2 + (u32)k];

            value <<= 4;

            if (c >= '0' && c <= '9') {
                value |= (u32)(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= (u32)(c - 'a' + 10);
            } else {
                return 0;
            }
        }

        out[i] = (u8)value;
    }

    return 1;
}

static int same(const u8* a, const u8* b, u32 n) {
    for (u32 i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }

    return 1;
}

static void check(const char* name, int ok) {
    puts(ok ? "  ok    " : "  FAIL  ");
    puts(name);
    puts("\n");
}

static void crypto_tests(void) {
    u8 want[32];
    u8 got[32];
    u8 a[32];
    u8 b[32];

    puts("Crypto self-tests:\n");

    /*
     * SHA-256("abc"), from the standard.
     */
    from_hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", want, 32);
    sha256("abc", 3, got);
    check("SHA-256", same(want, got, 32));

    /*
     * HMAC-SHA256, RFC 4231 test case 2.
     */
    from_hex("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", want, 32);
    hmac_sha256((const u8*)"Jefe", 4, (const u8*)"what do ya want for nothing?", 28, got);
    check("HMAC-SHA256", same(want, got, 32));

    /*
     * X25519, RFC 7748 section 5.2.
     */
    from_hex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", a, 32);
    from_hex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", b, 32);
    from_hex("c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", want, 32);
    x25519(got, a, b);
    check("X25519", same(want, got, 32));

    /*
     * ChaCha20-Poly1305: what is sealed must open, and must not open
     * once a byte has been changed.
     */
    u8 key[32];
    u8 nonce[12];
    u8 aad[5] = { 23, 3, 3, 0, 40 };
    u8 plain[24];
    u8 cipher[24];
    u8 back[24];
    u8 tag[16];

    for (int i = 0; i < 32; i++) {
        key[i] = (u8)(i * 7 + 1);
    }

    for (int i = 0; i < 12; i++) {
        nonce[i] = (u8)(i + 3);
    }

    for (int i = 0; i < 24; i++) {
        plain[i] = (u8)('a' + i);
    }

    chacha20poly1305_seal(key, nonce, aad, 5, plain, 24, cipher, tag);

    int opens = chacha20poly1305_open(key, nonce, aad, 5, cipher, 24, tag, back) &&
                same(plain, back, 24);

    cipher[3] ^= 1;

    int rejects = !chacha20poly1305_open(key, nonce, aad, 5, cipher, 24, tag, back);

    check("ChaCha20-Poly1305 round trip", opens && rejects);

    /*
     * AES-128, FIPS 197 appendix C.1.
     */
    u8 aes_key[16];
    u8 aes_in[16];
    u8 aes_out[16];

    from_hex("000102030405060708090a0b0c0d0e0f", aes_key, 16);
    from_hex("00112233445566778899aabbccddeeff", aes_in, 16);
    from_hex("69c4e0d86a7b0430d8cdb78070b4c55a", want, 16);
    aes128_encrypt_block(aes_key, aes_in, aes_out);
    check("AES-128", same(want, aes_out, 16));

    /*
     * AES-128-GCM, NIST test case 2: all-zero key, nonce and block.
     */
    u8 zero_key[16];
    u8 zero_nonce[12];
    u8 zero_block[16];
    u8 gcm_cipher[16];
    u8 gcm_tag[16];
    u8 want_tag[16];

    for (int i = 0; i < 16; i++) {
        zero_key[i] = 0;
        zero_block[i] = 0;
    }

    for (int i = 0; i < 12; i++) {
        zero_nonce[i] = 0;
    }

    from_hex("0388dace60b6a392f328c2b971b2fe78", want, 16);
    from_hex("ab6e47d42cec13bdf53a67b21257bddf", want_tag, 16);
    aes128gcm_seal(zero_key, zero_nonce, zero_block, 0, zero_block, 16, gcm_cipher, gcm_tag);
    check("AES-128-GCM", same(want, gcm_cipher, 16) && same(want_tag, gcm_tag, 16));
}

static void fetch(const char* host) {
    puts("\nHTTPS GET https://");
    puts(host);
    puts("/\n");

    if (!net_ready()) {
        puts("  the network is not up\n");
        return;
    }

    u64 start = get_ticks();

    if (!tls_connect(&g_conn, host, 443, 8000)) {
        puts("  TLS failed: ");
        puts(tls_error(&g_conn));
        puts("\n");
        return;
    }

    puts("  handshake done in ");
    print_number(time_ticks_to_ms(get_ticks() - start));
    puts(" ms\n");

    char request[256];
    u32 n = 0;
    const char* parts[4] = { "GET / HTTP/1.1\r\nHost: ", host,
                             "\r\nUser-Agent: NAQOS\r\nAccept-Encoding: identity\r\n"
                             "Connection: close\r\n\r\n", 0 };

    for (int p = 0; parts[p]; p++) {
        for (u32 i = 0; parts[p][i] && n + 1 < sizeof(request); i++) {
            request[n++] = parts[p][i];
        }
    }

    if (!tls_write(&g_conn, request, n)) {
        puts("  could not send the request\n");
        tls_close(&g_conn);
        return;
    }

    u32 total = 0;
    u32 shown = 0;

    for (;;) {
        int got = tls_read(&g_conn, g_buffer, sizeof(g_buffer));

        if (got <= 0) {
            if (got < 0) {
                puts("\n  read error: ");
                puts(tls_error(&g_conn));
            }

            break;
        }

        for (int i = 0; i < got && shown < 60u; i++, shown++) {
            char c = (char)g_buffer[i];

            if (c == '\n' || (c >= 32 && c < 127)) {
                putchar(c);
            }
        }

        total += (u32)got;
    }

    tls_close(&g_conn);

    puts("\n  [");
    print_number(total);
    puts(" bytes received]\n");
}

int main(void) {
    crypto_tests();

    puts("\nWith ChaCha20-Poly1305:");
    tls_only_cipher(0x1303);
    fetch("example.com");

    puts("\nWith AES-128-GCM:");
    tls_only_cipher(0x1301);
    fetch("example.com");

    tls_only_cipher(0);

    return 0;
}
