#ifndef IMAGE64_H
#define IMAGE64_H

#include "naq_boot_info.h"

#define NIMG64_MAGIC          0x474D494Eu  /* "NIMG" */
#define NIMG64_FORMAT_XRGB32  32u

typedef struct nimg64_header {
    nq_u32 magic;
    nq_u32 width;
    nq_u32 height;
    nq_u32 format;
} __attribute__((packed)) nimg64_header_t;

int image64_validate_nimg(
    const nq_u8* data,
    nq_u64 size,
    const nimg64_header_t** out_header
);

int image64_draw_nimg_buffer(
    const nq_u8* data,
    nq_u64 size,
    nq_u32 dst_x,
    nq_u32 dst_y
);

int image64_draw_nimg_buffer_centered(
    const nq_u8* data,
    nq_u64 size
);

int image64_draw_nimg_file_centered(
    const char* path,
    nq_u8* load_buffer,
    nq_u64 load_buffer_size
);

#endif