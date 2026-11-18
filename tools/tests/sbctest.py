#!/usr/bin/env python3
"""Checks the kernel's SBC encoder (kernel64/bt/sbc64.c) on the host.

Builds the encoder with the host compiler, encodes a test signal, decodes
the frames with an independent decoder written here from the A2DP
specification (floating point, straightforward), and reports how close the
result is to the original.

usage (from the project folder, in WSL):  python3 tools/tests/sbctest.py
"""
import math
import os
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
from gen_sbc_tables import PROTO  # noqa: E402

HARNESS = r'''
#include <stdio.h>
#include "sbc64.h"
int main(int argc, char** argv) {
    sbc64_t s;
    short pcm[256];
    unsigned char out[1024];
    sbc64_init(&s, (unsigned)atoi(argv[1]), (unsigned)atoi(argv[2]));
    while (fread(pcm, 2, 256, stdin) == 256) {
        unsigned n = sbc64_encode(&s, pcm, out);
        if (n != sbc64_frame_bytes(&s)) { fprintf(stderr, "size mismatch %u\n", n); return 1; }
        fwrite(out, 1, n, stdout);
    }
    return 0;
}
'''

OFFSET8 = [-4, 0, 0, 0, 0, 0, 1, 2]


def crc8(bits):
    crc = 0x0F
    for b in bits:
        top = (crc >> 7) & 1
        crc = (crc << 1) & 0xFF
        if top ^ b:
            crc ^= 0x1D
    return crc


def allocate(scale, bitpool):
    need = [[0] * 8 for _ in range(2)]
    for ch in range(2):
        for sb in range(8):
            if scale[ch][sb] == 0:
                need[ch][sb] = -5
            else:
                loud = scale[ch][sb] - OFFSET8[sb]
                need[ch][sb] = loud // 2 if loud > 0 else loud
    max_need = max(max(r) for r in need)
    count, slice_count, sl = 0, 0, max_need + 1
    while True:
        sl -= 1
        count += slice_count
        slice_count = 0
        for ch in range(2):
            for sb in range(8):
                if sl + 1 < need[ch][sb] < sl + 16:
                    slice_count += 1
                elif need[ch][sb] == sl + 1:
                    slice_count += 2
        if count + slice_count >= bitpool:
            break
    if count + slice_count == bitpool:
        count += slice_count
        sl -= 1
    bits = [[0] * 8 for _ in range(2)]
    for ch in range(2):
        for sb in range(8):
            bits[ch][sb] = 0 if need[ch][sb] < sl + 2 else min(need[ch][sb] - sl, 16)
    for final in (False, True):
        ch, sb = 0, 0
        while count < bitpool and sb < 8:
            if not final:
                if 2 <= bits[ch][sb] < 16:
                    bits[ch][sb] += 1
                    count += 1
                elif need[ch][sb] == sl + 1 and bitpool > count + 1:
                    bits[ch][sb] = 2
                    count += 2
            elif bits[ch][sb] < 16:
                bits[ch][sb] += 1
                count += 1
            if ch == 1:
                ch, sb = 0, sb + 1
            else:
                ch = 1
    return bits


class Decoder:
    def __init__(self):
        self.v = [[0.0] * 160 for _ in range(2)]
        self.matrix = [[math.cos((i + 4) * (2 * k + 1) * math.pi / 16) for k in range(8)] for i in range(16)]

    def frame(self, data):
        assert data[0] == 0x9C, 'bad sync byte'
        assert data[1] & 0x3F == 0x39, 'not 16 blocks / stereo / loudness / 8 subbands'
        bitpool = data[2]
        allbits = []
        for byte in data:
            allbits.extend((byte >> (7 - i)) & 1 for i in range(8))
        pos = 32

        def take(n):
            nonlocal pos
            value = 0
            for _ in range(n):
                value = (value << 1) | allbits[pos]
                pos += 1
            return value

        scale = [[take(4) for _ in range(8)] for _ in range(2)]
        assert crc8(allbits[8:24] + allbits[32:96]) == data[3], 'check value (CRC) wrong'
        bits = allocate(scale, bitpool)
        assert sum(sum(r) for r in bits) <= bitpool, 'allocation exceeds the bitpool'
        out = []
        for _ in range(16):
            sbs = [[0.0] * 8 for _ in range(2)]
            for ch in range(2):
                for sb in range(8):
                    n = bits[ch][sb]
                    if n:
                        levels = (1 << n) - 1
                        q = take(n)
                        sbs[ch][sb] = (2 ** (scale[ch][sb] + 1)) * ((q * 2.0 + 1.0) / levels - 1.0)
            block = [[0.0] * 8 for _ in range(2)]
            for ch in range(2):
                v = self.v[ch]
                v[16:] = v[:-16]
                for i in range(16):
                    v[i] = sum(self.matrix[i][k] * sbs[ch][k] for k in range(8))
                u = [0.0] * 80
                for i in range(5):
                    for j in range(8):
                        u[i * 16 + j] = v[i * 32 + j]
                        u[i * 16 + 8 + j] = v[i * 32 + 24 + j]
                for j in range(8):
                    block[ch][j] = sum(u[j + 8 * i] * PROTO[j + 8 * i] * -8.0 for i in range(10))
            for j in range(8):
                out.append((block[0][j], block[1][j]))
        return out, (pos + 7) // 8


def main():
    rate, bitpool, seconds = 48000, 51, 0.5
    frames = int(rate * seconds) // 128 * 128
    pcm = []
    for n in range(frames):
        t = n / rate
        left = 9000 * math.sin(2 * math.pi * 440 * t) + 4000 * math.sin(2 * math.pi * 3000 * t)
        right = 8000 * math.sin(2 * math.pi * (300 + 6000 * t) * t)
        pcm.append((int(left), int(right)))

    with tempfile.TemporaryDirectory() as tmp:
        tables = os.path.join(tmp, 'sbc_tables64.h')
        subprocess.check_call([sys.executable, os.path.join(ROOT, 'tools', 'gen_sbc_tables.py'), tables])
        src = os.path.join(tmp, 'harness.c')
        open(src, 'w').write('#include <stdlib.h>\n' + HARNESS)
        exe = os.path.join(tmp, 'sbcenc')
        subprocess.check_call(['gcc', '-O1', '-Wall', '-I' + tmp, '-I' + os.path.join(ROOT, 'kernel64', 'bt'),
                               '-I' + os.path.join(ROOT, 'include'), src,
                               os.path.join(ROOT, 'kernel64', 'bt', 'sbc64.c'), '-o', exe])
        raw = b''.join(struct.pack('<hh', l, r) for l, r in pcm)
        encoded = subprocess.run([exe, str(rate), str(bitpool)], input=raw, stdout=subprocess.PIPE, check=True).stdout

    frame_bytes = 4 + 8 + (16 * bitpool + 7) // 8
    count = len(encoded) // frame_bytes
    print('frames: %d of %d bytes each (%.0f kbit/s)' % (count, frame_bytes, frame_bytes * 8 * rate / 128 / 1000))
    assert count == frames // 128 and len(encoded) % frame_bytes == 0

    dec = Decoder()
    out = []
    for f in range(count):
        samples, used = dec.frame(encoded[f * frame_bytes:(f + 1) * frame_bytes])
        assert used == frame_bytes, 'frame length %d, expected %d' % (used, frame_bytes)
        out.extend(samples)

    # The filters delay the sound by 73 samples in each direction... find the
    # delay and sign that match best, then measure the error.
    best = None
    for delay in range(60, 90):
        for sign in (1, -1):
            num = sum(sign * out[n + delay][0] * pcm[n][0] for n in range(2000, 6000))
            if best is None or num > best[0]:
                best = (num, delay, sign)
    _, delay, sign = best
    for ch, name in ((0, 'left'), (1, 'right')):
        signal = sum(pcm[n][ch] ** 2 for n in range(1000, frames - 200))
        noise = sum((sign * out[n + delay][ch] - pcm[n][ch]) ** 2 for n in range(1000, frames - 200))
        snr = 10 * math.log10(signal / max(noise, 1e-9))
        print('%s: delay %d samples, sign %+d, signal-to-noise %.1f dB' % (name, delay, sign, snr))
        assert snr > 20, 'decoded sound does not match the original'
    print('OK')


if __name__ == '__main__':
    main()
