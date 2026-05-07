#include "image.h"
#include "framebuffer.h"
#include "kernel/fat32.h"

static inline u16 image_color_to_rgb565(u32 color) {
    u8 r = (u8)((color >> 16) & 0xFF);
    u8 g = (u8)((color >> 8) & 0xFF);
    u8 b = (u8)(color & 0xFF);

    return (u16)(((r >> 3) << 11) |
                  ((g >> 2) << 5) |
                  (b >> 3));
}

static int nimg_validate(const u8* data, u32 size, const nimg_header_t** out_header) {
    const nimg_header_t* h;
    u32 pixel_bytes;
    u32 expected_size;

    if (!data || !out_header) {
        return 0;
    }

    if (size < sizeof(nimg_header_t)) {
        return 0;
    }

    h = (const nimg_header_t*)data;

    if (h->magic != NIMG_MAGIC) {
        return 0;
    }

    if (h->format != NIMG_FORMAT_XRGB32) {
        return 0;
    }

    if (h->width == 0 || h->height == 0) {
        return 0;
    }

    if (h->width > 0xFFFFFFFFu / h->height) {
        return 0;
    }

    pixel_bytes = h->width * h->height;

    if (pixel_bytes > 0xFFFFFFFFu / 4u) {
        return 0;
    }

    pixel_bytes *= 4u;

    if (pixel_bytes > 0xFFFFFFFFu - sizeof(nimg_header_t)) {
        return 0;
    }

    expected_size = sizeof(nimg_header_t) + pixel_bytes;

    if (size < expected_size) {
        return 0;
    }

    *out_header = h;
    return 1;
}

static void image_draw_nimg_row(u32 dst_x, u32 dst_y, const u32* src_pixels, u32 count) {
    u32 bpp = fb_bpp();
    u32 pitch = fb_pitch();
    u32 bytes_per_pixel = (bpp + 7) / 8;
    u8* dest = (u8*)(unsigned long)fb_phys_addr() + dst_y * pitch + dst_x * bytes_per_pixel;

    if (bpp == 32) {
        u32* pixel = (u32*)dest;

        for (u32 i = 0; i < count; i++) {
            pixel[i] = src_pixels[i];
        }

        return;
    }

    if (bpp == 24) {
        u8* pixel = dest;

        for (u32 i = 0; i < count; i++) {
            u32 color = src_pixels[i];
            pixel[0] = (u8)(color & 0xFF);
            pixel[1] = (u8)((color >> 8) & 0xFF);
            pixel[2] = (u8)((color >> 16) & 0xFF);
            pixel += 3;
        }

        return;
    }

    u16* pixel = (u16*)dest;

    for (u32 i = 0; i < count; i++) {
        pixel[i] = image_color_to_rgb565(src_pixels[i]);
    }
}

int image_draw_nimg_buffer(const u8* data, u32 size, u32 dst_x, u32 dst_y) {
    const nimg_header_t* h;
    const u32* pixels;
    u32 fb_w;
    u32 fb_h;
    u32 draw_width;
    u32 draw_height;

    if (!fb_available()) {
        return 0;
    }

    if (!nimg_validate(data, size, &h)) {
        return 0;
    }

    fb_w = fb_width();
    fb_h = fb_height();

    if (dst_x >= fb_w || dst_y >= fb_h) {
        return 1;
    }

    pixels = (const u32*)(data + sizeof(nimg_header_t));
    draw_width = h->width;
    draw_height = h->height;

    if (draw_width > fb_w - dst_x) {
        draw_width = fb_w - dst_x;
    }

    if (draw_height > fb_h - dst_y) {
        draw_height = fb_h - dst_y;
    }

    for (u32 row = 0; row < draw_height; row++) {
        image_draw_nimg_row(dst_x, dst_y + row, pixels + row * h->width, draw_width);
    }

    return 1;
}

int image_draw_nimg_buffer_centered(const u8* data, u32 size) {
    const nimg_header_t* h;
    u32 x = 0;
    u32 y = 0;

    if (!fb_available()) {
        return 0;
    }

    if (!nimg_validate(data, size, &h)) {
        return 0;
    }

    if (fb_width() > h->width) {
        x = (fb_width() - h->width) / 2;
    }

    if (fb_height() > h->height) {
        y = (fb_height() - h->height) / 2;
    }

    return image_draw_nimg_buffer(data, size, x, y);
}

int image_draw_nimg_file_centered(const char* path, u8* load_buffer, u32 load_buffer_size) {
    u32 loaded_size = 0;

    if (!path || !load_buffer || load_buffer_size == 0) {
        return 0;
    }

    if (!fat32_load_file_to_memory_path(path, load_buffer, load_buffer_size, &loaded_size)) {
        return 0;
    }

    return image_draw_nimg_buffer_centered(load_buffer, loaded_size);
}
