#!/usr/bin/env python3
"""Checks the certificate code (userapps/runtime/x509.c) on the host.

Builds it with the host compiler and tests:
  - SHA-384 and SHA-512 against Python's own
  - RSA-PSS and ECDSA (P-256, P-384) signatures made with OpenSSL
  - real certificate chains fetched from real sites, good and bad

usage (from the project folder, in WSL; needs openssl and network access):
  python3 tools/tests/x509test.py
"""
import hashlib
import os
import re
import base64
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { int kind; const unsigned char* n; unsigned n_len; const unsigned char* e; unsigned e_len;
                 const unsigned char* point; unsigned point_len; } key_t_;
int x509_verify_chain(const unsigned char** certs, const unsigned* lengths, unsigned count, const char* host,
                      key_t_* key, char* why, unsigned why_size);
int x509_verify_signature(const key_t_* key, unsigned scheme, const unsigned char* msg, unsigned msg_len,
                          const unsigned char* sig, unsigned sig_len);
void sha384(const void* data, unsigned length, unsigned char* out);
void sha512(const void* data, unsigned length, unsigned char* out);

static const char* g_roots_path;

/* what the NAQ OS runtime would provide */
long long read_file(const char* path, void* buf, unsigned long size) {
    FILE* f = fopen(g_roots_path, "rb");
    (void)path;
    if (!f) return -1;
    long long n = (long long)fread(buf, 1, size, f);
    fclose(f);
    return n;
}
unsigned long long clock_read(void) {
    time_t t = time(0);
    struct tm* g = gmtime(&t);
    return ((unsigned long long)(g->tm_year + 1900) << 40) | ((unsigned long long)(g->tm_mon + 1) << 32) |
           ((unsigned long long)g->tm_mday << 24) | ((unsigned long long)g->tm_hour << 16) |
           ((unsigned long long)g->tm_min << 8) | (unsigned long long)g->tm_sec;
}
unsigned long long get_ticks(void) { return 0; }

static unsigned char* slurp(const char* path, unsigned* len) {
    FILE* f = fopen(path, "rb");
    unsigned char* b = malloc(1 << 20);
    *len = f ? (unsigned)fread(b, 1, 1 << 20, f) : 0;
    if (f) fclose(f);
    return b;
}

int main(int argc, char** argv) {
    g_roots_path = argv[2];
    if (!strcmp(argv[1], "hash")) {
        unsigned len; unsigned char* d = slurp(argv[3], &len); unsigned char out[64];
        sha384(d, len, out); for (int i = 0; i < 48; i++) printf("%02x", out[i]); printf("\n");
        sha512(d, len, out); for (int i = 0; i < 64; i++) printf("%02x", out[i]); printf("\n");
        return 0;
    }
    if (!strcmp(argv[1], "sig")) {
        /* sig <roots> <scheme hex> <cert.der> <message> <signature> */
        unsigned clen, mlen, slen; char why[100]; key_t_ key;
        const unsigned char* cert = slurp(argv[4], &clen);
        unsigned char* msg = slurp(argv[5], &mlen);
        unsigned char* sig = slurp(argv[6], &slen);
        x509_verify_chain(&cert, &clen, 1, "none", &key, why, sizeof(why));
        printf("%d", x509_verify_signature(&key, (unsigned)strtoul(argv[3], 0, 16), msg, mlen, sig, slen));
        msg[0] ^= 1;
        printf("%d\n", x509_verify_signature(&key, (unsigned)strtoul(argv[3], 0, 16), msg, mlen, sig, slen));
        return 0;
    }
    /* chain <roots> <host> <cert.der>... */
    const unsigned char* certs[8]; unsigned lens[8]; unsigned count = 0; char why[100] = ""; key_t_ key;
    for (int i = 4; i < argc && count < 8; i++) certs[count] = slurp(argv[i], &lens[count]), count++;
    int ok = x509_verify_chain(certs, lens, count, argv[3], &key, why, sizeof(why));
    printf("%s|%s|key kind %d\n", ok ? "OK" : "REFUSED", why, key.kind);
    return 0;
}
'''

SITES = [
    ('en.wikipedia.org', True), ('example.com', True), ('www.google.com', True),
    ('github.com', True), ('letsencrypt.org', True), ('www.cloudflare.com', True),
    ('duckduckgo.com', True), ('text.npr.org', True),
    ('expired.badssl.com', False), ('wrong.host.badssl.com', False),
    ('self-signed.badssl.com', False), ('untrusted-root.badssl.com', False),
]


def run(cmd, **kw):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, **kw)


def main():
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        t = lambda name: os.path.join(tmp, name)
        gen = os.path.join(ROOT, 'tools', 'gen_crypto_tables.py')
        subprocess.check_call([sys.executable, gen, 'roots', '/etc/ssl/certs/ca-certificates.crt', t('roots.bin')])
        open(t('harness.c'), 'w').write(HARNESS)
        rt = os.path.join(ROOT, 'userapps', 'runtime')
        for src in ('x509.c', 'crypto.c', 'string.c'):
            subprocess.check_call(['gcc', '-O2', '-ffreestanding', '-fno-builtin', '-fno-tree-loop-distribute-patterns',
                                   '-w', '-c', os.path.join(rt, src), '-o', t(src + '.o')])
        subprocess.check_call(['gcc', '-O1', '-w', t('harness.c'), t('x509.c.o'), t('crypto.c.o'), '-o', t('x')])

        # hashes
        for size in (0, 1, 111, 112, 127, 128, 129, 5000):
            data = bytes((i * 7 + 3) & 255 for i in range(size))
            open(t('data'), 'wb').write(data)
            got = run([t('x'), 'hash', t('roots.bin'), t('data')]).stdout.decode().split()
            want = [hashlib.sha384(data).hexdigest(), hashlib.sha512(data).hexdigest()]
            if got != want:
                print('FAIL hash of %d bytes' % size)
                failures += 1
        print('hashes checked')

        # signatures made by OpenSSL
        open(t('msg'), 'wb').write(b'NAQ OS signature test message ' * 5)
        cases = [('rsa2048', ['-newkey', 'rsa:2048'], [('0804', 'sha256'), ('0805', 'sha384'), ('0806', 'sha512')], True),
                 ('rsa4096', ['-newkey', 'rsa:4096'], [('0804', 'sha256')], True),
                 ('p256', ['-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:prime256v1'], [('0403', 'sha256')], False),
                 ('p384', ['-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:secp384r1'], [('0503', 'sha384')], False)]
        for name, keyopts, schemes, pss in cases:
            run(['openssl', 'req', '-x509', '-nodes', '-subj', '/CN=test', '-days', '2'] + keyopts +
                ['-keyout', t(name + '.key'), '-outform', 'DER', '-out', t(name + '.der')])
            for scheme, digest in schemes:
                cmd = ['openssl', 'dgst', '-' + digest, '-sign', t(name + '.key'), '-out', t('sig')]
                if pss:
                    cmd += ['-sigopt', 'rsa_padding_mode:pss', '-sigopt', 'rsa_pss_saltlen:digest']
                run(cmd + [t('msg')])
                got = run([t('x'), 'sig', t('roots.bin'), scheme, t(name + '.der'), t('msg'), t('sig')]).stdout.decode().strip()
                status = 'ok' if got == '10' else 'FAIL'
                failures += got != '10'
                print('%-4s signature %s %s (good accepted, altered refused: %s)' % (status, name, scheme, got))

        # real chains
        for host, should_pass in SITES:
            out = run(['openssl', 's_client', '-showcerts', '-connect', host + ':443', '-servername', host],
                      input=b'', timeout=30).stdout.decode('latin-1')
            blocks = re.findall(r'-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----', out, flags=re.S)
            if not blocks:
                print('skip %-28s (could not fetch its certificates)' % host)
                continue
            files = []
            for i, b in enumerate(blocks):
                path = t('%s.%d.der' % (host, i))
                open(path, 'wb').write(base64.b64decode(''.join(b.split())))
                files.append(path)
            got = run([t('x'), 'chain', t('roots.bin'), host] + files).stdout.decode().strip()
            passed = got.startswith('OK')
            status = 'ok' if passed == should_pass else 'FAIL'
            failures += passed != should_pass
            print('%-4s %-28s %d certs: %s' % (status, host, len(files), got))

    print('FAILURES: %d' % failures if failures else 'ALL OK')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
