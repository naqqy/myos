#include "include/naqos.h"
#include "include/render3d.h"

/*
 * WOLF3D.ELF
 *
 * Bounds-fixed optimized Wolfenstein-style raycaster.
 *
 * Important:
 *   All raycasting/background/minimap drawing happens in the low-res VIEW
 *   buffer. Only scale_view_to_present() writes to the full-size PRESENT
 *   buffer.
 *
 * This avoids writing WOLF_W/WOLF_H pixels into WOLF_VIEW_W/WOLF_VIEW_H
 * buffers, which was causing a user-mode page fault just past .bss.
 *
 * Controls:
 *   W/S = forward/back
 *   Q/E = strafe
 *   A/D = turn left/right
 *   Z   = quit
 */

#define WOLF_VIEW_W 300u
#define WOLF_VIEW_H 200u

#define WOLF_SCALE 2u

#define WOLF_W (WOLF_VIEW_W * WOLF_SCALE)
#define WOLF_H (WOLF_VIEW_H * WOLF_SCALE)

#define MAP_W 16
#define MAP_H 16

#define FP_SHIFT R3D_FP_SHIFT
#define FP_ONE   R3D_FP_ONE

#define PLAYER_RADIUS (FP_ONE / 5)
#define MOVE_STEP     (FP_ONE / 4)
#define TURN_STEP     2

/*
 * 0.66-ish gives a classic Wolfenstein-style FOV.
 */
#define CAMERA_PLANE_SCALE ((FP_ONE * 2) / 3)

static u32 g_view_pixels[WOLF_VIEW_W * WOLF_VIEW_H];
static u32 g_present_pixels[WOLF_W * WOLF_H];

static u32 g_background[WOLF_VIEW_W * WOLF_VIEW_H];
static i32 g_camera_x[WOLF_VIEW_W];
static int g_tables_ready = 0;

static const char g_map[MAP_H][MAP_W + 1] = {
    "1111111111111111",
    "1000000000000001",
    "1020000000000301",
    "1000000110000001",
    "1000000110000001",
    "1000000000000001",
    "1000400000500001",
    "1000000000000001",
    "1000011111100001",
    "1000000000000001",
    "1000000000000001",
    "1003000000000201",
    "1000000000000001",
    "1000000000000001",
    "1000000000000001",
    "1111111111111111"
};

typedef struct wolf_player {
    i32 x;
    i32 y;
    int angle;
} wolf_player_t;

typedef struct wolf_ray_hit {
    int side;
    int wall_type;
    i32 dist;
    i32 hit_x;
    i32 hit_y;
} wolf_ray_hit_t;

static i32 fp_abs(i32 v) {
    return v < 0 ? -v : v;
}

static i32 fp_mul(i32 a, i32 b) {
    return r3d_fp_mul(a, b);
}

static i32 fp_div(i32 a, i32 b) {
    return r3d_fp_div(a, b);
}

static int map_at(int x, int y) {
    if (x < 0 || y < 0 || x >= MAP_W || y >= MAP_H) {
        return '1';
    }

    return g_map[y][x];
}

static int map_is_wall_at_cell(int x, int y) {
    return map_at(x, y) != '0';
}

static int map_is_wall_at_pos(i32 x, i32 y) {
    int mx = x >> FP_SHIFT;
    int my = y >> FP_SHIFT;

    return map_is_wall_at_cell(mx, my);
}

static u32 shade_color(u32 color, int shade) {
    if (shade < 32) {
        shade = 32;
    }

    if (shade > 255) {
        shade = 255;
    }

    u32 r = (color >> 16) & 0xFFu;
    u32 g = (color >> 8) & 0xFFu;
    u32 b = color & 0xFFu;

    r = (r * (u32)shade) / 255u;
    g = (g * (u32)shade) / 255u;
    b = (b * (u32)shade) / 255u;

    return (r << 16) | (g << 8) | b;
}

static u32 wall_base_color(int wall_type) {
    if (wall_type == '2') {
        return 0x00E05050u;
    }

    if (wall_type == '3') {
        return 0x0058A8FFu;
    }

    if (wall_type == '4') {
        return 0x00D8D850u;
    }

    if (wall_type == '5') {
        return 0x0080E070u;
    }

    return 0x00B0B0B8u;
}

static void put_view_pixel(u32* pixels, int x, int y, u32 color) {
    if (!pixels) {
        return;
    }

    if (x < 0 || y < 0 ||
        x >= (int)WOLF_VIEW_W ||
        y >= (int)WOLF_VIEW_H) {
        return;
    }

    pixels[(u32)y * WOLF_VIEW_W + (u32)x] = color;
}

static void draw_view_vline(u32* pixels, int x, int y0, int y1, u32 color) {
    if (!pixels || x < 0 || x >= (int)WOLF_VIEW_W) {
        return;
    }

    if (y0 < 0) {
        y0 = 0;
    }

    if (y1 > (int)WOLF_VIEW_H) {
        y1 = (int)WOLF_VIEW_H;
    }

    if (y0 >= y1) {
        return;
    }

    u32* dst = pixels + ((u32)y0 * WOLF_VIEW_W) + (u32)x;

    for (int y = y0; y < y1; y++) {
        *dst = color;
        dst += WOLF_VIEW_W;
    }
}

static void draw_view_rect(u32* pixels, int x, int y, int w, int h, u32 color) {
    if (!pixels || w <= 0 || h <= 0) {
        return;
    }

    int x0 = x;
    int y0 = y;
    int x1 = x + w;
    int y1 = y + h;

    if (x0 < 0) {
        x0 = 0;
    }

    if (y0 < 0) {
        y0 = 0;
    }

    if (x1 > (int)WOLF_VIEW_W) {
        x1 = (int)WOLF_VIEW_W;
    }

    if (y1 > (int)WOLF_VIEW_H) {
        y1 = (int)WOLF_VIEW_H;
    }

    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    for (int yy = y0; yy < y1; yy++) {
        u32* row = pixels + ((u32)yy * WOLF_VIEW_W);

        for (int xx = x0; xx < x1; xx++) {
            row[(u32)xx] = color;
        }
    }
}

static void build_static_tables(void) {
    if (g_tables_ready) {
        return;
    }

    /*
     * Background is VIEW-sized, not PRESENT-sized.
     */
    for (u32 y = 0; y < WOLF_VIEW_H / 2u; y++) {
        u32 shade = 40u + (y * 48u) / (WOLF_VIEW_H / 2u);
        u32 color = (shade << 16) | ((shade + 8u) << 8) | (shade + 18u);

        for (u32 x = 0; x < WOLF_VIEW_W; x++) {
            g_background[y * WOLF_VIEW_W + x] = color;
        }
    }

    for (u32 y = WOLF_VIEW_H / 2u; y < WOLF_VIEW_H; y++) {
        u32 fy = y - WOLF_VIEW_H / 2u;
        u32 g = 54u + (fy * 52u) / (WOLF_VIEW_H / 2u);
        u32 color = (28u << 16) | (g << 8) | 34u;

        for (u32 x = 0; x < WOLF_VIEW_W; x++) {
            g_background[y * WOLF_VIEW_W + x] = color;
        }
    }

    for (u32 x = 0; x < WOLF_VIEW_W; x++) {
        g_background[(WOLF_VIEW_H / 2u) * WOLF_VIEW_W + x] = 0x00608070u;
    }

    /*
     * camera_x is VIEW-sized, not PRESENT-sized.
     */
    for (u32 x = 0; x < WOLF_VIEW_W; x++) {
        g_camera_x[x] =
            (i32)(((i64)2 * (i64)x * (i64)FP_ONE) /
                  (i64)(WOLF_VIEW_W - 1u)) -
            FP_ONE;
    }

    g_tables_ready = 1;
}

static void clear_scene(u32* pixels) {
    /*
     * Manual copy instead of memcpy so this stays simple/freestanding and
     * makes the copied size obvious.
     */
    for (u32 i = 0; i < WOLF_VIEW_W * WOLF_VIEW_H; i++) {
        pixels[i] = g_background[i];
    }
}

static void cast_ray_dir(
    wolf_player_t* player,
    i32 ray_x,
    i32 ray_y,
    wolf_ray_hit_t* out
) {
    if (ray_x == 0) {
        ray_x = 1;
    }

    if (ray_y == 0) {
        ray_y = 1;
    }

    int map_x = player->x >> FP_SHIFT;
    int map_y = player->y >> FP_SHIFT;

    int step_x = ray_x < 0 ? -1 : 1;
    int step_y = ray_y < 0 ? -1 : 1;

    i32 delta_x = fp_abs(fp_div(FP_ONE, ray_x));
    i32 delta_y = fp_abs(fp_div(FP_ONE, ray_y));

    i32 side_x;
    i32 side_y;

    if (ray_x < 0) {
        side_x = fp_mul(player->x - (map_x << FP_SHIFT), delta_x);
    } else {
        side_x = fp_mul(((map_x + 1) << FP_SHIFT) - player->x, delta_x);
    }

    if (ray_y < 0) {
        side_y = fp_mul(player->y - (map_y << FP_SHIFT), delta_y);
    } else {
        side_y = fp_mul(((map_y + 1) << FP_SHIFT) - player->y, delta_y);
    }

    out->side = 0;
    out->wall_type = '1';
    out->dist = 16 * FP_ONE;
    out->hit_x = player->x;
    out->hit_y = player->y;

    for (int i = 0; i < 96; i++) {
        if (side_x < side_y) {
            side_x += delta_x;
            map_x += step_x;
            out->side = 0;
        } else {
            side_y += delta_y;
            map_y += step_y;
            out->side = 1;
        }

        if (map_is_wall_at_cell(map_x, map_y)) {
            i32 numerator;

            out->wall_type = map_at(map_x, map_y);

            if (out->side == 0) {
                if (step_x > 0) {
                    numerator = (map_x << FP_SHIFT) - player->x;
                } else {
                    numerator = ((map_x + 1) << FP_SHIFT) - player->x;
                }

                out->dist = fp_div(numerator, ray_x);
            } else {
                if (step_y > 0) {
                    numerator = (map_y << FP_SHIFT) - player->y;
                } else {
                    numerator = ((map_y + 1) << FP_SHIFT) - player->y;
                }

                out->dist = fp_div(numerator, ray_y);
            }

            if (out->dist < FP_ONE / 16) {
                out->dist = FP_ONE / 16;
            }

            out->hit_x = player->x + fp_mul(ray_x, out->dist);
            out->hit_y = player->y + fp_mul(ray_y, out->dist);
            return;
        }
    }
}

static void render_walls(u32* pixels, wolf_player_t* player) {
    i32 dir_x = r3d_sin(player->angle);
    i32 dir_y = r3d_cos(player->angle);

    /*
     * Right/perpendicular camera plane.
     */
    i32 plane_x = fp_mul(r3d_cos(player->angle), CAMERA_PLANE_SCALE);
    i32 plane_y = fp_mul(-r3d_sin(player->angle), CAMERA_PLANE_SCALE);

    for (int x = 0; x < (int)WOLF_VIEW_W; x++) {
        i32 camera_x = g_camera_x[x];

        i32 ray_x = dir_x + fp_mul(plane_x, camera_x);
        i32 ray_y = dir_y + fp_mul(plane_y, camera_x);

        wolf_ray_hit_t hit;
        cast_ray_dir(player, ray_x, ray_y, &hit);

        i32 corrected_dist = hit.dist;

        if (corrected_dist < FP_ONE / 8) {
            corrected_dist = FP_ONE / 8;
        }

        int wall_h =
            (int)(((i64)WOLF_VIEW_H * (i64)FP_ONE) /
                  (i64)corrected_dist);

        if (wall_h > (int)WOLF_VIEW_H * 2) {
            wall_h = (int)WOLF_VIEW_H * 2;
        }

        int y0 = ((int)WOLF_VIEW_H - wall_h) / 2;
        int y1 = y0 + wall_h;

        if (y0 < 0) {
            y0 = 0;
        }

        if (y1 > (int)WOLF_VIEW_H) {
            y1 = (int)WOLF_VIEW_H;
        }

        int shade = 255 - (corrected_dist >> 7);

        if (hit.side) {
            shade -= 45;
        }

        u32 color = shade_color(wall_base_color(hit.wall_type), shade);

        /*
         * Cheap vertical stripe using impact coordinate.
         */
        i32 tex = hit.side ?
            (hit.hit_x & (FP_ONE - 1)) :
            (hit.hit_y & (FP_ONE - 1));

        if ((tex >> (FP_SHIFT - 3)) & 1) {
            color = shade_color(color, 210);
        }

        draw_view_vline(pixels, x, y0, y1, color);

        if (y0 > 0) {
            put_view_pixel(pixels, x, y0, 0x00E8F8FFu);
        }

        if (y1 < (int)WOLF_VIEW_H) {
            put_view_pixel(pixels, x, y1, 0x00203030u);
        }
    }
}

static void draw_minimap(u32* pixels, wolf_player_t* player) {
    int scale = 4;
    int ox = 7;
    int oy = 7;

    draw_view_rect(
        pixels,
        ox - 2,
        oy - 2,
        MAP_W * scale + 4,
        MAP_H * scale + 4,
        0x00102018u
    );

    for (int y = 0; y < MAP_H; y++) {
        for (int x = 0; x < MAP_W; x++) {
            u32 color =
                map_is_wall_at_cell(x, y) ?
                0x00A8E0B0u :
                0x00203028u;

            draw_view_rect(
                pixels,
                ox + x * scale,
                oy + y * scale,
                scale - 1,
                scale - 1,
                color
            );
        }
    }

    /*
     * Scale before dropping the fraction so the marker moves smoothly
     * instead of snapping to whole map cells.
     */
    int px = ox + ((player->x * scale) >> FP_SHIFT);
    int py = oy + ((player->y * scale) >> FP_SHIFT);

    draw_view_rect(pixels, px - 1, py - 1, 3, 3, 0x00FF4040u);

    int dx = (r3d_sin(player->angle) * 6) >> FP_SHIFT;
    int dy = (r3d_cos(player->angle) * 6) >> FP_SHIFT;

    draw_view_rect(pixels, px + dx, py + dy, 2, 2, 0x00FFFFFFu);
}

static void scale_view_to_present(const u32* src, u32* dst) {
    if (!src || !dst) {
        return;
    }

    for (u32 y = 0; y < WOLF_VIEW_H; y++) {
        u32* row0 = dst + ((y * 2u) * WOLF_W);
        u32* row1 = row0 + WOLF_W;
        const u32* srow = src + (y * WOLF_VIEW_W);

        for (u32 x = 0; x < WOLF_VIEW_W; x++) {
            u32 c = srow[x];
            u32 dx = x * 2u;

            row0[dx] = c;
            row0[dx + 1u] = c;
            row1[dx] = c;
            row1[dx + 1u] = c;
        }
    }
}

static int try_move(wolf_player_t* player, i32 dx, i32 dy) {
    i32 new_x = player->x + dx;
    i32 new_y = player->y + dy;

    /*
     * Separate axis resolution gives wall sliding.
     */
    if (!map_is_wall_at_pos(new_x - PLAYER_RADIUS, player->y) &&
        !map_is_wall_at_pos(new_x + PLAYER_RADIUS, player->y) &&
        !map_is_wall_at_pos(new_x, player->y - PLAYER_RADIUS) &&
        !map_is_wall_at_pos(new_x, player->y + PLAYER_RADIUS)) {
        player->x = new_x;
    }

    if (!map_is_wall_at_pos(player->x - PLAYER_RADIUS, new_y) &&
        !map_is_wall_at_pos(player->x + PLAYER_RADIUS, new_y) &&
        !map_is_wall_at_pos(player->x, new_y - PLAYER_RADIUS) &&
        !map_is_wall_at_pos(player->x, new_y + PLAYER_RADIUS)) {
        player->y = new_y;
    }

    return 1;
}

static void move_forward(wolf_player_t* player, i32 amount) {
    try_move(
        player,
        fp_mul(r3d_sin(player->angle), amount),
        fp_mul(r3d_cos(player->angle), amount)
    );
}

static void move_right(wolf_player_t* player, i32 amount) {
    try_move(
        player,
        fp_mul(r3d_cos(player->angle), amount),
        -fp_mul(r3d_sin(player->angle), amount)
    );
}

static void handle_key(wolf_player_t* player, u32 key, int* running) {
    if (key == 'z' || key == 'Z') {
        *running = 0;
    } else if (key == 'w' || key == 'W') {
        move_forward(player, MOVE_STEP);
    } else if (key == 's' || key == 'S') {
        move_forward(player, -MOVE_STEP);
    } else if (key == 'q' || key == 'Q') {
        move_right(player, -MOVE_STEP);
    } else if (key == 'e' || key == 'E') {
        move_right(player, MOVE_STEP);
    } else if (key == 'a' || key == 'A') {
        player->angle = (player->angle - TURN_STEP) & 63;
    } else if (key == 'd' || key == 'D') {
        player->angle = (player->angle + TURN_STEP) & 63;
    }
}

static void handle_events(int win, wolf_player_t* player, int* running) {
    naq_win_event_t ev;

    while (win_get_event(win, &ev) == 1) {
        if (ev.type == NAQ_WIN_EVENT_CLOSE) {
            *running = 0;
        } else if (ev.type == NAQ_WIN_EVENT_KEY) {
            handle_key(player, ev.key, running);
        }
    }
}

int main(void) {
    build_static_tables();

    int win = win_create(40, 40, WOLF_W, WOLF_H, "Wolf3D");

    if (win < 0) {
        puts("wolf3d: win_create failed\n");
        return 1;
    }

    puts("Wolf3D controls:\n");
    puts("  W/S forward/back, Q/E strafe, A/D turn, Z quit\n");

    wolf_player_t player;

    player.x = 1 * FP_ONE + FP_ONE / 2;
    player.y = 1 * FP_ONE + FP_ONE / 2;
    player.angle = 0;

    naq_frame_timer_t timer;
    frame_timer_init(&timer, 30);

    int running = 1;

    while (running) {
        frame_timer_begin(&timer);

        handle_events(win, &player, &running);

        clear_scene(g_view_pixels);
        render_walls(g_view_pixels, &player);
        draw_minimap(g_view_pixels, &player);

        scale_view_to_present(g_view_pixels, g_present_pixels);

        win_present(win, g_present_pixels, WOLF_W, WOLF_H);

        frame_timer_end(&timer);
    }

    win_destroy(win);
    puts("Wolf3D closed\n");

    return 0;
}
