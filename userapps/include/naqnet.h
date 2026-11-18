#ifndef NAQNET_H
#define NAQNET_H

#include "naqos.h"

/*
 * Networking for apps: TCP connections and TLS (HTTPS) on top of them.
 */

/* -------------------------------------------------------------------------- */
/* Raw system calls. None of these wait, except net_resolve().                 */
/* -------------------------------------------------------------------------- */

/*
 * 1 once the system has a network address.
 */
int net_ready(void);

/*
 * Looks up a host name (or parses "1.2.3.4"). Waits a few seconds at most.
 * Returns 0 on success; the address is in host byte order.
 */
int net_resolve(const char* name, u32* out_ip);

#define SOCK_CONNECTING 0
#define SOCK_OPEN       1
#define SOCK_CLOSED     2   /* the other side has finished sending */

/*
 * Starts a TCP connection and returns its handle (1 or more) at once, or
 * a negative value. sock_state() says when it is open.
 */
int sock_open(u32 ip, u16 port);
int sock_state(int handle);

/*
 * Returns how many bytes were taken, which may be fewer than asked or 0.
 */
int sock_send(int handle, const void* data, u32 length);

/*
 * Returns the byte count, 0 if nothing has arrived yet, -1 when the other
 * side has closed and everything has been read, -2 on error.
 */
int sock_recv(int handle, void* buffer, u32 capacity);
void sock_close(int handle);

/* -------------------------------------------------------------------------- */
/* TCP helpers that wait.                                                      */
/* -------------------------------------------------------------------------- */

/*
 * Resolves the name, connects and waits until the connection is open.
 * Returns the handle, or a negative value.
 */
int tcp_connect(const char* host, u16 port, u32 timeout_ms);

/*
 * Sends everything. Returns 1, or 0 on error or timeout.
 */
int tcp_write_all(int handle, const void* data, u32 length, u32 timeout_ms);

/*
 * Waits for data. Returns the byte count, 0 when the other side has
 * closed, -1 on timeout, -2 on error.
 */
int tcp_read(int handle, void* buffer, u32 capacity, u32 timeout_ms);

/* -------------------------------------------------------------------------- */
/* TLS 1.3 client.                                                             */
/* -------------------------------------------------------------------------- */

/*
 * What this is and is not:
 *
 *   - It speaks TLS 1.3 only, with X25519 key exchange and either
 *     ChaCha20-Poly1305 or AES-128-GCM encryption. Servers limited to
 *     TLS 1.2 cannot be reached.
 *   - The connection is encrypted and protected against tampering.
 *   - THE SERVER'S CERTIFICATE IS NOT CHECKED. Nothing proves that the
 *     server is who it claims to be, so someone able to intercept the
 *     connection could impersonate it. Do not send passwords or other
 *     secrets through it.
 */
#define TLS_RECORD_MAX 16384u

typedef struct tls_conn {
    int sock;
    int open;
    int ended;
    const char* error;
    u32 timeout_ms;

    /*
     * The cipher suite the server chose (0x1303 ChaCha20-Poly1305 or
     * 0x1301 AES-128-GCM), and its key length in bytes.
     */
    u32 cipher;
    u32 key_len;

    /*
     * Current keys, per direction, and how many records each has
     * protected (the count goes into each record's nonce).
     */
    u8 send_key[32];
    u8 send_iv[12];
    u64 send_seq;

    u8 recv_key[32];
    u8 recv_iv[12];
    u64 recv_seq;

    u8 recv_secret[32];

    /*
     * Decrypted application data not handed to the caller yet.
     */
    u8 plain[TLS_RECORD_MAX + 64u];
    u32 plain_len;
    u32 plain_pos;

    /*
     * One record as it travels, and handshake messages being reassembled
     * (a certificate chain can span several records).
     */
    u8 record[TLS_RECORD_MAX + 512u];
    u8 handshake[49152];
    u32 handshake_len;
} tls_conn_t;

/*
 * Connects and performs the handshake. Returns 1 on success; on failure
 * tls_error() says what went wrong.
 */
int tls_connect(tls_conn_t* c, const char* host, u16 port, u32 timeout_ms);

/*
 * Sends everything. Returns 1, or 0 on error.
 */
int tls_write(tls_conn_t* c, const void* data, u32 length);

/*
 * Returns the byte count, 0 when the connection has ended, -1 on timeout
 * or error.
 */
int tls_read(tls_conn_t* c, void* buffer, u32 capacity);

void tls_close(tls_conn_t* c);

/*
 * For testing: 0 offers both ciphers (the default), otherwise offer only
 * the given suite (0x1301 or 0x1303).
 */
void tls_only_cipher(u32 suite);

const char* tls_error(const tls_conn_t* c);

#endif
