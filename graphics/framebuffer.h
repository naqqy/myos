#ifndef GRAPHICS_FRAMEBUFFER_H
#define GRAPHICS_FRAMEBUFFER_H

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

#define FB_INFO_ADDR 0x00006000u
#define FB_INFO_MAGIC 0x3142464Eu  /* "NFB1" */

#define FONT8X16_INFO_ADDR 0x00003FF0u
#define FONT8X16_ADDR      0x00004000u
#define FONT8X16_MAGIC     0x3154464Eu  /* "NFT1" */

typedef struct framebuffer_info {
    u32 magic;
    u32 phys_addr;
    u32 width;
    u32 height;
    u32 pitch;
    u32 bpp;
} framebuffer_info_t;

void fb_init(void);
int fb_available(void);

u32 fb_width(void);
u32 fb_height(void);
u32 fb_pitch(void);
u32 fb_bpp(void);
u32 fb_phys_addr(void);
u32 fb_size_bytes(void);

void fb_put_pixel(u32 x, u32 y, u32 color);
void fb_fill_rect(u32 x, u32 y, u32 w, u32 h, u32 color);
void fb_clear(u32 color);
void fb_draw_xrgb32_row(u32 x, u32 y, const u32* pixels, u32 count);

void fb_draw_char8x16(u32 x, u32 y, char c, u32 fg, u32 bg);
void fb_draw_cursor8x16(u32 x, u32 y, u32 color);

#endif
