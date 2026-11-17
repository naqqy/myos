#include "framebuffer64.h"
#include "../k64_console.h"

#define FB64_PIXEL_FORMAT_RGBX 0u
#define FB64_PIXEL_FORMAT_BGRX 1u

static naq_boot_info_t* g_boot = 0;
static int g_fb_available = 0;

static nq_u64 checked_fb_index(nq_u32 x, nq_u32 y) {
    return ((nq_u64)y * g_boot->framebuffer_pixels_per_scanline) + x;
}

int fb64_init(naq_boot_info_t* boot) {
    g_boot = boot;
    g_fb_available = 0;

    if (!g_boot ||
        g_boot->magic != NAQ_BOOT_INFO_MAGIC ||
        g_boot->version != NAQ_BOOT_INFO_VERSION) {
        return 0;
    }

    if (g_boot->framebuffer_base == 0 ||
        g_boot->framebuffer_size == 0 ||
        g_boot->framebuffer_width == 0 ||
        g_boot->framebuffer_height == 0 ||
        g_boot->framebuffer_pixels_per_scanline == 0) {
        return 0;
    }

    /*
     * For now, support the two common GOP 32-bit formats:
     * 0 = PixelRedGreenBlueReserved8BitPerColor
     * 1 = PixelBlueGreenRedReserved8BitPerColor
     */
    if (g_boot->framebuffer_pixel_format != FB64_PIXEL_FORMAT_RGBX &&
        g_boot->framebuffer_pixel_format != FB64_PIXEL_FORMAT_BGRX) {
        return 0;
    }

    g_fb_available = 1;
    return 1;
}

int fb64_available(void) {
    return g_fb_available;
}

nq_u32 fb64_width(void) {
    return g_fb_available ? g_boot->framebuffer_width : 0;
}

nq_u32 fb64_height(void) {
    return g_fb_available ? g_boot->framebuffer_height : 0;
}

nq_u32 fb64_pixels_per_scanline(void) {
    return g_fb_available ? g_boot->framebuffer_pixels_per_scanline : 0;
}

nq_u32 fb64_pitch(void) {
    return fb64_pixels_per_scanline();
}

nq_u32 fb64_pixel_format(void) {
    return g_fb_available ? g_boot->framebuffer_pixel_format : 0;
}

nq_u32 fb64_format(void) {
    return fb64_pixel_format();
}

nq_u64 fb64_base(void) {
    return g_fb_available ? g_boot->framebuffer_base : 0;
}

nq_u64 fb64_size(void) {
    return g_fb_available ? g_boot->framebuffer_size : 0;
}

nq_u32 fb64_make_color(nq_u8 r, nq_u8 g, nq_u8 b) {
    if (!g_fb_available) {
        return 0;
    }

    if (g_boot->framebuffer_pixel_format == FB64_PIXEL_FORMAT_RGBX) {
        return ((nq_u32)r) |
               ((nq_u32)g << 8) |
               ((nq_u32)b << 16);
    }

    /*
     * BGRx.
     */
    return ((nq_u32)b) |
           ((nq_u32)g << 8) |
           ((nq_u32)r << 16);
}

void fb64_put_pixel(nq_u32 x, nq_u32 y, nq_u32 color) {
    if (!g_fb_available) {
        return;
    }

    if (x >= g_boot->framebuffer_width ||
        y >= g_boot->framebuffer_height) {
        return;
    }

    volatile nq_u32* fb = (volatile nq_u32*)g_boot->framebuffer_base;
    fb[checked_fb_index(x, y)] = color;
}

void fb64_fill_rect(
    nq_u32 x,
    nq_u32 y,
    nq_u32 w,
    nq_u32 h,
    nq_u32 color
) {
    if (!g_fb_available || w == 0 || h == 0) {
        return;
    }

    if (x >= g_boot->framebuffer_width ||
        y >= g_boot->framebuffer_height) {
        return;
    }

    if (w > g_boot->framebuffer_width - x) {
        w = g_boot->framebuffer_width - x;
    }

    if (h > g_boot->framebuffer_height - y) {
        h = g_boot->framebuffer_height - y;
    }

    volatile nq_u32* fb = (volatile nq_u32*)g_boot->framebuffer_base;

    for (nq_u32 yy = 0; yy < h; yy++) {
        nq_u64 row = ((nq_u64)(y + yy) * g_boot->framebuffer_pixels_per_scanline) + x;

        for (nq_u32 xx = 0; xx < w; xx++) {
            fb[row + xx] = color;
        }
    }
}

void fb64_clear(nq_u32 color) {
    if (!g_fb_available) {
        return;
    }

    fb64_fill_rect(
        0,
        0,
        g_boot->framebuffer_width,
        g_boot->framebuffer_height,
        color
    );
}

void fb64_scroll_region_up(
    nq_u32 x,
    nq_u32 y,
    nq_u32 w,
    nq_u32 h,
    nq_u32 pixels,
    nq_u32 fill_color
) {
    if (!g_fb_available || w == 0 || h == 0 || pixels == 0) {
        return;
    }

    if (x >= g_boot->framebuffer_width ||
        y >= g_boot->framebuffer_height) {
        return;
    }

    if (w > g_boot->framebuffer_width - x) {
        w = g_boot->framebuffer_width - x;
    }

    if (h > g_boot->framebuffer_height - y) {
        h = g_boot->framebuffer_height - y;
    }

    if (pixels >= h) {
        fb64_fill_rect(x, y, w, h, fill_color);
        return;
    }

    volatile nq_u32* fb = (volatile nq_u32*)g_boot->framebuffer_base;
    nq_u32 copy_height = h - pixels;

    /*
     * Source is below destination, so top-to-bottom copy is safe.
     */
    for (nq_u32 yy = 0; yy < copy_height; yy++) {
        nq_u64 dst_row =
            ((nq_u64)(y + yy) * g_boot->framebuffer_pixels_per_scanline) + x;

        nq_u64 src_row =
            ((nq_u64)(y + yy + pixels) * g_boot->framebuffer_pixels_per_scanline) + x;

        for (nq_u32 xx = 0; xx < w; xx++) {
            fb[dst_row + xx] = fb[src_row + xx];
        }
    }

    /*
     * Clear the newly exposed bottom area.
     */
    fb64_fill_rect(
        x,
        y + copy_height,
        w,
        pixels,
        fill_color
    );
}

void fb64_draw_xrgb32_row(
    nq_u32 x,
    nq_u32 y,
    const nq_u32* pixels,
    nq_u32 count
) {
    if (!g_fb_available || !pixels || count == 0) {
        return;
    }

    if (x >= g_boot->framebuffer_width ||
        y >= g_boot->framebuffer_height) {
        return;
    }

    if (count > g_boot->framebuffer_width - x) {
        count = g_boot->framebuffer_width - x;
    }

    volatile nq_u32* fb = (volatile nq_u32*)g_boot->framebuffer_base;
    nq_u64 row = ((nq_u64)y * g_boot->framebuffer_pixels_per_scanline) + x;

    for (nq_u32 i = 0; i < count; i++) {
        nq_u32 src = pixels[i];

        nq_u8 r = (nq_u8)((src >> 16) & 0xFFu);
        nq_u8 g = (nq_u8)((src >> 8) & 0xFFu);
        nq_u8 b = (nq_u8)(src & 0xFFu);

        fb[row + i] = fb64_make_color(r, g, b);
    }
}

void fb64_blit_xrgb32(
    nq_u32 x,
    nq_u32 y,
    nq_u32 w,
    nq_u32 h,
    const nq_u32* pixels,
    nq_u32 stride
) {
    if (!g_fb_available || !pixels || w == 0 || h == 0) {
        return;
    }

    if (x >= g_boot->framebuffer_width ||
        y >= g_boot->framebuffer_height) {
        return;
    }

    if (w > g_boot->framebuffer_width - x) {
        w = g_boot->framebuffer_width - x;
    }

    if (h > g_boot->framebuffer_height - y) {
        h = g_boot->framebuffer_height - y;
    }

    nq_u32* fb = (nq_u32*)g_boot->framebuffer_base;
    nq_u64 scanline = g_boot->framebuffer_pixels_per_scanline;
    int native = g_boot->framebuffer_pixel_format == FB64_PIXEL_FORMAT_BGRX;

    for (nq_u32 row = 0; row < h; row++) {
        const nq_u32* src = pixels + (nq_u64)row * stride;
        nq_u32* dst = fb + ((nq_u64)(y + row) * scanline) + x;

        if (native) {
            nq_u64 count = w;

            __asm__ volatile (
                "rep movsl"
                : "+D"(dst), "+S"(src), "+c"(count)
                :
                : "memory"
            );
        } else {
            for (nq_u32 i = 0; i < w; i++) {
                nq_u32 c = src[i];

                dst[i] =
                    ((c >> 16) & 0xFFu) |
                    (c & 0x0000FF00u) |
                    ((c & 0xFFu) << 16);
            }
        }
    }
}

void fb64_print_summary(void) {
    k64_print("Framebuffer64: ");

    if (!g_fb_available) {
        k64_print("not available\n");
        return;
    }

    k64_print("available\n");

    k64_print("Resolution: ");
    k64_print_dec64(g_boot->framebuffer_width);
    k64_print("x");
    k64_print_dec64(g_boot->framebuffer_height);
    k64_print("\n");

    k64_print("Base: ");
    k64_print_hex64(g_boot->framebuffer_base);
    k64_print("\n");

    k64_print("Size: ");
    k64_print_dec64(g_boot->framebuffer_size);
    k64_print(" bytes\n");

    k64_print("Pixels/scanline: ");
    k64_print_dec64(g_boot->framebuffer_pixels_per_scanline);
    k64_print("\n");

    k64_print("Pixel format: ");
    k64_print_dec64(g_boot->framebuffer_pixel_format);
    k64_print("\n");
}

void fb64_blit_rgb32(
    int dst_x,
    int dst_y,
    nq_u32 width,
    nq_u32 height,
    const nq_u32* pixels
) {
    if (!pixels || !fb64_available() || width == 0 || height == 0) {
        return;
    }

    int src_x0 = 0;
    int src_y0 = 0;
    int draw_w = (int)width;
    int draw_h = (int)height;

    if (dst_x < 0) {
        src_x0 = -dst_x;
        draw_w -= src_x0;
        dst_x = 0;
    }

    if (dst_y < 0) {
        src_y0 = -dst_y;
        draw_h -= src_y0;
        dst_y = 0;
    }

    if (dst_x + draw_w > (int)fb64_width()) {
        draw_w = (int)fb64_width() - dst_x;
    }

    if (dst_y + draw_h > (int)fb64_height()) {
        draw_h = (int)fb64_height() - dst_y;
    }

    if (draw_w <= 0 || draw_h <= 0) {
        return;
    }

    volatile nq_u32* fb = (volatile nq_u32*)fb64_base();
    nq_u32 pitch = fb64_pixels_per_scanline();

    for (int y = 0; y < draw_h; y++) {
        volatile nq_u32* dst =
            fb + ((nq_u64)(dst_y + y) * pitch) + dst_x;

        const nq_u32* src =
            pixels + ((nq_u64)(src_y0 + y) * width) + src_x0;

        for (int x = 0; x < draw_w; x++) {
            nq_u32 raw = src[x];

            nq_u8 r = (nq_u8)((raw >> 16) & 0xFFu);
            nq_u8 g = (nq_u8)((raw >> 8) & 0xFFu);
            nq_u8 b = (nq_u8)(raw & 0xFFu);

            dst[x] = fb64_make_color(r, g, b);
        }
    }
}