#include "include/naqos.h"

static u32 backbuf[320 * 200];

static void fill_backbuf(u32 color) {
    for (u32 i = 0; i < 320u * 200u; i++) {
        backbuf[i] = color;
    }
}

static void rect(u32 x, u32 y, u32 w, u32 h, u32 color) {
    for (u32 yy = 0; yy < h; yy++) {
        for (u32 xx = 0; xx < w; xx++) {
            u32 px = x + xx;
            u32 py = y + yy;

            if (px < 320 && py < 200) {
                backbuf[py * 320 + px] = color;
            }
        }
    }
}

int main(void) {
    naq_gfx_info_t info;

    puts("GFX test app\n");

    if (gfx_info(&info) < 0) {
        puts("gfx_info failed\n");
        return 1;
    }

    gfx_clear(0x00103050);

    for (u32 frame = 0; frame < 120; frame++) {
        fill_backbuf(0x00002040);

        rect(10, 10, 300, 180, 0x00005090);
        rect(20 + (frame % 220), 70, 60, 40, 0x0000D0FF);
        rect(80, 120, 160, 20, 0x0000FF90);

        gfx_blit(80, 80, backbuf, 320, 200);

        sleep_ms(16);
    }

    puts("GFX test done\n");
    return 0;
}