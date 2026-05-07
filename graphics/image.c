#include "image.h"
#include "framebuffer.h"
#include "kernel/fat32.h"

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
        fb_draw_xrgb32_row(dst_x, dst_y + row, pixels + row * h->width, draw_width);
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
