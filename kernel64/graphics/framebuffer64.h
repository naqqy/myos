#ifndef FRAMEBUFFER64_H
#define FRAMEBUFFER64_H

#include "naq_boot_info.h"

int fb64_init(naq_boot_info_t* boot);
int fb64_available(void);

nq_u32 fb64_width(void);
nq_u32 fb64_height(void);
nq_u32 fb64_pixels_per_scanline(void);
nq_u64 fb64_base(void);
nq_u64 fb64_size(void);
nq_u32 fb64_pixel_format(void);
nq_u32 fb64_pitch(void);
nq_u32 fb64_format(void);

nq_u32 fb64_make_color(nq_u8 r, nq_u8 g, nq_u8 b);

void fb64_put_pixel(nq_u32 x, nq_u32 y, nq_u32 color);
void fb64_fill_rect(nq_u32 x, nq_u32 y, nq_u32 w, nq_u32 h, nq_u32 color);
void fb64_clear(nq_u32 color);

void fb64_scroll_region_up(
    nq_u32 x,
    nq_u32 y,
    nq_u32 w,
    nq_u32 h,
    nq_u32 pixels,
    nq_u32 fill_color
);

/*
 * Input pixels are XRGB32-style: 0x00RRGGBB.
 * framebuffer64 converts to the native GOP pixel format.
 */
void fb64_draw_xrgb32_row(
    nq_u32 x,
    nq_u32 y,
    const nq_u32* pixels,
    nq_u32 count
);

/*
 * Copies a w x h block of XRGB32 pixels to the screen at (x, y).
 * `pixels` points at the first pixel of the block and `stride` is the
 * number of pixels between the starts of consecutive source rows.
 *
 * When the display is BGRx (the usual GOP format) XRGB32 is already the
 * native layout and each row is a straight memory copy.
 */
void fb64_blit_xrgb32(
    nq_u32 x,
    nq_u32 y,
    nq_u32 w,
    nq_u32 h,
    const nq_u32* pixels,
    nq_u32 stride
);

void fb64_blit_rgb32(
    int dst_x,
    int dst_y,
    nq_u32 width,
    nq_u32 height,
    const nq_u32* pixels
);

void fb64_print_summary(void);

#endif