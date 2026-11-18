#ifndef SBC64_H
#define SBC64_H

#include "naq_boot_info.h"

/*
 * SBC encoder.
 *
 * SBC is the sound compression every Bluetooth headset understands. It
 * splits the sound into 8 frequency bands and spends a fixed budget of
 * bits (the "bitpool") on them, more where the sound is loud.
 *
 * This encoder does one fixed form of it: stereo, 8 subbands, 16 blocks,
 * "loudness" bit allocation, at 44.1 or 48 kHz. One frame is 128 samples
 * per channel (2.7 ms at 48 kHz). Whole-number arithmetic only.
 */

#define SBC64_FRAME_SAMPLES 128u    /* per channel, per frame */

typedef struct sbc64 {
    nq_u8 rate_code;        /* 2 = 44.1 kHz, 3 = 48 kHz */
    nq_u8 bitpool;
    nq_s32 history[2][80];  /* the last 80 samples of each channel, newest first */
} sbc64_t;

void sbc64_init(sbc64_t* s, nq_u32 rate_hz, nq_u32 bitpool);

/* Bytes in every frame this encoder produces. */
nq_u32 sbc64_frame_bytes(const sbc64_t* s);

/*
 * Encodes 128 stereo samples (256 values, left/right alternating) into
 * `out`. Returns the number of bytes written (sbc64_frame_bytes()).
 */
nq_u32 sbc64_encode(sbc64_t* s, const nq_s16* pcm, nq_u8* out);

#endif
