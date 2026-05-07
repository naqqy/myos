#ifndef GRAPHICS_IMAGE_H
#define GRAPHICS_IMAGE_H

typedef unsigned char  u8;
typedef unsigned int   u32;

#define NIMG_MAGIC          0x474D494Eu  /* "NIMG" */
#define NIMG_FORMAT_XRGB32  32u

typedef struct nimg_header {
    u32 magic;
    u32 width;
    u32 height;
    u32 format;
} __attribute__((packed)) nimg_header_t;

int image_draw_nimg_buffer(const u8* data, u32 size, u32 dst_x, u32 dst_y);
int image_draw_nimg_buffer_centered(const u8* data, u32 size);
int image_draw_nimg_file_centered(const char* path, u8* load_buffer, u32 load_buffer_size);

#endif