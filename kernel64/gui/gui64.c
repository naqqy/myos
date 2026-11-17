#include "gui64.h"

#include "../graphics/framebuffer64.h"
#include "../k64_console.h"
#include "../mm/kmem64.h"
#include "../console/font8x8.h"
#include "../arch/x86_64/timer64.h"
#include "../proc/process64.h"

#define GUI64_FONT_W 8
#define GUI64_FONT_H 16

#define GUI64_TITLE_H 24
#define GUI64_FRAME_PAD_X 4
#define GUI64_CONTENT_Y 29
#define GUI64_CONTENT_BOTTOM_PAD 4
#define GUI64_TASKBAR_H 34

/*
 * 64C repaint pacing:
 *
 * Timer is usually 100 Hz right now, so 1 tick is about 10ms.
 * This caps compositor flushes to roughly one damage flush per timer tick.
 * App presents between ticks are merged into one damage rectangle.
 */
#define GUI64_REPAINT_MIN_TICKS 1ull
#define GUI64_DAMAGE_PAD 2

/*
 * ToaruOS/Yutani-inspired model:
 *
 *   - Every window owns a surface/buffer.
 *   - Apps update window buffers through gui64_present_window().
 *   - Terminal output updates terminal session state.
 *   - The compositor is the only normal path that builds the final frame.
 *   - A final XRGB32 scene is composed into g_gui64_framebuffer and then
 *     copied to GOP/framebuffer in one flush.
 *
 * This file intentionally does NOT try to salvage the older direct-draw
 * fast paths.
 */

static gui64_window_t g_gui64_windows[GUI64_MAX_WINDOWS];

static int g_gui64_active = 0;
static int g_gui64_next_id = 1;
static int g_gui64_active_id = 0;
static int g_gui64_shell_terminal_window_id = 0;

static int g_gui64_z_order[GUI64_MAX_WINDOWS];
static int g_gui64_z_count = 0;

static nq_u32* g_gui64_framebuffer = 0;
static nq_u32 g_gui64_framebuffer_w = 0;
static nq_u32 g_gui64_framebuffer_h = 0;

static int g_gui64_repaint_requested = 0;
static int g_gui64_is_rendering = 0;
static nq_u64 g_gui64_last_flush_tick = 0;
static int g_gui64_next_app_spawn_slot = 0;

typedef struct gui64_rect {
    int x;
    int y;
    int w;
    int h;
} gui64_rect_t;

static int g_gui64_damage_valid = 0;
static gui64_rect_t g_gui64_damage;

static int g_gui64_clip_enabled = 0;
static int g_gui64_clip_x0 = 0;
static int g_gui64_clip_y0 = 0;
static int g_gui64_clip_x1 = 0;
static int g_gui64_clip_y1 = 0;

/* -------------------------------------------------------------------------- */
/* Small libc-style helpers                                                    */
/* -------------------------------------------------------------------------- */

static void mem_zero64(void* ptr, nq_u64 size) {
    nq_u8* p = (nq_u8*)ptr;

    if (!p) {
        return;
    }

    for (nq_u64 i = 0; i < size; i++) {
        p[i] = 0;
    }
}

static void str_copy64(char* dst, const char* src, nq_u64 dst_size) {
    nq_u64 i = 0;

    if (!dst || dst_size == 0) {
        return;
    }

    if (!src) {
        dst[0] = 0;
        return;
    }

    while (src[i] && i + 1 < dst_size) {
        dst[i] = src[i];
        i++;
    }

    dst[i] = 0;
}

static int gui64_strlen_limited(const char* s, int max_chars) {
    int len = 0;

    if (!s) {
        return 0;
    }

    while (s[len] && len < max_chars) {
        len++;
    }

    return len;
}

static nq_u32 gui64_xrgb(nq_u8 r, nq_u8 g, nq_u8 b) {
    return ((nq_u32)r << 16) | ((nq_u32)g << 8) | (nq_u32)b;
}

/*
 * Public drawing helpers historically accepted native framebuffer colors from
 * callers using fb64_make_color(). Internally, this compositor uses XRGB32.
 *
 * Most GUI calls in this file pass colors from gui64_xrgb(), but external
 * callers may still pass native colors. On common QEMU GOP RGB modes these are
 * identical. This keeps compatibility while we move toward XRGB surfaces.
 */
static nq_u32 gui64_rgb(nq_u8 r, nq_u8 g, nq_u8 b) {
    return gui64_xrgb(r, g, b);
}

static nq_u8 gui64_lerp_u8(nq_u8 a, nq_u8 b, nq_u32 t, nq_u32 max_t) {
    if (max_t == 0) {
        return a;
    }

    return (nq_u8)(((nq_u32)a * (max_t - t) + (nq_u32)b * t) / max_t);
}

static nq_u32 gui64_aero_mix(
    nq_u8 r0,
    nq_u8 g0,
    nq_u8 b0,
    nq_u8 r1,
    nq_u8 g1,
    nq_u8 b1,
    nq_u32 t,
    nq_u32 max_t
) {
    return gui64_rgb(
        gui64_lerp_u8(r0, r1, t, max_t),
        gui64_lerp_u8(g0, g1, t, max_t),
        gui64_lerp_u8(b0, b1, t, max_t)
    );
}


static gui64_rect_t gui64_rect_make(int x, int y, int w, int h) {
    gui64_rect_t r;

    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;

    return r;
}

static int gui64_rect_empty(gui64_rect_t r) {
    return r.w <= 0 || r.h <= 0;
}

static gui64_rect_t gui64_rect_clip_to_screen(gui64_rect_t r) {
    int x0 = r.x;
    int y0 = r.y;
    int x1 = r.x + r.w;
    int y1 = r.y + r.h;

    if (x0 < 0) {
        x0 = 0;
    }

    if (y0 < 0) {
        y0 = 0;
    }

    if (x1 > (int)fb64_width()) {
        x1 = (int)fb64_width();
    }

    if (y1 > (int)fb64_height()) {
        y1 = (int)fb64_height();
    }

    if (x1 <= x0 || y1 <= y0) {
        return gui64_rect_make(0, 0, 0, 0);
    }

    return gui64_rect_make(x0, y0, x1 - x0, y1 - y0);
}

static gui64_rect_t gui64_rect_union(gui64_rect_t a, gui64_rect_t b) {
    if (gui64_rect_empty(a)) {
        return b;
    }

    if (gui64_rect_empty(b)) {
        return a;
    }

    int x0 = a.x < b.x ? a.x : b.x;
    int y0 = a.y < b.y ? a.y : b.y;
    int x1 = (a.x + a.w) > (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    int y1 = (a.y + a.h) > (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);

    return gui64_rect_make(x0, y0, x1 - x0, y1 - y0);
}

static int gui64_rect_intersects(gui64_rect_t a, gui64_rect_t b) {
    if (gui64_rect_empty(a) || gui64_rect_empty(b)) {
        return 0;
    }

    if (a.x + a.w <= b.x) {
        return 0;
    }

    if (b.x + b.w <= a.x) {
        return 0;
    }

    if (a.y + a.h <= b.y) {
        return 0;
    }

    if (b.y + b.h <= a.y) {
        return 0;
    }

    return 1;
}

static gui64_rect_t gui64_window_outer_rect(gui64_window_t* win) {
    if (!win || !win->used) {
        return gui64_rect_make(0, 0, 0, 0);
    }

    /*
     * Include the active focus glow (drawn out to x-3/y-3) and the drop
     * shadow (drawn out to x+w+7/y+h+7). If this rect is smaller than what
     * gui64_draw_window() paints, stale glow/shadow pixels are left behind.
     */
    return gui64_rect_make(
        win->x - 3,
        win->y - 3,
        win->w + 10,
        win->h + 10
    );
}

static int gui64_z_index_for_window(int id) {
    for (int i = 0; i < g_gui64_z_count; i++) {
        if (g_gui64_z_order[i] == id) {
            return i;
        }
    }

    return -1;
}

static void gui64_set_clip(gui64_rect_t r) {
    r = gui64_rect_clip_to_screen(r);

    if (gui64_rect_empty(r)) {
        g_gui64_clip_enabled = 0;
        return;
    }

    g_gui64_clip_enabled = 1;
    g_gui64_clip_x0 = r.x;
    g_gui64_clip_y0 = r.y;
    g_gui64_clip_x1 = r.x + r.w;
    g_gui64_clip_y1 = r.y + r.h;
}

static void gui64_clear_clip(void) {
    g_gui64_clip_enabled = 0;
    g_gui64_clip_x0 = 0;
    g_gui64_clip_y0 = 0;
    g_gui64_clip_x1 = 0;
    g_gui64_clip_y1 = 0;
}

static void gui64_request_damage(int x, int y, int w, int h) {
    if (!g_gui64_active) {
        return;
    }

    gui64_rect_t r = gui64_rect_clip_to_screen(gui64_rect_make(x, y, w, h));

    if (gui64_rect_empty(r)) {
        return;
    }

    if (!g_gui64_damage_valid) {
        g_gui64_damage = r;
        g_gui64_damage_valid = 1;
    } else {
        g_gui64_damage = gui64_rect_union(g_gui64_damage, r);
        g_gui64_damage = gui64_rect_clip_to_screen(g_gui64_damage);
    }

    g_gui64_repaint_requested = 1;
}

/* -------------------------------------------------------------------------- */
/* Compositor target helpers                                                   */
/* -------------------------------------------------------------------------- */

static int gui64_compositor_ensure_framebuffer(void) {
    nq_u32 w = fb64_width();
    nq_u32 h = fb64_height();

    if (!fb64_available() || w == 0 || h == 0) {
        return 0;
    }

    if (g_gui64_framebuffer &&
        g_gui64_framebuffer_w == w &&
        g_gui64_framebuffer_h == h) {
        return 1;
    }

    if (g_gui64_framebuffer) {
        kfree64(g_gui64_framebuffer);
        g_gui64_framebuffer = 0;
    }

    nq_u64 bytes = (nq_u64)w * (nq_u64)h * sizeof(nq_u32);

    g_gui64_framebuffer = (nq_u32*)kmalloc64(bytes);

    if (!g_gui64_framebuffer) {
        g_gui64_framebuffer_w = 0;
        g_gui64_framebuffer_h = 0;
        return 0;
    }

    g_gui64_framebuffer_w = w;
    g_gui64_framebuffer_h = h;

    return 1;
}

static void gui64_put_pixel(int x, int y, nq_u32 color) {
    if (!g_gui64_framebuffer) {
        return;
    }

    if (x < 0 || y < 0) {
        return;
    }

    if (x >= (int)g_gui64_framebuffer_w ||
        y >= (int)g_gui64_framebuffer_h) {
        return;
    }

    if (g_gui64_clip_enabled) {
        if (x < g_gui64_clip_x0 || x >= g_gui64_clip_x1 ||
            y < g_gui64_clip_y0 || y >= g_gui64_clip_y1) {
            return;
        }
    }

    g_gui64_framebuffer[(nq_u32)y * g_gui64_framebuffer_w + (nq_u32)x] = color;
}


static void gui64_fill_rect_target(int x, int y, int w, int h, nq_u32 color) {
    if (!g_gui64_framebuffer || w <= 0 || h <= 0) {
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

    if (x1 > (int)g_gui64_framebuffer_w) {
        x1 = (int)g_gui64_framebuffer_w;
    }

    if (y1 > (int)g_gui64_framebuffer_h) {
        y1 = (int)g_gui64_framebuffer_h;
    }

    if (g_gui64_clip_enabled) {
        if (x0 < g_gui64_clip_x0) {
            x0 = g_gui64_clip_x0;
        }

        if (y0 < g_gui64_clip_y0) {
            y0 = g_gui64_clip_y0;
        }

        if (x1 > g_gui64_clip_x1) {
            x1 = g_gui64_clip_x1;
        }

        if (y1 > g_gui64_clip_y1) {
            y1 = g_gui64_clip_y1;
        }
    }

    if (x1 <= x0 || y1 <= y0) {
        return;
    }

    for (int yy = y0; yy < y1; yy++) {
        nq_u32* row = g_gui64_framebuffer + ((nq_u32)yy * g_gui64_framebuffer_w);

        for (int xx = x0; xx < x1; xx++) {
            row[(nq_u32)xx] = color;
        }
    }
}


static void gui64_flush_framebuffer(void) {
    if (!g_gui64_framebuffer || !fb64_available()) {
        return;
    }

    for (nq_u32 y = 0; y < g_gui64_framebuffer_h; y++) {
        fb64_draw_xrgb32_row(
            0,
            y,
            g_gui64_framebuffer + ((nq_u64)y * (nq_u64)g_gui64_framebuffer_w),
            g_gui64_framebuffer_w
        );
    }
}

static void gui64_flush_framebuffer_rect(gui64_rect_t r) {
    if (!g_gui64_framebuffer || !fb64_available()) {
        return;
    }

    r = gui64_rect_clip_to_screen(r);

    if (gui64_rect_empty(r)) {
        return;
    }

    for (int row = 0; row < r.h; row++) {
        int y = r.y + row;

        fb64_draw_xrgb32_row(
            (nq_u32)r.x,
            (nq_u32)y,
            g_gui64_framebuffer +
                ((nq_u64)y * (nq_u64)g_gui64_framebuffer_w) +
                (nq_u32)r.x,
            (nq_u32)r.w
        );
    }
}


/* -------------------------------------------------------------------------- */
/* Drawing primitives                                                          */
/* -------------------------------------------------------------------------- */

static void gui64_draw_rect(int x, int y, int w, int h, nq_u32 color) {
    gui64_fill_rect_target(x, y, w, h, color);
}

static void gui64_draw_border(int x, int y, int w, int h, nq_u32 color) {
    gui64_draw_rect(x, y, w, 1, color);
    gui64_draw_rect(x, y + h - 1, w, 1, color);
    gui64_draw_rect(x, y, 1, h, color);
    gui64_draw_rect(x + w - 1, y, 1, h, color);
}

void gui64_draw_char(int x, int y, char c, nq_u32 fg) {
    const unsigned char* glyph = font8x8_get(c);

    for (int gy = 0; gy < 8; gy++) {
        unsigned char bits = glyph[gy];

        for (int gx = 0; gx < 8; gx++) {
            if (bits & (0x80u >> gx)) {
                gui64_put_pixel(x + gx, y + gy * 2, fg);
                gui64_put_pixel(x + gx, y + gy * 2 + 1, fg);
            }
        }
    }
}

void gui64_draw_text(int x, int y, const char* text, nq_u32 fg) {
    int cx = x;

    if (!text) {
        return;
    }

    while (*text) {
        if (*text == '\n') {
            y += GUI64_FONT_H;
            cx = x;
        } else {
            gui64_draw_char(cx, y, *text, fg);
            cx += GUI64_FONT_W;
        }

        text++;
    }
}

void gui64_draw_label(int x, int y, const char* text) {
    gui64_draw_text(x, y, text, gui64_rgb(220, 245, 255));
}

void gui64_draw_panel(int x, int y, int w, int h, nq_u32 color) {
    gui64_draw_rect(x, y, w, h, color);
    gui64_draw_border(x, y, w, h, gui64_rgb(90, 180, 220));
    gui64_draw_rect(x + 1, y + 1, w - 2, 1, gui64_rgb(150, 230, 255));
}

void gui64_draw_button(int x, int y, int w, int h, const char* text, int pressed) {
    nq_u32 fill = pressed ? gui64_rgb(18, 70, 100) : gui64_rgb(30, 110, 150);
    nq_u32 border_hi = pressed ? gui64_rgb(50, 130, 165) : gui64_rgb(150, 235, 255);
    nq_u32 border_lo = pressed ? gui64_rgb(120, 220, 245) : gui64_rgb(8, 45, 70);
    nq_u32 text_color = gui64_rgb(230, 250, 255);

    gui64_draw_rect(x, y, w, h, fill);
    gui64_draw_rect(x, y, w, 1, border_hi);
    gui64_draw_rect(x, y, 1, h, border_hi);
    gui64_draw_rect(x, y + h - 1, w, 1, border_lo);
    gui64_draw_rect(x + w - 1, y, 1, h, border_lo);

    if (text && w > 12 && h > 12) {
        int max_chars = (w - 10) / GUI64_FONT_W;
        int len = gui64_strlen_limited(text, max_chars);
        int tx = x + (w - (len * GUI64_FONT_W)) / 2;
        int ty = y + (h - GUI64_FONT_H) / 2;

        if (tx < x + 4) {
            tx = x + 4;
        }

        if (ty < y + 2) {
            ty = y + 2;
        }

        for (int i = 0; i < len; i++) {
            gui64_draw_char(tx + i * GUI64_FONT_W, ty, text[i], text_color);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Window lookup / z-order                                                     */
/* -------------------------------------------------------------------------- */

static int gui64_find_slot_by_id(int id) {
    for (int i = 0; i < GUI64_MAX_WINDOWS; i++) {
        if (g_gui64_windows[i].used &&
            g_gui64_windows[i].id == id) {
            return i;
        }
    }

    return -1;
}

static gui64_window_t* gui64_find_window_by_id_internal(int id) {
    int slot = gui64_find_slot_by_id(id);

    if (slot < 0) {
        return 0;
    }

    return &g_gui64_windows[slot];
}

static gui64_window_t* gui64_focused_window(void) {
    return gui64_find_window_by_id_internal(g_gui64_active_id);
}

static void gui64_z_remove(int id) {
    int out = 0;

    for (int i = 0; i < g_gui64_z_count; i++) {
        if (g_gui64_z_order[i] == id) {
            continue;
        }

        g_gui64_z_order[out++] = g_gui64_z_order[i];
    }

    for (int i = out; i < GUI64_MAX_WINDOWS; i++) {
        g_gui64_z_order[i] = 0;
    }

    g_gui64_z_count = out;
}

static void gui64_z_add_front(int id) {
    if (id <= 0) {
        return;
    }

    gui64_z_remove(id);

    if (g_gui64_z_count >= GUI64_MAX_WINDOWS) {
        return;
    }

    g_gui64_z_order[g_gui64_z_count++] = id;
}

static int gui64_front_window_id(void) {
    if (g_gui64_z_count <= 0) {
        return 0;
    }

    return g_gui64_z_order[g_gui64_z_count - 1];
}

/* -------------------------------------------------------------------------- */
/* Dirty / event helpers                                                       */
/* -------------------------------------------------------------------------- */

static void gui64_window_clear_dirty(gui64_window_t* win) {
    if (!win) {
        return;
    }

    win->dirty = 0;
    win->dirty_x0 = 0;
    win->dirty_y0 = 0;
    win->dirty_x1 = 0;
    win->dirty_y1 = 0;
}

static void gui64_window_mark_dirty(gui64_window_t* win, int x, int y, int w, int h) {
    if (!win || w <= 0 || h <= 0) {
        return;
    }

    win->dirty = 1;
    win->dirty_x0 = x;
    win->dirty_y0 = y;
    win->dirty_x1 = x + w;
    win->dirty_y1 = y + h;
}

static void gui64_event_clear_queue(gui64_window_t* win) {
    if (!win) {
        return;
    }

    win->event_head = 0;
    win->event_tail = 0;

    for (nq_u32 i = 0; i < GUI64_EVENT_QUEUE_SIZE; i++) {
        win->events[i].type = GUI64_EVENT_NONE;
        win->events[i].key = 0;
        win->events[i].x = 0;
        win->events[i].y = 0;
        win->events[i].w = 0;
        win->events[i].h = 0;
    }
}

static int gui64_event_queue_empty(gui64_window_t* win) {
    return !win || win->event_head == win->event_tail;
}

static int gui64_push_event_to_window(gui64_window_t* win, const gui64_event_t* ev) {
    nq_u32 next_tail;

    if (!win || !win->used || !ev) {
        return 0;
    }

    next_tail = (win->event_tail + 1u) % GUI64_EVENT_QUEUE_SIZE;

    if (next_tail == win->event_head) {
        win->event_head = (win->event_head + 1u) % GUI64_EVENT_QUEUE_SIZE;
    }

    win->events[win->event_tail] = *ev;
    win->event_tail = next_tail;

    return 1;
}

static void gui64_push_focus_event(gui64_window_t* win) {
    gui64_event_t ev;

    if (!win || !win->used) {
        return;
    }

    ev.type = GUI64_EVENT_FOCUS;
    ev.key = 0;
    ev.x = win->x;
    ev.y = win->y;
    ev.w = win->w;
    ev.h = win->h;

    gui64_push_event_to_window(win, &ev);
}

/* -------------------------------------------------------------------------- */
/* Window geometry / drawing                                                   */
/* -------------------------------------------------------------------------- */

static int gui64_window_is_borderless(gui64_window_t* win) {
    if (!win) {
        return 0;
    }

    return (win->flags & GUI64_WIN_FLAG_BORDERLESS) ||
           (win->flags & GUI64_WIN_FLAG_FULLSCREEN);
}

static void gui64_window_content_rect(gui64_window_t* win, int* x, int* y, int* w, int* h) {
    if (!win) {
        if (x) *x = 0;
        if (y) *y = 0;
        if (w) *w = 0;
        if (h) *h = 0;
        return;
    }

    if (gui64_window_is_borderless(win)) {
        if (x) *x = win->x;
        if (y) *y = win->y;
        if (w) *w = win->w;
        if (h) *h = win->h;
        return;
    }

    if (x) *x = win->x + GUI64_FRAME_PAD_X;
    if (y) *y = win->y + GUI64_CONTENT_Y;
    if (w) *w = win->w - GUI64_FRAME_PAD_X * 2;
    if (h) *h = win->h - GUI64_CONTENT_Y - GUI64_CONTENT_BOTTOM_PAD;
}

static void gui64_blit_surface_to_frame(
    gui64_window_t* win,
    int dst_x,
    int dst_y,
    int src_x,
    int src_y,
    int w,
    int h
) {
    if (!win || !win->buffer || !g_gui64_framebuffer) {
        return;
    }

    if (w <= 0 || h <= 0) {
        return;
    }

    if (src_x < 0) {
        dst_x -= src_x;
        w += src_x;
        src_x = 0;
    }

    if (src_y < 0) {
        dst_y -= src_y;
        h += src_y;
        src_y = 0;
    }

    if (src_x + w > (int)win->buffer_width) {
        w = (int)win->buffer_width - src_x;
    }

    if (src_y + h > (int)win->buffer_height) {
        h = (int)win->buffer_height - src_y;
    }

    if (w <= 0 || h <= 0) {
        return;
    }

    for (int row = 0; row < h; row++) {
        int dy = dst_y + row;

        if (dy < 0 || dy >= (int)g_gui64_framebuffer_h) {
            continue;
        }

        if (g_gui64_clip_enabled &&
            (dy < g_gui64_clip_y0 || dy >= g_gui64_clip_y1)) {
            continue;
        }

        int dx = dst_x;
        int copy_w = w;
        int src_off = 0;

        if (dx < 0) {
            src_off = -dx;
            copy_w += dx;
            dx = 0;
        }

        if (dx + copy_w > (int)g_gui64_framebuffer_w) {
            copy_w = (int)g_gui64_framebuffer_w - dx;
        }

        if (g_gui64_clip_enabled) {
            if (dx < g_gui64_clip_x0) {
                int delta = g_gui64_clip_x0 - dx;
                src_off += delta;
                copy_w -= delta;
                dx = g_gui64_clip_x0;
            }

            if (dx + copy_w > g_gui64_clip_x1) {
                copy_w = g_gui64_clip_x1 - dx;
            }
        }

        if (copy_w <= 0) {
            continue;
        }

        const nq_u32* src =
            win->buffer +
            ((nq_u64)(src_y + row) * (nq_u64)win->buffer_pitch) +
            (nq_u64)(src_x + src_off);

        nq_u32* dst =
            g_gui64_framebuffer +
            ((nq_u32)dy * g_gui64_framebuffer_w) +
            (nq_u32)dx;

        for (int col = 0; col < copy_w; col++) {
            dst[col] = src[col];
        }
    }
}


static void gui64_draw_desktop(void) {
    nq_u32 width = g_gui64_framebuffer_w;
    nq_u32 height = g_gui64_framebuffer_h;

    if (width == 0 || height == 0) {
        return;
    }

    /*
     * Muted solid green desktop.
     *
     * Darker and less neon than the previous 186,229,174 color.
     * The desktop stays simple; the Aero feeling comes from the glassy
     * window chrome and taskbar.
     */
    gui64_fill_rect_target(
        0,
        0,
        (int)width,
        (int)height,
        gui64_rgb(134, 181, 122)
    );

    /*
     * Very subtle low-contrast desktop label.
     */
    gui64_draw_text(18, 18, "NAQ OS", gui64_rgb(65, 111, 68));
}



static void gui64_draw_taskbar(void) {
    int w = (int)g_gui64_framebuffer_w;
    int h = (int)g_gui64_framebuffer_h;
    int y = h - GUI64_TASKBAR_H;

    /*
     * Slightly darker glossy aqua taskbar.
     */
    gui64_draw_rect(0, y, w, GUI64_TASKBAR_H, gui64_rgb(28, 134, 162));
    gui64_draw_rect(0, y + 1, w, 8, gui64_rgb(149, 231, 239));
    gui64_draw_rect(0, y + 9, w, 1, gui64_rgb(245, 255, 255));
    gui64_draw_rect(0, y + 10, w, GUI64_TASKBAR_H - 10, gui64_rgb(35, 157, 181));
    gui64_draw_rect(0, y + GUI64_TASKBAR_H - 2, w, 2, gui64_rgb(16, 91, 125));

    gui64_draw_button(8, y + 6, 58, 22, "NAQ", 0);

    int bx = 78;

    for (int zi = 0; zi < g_gui64_z_count; zi++) {
        int slot = gui64_find_slot_by_id(g_gui64_z_order[zi]);

        if (slot < 0) {
            continue;
        }

        gui64_window_t* win = &g_gui64_windows[slot];

        gui64_draw_button(
            bx,
            y + 6,
            110,
            22,
            win->title,
            win->active
        );

        bx += 118;

        if (bx + 110 >= w - 130) {
            break;
        }
    }

    gui64_draw_panel(w - 120, y + 6, 110, 22, gui64_rgb(70, 183, 200));
    gui64_draw_text(w - 110, y + 9, "AERO", gui64_rgb(255, 255, 255));
}


static void gui64_draw_window(gui64_window_t* win) {
    if (!win || !win->used) {
        return;
    }

    int x = win->x;
    int y = win->y;
    int w = win->w;
    int h = win->h;

    if (gui64_window_is_borderless(win)) {
        gui64_draw_rect(x, y, w, h, win->bg_color);

        if (win->kind == GUI64_WINDOW_TERMINAL && win->terminal) {
            terminal64_render_into_buffer(
                win->terminal,
                g_gui64_framebuffer,
                g_gui64_framebuffer_w,
                g_gui64_framebuffer_h,
                x,
                y,
                w,
                h
            );
        } else if (win->buffer) {
            gui64_blit_surface_to_frame(
                win,
                x,
                y,
                0,
                0,
                (int)win->buffer_width,
                (int)win->buffer_height
            );
        }

        win->chrome_drawn = 1;
        return;
    }

    /*
     * Stronger Aero glass chrome:
     * pale outer rim, aqua inner rim, white highlight, teal depth.
     */
    nq_u32 shadow = gui64_rgb(38, 83, 78);
    nq_u32 shadow_soft = gui64_rgb(72, 133, 121);

    nq_u32 outer_glass = win->active ? gui64_rgb(235, 255, 249) : gui64_rgb(174, 226, 220);
    nq_u32 inner_glass = win->active ? gui64_rgb(113, 218, 224) : gui64_rgb(93, 169, 180);
    nq_u32 rim_dark = win->active ? gui64_rgb(31, 132, 151) : gui64_rgb(48, 111, 124);
    nq_u32 content_border = win->active ? gui64_rgb(232, 255, 250) : gui64_rgb(197, 235, 229);

    nq_u32 title_top = win->active ? gui64_rgb(255, 255, 255) : gui64_rgb(220, 245, 241);
    nq_u32 title_mid = win->active ? gui64_rgb(119, 226, 235) : gui64_rgb(104, 181, 192);
    nq_u32 title_bot = win->active ? gui64_rgb(32, 151, 192) : gui64_rgb(56, 119, 150);

    gui64_draw_rect(x + 7, y + 7, w, h, shadow);
    gui64_draw_rect(x + 5, y + 5, w, h, shadow_soft);

    /*
     * Outer glass frame.
     */
    gui64_draw_rect(x, y, w, h, outer_glass);
    gui64_draw_rect(x + 1, y + 1, w - 2, h - 2, gui64_rgb(255, 255, 255));
    gui64_draw_rect(x + 2, y + 2, w - 4, h - 4, inner_glass);
    gui64_draw_rect(x + 3, y + 3, w - 6, h - 6, content_border);

    /*
     * Dark lower/right edge gives the glass border depth instead of the old
     * flat blue frame look.
     */
    gui64_draw_rect(x + 2, y + h - 3, w - 4, 1, rim_dark);
    gui64_draw_rect(x + w - 3, y + 2, 1, h - 4, rim_dark);

    /*
     * Glossy titlebar.
     */
    for (int yy = 0; yy < GUI64_TITLE_H; yy++) {
        nq_u32 color;

        if (yy < GUI64_TITLE_H / 2) {
            color = gui64_aero_mix(
                (nq_u8)((title_top >> 16) & 0xFFu),
                (nq_u8)((title_top >> 8) & 0xFFu),
                (nq_u8)(title_top & 0xFFu),
                (nq_u8)((title_mid >> 16) & 0xFFu),
                (nq_u8)((title_mid >> 8) & 0xFFu),
                (nq_u8)(title_mid & 0xFFu),
                (nq_u32)yy,
                (nq_u32)(GUI64_TITLE_H / 2)
            );
        } else {
            color = gui64_aero_mix(
                (nq_u8)((title_mid >> 16) & 0xFFu),
                (nq_u8)((title_mid >> 8) & 0xFFu),
                (nq_u8)(title_mid & 0xFFu),
                (nq_u8)((title_bot >> 16) & 0xFFu),
                (nq_u8)((title_bot >> 8) & 0xFFu),
                (nq_u8)(title_bot & 0xFFu),
                (nq_u32)(yy - GUI64_TITLE_H / 2),
                (nq_u32)(GUI64_TITLE_H - GUI64_TITLE_H / 2)
            );
        }

        gui64_draw_rect(x + 4, y + 4 + yy, w - 8, 1, color);
    }

    /*
     * Glass glare streaks.
     */
    gui64_draw_rect(x + 6, y + 6, w - 12, 2, gui64_rgb(255, 255, 255));
    gui64_draw_rect(x + 7, y + 9, w - 14, 1, gui64_rgb(220, 255, 252));
    gui64_draw_rect(x + 10, y + 12, w / 2, 1, gui64_rgb(192, 255, 246));

    /*
     * Title text with subtle dark shadow.
     */
    gui64_draw_text(x + 13, y + 8, win->title, gui64_rgb(22, 91, 112));
    gui64_draw_text(x + 12, y + 7, win->title, gui64_rgb(255, 255, 255));

    gui64_draw_rect(
        x + GUI64_FRAME_PAD_X,
        y + GUI64_CONTENT_Y,
        w - GUI64_FRAME_PAD_X * 2,
        h - GUI64_CONTENT_Y - GUI64_CONTENT_BOTTOM_PAD,
        win->bg_color
    );

    /*
     * Thin inner content bevel.
     */
    gui64_draw_rect(
        x + GUI64_FRAME_PAD_X,
        y + GUI64_CONTENT_Y,
        w - GUI64_FRAME_PAD_X * 2,
        1,
        gui64_rgb(255, 255, 255)
    );
    gui64_draw_rect(
        x + GUI64_FRAME_PAD_X,
        y + h - GUI64_CONTENT_BOTTOM_PAD - 1,
        w - GUI64_FRAME_PAD_X * 2,
        1,
        gui64_rgb(95, 172, 170)
    );

    if (win->kind == GUI64_WINDOW_TERMINAL && win->terminal) {
        terminal64_render_into_buffer(
            win->terminal,
            g_gui64_framebuffer,
            g_gui64_framebuffer_w,
            g_gui64_framebuffer_h,
            x + 8,
            y + 33,
            w - 16,
            h - 41
        );
    } else if (win->buffer) {
        gui64_blit_surface_to_frame(
            win,
            x + GUI64_FRAME_PAD_X,
            y + GUI64_CONTENT_Y,
            0,
            0,
            (int)win->buffer_width,
            (int)win->buffer_height
        );
    } else {
        gui64_draw_label(x + 16, y + 45, "Kernel GUI window");
        gui64_draw_text(
            x + 16,
            y + 66,
            "widgets: labels panels buttons",
            gui64_rgb(255, 255, 255)
        );

        gui64_draw_panel(x + 16, y + 92, w - 32, 42, gui64_rgb(90, 205, 221));
        gui64_draw_button(x + 28, y + 103, 82, 22, "Button", 0);
        gui64_draw_button(x + 120, y + 103, 82, 22, "Pressed", 1);
    }

    int bx = x + w - 23;
    int by = y + 7;

    gui64_draw_button(bx - 18, by, 14, 14, "_", 0);
    gui64_draw_button(bx, by, 14, 14, "X", 0);

    if (win->active) {
        /*
         * Bright glass focus glow.
         */
        gui64_draw_border(x - 1, y - 1, w + 2, h + 2, gui64_rgb(255, 255, 255));
        gui64_draw_border(x - 2, y - 2, w + 4, h + 4, gui64_rgb(139, 247, 223));
        gui64_draw_border(x - 3, y - 3, w + 6, h + 6, gui64_rgb(75, 173, 160));
    }

    win->chrome_drawn = 1;
}


static void gui64_request_window_outer_damage(gui64_window_t* win) {
    if (!win || !win->used) {
        return;
    }

    /*
     * Include focus glow and shadow.
     */
    gui64_rect_t outer = gui64_window_outer_rect(win);

    gui64_request_damage(outer.x, outer.y, outer.w, outer.h);
}

static void gui64_request_window_content_damage(gui64_window_t* win) {
    if (!win || !win->used) {
        return;
    }

    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    gui64_window_content_rect(win, &x, &y, &w, &h);

    gui64_rect_t content_damage = gui64_rect_make(
        x - GUI64_DAMAGE_PAD,
        y - GUI64_DAMAGE_PAD,
        w + GUI64_DAMAGE_PAD * 2,
        h + GUI64_DAMAGE_PAD * 2
    );

    gui64_rect_t dirty_outer = gui64_window_outer_rect(win);
    gui64_rect_t damage = content_damage;
    int dirty_z = gui64_z_index_for_window(win->id);
    int overlaps_any_window = 0;

    /*
     * Stability-first overlap rule:
     *
     * If an animated/content-presenting window overlaps any other window,
     * do not try to refresh only its content rectangle. Expand to the full
     * dirty window rect. This prevents terminal/window internals that are
     * still rendered dynamically during composition from leaking through the
     * titlebar/border/non-content area.
     */
    for (int i = 0; i < GUI64_MAX_WINDOWS; i++) {
        gui64_window_t* other = &g_gui64_windows[i];

        if (!other->used || other->id == win->id) {
            continue;
        }

        if (gui64_rect_intersects(dirty_outer, gui64_window_outer_rect(other))) {
            overlaps_any_window = 1;
            break;
        }
    }

    if (overlaps_any_window) {
        damage = dirty_outer;
    }

    /*
     * If any higher z-order window overlaps the damaged area, include that
     * higher window's full outer rect too. This guarantees that top windows
     * get restored completely after a lower/underlapping app presents.
     */
    if (dirty_z >= 0) {
        for (int zi = dirty_z + 1; zi < g_gui64_z_count; zi++) {
            int slot = gui64_find_slot_by_id(g_gui64_z_order[zi]);

            if (slot < 0) {
                continue;
            }

            gui64_window_t* above = &g_gui64_windows[slot];
            gui64_rect_t above_outer = gui64_window_outer_rect(above);

            if (gui64_rect_intersects(damage, above_outer)) {
                damage = gui64_rect_union(damage, above_outer);
            }
        }
    }

    damage = gui64_rect_clip_to_screen(damage);

    if (!gui64_rect_empty(damage)) {
        gui64_request_damage(damage.x, damage.y, damage.w, damage.h);
    }
}



/* -------------------------------------------------------------------------- */
/* Lifecycle / compositor                                                      */
/* -------------------------------------------------------------------------- */

void gui64_init(void) {
    mem_zero64(g_gui64_windows, sizeof(g_gui64_windows));

    g_gui64_active = 0;
    g_gui64_next_id = 1;
    g_gui64_active_id = 0;
    g_gui64_shell_terminal_window_id = 0;
    g_gui64_z_count = 0;
    g_gui64_repaint_requested = 0;
    g_gui64_is_rendering = 0;
    g_gui64_last_flush_tick = 0;
    g_gui64_next_app_spawn_slot = 0;
    g_gui64_damage_valid = 0;
    g_gui64_damage = gui64_rect_make(0, 0, 0, 0);
    gui64_clear_clip();

    for (int i = 0; i < GUI64_MAX_WINDOWS; i++) {
        g_gui64_z_order[i] = 0;
    }
}

void gui64_enter(void) {
    g_gui64_active = 1;
    terminal64_set_gui_mode(1);
    gui64_show_kernel_terminal();
    gui64_render();
}

void gui64_exit(void) {
    g_gui64_active = 0;
    g_gui64_repaint_requested = 0;
    g_gui64_last_flush_tick = 0;
    g_gui64_damage_valid = 0;
    gui64_clear_clip();
    terminal64_set_gui_mode(0);
}

int gui64_active(void) {
    return g_gui64_active;
}

void gui64_request_repaint(void) {
    if (!g_gui64_active) {
        return;
    }

    /*
     * Public repaint request means "the whole desktop may need rebuilding."
     * Fine-grained paths use gui64_request_damage().
     */
    gui64_request_damage(
        0,
        0,
        (int)fb64_width(),
        (int)fb64_height()
    );
}

static void gui64_composite_rect(gui64_rect_t damage, int flush_to_screen) {
    damage = gui64_rect_clip_to_screen(damage);

    if (gui64_rect_empty(damage)) {
        return;
    }

    if (!fb64_available()) {
        return;
    }

    /*
     * The compositor touches global GUI state and the screen backbuffer.
     * Do not let timer preemption switch to another app/syscall in the
     * middle of rebuilding a damaged region.
     */
    process64_preempt_disable();

    if (g_gui64_is_rendering) {
        g_gui64_repaint_requested = 1;
        g_gui64_damage = gui64_rect_union(g_gui64_damage, damage);
        g_gui64_damage_valid = 1;
        process64_preempt_enable();
        return;
    }

    if (!gui64_compositor_ensure_framebuffer()) {
        process64_preempt_enable();
        return;
    }

    g_gui64_is_rendering = 1;

    /*
     * OSDev-style buffered compositor:
     *
     * 1. Draw only the damaged screen rectangle into the compositor
     *    backbuffer.
     * 2. Rebuild that rectangle from bottom to top using z-order.
     * 3. Flush only that rectangle to the real framebuffer.
     *
     * No app or terminal path draws directly to video memory.
     */
    gui64_set_clip(damage);

    gui64_draw_desktop();

    for (int zi = 0; zi < g_gui64_z_count; zi++) {
        int slot = gui64_find_slot_by_id(g_gui64_z_order[zi]);

        if (slot < 0) {
            continue;
        }

        gui64_draw_window(&g_gui64_windows[slot]);
    }

    gui64_draw_taskbar();

    gui64_clear_clip();

    if (flush_to_screen) {
        gui64_flush_framebuffer_rect(damage);
    }

    g_gui64_is_rendering = 0;

    process64_preempt_enable();
}


static int gui64_can_flush_repaint_now(void) {
    nq_u64 now = timer64_get_ticks();

    /*
     * If the timer has not started yet, allow repaint so GUI boot still
     * appears. Once ticks are advancing, coalesce damage to at most one flush
     * per GUI64_REPAINT_MIN_TICKS.
     */
    if (now == 0 || g_gui64_last_flush_tick == 0) {
        return 1;
    }

    if (now < g_gui64_last_flush_tick) {
        return 1;
    }

    if ((now - g_gui64_last_flush_tick) >= GUI64_REPAINT_MIN_TICKS) {
        return 1;
    }

    return 0;
}

void gui64_flush_repaint(void) {
    if (!g_gui64_active) {
        g_gui64_repaint_requested = 0;
        g_gui64_damage_valid = 0;
        return;
    }

    if (!g_gui64_repaint_requested || !g_gui64_damage_valid) {
        return;
    }

    /*
     * Frame pacing / coalescing:
     *
     * Do not flush every SYS_WIN_PRESENT immediately. Multiple app presents
     * and terminal updates between timer ticks stay merged in g_gui64_damage.
     * The next allowed flush draws only the newest combined damage.
     */
    if (!gui64_can_flush_repaint_now()) {
        return;
    }

    gui64_rect_t damage = g_gui64_damage;

    g_gui64_repaint_requested = 0;
    g_gui64_damage_valid = 0;
    g_gui64_damage = gui64_rect_make(0, 0, 0, 0);

    gui64_composite_rect(damage, 1);

    g_gui64_last_flush_tick = timer64_get_ticks();
}


void gui64_render(void) {
    if (!fb64_available()) {
        return;
    }

    /*
     * Explicit full render for structural changes or GUI enter.
     * This bypasses pacing because structure/focus/title/window-list changes
     * should be visible immediately.
     */
    g_gui64_repaint_requested = 0;
    g_gui64_damage_valid = 0;
    g_gui64_damage = gui64_rect_make(0, 0, 0, 0);

    gui64_composite_rect(
        gui64_rect_make(
            0,
            0,
            (int)fb64_width(),
            (int)fb64_height()
        ),
        1
    );

    g_gui64_last_flush_tick = timer64_get_ticks();
}



/* -------------------------------------------------------------------------- */
/* Window creation / focus                                                     */
/* -------------------------------------------------------------------------- */

int gui64_create_window(
    const char* title,
    int x,
    int y,
    int w,
    int h,
    nq_u32 bg_color
) {
    for (int i = 0; i < GUI64_MAX_WINDOWS; i++) {
        gui64_window_t* win = &g_gui64_windows[i];

        if (win->used) {
            continue;
        }

        mem_zero64(win, sizeof(*win));

        win->used = 1;
        win->id = g_gui64_next_id++;
        win->active = 0;
        win->chrome_drawn = 0;
        win->kind = GUI64_WINDOW_NORMAL;
        win->flags = GUI64_WIN_FLAG_BORDERED;
        win->owner_pid = 0;
        win->terminal = 0;

        win->x = x;
        win->y = y;
        win->w = w;
        win->h = h;
        win->bg_color = bg_color;

        gui64_window_clear_dirty(win);
        gui64_event_clear_queue(win);

        str_copy64(win->title, title ? title : "Window", sizeof(win->title));

        gui64_z_add_front(win->id);

        if (g_gui64_active_id == 0) {
            gui64_set_active_window(win->id);
        }

        return win->id;
    }

    return -1;
}

void gui64_set_active_window(int id) {
    int found = 0;
    gui64_window_t* focused = 0;

    for (int i = 0; i < GUI64_MAX_WINDOWS; i++) {
        gui64_window_t* win = &g_gui64_windows[i];

        if (!win->used) {
            continue;
        }

        win->chrome_drawn = 0;

        if (win->id == id) {
            win->active = 1;
            focused = win;
            found = 1;
        } else {
            win->active = 0;
        }
    }

    if (!found) {
        g_gui64_active_id = 0;
        return;
    }

    g_gui64_active_id = id;
    gui64_z_add_front(id);
    gui64_push_focus_event(focused);

    gui64_request_repaint();
}

void gui64_next_window(void) {
    if (g_gui64_z_count <= 1) {
        return;
    }

    /*
     * Real cycle behavior:
     *
     * g_gui64_z_order is bottom -> top, and the top window is the last item.
     *
     * The old code picked z_count - 2, then gui64_set_active_window() moved
     * that window back to the front. That creates a two-window toggle:
     *
     *   A B C [D] -> pick C -> A B D [C] -> pick D -> A B C [D]
     *
     * To cycle through every window, rotate the current front/top window to
     * the bottom, then make the new top active without calling
     * gui64_set_active_window(), because that would reorder it again.
     */
    int old_front = g_gui64_z_order[g_gui64_z_count - 1];

    for (int i = g_gui64_z_count - 1; i > 0; i--) {
        g_gui64_z_order[i] = g_gui64_z_order[i - 1];
    }

    g_gui64_z_order[0] = old_front;

    int next_id = g_gui64_z_order[g_gui64_z_count - 1];
    gui64_window_t* focused = 0;
    int found = 0;

    for (int i = 0; i < GUI64_MAX_WINDOWS; i++) {
        gui64_window_t* win = &g_gui64_windows[i];

        if (!win->used) {
            continue;
        }

        win->chrome_drawn = 0;

        if (win->id == next_id) {
            win->active = 1;
            focused = win;
            found = 1;
        } else {
            win->active = 0;
        }
    }

    if (!found) {
        g_gui64_active_id = 0;
        gui64_request_repaint();

        if (g_gui64_active) {
            gui64_render();
        }

        return;
    }

    g_gui64_active_id = next_id;
    gui64_push_focus_event(focused);

    gui64_request_repaint();

    if (g_gui64_active) {
        gui64_render();
    }
}


/* -------------------------------------------------------------------------- */
/* Terminal windows                                                            */
/* -------------------------------------------------------------------------- */

int gui64_create_terminal_window(
    terminal64_session_t* terminal,
    const char* title,
    int x,
    int y,
    int w,
    int h
) {
    if (!terminal || w <= 0 || h <= 0) {
        return -1;
    }

    int id = gui64_create_window(
        title ? title : "Terminal",
        x,
        y,
        w,
        h,
        gui64_rgb(3, 10, 16)
    );

    if (id < 0) {
        return -1;
    }

    gui64_window_t* win = gui64_find_window_by_id_internal(id);

    if (!win) {
        return -1;
    }

    win->kind = GUI64_WINDOW_TERMINAL;
    win->owner_pid = 0;
    win->terminal = terminal;
    win->chrome_drawn = 0;

    gui64_request_window_outer_damage(win);

    return id;
}

int gui64_show_kernel_terminal(void) {
    if (g_gui64_shell_terminal_window_id != 0 &&
        gui64_find_window_by_id_internal(g_gui64_shell_terminal_window_id)) {
        gui64_set_active_window(g_gui64_shell_terminal_window_id);
        return g_gui64_shell_terminal_window_id;
    }

    int screen_w = (int)fb64_width();
    int screen_h = (int)fb64_height();

    if (screen_w <= 0) {
        screen_w = 1024;
    }

    if (screen_h <= 0) {
        screen_h = 768;
    }

    int w = screen_w > 900 ? 720 : screen_w - 80;
    int h = screen_h > 650 ? 430 : screen_h - 150;

    if (w < 240) {
        w = 240;
    }

    if (h < 160) {
        h = 160;
    }

    g_gui64_shell_terminal_window_id = gui64_create_terminal_window(
        terminal64_kernel(),
        "KER64 Terminal",
        42,
        52,
        w,
        h
    );

    return g_gui64_shell_terminal_window_id;
}

int gui64_has_focused_terminal_window(void) {
    gui64_window_t* win = gui64_focused_window();

    if (!win || !win->used) {
        return 0;
    }

    return win->kind == GUI64_WINDOW_TERMINAL && win->terminal != 0;
}

void gui64_render_terminal_windows(void) {
    /*
     * A terminal write can affect any terminal surface. Mark terminal content
     * areas damaged; the compositor will rebuild final pixels in z-order.
     */
    for (int i = 0; i < GUI64_MAX_WINDOWS; i++) {
        gui64_window_t* win = &g_gui64_windows[i];

        if (!win->used ||
            win->kind != GUI64_WINDOW_TERMINAL ||
            !win->terminal) {
            continue;
        }

        gui64_request_window_content_damage(win);
    }
}

void gui64_render_focused_terminal_content(void) {
    gui64_window_t* win = gui64_focused_window();

    if (!win ||
        win->kind != GUI64_WINDOW_TERMINAL ||
        !win->terminal) {
        return;
    }

    gui64_request_window_content_damage(win);
}

void gui64_render_focused_terminal_window(void) {
    gui64_window_t* win = gui64_focused_window();

    if (!win ||
        win->kind != GUI64_WINDOW_TERMINAL ||
        !win->terminal) {
        return;
    }

    gui64_request_window_outer_damage(win);
}


/* -------------------------------------------------------------------------- */
/* App windows                                                                 */
/* -------------------------------------------------------------------------- */

int gui64_create_app_window_ex(
    int owner_pid,
    const char* title,
    int x,
    int y,
    int w,
    int h,
    nq_u32 flags
) {
    if (owner_pid <= 0) {
        return -1;
    }

    if (flags & GUI64_WIN_FLAG_FULLSCREEN) {
        x = 0;
        y = 0;
        w = (int)fb64_width();
        h = (int)fb64_height();
        flags |= GUI64_WIN_FLAG_BORDERLESS;
    }

    if (w <= 0 || h <= 0) {
        return -1;
    }

    /*
    * TEMP TEST:
    * Force normal app windows to spawn in separated positions so we can
    * check whether flicker/clipping only happens when windows overlap.
    */
    if (!(flags & GUI64_WIN_FLAG_FULLSCREEN)) {
        int slot = g_gui64_next_app_spawn_slot++ & 3;

        int screen_w = (int)fb64_width();
        int screen_h = (int)fb64_height();

        if (screen_w <= 0) {
            screen_w = 1280;
        }

        if (screen_h <= 0) {
            screen_h = 800;
        }

        if (slot == 0) {
            x = 90;
            y = 90;
        } else if (slot == 1) {
            x = screen_w / 2;
            y = 90;
        } else if (slot == 2) {
            x = 90;
            y = screen_h / 2;
        } else {
            x = screen_w / 2;
            y = screen_h / 2;
        }

        /*
        * Keep windows above the taskbar and inside the screen.
        */
        if (x + w + 16 > screen_w) {
            x = screen_w - w - 24;
        }

        if (y + h + GUI64_TASKBAR_H + 16 > screen_h) {
            y = screen_h - h - GUI64_TASKBAR_H - 24;
        }

        if (x < 20) {
            x = 20;
        }

        if (y < 50) {
            y = 50;
        }
    }

    if (w > 3840 || h > 2160) {
        return -1;
    }

    int outer_w = w;
    int outer_h = h;

    if (!(flags & GUI64_WIN_FLAG_BORDERLESS) &&
        !(flags & GUI64_WIN_FLAG_FULLSCREEN)) {
        outer_w = w + GUI64_FRAME_PAD_X * 2;
        outer_h = h + GUI64_CONTENT_Y + GUI64_CONTENT_BOTTOM_PAD;
    }

    int id = gui64_create_window(
        title ? title : "App",
        x,
        y,
        outer_w,
        outer_h,
        gui64_rgb(5, 18, 28)
    );

    if (id < 0) {
        return -1;
    }

    gui64_window_t* win = gui64_find_window_by_id_internal(id);

    if (!win) {
        return -1;
    }

    win->kind = GUI64_WINDOW_APP;
    win->flags = flags;
    win->owner_pid = owner_pid;
    win->terminal = 0;

    win->buffer_width = (nq_u32)w;
    win->buffer_height = (nq_u32)h;
    win->buffer_pitch = (nq_u32)w;
    win->buffer_size =
        (nq_u64)win->buffer_pitch *
        (nq_u64)win->buffer_height *
        sizeof(nq_u32);

    win->buffer = (nq_u32*)kmalloc64(win->buffer_size);

    if (!win->buffer) {
        gui64_z_remove(id);
        mem_zero64(win, sizeof(*win));
        return -1;
    }

    for (nq_u64 i = 0;
         i < ((nq_u64)win->buffer_pitch * (nq_u64)win->buffer_height);
         i++) {
        win->buffer[i] = 0x00000000u;
    }

    gui64_window_mark_dirty(win, 0, 0, w, h);
    gui64_set_active_window(id);
    gui64_request_window_outer_damage(win);

    return id;
}

int gui64_create_app_window(
    int owner_pid,
    const char* title,
    int x,
    int y,
    int w,
    int h
) {
    return gui64_create_app_window_ex(
        owner_pid,
        title,
        x,
        y,
        w,
        h,
        GUI64_WIN_FLAG_BORDERED
    );
}

int gui64_present_window(
    int id,
    int requester_pid,
    const nq_u32* pixels,
    nq_u32 width,
    nq_u32 height
) {
    gui64_window_t* win = gui64_find_window_by_id_internal(id);

    if (!win || !pixels) {
        return 0;
    }

    if (win->owner_pid != requester_pid && requester_pid != 0) {
        return 0;
    }

    if (!win->buffer) {
        return 0;
    }

    if (width != win->buffer_width ||
        height != win->buffer_height) {
        return 0;
    }

    /*
     * App presents update shared kernel window buffers and damage state.
     * Protect this path too. If called from syscall64, this nests safely.
     */
    process64_preempt_disable();

    /*
     * Application presents update only the window surface.
     * They never touch the real framebuffer directly.
     */
    for (nq_u32 y = 0; y < height; y++) {
        nq_u32* dst =
            win->buffer +
            ((nq_u64)y * (nq_u64)win->buffer_pitch);

        const nq_u32* src =
            pixels + ((nq_u64)y * (nq_u64)width);

        for (nq_u32 x = 0; x < width; x++) {
            dst[x] = src[x];
        }
    }

    gui64_window_mark_dirty(win, 0, 0, (int)width, (int)height);

    /*
     * Convert the app content area into screen-space damage. The compositor
     * will redraw that screen region from desktop -> windows -> taskbar in
     * z-order and then flush only that region.
     */
    if (g_gui64_active) {
        gui64_request_window_content_damage(win);
    }

    gui64_window_clear_dirty(win);

    process64_preempt_enable();

    return 1;
}



int gui64_set_window_title(int id, int requester_pid, const char* title) {
    gui64_window_t* win = gui64_find_window_by_id_internal(id);

    if (!win || !title) {
        return 0;
    }

    if (win->owner_pid != requester_pid && requester_pid != 0) {
        return 0;
    }

    str_copy64(win->title, title, sizeof(win->title));
    win->chrome_drawn = 0;

    gui64_request_window_outer_damage(win);

    return 1;
}

int gui64_destroy_window(int id, int requester_pid) {
    gui64_window_t* win = gui64_find_window_by_id_internal(id);

    if (!win) {
        return 0;
    }

    if (win->owner_pid != requester_pid && requester_pid != 0) {
        return 0;
    }

    int was_active = win->active;

    if (win->buffer) {
        kfree64(win->buffer);
        win->buffer = 0;
    }

    gui64_z_remove(id);
    mem_zero64(win, sizeof(*win));

    if (id == g_gui64_shell_terminal_window_id) {
        g_gui64_shell_terminal_window_id = 0;
    }

    if (was_active || g_gui64_active_id == id) {
        g_gui64_active_id = 0;

        if (g_gui64_z_count > 0) {
            gui64_set_active_window(g_gui64_z_order[g_gui64_z_count - 1]);
        }
    }

    gui64_request_repaint();

    return 1;
}

void gui64_destroy_windows_for_pid(int owner_pid) {
    int destroyed_any = 0;

    if (owner_pid <= 0) {
        return;
    }

    for (int i = 0; i < GUI64_MAX_WINDOWS; i++) {
        gui64_window_t* win = &g_gui64_windows[i];

        if (!win->used || win->owner_pid != owner_pid) {
            continue;
        }

        int id = win->id;

        if (win->buffer) {
            kfree64(win->buffer);
            win->buffer = 0;
        }

        gui64_z_remove(id);
        mem_zero64(win, sizeof(*win));
        destroyed_any = 1;
    }

    if (!destroyed_any) {
        return;
    }

    g_gui64_active_id = 0;

    if (g_gui64_z_count > 0) {
        gui64_set_active_window(g_gui64_z_order[g_gui64_z_count - 1]);
    }

    gui64_request_repaint();
}

int gui64_pid_has_window(int pid) {
    if (pid <= 0) {
        return 0;
    }

    for (int i = 0; i < GUI64_MAX_WINDOWS; i++) {
        if (g_gui64_windows[i].used && g_gui64_windows[i].owner_pid == pid) {
            return 1;
        }
    }

    return 0;
}

int gui64_focus_pid_window(int pid) {
    if (pid <= 0) {
        return 0;
    }

    for (int i = 0; i < GUI64_MAX_WINDOWS; i++) {
        gui64_window_t* win = &g_gui64_windows[i];

        if (!win->used || win->owner_pid != pid) {
            continue;
        }

        gui64_set_active_window(win->id);

        if (g_gui64_active) {
            gui64_render();
        }

        return 1;
    }

    return 0;
}

int gui64_window_belongs_to(int id, int pid) {
    gui64_window_t* win = gui64_find_window_by_id_internal(id);

    if (!win) {
        return 0;
    }

    return win->owner_pid == pid;
}

/* -------------------------------------------------------------------------- */
/* Event API                                                                   */
/* -------------------------------------------------------------------------- */

int gui64_focused_window_id(void) {
    return g_gui64_active_id;
}

int gui64_has_focused_app_window(void) {
    gui64_window_t* win = gui64_focused_window();

    if (!win || !win->used) {
        return 0;
    }

    return win->kind == GUI64_WINDOW_APP && win->owner_pid > 0;
}

int gui64_pop_event(int window_id, int requester_pid, gui64_event_t* out) {
    gui64_window_t* win = gui64_find_window_by_id_internal(window_id);

    if (!out) {
        return 0;
    }

    out->type = GUI64_EVENT_NONE;
    out->key = 0;
    out->x = 0;
    out->y = 0;
    out->w = 0;
    out->h = 0;

    if (!win || !win->used) {
        return 0;
    }

    if (win->owner_pid != requester_pid && requester_pid != 0) {
        return 0;
    }

    if (gui64_event_queue_empty(win)) {
        return 0;
    }

    *out = win->events[win->event_head];
    win->event_head = (win->event_head + 1u) % GUI64_EVENT_QUEUE_SIZE;

    return 1;
}

int gui64_push_key_to_focused(char c) {
    gui64_window_t* win = gui64_focused_window();
    gui64_event_t ev;

    if (!win || !win->used || win->owner_pid <= 0) {
        return 0;
    }

    ev.type = GUI64_EVENT_KEY;
    ev.key = (nq_u32)(nq_u8)c;
    ev.x = 0;
    ev.y = 0;
    ev.w = 0;
    ev.h = 0;

    return gui64_push_event_to_window(win, &ev);
}

int gui64_push_close_to_focused(void) {
    gui64_window_t* win = gui64_focused_window();
    gui64_event_t ev;

    if (!win || !win->used || win->owner_pid <= 0) {
        return 0;
    }

    ev.type = GUI64_EVENT_CLOSE;
    ev.key = 0;
    ev.x = win->x;
    ev.y = win->y;
    ev.w = win->w;
    ev.h = win->h;

    return gui64_push_event_to_window(win, &ev);
}
