#include "include/naqos.h"

#define W 240u
#define H 140u

static u32 pixels[W * H];

static void clear(u32 color) {
    for (u32 i = 0; i < W * H; i++) {
        pixels[i] = color;
    }
}

static void rect(u32 x, u32 y, u32 w, u32 h, u32 color) {
    for (u32 yy = 0; yy < h; yy++) {
        for (u32 xx = 0; xx < w; xx++) {
            u32 px = x + xx;
            u32 py = y + yy;

            if (px < W && py < H) {
                pixels[py * W + px] = color;
            }
        }
    }
}

int main(void) {
    int win = win_create(120, 90, W, H, "Window App");

    if (win < 0) {
        puts("win_create failed\n");
        return 1;
    }

    for (u32 frame = 0; frame < 2000; frame++) {
        clear(0x00001830);

        rect(8, 8, W - 16, H - 16, 0x00004080);
        rect(20 + (frame % 160), 50, 40, 30, 0x0000E0FF);
        rect(70, 95, 100, 14, 0x0000FF90);

        win_present(win, pixels, W, H);

        sleep_ms(16);
    }

    win_destroy(win);

    puts("window test done\n");
    return 0;
}