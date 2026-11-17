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

/*
 * r3d_draw_mesh() keeps per-vertex scratch arrays on the stack
 * (about 40 bytes per vertex), so this bounds stack use.
 */
#define R3D_MAX_MESH_VERTICES 1024u

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

/*
 * Triangle corner indices. Used both for vertex indices (mesh.tris) and for
 * texture-coordinate indices (mesh.tri_uvs). Matches the NMODEL file layout.
 */
typedef struct r3d_tri {
    u16 a;
    u16 b;
    u16 c;
} r3d_tri_t;

/*
 * Texture: 0x00RRGGBB pixels, row 0 at the top. Sampling wraps, so u/v
 * outside 0..1 tile the image.
 */
typedef struct r3d_texture {
    const u32* pixels;
    u32 width;
    u32 height;
} r3d_texture_t;

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

    /*
     * Optional texture mapping. A triangle is textured when uvs, tri_uvs
     * and texture are all set; otherwise it is flat shaded with tri_colors.
     * uvs are R3D_FP_ONE == 1.0; tri_uvs[i] indexes uvs for triangle i.
     */
    const r3d_vec2_t* uvs;
    u32 uv_count;
    const r3d_tri_t* tri_uvs;
    const r3d_texture_t* texture;
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

void r3d_draw_triangle_textured(
    r3d_context_t* ctx,
    r3d_vertex2_t a,
    r3d_vertex2_t b,
    r3d_vertex2_t c,
    r3d_vec2_t uva,
    r3d_vec2_t uvb,
    r3d_vec2_t uvc,
    const r3d_texture_t* texture,
    int brightness
);

/*
 * Assets.
 *
 * Both parsers work in place: the returned mesh/texture points into `data`,
 * so the buffer must stay alive (and unmodified) while it is in use.
 * They validate every count, offset and index against `size` and return 0
 * for anything malformed.
 */
int r3d_model_parse(const void* data, u64 size, r3d_mesh_t* out);
int r3d_texture_parse(const void* data, u64 size, r3d_texture_t* out);

/*
 * Convenience loaders: read the file into `buf` (capacity `buf_size`) and
 * parse it. Return 1 on success, 0 if the file is missing, too large for
 * the buffer, or malformed.
 */
int r3d_model_load(const char* path, void* buf, u64 buf_size, r3d_mesh_t* out);
int r3d_texture_load(
    const char* path,
    void* buf,
    u64 buf_size,
    r3d_texture_t* out
);

u32 r3d_texture_sample(const r3d_texture_t* texture, i32 u, i32 v);
u32 r3d_shade(u32 color, int brightness);

/*
 * Built-in test mesh.
 */
r3d_mesh_t r3d_cube_mesh(void);

#endif
