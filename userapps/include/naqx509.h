#ifndef NAQX509_H
#define NAQX509_H

#include "naqos.h"

/*
 * Checking who a secure server is.
 *
 * A server proves its identity with a certificate: a signed statement
 * "this public key belongs to example.com", issued by a certificate
 * authority, whose own certificate may be signed by another, up to a
 * "root" that the system trusts from the start. The list of trusted roots
 * is the file /system/roots.bin (Mozilla's list, packed at build time).
 *
 * x509_verify_chain() checks all of that: every signature in the chain,
 * that it ends at a trusted root, that the dates are current, and that
 * the certificate names the host being visited. tls.c then checks that
 * the server really holds the certificate's key (x509_verify_signature).
 *
 * Supported: RSA (PKCS#1 v1.5 and PSS) with SHA-256/384/512, and ECDSA on
 * the P-256 and P-384 curves. Not checked: revocation (whether a
 * certificate was withdrawn early) and name constraints.
 */

#define X509_KEY_RSA  1
#define X509_KEY_P256 2
#define X509_KEY_P384 3

typedef struct x509_key {
    int kind;
    const u8* n;        /* RSA: modulus and exponent, big-endian */
    u32 n_len;
    const u8* e;
    u32 e_len;
    const u8* point;    /* curves: 04, X, Y */
    u32 point_len;
} x509_key_t;

/*
 * certs[0] is the server's own certificate, followed by the others it
 * sent, each as DER. Returns 1 if the chain is good for `host`; otherwise
 * 0 with the reason, in plain words, in `why`. The server's key is
 * returned either way if its certificate could be read.
 */
int x509_verify_chain(const u8** certs, const u32* lengths, u32 count, const char* host,
                      x509_key_t* server_key, char* why, u32 why_size);

/*
 * Checks a TLS 1.3 signature. scheme is the TLS number: 0x0403 / 0x0503
 * (ECDSA with SHA-256 / SHA-384), 0x0804..0x0806 (RSA-PSS with
 * SHA-256/384/512). Returns 1 if it is valid.
 */
int x509_verify_signature(const x509_key_t* key, u32 scheme, const u8* message, u32 message_len,
                          const u8* signature, u32 signature_len);

void sha384(const void* data, u32 length, u8 out[48]);
void sha512(const void* data, u32 length, u8 out[64]);

#endif
