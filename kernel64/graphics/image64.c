#include "image64.h"
#include "framebuffer64.h"
#include "../fs/vfs64.h"
#include "../k64_console.h"

int image64_validate_nimg(
    const nq_u8* data,
    nq_u64 size,
    const nimg64_header_t** out_header
) {
    const nimg64_header_t* h;
    nq_u64 pixel_count;
    nq_u64 pixel_bytes;
    nq_u64 expected_size;

    if (out_header) {
        *out_header = 0;
    }

    if (!data || !out_header) {
        return 0;
    }

    if (size < sizeof(nimg64_header_t)) {
        return 0;
    }

    h = (const nimg64_header_t*)data;

    if (h->magic != NIMG64_MAGIC) {
        return 0;
    }

    if (h->format != NIMG64_FORMAT_XRGB32) {
        return 0;
    }

    if (h->width == 0 || h->height == 0) {
        return 0;
    }

    pixel_count = (nq_u64)h->width * (nq_u64)h->height;

    if (h->width != 0 && pixel_count / h->width != h->height) {
        return 0;
    }

    if (pixel_count > 0xFFFFFFFFFFFFFFFFull / 4ull) {
        return 0;
    }

    pixel_bytes = pixel_count * 4ull;

    if (pixel_bytes > 0xFFFFFFFFFFFFFFFFull - sizeof(nimg64_header_t)) {
        return 0;
    }

    expected_size = sizeof(nimg64_header_t) + pixel_bytes;

    if (size < expected_size) {
        return 0;
    }

    *out_header = h;
    return 1;
}

int image64_draw_nimg_buffer(
    const nq_u8* data,
    nq_u64 size,
    nq_u32 dst_x,
    nq_u32 dst_y
) {
    const nimg64_header_t* h;
    const nq_u32* pixels;
    nq_u32 draw_width;
    nq_u32 draw_height;

    if (!fb64_available()) {
        return 0;
    }

    if (!image64_validate_nimg(data, size, &h)) {
        return 0;
    }

    if (dst_x >= fb64_width() || dst_y >= fb64_height()) {
        return 1;
    }

    pixels = (const nq_u32*)(data + sizeof(nimg64_header_t));

    draw_width = h->width;
    draw_height = h->height;

    if (draw_width > fb64_width() - dst_x) {
        draw_width = fb64_width() - dst_x;
    }

    if (draw_height > fb64_height() - dst_y) {
        draw_height = fb64_height() - dst_y;
    }

    for (nq_u32 row = 0; row < draw_height; row++) {
        fb64_draw_xrgb32_row(
            dst_x,
            dst_y + row,
            pixels + ((nq_u64)row * h->width),
            draw_width
        );
    }

    return 1;
}

int image64_draw_nimg_buffer_centered(
    const nq_u8* data,
    nq_u64 size
) {
    const nimg64_header_t* h;
    nq_u32 x = 0;
    nq_u32 y = 0;

    if (!fb64_available()) {
        return 0;
    }

    if (!image64_validate_nimg(data, size, &h)) {
        return 0;
    }

    if (fb64_width() > h->width) {
        x = (fb64_width() - h->width) / 2u;
    }

    if (fb64_height() > h->height) {
        y = (fb64_height() - h->height) / 2u;
    }

    return image64_draw_nimg_buffer(data, size, x, y);
}

int image64_draw_nimg_file_centered(
    const char* path,
    nq_u8* load_buffer,
    nq_u64 load_buffer_size
) {
    nq_u64 loaded_size = 0;

    if (!path || !load_buffer || load_buffer_size == 0) {
        return 0;
    }

    if (!vfs64_read_file(path, load_buffer, load_buffer_size, &loaded_size)) {
        return 0;
    }

    return image64_draw_nimg_buffer_centered(load_buffer, loaded_size);
}