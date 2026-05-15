#include "include/naqos.h"

#define CUBE_W 320u
#define CUBE_H 200u

#define COLOR_BG      0x00081420u
#define COLOR_GRID    0x00102035u
#define COLOR_EDGE_A  0x0000E0FFu
#define COLOR_EDGE_B  0x0000FF90u
#define COLOR_VERTEX  0x00FFFFFFu
#define COLOR_CENTER  0x00FF4060u

#define FIX_SHIFT 10
#define FIX_ONE   (1 << FIX_SHIFT)

/*
 * 64-step full circle.
 * Quarter-wave table: sin(i * 90/16 degrees) * 1024.
 */
static const int g_sin_quarter[17] = {
    0, 100, 200, 297, 392, 483, 569, 650, 724,
    792, 851, 903, 946, 981, 1004, 1019, 1024
};

static u32 g_pixels_a[CUBE_W * CUBE_H];
static u32 g_pixels_b[CUBE_W * CUBE_H];

typedef struct vec3i {
    int x;
    int y;
    int z;
} vec3i_t;

typedef struct vec2i {
    int x;
    int y;
} vec2i_t;

static const vec3i_t g_cube_vertices[8] = {
    { -64, -64, -64 },
    {  64, -64, -64 },
    {  64,  64, -64 },
    { -64,  64, -64 },

    { -64, -64,  64 },
    {  64, -64,  64 },
    {  64,  64,  64 },
    { -64,  64,  64 }
};

static const u8 g_cube_edges[12][2] = {
    { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 },
    { 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 },
    { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 }
};

static int abs_i(int v) {
    return v < 0 ? -v : v;
}

static int sin_fixed(int angle) {
    angle &= 63;

    if (angle < 16) {
        return g_sin_quarter[angle];
    }

    if (angle < 32) {
        return g_sin_quarter[32 - angle];
    }

    if (angle < 48) {
        return -g_sin_quarter[angle - 32];
    }

    return -g_sin_quarter[64 - angle];
}

static int cos_fixed(int angle) {
    return sin_fixed(angle + 16);
}

static int fixed_mul(int a, int b) {
    return (a * b) >> FIX_SHIFT;
}

static vec3i_t rotate_vertex(vec3i_t v, int ax, int ay, int az) {
    int sx = sin_fixed(ax);
    int cx = cos_fixed(ax);

    int sy = sin_fixed(ay);
    int cy = cos_fixed(ay);

    int sz = sin_fixed(az);
    int cz = cos_fixed(az);

    /*
     * Rotate around X.
     */
    int x1 = v.x;
    int y1 = fixed_mul(v.y, cx) - fixed_mul(v.z, sx);
    int z1 = fixed_mul(v.y, sx) + fixed_mul(v.z, cx);

    /*
     * Rotate around Y.
     */
    int x2 = fixed_mul(x1, cy) + fixed_mul(z1, sy);
    int y2 = y1;
    int z2 = -fixed_mul(x1, sy) + fixed_mul(z1, cy);

    /*
     * Rotate around Z.
     */
    int x3 = fixed_mul(x2, cz) - fixed_mul(y2, sz);
    int y3 = fixed_mul(x2, sz) + fixed_mul(y2, cz);
    int z3 = z2;

    vec3i_t out;
    out.x = x3;
    out.y = y3;
    out.z = z3;
    return out;
}

static vec2i_t project_vertex(vec3i_t v) {
    /*
     * Simple perspective projection.
     */
    int camera_dist = 280;
    int scale = 180;

    int z = v.z + camera_dist;

    if (z < 32) {
        z = 32;
    }

    vec2i_t out;
    out.x = (int)(CUBE_W / 2) + ((v.x * scale) / z);
    out.y = (int)(CUBE_H / 2) - ((v.y * scale) / z);
    return out;
}

static void draw_line(
    naq_gfx_buffer_t* buf,
    int x0,
    int y0,
    int x1,
    int y1,
    u32 color
) {
    int dx = abs_i(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;

    int dy = -abs_i(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;

    int err = dx + dy;

    for (;;) {
        gfx_buffer_put_pixel(buf, x0, y0, color);

        if (x0 == x1 && y0 == y1) {
            break;
        }

        int e2 = err * 2;

        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }

        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static void draw_background(naq_gfx_buffer_t* buf) {
    gfx_buffer_clear(buf, COLOR_BG);

    /*
     * Subtle grid.
     */
    for (u32 x = 0; x < CUBE_W; x += 16) {
        gfx_buffer_draw_rect(buf, (i32)x, 0, 1, CUBE_H, COLOR_GRID);
    }

    for (u32 y = 0; y < CUBE_H; y += 16) {
        gfx_buffer_draw_rect(buf, 0, (i32)y, CUBE_W, 1, COLOR_GRID);
    }

    /*
     * Tiny center marker.
     */
    gfx_buffer_draw_rect(
        buf,
        (i32)(CUBE_W / 2) - 2,
        (i32)(CUBE_H / 2) - 2,
        4,
        4,
        COLOR_CENTER
    );
}

static void draw_cube(
    naq_gfx_buffer_t* buf,
    int ax,
    int ay,
    int az
) {
    vec2i_t projected[8];

    for (int i = 0; i < 8; i++) {
        vec3i_t rv = rotate_vertex(g_cube_vertices[i], ax, ay, az);
        projected[i] = project_vertex(rv);
    }

    for (int i = 0; i < 12; i++) {
        int a = g_cube_edges[i][0];
        int b = g_cube_edges[i][1];

        u32 color = i < 4 ? COLOR_EDGE_A : COLOR_EDGE_B;

        draw_line(
            buf,
            projected[a].x,
            projected[a].y,
            projected[b].x,
            projected[b].y,
            color
        );
    }

    for (int i = 0; i < 8; i++) {
        gfx_buffer_draw_rect(
            buf,
            projected[i].x - 2,
            projected[i].y - 2,
            5,
            5,
            COLOR_VERTEX
        );
    }
}

static void print_u64(u64 value) {
    char buf[32];
    int i = 0;

    if (value == 0) {
        putchar('0');
        return;
    }

    while (value > 0 && i < (int)sizeof(buf)) {
        buf[i++] = (char)('0' + (value % 10));
        value /= 10;
    }

    while (i > 0) {
        putchar(buf[--i]);
    }
}

static void handle_window_events(int win, int* running) {
    naq_win_event_t ev;

    for (;;) {
        int r = win_get_event(win, &ev);

        if (r <= 0) {
            break;
        }

        if (ev.type == NAQ_WIN_EVENT_CLOSE) {
            *running = 0;
            break;
        }

        if (ev.type == NAQ_WIN_EVENT_KEY) {
            if (ev.key == 'q' || ev.key == 'Q') {
                *running = 0;
                break;
            }
        }
    }
}

int main(void) {
    naq_frame_timer_t timer;

    naq_gfx_buffer_t buffers[2];
    u32 current_buffer = 0;

    int ax = 0;
    int ay = 0;
    int az = 0;

    int running = 1;

    /*
     * Windowed version:
     *
     * Do not call gfx_set_mode(GFX_MODE_GRAPHICS). That old path asks the
     * kernel to show the process framebuffer directly. For GUI desktop apps,
     * create a window and present pixels through SYS_WIN_PRESENT instead.
     */
    int win = win_create(120, 90, CUBE_W, CUBE_H, "3D Cube");

    if (win < 0) {
        puts("cube3d: win_create failed\n");
        return 1;
    }

    gfx_buffer_init(&buffers[0], g_pixels_a, CUBE_W, CUBE_H);
    gfx_buffer_init(&buffers[1], g_pixels_b, CUBE_W, CUBE_H);

    frame_timer_init(&timer, 30);

    while (running) {
        naq_gfx_buffer_t* buf = &buffers[current_buffer];

        frame_timer_begin(&timer);

        handle_window_events(win, &running);

        draw_background(buf);
        draw_cube(buf, ax, ay, az);

        /*
         * Present into this GUI window. The compositor owns the real screen.
         */
        win_present(win, buf->pixels, CUBE_W, CUBE_H);

        current_buffer ^= 1u;

        ax = (ax + 1) & 63;
        ay = (ay + 2) & 63;
        az = (az + 1) & 63;

        /*
         * Log timing occasionally to the app terminal.
         */
        if ((timer.frame_count % 120ull) == 0ull && timer.frame_count != 0ull) {
            puts("cube window frame=");
            print_u64(timer.frame_count);
            puts(" delta_ms=");
            print_u64(frame_timer_delta_ms(&timer));
            puts("\n");
        }

        frame_timer_end(&timer);
    }

    win_destroy(win);

    puts("3D cube window demo done\n");

    return 0;
}
