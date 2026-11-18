#include "../include/naqnet.h"
#include "../include/naqx509.h"
#include "../include/crypto.h"

/*
 * TLS 1.3 client. See naqnet.h for what it does and does not protect.
 *
 * The handshake in outline:
 *
 *   client -> ClientHello      our random bytes, our X25519 public key
 *   server -> ServerHello      its random bytes, its X25519 public key
 *       both sides now compute the same shared secret and derive
 *       "handshake" keys; everything after this is encrypted
 *   server -> EncryptedExtensions, Certificate, CertificateVerify, Finished
 *   client -> Finished
 *       both sides derive "application" keys; data flows
 *
 * Every handshake message is fed into a running SHA-256 (the
 * "transcript"); keys and the Finished checks are derived from it, so
 * both ends must have seen exactly the same conversation.
 *
 * On the wire everything travels in records:
 *   [type][version 03 03][length][payload]
 * Once keys exist every record has type 23 and its payload is
 * encrypted data + 16-byte tag; the real type is the last byte of the
 * decrypted data.
 */

#define RECORD_CHANGE_CIPHER 20u
#define RECORD_ALERT         21u
#define RECORD_HANDSHAKE     22u
#define RECORD_APPLICATION   23u

#define HS_CLIENT_HELLO         1u
#define HS_SERVER_HELLO         2u
#define HS_NEW_SESSION_TICKET   4u
#define HS_ENCRYPTED_EXTENSIONS 8u
#define HS_CERTIFICATE          11u
#define HS_CERTIFICATE_REQUEST  13u
#define HS_CERTIFICATE_VERIFY   15u
#define HS_FINISHED             20u
#define HS_KEY_UPDATE           24u

#define CIPHER_AES_128_GCM_SHA256       0x1301u
#define CIPHER_CHACHA20_POLY1305_SHA256 0x1303u

static u32 g_only_cipher = 0;

void tls_only_cipher(u32 suite) {
    g_only_cipher = suite;
}

/*
 * The record cipher, whichever the server picked.
 */
static void record_seal(
    const tls_conn_t* c,
    const u8* key,
    const u8 nonce[12],
    const u8* aad,
    const u8* plain,
    u32 length,
    u8* cipher,
    u8* tag
) {
    if (c->cipher == CIPHER_AES_128_GCM_SHA256) {
        aes128gcm_seal(key, nonce, aad, 5, plain, length, cipher, tag);
    } else {
        chacha20poly1305_seal(key, nonce, aad, 5, plain, length, cipher, tag);
    }
}

static int record_open(
    const tls_conn_t* c,
    const u8* key,
    const u8 nonce[12],
    const u8* aad,
    const u8* cipher,
    u32 length,
    const u8* tag,
    u8* plain
) {
    if (c->cipher == CIPHER_AES_128_GCM_SHA256) {
        return aes128gcm_open(key, nonce, aad, 5, cipher, length, tag, plain);
    }

    return chacha20poly1305_open(key, nonce, aad, 5, cipher, length, tag, plain);
}

#define TAG_LEN 16u

static void copy(u8* dst, const u8* src, u32 n) {
    for (u32 i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

static int fail(tls_conn_t* c, const char* why) {
    if (!c->error) {
        c->error = why;
    }

    return 0;
}

const char* tls_error(const tls_conn_t* c) {
    return c->error ? c->error : "";
}

/* -------------------------------------------------------------------------- */
/* Records                                                                     */
/* -------------------------------------------------------------------------- */

static int read_exact(tls_conn_t* c, u8* out, u32 n) {
    u32 done = 0;

    while (done < n) {
        int got = tcp_read(c->sock, out + done, n - done, c->timeout_ms);

        if (got == 0) {
            c->ended = 1;
            return fail(c, "the server closed the connection");
        }

        if (got < 0) {
            return fail(c, got == -1 ? "the server stopped answering"
                                     : "the connection was reset");
        }

        done += (u32)got;
    }

    return 1;
}

/*
 * Per-record nonce: the IV with the record counter XORed into its last
 * eight bytes.
 */
static void make_nonce(const u8 iv[12], u64 seq, u8 nonce[12]) {
    for (int i = 0; i < 12; i++) {
        nonce[i] = iv[i];
    }

    for (int i = 0; i < 8; i++) {
        nonce[11 - i] ^= (u8)(seq >> (i * 8));
    }
}

/*
 * Reads one record into c->record. For an encrypted record, decrypts it
 * in place and reports the real type. *out points at the payload.
 */
static int read_record(tls_conn_t* c, int encrypted, u32* out_type, u8** out, u32* out_len) {
    u8 header[5];

    if (!read_exact(c, header, 5)) {
        return 0;
    }

    u32 type = header[0];
    u32 length = ((u32)header[3] << 8) | header[4];

    if (length > TLS_RECORD_MAX + 256u) {
        return fail(c, "the server sent an oversized record");
    }

    if (!read_exact(c, c->record, length)) {
        return 0;
    }

    if (!encrypted || type != RECORD_APPLICATION) {
        *out_type = type;
        *out = c->record;
        *out_len = length;
        return 1;
    }

    if (length < TAG_LEN + 1u) {
        return fail(c, "the server sent a malformed record");
    }

    u8 nonce[12];
    u32 cipher_len = length - TAG_LEN;

    make_nonce(c->recv_iv, c->recv_seq, nonce);

    if (!record_open(
            c,
            c->recv_key,
            nonce,
            header,
            c->record,
            cipher_len,
            c->record + cipher_len,
            c->record)) {
        return fail(c, "a record failed its integrity check");
    }

    c->recv_seq++;

    /*
     * Strip the zero padding; the last non-zero byte is the real type.
     */
    while (cipher_len > 0 && c->record[cipher_len - 1u] == 0) {
        cipher_len--;
    }

    if (cipher_len == 0) {
        return fail(c, "the server sent an empty record");
    }

    *out_type = c->record[cipher_len - 1u];
    *out = c->record;
    *out_len = cipher_len - 1u;

    return 1;
}

static int write_plain_record(tls_conn_t* c, u8 type, const u8* data, u32 length) {
    u8 header[5];

    header[0] = type;
    header[1] = 0x03;
    header[2] = type == RECORD_HANDSHAKE ? 0x01 : 0x03;
    header[3] = (u8)(length >> 8);
    header[4] = (u8)length;

    return tcp_write_all(c->sock, header, 5, c->timeout_ms) &&
           tcp_write_all(c->sock, data, length, c->timeout_ms);
}

static int write_encrypted_record(tls_conn_t* c, u8 type, const u8* data, u32 length) {
    static u8 out[TLS_RECORD_MAX + 64u];
    static u8 inner[TLS_RECORD_MAX + 8u];
    u8 nonce[12];

    if (length > TLS_RECORD_MAX) {
        return 0;
    }

    copy(inner, data, length);
    inner[length] = type;

    u32 cipher_len = length + 1u;
    u32 total = cipher_len + TAG_LEN;

    out[0] = RECORD_APPLICATION;
    out[1] = 0x03;
    out[2] = 0x03;
    out[3] = (u8)(total >> 8);
    out[4] = (u8)total;

    make_nonce(c->send_iv, c->send_seq, nonce);

    record_seal(
        c,
        c->send_key,
        nonce,
        out,
        inner,
        cipher_len,
        out + 5,
        out + 5 + cipher_len
    );

    c->send_seq++;

    return tcp_write_all(c->sock, out, 5u + total, c->timeout_ms);
}

/* -------------------------------------------------------------------------- */
/* Key schedule                                                                */
/* -------------------------------------------------------------------------- */

static void transcript_hash(const sha256_ctx_t* transcript, u8 out[32]) {
    sha256_ctx_t snapshot = *transcript;

    sha256_final(&snapshot, out);
}

/*
 * Derive-Secret(secret, label, transcript): a new secret tied to
 * everything said so far.
 */
static void derive_secret(const u8 secret[32], const char* label, const u8 hash[32], u8 out[32]) {
    hkdf_expand_label(secret, label, hash, 32, out, 32);
}

static void traffic_keys(const tls_conn_t* c, const u8 secret[32], u8 key[32], u8 iv[12]) {
    hkdf_expand_label(secret, "key", 0, 0, key, c->key_len);
    hkdf_expand_label(secret, "iv", 0, 0, iv, 12);
}

/* -------------------------------------------------------------------------- */
/* Handshake                                                                   */
/* -------------------------------------------------------------------------- */

static void put16(u8* p, u32 v) {
    p[0] = (u8)(v >> 8);
    p[1] = (u8)v;
}

/*
 * Builds the ClientHello handshake message. Returns its length.
 */
static u32 build_client_hello(
    u8* out,
    const char* host,
    const u8 random[32],
    const u8 session_id[32],
    const u8 public_key[32]
) {
    u32 host_len = (u32)strlen(host);
    u32 n = 4;      /* type + 3-byte length, filled in at the end */

    out[n++] = 0x03;                /* legacy version: TLS 1.2 */
    out[n++] = 0x03;

    copy(out + n, random, 32);
    n += 32;

    /*
     * A session ID is unused in TLS 1.3 but sending one makes the
     * exchange look like TLS 1.2 to network equipment that expects it.
     */
    out[n++] = 32;
    copy(out + n, session_id, 32);
    n += 32;

    /*
     * Cipher suites, preferred first. ChaCha20 is the faster of the two
     * without special CPU instructions.
     */
    if (g_only_cipher) {
        put16(out + n, 2);
        put16(out + n + 2, g_only_cipher);
        n += 4;
    } else {
        put16(out + n, 4);
        put16(out + n + 2, CIPHER_CHACHA20_POLY1305_SHA256);
        put16(out + n + 4, CIPHER_AES_128_GCM_SHA256);
        n += 6;
    }

    out[n++] = 1;                   /* compression: none */
    out[n++] = 0;

    u32 ext_len_at = n;

    n += 2;

    /*
     * server_name: which site we want (one address can host many).
     */
    put16(out + n, 0);
    put16(out + n + 2, host_len + 5u);
    put16(out + n + 4, host_len + 3u);
    out[n + 6] = 0;
    put16(out + n + 7, host_len);
    copy(out + n + 9, (const u8*)host, host_len);
    n += 9u + host_len;

    /*
     * supported_groups: X25519.
     */
    put16(out + n, 10);
    put16(out + n + 2, 4);
    put16(out + n + 4, 2);
    put16(out + n + 6, 0x001D);
    n += 8;

    /*
     * signature_algorithms: the kinds of signature this client can check
     * (ECDSA on P-256 / P-384, RSA with SHA-256/384/512).
     */
    static const u16 sigs[8] = {
        0x0403, 0x0804, 0x0401, 0x0503, 0x0805, 0x0501, 0x0806, 0x0601
    };

    put16(out + n, 13);
    put16(out + n + 2, 2u + 16u);
    put16(out + n + 4, 16);
    n += 6;

    for (int i = 0; i < 8; i++) {
        put16(out + n, sigs[i]);
        n += 2;
    }

    /*
     * supported_versions: TLS 1.3.
     */
    put16(out + n, 43);
    put16(out + n + 2, 3);
    out[n + 4] = 2;
    put16(out + n + 5, 0x0304);
    n += 7;

    /*
     * ALPN: we speak HTTP/1.1.
     */
    put16(out + n, 16);
    put16(out + n + 2, 11);
    put16(out + n + 4, 9);
    out[n + 6] = 8;
    copy(out + n + 7, (const u8*)"http/1.1", 8);
    n += 15;

    /*
     * key_share: our X25519 public key.
     */
    put16(out + n, 51);
    put16(out + n + 2, 38);
    put16(out + n + 4, 36);
    put16(out + n + 6, 0x001D);
    put16(out + n + 8, 32);
    copy(out + n + 10, public_key, 32);
    n += 42;

    put16(out + ext_len_at, n - ext_len_at - 2u);

    out[0] = HS_CLIENT_HELLO;
    out[1] = 0;
    put16(out + 2, n - 4u);

    return n;
}

/*
 * Picks the server's public key out of a ServerHello body.
 */
static int parse_server_hello(tls_conn_t* c, const u8* p, u32 length, u8 server_key[32]) {
    static const u8 retry_marker[8] = {
        0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11
    };

    if (length < 38u) {
        return fail(c, "the server's hello was too short");
    }

    int retry = 1;

    for (int i = 0; i < 8; i++) {
        if (p[2 + i] != retry_marker[i]) {
            retry = 0;
        }
    }

    if (retry) {
        return fail(c, "the server wants a key exchange this client does not support");
    }

    u32 at = 34;
    u32 session_len = p[at++];

    at += session_len;

    if (at + 5u > length) {
        return fail(c, "the server's hello was malformed");
    }

    u32 cipher = ((u32)p[at] << 8) | p[at + 1];

    at += 3;        /* cipher (2) + compression (1) */

    if (cipher == CIPHER_CHACHA20_POLY1305_SHA256) {
        c->key_len = 32;
    } else if (cipher == CIPHER_AES_128_GCM_SHA256) {
        c->key_len = 16;
    } else {
        return fail(c, "the server chose a cipher this client does not support");
    }

    c->cipher = cipher;

    u32 ext_end = at + 2u + (((u32)p[at] << 8) | p[at + 1]);

    at += 2;

    if (ext_end > length) {
        return fail(c, "the server's hello was malformed");
    }

    int have_version = 0;
    int have_key = 0;

    while (at + 4u <= ext_end) {
        u32 type = ((u32)p[at] << 8) | p[at + 1];
        u32 size = ((u32)p[at + 2] << 8) | p[at + 3];

        at += 4;

        if (at + size > ext_end) {
            break;
        }

        if (type == 43u && size == 2u && p[at] == 0x03 && p[at + 1] == 0x04) {
            have_version = 1;
        } else if (type == 51u && size == 36u &&
                   p[at] == 0x00 && p[at + 1] == 0x1D &&
                   p[at + 2] == 0x00 && p[at + 3] == 32) {
            copy(server_key, p + at + 4, 32);
            have_key = 1;
        }

        at += size;
    }

    if (!have_version) {
        return fail(c, "the server does not support TLS 1.3");
    }

    if (!have_key) {
        return fail(c, "the server did not send a usable key");
    }

    return 1;
}

int tls_connect(tls_conn_t* c, const char* host, u16 port, u32 timeout_ms) {
    static u8 hello[512];
    sha256_ctx_t transcript;

    u8 client_random[32];
    u8 session_id[32];
    u8 private_key[32];
    u8 public_key[32];
    u8 server_key[32];
    u8 shared[32];

    u8 zeros[32];
    u8 empty_hash[32];
    u8 hash[32];
    u8 early[32];
    u8 derived[32];
    u8 handshake_secret[32];
    u8 master[32];
    u8 client_hs[32];
    u8 server_hs[32];
    u8 client_app[32];
    u8 server_app[32];
    u8 finished_key[32];
    u8 verify[32];

    c->sock = -1;
    c->open = 0;
    c->ended = 0;
    c->error = 0;
    c->timeout_ms = timeout_ms;
    c->send_seq = 0;
    c->recv_seq = 0;
    c->plain_len = 0;
    c->plain_pos = 0;
    c->handshake_len = 0;
    c->cipher = CIPHER_CHACHA20_POLY1305_SHA256;
    c->key_len = 32;
    c->verified = 0;
    c->verify_error[0] = 0;

    /* who the server is: filled in as its certificate messages arrive */
    static u8 cert_message[32768];
    static x509_key_t server_public;
    int chain_ok = 0;
    int key_proved = 0;

    if (strlen(host) > 200u) {
        return fail(c, "that host name is too long");
    }

    for (int i = 0; i < 32; i++) {
        zeros[i] = 0;
    }

    c->sock = tcp_connect(host, port, timeout_ms);

    if (c->sock < 0) {
        return fail(c, "could not connect to the server");
    }

    random_bytes(client_random, 32);
    random_bytes(session_id, 32);
    random_bytes(private_key, 32);
    x25519_base(public_key, private_key);

    sha256_init(&transcript);

    u32 hello_len = build_client_hello(hello, host, client_random, session_id, public_key);

    sha256_update(&transcript, hello, hello_len);

    if (!write_plain_record(c, RECORD_HANDSHAKE, hello, hello_len)) {
        tls_close(c);
        return fail(c, "could not send to the server");
    }

    /*
     * ServerHello, in the clear.
     */
    for (;;) {
        u32 type = 0;
        u8* data = 0;
        u32 length = 0;

        if (!read_record(c, 0, &type, &data, &length)) {
            tls_close(c);
            return 0;
        }

        if (type == RECORD_ALERT) {
            tls_close(c);
            return fail(c, "the server refused the connection (TLS alert)");
        }

        if (type != RECORD_HANDSHAKE) {
            continue;
        }

        if (c->handshake_len + length > sizeof(c->handshake)) {
            tls_close(c);
            return fail(c, "the server's handshake was too large");
        }

        copy(c->handshake + c->handshake_len, data, length);
        c->handshake_len += length;

        if (c->handshake_len < 4u) {
            continue;
        }

        u32 body = ((u32)c->handshake[1] << 16) | ((u32)c->handshake[2] << 8) |
                   c->handshake[3];

        if (c->handshake_len < 4u + body) {
            continue;
        }

        if (c->handshake[0] != HS_SERVER_HELLO) {
            tls_close(c);
            return fail(c, "the server answered with something unexpected");
        }

        if (!parse_server_hello(c, c->handshake + 4, body, server_key)) {
            tls_close(c);
            return 0;
        }

        sha256_update(&transcript, c->handshake, 4u + body);

        /*
         * Keep anything that followed in the same record.
         */
        u32 rest = c->handshake_len - 4u - body;

        for (u32 i = 0; i < rest; i++) {
            c->handshake[i] = c->handshake[4u + body + i];
        }

        c->handshake_len = rest;
        break;
    }

    /*
     * Both sides can now compute the same secret.
     */
    x25519(shared, private_key, server_key);

    sha256("", 0, empty_hash);

    hkdf_extract(zeros, 32, zeros, 32, early);
    derive_secret(early, "derived", empty_hash, derived);
    hkdf_extract(derived, 32, shared, 32, handshake_secret);

    transcript_hash(&transcript, hash);
    derive_secret(handshake_secret, "c hs traffic", hash, client_hs);
    derive_secret(handshake_secret, "s hs traffic", hash, server_hs);

    traffic_keys(c, server_hs, c->recv_key, c->recv_iv);
    traffic_keys(c, client_hs, c->send_key, c->send_iv);

    derive_secret(handshake_secret, "derived", empty_hash, derived);
    hkdf_extract(derived, 32, zeros, 32, master);

    /*
     * The rest of the server's handshake, encrypted.
     */
    int finished = 0;

    while (!finished) {
        u32 type = 0;
        u8* data = 0;
        u32 length = 0;

        if (!read_record(c, 1, &type, &data, &length)) {
            tls_close(c);
            return 0;
        }

        if (type == RECORD_CHANGE_CIPHER) {
            continue;
        }

        if (type == RECORD_ALERT) {
            tls_close(c);
            return fail(c, "the server ended the handshake (TLS alert)");
        }

        if (type != RECORD_HANDSHAKE) {
            tls_close(c);
            return fail(c, "the server sent data before finishing the handshake");
        }

        if (c->handshake_len + length > sizeof(c->handshake)) {
            tls_close(c);
            return fail(c, "the server's certificate chain is too large");
        }

        copy(c->handshake + c->handshake_len, data, length);
        c->handshake_len += length;

        /*
         * Take every complete message out of the buffer.
         */
        u32 at = 0;

        while (!finished && c->handshake_len - at >= 4u) {
            const u8* m = c->handshake + at;
            u32 body = ((u32)m[1] << 16) | ((u32)m[2] << 8) | m[3];

            if (c->handshake_len - at < 4u + body) {
                break;
            }

            if (m[0] == HS_CERTIFICATE_REQUEST) {
                tls_close(c);
                return fail(c, "the server asked for a client certificate");
            }

            if (m[0] == HS_FINISHED) {
                /*
                 * The server's Finished is an HMAC over the transcript
                 * up to (not including) this message. A match proves
                 * both sides derived the same keys.
                 */
                hkdf_expand_label(server_hs, "finished", 0, 0, finished_key, 32);
                transcript_hash(&transcript, hash);
                hmac_sha256(finished_key, 32, hash, 32, verify);

                u8 diff = 0;

                if (body != 32u) {
                    diff = 1;
                } else {
                    for (int i = 0; i < 32; i++) {
                        diff |= (u8)(verify[i] ^ m[4 + i]);
                    }
                }

                if (diff) {
                    tls_close(c);
                    return fail(c, "the handshake could not be confirmed");
                }

                finished = 1;
            }

            if (m[0] == HS_CERTIFICATE && body <= sizeof(cert_message)) {
                /*
                 * The certificate chain: a context, then a list of
                 * certificates, each with a 3-byte length and followed
                 * by its extensions. Kept in a buffer of its own because
                 * the server's key is needed again for the next message.
                 */
                const u8* certs[8];
                u32 lengths[8];
                u32 count = 0;

                copy(cert_message, m + 4, body);

                u32 p = 1u + cert_message[0];

                p += 3u;    /* length of the whole list */

                while (p + 3u <= body && count < 8u) {
                    u32 len = ((u32)cert_message[p] << 16) | ((u32)cert_message[p + 1u] << 8) | cert_message[p + 2u];

                    p += 3u;

                    if (p + len + 2u > body) {
                        break;
                    }

                    certs[count] = cert_message + p;
                    lengths[count] = len;
                    count++;
                    p += len;
                    p += 2u + (((u32)cert_message[p] << 8) | cert_message[p + 1u]);
                }

                chain_ok = x509_verify_chain(certs, lengths, count, host, &server_public,
                                             c->verify_error, sizeof(c->verify_error));
            }

            if (m[0] == HS_CERTIFICATE_VERIFY && body >= 4u) {
                /*
                 * Proof that the server holds the certificate's key: its
                 * signature over everything said so far in the handshake.
                 */
                static u8 signed_text[64 + 34 + 32];
                static const char label[] = "TLS 1.3, server CertificateVerify";
                u32 scheme = ((u32)m[4] << 8) | m[5];
                u32 sig_len = ((u32)m[6] << 8) | m[7];

                for (u32 i = 0; i < 64u; i++) {
                    signed_text[i] = 0x20;
                }

                copy(signed_text + 64, (const u8*)label, 33);
                signed_text[64 + 33] = 0;
                transcript_hash(&transcript, signed_text + 64 + 34);

                key_proved = sig_len + 4u <= body && server_public.kind != 0 &&
                             x509_verify_signature(&server_public, scheme, signed_text, sizeof(signed_text),
                                                   m + 8, sig_len);
            }

            sha256_update(&transcript, m, 4u + body);

            at += 4u + body;
        }

        u32 rest = c->handshake_len - at;

        for (u32 i = 0; i < rest; i++) {
            c->handshake[i] = c->handshake[at + i];
        }

        c->handshake_len = rest;
    }

    c->verified = chain_ok && key_proved;

    if (chain_ok && !key_proved) {
        const char* why = "the server could not prove it holds the certificate's key";
        u32 i = 0;

        for (; why[i] && i + 1u < sizeof(c->verify_error); i++) {
            c->verify_error[i] = why[i];
        }

        c->verify_error[i] = 0;
    } else if (!chain_ok && !c->verify_error[0]) {
        const char* why = "the server sent no certificate";
        u32 i = 0;

        for (; why[i]; i++) {
            c->verify_error[i] = why[i];
        }

        c->verify_error[i] = 0;
    }

    /*
     * Application secrets come from the transcript through the server's
     * Finished; our own Finished covers the same.
     */
    transcript_hash(&transcript, hash);
    derive_secret(master, "c ap traffic", hash, client_app);
    derive_secret(master, "s ap traffic", hash, server_app);

    hkdf_expand_label(client_hs, "finished", 0, 0, finished_key, 32);
    hmac_sha256(finished_key, 32, hash, 32, verify);

    u8 message[36];
    u8 change_cipher = 1;

    message[0] = HS_FINISHED;
    message[1] = 0;
    message[2] = 0;
    message[3] = 32;
    copy(message + 4, verify, 32);

    /*
     * A dummy "change cipher spec" first, again for old network
     * equipment; then our Finished under the handshake key.
     */
    if (!write_plain_record(c, RECORD_CHANGE_CIPHER, &change_cipher, 1) ||
        !write_encrypted_record(c, RECORD_HANDSHAKE, message, 36)) {
        tls_close(c);
        return fail(c, "could not send to the server");
    }

    traffic_keys(c, client_app, c->send_key, c->send_iv);
    traffic_keys(c, server_app, c->recv_key, c->recv_iv);
    copy(c->recv_secret, server_app, 32);

    c->send_seq = 0;
    c->recv_seq = 0;
    c->handshake_len = 0;
    c->open = 1;

    return 1;
}

/* -------------------------------------------------------------------------- */
/* Application data                                                            */
/* -------------------------------------------------------------------------- */

int tls_write(tls_conn_t* c, const void* data, u32 length) {
    const u8* p = (const u8*)data;

    if (!c->open) {
        return 0;
    }

    while (length > 0) {
        u32 chunk = length < TLS_RECORD_MAX ? length : TLS_RECORD_MAX;

        if (!write_encrypted_record(c, RECORD_APPLICATION, p, chunk)) {
            return fail(c, "could not send to the server");
        }

        p += chunk;
        length -= chunk;
    }

    return 1;
}

/*
 * Handshake messages that arrive after the handshake: session tickets
 * (ignored) and key updates (the server switches to a fresh key).
 */
static void post_handshake(tls_conn_t* c, const u8* data, u32 length) {
    u32 at = 0;

    while (length - at >= 4u) {
        u32 body = ((u32)data[at + 1] << 16) | ((u32)data[at + 2] << 8) | data[at + 3];

        if (length - at < 4u + body) {
            break;
        }

        if (data[at] == HS_KEY_UPDATE) {
            u8 next[32];

            hkdf_expand_label(c->recv_secret, "traffic upd", 0, 0, next, 32);
            copy(c->recv_secret, next, 32);
            traffic_keys(c, c->recv_secret, c->recv_key, c->recv_iv);
            c->recv_seq = 0;
        }

        at += 4u + body;
    }
}

int tls_read(tls_conn_t* c, void* buffer, u32 capacity) {
    if (!c->open) {
        return -1;
    }

    while (c->plain_pos >= c->plain_len) {
        u32 type = 0;
        u8* data = 0;
        u32 length = 0;

        if (c->ended) {
            return 0;
        }

        if (!read_record(c, 1, &type, &data, &length)) {
            /*
             * A server that simply closes the connection after its last
             * byte is common enough to treat as a normal end.
             */
            return c->ended ? 0 : -1;
        }

        if (type == RECORD_APPLICATION) {
            copy(c->plain, data, length);
            c->plain_len = length;
            c->plain_pos = 0;
        } else if (type == RECORD_ALERT) {
            /*
             * Alert 0 is "close_notify": a polite end of the stream.
             */
            c->ended = 1;

            if (length >= 2u && data[1] != 0) {
                fail(c, "the server reported an error (TLS alert)");
                return -1;
            }

            return 0;
        } else if (type == RECORD_HANDSHAKE) {
            post_handshake(c, data, length);
        }
    }

    u32 available = c->plain_len - c->plain_pos;
    u32 take = available < capacity ? available : capacity;

    copy((u8*)buffer, c->plain + c->plain_pos, take);
    c->plain_pos += take;

    return (int)take;
}

void tls_close(tls_conn_t* c) {
    if (c->open) {
        /*
         * close_notify: warning level (1), description 0.
         */
        u8 alert[2] = { 1, 0 };

        write_encrypted_record(c, RECORD_ALERT, alert, 2);
    }

    if (c->sock > 0) {
        sock_close(c->sock);
    }

    c->sock = -1;
    c->open = 0;
}
