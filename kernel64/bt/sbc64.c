#include "sbc64.h"
#include "sbc_tables64.h"

/*
 * SBC encoder. See sbc64.h.
 *
 * The steps for each frame, as laid down in the A2DP specification:
 *
 *   1. Analysis: 8 new samples at a time go into a history of 80; the
 *      history is multiplied by a fixed window, folded to 16 values and
 *      turned into 8 subband samples with a table of cosines. 16 such
 *      blocks make a frame.
 *   2. Scale factors: for each channel and subband, how large its
 *      samples are (as a power of two).
 *   3. Bit allocation: the bitpool is shared out among the subbands
 *      according to their scale factors.
 *   4. Each sample is reduced to the number of bits its subband was
 *      given, and everything is packed into the frame.
 */

#define CHANNELS 2
#define SUBBANDS 8
#define BLOCKS   16

/* How much less loud each subband may be before it matters (loudness mode). */
static const nq_s8 g_offset_44_48[SUBBANDS] = { -4, 0, 0, 0, 0, 0, 1, 2 };

void sbc64_init(sbc64_t* s, nq_u32 rate_hz, nq_u32 bitpool) {
    s->rate_code = (rate_hz == 44100u) ? 2u : 3u;
    s->bitpool = (nq_u8)(bitpool < 2u ? 2u : (bitpool > 250u ? 250u : bitpool));

    for (int ch = 0; ch < CHANNELS; ch++) {
        for (int i = 0; i < 80; i++) {
            s->history[ch][i] = 0;
        }
    }
}

nq_u32 sbc64_frame_bytes(const sbc64_t* s) {
    /* header 4, scale factors 2 * 8 * 4 bits, samples 16 blocks * bitpool bits */
    return 4u + (CHANNELS * SUBBANDS * 4u) / 8u + (BLOCKS * (nq_u32)s->bitpool + 7u) / 8u;
}

/* One block: 8 new samples of one channel in, 8 subband samples out. */
static void analyse(nq_s32* history, const nq_s16* pcm, nq_s32* out) {
    nq_s64 folded[16];

    for (int i = 79; i >= 8; i--) {
        history[i] = history[i - 8];
    }

    for (int i = 0; i < 8; i++) {
        history[7 - i] = pcm[i * 2];
    }

    for (int i = 0; i < 16; i++) {
        nq_s64 sum = 0;

        for (int k = 0; k < 5; k++) {
            sum += (nq_s64)g_sbc_proto[i + 16 * k] * history[i + 16 * k];
        }

        folded[i] = sum;            /* scaled by 2^24 */
    }

    for (int sb = 0; sb < SUBBANDS; sb++) {
        nq_s64 sum = 0;

        for (int i = 0; i < 16; i++) {
            sum += folded[i] * g_sbc_cos[sb][i];
        }

        /* scaled by 2^24 * 2^14; round to a whole sample value */
        sum = (sum + ((nq_s64)1 << 37)) >> 38;

        if (sum > 65535) sum = 65535;
        if (sum < -65535) sum = -65535;

        out[sb] = (nq_s32)sum;
    }
}

/* Shares the bitpool out among the 16 channel/subband pairs. */
static void allocate(const nq_u8 scale[CHANNELS][SUBBANDS], int bitpool,
                     nq_u8 bits[CHANNELS][SUBBANDS]) {
    int need[CHANNELS][SUBBANDS];
    int max_need = 0;

    for (int ch = 0; ch < CHANNELS; ch++) {
        for (int sb = 0; sb < SUBBANDS; sb++) {
            int n;

            if (scale[ch][sb] == 0) {
                n = -5;
            } else {
                int loudness = (int)scale[ch][sb] - g_offset_44_48[sb];

                n = loudness > 0 ? loudness / 2 : loudness;
            }

            need[ch][sb] = n;

            if (n > max_need) {
                max_need = n;
            }
        }
    }

    int count = 0;
    int slice_count = 0;
    int slice = max_need + 1;

    do {
        slice--;
        count += slice_count;
        slice_count = 0;

        for (int ch = 0; ch < CHANNELS; ch++) {
            for (int sb = 0; sb < SUBBANDS; sb++) {
                if (need[ch][sb] > slice + 1 && need[ch][sb] < slice + 16) {
                    slice_count++;
                } else if (need[ch][sb] == slice + 1) {
                    slice_count += 2;
                }
            }
        }
    } while (count + slice_count < bitpool);

    if (count + slice_count == bitpool) {
        count += slice_count;
        slice--;
    }

    for (int ch = 0; ch < CHANNELS; ch++) {
        for (int sb = 0; sb < SUBBANDS; sb++) {
            if (need[ch][sb] < slice + 2) {
                bits[ch][sb] = 0;
            } else {
                int b = need[ch][sb] - slice;

                bits[ch][sb] = (nq_u8)(b > 16 ? 16 : b);
            }
        }
    }

    /* Bits left over go round the subbands, lowest first. */
    int ch = 0;
    int sb = 0;

    while (count < bitpool && sb < SUBBANDS) {
        if (bits[ch][sb] >= 2 && bits[ch][sb] < 16) {
            bits[ch][sb]++;
            count++;
        } else if (need[ch][sb] == slice + 1 && bitpool > count + 1) {
            bits[ch][sb] = 2;
            count += 2;
        }

        if (ch == 1) {
            ch = 0;
            sb++;
        } else {
            ch = 1;
        }
    }

    ch = 0;
    sb = 0;

    while (count < bitpool && sb < SUBBANDS) {
        if (bits[ch][sb] < 16) {
            bits[ch][sb]++;
            count++;
        }

        if (ch == 1) {
            ch = 0;
            sb++;
        } else {
            ch = 1;
        }
    }
}

/* The frame's check value: CRC-8 with polynomial 0x1D, over `bit_count` bits. */
static nq_u8 crc8(const nq_u8* data, nq_u32 bit_count) {
    nq_u8 crc = 0x0F;

    for (nq_u32 i = 0; i < bit_count; i++) {
        nq_u8 bit = (nq_u8)((data[i / 8u] >> (7u - (i % 8u))) & 1u);
        nq_u8 top = (nq_u8)((crc >> 7) & 1u);

        crc = (nq_u8)(crc << 1);

        if (top ^ bit) {
            crc ^= 0x1D;
        }
    }

    return crc;
}

typedef struct bit_writer {
    nq_u8* out;
    nq_u32 at;          /* in bits */
} bit_writer_t;

static void put_bits(bit_writer_t* w, nq_u32 value, nq_u32 count) {
    for (nq_u32 i = 0; i < count; i++) {
        nq_u32 bit = (value >> (count - 1u - i)) & 1u;
        nq_u32 byte = w->at / 8u;
        nq_u32 shift = 7u - (w->at % 8u);

        if (shift == 7u) {
            w->out[byte] = 0;
        }

        w->out[byte] = (nq_u8)(w->out[byte] | (bit << shift));
        w->at++;
    }
}

nq_u32 sbc64_encode(sbc64_t* s, const nq_s16* pcm, nq_u8* out) {
    static nq_s32 samples[BLOCKS][CHANNELS][SUBBANDS];
    nq_u8 scale[CHANNELS][SUBBANDS];
    nq_u8 bits[CHANNELS][SUBBANDS];

    for (int blk = 0; blk < BLOCKS; blk++) {
        for (int ch = 0; ch < CHANNELS; ch++) {
            analyse(s->history[ch], pcm + blk * 16 + ch, samples[blk][ch]);
        }
    }

    /* Scale factor: the smallest f with every |sample| < 2^(f + 1). */
    for (int ch = 0; ch < CHANNELS; ch++) {
        for (int sb = 0; sb < SUBBANDS; sb++) {
            nq_s32 peak = 0;

            for (int blk = 0; blk < BLOCKS; blk++) {
                nq_s32 v = samples[blk][ch][sb];

                if (v < 0) v = -v;
                if (v > peak) peak = v;
            }

            nq_u8 f = 0;

            while (f < 15 && peak >= ((nq_s32)2 << f)) {
                f++;
            }

            scale[ch][sb] = f;
        }
    }

    allocate(scale, s->bitpool, bits);

    /*
     * Header: sync, then rate / 16 blocks / stereo / loudness / 8 subbands,
     * then the bitpool and the check value.
     */
    out[0] = 0x9C;
    out[1] = (nq_u8)((s->rate_code << 6) | (3u << 4) | (2u << 2) | (0u << 1) | 1u);
    out[2] = s->bitpool;
    out[3] = 0;

    bit_writer_t w;

    w.out = out;
    w.at = 32;

    for (int ch = 0; ch < CHANNELS; ch++) {
        for (int sb = 0; sb < SUBBANDS; sb++) {
            put_bits(&w, scale[ch][sb], 4);
        }
    }

    /* The check covers bytes 1 and 2 and the scale factors. */
    nq_u8 covered[2 + 8];

    covered[0] = out[1];
    covered[1] = out[2];

    for (int i = 0; i < 8; i++) {
        covered[2 + i] = out[4 + i];
    }

    out[3] = crc8(covered, 16u + 64u);

    for (int blk = 0; blk < BLOCKS; blk++) {
        for (int ch = 0; ch < CHANNELS; ch++) {
            for (int sb = 0; sb < SUBBANDS; sb++) {
                nq_u32 n = bits[ch][sb];

                if (!n) {
                    continue;
                }

                /*
                 * Map the sample from -2^(f+1) .. 2^(f+1) onto
                 * 0 .. 2^n - 1.
                 */
                nq_u32 levels = (1u << n) - 1u;
                nq_s64 range = (nq_s64)2 << scale[ch][sb];
                nq_s64 q = (((nq_s64)samples[blk][ch][sb] + range) * levels) / (2 * range);

                if (q < 0) q = 0;
                if (q > (nq_s64)levels) q = levels;

                put_bits(&w, (nq_u32)q, n);
            }
        }
    }

    /* pad the last byte with zeros */
    while (w.at % 8u) {
        put_bits(&w, 0, 1);
    }

    return w.at / 8u;
}
