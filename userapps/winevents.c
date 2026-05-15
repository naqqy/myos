#include "include/naqos.h"

#define W 240u
#define H 140u

static u32 g_pixels[W * H];

static void clear(u32 color) {
    for (u32 i = 0; i < W * H; i++) {
        g_pixels[i] = color;
    }
}

static void rect(u32 x, u32 y, u32 w, u32 h, u32 color) {
    for (u32 yy = 0; yy < h; yy++) {
        for (u32 xx = 0; xx < w; xx++) {
            u32 px = x + xx;
            u32 py = y + yy;

            if (px < W && py < H) {
                g_pixels[py * W + px] = color;
            }
        }
    }
}

int main(void) {
    int win = win_create(120, 90, W, H, "Event Test");

    if (win < 0) {
        puts("win_create failed\n");
        return 1;
    }

    i32 player_x = 24;
    i32 player_y = 24;
    u32 frame = 0;
    int running = 1;

    while (running) {
        naq_win_event_t ev;

        while (win_get_event(win, &ev) == 1) {
            if (ev.type == NAQ_WIN_EVENT_KEY) {
                if (ev.key == 'q' || ev.key == 'Q') {
                    running = 0;
                } else if (ev.key == 'a' || ev.key == 'A') {
                    player_x -= 5;
                } else if (ev.key == 'd' || ev.key == 'D') {
                    player_x += 5;
                } else if (ev.key == 'w' || ev.key == 'W') {
                    player_y -= 5;
                } else if (ev.key == 's' || ev.key == 'S') {
                    player_y += 5;
                }
            } else if (ev.type == NAQ_WIN_EVENT_CLOSE) {
                running = 0;
            }
        }

        if (player_x < 8) player_x = 8;
        if (player_y < 8) player_y = 8;
        if (player_x > (i32)W - 28) player_x = (i32)W - 28;
        if (player_y > (i32)H - 28) player_y = (i32)H - 28;

        clear(0x00001428u);
        rect(8, 8, W - 16, H - 16, 0x00003060u);
        rect((u32)player_x, (u32)player_y, 20, 20, 0x0000E0FFu);
        rect(16 + (frame % 180u), 112, 18, 10, 0x0000FF90u);

        win_present(win, g_pixels, W, H);
        frame++;
        sleep_ms(16);
    }

    win_destroy(win);
    return 0;
}
