#include "../include/naqimage.h"

/*
 * Image decoders. See naqimage.h for what is and is not supported.
 */

static u32 be32(const u8* p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

static u32 be16(const u8* p) {
    return ((u32)p[0] << 8) | (u32)p[1];
}

static u32 le16(const u8* p) {
    return (u32)p[0] | ((u32)p[1] << 8);
}

/*
 * Blends a colour with the given opacity (0..255) onto white.
 */
static u32 over_white(u32 r, u32 g, u32 b, u32 a) {
    r = (r * a + 255u * (255u - a)) / 255u;
    g = (g * a + 255u * (255u - a)) / 255u;
    b = (b * a + 255u * (255u - a)) / 255u;

    return (r << 16) | (g << 8) | b;
}

/* -------------------------------------------------------------------------- */
/* Inflate                                                                     */
/* -------------------------------------------------------------------------- */

/*
 * "Deflate" data is a series of blocks. Each is either stored as is, or
 * a stream of Huffman-coded symbols: literal bytes, and (length,
 * distance) pairs meaning "copy `length` bytes from `distance` back in
 * the output". The code tables are either fixed by the standard or
 * described at the start of the block.
 *
 * Huffman codes here are "canonical": fully determined by how many codes
 * there are of each bit length, so a table is just those counts plus the
 * symbols in order.
 */
typedef struct inflate_state {
    const u8* in;
    u32 in_len;
    u32 in_pos;

    u32 bit_buffer;
    u32 bit_count;

    u8* out;
    u32 out_cap;
    u32 out_pos;

    int failed;
} inflate_state_t;

typedef struct huffman {
    u16 count[16];
    u16 symbol[288];
} huffman_t;

static u32 take_bits(inflate_state_t* s, u32 need) {
    u32 value = s->bit_buffer;

    while (s->bit_count < need) {
        if (s->in_pos >= s->in_len) {
            s->failed = 1;
            return 0;
        }

        value |= (u32)s->in[s->in_pos++] << s->bit_count;
        s->bit_count += 8;
    }

    s->bit_buffer = need < 32u ? value >> need : 0;
    s->bit_count -= need;

    return need < 32u ? value & ((1u << need) - 1u) : value;
}

static int huffman_decode(inflate_state_t* s, const huffman_t* h) {
    int code = 0;
    int first = 0;
    int index = 0;

    for (int len = 1; len <= 15; len++) {
        code |= (int)take_bits(s, 1);

        if (s->failed) {
            return -1;
        }

        int count = h->count[len];

        if (code - count < first) {
            return h->symbol[index + (code - first)];
        }

        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }

    return -1;
}

/*
 * Builds a table from the code length of each symbol. Returns 0 for a
 * complete code, negative if the lengths are impossible.
 */
static int huffman_build(huffman_t* h, const u16* lengths, int n) {
    u16 offsets[16];

    for (int len = 0; len <= 15; len++) {
        h->count[len] = 0;
    }

    for (int sym = 0; sym < n; sym++) {
        h->count[lengths[sym]]++;
    }

    if (h->count[0] == n) {
        return 0;
    }

    int left = 1;

    for (int len = 1; len <= 15; len++) {
        left <<= 1;
        left -= h->count[len];

        if (left < 0) {
            return left;
        }
    }

    offsets[1] = 0;

    for (int len = 1; len < 15; len++) {
        offsets[len + 1] = (u16)(offsets[len] + h->count[len]);
    }

    for (int sym = 0; sym < n; sym++) {
        if (lengths[sym] != 0) {
            h->symbol[offsets[lengths[sym]]++] = (u16)sym;
        }
    }

    return left;
}

static int inflate_codes(inflate_state_t* s, const huffman_t* lit, const huffman_t* dist) {
    static const u16 length_base[29] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
    };

    static const u16 length_extra[29] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
    };

    static const u16 dist_base[30] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
        8193, 12289, 16385, 24577
    };

    static const u16 dist_extra[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
    };

    for (;;) {
        int symbol = huffman_decode(s, lit);

        if (symbol < 0) {
            return 0;
        }

        if (symbol < 256) {
            if (s->out_pos >= s->out_cap) {
                return 0;
            }

            s->out[s->out_pos++] = (u8)symbol;
            continue;
        }

        if (symbol == 256) {
            return 1;
        }

        symbol -= 257;

        if (symbol >= 29) {
            return 0;
        }

        u32 length = length_base[symbol] + take_bits(s, length_extra[symbol]);

        symbol = huffman_decode(s, dist);

        if (symbol < 0 || symbol >= 30) {
            return 0;
        }

        u32 distance = dist_base[symbol] + take_bits(s, dist_extra[symbol]);

        if (s->failed || distance > s->out_pos || s->out_pos + length > s->out_cap) {
            return 0;
        }

        /*
         * Byte by byte on purpose: the copy may overlap its own output
         * (that is how a run of one repeated byte is coded).
         */
        for (u32 i = 0; i < length; i++) {
            s->out[s->out_pos] = s->out[s->out_pos - distance];
            s->out_pos++;
        }
    }
}

static int inflate_stored(inflate_state_t* s) {
    s->bit_buffer = 0;
    s->bit_count = 0;

    if (s->in_pos + 4u > s->in_len) {
        return 0;
    }

    u32 length = le16(s->in + s->in_pos);
    u32 check = le16(s->in + s->in_pos + 2u);

    s->in_pos += 4u;

    if (length != (~check & 0xFFFFu) ||
        s->in_pos + length > s->in_len ||
        s->out_pos + length > s->out_cap) {
        return 0;
    }

    for (u32 i = 0; i < length; i++) {
        s->out[s->out_pos++] = s->in[s->in_pos++];
    }

    return 1;
}

static int inflate_fixed(inflate_state_t* s) {
    static huffman_t lit;
    static huffman_t dist;
    static int built = 0;

    if (!built) {
        u16 lengths[288];
        int sym = 0;

        for (; sym < 144; sym++) {
            lengths[sym] = 8;
        }

        for (; sym < 256; sym++) {
            lengths[sym] = 9;
        }

        for (; sym < 280; sym++) {
            lengths[sym] = 7;
        }

        for (; sym < 288; sym++) {
            lengths[sym] = 8;
        }

        huffman_build(&lit, lengths, 288);

        for (sym = 0; sym < 30; sym++) {
            lengths[sym] = 5;
        }

        huffman_build(&dist, lengths, 30);

        built = 1;
    }

    return inflate_codes(s, &lit, &dist);
}

static int inflate_dynamic(inflate_state_t* s) {
    static const u8 order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };

    static huffman_t lit;
    static huffman_t dist;

    u16 lengths[320];

    u32 nlen = take_bits(s, 5) + 257u;
    u32 ndist = take_bits(s, 5) + 1u;
    u32 ncode = take_bits(s, 4) + 4u;

    if (s->failed || nlen > 286u || ndist > 30u) {
        return 0;
    }

    /*
     * The code lengths are themselves Huffman coded; first comes the
     * small table used to read them.
     */
    for (u32 i = 0; i < 19; i++) {
        lengths[order[i]] = (u16)(i < ncode ? take_bits(s, 3) : 0);
    }

    if (huffman_build(&lit, lengths, 19) != 0) {
        return 0;
    }

    for (u32 index = 0; index < nlen + ndist; ) {
        int symbol = huffman_decode(s, &lit);

        if (symbol < 0) {
            return 0;
        }

        if (symbol < 16) {
            lengths[index++] = (u16)symbol;
            continue;
        }

        u16 value = 0;
        u32 repeat;

        if (symbol == 16) {
            if (index == 0) {
                return 0;
            }

            value = lengths[index - 1];
            repeat = 3u + take_bits(s, 2);
        } else if (symbol == 17) {
            repeat = 3u + take_bits(s, 3);
        } else {
            repeat = 11u + take_bits(s, 7);
        }

        if (index + repeat > nlen + ndist) {
            return 0;
        }

        while (repeat--) {
            lengths[index++] = value;
        }
    }

    if (s->failed || lengths[256] == 0) {
        return 0;
    }

    int result = huffman_build(&lit, lengths, (int)nlen);

    if (result < 0 || (result > 0 && nlen - lit.count[0] != 1)) {
        return 0;
    }

    result = huffman_build(&dist, lengths + nlen, (int)ndist);

    if (result < 0 || (result > 0 && ndist - dist.count[0] != 1)) {
        return 0;
    }

    return inflate_codes(s, &lit, &dist);
}

int inflate_raw(const u8* in, u32 in_len, u8* out, u32 out_cap, u32* out_len) {
    inflate_state_t s;

    s.in = in;
    s.in_len = in_len;
    s.in_pos = 0;
    s.bit_buffer = 0;
    s.bit_count = 0;
    s.out = out;
    s.out_cap = out_cap;
    s.out_pos = 0;
    s.failed = 0;

    for (;;) {
        u32 last = take_bits(&s, 1);
        u32 type = take_bits(&s, 2);
        int ok;

        if (s.failed) {
            return 0;
        }

        if (type == 0) {
            ok = inflate_stored(&s);
        } else if (type == 1) {
            ok = inflate_fixed(&s);
        } else if (type == 2) {
            ok = inflate_dynamic(&s);
        } else {
            ok = 0;
        }

        if (!ok) {
            return 0;
        }

        if (last) {
            break;
        }
    }

    if (out_len) {
        *out_len = s.out_pos;
    }

    return 1;
}

int inflate_zlib(const u8* in, u32 in_len, u8* out, u32 out_cap, u32* out_len) {
    /*
     * Two header bytes: method 8 (deflate) in the low half of the first,
     * and no preset dictionary (bit 5 of the second).
     */
    if (in_len < 2u || (in[0] & 0x0Fu) != 8u || (in[1] & 0x20u)) {
        return 0;
    }

    return inflate_raw(in + 2, in_len - 2u, out, out_cap, out_len);
}

/* -------------------------------------------------------------------------- */
/* PNG                                                                         */
/* -------------------------------------------------------------------------- */

static u8 paeth(u8 a, u8 b, u8 c) {
    int p = (int)a + (int)b - (int)c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;

    if (pa <= pb && pa <= pc) {
        return a;
    }

    return pb <= pc ? b : c;
}

static int decode_png(
    const u8* data,
    u32 length,
    u32* pixels,
    u32 max_pixels,
    u8* scratch,
    u32 scratch_size,
    u32* out_width,
    u32* out_height
) {
    u32 width = 0;
    u32 height = 0;
    u32 depth = 0;
    u32 color_type = 0;
    u32 interlace = 0;

    u8 palette[256][3];
    u8 palette_alpha[256];
    u32 palette_count = 0;

    int have_key = 0;
    u32 key_r = 0;
    u32 key_g = 0;
    u32 key_b = 0;

    u32 packed = 0;         /* compressed bytes gathered at scratch[0..] */
    int have_header = 0;

    for (u32 i = 0; i < 256; i++) {
        palette_alpha[i] = 255;
    }

    /*
     * A PNG is a signature followed by chunks: [length][type][data][crc].
     */
    for (u32 at = 8; at + 12u <= length; ) {
        u32 size = be32(data + at);
        const u8* type = data + at + 4;
        const u8* body = data + at + 8;

        if (size > length - at - 12u) {
            return IMAGE_DAMAGED;
        }

        if (type[0] == 'I' && type[1] == 'H' && type[2] == 'D' && type[3] == 'R' && size >= 13u) {
            width = be32(body);
            height = be32(body + 4);
            depth = body[8];
            color_type = body[9];
            interlace = body[12];
            have_header = 1;
        } else if (type[0] == 'P' && type[1] == 'L' && type[2] == 'T' && type[3] == 'E') {
            palette_count = size / 3u;

            if (palette_count > 256u) {
                palette_count = 256u;
            }

            for (u32 i = 0; i < palette_count; i++) {
                palette[i][0] = body[i * 3];
                palette[i][1] = body[i * 3 + 1];
                palette[i][2] = body[i * 3 + 2];
            }
        } else if (type[0] == 't' && type[1] == 'R' && type[2] == 'N' && type[3] == 'S') {
            if (color_type == 3) {
                for (u32 i = 0; i < size && i < 256u; i++) {
                    palette_alpha[i] = body[i];
                }
            } else if (color_type == 0 && size >= 2u) {
                have_key = 1;
                key_r = be16(body);
            } else if (color_type == 2 && size >= 6u) {
                have_key = 1;
                key_r = be16(body);
                key_g = be16(body + 2);
                key_b = be16(body + 4);
            }
        } else if (type[0] == 'I' && type[1] == 'D' && type[2] == 'A' && type[3] == 'T') {
            if (packed + size > scratch_size) {
                return IMAGE_TOO_LARGE;
            }

            for (u32 i = 0; i < size; i++) {
                scratch[packed + i] = body[i];
            }

            packed += size;
        } else if (type[0] == 'I' && type[1] == 'E' && type[2] == 'N' && type[3] == 'D') {
            break;
        }

        at += 12u + size;
    }

    if (!have_header || width == 0 || height == 0) {
        return IMAGE_DAMAGED;
    }

    if (interlace != 0) {
        return IMAGE_UNSUPPORTED;
    }

    if ((u64)width * height > max_pixels) {
        return IMAGE_TOO_LARGE;
    }

    /*
     * Samples per pixel: grey 1, RGB 3, palette 1, grey+alpha 2, RGBA 4.
     */
    u32 channels;

    switch (color_type) {
        case 0: channels = 1; break;
        case 2: channels = 3; break;
        case 3: channels = 1; break;
        case 4: channels = 2; break;
        case 6: channels = 4; break;
        default: return IMAGE_UNSUPPORTED;
    }

    if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16) {
        return IMAGE_UNSUPPORTED;
    }

    u32 bits_per_pixel = channels * depth;
    u32 bytes_per_pixel = (bits_per_pixel + 7u) / 8u;
    u32 row_bytes = (width * bits_per_pixel + 7u) / 8u;
    u64 raw_size = (u64)(row_bytes + 1u) * height;

    if (packed + raw_size > scratch_size) {
        return IMAGE_TOO_LARGE;
    }

    u8* raw = scratch + packed;
    u32 got = 0;

    if (!inflate_zlib(scratch, packed, raw, (u32)raw_size, &got) || got < raw_size) {
        return IMAGE_DAMAGED;
    }

    /*
     * Every row starts with a filter type: the row was stored as the
     * difference from its left / upper / upper-left neighbours.
     */
    for (u32 y = 0; y < height; y++) {
        u8* row = raw + (u64)y * (row_bytes + 1u);
        u8 filter = row[0];
        u8* cur = row + 1;
        const u8* up = y ? row - row_bytes : 0;

        for (u32 x = 0; x < row_bytes; x++) {
            u8 a = x >= bytes_per_pixel ? cur[x - bytes_per_pixel] : 0;
            u8 b = up ? up[x] : 0;
            u8 c = (up && x >= bytes_per_pixel) ? up[x - bytes_per_pixel] : 0;

            switch (filter) {
                case 1: cur[x] = (u8)(cur[x] + a); break;
                case 2: cur[x] = (u8)(cur[x] + b); break;
                case 3: cur[x] = (u8)(cur[x] + (u8)(((u32)a + (u32)b) / 2u)); break;
                case 4: cur[x] = (u8)(cur[x] + paeth(a, b, c)); break;
                default: break;
            }
        }

        u32* out = pixels + (u64)y * width;

        for (u32 x = 0; x < width; x++) {
            u32 sample[4];

            for (u32 ch = 0; ch < channels; ch++) {
                if (depth == 8) {
                    sample[ch] = cur[x * channels + ch];
                } else if (depth == 16) {
                    sample[ch] = be16(cur + (x * channels + ch) * 2u);
                } else {
                    /*
                     * Packed bits, most significant first.
                     */
                    u32 bit = x * depth;
                    u32 shift = 8u - depth - (bit & 7u);

                    sample[ch] = (cur[bit >> 3] >> shift) & ((1u << depth) - 1u);
                }
            }

            u32 r;
            u32 g;
            u32 b;
            u32 a = 255;

            if (color_type == 3) {
                u32 index = sample[0] < palette_count ? sample[0] : 0;

                r = palette[index][0];
                g = palette[index][1];
                b = palette[index][2];
                a = palette_alpha[index];
            } else {
                if (have_key && color_type == 0 && sample[0] == key_r) {
                    a = 0;
                } else if (have_key && color_type == 2 &&
                           sample[0] == key_r && sample[1] == key_g && sample[2] == key_b) {
                    a = 0;
                }

                /*
                 * Bring every sample to 8 bits.
                 */
                for (u32 ch = 0; ch < channels; ch++) {
                    if (depth == 16) {
                        sample[ch] >>= 8;
                    } else if (depth < 8) {
                        sample[ch] = (sample[ch] * 255u) / ((1u << depth) - 1u);
                    }
                }

                if (color_type == 0 || color_type == 4) {
                    r = sample[0];
                    g = sample[0];
                    b = sample[0];

                    if (color_type == 4) {
                        a = sample[1];
                    }
                } else {
                    r = sample[0];
                    g = sample[1];
                    b = sample[2];

                    if (color_type == 6) {
                        a = sample[3];
                    }
                }
            }

            out[x] = over_white(r, g, b, a);
        }
    }

    *out_width = width;
    *out_height = height;

    return IMAGE_OK;
}

/* -------------------------------------------------------------------------- */
/* GIF                                                                         */
/* -------------------------------------------------------------------------- */

static int decode_gif(
    const u8* data,
    u32 length,
    u32* pixels,
    u32 max_pixels,
    u8* scratch,
    u32 scratch_size,
    u32* out_width,
    u32* out_height
) {
    if (length < 13u) {
        return IMAGE_DAMAGED;
    }

    u32 width = le16(data + 6);
    u32 height = le16(data + 8);
    u32 flags = data[10];
    u32 at = 13;

    const u8* global_table = 0;
    u32 global_count = 0;

    if (width == 0 || height == 0) {
        return IMAGE_DAMAGED;
    }

    if ((u64)width * height > max_pixels) {
        return IMAGE_TOO_LARGE;
    }

    if (flags & 0x80u) {
        global_count = 1u << ((flags & 7u) + 1u);
        global_table = data + at;
        at += global_count * 3u;
    }

    for (u32 i = 0; i < width * height; i++) {
        pixels[i] = 0x00FFFFFFu;
    }

    int transparent = -1;

    while (at < length) {
        u8 block = data[at++];

        if (block == 0x3B) {
            break;
        }

        if (block == 0x21) {
            /*
             * Extension. The "graphic control" one (0xF9) carries the
             * transparent colour index for the next image.
             */
            if (at >= length) {
                break;
            }

            u8 label = data[at++];

            if (label == 0xF9 && at + 5u <= length && data[at] >= 4) {
                if (data[at + 1] & 1u) {
                    transparent = data[at + 4];
                }
            }

            while (at < length && data[at] != 0) {
                at += 1u + data[at];
            }

            at++;
            continue;
        }

        if (block != 0x2C || at + 9u > length) {
            break;
        }

        u32 left = le16(data + at);
        u32 top = le16(data + at + 2);
        u32 w = le16(data + at + 4);
        u32 h = le16(data + at + 6);
        u32 image_flags = data[at + 8];

        at += 9;

        const u8* table = global_table;
        u32 table_count = global_count;

        if (image_flags & 0x80u) {
            table_count = 1u << ((image_flags & 7u) + 1u);
            table = data + at;
            at += table_count * 3u;
        }

        if (!table || at >= length) {
            return IMAGE_DAMAGED;
        }

        u32 min_code = data[at++];

        if (min_code < 2u || min_code > 8u) {
            return IMAGE_DAMAGED;
        }

        /*
         * LZW: codes of growing width, each standing for a string of
         * colour indexes built from earlier codes. The dictionary lives
         * in the scratch buffer: prefix code, last byte, and a stack for
         * unwinding a string.
         */
        if (scratch_size < 4096u * 2u + 4096u + 4096u) {
            return IMAGE_TOO_LARGE;
        }

        u16* prefix = (u16*)scratch;
        u8* suffix = scratch + 8192u;
        u8* stack = scratch + 8192u + 4096u;

        u32 clear = 1u << min_code;
        u32 end = clear + 1u;
        u32 next = clear + 2u;
        u32 code_size = min_code + 1u;
        u32 old = 0xFFFFu;
        u32 first = 0;

        u32 bit_buffer = 0;
        u32 bit_count = 0;
        u32 block_left = 0;

        u32 x = 0;
        u32 y = 0;
        u32 pass = 0;
        int done = 0;

        static const u32 pass_start[4] = { 0, 4, 2, 1 };
        static const u32 pass_step[4] = { 8, 8, 4, 2 };

        while (!done) {
            while (bit_count < code_size) {
                if (block_left == 0) {
                    if (at >= length || data[at] == 0) {
                        done = 1;
                        break;
                    }

                    block_left = data[at++];
                }

                if (at >= length) {
                    done = 1;
                    break;
                }

                bit_buffer |= (u32)data[at++] << bit_count;
                bit_count += 8;
                block_left--;
            }

            if (done) {
                break;
            }

            u32 code = bit_buffer & ((1u << code_size) - 1u);

            bit_buffer >>= code_size;
            bit_count -= code_size;

            if (code == clear) {
                next = clear + 2u;
                code_size = min_code + 1u;
                old = 0xFFFFu;
                continue;
            }

            if (code == end) {
                break;
            }

            u32 depth = 0;
            u32 cur = code;

            if (old == 0xFFFFu) {
                if (code >= clear) {
                    return IMAGE_DAMAGED;
                }

                first = code;
            } else if (code >= next) {
                /*
                 * The one code that can be used before it is defined:
                 * the previous string plus its own first byte.
                 */
                if (code > next) {
                    return IMAGE_DAMAGED;
                }

                stack[depth++] = (u8)first;
                cur = old;
            }

            while (cur >= clear) {
                if (cur >= 4096u || depth >= 4095u) {
                    return IMAGE_DAMAGED;
                }

                stack[depth++] = suffix[cur];
                cur = prefix[cur];
            }

            stack[depth++] = (u8)cur;
            first = cur;

            if (old != 0xFFFFu && next < 4096u) {
                prefix[next] = (u16)old;
                suffix[next] = (u8)first;
                next++;

                if (next == (1u << code_size) && code_size < 12u) {
                    code_size++;
                }
            }

            old = code;

            while (depth > 0) {
                u32 index = stack[--depth];

                if (y < h) {
                    u32 px = left + x;
                    u32 py = top + y;

                    if (px < width && py < height && (int)index != transparent &&
                        index < table_count) {
                        const u8* c = table + index * 3u;

                        pixels[py * width + px] = ((u32)c[0] << 16) | ((u32)c[1] << 8) | c[2];
                    }
                }

                if (++x >= w) {
                    x = 0;

                    if (image_flags & 0x40u) {
                        /*
                         * Interlaced: rows arrive in four passes.
                         */
                        y += pass_step[pass];

                        while (y >= h && pass < 3u) {
                            pass++;
                            y = pass_start[pass];
                        }
                    } else {
                        y++;
                    }
                }
            }
        }

        /*
         * First frame only.
         */
        break;
    }

    *out_width = width;
    *out_height = height;

    return IMAGE_OK;
}

/* -------------------------------------------------------------------------- */
/* JPEG (baseline)                                                             */
/* -------------------------------------------------------------------------- */

/*
 * A JPEG stores the picture as luminance plus two colour-difference
 * planes, each cut into 8x8 blocks. Every block is a set of 64 frequency
 * coefficients (the DCT), divided by a quantisation table, walked in
 * zigzag order and Huffman coded. Decoding runs that backwards.
 */
typedef struct jpeg_component {
    u32 id;
    u32 h;              /* sampling factors */
    u32 v;
    u32 table;          /* quantisation table */
    u32 dc_table;
    u32 ac_table;
    int dc_pred;

    u32 plane_w;
    u32 plane_h;
    u8* plane;
} jpeg_component_t;

typedef struct jpeg_huffman {
    u8 count[17];
    u8 symbol[256];
} jpeg_huffman_t;

typedef struct jpeg_state {
    const u8* data;
    u32 length;
    u32 pos;

    u32 bit_buffer;
    u32 bit_count;
    int ended;

    u16 quant[4][64];
    jpeg_huffman_t dc[4];
    jpeg_huffman_t ac[4];

    jpeg_component_t comp[3];
    u32 comp_count;

    u32 width;
    u32 height;
    u32 restart_interval;
} jpeg_state_t;

static const u8 g_zigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

/*
 * One bit of entropy-coded data. In it a 0xFF byte is always followed by
 * 0x00 (which is dropped); 0xFF followed by anything else is a marker
 * and ends the data.
 */
static u32 jpeg_bit(jpeg_state_t* s) {
    if (s->bit_count == 0) {
        u32 byte = 0;

        if (!s->ended && s->pos < s->length) {
            byte = s->data[s->pos];

            if (byte == 0xFFu) {
                if (s->pos + 1u < s->length && s->data[s->pos + 1u] == 0) {
                    s->pos += 2u;
                } else {
                    s->ended = 1;
                    byte = 0;
                }
            } else {
                s->pos++;
            }
        } else {
            s->ended = 1;
        }

        s->bit_buffer = byte;
        s->bit_count = 8;
    }

    s->bit_count--;

    return (s->bit_buffer >> s->bit_count) & 1u;
}

static int jpeg_receive(jpeg_state_t* s, u32 count) {
    int value = 0;

    for (u32 i = 0; i < count; i++) {
        value = (value << 1) | (int)jpeg_bit(s);
    }

    /*
     * A value with its top bit clear stands for a negative number.
     */
    if (count && value < (1 << (count - 1u))) {
        value -= (1 << count) - 1;
    }

    return value;
}

static int jpeg_decode_symbol(jpeg_state_t* s, const jpeg_huffman_t* h) {
    int code = 0;
    int first = 0;
    int index = 0;

    for (int len = 1; len <= 16; len++) {
        code |= (int)jpeg_bit(s);

        int count = h->count[len];

        if (code - count < first) {
            return h->symbol[index + (code - first)];
        }

        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }

    return -1;
}

static u8 clip8(int v) {
    return (u8)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

/*
 * Inverse DCT, one row then one column at a time, in fixed-point
 * arithmetic (the classic integer formulation).
 */
#define W1 2841
#define W2 2676
#define W3 2408
#define W5 1609
#define W6 1108
#define W7 565

static void idct_row(int* b) {
    int x0;
    int x1 = b[4] << 11;
    int x2 = b[6];
    int x3 = b[2];
    int x4 = b[1];
    int x5 = b[7];
    int x6 = b[5];
    int x7 = b[3];
    int x8;

    if (!(x1 | x2 | x3 | x4 | x5 | x6 | x7)) {
        int v = b[0] << 3;

        for (int i = 0; i < 8; i++) {
            b[i] = v;
        }

        return;
    }

    x0 = (b[0] << 11) + 128;
    x8 = W7 * (x4 + x5);
    x4 = x8 + (W1 - W7) * x4;
    x5 = x8 - (W1 + W7) * x5;
    x8 = W3 * (x6 + x7);
    x6 = x8 - (W3 - W5) * x6;
    x7 = x8 - (W3 + W5) * x7;
    x8 = x0 + x1;
    x0 -= x1;
    x1 = W6 * (x3 + x2);
    x2 = x1 - (W2 + W6) * x2;
    x3 = x1 + (W2 - W6) * x3;
    x1 = x4 + x6;
    x4 -= x6;
    x6 = x5 + x7;
    x5 -= x7;
    x7 = x8 + x3;
    x8 -= x3;
    x3 = x0 + x2;
    x0 -= x2;
    x2 = (181 * (x4 + x5) + 128) >> 8;
    x4 = (181 * (x4 - x5) + 128) >> 8;

    b[0] = (x7 + x1) >> 8;
    b[1] = (x3 + x2) >> 8;
    b[2] = (x0 + x4) >> 8;
    b[3] = (x8 + x6) >> 8;
    b[4] = (x8 - x6) >> 8;
    b[5] = (x0 - x4) >> 8;
    b[6] = (x3 - x2) >> 8;
    b[7] = (x7 - x1) >> 8;
}

static void idct_column(const int* b, u8* out, u32 stride) {
    int x0;
    int x1 = b[8 * 4] << 8;
    int x2 = b[8 * 6];
    int x3 = b[8 * 2];
    int x4 = b[8 * 1];
    int x5 = b[8 * 7];
    int x6 = b[8 * 5];
    int x7 = b[8 * 3];
    int x8;

    if (!(x1 | x2 | x3 | x4 | x5 | x6 | x7)) {
        u8 v = clip8(((b[0] + 32) >> 6) + 128);

        for (int i = 0; i < 8; i++) {
            *out = v;
            out += stride;
        }

        return;
    }

    x0 = (b[0] << 8) + 8192;
    x8 = W7 * (x4 + x5) + 4;
    x4 = (x8 + (W1 - W7) * x4) >> 3;
    x5 = (x8 - (W1 + W7) * x5) >> 3;
    x8 = W3 * (x6 + x7) + 4;
    x6 = (x8 - (W3 - W5) * x6) >> 3;
    x7 = (x8 - (W3 + W5) * x7) >> 3;
    x8 = x0 + x1;
    x0 -= x1;
    x1 = W6 * (x3 + x2) + 4;
    x2 = (x1 - (W2 + W6) * x2) >> 3;
    x3 = (x1 + (W2 - W6) * x3) >> 3;
    x1 = x4 + x6;
    x4 -= x6;
    x6 = x5 + x7;
    x5 -= x7;
    x7 = x8 + x3;
    x8 -= x3;
    x3 = x0 + x2;
    x0 -= x2;
    x2 = (181 * (x4 + x5) + 128) >> 8;
    x4 = (181 * (x4 - x5) + 128) >> 8;

    *out = clip8(((x7 + x1) >> 14) + 128);
    out += stride;
    *out = clip8(((x3 + x2) >> 14) + 128);
    out += stride;
    *out = clip8(((x0 + x4) >> 14) + 128);
    out += stride;
    *out = clip8(((x8 + x6) >> 14) + 128);
    out += stride;
    *out = clip8(((x8 - x6) >> 14) + 128);
    out += stride;
    *out = clip8(((x0 - x4) >> 14) + 128);
    out += stride;
    *out = clip8(((x3 - x2) >> 14) + 128);
    out += stride;
    *out = clip8(((x7 - x1) >> 14) + 128);
}

static int jpeg_block(jpeg_state_t* s, jpeg_component_t* c, u8* out) {
    int block[64];

    for (int i = 0; i < 64; i++) {
        block[i] = 0;
    }

    /*
     * First coefficient: the difference from the previous block's.
     */
    int size = jpeg_decode_symbol(s, &s->dc[c->dc_table]);

    if (size < 0 || size > 11) {
        return 0;
    }

    c->dc_pred += jpeg_receive(s, (u32)size);
    block[0] = c->dc_pred * (int)s->quant[c->table][0];

    /*
     * The other 63: (run of zeros, size) pairs.
     */
    for (int k = 1; k < 64; ) {
        int symbol = jpeg_decode_symbol(s, &s->ac[c->ac_table]);

        if (symbol < 0) {
            return 0;
        }

        int run = symbol >> 4;
        int bits = symbol & 15;

        if (bits == 0) {
            if (run != 15) {
                break;          /* end of block */
            }

            k += 16;
            continue;
        }

        k += run;

        if (k > 63) {
            return 0;
        }

        block[g_zigzag[k]] = jpeg_receive(s, (u32)bits) * (int)s->quant[c->table][k];
        k++;
    }

    for (int row = 0; row < 8; row++) {
        idct_row(block + row * 8);
    }

    for (int col = 0; col < 8; col++) {
        idct_column(block + col, out + col, c->plane_w);
    }

    return 1;
}

static int decode_jpeg(
    const u8* data,
    u32 length,
    u32* pixels,
    u32 max_pixels,
    u8* scratch,
    u32 scratch_size,
    u32* out_width,
    u32* out_height
) {
    static jpeg_state_t s;
    u32 at = 2;
    int have_frame = 0;
    u32 hmax = 1;
    u32 vmax = 1;

    s.data = data;
    s.length = length;
    s.comp_count = 0;
    s.restart_interval = 0;
    s.width = 0;
    s.height = 0;

    for (int t = 0; t < 4; t++) {
        for (int i = 0; i < 17; i++) {
            s.dc[t].count[i] = 0;
            s.ac[t].count[i] = 0;
        }
    }

    while (at + 4u <= length) {
        if (data[at] != 0xFFu) {
            at++;
            continue;
        }

        u32 marker = data[at + 1];

        if (marker == 0xFFu) {
            at++;
            continue;
        }

        if (marker == 0xD8u || (marker >= 0xD0u && marker <= 0xD7u) || marker == 0x01u) {
            at += 2;
            continue;
        }

        if (marker == 0xD9u) {
            break;
        }

        u32 size = be16(data + at + 2);
        const u8* p = data + at + 4;

        if (size < 2u || at + 2u + size > length) {
            return IMAGE_DAMAGED;
        }

        u32 body = size - 2u;

        if (marker == 0xDBu) {
            /*
             * Quantisation tables.
             */
            for (u32 i = 0; i < body; ) {
                u32 precision = p[i] >> 4;
                u32 id = p[i] & 15u;

                i++;

                if (id > 3u || i + (precision ? 128u : 64u) > body) {
                    return IMAGE_DAMAGED;
                }

                for (u32 k = 0; k < 64; k++) {
                    if (precision) {
                        s.quant[id][k] = (u16)be16(p + i);
                        i += 2;
                    } else {
                        s.quant[id][k] = p[i++];
                    }
                }
            }
        } else if (marker == 0xC4u) {
            /*
             * Huffman tables.
             */
            for (u32 i = 0; i + 17u <= body; ) {
                u32 klass = p[i] >> 4;
                u32 id = p[i] & 15u;
                u32 total = 0;

                if (klass > 1u || id > 3u) {
                    return IMAGE_DAMAGED;
                }

                jpeg_huffman_t* h = klass ? &s.ac[id] : &s.dc[id];

                h->count[0] = 0;

                for (u32 k = 1; k <= 16; k++) {
                    h->count[k] = p[i + k];
                    total += p[i + k];
                }

                i += 17;

                if (total > 256u || i + total > body) {
                    return IMAGE_DAMAGED;
                }

                for (u32 k = 0; k < total; k++) {
                    h->symbol[k] = p[i + k];
                }

                i += total;
            }
        } else if (marker == 0xC0u || marker == 0xC1u) {
            /*
             * Frame header (baseline or extended sequential).
             */
            if (body < 6u || p[0] != 8) {
                return IMAGE_UNSUPPORTED;
            }

            s.height = be16(p + 1);
            s.width = be16(p + 3);
            s.comp_count = p[5];

            if ((s.comp_count != 1u && s.comp_count != 3u) || body < 6u + s.comp_count * 3u) {
                return IMAGE_UNSUPPORTED;
            }

            for (u32 i = 0; i < s.comp_count; i++) {
                jpeg_component_t* c = &s.comp[i];

                c->id = p[6 + i * 3];
                c->h = p[7 + i * 3] >> 4;
                c->v = p[7 + i * 3] & 15u;
                c->table = p[8 + i * 3] & 3u;

                if (c->h == 0 || c->v == 0 || c->h > 4u || c->v > 4u) {
                    return IMAGE_DAMAGED;
                }

                if (c->h > hmax) {
                    hmax = c->h;
                }

                if (c->v > vmax) {
                    vmax = c->v;
                }
            }

            have_frame = 1;
        } else if (marker == 0xC2u || (marker >= 0xC5u && marker <= 0xCFu && marker != 0xC8u &&
                                       marker != 0xCCu)) {
            /*
             * Progressive, lossless or arithmetic-coded.
             */
            return IMAGE_UNSUPPORTED;
        } else if (marker == 0xDDu && body >= 2u) {
            s.restart_interval = be16(p);
        } else if (marker == 0xDAu) {
            /*
             * Scan header; the compressed data follows it.
             */
            if (!have_frame || body < 1u || p[0] != s.comp_count ||
                body < 1u + s.comp_count * 2u) {
                return IMAGE_UNSUPPORTED;
            }

            for (u32 i = 0; i < s.comp_count; i++) {
                jpeg_component_t* c = 0;

                for (u32 k = 0; k < s.comp_count; k++) {
                    if (s.comp[k].id == p[1 + i * 2]) {
                        c = &s.comp[k];
                    }
                }

                if (!c) {
                    return IMAGE_DAMAGED;
                }

                c->dc_table = (p[2 + i * 2] >> 4) & 3u;
                c->ac_table = p[2 + i * 2] & 3u;
            }

            at += 2u + size;
            break;
        }

        at += 2u + size;
    }

    if (!have_frame || s.width == 0 || s.height == 0 || at >= length) {
        return IMAGE_DAMAGED;
    }

    if ((u64)s.width * s.height > max_pixels) {
        return IMAGE_TOO_LARGE;
    }

    /*
     * Blocks are grouped into "MCUs" covering hmax*8 by vmax*8 pixels.
     */
    u32 mcu_w = hmax * 8u;
    u32 mcu_h = vmax * 8u;
    u32 mcus_x = (s.width + mcu_w - 1u) / mcu_w;
    u32 mcus_y = (s.height + mcu_h - 1u) / mcu_h;
    u64 needed = 0;

    for (u32 i = 0; i < s.comp_count; i++) {
        jpeg_component_t* c = &s.comp[i];

        c->plane_w = mcus_x * c->h * 8u;
        c->plane_h = mcus_y * c->v * 8u;
        c->dc_pred = 0;

        if (needed + (u64)c->plane_w * c->plane_h > scratch_size) {
            return IMAGE_TOO_LARGE;
        }

        c->plane = scratch + needed;
        needed += (u64)c->plane_w * c->plane_h;
    }

    s.pos = at;
    s.bit_count = 0;
    s.bit_buffer = 0;
    s.ended = 0;

    u32 until_restart = s.restart_interval;

    for (u32 my = 0; my < mcus_y; my++) {
        for (u32 mx = 0; mx < mcus_x; mx++) {
            for (u32 i = 0; i < s.comp_count; i++) {
                jpeg_component_t* c = &s.comp[i];

                for (u32 by = 0; by < c->v; by++) {
                    for (u32 bx = 0; bx < c->h; bx++) {
                        u8* out = c->plane +
                            (u64)(my * c->v + by) * 8u * c->plane_w +
                            (mx * c->h + bx) * 8u;

                        if (!jpeg_block(&s, c, out)) {
                            return IMAGE_DAMAGED;
                        }
                    }
                }
            }

            /*
             * Restart markers: every so many MCUs the bit stream is
             * realigned and the predictors start over.
             */
            if (s.restart_interval && --until_restart == 0) {
                until_restart = s.restart_interval;
                s.bit_count = 0;
                s.ended = 0;

                while (s.pos + 1u < length &&
                       !(data[s.pos] == 0xFFu && data[s.pos + 1u] >= 0xD0u &&
                         data[s.pos + 1u] <= 0xD7u)) {
                    if (data[s.pos] == 0xFFu && data[s.pos + 1u] == 0xD9u) {
                        break;
                    }

                    s.pos++;
                }

                if (s.pos + 1u < length && data[s.pos + 1u] != 0xD9u) {
                    s.pos += 2u;
                }

                for (u32 i = 0; i < s.comp_count; i++) {
                    s.comp[i].dc_pred = 0;
                }
            }
        }
    }

    /*
     * Planes -> RGB. Colour planes that are smaller than the picture are
     * stretched by repeating samples.
     */
    for (u32 y = 0; y < s.height; y++) {
        u32* out = pixels + (u64)y * s.width;

        for (u32 x = 0; x < s.width; x++) {
            const jpeg_component_t* c0 = &s.comp[0];
            int luma = c0->plane[(u64)(y * c0->v / vmax) * c0->plane_w + (x * c0->h / hmax)];

            if (s.comp_count == 1u) {
                out[x] = ((u32)luma << 16) | ((u32)luma << 8) | (u32)luma;
                continue;
            }

            const jpeg_component_t* c1 = &s.comp[1];
            const jpeg_component_t* c2 = &s.comp[2];

            int cb = c1->plane[(u64)(y * c1->v / vmax) * c1->plane_w + (x * c1->h / hmax)] - 128;
            int cr = c2->plane[(u64)(y * c2->v / vmax) * c2->plane_w + (x * c2->h / hmax)] - 128;

            u32 r = clip8((luma * 256 + 359 * cr + 128) >> 8);
            u32 g = clip8((luma * 256 - 88 * cb - 183 * cr + 128) >> 8);
            u32 b = clip8((luma * 256 + 454 * cb + 128) >> 8);

            out[x] = (r << 16) | (g << 8) | b;
        }
    }

    *out_width = s.width;
    *out_height = s.height;

    return IMAGE_OK;
}

/* -------------------------------------------------------------------------- */
/* Entry point                                                                 */
/* -------------------------------------------------------------------------- */

int image_decode(
    const u8* data,
    u32 length,
    u32* pixels,
    u32 max_pixels,
    u8* scratch,
    u32 scratch_size,
    u32* out_width,
    u32* out_height
) {
    static const u8 png_signature[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };

    if (!data || !pixels || !scratch || !out_width || !out_height || length < 10u) {
        return IMAGE_UNKNOWN;
    }

    int is_png = 1;

    for (int i = 0; i < 8; i++) {
        if (data[i] != png_signature[i]) {
            is_png = 0;
        }
    }

    if (is_png) {
        return decode_png(data, length, pixels, max_pixels, scratch, scratch_size,
                          out_width, out_height);
    }

    if (data[0] == 0xFFu && data[1] == 0xD8u) {
        return decode_jpeg(data, length, pixels, max_pixels, scratch, scratch_size,
                           out_width, out_height);
    }

    if (data[0] == 'G' && data[1] == 'I' && data[2] == 'F' && data[3] == '8') {
        return decode_gif(data, length, pixels, max_pixels, scratch, scratch_size,
                          out_width, out_height);
    }

    return IMAGE_UNKNOWN;
}
