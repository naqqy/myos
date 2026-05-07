#include "framebuffer.h"
#include "font8x8.h"

static framebuffer_info_t fb;
static int fb_is_available = 0;
static u32 fb_bytes = 0;
static u8* fb_base = 0;
static u32 fb_pixel_bytes = 0;

static inline u16 fb_color_to_rgb565(u32 color) {
    u8 r = (u8)((color >> 16) & 0xFF);
    u8 g = (u8)((color >> 8) & 0xFF);
    u8 b = (u8)(color & 0xFF);

    return (u16)(((r >> 3) << 11) |
                  ((g >> 2) << 5) |
                  (b >> 3));
}

static inline void fb_write_pixel_ptr(u8* pixel, u32 color) {
    if (fb.bpp == 32) {
        *(u32*)pixel = color;
        return;
    }

    if (fb.bpp == 24) {
        pixel[0] = (u8)(color & 0xFF);
        pixel[1] = (u8)((color >> 8) & 0xFF);
        pixel[2] = (u8)((color >> 16) & 0xFF);
        return;
    }

    *(u16*)pixel = fb_color_to_rgb565(color);
}

static void fb_write_pixel_row(u8* row, u32 x, u32 count, u32 color) {
    if (fb.bpp == 32) {
        u32* pixel = (u32*)(row + x * 4);

        for (u32 i = 0; i < count; i++) {
            pixel[i] = color;
        }

        return;
    }

    if (fb.bpp == 24) {
        u8 r = (u8)((color >> 16) & 0xFF);
        u8 g = (u8)((color >> 8) & 0xFF);
        u8 b = (u8)(color & 0xFF);
        u8* pixel = row + x * 3;

        for (u32 i = 0; i < count; i++) {
            pixel[0] = b;
            pixel[1] = g;
            pixel[2] = r;
            pixel += 3;
        }

        return;
    }

    u16 rgb565 = fb_color_to_rgb565(color);
    u16* pixel = (u16*)(row + x * 2);

    for (u32 i = 0; i < count; i++) {
        pixel[i] = rgb565;
    }
}

void fb_init(void) {
    framebuffer_info_t* info = (framebuffer_info_t*)FB_INFO_ADDR;

    fb_is_available = 0;
    fb_bytes = 0;
    fb_base = 0;
    fb_pixel_bytes = 0;

    if (info->magic != FB_INFO_MAGIC) {
        return;
    }

    if (info->phys_addr == 0 ||
        info->width == 0 ||
        info->height == 0 ||
        info->pitch == 0 ||
        info->bpp == 0) {
        return;
    }

    if (info->width != 1024 || info->height != 768) {
        return;
    }

    if (info->bpp != 16 && info->bpp != 24 && info->bpp != 32) {
        return;
    }

    u32 min_pitch = (info->width * info->bpp + 7) / 8;

    if (info->pitch < min_pitch) {
        return;
    }

    if (info->pitch > 16384) {
        return;
    }

    if (info->height > 0xFFFFFFFFu / info->pitch) {
        return;
    }

    if (info->height > 0xFFFFFFFFu / min_pitch) {
        return;
    }

    u32 size = info->pitch * info->height;

    if (size < min_pitch * info->height) {
        return;
    }

    if (size == 0 || size > (16u * 1024u * 1024u)) {
        return;
    }

    if (info->phys_addr < 0x01000000u) {
        return;
    }

    if (info->phys_addr > 0xFFFFFFFFu - size) {
        return;
    }

    fb = *info;
    fb_bytes = size;
    fb_base = (u8*)(unsigned long)info->phys_addr;
    fb_pixel_bytes = (fb.bpp + 7) / 8;
    fb_is_available = 1;
}

int fb_available(void) {
    return fb_is_available;
}

u32 fb_width(void) {
    return fb.width;
}

u32 fb_height(void) {
    return fb.height;
}

u32 fb_pitch(void) {
    return fb.pitch;
}

u32 fb_bpp(void) {
    return fb.bpp;
}

u32 fb_phys_addr(void) {
    return fb.phys_addr;
}

u32 fb_size_bytes(void) {
    if (!fb_is_available) {
        return 0;
    }

    return fb_bytes;
}

void fb_put_pixel(u32 x, u32 y, u32 color) {
    if (!fb_is_available) {
        return;
    }

    if (x >= fb.width || y >= fb.height) {
        return;
    }

    u8* row = fb_base + y * fb.pitch;
    u8* pixel = row + x * fb_pixel_bytes;

    fb_write_pixel_ptr(pixel, color);
}

void fb_fill_rect(u32 x, u32 y, u32 w, u32 h, u32 color) {
    if (!fb_is_available) {
        return;
    }

    if (w == 0 || h == 0) {
        return;
    }

    if (x >= fb.width || y >= fb.height) {
        return;
    }

    if (w > fb.width - x) {
        w = fb.width - x;
    }

    if (h > fb.height - y) {
        h = fb.height - y;
    }

    for (u32 yy = 0; yy < h; yy++) {
        u8* row = fb_base + (y + yy) * fb.pitch;
        fb_write_pixel_row(row, x, w, color);
    }
}

void fb_clear(u32 color) {
    if (!fb_is_available) {
        return;
    }

    fb_fill_rect(0, 0, fb.width, fb.height, color);
}

void fb_draw_xrgb32_row(u32 x, u32 y, const u32* pixels, u32 count) {
    if (!fb_is_available || !pixels || count == 0) {
        return;
    }

    if (x >= fb.width || y >= fb.height) {
        return;
    }

    if (count > fb.width - x) {
        count = fb.width - x;
    }

    u8* dest = fb_base + y * fb.pitch + x * fb_pixel_bytes;

    if (fb.bpp == 32) {
        u32* pixel = (u32*)dest;

        for (u32 i = 0; i < count; i++) {
            pixel[i] = pixels[i];
        }

        return;
    }

    if (fb.bpp == 24) {
        u8* pixel = dest;

        for (u32 i = 0; i < count; i++) {
            u32 color = pixels[i];
            pixel[0] = (u8)(color & 0xFF);
            pixel[1] = (u8)((color >> 8) & 0xFF);
            pixel[2] = (u8)((color >> 16) & 0xFF);
            pixel += 3;
        }

        return;
    }

    u16* pixel = (u16*)dest;

    for (u32 i = 0; i < count; i++) {
        pixel[i] = fb_color_to_rgb565(pixels[i]);
    }
}

static const u8* fb_get_bios_font8x16(char c) {
    u32* magic = (u32*)FONT8X16_INFO_ADDR;

    if (*magic != FONT8X16_MAGIC) {
        return 0;
    }

    return (const u8*)(FONT8X16_ADDR + ((u32)(u8)c * 16u));
}

void fb_draw_char8x16(u32 x, u32 y, char c, u32 fg, u32 bg) {
    if (!fb_is_available) {
        return;
    }

    const u8* glyph16 = fb_get_bios_font8x16(c);

    if (x >= fb.width || y >= fb.height) {
        return;
    }

    u32 max_x = x + 8;
    u32 max_y = y + 16;

    if (max_x > fb.width) {
        max_x = fb.width;
    }

    if (max_y > fb.height) {
        max_y = fb.height;
    }

    u32 draw_width = max_x - x;
    u32 draw_height = max_y - y;

    if (draw_width == 0 || draw_height == 0) {
        return;
    }

    if (glyph16) {
        for (u32 row = 0; row < draw_height; row++) {
            u8 bits = glyph16[row];
            u8* pixel = fb_base + (y + row) * fb.pitch + x * fb_pixel_bytes;

            for (u32 col = 0; col < draw_width; col++) {
                u32 color = (bits & (0x80 >> col)) ? fg : bg;
                fb_write_pixel_ptr(pixel, color);
                pixel += fb_pixel_bytes;
            }
        }

        return;
    }

    const u8* glyph8 = font8x8_get(c);

    for (u32 row = 0; row < 8; row++) {
        u32 row_y1 = y + row * 2;

        if (row_y1 >= max_y) {
            break;
        }

        u8 bits = glyph8[row];
        u8* pixel1 = fb_base + row_y1 * fb.pitch + x * fb_pixel_bytes;

        for (u32 col = 0; col < draw_width; col++) {
            u32 color = (bits & (0x80 >> col)) ? fg : bg;
            fb_write_pixel_ptr(pixel1, color);
            pixel1 += fb_pixel_bytes;
        }

        u32 row_y2 = row_y1 + 1;

        if (row_y2 < max_y) {
            u8* pixel2 = fb_base + row_y2 * fb.pitch + x * fb_pixel_bytes;

            for (u32 col = 0; col < draw_width; col++) {
                u32 color = (bits & (0x80 >> col)) ? fg : bg;
                fb_write_pixel_ptr(pixel2, color);
                pixel2 += fb_pixel_bytes;
            }
        }
    }
}

void fb_draw_cursor8x16(u32 x, u32 y, u32 color) {
    if (!fb_is_available) {
        return;
    }

    if (y > 0xFFFFFFFFu - 14u) {
        return;
    }

    fb_fill_rect(x, y + 14, 8, 2, color);
}
