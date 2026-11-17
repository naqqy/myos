#include "../include/naqos.h"

/*
 * User-space graphics buffer helpers.
 *
 * App buffers store pixels as XRGB/RGB-style 0x00RRGGBB.
 * The kernel graphics syscalls convert to the native framebuffer format.
 */

static void gfx_buffer_mark_dirty(
    naq_gfx_buffer_t* buffer,
    i32 x,
    i32 y,
    i32 w,
    i32 h
) {
    if (!buffer || w <= 0 || h <= 0) {
        return;
    }

    i32 x0 = x;
    i32 y0 = y;
    i32 x1 = x + w;
    i32 y1 = y + h;

    if (x0 < 0) {
        x0 = 0;
    }

    if (y0 < 0) {
        y0 = 0;
    }

    if (x1 > (i32)buffer->width) {
        x1 = (i32)buffer->width;
    }

    if (y1 > (i32)buffer->height) {
        y1 = (i32)buffer->height;
    }

    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    /*
     * Empty dirty rect.
     */
    if (buffer->dirty_x1 <= buffer->dirty_x0 ||
        buffer->dirty_y1 <= buffer->dirty_y0) {
        buffer->dirty_x0 = x0;
        buffer->dirty_y0 = y0;
        buffer->dirty_x1 = x1;
        buffer->dirty_y1 = y1;
        return;
    }

    /*
     * Expand existing dirty rect.
     */
    if (x0 < buffer->dirty_x0) {
        buffer->dirty_x0 = x0;
    }

    if (y0 < buffer->dirty_y0) {
        buffer->dirty_y0 = y0;
    }

    if (x1 > buffer->dirty_x1) {
        buffer->dirty_x1 = x1;
    }

    if (y1 > buffer->dirty_y1) {
        buffer->dirty_y1 = y1;
    }
}

void gfx_buffer_clear_dirty(naq_gfx_buffer_t* buffer) {
    if (!buffer) {
        return;
    }

    buffer->dirty_x0 = 0;
    buffer->dirty_y0 = 0;
    buffer->dirty_x1 = 0;
    buffer->dirty_y1 = 0;
}

void gfx_buffer_init(
    naq_gfx_buffer_t* buffer,
    u32* pixels,
    u32 width,
    u32 height
) {
    if (!buffer) {
        return;
    }

    buffer->pixels = pixels;
    buffer->width = width;
    buffer->height = height;
    buffer->stride = width;

    gfx_buffer_clear_dirty(buffer);
}

void gfx_buffer_clear(
    naq_gfx_buffer_t* buffer,
    u32 color
) {
    if (!buffer || !buffer->pixels) {
        return;
    }

    for (u32 y = 0; y < buffer->height; y++) {
        u32 row = y * buffer->stride;

        for (u32 x = 0; x < buffer->width; x++) {
            buffer->pixels[row + x] = color;
        }
    }

    gfx_buffer_mark_dirty(
        buffer,
        0,
        0,
        (i32)buffer->width,
        (i32)buffer->height
    );
}

void gfx_buffer_put_pixel(
    naq_gfx_buffer_t* buffer,
    i32 x,
    i32 y,
    u32 color
) {
    if (!buffer || !buffer->pixels) {
        return;
    }

    if (x < 0 || y < 0) {
        return;
    }

    if ((u32)x >= buffer->width || (u32)y >= buffer->height) {
        return;
    }

    buffer->pixels[(u32)y * buffer->stride + (u32)x] = color;

    gfx_buffer_mark_dirty(buffer, x, y, 1, 1);
}

void gfx_buffer_draw_rect(
    naq_gfx_buffer_t* buffer,
    i32 x,
    i32 y,
    i32 w,
    i32 h,
    u32 color
) {
    if (!buffer || !buffer->pixels || w <= 0 || h <= 0) {
        return;
    }

    i32 x0 = x;
    i32 y0 = y;
    i32 x1 = x + w;
    i32 y1 = y + h;

    if (x0 < 0) {
        x0 = 0;
    }

    if (y0 < 0) {
        y0 = 0;
    }

    if (x1 > (i32)buffer->width) {
        x1 = (i32)buffer->width;
    }

    if (y1 > (i32)buffer->height) {
        y1 = (i32)buffer->height;
    }

    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    for (i32 yy = y0; yy < y1; yy++) {
        u32 row = (u32)yy * buffer->stride;

        for (i32 xx = x0; xx < x1; xx++) {
            buffer->pixels[row + (u32)xx] = color;
        }
    }

    gfx_buffer_mark_dirty(buffer, x0, y0, x1 - x0, y1 - y0);
}

int gfx_buffer_present(
    naq_gfx_buffer_t* buffer,
    i32 dst_x,
    i32 dst_y
) {
    if (!buffer || !buffer->pixels) {
        return -1;
    }

    /*
     * Full-buffer present.
     * Use this for simple double-buffered rendering where most of the
     * frame changes every update.
     */
    int ret = gfx_blit(
        dst_x,
        dst_y,
        buffer->pixels,
        buffer->width,
        buffer->height
    );

    if (ret >= 0) {
        gfx_buffer_clear_dirty(buffer);
    }

    return ret;
}

int gfx_buffer_present_dirty(
    naq_gfx_buffer_t* buffer,
    i32 dst_x,
    i32 dst_y
) {
    if (!buffer || !buffer->pixels) {
        return -1;
    }

    if (buffer->dirty_x1 <= buffer->dirty_x0 ||
        buffer->dirty_y1 <= buffer->dirty_y0) {
        return 0;
    }

    /*
     * POC dirty tracking:
     *
     * We track the dirty rectangle now, but the current syscall ABI only
     * supports blitting a contiguous full source image. Until we add
     * SYS_GFX_BLIT_REGION, this still presents the full buffer.
     *
     * The win for now is that apps can start tracking dirty regions without
     * changing their code later.
     */
    int ret = gfx_blit(
        dst_x,
        dst_y,
        buffer->pixels,
        buffer->width,
        buffer->height
    );

    if (ret >= 0) {
        gfx_buffer_clear_dirty(buffer);
    }

    return ret;
}

/*
 * 8x8 bitmap font shared with the kernel (kernel64/console/font8x8.c is
 * compiled into the app runtime). Each font row is drawn twice, giving
 * 8x16 cells.
 */
extern const unsigned char* font8x8_get(char c);

void gfx_text_n(
    u32* pixels,
    u32 width,
    u32 height,
    i32 x,
    i32 y,
    const char* text,
    u32 max_chars,
    u32 color
) {
    if (!pixels || !text) {
        return;
    }

    for (u32 i = 0; text[i] && i < max_chars; i++) {
        const unsigned char* glyph = font8x8_get(text[i]);
        i32 cx = x + (i32)(i * GFX_FONT_W);

        if (!glyph || cx >= (i32)width || cx + GFX_FONT_W <= 0) {
            continue;
        }

        for (i32 gy = 0; gy < 8; gy++) {
            unsigned char bits = glyph[gy];

            if (!bits) {
                continue;
            }

            for (i32 half = 0; half < 2; half++) {
                i32 py = y + gy * 2 + half;

                if (py < 0 || py >= (i32)height) {
                    continue;
                }

                for (i32 gx = 0; gx < 8; gx++) {
                    i32 px = cx + gx;

                    if ((bits & (0x80u >> gx)) && px >= 0 && px < (i32)width) {
                        pixels[(u32)py * width + (u32)px] = color;
                    }
                }
            }
        }
    }
}

void gfx_text(
    u32* pixels,
    u32 width,
    u32 height,
    i32 x,
    i32 y,
    const char* text,
    u32 color
) {
    gfx_text_n(pixels, width, height, x, y, text, 0xFFFFFFFFu, color);
}
