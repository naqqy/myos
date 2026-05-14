#include "include/naqos.h"

#define SCREEN_W 320u
#define SCREEN_H 200u

#define COLOR_BG      0x00102030u
#define COLOR_PANEL   0x00003060u
#define COLOR_GRID    0x00004070u
#define COLOR_PLAYER  0x0000E0FFu
#define COLOR_BOX     0x0000FF90u
#define COLOR_WHITE   0x00FFFFFFu
#define COLOR_RED     0x00FF4040u
#define COLOR_YELLOW  0x00FFD060u

static u32 g_pixels_a[SCREEN_W * SCREEN_H];
static u32 g_pixels_b[SCREEN_W * SCREEN_H];

static void draw_border(naq_gfx_buffer_t* buf) {
    gfx_buffer_draw_rect(buf, 0, 0, SCREEN_W, 4, COLOR_PANEL);
    gfx_buffer_draw_rect(buf, 0, SCREEN_H - 4, SCREEN_W, 4, COLOR_PANEL);
    gfx_buffer_draw_rect(buf, 0, 0, 4, SCREEN_H, COLOR_PANEL);
    gfx_buffer_draw_rect(buf, SCREEN_W - 4, 0, 4, SCREEN_H, COLOR_PANEL);
}

static void draw_grid(naq_gfx_buffer_t* buf) {
    for (u32 x = 16; x < SCREEN_W; x += 16) {
        gfx_buffer_draw_rect(buf, (i32)x, 4, 1, SCREEN_H - 8, COLOR_GRID);
    }

    for (u32 y = 16; y < SCREEN_H; y += 16) {
        gfx_buffer_draw_rect(buf, 4, (i32)y, SCREEN_W - 8, 1, COLOR_GRID);
    }
}

static void draw_sprite_smiley(naq_gfx_buffer_t* buf, i32 x, i32 y) {
    /*
     * Simple 16x16 sprite drawn from rectangles/pixels.
     */
    gfx_buffer_draw_rect(buf, x + 2, y + 2, 12, 12, COLOR_YELLOW);
    gfx_buffer_draw_rect(buf, x + 4, y + 5, 2, 2, COLOR_BG);
    gfx_buffer_draw_rect(buf, x + 10, y + 5, 2, 2, COLOR_BG);

    gfx_buffer_put_pixel(buf, x + 5, y + 11, COLOR_BG);
    gfx_buffer_put_pixel(buf, x + 6, y + 12, COLOR_BG);
    gfx_buffer_put_pixel(buf, x + 7, y + 12, COLOR_BG);
    gfx_buffer_put_pixel(buf, x + 8, y + 12, COLOR_BG);
    gfx_buffer_put_pixel(buf, x + 9, y + 12, COLOR_BG);
    gfx_buffer_put_pixel(buf, x + 10, y + 11, COLOR_BG);
}

static void handle_input(i32* player_x, i32* player_y, int* running) {
    for (;;) {
        int c = input_poll_key();

        if (c == 0) {
            break;
        }

        if (c == 'q' || c == 'Q') {
            *running = 0;
            return;
        }

        if (c == 'a' || c == 'A') {
            *player_x -= 4;
        } else if (c == 'd' || c == 'D') {
            *player_x += 4;
        } else if (c == 'w' || c == 'W') {
            *player_y -= 4;
        } else if (c == 's' || c == 'S') {
            *player_y += 4;
        }

        if (*player_x < 6) {
            *player_x = 6;
        }

        if (*player_y < 6) {
            *player_y = 6;
        }

        if (*player_x > (i32)SCREEN_W - 22) {
            *player_x = (i32)SCREEN_W - 22;
        }

        if (*player_y > (i32)SCREEN_H - 22) {
            *player_y = (i32)SCREEN_H - 22;
        }
    }
}

int main(void) {
    naq_gfx_info_t info;
    naq_frame_timer_t timer;

    naq_gfx_buffer_t buffers[2];
    u32 current_buffer = 0;

    i32 player_x = 40;
    i32 player_y = 50;

    i32 box_x = 120;
    i32 box_y = 80;
    i32 box_vx = 2;
    i32 box_vy = 1;

    int running = 1;

    /*
     * Enter graphical app mode before drawing.
     */
    if (gfx_set_mode(GFX_MODE_GRAPHICS) < 0) {
        puts("demo2d: gfx_set_mode failed\n");
        return 1;
    }

    if (gfx_info(&info) < 0) {
        puts("demo2d: gfx_info failed\n");
        return 1;
    }

    gfx_buffer_init(&buffers[0], g_pixels_a, SCREEN_W, SCREEN_H);
    gfx_buffer_init(&buffers[1], g_pixels_b, SCREEN_W, SCREEN_H);

    frame_timer_init(&timer, 30);

    while (running) {
        naq_gfx_buffer_t* buf = &buffers[current_buffer];

        frame_timer_begin(&timer);

        handle_input(&player_x, &player_y, &running);

        box_x += box_vx;
        box_y += box_vy;

        if (box_x < 8 || box_x > (i32)SCREEN_W - 28) {
            box_vx = -box_vx;
            box_x += box_vx;
        }

        if (box_y < 8 || box_y > (i32)SCREEN_H - 28) {
            box_vy = -box_vy;
            box_y += box_vy;
        }

        gfx_buffer_clear(buf, COLOR_BG);
        draw_grid(buf);
        draw_border(buf);

        gfx_buffer_draw_rect(buf, box_x, box_y, 20, 20, COLOR_BOX);
        draw_sprite_smiley(buf, box_x + 2, box_y + 2);

        gfx_buffer_draw_rect(buf, player_x, player_y, 16, 16, COLOR_PLAYER);
        gfx_buffer_draw_rect(buf, player_x + 4, player_y + 4, 8, 8, COLOR_WHITE);

        /*
         * Small red marker so motion is obvious.
         */
        gfx_buffer_draw_rect(
            buf,
            8 + (i32)(timer.frame_count % 100),
            184,
            8,
            8,
            COLOR_RED
        );

        gfx_buffer_present(buf, 80, 80);

        current_buffer ^= 1u;

        frame_timer_end(&timer);
    }

    gfx_set_mode(GFX_MODE_TEXT);
    puts("2D graphics demo done\n");

    return 0;
}