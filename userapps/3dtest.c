#include "include/naqos.h"
#include "include/render3d.h"

/*
 * 3DTEST.ELF
 *
 * Model viewer for the render3d library.
 *
 * Shows models loaded from /assets/game (built from OBJ files by
 * tools/make_nmodel.py) plus the built-in cube:
 *
 *   crate.nm + crate.ni   textured, perspective-correct
 *   torus.nm              flat shaded with per-triangle colours
 *   built-in cube         always available, used if no asset loads
 *
 * Controls:
 *   M            next model
 *   WASD R/F     move camera, J/L yaw, I/K pitch, Space reset
 *   P            pause rotation
 *   Z            wireframe overlay
 *   X            back-face culling
 *   Q            quit
 *
 * The window can be resized or maximised: the scene is re-rendered at the
 * new size, up to RENDER3D_MAX_W x RENDER3D_MAX_H. Larger windows get that
 * frame stretched by the desktop.
 */

#define RENDER3D_W 320u
#define RENDER3D_H 200u

#define RENDER3D_MAX_W 640u
#define RENDER3D_MAX_H 400u

#define COLOR_BG_TOP     0x000B1420u
#define COLOR_BG_BOTTOM  0x0013232Cu

#define MODEL_FILE_MAX   (256u * 1024u)
#define TEXTURE_FILE_MAX (300u * 1024u)

#define MAX_MODELS 3

static u32 g_pixels_a[RENDER3D_MAX_W * RENDER3D_MAX_H];
static u32 g_pixels_b[RENDER3D_MAX_W * RENDER3D_MAX_H];
static i32 g_depth[RENDER3D_MAX_W * RENDER3D_MAX_H];

/*
 * Asset files are parsed in place, so each one keeps its own buffer.
 * u32 arrays give the 4-byte alignment the parsers require.
 */
static u32 g_crate_file[MODEL_FILE_MAX / 4];
static u32 g_torus_file[MODEL_FILE_MAX / 4];
static u32 g_crate_texture_file[TEXTURE_FILE_MAX / 4];

static r3d_texture_t g_crate_texture;

typedef struct viewer_model {
    const char* name;
    r3d_mesh_t mesh;
} viewer_model_t;

static viewer_model_t g_models[MAX_MODELS];
static u32 g_model_count = 0;

static void add_model(const char* name, r3d_mesh_t mesh) {
    if (g_model_count >= MAX_MODELS) {
        return;
    }

    g_models[g_model_count].name = name;
    g_models[g_model_count].mesh = mesh;
    g_model_count++;
}

static void load_models(void) {
    r3d_mesh_t mesh;

    if (r3d_model_load(
            "/assets/game/crate.nm",
            g_crate_file,
            sizeof(g_crate_file),
            &mesh)) {
        if (r3d_texture_load(
                "/assets/game/crate.ni",
                g_crate_texture_file,
                sizeof(g_crate_texture_file),
                &g_crate_texture)) {
            mesh.texture = &g_crate_texture;
        } else {
            puts("3dtest: crate.ni not loaded, crate will be flat shaded\n");
        }

        add_model("Crate (textured)", mesh);
    } else {
        puts("3dtest: /assets/game/crate.nm not loaded\n");
    }

    if (r3d_model_load(
            "/assets/game/torus.nm",
            g_torus_file,
            sizeof(g_torus_file),
            &mesh)) {
        add_model("Torus", mesh);
    } else {
        puts("3dtest: /assets/game/torus.nm not loaded\n");
    }

    add_model("Built-in cube", r3d_cube_mesh());
}

static r3d_mat4_t make_model_matrix(int ax, int ay, int az) {
    r3d_mat4_t rx = r3d_mat4_rotation_x(ax);
    r3d_mat4_t ry = r3d_mat4_rotation_y(ay);
    r3d_mat4_t rz = r3d_mat4_rotation_z(az);
    r3d_mat4_t t = r3d_mat4_translation(0, 0, 4 * R3D_FP_ONE);

    return r3d_mat4_mul(t, r3d_mat4_mul(ry, r3d_mat4_mul(rx, rz)));
}

static void show_model_title(int win, u32 index) {
    win_set_title(win, g_models[index].name);

    puts("3dtest: showing ");
    puts(g_models[index].name);
    puts("\n");
}

static void handle_events(
    int win,
    r3d_camera_t* cam,
    int* running,
    int* auto_rotate,
    u32* model_index,
    r3d_context_t* ctx
) {
    naq_win_event_t ev;

    while (win_get_event(win, &ev) == 1) {
        if (ev.type == NAQ_WIN_EVENT_CLOSE) {
            *running = 0;
            continue;
        }

        if (ev.type == NAQ_WIN_EVENT_RESIZE) {
            /*
             * Render at the window's new size, within the buffers we have.
             * Re-initialising the context recomputes the projection; keep
             * the user's render flags.
             */
            u32 w = (u32)ev.w;
            u32 h = (u32)ev.h;
            u32 flags = ctx->flags;

            if (w > RENDER3D_MAX_W) {
                w = RENDER3D_MAX_W;
            }

            if (h > RENDER3D_MAX_H) {
                h = RENDER3D_MAX_H;
            }

            if (w >= 32 && h >= 32) {
                r3d_context_init(ctx, ctx->pixels, g_depth, w, h);
                r3d_context_set_flags(ctx, flags);
                r3d_context_set_wire_color(ctx, 0x00FFFFFFu);
            }

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
        } else if (ev.key == 'm' || ev.key == 'M') {
            *model_index = (*model_index + 1u) % g_model_count;
            show_model_title(win, *model_index);
        } else {
            r3d_camera_handle_key(cam, ev.key, R3D_FP_ONE / 4);
        }
    }
}

int main(void) {
    int win = win_create(120, 90, RENDER3D_W, RENDER3D_H, "Renderer3D");

    if (win < 0) {
        puts("3dtest: win_create failed\n");
        return 1;
    }

    puts("Renderer3D model viewer\n");
    puts("  M next model, WASD move, R/F up/down, J/L yaw, I/K pitch\n");
    puts("  P pause rotation, Z wire, X cull, Space reset, Q quit\n");

    load_models();

    r3d_context_t ctx;
    r3d_context_init(&ctx, g_pixels_a, g_depth, RENDER3D_W, RENDER3D_H);
    r3d_context_set_flags(&ctx, R3D_FLAG_BACKFACE_CULL);
    r3d_context_set_wire_color(&ctx, 0x00FFFFFFu);

    r3d_camera_t cam;
    r3d_camera_reset(&cam);

    naq_frame_timer_t timer;
    frame_timer_init(&timer, 30);

    int running = 1;
    int auto_rotate = 1;
    u32 current_buffer = 0;
    u32 model_index = 0;

    int ax = 10;
    int ay = 0;
    int az = 0;

    /*
     * Rotation advances one 64th of a turn every few frames so the motion
     * is slow enough to see the shading.
     */
    u32 frame = 0;

    show_model_title(win, model_index);

    while (running) {
        u32* pixels = current_buffer ? g_pixels_b : g_pixels_a;

        frame_timer_begin(&timer);

        ctx.pixels = pixels;

        handle_events(win, &cam, &running, &auto_rotate, &model_index, &ctx);

        r3d_clear(&ctx, COLOR_BG_TOP, COLOR_BG_BOTTOM);
        r3d_clear_depth(&ctx);

        r3d_draw_mesh(
            &ctx,
            &g_models[model_index].mesh,
            make_model_matrix(ax, ay, az),
            &cam
        );

        win_present(win, pixels, ctx.width, ctx.height);

        if (auto_rotate && (frame % 3u) == 0) {
            ay = (ay + 1) & 63;

            if ((frame % 12u) == 0) {
                ax = (ax + 1) & 63;
            }
        }

        frame++;
        current_buffer ^= 1u;

        frame_timer_end(&timer);
    }

    win_destroy(win);
    puts("Renderer3D closed\n");

    return 0;
}
