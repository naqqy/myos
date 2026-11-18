#ifndef NAQIMAGE_H
#define NAQIMAGE_H

#include "naqos.h"

/*
 * Image decoding for apps: PNG, JPEG and GIF, plus the "inflate"
 * decompressor PNG is built on.
 *
 * Supported:
 *   PNG   all colour types, 1 to 16 bits per channel, transparency;
 *         not the interlaced (Adam7) variant
 *   JPEG  baseline (sequential Huffman), greyscale or colour, any chroma
 *         subsampling; not progressive or arithmetic-coded files
 *   GIF   the first frame, with transparency (no animation)
 *
 * Transparent pixels are blended onto white. Pixels come out as
 * 0x00RRGGBB, like everything else the apps draw.
 *
 * The decoders keep no large memory of their own: the caller supplies a
 * scratch buffer for intermediate data.
 */

/*
 * Decompresses raw "deflate" data. Returns 1 on success.
 */
int inflate_raw(const u8* in, u32 in_len, u8* out, u32 out_cap, u32* out_len);

/*
 * The same with the 2-byte zlib header that PNG uses.
 */
int inflate_zlib(const u8* in, u32 in_len, u8* out, u32 out_cap, u32* out_len);

#define IMAGE_OK          0
#define IMAGE_UNKNOWN     1     /* not a PNG, JPEG or GIF */
#define IMAGE_UNSUPPORTED 2     /* a variant the decoder does not handle */
#define IMAGE_TOO_LARGE   3     /* more pixels than the caller has room for */
#define IMAGE_DAMAGED     4

/*
 * Decodes an image file held in memory.
 *
 * pixels / max_pixels   where the picture goes and how many pixels fit
 * scratch / scratch_size   working memory; allow about 5 bytes per pixel
 *                          of the image plus the size of the file
 *
 * Returns IMAGE_OK and the size, or one of the codes above.
 */
int image_decode(
    const u8* data,
    u32 length,
    u32* pixels,
    u32 max_pixels,
    u8* scratch,
    u32 scratch_size,
    u32* out_width,
    u32* out_height
);

#endif
