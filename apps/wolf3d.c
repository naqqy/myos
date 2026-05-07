#include "syscall.h"

#define SCREEN_W 60
#define SCREEN_H 20

#define MAP_W 16
#define MAP_H 16

#define FP_SHIFT 8
#define FP_ONE   (1 << FP_SHIFT)

#define FOV_RAYS SCREEN_W

/*
 * Fixed-point trig table.
 * Angles are 0-63.
 * 0   = east
 * 16  = south
 * 32  = west
 * 48  = north
 */
static const int sin_table[64] = {
      0,  25,  50,  74,  98, 121, 142, 162,
    181, 197, 212, 225, 236, 244, 251, 255,
    256, 255, 251, 244, 236, 225, 212, 197,
    181, 162, 142, 121,  98,  74,  50,  25,
      0, -25, -50, -74, -98,-121,-142,-162,
   -181,-197,-212,-225,-236,-244,-251,-255,
   -256,-255,-251,-244,-236,-225,-212,-197,
   -181,-162,-142,-121, -98, -74, -50, -25
};

static const int cos_table[64] = {
    256, 255, 251, 244, 236, 225, 212, 197,
    181, 162, 142, 121,  98,  74,  50,  25,
      0, -25, -50, -74, -98,-121,-142,-162,
   -181,-197,-212,-225,-236,-244,-251,-255,
   -256,-255,-251,-244,-236,-225,-212,-197,
   -181,-162,-142,-121, -98, -74, -50, -25,
      0,  25,  50,  74,  98, 121, 142, 162,
    181, 197, 212, 225, 236, 244, 251, 255
};

static const char world_map[MAP_H][MAP_W + 1] = {
    "################",
    "#..............#",
    "#..##..........#",
    "#..##.....M....#",
    "#..............#",
    "#......####....#",
    "#........#.....#",
    "#........#.....#",
    "#........#.....#",
    "#....M...#.....#",
    "#..............#",
    "#......####....#",
    "#..............#",
    "#...##.........#",
    "#..............#",
    "################"
};

static char screen[SCREEN_H][SCREEN_W + 1];

static int player_x = 3 * FP_ONE;
static int player_y = 3 * FP_ONE;
static int player_angle = 0;

static int running = 1;

static int abs_int(int x) {
    return x < 0 ? -x : x;
}

static int clamp_angle(int a) {
    while (a < 0) {
        a += 64;
    }

    while (a >= 64) {
        a -= 64;
    }

    return a;
}

static char map_at_cell(int x, int y) {
    if (x < 0 || x >= MAP_W || y < 0 || y >= MAP_H) {
        return '#';
    }

    return world_map[y][x];
}

static int is_wall_at_fixed(int fx, int fy) {
    int mx = fx >> FP_SHIFT;
    int my = fy >> FP_SHIFT;

    return map_at_cell(mx, my) == '#';
}

static void clear_screen_buffer(void) {
    for (int y = 0; y < SCREEN_H; y++) {
        for (int x = 0; x < SCREEN_W; x++) {
            if (y < SCREEN_H / 2) {
                screen[y][x] = ' ';
            } else {
                screen[y][x] = '.';
            }
        }

        screen[y][SCREEN_W] = 0;
    }
}

static char wall_shade_for_distance(int dist) {
    if (dist < 2 * FP_ONE) return '#';
    if (dist < 4 * FP_ONE) return 'H';
    if (dist < 6 * FP_ONE) return '=';
    if (dist < 8 * FP_ONE) return '-';
    if (dist < 12 * FP_ONE) return '.';
    return '\'';
}

static void cast_rays(void) {
    /*
     * FOV is about 60 degrees.
     * With 64 angle units per circle, 60 degrees ~= 11 units.
     * So use angle - 10 to angle + 10 across the screen.
     */
    int fov_units = 20;

    for (int col = 0; col < SCREEN_W; col++) {
        int ray_angle = player_angle - (fov_units / 2) +
                        (col * fov_units) / SCREEN_W;

        ray_angle = clamp_angle(ray_angle);

        int ray_dx = cos_table[ray_angle];
        int ray_dy = sin_table[ray_angle];

        int rx = player_x;
        int ry = player_y;

        int dist = 0;
        int hit = 0;
        char hit_cell = '#';

        /*
         * Ray step. Smaller step = smoother but slower.
         */
        int step = FP_ONE / 16;

        for (int i = 0; i < 256; i++) {
            rx += (ray_dx * step) >> FP_SHIFT;
            ry += (ray_dy * step) >> FP_SHIFT;
            dist += step;

            int mx = rx >> FP_SHIFT;
            int my = ry >> FP_SHIFT;

            hit_cell = map_at_cell(mx, my);

            if (hit_cell == '#') {
                hit = 1;
                break;
            }

            /*
             * Treat monster as visible object for now.
             */
            if (hit_cell == 'M') {
                hit = 1;
                break;
            }
        }

        if (!hit) {
            continue;
        }

        /*
         * Basic fish-eye correction using angle difference.
         */
        int diff = clamp_angle(ray_angle - player_angle);
        if (diff > 32) {
            diff -= 64;
        }

        int correction = cos_table[abs_int(diff)];
        if (correction < 1) {
            correction = 1;
        }

        int corrected_dist = (dist * correction) >> FP_SHIFT;
        if (corrected_dist < 1) {
            corrected_dist = 1;
        }

        int wall_height = (SCREEN_H * FP_ONE) / corrected_dist;
        if (wall_height > SCREEN_H) {
            wall_height = SCREEN_H;
        }

        int top = (SCREEN_H - wall_height) / 2;
        int bottom = top + wall_height;

        char shade;

        if (hit_cell == 'M') {
            shade = 'M';
        } else {
            shade = wall_shade_for_distance(corrected_dist);
        }

        for (int y = top; y < bottom; y++) {
            if (y >= 0 && y < SCREEN_H) {
                screen[y][col] = shade;
            }
        }
    }
}

static void draw_minimap(void) {
    /*
     * Small minimap in top-left corner.
     */
    for (int y = 0; y < MAP_H && y < 8; y++) {
        for (int x = 0; x < MAP_W && x < 16; x++) {
            char c = world_map[y][x];

            if ((player_x >> FP_SHIFT) == x &&
                (player_y >> FP_SHIFT) == y) {
                c = '@';
            }

            screen[y][x] = c;
        }
    }
}

static void draw_hud(void) {
    app_write("\n");
    app_write("W/S move  A/D turn  Q quit     Angle: ");
    app_print_dec32((u32)player_angle);
    app_write("\n");
}

static void render(void) {
    clear_screen_buffer();
    cast_rays();
    draw_minimap();

    app_begin_frame();

    app_clear();

    app_write("+");
    for (int i = 0; i < SCREEN_W; i++) {
        app_write("-");
    }
    app_write("+\n");

    for (int y = 0; y < SCREEN_H; y++) {
        app_write("|");
        app_write(screen[y]);
        app_write("|\n");
    }

    app_write("+");
    for (int i = 0; i < SCREEN_W; i++) {
        app_write("-");
    }
    app_write("+");

    draw_hud();

    app_end_frame();
}

static void try_move(int amount) {
    int dx = (cos_table[player_angle] * amount) >> FP_SHIFT;
    int dy = (sin_table[player_angle] * amount) >> FP_SHIFT;

    int nx = player_x + dx;
    int ny = player_y + dy;

    /*
     * Simple collision.
     */
    if (!is_wall_at_fixed(nx, player_y)) {
        player_x = nx;
    }

    if (!is_wall_at_fixed(player_x, ny)) {
        player_y = ny;
    }
}

static void handle_key(int key) {
    if (key == 'q' || key == 'Q') {
        running = 0;
    }
    else if (key == 'w' || key == 'W') {
        try_move(FP_ONE / 4);
    }
    else if (key == 's' || key == 'S') {
        try_move(-(FP_ONE / 4));
    }
    else if (key == 'a' || key == 'A') {
        player_angle = clamp_angle(player_angle - 2);
    }
    else if (key == 'd' || key == 'D') {
        player_angle = clamp_angle(player_angle + 2);
    }
}

static void intro(void) {
    app_clear();
    app_write("============================================\n");
    app_write("          NAQ WOLF TEXT 3D DEMO             \n");
    app_write("============================================\n\n");

    app_write("This is a first-person ASCII raycaster.\n\n");
    app_write("Controls:\n");
    app_write("  W/S  move forward/back\n");
    app_write("  A/D  turn left/right\n");
    app_write("  Q    quit\n\n");

    app_write("Press any key to start...");
    app_read_key();
}

void app_main(void) {
    player_x = 3 * FP_ONE;
    player_y = 3 * FP_ONE;
    player_angle = 0;
    running = 1;

    app_set_raw_input(1);

    intro();

    while (running) {
        int key;

        render();

        /*
         * Wait for one key.
         * This keeps the first version simple and stable.
         */
        key = app_read_key();
        handle_key(key);

        if (key == -2) {
            break;
        }
    }

    app_set_raw_input(0);
    app_clear();
    app_write("Exited NAQ Wolf Text 3D.\n");
}