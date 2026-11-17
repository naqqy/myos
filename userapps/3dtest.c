#include "include/naqos.h"
#include "include/render3d.h"

#define RENDER3D_W 320u
#define RENDER3D_H 200u

#define COLOR_BG_TOP     0x000B1420u
#define COLOR_BG_BOTTOM  0x0013232Cu

static u32 g_pixels_a[RENDER3D_W * RENDER3D_H];
static u32 g_pixels_b[RENDER3D_W * RENDER3D_H];
static i32 g_depth[RENDER3D_W * RENDER3D_H];

static r3d_mat4_t make_cube_model(int ax, int ay, int az) {
    r3d_mat4_t rx = r3d_mat4_rotation_x(ax);
    r3d_mat4_t ry = r3d_mat4_rotation_y(ay);
    r3d_mat4_t rz = r3d_mat4_rotation_z(az);
    r3d_mat4_t t = r3d_mat4_translation(0, 0, 6 * R3D_FP_ONE);

    return r3d_mat4_mul(t, r3d_mat4_mul(ry, r3d_mat4_mul(rx, rz)));
}

static void handle_events(
    int win,
    r3d_camera_t* cam,
    int* running,
    int* auto_rotate,
    r3d_context_t* ctx
) {
    naq_win_event_t ev;

    while (win_get_event(win, &ev) == 1) {
        if (ev.type == NAQ_WIN_EVENT_CLOSE) {
            *running = 0;
            continue;
        }

        if (ev.type != NAQ_WIN_EVENT_KEY) {
            continue;
        }

        if (ev.key == 'q' || ev.key == 'Q') {
            *running = 0;
        } else if (ev.key == 'p' || ev.key == 'P') {
            *auto_rotate = !*auto_rotate;
        } else if (ev.key == 'z' || ev.key == 'Z') {
            ctx->flags ^= R3D_FLAG_WIREFRAME;
        } else if (ev.key == 'x' || ev.key == 'X') {
            ctx->flags ^= R3D_FLAG_BACKFACE_CULL;
        } else {
            r3d_camera_handle_key(cam, ev.key, R3D_FP_ONE / 4);
        }
    }
}

int main(void) {
    int win = win_create(120, 90, RENDER3D_W, RENDER3D_H, "Renderer3D");

    if (win < 0) {
        puts("render3d: win_create failed\n");
        return 1;
    }

    puts("Renderer3D library demo\n");
    puts("  WASD move, R/F up/down, J/L yaw, I/K pitch\n");
    puts("  P pause rotation, Z wire, X cull, Space reset, Q quit\n");

    r3d_context_t ctx;
    r3d_context_init(&ctx, g_pixels_a, g_depth, RENDER3D_W, RENDER3D_H);
    r3d_context_set_wire_color(&ctx, 0x00FFFFFFu);

    r3d_camera_t cam;
    r3d_camera_reset(&cam);

    r3d_mesh_t cube = r3d_cube_mesh();

    naq_frame_timer_t timer;
    frame_timer_init(&timer, 30);

    int running = 1;
    int auto_rotate = 1;
    u32 current_buffer = 0;

    int ax = 6;
    int ay = 0;
    int az = 0;

    while (running) {
        u32* pixels = current_buffer ? g_pixels_b : g_pixels_a;

        frame_timer_begin(&timer);

        ctx.pixels = pixels;

        handle_events(win, &cam, &running, &auto_rotate, &ctx);

        r3d_clear(&ctx, COLOR_BG_TOP, COLOR_BG_BOTTOM);
        r3d_clear_depth(&ctx);

        r3d_draw_mesh(&ctx, &cube, make_cube_model(ax, ay, az), &cam);

        win_present(win, pixels, RENDER3D_W, RENDER3D_H);

        if (auto_rotate) {
            ax = (ax + 1) & 63;
            ay = (ay + 2) & 63;
            az = (az + 1) & 63;
        }

        current_buffer ^= 1u;

        frame_timer_end(&timer);
    }

    win_destroy(win);
    puts("Renderer3D closed\n");

    return 0;
}
