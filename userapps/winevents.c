#include "include/naqos.h"

/*
 * WINEVENTS.ELF
 *
 * Window event test: keyboard, mouse, resize and close.
 *
 *   W/A/S/D        move the square
 *   mouse move     crosshair follows the pointer
 *   left click     move the square to the pointer
 *   right button   hold to turn the square orange
 *   resize window  the app re-renders at the new size (up to MAX_W x MAX_H;
 *                  beyond that the desktop stretches the frame)
 *   Q or close     quit
 */

#define START_W 240u
#define START_H 140u

#define MAX_W 800u
#define MAX_H 600u

static u32 g_pixels[MAX_W * MAX_H];

/*
 * Current frame size. Follows the window's content size.
 */
static u32 g_w = START_W;
static u32 g_h = START_H;

/*
 * Size of the window's content area, which can be larger than the frame.
 * Mouse coordinates arrive in window pixels and are scaled with these.
 */
static u32 g_window_w = START_W;
static u32 g_window_h = START_H;

static void clear(u32 color) {
    for (u32 i = 0; i < g_w * g_h; i++) {
        g_pixels[i] = color;
    }
}

static void rect(i32 x, i32 y, i32 w, i32 h, u32 color) {
    for (i32 yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= (i32)g_h) {
            continue;
        }

        for (i32 xx = x; xx < x + w; xx++) {
            if (xx >= 0 && xx < (i32)g_w) {
                g_pixels[(u32)yy * g_w + (u32)xx] = color;
            }
        }
    }
}

static void set_size(u32 window_w, u32 window_h) {
    g_window_w = window_w ? window_w : 1;
    g_window_h = window_h ? window_h : 1;

    g_w = g_window_w > MAX_W ? MAX_W : g_window_w;
    g_h = g_window_h > MAX_H ? MAX_H : g_window_h;
}

int main(void) {
    int win = win_create(120, 90, START_W, START_H, "Event Test");

    if (win < 0) {
        puts("win_create failed\n");
        return 1;
    }

    i32 player_x = 24;
    i32 player_y = 24;
    i32 mouse_x = -100;
    i32 mouse_y = -100;
    u32 buttons = 0;
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
            } else if (ev.type == NAQ_WIN_EVENT_MOUSE) {
                /*
                 * Window pixels -> frame pixels.
                 */
                mouse_x = (i32)(((i64)ev.x * (i64)g_w) / (i64)g_window_w);
                mouse_y = (i32)(((i64)ev.y * (i64)g_h) / (i64)g_window_h);
                buttons = ev.key;

                if (ev.w == NAQ_MOUSE_PRESS &&
                    ev.h == (i32)NAQ_MOUSE_BUTTON_LEFT) {
                    player_x = mouse_x - 10;
                    player_y = mouse_y - 10;
                }
            } else if (ev.type == NAQ_WIN_EVENT_RESIZE) {
                set_size((u32)ev.w, (u32)ev.h);
            } else if (ev.type == NAQ_WIN_EVENT_CLOSE) {
                running = 0;
            }
        }

        if (player_x < 8) player_x = 8;
        if (player_y < 8) player_y = 8;
        if (player_x > (i32)g_w - 28) player_x = (i32)g_w - 28;
        if (player_y > (i32)g_h - 28) player_y = (i32)g_h - 28;

        clear(0x00001428u);
        rect(8, 8, (i32)g_w - 16, (i32)g_h - 16, 0x00003060u);

        rect(player_x, player_y, 20, 20,
             (buttons & NAQ_MOUSE_BUTTON_RIGHT) ? 0x00FFA030u : 0x0000E0FFu);

        rect(16 + (i32)(frame % (g_w > 60 ? g_w - 60 : 1)), (i32)g_h - 28,
             18, 10, 0x0000FF90u);

        /*
         * Crosshair at the pointer; brighter while the left button is down.
         */
        u32 cross = (buttons & NAQ_MOUSE_BUTTON_LEFT) ?
            0x00FFFFFFu : 0x0090C0D0u;

        rect(mouse_x - 8, mouse_y, 17, 1, cross);
        rect(mouse_x, mouse_y - 8, 1, 17, cross);

        win_present(win, g_pixels, g_w, g_h);
        frame++;
        sleep_ms(16);
    }

    win_destroy(win);
    return 0;
}
