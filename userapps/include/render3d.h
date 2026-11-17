#ifndef R3D_H
#define R3D_H

#include "naqos.h"

/*
 * NAQ OS user-space software 3D renderer library.
 *
 * Fixed point format:
 *   R3D_FP_ONE == 1.0
 *
 * Intended for freestanding user apps:
 *   - no libm
 *   - no malloc required
 *   - caller owns color/depth buffers
 */

#define R3D_FP_SHIFT 12
#define R3D_FP_ONE   (1 << R3D_FP_SHIFT)

#define R3D_MAX_MESH_VERTICES 128u

#define R3D_FLAG_WIREFRAME      0x00000001u
#define R3D_FLAG_BACKFACE_CULL  0x00000002u

typedef struct r3d_vec2 {
    i32 x;
    i32 y;
} r3d_vec2_t;

typedef struct r3d_vec3 {
    i32 x;
    i32 y;
    i32 z;
} r3d_vec3_t;

typedef struct r3d_mat4 {
    i32 m[4][4];
} r3d_mat4_t;

typedef struct r3d_vertex2 {
    i32 x;
    i32 y;
    i32 z;
} r3d_vertex2_t;

typedef struct r3d_tri {
    u8 a;
    u8 b;
    u8 c;
} r3d_tri_t;

typedef struct r3d_mesh {
    const r3d_vec3_t* vertices;
    u32 vertex_count;

    const r3d_tri_t* tris;
    u32 tri_count;

    /*
     * Optional per-triangle colors.
     * If null, the renderer uses a default aqua color.
     */
    const u32* tri_colors;
} r3d_mesh_t;

typedef struct r3d_camera {
    i32 x;
    i32 y;
    i32 z;

    /*
     * 64 steps = full circle.
     */
    int yaw;
    int pitch;
} r3d_camera_t;

typedef struct r3d_context {
    u32* pixels;
    i32* depth;

    u32 width;
    u32 height;
    u32 stride;

    i32 focal;
    i32 near_z;
    i32 far_z;

    u32 flags;
    u32 wire_color;
} r3d_context_t;

/*
 * Fixed-point helpers.
 */
i32 r3d_fp_mul(i32 a, i32 b);
i32 r3d_fp_div(i32 a, i32 b);
int r3d_sin(int angle);
int r3d_cos(int angle);

/*
 * Vector helpers.
 */
r3d_vec3_t r3d_vec3_make(i32 x, i32 y, i32 z);
r3d_vec3_t r3d_vec3_add(r3d_vec3_t a, r3d_vec3_t b);
r3d_vec3_t r3d_vec3_sub(r3d_vec3_t a, r3d_vec3_t b);
r3d_vec3_t r3d_vec3_scale(r3d_vec3_t v, i32 s);

/*
 * Matrix helpers.
 */
r3d_mat4_t r3d_mat4_identity(void);
r3d_mat4_t r3d_mat4_mul(r3d_mat4_t a, r3d_mat4_t b);
r3d_mat4_t r3d_mat4_translation(i32 x, i32 y, i32 z);
r3d_mat4_t r3d_mat4_rotation_x(int angle);
r3d_mat4_t r3d_mat4_rotation_y(int angle);
r3d_mat4_t r3d_mat4_rotation_z(int angle);
r3d_vec3_t r3d_mat4_transform_point(r3d_mat4_t m, r3d_vec3_t v);

/*
 * Camera helpers.
 */
void r3d_camera_reset(r3d_camera_t* cam);
void r3d_camera_move_forward(r3d_camera_t* cam, i32 amount);
void r3d_camera_move_right(r3d_camera_t* cam, i32 amount);
void r3d_camera_move_up(r3d_camera_t* cam, i32 amount);
int  r3d_camera_handle_key(r3d_camera_t* cam, u32 key, i32 move_step);

/*
 * Renderer context.
 */
void r3d_context_init(
    r3d_context_t* ctx,
    u32* pixels,
    i32* depth,
    u32 width,
    u32 height
);

void r3d_context_set_flags(r3d_context_t* ctx, u32 flags);
void r3d_context_set_wire_color(r3d_context_t* ctx, u32 color);

void r3d_clear(r3d_context_t* ctx, u32 top_color, u32 bottom_color);
void r3d_clear_depth(r3d_context_t* ctx);

int r3d_project_vertex(
    const r3d_context_t* ctx,
    r3d_vec3_t view,
    r3d_vertex2_t* out
);

void r3d_draw_triangle(
    r3d_context_t* ctx,
    r3d_vertex2_t a,
    r3d_vertex2_t b,
    r3d_vertex2_t c,
    u32 color
);

void r3d_draw_mesh(
    r3d_context_t* ctx,
    const r3d_mesh_t* mesh,
    r3d_mat4_t model,
    const r3d_camera_t* cam
);

/*
 * Built-in test mesh.
 */
r3d_mesh_t r3d_cube_mesh(void);

#endif
