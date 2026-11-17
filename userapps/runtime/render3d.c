#include "../include/render3d.h"

/*
 * 64-step full circle.
 * Quarter-wave table: sin(i * 90/16 degrees) * R3D_FP_ONE.
 */
static const int g_r3d_sin_quarter[17] = {
    0, 400, 799, 1189, 1567, 1931, 2276, 2601, 2896,
    3167, 3406, 3612, 3784, 3923, 4017, 4077, 4096
};

static int r3d_abs_i(int v) {
    return v < 0 ? -v : v;
}

static i64 r3d_abs_i64(i64 v) {
    return v < 0 ? -v : v;
}

i32 r3d_fp_mul(i32 a, i32 b) {
    return (i32)(((i64)a * (i64)b) >> R3D_FP_SHIFT);
}

i32 r3d_fp_div(i32 a, i32 b) {
    if (b == 0) {
        return 0;
    }

    return (i32)(((i64)a * (i64)R3D_FP_ONE) / (i64)b);
}

int r3d_sin(int angle) {
    angle &= 63;

    if (angle < 16) {
        return g_r3d_sin_quarter[angle];
    }

    if (angle < 32) {
        return g_r3d_sin_quarter[32 - angle];
    }

    if (angle < 48) {
        return -g_r3d_sin_quarter[angle - 32];
    }

    return -g_r3d_sin_quarter[64 - angle];
}

int r3d_cos(int angle) {
    return r3d_sin(angle + 16);
}

r3d_vec3_t r3d_vec3_make(i32 x, i32 y, i32 z) {
    r3d_vec3_t v;

    v.x = x;
    v.y = y;
    v.z = z;

    return v;
}

r3d_vec3_t r3d_vec3_add(r3d_vec3_t a, r3d_vec3_t b) {
    return r3d_vec3_make(a.x + b.x, a.y + b.y, a.z + b.z);
}

r3d_vec3_t r3d_vec3_sub(r3d_vec3_t a, r3d_vec3_t b) {
    return r3d_vec3_make(a.x - b.x, a.y - b.y, a.z - b.z);
}

r3d_vec3_t r3d_vec3_scale(r3d_vec3_t v, i32 s) {
    return r3d_vec3_make(
        r3d_fp_mul(v.x, s),
        r3d_fp_mul(v.y, s),
        r3d_fp_mul(v.z, s)
    );
}

r3d_mat4_t r3d_mat4_identity(void) {
    r3d_mat4_t m;

    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            m.m[y][x] = 0;
        }
    }

    m.m[0][0] = R3D_FP_ONE;
    m.m[1][1] = R3D_FP_ONE;
    m.m[2][2] = R3D_FP_ONE;
    m.m[3][3] = R3D_FP_ONE;

    return m;
}

r3d_mat4_t r3d_mat4_mul(r3d_mat4_t a, r3d_mat4_t b) {
    r3d_mat4_t out;

    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            i64 acc = 0;

            for (int k = 0; k < 4; k++) {
                acc += (i64)a.m[row][k] * (i64)b.m[k][col];
            }

            out.m[row][col] = (i32)(acc >> R3D_FP_SHIFT);
        }
    }

    return out;
}

r3d_mat4_t r3d_mat4_translation(i32 x, i32 y, i32 z) {
    r3d_mat4_t m = r3d_mat4_identity();

    m.m[0][3] = x;
    m.m[1][3] = y;
    m.m[2][3] = z;

    return m;
}

r3d_mat4_t r3d_mat4_rotation_x(int angle) {
    r3d_mat4_t m = r3d_mat4_identity();

    i32 s = r3d_sin(angle);
    i32 c = r3d_cos(angle);

    m.m[1][1] = c;
    m.m[1][2] = -s;
    m.m[2][1] = s;
    m.m[2][2] = c;

    return m;
}

r3d_mat4_t r3d_mat4_rotation_y(int angle) {
    r3d_mat4_t m = r3d_mat4_identity();

    i32 s = r3d_sin(angle);
    i32 c = r3d_cos(angle);

    m.m[0][0] = c;
    m.m[0][2] = s;
    m.m[2][0] = -s;
    m.m[2][2] = c;

    return m;
}

r3d_mat4_t r3d_mat4_rotation_z(int angle) {
    r3d_mat4_t m = r3d_mat4_identity();

    i32 s = r3d_sin(angle);
    i32 c = r3d_cos(angle);

    m.m[0][0] = c;
    m.m[0][1] = -s;
    m.m[1][0] = s;
    m.m[1][1] = c;

    return m;
}

r3d_vec3_t r3d_mat4_transform_point(r3d_mat4_t m, r3d_vec3_t v) {
    r3d_vec3_t out;

    out.x =
        r3d_fp_mul(m.m[0][0], v.x) +
        r3d_fp_mul(m.m[0][1], v.y) +
        r3d_fp_mul(m.m[0][2], v.z) +
        m.m[0][3];

    out.y =
        r3d_fp_mul(m.m[1][0], v.x) +
        r3d_fp_mul(m.m[1][1], v.y) +
        r3d_fp_mul(m.m[1][2], v.z) +
        m.m[1][3];

    out.z =
        r3d_fp_mul(m.m[2][0], v.x) +
        r3d_fp_mul(m.m[2][1], v.y) +
        r3d_fp_mul(m.m[2][2], v.z) +
        m.m[2][3];

    return out;
}

void r3d_camera_reset(r3d_camera_t* cam) {
    if (!cam) {
        return;
    }

    cam->x = 0;
    cam->y = 0;
    cam->z = 0;
    cam->yaw = 0;
    cam->pitch = 0;
}

void r3d_camera_move_forward(r3d_camera_t* cam, i32 amount) {
    if (!cam) {
        return;
    }

    i32 s = r3d_sin(cam->yaw);
    i32 c = r3d_cos(cam->yaw);

    cam->x += r3d_fp_mul(s, amount);
    cam->z += r3d_fp_mul(c, amount);
}

void r3d_camera_move_right(r3d_camera_t* cam, i32 amount) {
    if (!cam) {
        return;
    }

    i32 s = r3d_sin(cam->yaw);
    i32 c = r3d_cos(cam->yaw);

    cam->x += r3d_fp_mul(c, amount);
    cam->z -= r3d_fp_mul(s, amount);
}

void r3d_camera_move_up(r3d_camera_t* cam, i32 amount) {
    if (!cam) {
        return;
    }

    cam->y += amount;
}

int r3d_camera_handle_key(r3d_camera_t* cam, u32 key, i32 move_step) {
    if (!cam) {
        return 0;
    }

    if (move_step == 0) {
        move_step = R3D_FP_ONE / 4;
    }

    if (key == 'w' || key == 'W') {
        r3d_camera_move_forward(cam, move_step);
    } else if (key == 's' || key == 'S') {
        r3d_camera_move_forward(cam, -move_step);
    } else if (key == 'a' || key == 'A') {
        r3d_camera_move_right(cam, -move_step);
    } else if (key == 'd' || key == 'D') {
        r3d_camera_move_right(cam, move_step);
    } else if (key == 'r' || key == 'R') {
        r3d_camera_move_up(cam, move_step);
    } else if (key == 'f' || key == 'F') {
        r3d_camera_move_up(cam, -move_step);
    } else if (key == 'j' || key == 'J') {
        cam->yaw = (cam->yaw - 1) & 63;
    } else if (key == 'l' || key == 'L') {
        cam->yaw = (cam->yaw + 1) & 63;
    } else if (key == 'i' || key == 'I') {
        cam->pitch = (cam->pitch + 1) & 63;
    } else if (key == 'k' || key == 'K') {
        cam->pitch = (cam->pitch - 1) & 63;
    } else if (key == ' ') {
        r3d_camera_reset(cam);
    } else {
        return 0;
    }

    return 1;
}

void r3d_context_init(
    r3d_context_t* ctx,
    u32* pixels,
    i32* depth,
    u32 width,
    u32 height
) {
    if (!ctx) {
        return;
    }

    ctx->pixels = pixels;
    ctx->depth = depth;
    ctx->width = width;
    ctx->height = height;
    ctx->stride = width;

    ctx->focal = (i32)((height * 3u) / 4u);

    if (ctx->focal <= 0) {
        ctx->focal = 120;
    }

    ctx->near_z = R3D_FP_ONE / 3;
    ctx->far_z = 128 * R3D_FP_ONE;
    ctx->flags = R3D_FLAG_WIREFRAME;
    ctx->wire_color = 0x00FFFFFFu;
}

void r3d_context_set_flags(r3d_context_t* ctx, u32 flags) {
    if (!ctx) {
        return;
    }

    ctx->flags = flags;
}

void r3d_context_set_wire_color(r3d_context_t* ctx, u32 color) {
    if (!ctx) {
        return;
    }

    ctx->wire_color = color;
}

void r3d_clear(r3d_context_t* ctx, u32 top_color, u32 bottom_color) {
    if (!ctx || !ctx->pixels || ctx->width == 0 || ctx->height == 0) {
        return;
    }

    for (u32 y = 0; y < ctx->height; y++) {
        u32 color = y < ctx->height / 2u ? top_color : bottom_color;

        for (u32 x = 0; x < ctx->width; x++) {
            ctx->pixels[y * ctx->stride + x] = color;
        }
    }
}

void r3d_clear_depth(r3d_context_t* ctx) {
    if (!ctx || !ctx->depth) {
        return;
    }

    for (u32 i = 0; i < ctx->width * ctx->height; i++) {
        ctx->depth[i] = ctx->far_z;
    }
}

static r3d_vec3_t r3d_camera_to_view(const r3d_camera_t* cam, r3d_vec3_t p) {
    if (!cam) {
        return p;
    }

    p.x -= cam->x;
    p.y -= cam->y;
    p.z -= cam->z;

    /*
     * Inverse camera yaw.
     */
    {
        i32 s = r3d_sin(-cam->yaw);
        i32 c = r3d_cos(-cam->yaw);

        i32 x = r3d_fp_mul(p.x, c) + r3d_fp_mul(p.z, s);
        i32 z = -r3d_fp_mul(p.x, s) + r3d_fp_mul(p.z, c);

        p.x = x;
        p.z = z;
    }

    /*
     * Inverse camera pitch.
     */
    {
        i32 s = r3d_sin(-cam->pitch);
        i32 c = r3d_cos(-cam->pitch);

        i32 y = r3d_fp_mul(p.y, c) - r3d_fp_mul(p.z, s);
        i32 z = r3d_fp_mul(p.y, s) + r3d_fp_mul(p.z, c);

        p.y = y;
        p.z = z;
    }

    return p;
}

int r3d_project_vertex(
    const r3d_context_t* ctx,
    r3d_vec3_t view,
    r3d_vertex2_t* out
) {
    if (!ctx || !out) {
        return 0;
    }

    if (view.z <= ctx->near_z || view.z >= ctx->far_z) {
        return 0;
    }

    out->x = (i32)(ctx->width / 2u) +
        (i32)(((i64)view.x * (i64)ctx->focal) / (i64)view.z);

    out->y = (i32)(ctx->height / 2u) -
        (i32)(((i64)view.y * (i64)ctx->focal) / (i64)view.z);

    out->z = view.z;

    return 1;
}

static i64 r3d_edge(i32 ax, i32 ay, i32 bx, i32 by, i32 cx, i32 cy) {
    return (i64)(cx - ax) * (i64)(by - ay) -
           (i64)(cy - ay) * (i64)(bx - ax);
}

static void r3d_put_depth_pixel(
    r3d_context_t* ctx,
    i32 x,
    i32 y,
    i32 z,
    u32 color
) {
    if (!ctx || !ctx->pixels || !ctx->depth) {
        return;
    }

    if (x < 0 || y < 0) {
        return;
    }

    if ((u32)x >= ctx->width || (u32)y >= ctx->height) {
        return;
    }

    if (z <= 0) {
        return;
    }

    u32 index = (u32)y * ctx->stride + (u32)x;

    if (z < ctx->depth[index]) {
        ctx->depth[index] = z;
        ctx->pixels[index] = color;
    }
}

void r3d_draw_triangle(
    r3d_context_t* ctx,
    r3d_vertex2_t a,
    r3d_vertex2_t b,
    r3d_vertex2_t c,
    u32 color
) {
    if (!ctx || !ctx->pixels || !ctx->depth) {
        return;
    }

    i64 area = r3d_edge(a.x, a.y, b.x, b.y, c.x, c.y);

    if (area == 0) {
        return;
    }

    if (area < 0) {
        r3d_vertex2_t tmp = b;
        b = c;
        c = tmp;
        area = -area;
    }

    i32 min_x = a.x;
    i32 max_x = a.x;
    i32 min_y = a.y;
    i32 max_y = a.y;

    if (b.x < min_x) min_x = b.x;
    if (c.x < min_x) min_x = c.x;
    if (b.x > max_x) max_x = b.x;
    if (c.x > max_x) max_x = c.x;

    if (b.y < min_y) min_y = b.y;
    if (c.y < min_y) min_y = c.y;
    if (b.y > max_y) max_y = b.y;
    if (c.y > max_y) max_y = c.y;

    if (min_x < 0) min_x = 0;
    if (min_y < 0) min_y = 0;

    if (max_x >= (i32)ctx->width) max_x = (i32)ctx->width - 1;
    if (max_y >= (i32)ctx->height) max_y = (i32)ctx->height - 1;

    for (i32 y = min_y; y <= max_y; y++) {
        for (i32 x = min_x; x <= max_x; x++) {
            i64 w0 = r3d_edge(b.x, b.y, c.x, c.y, x, y);
            i64 w1 = r3d_edge(c.x, c.y, a.x, a.y, x, y);
            i64 w2 = r3d_edge(a.x, a.y, b.x, b.y, x, y);

            if (w0 < 0 || w1 < 0 || w2 < 0) {
                continue;
            }

            i32 z = (i32)((w0 * (i64)a.z +
                           w1 * (i64)b.z +
                           w2 * (i64)c.z) / area);

            r3d_put_depth_pixel(ctx, x, y, z, color);
        }
    }
}

static void r3d_draw_wire_line(
    r3d_context_t* ctx,
    int x0,
    int y0,
    int x1,
    int y1,
    u32 color
) {
    if (!ctx || !ctx->pixels) {
        return;
    }

    int dx = r3d_abs_i(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;

    int dy = -r3d_abs_i(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;

    int err = dx + dy;

    for (;;) {
        if (x0 >= 0 && y0 >= 0 &&
            x0 < (int)ctx->width &&
            y0 < (int)ctx->height) {
            ctx->pixels[(u32)y0 * ctx->stride + (u32)x0] = color;
        }

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

static u32 r3d_shade_color(u32 base, int brightness) {
    if (brightness < 32) {
        brightness = 32;
    }

    if (brightness > 255) {
        brightness = 255;
    }

    u32 r = (base >> 16) & 0xFFu;
    u32 g = (base >> 8) & 0xFFu;
    u32 b = base & 0xFFu;

    r = (r * (u32)brightness) / 255u;
    g = (g * (u32)brightness) / 255u;
    b = (b * (u32)brightness) / 255u;

    return (r << 16) | (g << 8) | b;
}

static int r3d_triangle_brightness(r3d_vec3_t a, r3d_vec3_t b, r3d_vec3_t c) {
    r3d_vec3_t ab = r3d_vec3_sub(b, a);
    r3d_vec3_t ac = r3d_vec3_sub(c, a);

    i64 nx = (i64)ab.y * (i64)ac.z - (i64)ab.z * (i64)ac.y;
    i64 ny = (i64)ab.z * (i64)ac.x - (i64)ab.x * (i64)ac.z;
    i64 nz = (i64)ab.x * (i64)ac.y - (i64)ab.y * (i64)ac.x;

    i64 sum = r3d_abs_i64(nx) + r3d_abs_i64(ny) + r3d_abs_i64(nz);

    if (sum == 0) {
        return 96;
    }

    /*
     * Camera-facing light plus a small side-fill.
     */
    i64 camera_facing = r3d_abs_i64(nz);

    int brightness = 72 + (int)((camera_facing * 138) / sum);
    brightness += (int)((r3d_abs_i64(nx) * 28) / sum);

    if (brightness > 240) {
        brightness = 240;
    }

    return brightness;
}

static int r3d_is_backface(r3d_vertex2_t a, r3d_vertex2_t b, r3d_vertex2_t c) {
    i64 area = r3d_edge(a.x, a.y, b.x, b.y, c.x, c.y);

    return area <= 0;
}

void r3d_draw_mesh(
    r3d_context_t* ctx,
    const r3d_mesh_t* mesh,
    r3d_mat4_t model,
    const r3d_camera_t* cam
) {
    r3d_vec3_t world[R3D_MAX_MESH_VERTICES];
    r3d_vec3_t view[R3D_MAX_MESH_VERTICES];
    r3d_vertex2_t projected[R3D_MAX_MESH_VERTICES];
    int visible[R3D_MAX_MESH_VERTICES];

    if (!ctx || !mesh || !mesh->vertices || !mesh->tris) {
        return;
    }

    if (mesh->vertex_count == 0 ||
        mesh->vertex_count > R3D_MAX_MESH_VERTICES) {
        return;
    }

    for (u32 i = 0; i < mesh->vertex_count; i++) {
        world[i] = r3d_mat4_transform_point(model, mesh->vertices[i]);
        view[i] = r3d_camera_to_view(cam, world[i]);
        visible[i] = r3d_project_vertex(ctx, view[i], &projected[i]);
    }

    for (u32 tri = 0; tri < mesh->tri_count; tri++) {
        u32 ia = mesh->tris[tri].a;
        u32 ib = mesh->tris[tri].b;
        u32 ic = mesh->tris[tri].c;

        if (ia >= mesh->vertex_count ||
            ib >= mesh->vertex_count ||
            ic >= mesh->vertex_count) {
            continue;
        }

        if (!visible[ia] || !visible[ib] || !visible[ic]) {
            continue;
        }

        if ((ctx->flags & R3D_FLAG_BACKFACE_CULL) &&
            r3d_is_backface(projected[ia], projected[ib], projected[ic])) {
            continue;
        }

        u32 base = mesh->tri_colors ? mesh->tri_colors[tri] : 0x0000D8FFu;
        int brightness = r3d_triangle_brightness(view[ia], view[ib], view[ic]);
        u32 color = r3d_shade_color(base, brightness);

        r3d_draw_triangle(
            ctx,
            projected[ia],
            projected[ib],
            projected[ic],
            color
        );
    }

    if (ctx->flags & R3D_FLAG_WIREFRAME) {
        for (u32 tri = 0; tri < mesh->tri_count; tri++) {
            u32 ia = mesh->tris[tri].a;
            u32 ib = mesh->tris[tri].b;
            u32 ic = mesh->tris[tri].c;

            if (ia >= mesh->vertex_count ||
                ib >= mesh->vertex_count ||
                ic >= mesh->vertex_count) {
                continue;
            }

            if (!visible[ia] || !visible[ib] || !visible[ic]) {
                continue;
            }

            r3d_draw_wire_line(
                ctx,
                projected[ia].x,
                projected[ia].y,
                projected[ib].x,
                projected[ib].y,
                ctx->wire_color
            );

            r3d_draw_wire_line(
                ctx,
                projected[ib].x,
                projected[ib].y,
                projected[ic].x,
                projected[ic].y,
                ctx->wire_color
            );

            r3d_draw_wire_line(
                ctx,
                projected[ic].x,
                projected[ic].y,
                projected[ia].x,
                projected[ia].y,
                ctx->wire_color
            );
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Built-in cube mesh                                                          */
/* -------------------------------------------------------------------------- */

static const r3d_vec3_t g_r3d_cube_vertices[8] = {
    { -R3D_FP_ONE, -R3D_FP_ONE, -R3D_FP_ONE },
    {  R3D_FP_ONE, -R3D_FP_ONE, -R3D_FP_ONE },
    {  R3D_FP_ONE,  R3D_FP_ONE, -R3D_FP_ONE },
    { -R3D_FP_ONE,  R3D_FP_ONE, -R3D_FP_ONE },

    { -R3D_FP_ONE, -R3D_FP_ONE,  R3D_FP_ONE },
    {  R3D_FP_ONE, -R3D_FP_ONE,  R3D_FP_ONE },
    {  R3D_FP_ONE,  R3D_FP_ONE,  R3D_FP_ONE },
    { -R3D_FP_ONE,  R3D_FP_ONE,  R3D_FP_ONE }
};

static const r3d_tri_t g_r3d_cube_tris[12] = {
    { 0, 3, 2 }, { 0, 2, 1 },
    { 4, 5, 6 }, { 4, 6, 7 },

    { 0, 4, 7 }, { 0, 7, 3 },
    { 1, 2, 6 }, { 1, 6, 5 },

    { 3, 7, 6 }, { 3, 6, 2 },
    { 0, 1, 5 }, { 0, 5, 4 }
};

static const u32 g_r3d_cube_colors[12] = {
    0x0000D8FFu, 0x0000B8E8u,
    0x0000FF9Au, 0x0000D878u,
    0x00B8FF4Au, 0x0095D83Au,
    0x00FFE35Au, 0x00D8B83Au,
    0x00FF7A7Au, 0x00D85858u,
    0x00BDA0FFu, 0x009078D8u
};

r3d_mesh_t r3d_cube_mesh(void) {
    r3d_mesh_t mesh;

    mesh.vertices = g_r3d_cube_vertices;
    mesh.vertex_count = 8;
    mesh.tris = g_r3d_cube_tris;
    mesh.tri_count = 12;
    mesh.tri_colors = g_r3d_cube_colors;

    return mesh;
}
