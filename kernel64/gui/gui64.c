#include "gui64.h"

#include "../graphics/framebuffer64.h"
#include "../k64_console.h"
#include "../mm/kmem64.h"
#include "../console/font8x8.h"
#include "../arch/x86_64/timer64.h"
#include "../arch/x86_64/io64.h"
#include "../arch/x86_64/power64.h"
#include "../proc/process64.h"
#include "../input/kinput64.h"
#include "../input/mouse64.h"
#include "../fs/vfs64.h"
#include "../exec/elf64.h"
#include "../sound/sound64.h"

/*
 * See gui64.h for the overall model. This file has, in order:
 *
 *   1. helpers, rectangles, surfaces and drawing primitives
 *   2. window storage, z-order, events
 *   3. window frame ("chrome") rendering
 *   4. damage tracking and the compositor
 *   5. desktop shell: wallpaper, icons, taskbar, start menu, clock
 *   6. window manager operations (focus, move, resize, minimise, ...)
 *   7. input: mouse and keyboard
 *   8. the desktop task and the public API
 *
 * All pixels are XRGB32 (0x00RRGGBB).
 *
 * Locking: window and compositor state is shared between the desktop task,
 * app syscalls and the shell task. Every public entry point takes the kernel
 * preemption guard (it nests), so only one of them touches the state at a
 * time.
 */

/* -------------------------------------------------------------------------- */
/* Layout constants                                                            */
/* -------------------------------------------------------------------------- */

#define GUI64_FONT_W 8
#define GUI64_FONT_H 16

#define GUI64_FRAME 4           /* frame thickness left/right/bottom          */
#define GUI64_TITLE_H 24        /* title bar height                           */
#define GUI64_CONTENT_Y 29      /* frame + title + 1px separator              */
#define GUI64_TASKBAR_H 46      /* strip that holds the floating dock         */
#define GUI64_SHADOW 6
#define GUI64_CORNER 10         /* window corner radius                       */
#define GUI64_MENU_CORNER 10

#define GUI64_MIN_WINDOW_W 160
#define GUI64_MIN_WINDOW_H 96

#define GUI64_BUTTON_W 16       /* title-bar buttons are round "orbs"         */
#define GUI64_BUTTON_H 16
#define GUI64_BUTTON_Y 8
#define GUI64_BUTTON_GAP 4

#define GUI64_RESIZE_CORNER 14

#define GUI64_FRAME_TICKS 10ull         /* desktop task period                */
#define GUI64_DOUBLE_CLICK_TICKS 450ull
#define GUI64_CLOSE_GRACE_TICKS 2000ull /* then an unresponsive app is killed */
#define GUI64_CLOCK_TICKS 1000ull

#define GUI64_MAX_DAMAGE 24
#define GUI64_MAX_APPS 16
#define GUI64_APP_NAME_MAX 32
#define GUI64_SHELL_QUEUE_SIZE 256u

#define GUI64_DOCK_MARGIN 8      /* gap between the dock and the screen edges  */
#define GUI64_DOCK_Y 4
#define GUI64_DOCK_H 38
#define GUI64_DOCK_CORNER 15

#define GUI64_TASK_START_X 12
#define GUI64_TASK_START_W 38
#define GUI64_TASK_BUTTON_X 58
#define GUI64_TASK_BUTTON_W 140
#define GUI64_TASK_BUTTON_PITCH 146
#define GUI64_TASK_CLOCK_W 62

#define GUI64_MENU_W 224
#define GUI64_MENU_HEADER_H 40
#define GUI64_MENU_ITEM_H 28
#define GUI64_MENU_FIXED_ITEMS 4

#define GUI64_ICON_CELL_W 96
#define GUI64_ICON_CELL_H 92
#define GUI64_ICON_SIZE 48
#define GUI64_ICON_ORIGIN_X 14
#define GUI64_ICON_ORIGIN_Y 18

#define GUI64_CURSOR_W 14       /* arrow plus its soft shadow                 */
#define GUI64_CURSOR_H 22
#define GUI64_CURSOR_ROWS 19

#define GUI64_MASCOT_W 124
#define GUI64_MASCOT_H 148

#define GUI64_TERMINAL_BG 0x00091C2Au

#define GUI64_LOCK()   process64_preempt_disable()
#define GUI64_UNLOCK() process64_preempt_enable()

/* -------------------------------------------------------------------------- */
/* Types                                                                       */
/* -------------------------------------------------------------------------- */

typedef struct gui64_rect {
    int x;
    int y;
    int w;
    int h;
} gui64_rect_t;

typedef struct gui64_surface {
    nq_u32* px;
    int w;
    int h;
} gui64_surface_t;

typedef struct gui64_window {
    int used;
    int id;
    int kind;
    nq_u32 flags;

    int owner_pid;
    terminal64_session_t* terminal;

    /*
     * Whole window on screen, frame included.
     */
    int x;
    int y;
    int w;
    int h;

    int active;
    int minimized;
    int maximized;

    /*
     * 1 while the window fills the left or right half of the screen after
     * being dragged to that edge; `restore` holds its size from before.
     */
    int snapped;
    gui64_rect_t restore;

    char title[32];

    /*
     * w x h image of the window: frame, title bar and content.
     */
    gui64_surface_t surface;
    int chrome_dirty;
    int content_stale;

    /*
     * Terminal windows only: the window frees the terminal session when it
     * closes (the process that created the session has already exited).
     */
    int owns_terminal;

    /*
     * Non-zero once the user asked to close the window; the tick at which
     * that happened.
     */
    nq_u64 close_requested_tick;

    gui64_event_t events[GUI64_EVENT_QUEUE_SIZE];
    nq_u32 event_head;
    nq_u32 event_tail;
} gui64_window_t;

#define GUI64_DRAG_NONE   0
#define GUI64_DRAG_MOVE   1
#define GUI64_DRAG_RESIZE 2

#define GUI64_PART_NONE     0
#define GUI64_PART_CONTENT  1
#define GUI64_PART_TITLE    2
#define GUI64_PART_CLOSE    3
#define GUI64_PART_MAXIMIZE 4
#define GUI64_PART_MINIMIZE 5
#define GUI64_PART_RESIZE   6
#define GUI64_PART_LOG      7

#define GUI64_EDGE_LEFT   1
#define GUI64_EDGE_RIGHT  2
#define GUI64_EDGE_TOP    4
#define GUI64_EDGE_BOTTOM 8

/* -------------------------------------------------------------------------- */
/* State                                                                       */
/* -------------------------------------------------------------------------- */

static gui64_window_t g_windows[GUI64_MAX_WINDOWS];

/*
 * Window ids, bottom to top.
 */
static int g_z[GUI64_MAX_WINDOWS];
static int g_z_count = 0;

static int g_next_id = 1;
static int g_active_id = 0;
static int g_terminal_window_id = 0;
static int g_cascade = 0;

static int g_active = 0;
static int g_task_started = 0;
static int g_text_mode_request = 0;

static int g_screen_w = 0;
static int g_screen_h = 0;

static gui64_surface_t g_back;      /* composed frame                         */
static gui64_surface_t g_wallpaper; /* rendered once per screen size          */
static gui64_surface_t g_desktop;   /* wallpaper + icons                      */
static gui64_surface_t g_taskbar;
static gui64_surface_t g_menu;

static int g_taskbar_dirty = 1;
static int g_menu_open = 0;
static int g_menu_hover = -1;

static gui64_rect_t g_damage[GUI64_MAX_DAMAGE];
static int g_damage_count = 0;

/*
 * Clip rectangle and target for the drawing primitives.
 */
static gui64_surface_t* g_target = 0;
static gui64_rect_t g_clip;

static int g_cursor_x = 0;
static int g_cursor_y = 0;
static int g_cursor_placed = 0;
static nq_u8 g_buttons = 0;

static int g_drag_mode = GUI64_DRAG_NONE;
static int g_drag_window = 0;
static int g_drag_dx = 0;
static int g_drag_dy = 0;

/*
 * Resize drags: which edges move, the window's rectangle when the drag
 * started and where the pointer was.
 */
static int g_drag_edges = 0;
static gui64_rect_t g_drag_origin;
static int g_drag_start_x = 0;
static int g_drag_start_y = 0;

/*
 * Edges under the pointer, set by window_part_at() when it reports
 * GUI64_PART_RESIZE.
 */
static int g_hit_edges = 0;

static nq_u64 g_console_scan_tick = 0;


/*
 * Processes that have created an app window at some point. They are
 * graphical programs: their text output stays hidden unless the user asks
 * for it, even while they briefly have no window (or after they close it
 * on the way out).
 */
#define GUI64_MAX_GUI_PIDS 32

static int g_gui_pids[GUI64_MAX_GUI_PIDS];
static gui64_rect_t g_outline;
static int g_outline_visible = 0;

/*
 * Window that received a button press in its content area; it keeps getting
 * the mouse until the button is released.
 */
static int g_capture_window = 0;

static char g_apps[GUI64_MAX_APPS][GUI64_APP_NAME_MAX];
static int g_app_count = 0;

static int g_icon_selected = -1;
static int g_last_icon_click = -1;
static nq_u64 g_last_icon_click_tick = 0;

static int g_last_title_click_window = 0;
static nq_u64 g_last_title_click_tick = 0;

static char g_clock_text[12] = "--:--";

/*
 * Desktop settings (see gui64_setting_set). Kept in /usb/NAQOS.CFG.
 */
static int g_set_wallpaper = 0;   /* GUI64_WALLPAPER_* */
static int g_set_accent = 0;      /* GUI64_ACCENT_* */
static int g_set_clock12 = 0;     /* 1 = 12-hour clock */

static void wallpaper_grade(void);
static nq_u64 g_clock_checked_tick = 0;

static char g_shell_queue[GUI64_SHELL_QUEUE_SIZE];
static nq_u32 g_shell_head = 0;
static nq_u32 g_shell_tail = 0;

/* -------------------------------------------------------------------------- */
/* Small helpers                                                               */
/* -------------------------------------------------------------------------- */

static void mem_zero64(void* ptr, nq_u64 size) {
    nq_u8* p = (nq_u8*)ptr;

    for (nq_u64 i = 0; i < size; i++) {
        p[i] = 0;
    }
}

static void str_copy64(char* dst, const char* src, nq_u64 dst_size) {
    nq_u64 i = 0;

    if (!dst || dst_size == 0) {
        return;
    }

    if (src) {
        while (src[i] && i + 1 < dst_size) {
            dst[i] = src[i];
            i++;
        }
    }

    dst[i] = 0;
}

static int str_len64(const char* s) {
    int n = 0;

    if (!s) {
        return 0;
    }

    while (s[n]) {
        n++;
    }

    return n;
}

static inline void fill32(nq_u32* dst, nq_u32 value, nq_u64 count) {
    __asm__ volatile (
        "rep stosl"
        : "+D"(dst), "+c"(count)
        : "a"(value)
        : "memory"
    );
}

static inline void copy32(nq_u32* dst, const nq_u32* src, nq_u64 count) {
    __asm__ volatile (
        "rep movsl"
        : "+D"(dst), "+S"(src), "+c"(count)
        :
        : "memory"
    );
}

static nq_u32 rgb(nq_u32 r, nq_u32 g, nq_u32 b) {
    return (r << 16) | (g << 8) | b;
}

static nq_u32 mix(nq_u32 a, nq_u32 b, int t, int max_t) {
    if (max_t <= 0) {
        return a;
    }

    nq_u32 ar = (a >> 16) & 0xFFu;
    nq_u32 ag = (a >> 8) & 0xFFu;
    nq_u32 ab = a & 0xFFu;

    nq_u32 br = (b >> 16) & 0xFFu;
    nq_u32 bg = (b >> 8) & 0xFFu;
    nq_u32 bb = b & 0xFFu;

    nq_u32 u = (nq_u32)t;
    nq_u32 m = (nq_u32)max_t;

    return rgb(
        (ar * (m - u) + br * u) / m,
        (ag * (m - u) + bg * u) / m,
        (ab * (m - u) + bb * u) / m
    );
}

/* -------------------------------------------------------------------------- */
/* Rectangles                                                                  */
/* -------------------------------------------------------------------------- */

static gui64_rect_t rect_make(int x, int y, int w, int h) {
    gui64_rect_t r;

    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;

    return r;
}

static int rect_empty(gui64_rect_t r) {
    return r.w <= 0 || r.h <= 0;
}

static gui64_rect_t rect_intersect(gui64_rect_t a, gui64_rect_t b) {
    int x0 = a.x > b.x ? a.x : b.x;
    int y0 = a.y > b.y ? a.y : b.y;
    int x1 = (a.x + a.w) < (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    int y1 = (a.y + a.h) < (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);

    if (x1 <= x0 || y1 <= y0) {
        return rect_make(0, 0, 0, 0);
    }

    return rect_make(x0, y0, x1 - x0, y1 - y0);
}

static gui64_rect_t rect_union(gui64_rect_t a, gui64_rect_t b) {
    if (rect_empty(a)) {
        return b;
    }

    if (rect_empty(b)) {
        return a;
    }

    int x0 = a.x < b.x ? a.x : b.x;
    int y0 = a.y < b.y ? a.y : b.y;
    int x1 = (a.x + a.w) > (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    int y1 = (a.y + a.h) > (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);

    return rect_make(x0, y0, x1 - x0, y1 - y0);
}

static int rect_contains(gui64_rect_t outer, gui64_rect_t inner) {
    return inner.x >= outer.x &&
           inner.y >= outer.y &&
           inner.x + inner.w <= outer.x + outer.w &&
           inner.y + inner.h <= outer.y + outer.h;
}

static int rect_has_point(gui64_rect_t r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static gui64_rect_t screen_rect(void) {
    return rect_make(0, 0, g_screen_w, g_screen_h);
}

/* -------------------------------------------------------------------------- */
/* Surfaces and drawing primitives                                             */
/* -------------------------------------------------------------------------- */

static int surface_alloc(gui64_surface_t* s, int w, int h) {
    s->px = 0;
    s->w = 0;
    s->h = 0;

    if (w <= 0 || h <= 0) {
        return 0;
    }

    s->px = (nq_u32*)kmalloc64((nq_u64)w * (nq_u64)h * sizeof(nq_u32));

    if (!s->px) {
        return 0;
    }

    s->w = w;
    s->h = h;

    return 1;
}

static void surface_free(gui64_surface_t* s) {
    if (s->px) {
        kfree64(s->px);
    }

    s->px = 0;
    s->w = 0;
    s->h = 0;
}

/*
 * Selects the surface that the primitives below draw into and resets the
 * clip rectangle to the whole surface.
 */
static void draw_to(gui64_surface_t* s) {
    g_target = s;
    g_clip = rect_make(0, 0, s ? s->w : 0, s ? s->h : 0);
}

static void draw_clip(gui64_rect_t r) {
    if (!g_target) {
        return;
    }

    g_clip = rect_intersect(r, rect_make(0, 0, g_target->w, g_target->h));
}

static void fill(int x, int y, int w, int h, nq_u32 color) {
    if (!g_target || !g_target->px) {
        return;
    }

    gui64_rect_t r = rect_intersect(rect_make(x, y, w, h), g_clip);

    if (rect_empty(r)) {
        return;
    }

    for (int row = 0; row < r.h; row++) {
        fill32(
            g_target->px + (nq_u64)(r.y + row) * (nq_u64)g_target->w + r.x,
            color,
            (nq_u64)r.w
        );
    }
}

static void border(int x, int y, int w, int h, nq_u32 color) {
    fill(x, y, w, 1, color);
    fill(x, y + h - 1, w, 1, color);
    fill(x, y, 1, h, color);
    fill(x + w - 1, y, 1, h, color);
}

static void vgradient(int x, int y, int w, int h, nq_u32 top, nq_u32 bottom) {
    for (int row = 0; row < h; row++) {
        fill(x, y + row, w, 1, mix(top, bottom, row, h > 1 ? h - 1 : 1));
    }
}

static void put_pixel(int x, int y, nq_u32 color) {
    if (x < g_clip.x || y < g_clip.y ||
        x >= g_clip.x + g_clip.w || y >= g_clip.y + g_clip.h) {
        return;
    }

    g_target->px[(nq_u64)y * (nq_u64)g_target->w + x] = color;
}

/*
 * Multiplies the pixels in a rectangle by `percent` / 100 (used for shadows).
 */
static void darken(int x, int y, int w, int h, nq_u32 percent) {
    if (!g_target || !g_target->px) {
        return;
    }

    gui64_rect_t r = rect_intersect(rect_make(x, y, w, h), g_clip);

    if (rect_empty(r)) {
        return;
    }

    for (int row = 0; row < r.h; row++) {
        nq_u32* p =
            g_target->px + (nq_u64)(r.y + row) * (nq_u64)g_target->w + r.x;

        for (int col = 0; col < r.w; col++) {
            nq_u32 c = p[col];

            p[col] = rgb(
                (((c >> 16) & 0xFFu) * percent) / 100u,
                (((c >> 8) & 0xFFu) * percent) / 100u,
                ((c & 0xFFu) * percent) / 100u
            );
        }
    }
}

/*
 * 8x8 font drawn `scale` wide and 2 * `scale` tall per font pixel, so
 * scale 1 gives the usual 8x16 cell.
 */
static void draw_char(int x, int y, char c, nq_u32 color, int scale) {
    const u8* glyph = font8x8_get(c);

    if (!g_target || !g_target->px || !glyph) {
        return;
    }

    if (x >= g_clip.x + g_clip.w || y >= g_clip.y + g_clip.h ||
        x + 8 * scale <= g_clip.x || y + 16 * scale <= g_clip.y) {
        return;
    }

    for (int gy = 0; gy < 8; gy++) {
        u8 bits = glyph[gy];

        if (!bits) {
            continue;
        }

        for (int gx = 0; gx < 8; gx++) {
            if (!(bits & (0x80u >> gx))) {
                continue;
            }

            if (scale == 1) {
                put_pixel(x + gx, y + gy * 2, color);
                put_pixel(x + gx, y + gy * 2 + 1, color);
            } else {
                fill(
                    x + gx * scale,
                    y + gy * 2 * scale,
                    scale,
                    2 * scale,
                    color
                );
            }
        }
    }
}

/*
 * Draws at most max_chars characters (all of them when max_chars < 0).
 */
static void draw_text(int x, int y, const char* text, nq_u32 color, int max_chars) {
    if (!text) {
        return;
    }

    for (int i = 0; text[i] && (max_chars < 0 || i < max_chars); i++) {
        draw_char(x + i * GUI64_FONT_W, y, text[i], color, 1);
    }
}

/*
 * Copies src to (dx, dy) on the current target, clipped.
 */
static void blit(const gui64_surface_t* src, int dx, int dy) {
    if (!g_target || !g_target->px || !src || !src->px) {
        return;
    }

    gui64_rect_t r = rect_intersect(rect_make(dx, dy, src->w, src->h), g_clip);

    if (rect_empty(r)) {
        return;
    }

    for (int row = 0; row < r.h; row++) {
        copy32(
            g_target->px + (nq_u64)(r.y + row) * (nq_u64)g_target->w + r.x,
            src->px +
                (nq_u64)(r.y + row - dy) * (nq_u64)src->w +
                (r.x - dx),
            (nq_u64)r.w
        );
    }
}

/* -------------------------------------------------------------------------- */
/* Soft drawing: blending, rounded shapes                                      */
/* -------------------------------------------------------------------------- */

/*
 * Mixes `src` over `dst` with opacity a (0 = dst, 255 = src).
 */
static inline nq_u32 blend(nq_u32 dst, nq_u32 src, nq_u32 a) {
    nq_u32 ia = 255u - a;
    nq_u32 rb = ((dst & 0x00FF00FFu) * ia + (src & 0x00FF00FFu) * a) >> 8;
    nq_u32 g = ((dst & 0x0000FF00u) * ia + (src & 0x0000FF00u) * a) >> 8;

    return (rb & 0x00FF00FFu) | (g & 0x0000FF00u);
}

static void blend_pixel(int x, int y, nq_u32 color, nq_u32 a) {
    if (x < g_clip.x || y < g_clip.y ||
        x >= g_clip.x + g_clip.w || y >= g_clip.y + g_clip.h) {
        return;
    }

    nq_u32* p = g_target->px + (nq_u64)y * (nq_u64)g_target->w + x;

    *p = blend(*p, color, a);
}

static void fill_alpha(int x, int y, int w, int h, nq_u32 color, nq_u32 a) {
    if (!g_target || !g_target->px) {
        return;
    }

    gui64_rect_t r = rect_intersect(rect_make(x, y, w, h), g_clip);

    if (rect_empty(r)) {
        return;
    }

    for (int row = 0; row < r.h; row++) {
        nq_u32* p =
            g_target->px + (nq_u64)(r.y + row) * (nq_u64)g_target->w + r.x;

        for (int col = 0; col < r.w; col++) {
            p[col] = blend(p[col], color, a);
        }
    }
}

static nq_u32 isqrt32(nq_u32 n) {
    /*
     * Bit-by-bit integer square root: 16 steps whatever the size.
     */
    nq_u32 result = 0;
    nq_u32 bit = 1u << 30;

    while (bit > n) {
        bit >>= 2;
    }

    while (bit != 0) {
        if (n >= result + bit) {
            n -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }

        bit >>= 2;
    }

    return result;
}

/*
 * How far a rounded corner of the given radius cuts in on row i, where
 * i = 0 is the outermost row. Rows at or beyond the radius are not cut.
 */
static int corner_inset(int radius, int i) {
    if (radius <= 0 || i < 0 || i >= radius) {
        return 0;
    }

    /*
     * inset = R - sqrt(R^2 - (dy + 0.5)^2), computed with everything
     * doubled to stay in integers.
     */
    int dy2 = 2 * (radius - 1 - i) + 1;
    int v = 4 * radius * radius - dy2 * dy2;

    if (v <= 0) {
        return radius;
    }

    return radius - ((int)isqrt32((nq_u32)v) + 1) / 2;
}

/*
 * Inset for row `row` of a rounded rectangle h rows tall.
 */
static int row_inset(int h, int radius, int row) {
    if (row < radius) {
        return corner_inset(radius, row);
    }

    if (row >= h - radius) {
        return corner_inset(radius, h - 1 - row);
    }

    return 0;
}

static void rrect_fill(int x, int y, int w, int h, int radius, nq_u32 color) {
    for (int row = 0; row < h; row++) {
        int inset = row_inset(h, radius, row);

        fill(x + inset, y + row, w - inset * 2, 1, color);
    }
}

static void rrect_gradient(
    int x,
    int y,
    int w,
    int h,
    int radius,
    nq_u32 top,
    nq_u32 bottom
) {
    for (int row = 0; row < h; row++) {
        int inset = row_inset(h, radius, row);

        fill(x + inset, y + row, w - inset * 2, 1,
             mix(top, bottom, row, h > 1 ? h - 1 : 1));
    }
}

static void rrect_alpha(
    int x,
    int y,
    int w,
    int h,
    int radius,
    nq_u32 color,
    nq_u32 a
) {
    for (int row = 0; row < h; row++) {
        int inset = row_inset(h, radius, row);

        fill_alpha(x + inset, y + row, w - inset * 2, 1, color, a);
    }
}

/*
 * White highlight over the top `rows` rows of a rounded rectangle that is
 * `h` rows tall, fading from a_top to a_bottom. This is the "gel" shine.
 */
static void rrect_gloss(
    int x,
    int y,
    int w,
    int h,
    int radius,
    int rows,
    nq_u32 a_top,
    nq_u32 a_bottom
) {
    for (int row = 0; row < rows && row < h; row++) {
        int inset = row_inset(h, radius, row);
        nq_u32 a =
            (a_top * (nq_u32)(rows - row) + a_bottom * (nq_u32)row) /
            (nq_u32)rows;

        fill_alpha(x + inset, y + row, w - inset * 2, 1, 0x00FFFFFFu, a);
    }
}

/*
 * Filled circle, blended.
 */
static void disc(int cx, int cy, int r, nq_u32 color, nq_u32 a) {
    for (int dy = -r; dy <= r; dy++) {
        int half = (int)isqrt32((nq_u32)(r * r - dy * dy));

        fill_alpha(cx - half, cy + dy, half * 2 + 1, 1, color, a);
    }
}

/*
 * Filled circle with a top-to-bottom gradient.
 */
static void disc_gradient(int cx, int cy, int r, nq_u32 top, nq_u32 bottom) {
    for (int dy = -r; dy <= r; dy++) {
        int half = (int)isqrt32((nq_u32)(r * r - dy * dy));

        fill(cx - half, cy + dy, half * 2 + 1, 1,
             mix(top, bottom, dy + r, r * 2 > 0 ? r * 2 : 1));
    }
}

/*
 * A glossy ball: dark rim, gradient body, shine on the upper half.
 */
static void orb(int cx, int cy, int r, nq_u32 top, nq_u32 bottom) {
    disc_gradient(cx, cy, r, mix(bottom, 0, 1, 3), mix(bottom, 0, 1, 2));
    disc_gradient(cx, cy, r - 1, top, bottom);
    disc(cx, cy - r / 3, r / 2, 0x00FFFFFFu, 110);
}

/*
 * Copies src to (dx, dy) leaving out the four rounded corners.
 */
static void blit_rounded(const gui64_surface_t* src, int dx, int dy, int radius) {
    if (!g_target || !g_target->px || !src || !src->px) {
        return;
    }

    gui64_rect_t r = rect_intersect(rect_make(dx, dy, src->w, src->h), g_clip);

    if (rect_empty(r)) {
        return;
    }

    for (int row = 0; row < r.h; row++) {
        int sy = r.y + row - dy;
        int inset = row_inset(src->h, radius, sy);

        int x0 = r.x;
        int x1 = r.x + r.w;

        if (x0 < dx + inset) {
            x0 = dx + inset;
        }

        if (x1 > dx + src->w - inset) {
            x1 = dx + src->w - inset;
        }

        if (x1 <= x0) {
            continue;
        }

        copy32(
            g_target->px + (nq_u64)(r.y + row) * (nq_u64)g_target->w + x0,
            src->px + (nq_u64)sy * (nq_u64)src->w + (x0 - dx),
            (nq_u64)(x1 - x0)
        );
    }
}

/*
 * Sine for wallpaper shapes. A full turn is 65536; the result is
 * -4096..4096. Linear interpolation over a quarter-wave table.
 */
static int isin(nq_u32 angle) {
    static const int quarter[17] = {
        0, 400, 799, 1189, 1567, 1931, 2276, 2601, 2896,
        3167, 3406, 3612, 3784, 3923, 4017, 4077, 4096
    };

    nq_u32 a = angle & 0xFFFFu;
    nq_u32 quadrant = a >> 14;
    nq_u32 pos = a & 0x3FFFu;

    if (quadrant & 1u) {
        pos = 0x4000u - pos;
    }

    nq_u32 index = pos >> 10;
    int value;

    if (index >= 16) {
        value = quarter[16];
    } else {
        int frac = (int)(pos & 1023u);

        value = quarter[index] +
            ((quarter[index + 1] - quarter[index]) * frac) / 1024;
    }

    return (quadrant & 2u) ? -value : value;
}

/*
 * Filled ellipse, blended.
 */
static void ellipse(int cx, int cy, int rx, int ry, nq_u32 color, nq_u32 a) {
    if (rx <= 0 || ry <= 0) {
        return;
    }

    for (int dy = -ry; dy <= ry; dy++) {
        int half =
            (rx * (int)isqrt32((nq_u32)((ry * ry - dy * dy) * 256))) /
            (ry * 16);

        fill_alpha(cx - half, cy + dy, half * 2 + 1, 1, color, a);
    }
}

/*
 * Filled ellipse with a top-to-bottom gradient.
 */
static void ellipse_gradient(
    int cx,
    int cy,
    int rx,
    int ry,
    nq_u32 top,
    nq_u32 bottom
) {
    if (rx <= 0 || ry <= 0) {
        return;
    }

    for (int dy = -ry; dy <= ry; dy++) {
        int half =
            (rx * (int)isqrt32((nq_u32)((ry * ry - dy * dy) * 256))) /
            (ry * 16);

        fill(cx - half, cy + dy, half * 2 + 1, 1,
             mix(top, bottom, dy + ry, ry * 2));
    }
}

/*
 * Filled triangle (small shapes: ears, petals, tips).
 */
static void triangle(
    int x0, int y0,
    int x1, int y1,
    int x2, int y2,
    nq_u32 color
) {
    int min_x = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
    int max_x = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    int min_y = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    int max_y = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);

    int area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);

    if (area == 0) {
        return;
    }

    for (int y = min_y; y <= max_y; y++) {
        for (int x = min_x; x <= max_x; x++) {
            int w0 = (x1 - x0) * (y - y0) - (y1 - y0) * (x - x0);
            int w1 = (x2 - x1) * (y - y1) - (y2 - y1) * (x - x1);
            int w2 = (x0 - x2) * (y - y2) - (y0 - y2) * (x - x2);

            if ((w0 >= 0 && w1 >= 0 && w2 >= 0) ||
                (w0 <= 0 && w1 <= 0 && w2 <= 0)) {
                put_pixel(x, y, color);
            }
        }
    }
}

/*
 * A short curved stroke: depth > 0 curves down in the middle, depth < 0 up.
 */
static void arc_stroke(int cx, int cy, int half_width, int depth, nq_u32 color) {
    if (half_width <= 0) {
        return;
    }

    for (int dx = -half_width; dx <= half_width; dx++) {
        int y =
            cy + depth -
            (depth * dx * dx) / (half_width * half_width);

        put_pixel(cx + dx, y, color);
        put_pixel(cx + dx, y + 1, color);
    }
}

/*
 * A ball of coloured glass: dark rim, glowing lower half where light
 * collects, and a bright reflection near the top. `alpha` below 255 lets
 * whatever is behind show through.
 */
static void glass_ball(
    int cx,
    int cy,
    int rx,
    int ry,
    nq_u32 color,
    nq_u32 alpha
) {
    nq_u32 dark = mix(color, 0x00001030u, 1, 2);
    nq_u32 light = mix(color, 0x00FFFFFFu, 1, 2);

    ellipse(cx, cy, rx, ry, dark, alpha);
    ellipse(cx, cy, rx - 2, ry - 2, color, alpha);

    /*
     * Light pooling in the lower half.
     */
    ellipse(cx, cy + ry / 3, (rx * 7) / 10, (ry * 5) / 10, light, 110);
    ellipse(cx, cy + ry / 2, (rx * 5) / 10, (ry * 3) / 10, 0x00FFFFFFu, 60);

    /*
     * Reflection of the sky across the top.
     */
    ellipse(cx, cy - (ry * 45) / 100, (rx * 62) / 100, (ry * 30) / 100,
            0x00FFFFFFu, 150);
    ellipse(cx - rx / 3, cy - (ry * 55) / 100, rx / 6 > 0 ? rx / 6 : 1,
            ry / 9 > 0 ? ry / 9 : 1, 0x00FFFFFFu, 230);
}

/* -------------------------------------------------------------------------- */
/* Window storage, z-order, events                                             */
/* -------------------------------------------------------------------------- */

static gui64_window_t* window_by_id(int id) {
    if (id <= 0) {
        return 0;
    }

    for (int i = 0; i < GUI64_MAX_WINDOWS; i++) {
        if (g_windows[i].used && g_windows[i].id == id) {
            return &g_windows[i];
        }
    }

    return 0;
}

static gui64_window_t* focused_window(void) {
    return window_by_id(g_active_id);
}

static int window_visible(const gui64_window_t* win) {
    return win && win->used && !win->minimized && win->surface.px;
}

static int window_borderless(const gui64_window_t* win) {
    return (win->flags & (GUI64_WIN_FLAG_BORDERLESS |
                          GUI64_WIN_FLAG_FULLSCREEN)) != 0;
}

static int window_fullscreen(const gui64_window_t* win) {
    return (win->flags & GUI64_WIN_FLAG_FULLSCREEN) != 0;
}

static gui64_rect_t window_rect(const gui64_window_t* win) {
    return rect_make(win->x, win->y, win->w, win->h);
}

/*
 * Content area in window-local coordinates.
 */
static gui64_rect_t window_content_local(const gui64_window_t* win) {
    if (window_borderless(win)) {
        return rect_make(0, 0, win->w, win->h);
    }

    return rect_make(
        GUI64_FRAME,
        GUI64_CONTENT_Y,
        win->w - GUI64_FRAME * 2,
        win->h - GUI64_CONTENT_Y - GUI64_FRAME
    );
}

static int window_has_shadow(const gui64_window_t* win) {
    return !window_borderless(win) && !win->maximized;
}

/*
 * Corner radius the window is drawn with. Maximised and borderless windows
 * are square so they meet the screen edges.
 */
static int window_corner(const gui64_window_t* win) {
    return window_has_shadow(win) ? GUI64_CORNER : 0;
}

/*
 * Part of the window that is guaranteed to cover everything below it (the
 * rounded corners are see-through).
 */
static gui64_rect_t window_opaque_rect(const gui64_window_t* win) {
    int r = window_corner(win);

    return rect_make(win->x, win->y + r, win->w, win->h - r * 2);
}

/*
 * Screen area a window can affect: itself plus its shadow.
 */
static gui64_rect_t window_damage_rect(const gui64_window_t* win) {
    if (!window_has_shadow(win)) {
        return window_rect(win);
    }

    return rect_make(
        win->x,
        win->y,
        win->w + GUI64_SHADOW,
        win->h + GUI64_SHADOW
    );
}

static void z_remove(int id) {
    int out = 0;

    for (int i = 0; i < g_z_count; i++) {
        if (g_z[i] != id) {
            g_z[out++] = g_z[i];
        }
    }

    g_z_count = out;
}

static void z_raise(int id) {
    z_remove(id);

    if (g_z_count < GUI64_MAX_WINDOWS) {
        g_z[g_z_count++] = id;
    }
}

static int event_push(gui64_window_t* win, const gui64_event_t* ev) {
    if (!win || !win->used || !ev) {
        return 0;
    }

    nq_u32 next = (win->event_tail + 1u) % GUI64_EVENT_QUEUE_SIZE;

    if (next == win->event_head) {
        /*
         * Full: drop the oldest event.
         */
        win->event_head = (win->event_head + 1u) % GUI64_EVENT_QUEUE_SIZE;
    }

    win->events[win->event_tail] = *ev;
    win->event_tail = next;

    return 1;
}

static void event_push_simple(gui64_window_t* win, nq_u32 type, nq_u32 key) {
    gui64_event_t ev;

    ev.type = type;
    ev.key = key;
    ev.x = win->x;
    ev.y = win->y;
    ev.w = win->w;
    ev.h = win->h;

    event_push(win, &ev);
}

/* -------------------------------------------------------------------------- */
/* Damage                                                                      */
/* -------------------------------------------------------------------------- */

static void damage_add(gui64_rect_t r) {
    if (!g_active) {
        return;
    }

    r = rect_intersect(r, screen_rect());

    if (rect_empty(r)) {
        return;
    }

    /*
     * Merge with any rectangle it touches, and keep merging: the union may
     * now touch others. Rectangles that do not touch stay separate so two
     * animated windows on opposite sides of the screen do not force the
     * whole area between them to be recomposed.
     */
    int merged = 1;

    while (merged) {
        merged = 0;

        for (int i = 0; i < g_damage_count; i++) {
            if (rect_empty(rect_intersect(r, g_damage[i]))) {
                continue;
            }

            r = rect_union(r, g_damage[i]);
            g_damage[i] = g_damage[--g_damage_count];
            merged = 1;
            break;
        }
    }

    if (g_damage_count >= GUI64_MAX_DAMAGE) {
        for (int i = 0; i < g_damage_count; i++) {
            r = rect_union(r, g_damage[i]);
        }

        g_damage_count = 0;
    }

    g_damage[g_damage_count++] = r;
}

static void damage_all(void) {
    damage_add(screen_rect());
}

static void damage_window(const gui64_window_t* win) {
    if (window_visible(win)) {
        damage_add(window_damage_rect(win));
    }
}

/* -------------------------------------------------------------------------- */
/* Window frame rendering                                                      */
/* -------------------------------------------------------------------------- */

static int name_is(const char* a, const char* b) {
    int i = 0;

    while (a[i] && b[i]) {
        char x = a[i];
        char y = b[i];

        if (x >= 'a' && x <= 'z') {
            x = (char)(x - 'a' + 'A');
        }

        if (y >= 'a' && y <= 'z') {
            y = (char)(y - 'a' + 'A');
        }

        if (x != y) {
            return 0;
        }

        i++;
    }

    return a[i] == 0 && b[i] == 0;
}

/*
 * 1 if `name` is `stem`, with or without an extension ("CUBE", "CUBE.ELF").
 */
static int app_is(const char* name, const char* stem) {
    char base[32];

    str_copy64(base, name, sizeof(base));

    for (int i = 0; base[i]; i++) {
        if (base[i] == '.') {
            base[i] = 0;
            break;
        }
    }

    return name_is(base, stem);
}

/*
 * Colour of an app's glass icon and its little orbs. Known apps have a
 * fixed colour; anything else picks one from its name.
 */
static nq_u32 icon_color(const char* name) {
    static const nq_u32 palette[6] = {
        0x0000A8FFu, 0x00FF8A00u, 0x0052C41Au,
        0x00965CF6u, 0x00FFB400u, 0x00FF3D7Fu
    };

    if (app_is(name, "FILES")) {
        return 0x00FFA800u;
    }

    if (app_is(name, "BROWSER") || app_is(name, "Browser")) {
        return 0x001C8CF0u;
    }

    if (app_is(name, "SETTINGS") || app_is(name, "Settings")) {
        return 0x00708AA4u;
    }

    if (app_is(name, "EDIT") || app_is(name, "Editor")) {
        return 0x00FF5C9Au;
    }

    if (app_is(name, "CUBE") || app_is(name, "3D Cube")) {
        return 0x0000A8FFu;
    }

    if (app_is(name, "3DTEST")) {
        return 0x0052C41Au;
    }

    if (app_is(name, "WOLF3D")) {
        return 0x00965CF6u;
    }

    if (app_is(name, "CRASH")) {
        return 0x00FF3048u;
    }

    if (app_is(name, "WINEVENTS")) {
        return 0x0000BFA5u;
    }

    nq_u32 hash = 0;

    for (int i = 0; name[i]; i++) {
        hash = hash * 31u + (nq_u8)name[i];
    }

    return palette[hash % 6u];
}

/*
 * The "output" button is only shown on app windows whose process has its
 * own terminal to show.
 */
static int window_has_log_button(const gui64_window_t* win) {
    process64_info_t info;

    return win->kind == GUI64_WINDOW_APP &&
           win->owner_pid > 0 &&
           process64_info_for_pid((nq_u32)win->owner_pid, &info) &&
           info.owns_terminal &&
           info.terminal != 0;
}

/*
 * Title-bar button rectangle in window-local coordinates.
 * which: GUI64_PART_CLOSE / MAXIMIZE / MINIMIZE / LOG, right to left.
 */
static gui64_rect_t window_button_rect(const gui64_window_t* win, int which) {
    int slot = 0;

    if (which == GUI64_PART_MAXIMIZE) {
        slot = 1;
    } else if (which == GUI64_PART_MINIMIZE) {
        slot = 2;
    } else if (which == GUI64_PART_LOG) {
        slot = 3;
    }

    int x =
        win->w - 10 - GUI64_BUTTON_W -
        slot * (GUI64_BUTTON_W + GUI64_BUTTON_GAP);

    return rect_make(x, GUI64_BUTTON_Y, GUI64_BUTTON_W, GUI64_BUTTON_H);
}

static void draw_title_button(gui64_rect_t r, int which, int active) {
    nq_u32 top;
    nq_u32 bottom;

    if (which == GUI64_PART_CLOSE) {
        top = active ? rgb(255, 176, 160) : rgb(226, 190, 188);
        bottom = active ? rgb(226, 52, 60) : rgb(168, 120, 124);
    } else if (which == GUI64_PART_MAXIMIZE) {
        top = active ? rgb(206, 250, 150) : rgb(204, 226, 190);
        bottom = active ? rgb(70, 178, 70) : rgb(120, 156, 124);
    } else if (which == GUI64_PART_MINIMIZE) {
        top = active ? rgb(255, 240, 150) : rgb(228, 222, 190);
        bottom = active ? rgb(240, 160, 30) : rgb(170, 150, 110);
    } else {
        top = active ? rgb(190, 240, 255) : rgb(200, 224, 232);
        bottom = active ? rgb(40, 140, 214) : rgb(110, 146, 170);
    }

    int cx = r.x + r.w / 2;
    int cy = r.y + r.h / 2;

    orb(cx, cy, r.w / 2, top, bottom);

    nq_u32 ink = 0x00FFFFFFu;

    if (which == GUI64_PART_CLOSE) {
        for (int i = -2; i <= 2; i++) {
            put_pixel(cx + i, cy + i, ink);
            put_pixel(cx + i + 1, cy + i, ink);
            put_pixel(cx + i, cy - i, ink);
            put_pixel(cx + i + 1, cy - i, ink);
        }
    } else if (which == GUI64_PART_MAXIMIZE) {
        border(cx - 3, cy - 3, 7, 6, ink);
        fill(cx - 3, cy - 3, 7, 2, ink);
    } else if (which == GUI64_PART_MINIMIZE) {
        fill(cx - 3, cy + 1, 7, 2, ink);
    } else {
        fill(cx - 3, cy - 3, 7, 1, ink);
        fill(cx - 3, cy, 7, 1, ink);
        fill(cx - 3, cy + 3, 5, 1, ink);
    }
}

/*
 * Paints the whole frame. Called with the clip set to one band of the
 * frame at a time so the content area is never touched.
 */
static void chrome_paint(gui64_window_t* win) {
    int w = win->w;
    int h = win->h;
    int active = win->active;
    int radius = window_corner(win);

    nq_u32 edge = active ? rgb(10, 78, 150) : rgb(98, 122, 144);
    nq_u32 shine = active ? rgb(255, 255, 255) : rgb(238, 246, 248);
    nq_u32 frame_top = active ? rgb(190, 232, 255) : rgb(214, 226, 236);
    nq_u32 frame_bottom = active ? rgb(52, 150, 232) : rgb(150, 176, 198);

    nq_u32 title_top = active ? rgb(214, 242, 255) : rgb(226, 234, 240);
    nq_u32 title_mid = active ? rgb(92, 186, 250) : rgb(176, 196, 212);
    nq_u32 title_bottom = active ? rgb(18, 116, 218) : rgb(126, 152, 176);

    /*
     * Glass frame: dark outline, bright inner line, aqua body.
     */
    rrect_fill(0, 0, w, h, radius, edge);
    rrect_fill(1, 1, w - 2, h - 2, radius > 0 ? radius - 1 : 0, shine);
    rrect_gradient(2, 2, w - 4, h - 4, radius > 1 ? radius - 2 : 0,
                   frame_top, frame_bottom);

    /*
     * Title bar: two-part gradient that follows the rounded top corners,
     * with a bright shine across the upper half.
     */
    int title_rows = GUI64_CONTENT_Y - 3;
    int half = title_rows / 2;

    for (int row = 0; row < title_rows; row++) {
        int y = 2 + row;
        int inset = row_inset(h, radius, y);

        if (inset < 2) {
            inset = 2;
        }

        nq_u32 color = row < half ?
            mix(title_top, title_mid, row, half) :
            mix(title_mid, title_bottom, row - half, title_rows - half);

        fill(inset, y, w - inset * 2, 1, color);

        if (row < half) {
            fill_alpha(inset, y, w - inset * 2, 1, 0x00FFFFFFu,
                       (nq_u32)(120 - (row * 90) / half));
        }
    }

    fill(GUI64_FRAME - 1, GUI64_CONTENT_Y - 1,
         w - (GUI64_FRAME - 1) * 2, 1, edge);

    /*
     * Thin dark line around the content so it sits "inside" the glass.
     */
    border(GUI64_FRAME - 1, GUI64_CONTENT_Y - 1,
           w - (GUI64_FRAME - 1) * 2,
           h - GUI64_CONTENT_Y - GUI64_FRAME + 2, edge);

    /*
     * Little app orb, then the title with a soft shadow.
     */
    nq_u32 dot = win->kind == GUI64_WINDOW_TERMINAL ?
        rgb(30, 70, 110) : icon_color(win->title);

    orb(15, 15, 6, mix(dot, 0x00FFFFFFu, 1, 2), dot);

    gui64_rect_t leftmost = window_button_rect(
        win,
        window_has_log_button(win) ? GUI64_PART_LOG : GUI64_PART_MINIMIZE
    );

    int max_chars = (leftmost.x - 6 - 27) / GUI64_FONT_W;

    if (max_chars > 0) {
        /*
         * White text with a deep blue shadow, like etched glass.
         */
        draw_text(28, 9, win->title,
                  active ? rgb(6, 62, 132) : rgb(84, 106, 128), max_chars);
        draw_text(27, 8, win->title, 0x00FFFFFFu, max_chars);
    }

    draw_title_button(window_button_rect(win, GUI64_PART_CLOSE),
                      GUI64_PART_CLOSE, active);
    draw_title_button(window_button_rect(win, GUI64_PART_MAXIMIZE),
                      GUI64_PART_MAXIMIZE, active);
    draw_title_button(window_button_rect(win, GUI64_PART_MINIMIZE),
                      GUI64_PART_MINIMIZE, active);

    if (window_has_log_button(win)) {
        draw_title_button(window_button_rect(win, GUI64_PART_LOG),
                          GUI64_PART_LOG, active);
    }
}

/*
 * Draws the frame and title bar into the window surface. The content area
 * is left untouched.
 */
/*
 * Accent colour.
 *
 * The window frames, dock and start menu are painted in blue glass. Any
 * other accent is produced afterwards by re-mapping those pixels: a pixel
 * counts as "blue glass" when blue is its strongest channel and red its
 * weakest, and its three channels are then swapped around to give the same
 * shading in another hue. Whites and greys are unchanged by this, and
 * so are the coloured title buttons and app orbs (which are not
 * blue-dominant), so the close button stays red.
 */
static nq_u32 accent_pixel(nq_u32 c) {
    nq_u32 r = (c >> 16) & 0xFFu;
    nq_u32 g = (c >> 8) & 0xFFu;
    nq_u32 b = c & 0xFFu;

    if (!(b > g && g >= r)) {
        return c;
    }

    nq_u32 top = c & 0xFF000000u;

    switch (g_set_accent) {
        case GUI64_ACCENT_TEAL:
            return top | rgb(r, b, (g + b) / 2u);

        case GUI64_ACCENT_GREEN:
            return top | rgb(r, b, g);

        case GUI64_ACCENT_PURPLE:
            return top | rgb(g, r, b);

        case GUI64_ACCENT_PINK:
            return top | rgb(b, r, g);

        case GUI64_ACCENT_ORANGE:
            return top | rgb(b, g, r);

        case GUI64_ACCENT_GRAPHITE: {
            nq_u32 v = (r + g + b) / 3u;

            return top | rgb(v, v, v + (255u - v) / 10u);
        }

        default:
            return c;
    }
}

static void accent_apply(gui64_surface_t* s, gui64_rect_t r) {
    if (g_set_accent == GUI64_ACCENT_BLUE || !s || !s->px) {
        return;
    }

    int x0 = r.x < 0 ? 0 : r.x;
    int y0 = r.y < 0 ? 0 : r.y;
    int x1 = r.x + r.w > s->w ? s->w : r.x + r.w;
    int y1 = r.y + r.h > s->h ? s->h : r.y + r.h;

    for (int y = y0; y < y1; y++) {
        nq_u32* row = s->px + (nq_u64)y * (nq_u64)s->w;

        for (int x = x0; x < x1; x++) {
            row[x] = accent_pixel(row[x]);
        }
    }
}

static void window_draw_chrome(gui64_window_t* win) {
    win->chrome_dirty = 0;

    if (!win->surface.px || window_borderless(win)) {
        return;
    }

    int w = win->w;
    int h = win->h;

    gui64_rect_t bands[4];

    bands[0] = rect_make(0, 0, w, GUI64_CONTENT_Y);
    bands[1] = rect_make(0, GUI64_CONTENT_Y, GUI64_FRAME, h - GUI64_CONTENT_Y);
    bands[2] = rect_make(w - GUI64_FRAME, GUI64_CONTENT_Y,
                         GUI64_FRAME, h - GUI64_CONTENT_Y);
    bands[3] = rect_make(0, h - GUI64_FRAME, w, GUI64_FRAME);

    draw_to(&win->surface);

    for (int i = 0; i < 4; i++) {
        draw_clip(bands[i]);
        chrome_paint(win);
    }

    /*
     * The bands overlap at the corners; recolour each pixel once.
     */
    accent_apply(&win->surface, bands[0]);
    accent_apply(&win->surface, bands[3]);
    accent_apply(&win->surface,
                 rect_make(0, GUI64_CONTENT_Y, GUI64_FRAME,
                           h - GUI64_CONTENT_Y - GUI64_FRAME));
    accent_apply(&win->surface,
                 rect_make(w - GUI64_FRAME, GUI64_CONTENT_Y, GUI64_FRAME,
                           h - GUI64_CONTENT_Y - GUI64_FRAME));
}

static void window_fill_content(gui64_window_t* win, nq_u32 color) {
    gui64_rect_t c = window_content_local(win);

    draw_to(&win->surface);
    fill(c.x, c.y, c.w, c.h, color);
}

/*
 * Redraws a terminal window's text from its terminal session.
 */
static void window_render_terminal(gui64_window_t* win) {
    if (!win->terminal || !win->surface.px) {
        return;
    }

    gui64_rect_t c = window_content_local(win);

    if (c.w <= 0 || c.h <= 0) {
        return;
    }

    terminal64_render_into_buffer(
        win->terminal,
        win->surface.px,
        (nq_u32)win->surface.w,
        (nq_u32)win->surface.h,
        c.x,
        c.y,
        c.w,
        c.h
    );

    win->content_stale = 0;

    if (window_visible(win)) {
        damage_add(rect_make(win->x + c.x, win->y + c.y, c.w, c.h));
    }
}

/* -------------------------------------------------------------------------- */
/* Desktop shell: wallpaper, icons                                             */
/* -------------------------------------------------------------------------- */

static void app_label(const char* name, char* out, nq_u64 out_size) {
    str_copy64(out, name, out_size);

    /*
     * Drop the extension for display.
     */
    for (int i = str_len64(out) - 1; i > 0; i--) {
        if (out[i] == '.') {
            out[i] = 0;
            break;
        }
    }
}

static gui64_rect_t icon_cell_rect(int index) {
    int rows = (g_screen_h - GUI64_TASKBAR_H - GUI64_ICON_ORIGIN_Y) /
               GUI64_ICON_CELL_H;

    if (rows < 1) {
        rows = 1;
    }

    return rect_make(
        GUI64_ICON_ORIGIN_X + (index / rows) * GUI64_ICON_CELL_W,
        GUI64_ICON_ORIGIN_Y + (index % rows) * GUI64_ICON_CELL_H,
        GUI64_ICON_CELL_W,
        GUI64_ICON_CELL_H
    );
}

/*
 * A rounded, glossy "gel" tile: the basic Aero icon shape.
 */
/*
 * White pictogram on an app's glass orb. Known apps get a symbol; the rest
 * show their initial.
 */
static void draw_icon_glyph(int cx, int cy, const char* label, nq_u32 base) {
    nq_u32 white = 0x00FFFFFFu;
    nq_u32 shade = mix(base, 0x00001030u, 1, 2);

    if (name_is(label, "BROWSER")) {
        /*
         * Globe: a disc with an equator, two parallels and a meridian.
         */
        disc(cx, cy, 12, white, 255);
        ellipse(cx, cy, 6, 12, shade, 255);
        ellipse(cx, cy, 4, 11, white, 255);
        fill(cx, cy - 12, 1, 24, shade);
        fill(cx - 12, cy, 24, 1, shade);
        fill(cx - 10, cy - 6, 20, 1, shade);
        fill(cx - 10, cy + 6, 20, 1, shade);
        return;
    }

    if (name_is(label, "SETTINGS")) {
        /*
         * Gear: a ring with eight teeth.
         */
        static const int tooth[8][2] = {
            { 0, -12 }, { 9, -9 }, { 12, 0 }, { 9, 9 },
            { 0, 12 }, { -9, 9 }, { -12, 0 }, { -9, -9 }
        };

        for (int i = 0; i < 8; i++) {
            disc(cx + tooth[i][0], cy + tooth[i][1], 3, white, 255);
        }

        disc(cx, cy, 10, white, 255);
        disc(cx, cy, 4, shade, 255);
        return;
    }

    if (name_is(label, "EDIT")) {
        /*
         * Sheet of paper with lines of writing.
         */
        rrect_fill(cx - 9, cy - 11, 20, 25, 3, shade);
        rrect_fill(cx - 10, cy - 12, 20, 24, 3, white);

        for (int i = 0; i < 4; i++) {
            fill(cx - 6, cy - 7 + i * 5, i == 3 ? 7 : 12, 2, shade);
        }

        return;
    }

    if (name_is(label, "FILES")) {
        /*
         * Folder.
         */
        rrect_fill(cx - 12, cy - 9, 11, 6, 2, shade);
        rrect_fill(cx - 13, cy - 6, 27, 18, 3, shade);
        rrect_fill(cx - 12, cy - 10, 10, 6, 2, white);
        rrect_gradient(cx - 12, cy - 6, 25, 16, 3, white, rgb(255, 236, 190));
        return;
    }

    if (name_is(label, "CUBE") || name_is(label, "3DTEST")) {
        /*
         * A cube seen from a corner: top, left and right faces.
         */
        int filled = name_is(label, "3DTEST");

        triangle(cx, cy - 13, cx - 12, cy - 6, cx, cy + 1, white);
        triangle(cx, cy - 13, cx + 12, cy - 6, cx, cy + 1, white);

        triangle(cx - 12, cy - 5, cx - 1, cy + 2, cx - 12, cy + 8,
                 filled ? rgb(214, 244, 190) : rgb(190, 230, 255));
        triangle(cx - 1, cy + 2, cx - 1, cy + 14, cx - 12, cy + 8,
                 filled ? rgb(214, 244, 190) : rgb(190, 230, 255));

        triangle(cx + 12, cy - 5, cx + 1, cy + 2, cx + 12, cy + 8,
                 filled ? rgb(170, 226, 140) : rgb(140, 206, 250));
        triangle(cx + 1, cy + 2, cx + 1, cy + 14, cx + 12, cy + 8,
                 filled ? rgb(170, 226, 140) : rgb(140, 206, 250));
        return;
    }

    if (name_is(label, "WOLF3D")) {
        /*
         * Brick wall.
         */
        for (int row = 0; row < 4; row++) {
            int y = cy - 11 + row * 6;
            int offset = (row & 1) ? -5 : 0;

            for (int col = -1; col < 3; col++) {
                int x = cx - 12 + offset + col * 10;
                int x0 = x < cx - 12 ? cx - 12 : x;
                int x1 = x + 9 > cx + 13 ? cx + 13 : x + 9;

                if (x1 > x0) {
                    fill(x0, y, x1 - x0, 5, white);
                }
            }
        }

        return;
    }

    if (name_is(label, "CRASH")) {
        /*
         * Warning triangle.
         */
        triangle(cx, cy - 13, cx - 14, cy + 11, cx + 14, cy + 11, white);
        fill(cx - 1, cy - 5, 3, 9, base);
        fill(cx - 1, cy + 6, 3, 3, base);
        return;
    }

    if (name_is(label, "WINEVENTS")) {
        /*
         * Pointer arrow.
         */
        triangle(cx - 8, cy - 13, cx - 8, cy + 9, cx + 8, cy + 3, white);
        triangle(cx - 2, cy + 2, cx + 3, cy + 13, cx + 7, cy + 11, white);
        triangle(cx - 2, cy + 2, cx + 2, cy, cx + 7, cy + 11, white);
        return;
    }

    draw_char(cx - 7, cy - 15, label[0], shade, 2);
    draw_char(cx - 8, cy - 16, label[0], white, 2);
}

static void desktop_draw_icon(int index) {
    char label[GUI64_APP_NAME_MAX];
    gui64_rect_t cell = icon_cell_rect(index);

    app_label(g_apps[index], label, sizeof(label));

    if (index == g_icon_selected) {
        rrect_alpha(cell.x + 3, cell.y + 1, cell.w - 6, cell.h - 3, 12,
                    0x00FFFFFFu, 80);
        rrect_alpha(cell.x + 4, cell.y + 2, cell.w - 8, (cell.h - 5) / 2, 11,
                    0x00FFFFFFu, 60);
    }

    int radius = GUI64_ICON_SIZE / 2;
    int cx = cell.x + cell.w / 2;
    int cy = cell.y + 8 + radius;
    nq_u32 base = icon_color(g_apps[index]);

    /*
     * Shadow on the ground under the orb, then the glass orb itself.
     */
    ellipse(cx, cy + radius + 1, radius - 4, 4, rgb(0, 40, 30), 70);
    glass_ball(cx, cy, radius, radius, base, 255);

    draw_to(&g_desktop);
    draw_icon_glyph(cx, cy + 2, label, base);

    /*
     * Second, softer reflection over the pictogram so it sits inside the
     * glass rather than on top of it.
     */
    ellipse(cx, cy - (radius * 50) / 100, (radius * 58) / 100,
            (radius * 26) / 100, 0x00FFFFFFu, 70);

    int len = str_len64(label);
    int max_chars = (cell.w - 4) / GUI64_FONT_W;

    if (len > max_chars) {
        len = max_chars;
    }

    int tx = cell.x + (cell.w - len * GUI64_FONT_W) / 2;
    int ty = cy + radius + 8;

    draw_text(tx + 1, ty + 1, label, rgb(0, 44, 96), len);
    draw_text(tx, ty, label, 0x00FFFFFFu, len);
}

static nq_u32 wallpaper_random(nq_u32* seed) {
    *seed = *seed * 1664525u + 1013904223u;

    return *seed >> 8;
}

/*
 * A soap bubble: almost clear body, rainbow rim, strong reflections.
 */
static void draw_bubble(int cx, int cy, int r) {
    int r2 = r * r;
    int rim = (r - 2) * (r - 2);
    int inner = (r - 6) * (r - 6);

    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            int d2 = dx * dx + dy * dy;

            if (d2 > r2) {
                continue;
            }

            nq_u32 color = 0x00FFFFFFu;
            nq_u32 a = 6u + (nq_u32)((30 * d2) / (r2 > 0 ? r2 : 1));

            if (d2 >= rim) {
                /*
                 * Thin-film colours around the edge.
                 */
                if (dx < 0) {
                    color = dy < 0 ? rgb(170, 240, 255) : rgb(190, 255, 210);
                } else {
                    color = dy < 0 ? rgb(255, 200, 250) : rgb(255, 250, 180);
                }

                a = 190;
            } else if (d2 >= inner) {
                a = 60;
            }

            blend_pixel(cx + dx, cy + dy, color, a);
        }
    }

    ellipse(cx - (r * 34) / 100, cy - (r * 42) / 100, r / 3, r / 5,
            0x00FFFFFFu, 120);
    disc(cx - (r * 44) / 100, cy - (r * 48) / 100, r / 9 > 0 ? r / 9 : 1,
         0x00FFFFFFu, 240);
    ellipse(cx + (r * 36) / 100, cy + (r * 46) / 100, r / 4, r / 9 > 0 ? r / 9 : 1,
            0x00FFFFFFu, 70);
}

/*
 * Cumulus cloud: a soft halo, a body of solid white puffs that sit on a
 * flat base, and blue-grey shading underneath.
 */
static void draw_soft_cloud(int cx, int cy, int s, nq_u32* seed) {
    /*
     * Puff layout along the cloud: offset from the centre and size, in
     * tenths of s. Tallest in the middle, smaller toward the ends.
     */
    static const int puffs[7][3] = {
        { -22,  2,  8 }, { -13, -4, 11 }, { -3, -9, 14 }, { 8, -6, 12 },
        {  17, -1,  9 }, {  -7,  3, 10 }, {  6,  4, 10 }
    };

    /*
     * Halo: faint, slightly larger copies give the edge a soft falloff.
     */
    for (int i = 0; i < 7; i++) {
        int px = cx + (puffs[i][0] * s) / 10;
        int py = cy + (puffs[i][1] * s) / 10;
        int pr = (puffs[i][2] * s) / 10;

        disc(px, py, pr + s / 3, 0x00FFFFFFu, 34);
        disc(px, py, pr + s / 6, 0x00FFFFFFu, 60);
    }

    /*
     * Body.
     */
    for (int i = 0; i < 7; i++) {
        int px = cx + (puffs[i][0] * s) / 10;
        int py = cy + (puffs[i][1] * s) / 10;
        int pr = (puffs[i][2] * s) / 10;

        disc_gradient(px, py, pr, 0x00FFFFFFu, rgb(226, 240, 255));
    }

    /*
     * A few small extra puffs so no two clouds look the same.
     */
    for (int i = 0; i < 5; i++) {
        int ox = (int)(wallpaper_random(seed) % (nq_u32)(s * 4 + 1)) - s * 2;
        int oy = (int)(wallpaper_random(seed) % (nq_u32)(s / 2 + 1)) - s / 2;

        disc(cx + ox, cy + oy, s / 2 + (int)(wallpaper_random(seed) % (nq_u32)(s / 3 + 1)),
             0x00FFFFFFu, 255);
    }

    /*
     * Flat, shaded underside.
     */
    ellipse(cx, cy + (s * 9) / 10, (s * 26) / 10, s / 2, rgb(150, 196, 244), 120);
    ellipse(cx, cy + (s * 6) / 10, (s * 24) / 10, s / 2, 0x00FFFFFFu, 140);
}

/*
 * Goldfish. dir is +1 facing right, -1 facing left.
 */
static void draw_goldfish(int cx, int cy, int s, int dir) {
    nq_u32 fin = rgb(255, 130, 20);
    nq_u32 fin_light = rgb(255, 196, 96);

    /*
     * Forked tail behind the body.
     */
    triangle(cx - dir * (s * 3) / 2, cy,
             cx - dir * s * 3, cy - (s * 14) / 10,
             cx - dir * (s * 26) / 10, cy + s / 4, fin);
    triangle(cx - dir * (s * 3) / 2, cy,
             cx - dir * s * 3, cy + (s * 14) / 10,
             cx - dir * (s * 26) / 10, cy - s / 4, fin);
    triangle(cx - dir * (s * 3) / 2, cy,
             cx - dir * (s * 27) / 10, cy - (s * 8) / 10,
             cx - dir * (s * 24) / 10, cy, fin_light);

    /*
     * Dorsal and belly fins.
     */
    triangle(cx - dir * s / 2, cy - s, cx + dir * s / 2, cy - (s * 9) / 10,
             cx - dir * s, cy - (s * 19) / 10, fin);
    triangle(cx - dir * s / 4, cy + (s * 8) / 10, cx + dir * s / 2, cy + (s * 7) / 10,
             cx - dir * s / 2, cy + (s * 15) / 10, fin);

    ellipse_gradient(cx, cy, s * 2, (s * 11) / 10,
                     rgb(255, 210, 90), rgb(240, 96, 8));
    ellipse(cx + dir * s / 4, cy + s / 3, (s * 12) / 10, s / 3,
            rgb(255, 236, 170), 120);
    ellipse(cx, cy - s / 2, (s * 12) / 10, s / 4, 0x00FFFFFFu, 130);

    disc(cx + dir * (s * 13) / 10, cy - s / 5, s / 3 > 1 ? s / 3 : 2,
         0x00FFFFFFu, 255);
    disc(cx + dir * (s * 14) / 10, cy - s / 5, s / 6 > 0 ? s / 6 : 1,
         rgb(20, 20, 30), 255);
}

/*
 * Crest of the far and near hills at column x.
 */
static int hill_far_top(int x) {
    int w = g_screen_w;
    int h = g_screen_h;
    int horizon = (h * 62) / 100;
    nq_u32 t = (nq_u32)(((nq_u64)x << 16) / (nq_u64)w);

    return horizon - h / 30 -
           (isin(t + 9000u) * (h / 26)) / 4096 -
           (isin(t * 2u + 21000u) * (h / 70)) / 4096;
}

static int hill_near_top(int x) {
    int w = g_screen_w;
    int h = g_screen_h;
    int horizon = (h * 62) / 100;
    nq_u32 t = (nq_u32)(((nq_u64)x << 16) / (nq_u64)w);

    return horizon + h / 9 -
           (isin((t * 3u) / 2u + 34000u) * (h / 14)) / 4096 -
           (isin(t * 3u + 5000u) * (h / 80)) / 4096;
}

/*
 * Column fill for a grassy hill: bright along the crest, deep green below.
 */
static void draw_hill_column(int x, int top, nq_u32 crest, nq_u32 base) {
    if (top < 0) {
        top = 0;
    }

    int span = g_screen_h - top;

    if (span <= 0) {
        return;
    }

    for (int y = top; y < g_screen_h; y++) {
        nq_u32 c = mix(crest, base, y - top, span);

        if (y - top < 3) {
            c = mix(c, 0x00FFFFFFu, 3 - (y - top), 7);
        }

        g_wallpaper.px[(nq_u64)y * (nq_u64)g_wallpaper.w + x] = c;
    }
}

/*
 * Wallpaper: a saturated blue sky with soft clouds and a sun flare, a hazy
 * skyline, a river winding through bright grass, glass bubbles and a pair
 * of goldfish. Rendered once and cached.
 */
static void wallpaper_render(void) {
    if (!g_wallpaper.px) {
        return;
    }

    int w = g_screen_w;
    int h = g_screen_h;
    int horizon = (h * 62) / 100;
    nq_u32 seed = 0x0BADF00Du;

    draw_to(&g_wallpaper);

    /*
     * Sky: deep blue overhead, bright cyan toward the horizon.
     */
    nq_u32 sky_top = rgb(8, 84, 214);
    nq_u32 sky_mid = rgb(40, 150, 248);
    nq_u32 sky_low = rgb(170, 232, 255);

    for (int y = 0; y < h; y++) {
        nq_u32 c;

        if (y < horizon / 2) {
            c = mix(sky_top, sky_mid, y, horizon / 2);
        } else if (y < horizon) {
            c = mix(sky_mid, sky_low, y - horizon / 2, horizon - horizon / 2);
        } else {
            c = sky_low;
        }

        fill(0, y, w, 1, c);
    }

    /*
     * Sun flare, upper right: layered glow, a bright core and a few
     * streaks of light.
     */
    int sun_x = (w * 86) / 100;
    int sun_y = (h * 12) / 100;

    for (int r = (h * 50) / 100; r > 10; r -= h / 36 + 1) {
        disc(sun_x, sun_y, r, 0x00FFFFFFu, 8);
    }

    disc(sun_x, sun_y, h / 16, 0x00FFFFFFu, 110);
    disc(sun_x, sun_y, h / 28, 0x00FFFFFFu, 255);

    for (int k = 0; k < 8; k++) {
        nq_u32 angle = (nq_u32)(k * 8192 + 2000);

        for (int dist = h / 20; dist < h / 4; dist++) {
            int px = sun_x + (isin(angle) * dist) / 4096;
            int py = sun_y - (isin(angle + 16384u) * dist) / 4096;

            blend_pixel(px, py, 0x00FFFFFFu,
                        (nq_u32)(70 - (dist * 60) / (h / 4)));
        }
    }

    /*
     * Clouds.
     */
    draw_soft_cloud((w * 16) / 100, (h * 20) / 100, h / 22, &seed);
    draw_soft_cloud((w * 44) / 100, (h * 11) / 100, h / 30, &seed);
    draw_soft_cloud((w * 63) / 100, (h * 30) / 100, h / 20, &seed);
    draw_soft_cloud((w * 32) / 100, (h * 40) / 100, h / 34, &seed);
    draw_soft_cloud((w * 94) / 100, (h * 44) / 100, h / 24, &seed);

    /*
     * Skyline in the haze along the horizon.
     */
    for (int x = (w * 40) / 100; x < (w * 98) / 100; ) {
        int bw = w / 70 + (int)(wallpaper_random(&seed) % (nq_u32)(w / 60 + 1));
        int bh = h / 40 + (int)(wallpaper_random(&seed) % (nq_u32)(h / 9 + 1));
        int base = hill_far_top(x) + 8;

        fill_alpha(x, base - bh, bw, bh, rgb(110, 170, 232), 90);
        fill_alpha(x, base - bh, bw / 3 + 1, bh, 0x00FFFFFFu, 30);

        x += bw + (int)(wallpaper_random(&seed) % 6u);
    }

    /*
     * Far hill.
     */
    for (int x = 0; x < w; x++) {
        draw_hill_column(x, hill_far_top(x),
                         rgb(140, 232, 60), rgb(40, 160, 40));
    }

    /*
     * River winding across the far hill, wider toward the viewer on the
     * left, with a bright edge and glints on the water.
     */
    for (int x = 0; x < (w * 70) / 100; x++) {
        nq_u32 t = (nq_u32)(((nq_u64)x << 16) / (nq_u64)w);
        int cy = horizon + h / 14 + (isin(t * 2u + 12000u) * (h / 40)) / 4096;
        int half = (((w * 70) / 100 - x) * (h / 14)) / w;

        if (half < 1) {
            continue;
        }

        for (int y = cy - half; y <= cy + half; y++) {
            if (y < 0 || y >= h) {
                continue;
            }

            g_wallpaper.px[(nq_u64)y * (nq_u64)w + x] =
                mix(rgb(20, 120, 240), rgb(120, 214, 255),
                    y - (cy - half), half * 2 + 1);
        }

        blend_pixel(x, cy - half, 0x00FFFFFFu, 150);
        blend_pixel(x, cy - half + 1, 0x00FFFFFFu, 70);
        blend_pixel(x, cy + half, rgb(0, 60, 150), 90);
    }

    for (int i = 0; i < 40; i++) {
        int gx = (int)(wallpaper_random(&seed) % (nq_u32)((w * 66) / 100));
        nq_u32 t = (nq_u32)(((nq_u64)gx << 16) / (nq_u64)w);
        int cy = horizon + h / 14 + (isin(t * 2u + 12000u) * (h / 40)) / 4096;
        int half = (((w * 70) / 100 - gx) * (h / 14)) / w;
        int gy = cy - half / 2 + (int)(wallpaper_random(&seed) % (nq_u32)(half + 1));

        fill_alpha(gx, gy, 6 + (int)(wallpaper_random(&seed) % 14u), 1,
                   0x00FFFFFFu, 120);
    }

    /*
     * Near hill, then blades of grass over it: thousands of short strokes
     * in different greens, longer toward the bottom of the screen.
     */
    for (int x = 0; x < w; x++) {
        draw_hill_column(x, hill_near_top(x),
                         rgb(170, 244, 40), rgb(30, 150, 26));
    }

    for (int i = 0; i < (w * h) / 260; i++) {
        int bx = (int)(wallpaper_random(&seed) % (nq_u32)w);
        int top = hill_near_top(bx);
        int room = h - top;

        if (room <= 4) {
            continue;
        }

        int depth = (int)(wallpaper_random(&seed) % (nq_u32)room);
        int by = top + 2 + depth;
        int len = 4 + (depth * 16) / (room > 0 ? room : 1);
        int lean = (int)(wallpaper_random(&seed) % 7u) - 3;
        nq_u32 shade = wallpaper_random(&seed) % 100u;
        nq_u32 color = mix(rgb(214, 255, 90), rgb(18, 120, 20), shade, 100);

        for (int k = 0; k < len; k++) {
            int px = bx + (lean * k) / len;
            int py = by - k;

            if (py >= 0 && py < h && px >= 0 && px < w) {
                g_wallpaper.px[(nq_u64)py * (nq_u64)w + px] = color;

                if (len > 10 && px + 1 < w) {
                    g_wallpaper.px[(nq_u64)py * (nq_u64)w + px + 1] = color;
                }
            }
        }
    }

    /*
     * Small wild flowers scattered in the grass.
     */
    for (int i = 0; i < 70; i++) {
        int fx = (int)(wallpaper_random(&seed) % (nq_u32)w);
        int top = hill_near_top(fx);
        int room = h - top - 20;

        if (room <= 4) {
            continue;
        }

        int depth = (int)(wallpaper_random(&seed) % (nq_u32)room);
        int size = 1 + (depth * 3) / room;
        nq_u32 petal = (wallpaper_random(&seed) & 1u) ?
            rgb(255, 232, 60) : 0x00FFFFFFu;

        disc(fx, top + 14 + depth, size + 1, petal, 255);
        disc(fx, top + 14 + depth, size > 1 ? size - 1 : 1, rgb(255, 170, 20), 255);
    }

    /*
     * Goldfish: one drifting inside a big bubble, one loose over the grass.
     */
    int bowl_x = (w * 30) / 100;
    int bowl_y = (h * 16) / 100;
    int bowl_r = h / 11;

    draw_goldfish(bowl_x, bowl_y + bowl_r / 8, bowl_r / 5, 1);
    draw_bubble(bowl_x, bowl_y, bowl_r);

    draw_goldfish((w * 70) / 100, (h * 58) / 100, h / 44, -1);

    /*
     * Bubbles, large and small.
     */
    seed = 0x02F6E2B1u;

    for (int i = 0; i < 15; i++) {
        int bx = (int)(wallpaper_random(&seed) % (nq_u32)w);
        int by = (int)(wallpaper_random(&seed) % (nq_u32)((h * 82) / 100));
        int br = 9 + (int)(wallpaper_random(&seed) % (nq_u32)(h / 12 + 1));

        /*
         * Keep clear of the fish bowl.
         */
        if ((bx - bowl_x) * (bx - bowl_x) + (by - bowl_y) * (by - bowl_y) <
            (bowl_r + br + 8) * (bowl_r + br + 8)) {
            continue;
        }

        draw_bubble(bx, by, br);
    }
}

/* -------------------------------------------------------------------------- */
/* Glass figure                                                                */
/* -------------------------------------------------------------------------- */

/*
 * A figure made of clear blue glass stands in the grass in the lower right
 * corner. It is drawn into the cached desktop surface over the wallpaper,
 * so the scene shows through it.
 */
static gui64_rect_t mascot_rect(void) {
    return rect_make(
        g_screen_w - GUI64_MASCOT_W - 40,
        g_screen_h - GUI64_TASKBAR_H - GUI64_MASCOT_H - 2,
        GUI64_MASCOT_W,
        GUI64_MASCOT_H
    );
}

static void mascot_draw(void) {
    gui64_rect_t r = mascot_rect();

    if (r.x < 140 || r.y < 0 || !g_desktop.px) {
        return;
    }

    draw_to(&g_desktop);
    draw_clip(r);

    int cx = r.x + r.w / 2;
    int ground = r.y + r.h;
    nq_u32 glass = rgb(30, 140, 255);

    /*
     * Body: a tall rounded bell that runs off the bottom of its box, then
     * the head resting on it.
     */
    glass_ball(cx, ground - 20, 50, 62, glass, 170);
    glass_ball(cx, r.y + 44, 38, 38, glass, 185);

    /*
     * Closed, contented eyes: two short arcs.
     */
    arc_stroke(cx - 14, r.y + 50, 6, -3, rgb(8, 70, 170));
    arc_stroke(cx + 14, r.y + 50, 6, -3, rgb(8, 70, 170));

    /*
     * Rim light down one side.
     */
    ellipse(cx + 26, r.y + 36, 4, 12, 0x00FFFFFFu, 60);
}

/*
 * Rebuilds the cached desktop: the wallpaper with the icons and the glass
 * figure on top.
 */
static void desktop_render(void) {
    if (!g_desktop.px || !g_wallpaper.px) {
        return;
    }

    copy32(g_desktop.px, g_wallpaper.px,
           (nq_u64)g_screen_w * (nq_u64)g_screen_h);

    draw_to(&g_desktop);

    for (int i = 0; i < g_app_count; i++) {
        desktop_draw_icon(i);
    }

    mascot_draw();
    draw_to(&g_desktop);
}

static void app_list_add(const char* name, nq_u64 size, int is_dir, void* ctx) {
    (void)size;
    (void)ctx;

    if (is_dir || g_app_count >= GUI64_MAX_APPS) {
        return;
    }

    int len = str_len64(name);

    if (len < 5 || len >= GUI64_APP_NAME_MAX) {
        return;
    }

    const char* ext = name + len - 4;

    if (ext[0] != '.' ||
        (ext[1] != 'E' && ext[1] != 'e') ||
        (ext[2] != 'L' && ext[2] != 'l') ||
        (ext[3] != 'F' && ext[3] != 'f')) {
        return;
    }

    str_copy64(g_apps[g_app_count++], name, GUI64_APP_NAME_MAX);
}

static void app_list_load(void) {
    g_app_count = 0;
    vfs64_enum_dir("/apps", app_list_add, 0);
}

static void launch_app(int index) {
    char path[GUI64_APP_NAME_MAX + 8];

    if (index < 0 || index >= g_app_count) {
        return;
    }

    str_copy64(path, "/apps/", sizeof(path));
    str_copy64(path + 6, g_apps[index], sizeof(path) - 6);

    elf64_spawn_file_ex(path, PROCESS64_SPAWN_PRIVATE_TERMINAL);
}

/* -------------------------------------------------------------------------- */
/* Desktop shell: clock, taskbar, start menu                                   */
/* -------------------------------------------------------------------------- */

static nq_u8 cmos_read(nq_u8 reg) {
    outb(0x70, reg);
    io_wait();

    return inb(0x71);
}

/*
 * Reads hours and minutes from the CMOS real-time clock.
 * Returns 1 if the displayed text changed.
 */
static int clock_update(void) {
    /*
     * Wait (briefly) for an update cycle to finish so the registers are
     * consistent.
     */
    for (int i = 0; i < 2000; i++) {
        if (!(cmos_read(0x0A) & 0x80)) {
            break;
        }
    }

    nq_u8 minute = cmos_read(0x02);
    nq_u8 hour = cmos_read(0x04);
    nq_u8 status_b = cmos_read(0x0B);

    int pm = hour & 0x80;

    hour &= 0x7F;

    if (!(status_b & 0x04)) {
        minute = (nq_u8)((minute & 0x0F) + ((minute >> 4) * 10));
        hour = (nq_u8)((hour & 0x0F) + ((hour >> 4) * 10));
    }

    if (!(status_b & 0x02)) {
        /*
         * 12-hour mode.
         */
        hour = (nq_u8)(hour % 12);

        if (pm) {
            hour = (nq_u8)(hour + 12);
        }
    }

    if (hour > 23 || minute > 59) {
        return 0;
    }

    char text[12];
    int n = 0;

    if (g_set_clock12) {
        int shown = hour % 12;

        if (shown == 0) {
            shown = 12;
        }

        if (shown >= 10) {
            text[n++] = '1';
        }

        text[n++] = (char)('0' + shown % 10);
    } else {
        text[n++] = (char)('0' + hour / 10);
        text[n++] = (char)('0' + hour % 10);
    }

    text[n++] = ':';
    text[n++] = (char)('0' + minute / 10);
    text[n++] = (char)('0' + minute % 10);

    if (g_set_clock12) {
        text[n++] = ' ';
        text[n++] = hour >= 12 ? 'P' : 'A';
        text[n++] = 'M';
    }

    text[n] = 0;

    for (int i = 0; i <= n; i++) {
        if (text[i] != g_clock_text[i]) {
            str_copy64(g_clock_text, text, sizeof(g_clock_text));
            return 1;
        }
    }

    return 0;
}

static gui64_rect_t taskbar_rect(void) {
    return rect_make(0, g_screen_h - GUI64_TASKBAR_H, g_screen_w,
                     GUI64_TASKBAR_H);
}

/*
 * Taskbar buttons follow window slot order, which is stable while windows
 * are focused and raised. Returns the window for button `index`, or 0.
 */
static gui64_window_t* taskbar_window(int index) {
    int n = 0;

    for (int i = 0; i < GUI64_MAX_WINDOWS; i++) {
        if (!g_windows[i].used) {
            continue;
        }

        if (n == index) {
            return &g_windows[i];
        }

        n++;
    }

    return 0;
}

static void taskbar_render(void) {
    g_taskbar_dirty = 0;

    if (!g_taskbar.px || !g_wallpaper.px) {
        return;
    }

    int w = g_taskbar.w;
    int h = GUI64_TASKBAR_H;

    draw_to(&g_taskbar);

    /*
     * The strip starts as the wallpaper behind it; the dock is a rounded
     * pane of blue glass floating in that strip.
     */
    for (int row = 0; row < h; row++) {
        copy32(
            g_taskbar.px + (nq_u64)row * (nq_u64)w,
            g_wallpaper.px +
                (nq_u64)(g_screen_h - h + row) * (nq_u64)g_wallpaper.w,
            (nq_u64)w
        );
    }

    int dx = GUI64_DOCK_MARGIN;
    int dy = GUI64_DOCK_Y;
    int dw = w - GUI64_DOCK_MARGIN * 2;
    int dh = GUI64_DOCK_H;

    rrect_alpha(dx + 1, dy + 3, dw, dh, GUI64_DOCK_CORNER, rgb(0, 30, 20), 90);
    rrect_alpha(dx, dy, dw, dh, GUI64_DOCK_CORNER, rgb(4, 50, 120), 200);
    rrect_alpha(dx + 1, dy + 1, dw - 2, dh - 2, GUI64_DOCK_CORNER - 1,
                rgb(20, 120, 230), 120);

    /*
     * Glass shine: bright upper half, glow along the bottom edge.
     */
    rrect_gloss(dx + 2, dy + 2, dw - 4, dh - 4, GUI64_DOCK_CORNER - 2,
                16, 150, 24);
    rrect_alpha(dx + 6, dy + dh - 9, dw - 12, 6, 3, rgb(120, 220, 255), 70);
    fill_alpha(dx + GUI64_DOCK_CORNER, dy, dw - GUI64_DOCK_CORNER * 2, 1,
               0x00FFFFFFu, 220);

    int mid = dy + dh / 2;

    /*
     * Start orb.
     */
    int orb_x = GUI64_TASK_START_X + GUI64_TASK_START_W / 2;

    glass_ball(orb_x, mid, 16, 16,
               g_menu_open ? rgb(40, 150, 30) : rgb(90, 210, 30), 255);

    draw_char(orb_x - 3, mid - 7, 'N', rgb(10, 80, 10), 1);
    draw_char(orb_x - 4, mid - 8, 'N', 0x00FFFFFFu, 1);

    /*
     * One glass pill per window.
     */
    /*
     * "14:05" or "2:05 PM": the clock takes as much room as its text.
     */
    int clock_w = (int)str_len64(g_clock_text) * 8 + 22;

    if (clock_w < GUI64_TASK_CLOCK_W) {
        clock_w = GUI64_TASK_CLOCK_W;
    }

    int limit = w - GUI64_DOCK_MARGIN - clock_w - 20;

    for (int i = 0; ; i++) {
        gui64_window_t* win = taskbar_window(i);
        int x = GUI64_TASK_BUTTON_X + i * GUI64_TASK_BUTTON_PITCH;

        if (!win || x + GUI64_TASK_BUTTON_W > limit) {
            break;
        }

        int focused = win->active && !win->minimized;

        rrect_alpha(x, mid - 14, GUI64_TASK_BUTTON_W, 28, 11, 0x00FFFFFFu,
                    focused ? 130u : (win->minimized ? 22u : 52u));
        rrect_gloss(x + 1, mid - 13, GUI64_TASK_BUTTON_W - 2, 26, 10, 11,
                    focused ? 130u : 70u, 10u);

        nq_u32 dot = win->kind == GUI64_WINDOW_TERMINAL ?
            rgb(60, 90, 140) : icon_color(win->title);

        glass_ball(x + 16, mid, 8, 8, dot, 255);

        int max_chars = (GUI64_TASK_BUTTON_W - 38) / GUI64_FONT_W;

        draw_text(x + 30, mid - 7, win->title, rgb(0, 40, 100), max_chars);
        draw_text(x + 29, mid - 8, win->title,
                  win->minimized ? rgb(176, 210, 240) : 0x00FFFFFFu,
                  max_chars);
    }

    /*
     * Clock.
     */
    int clock_x = w - GUI64_DOCK_MARGIN - clock_w - 2;

    draw_text(clock_x + 9, mid - 7, g_clock_text, rgb(0, 40, 100), -1);
    draw_text(clock_x + 8, mid - 8, g_clock_text, 0x00FFFFFFu, -1);

    accent_apply(&g_taskbar, rect_make(0, 0, g_taskbar.w, g_taskbar.h));

    damage_add(taskbar_rect());
}

static const char* menu_fixed_label(int fixed_index) {
    switch (fixed_index) {
        case 0:
            return "Terminal";

        case 1:
            return "Text mode";

        case 2:
            return "Restart";

        default:
            return "Shut down";
    }
}

static nq_u32 menu_fixed_color(int fixed_index) {
    switch (fixed_index) {
        case 0:
            return rgb(40, 70, 130);

        case 1:
            return rgb(110, 130, 150);

        case 2:
            return rgb(255, 150, 0);

        default:
            return rgb(240, 40, 60);
    }
}

static int menu_item_count(void) {
    return g_app_count + GUI64_MENU_FIXED_ITEMS;
}

static gui64_rect_t menu_rect(void) {
    int h = GUI64_MENU_HEADER_H + menu_item_count() * GUI64_MENU_ITEM_H + 22;

    return rect_make(4, g_screen_h - GUI64_TASKBAR_H - h - 2, GUI64_MENU_W, h);
}

/*
 * Screen area the open menu paints: the menu plus its shadow on the right.
 * Damage must cover all of it, or closing the menu leaves the shadow behind.
 */
static gui64_rect_t menu_damage_rect(void) {
    gui64_rect_t r = menu_rect();

    r.w += GUI64_SHADOW;
    r.h += GUI64_SHADOW;

    return r;
}

/*
 * Y offset of an item inside the menu surface. The fixed items sit below a
 * separator.
 */
static int menu_item_y(int index) {
    int y = GUI64_MENU_HEADER_H + 6 + index * GUI64_MENU_ITEM_H;

    if (index >= g_app_count) {
        y += 8;
    }

    return y;
}

static void menu_render(void) {
    gui64_rect_t r = menu_rect();

    if (!g_menu.px || g_menu.w != r.w || g_menu.h != r.h) {
        surface_free(&g_menu);

        if (!surface_alloc(&g_menu, r.w, r.h)) {
            g_menu_open = 0;
            return;
        }
    }

    draw_to(&g_menu);

    /*
     * Rounded panel: deep blue edge, pale glass body.
     */
    rrect_fill(0, 0, r.w, r.h, GUI64_MENU_CORNER, rgb(8, 70, 150));
    rrect_gradient(1, 1, r.w - 2, r.h - 2, GUI64_MENU_CORNER - 1,
                   rgb(244, 251, 255), rgb(196, 228, 250));

    /*
     * Blue glass header.
     */
    for (int row = 1; row < GUI64_MENU_HEADER_H; row++) {
        int inset = row_inset(r.h, GUI64_MENU_CORNER, row);

        if (inset < 1) {
            inset = 1;
        }

        fill(inset, row, r.w - inset * 2, 1,
             mix(rgb(96, 190, 255), rgb(12, 100, 214), row,
                 GUI64_MENU_HEADER_H));

        if (row < GUI64_MENU_HEADER_H / 2) {
            fill_alpha(inset, row, r.w - inset * 2, 1, 0x00FFFFFFu,
                       (nq_u32)(140 - row * 6));
        }
    }

    glass_ball(24, 20, 13, 13, rgb(90, 210, 30), 255);
    draw_char(21, 13, 'N', rgb(10, 80, 10), 1);
    draw_char(20, 12, 'N', 0x00FFFFFFu, 1);

    draw_text(47, 13, "NAQ OS", rgb(0, 50, 130), -1);
    draw_text(46, 12, "NAQ OS", 0x00FFFFFFu, -1);

    for (int i = 0; i < menu_item_count(); i++) {
        char label[GUI64_APP_NAME_MAX];
        int y = menu_item_y(i);
        nq_u32 bead;

        if (i < g_app_count) {
            app_label(g_apps[i], label, sizeof(label));
            bead = icon_color(g_apps[i]);
        } else {
            str_copy64(label, menu_fixed_label(i - g_app_count), sizeof(label));
            bead = menu_fixed_color(i - g_app_count);
        }

        if (i == g_app_count) {
            fill(12, y - 5, r.w - 24, 1, rgb(140, 190, 230));
            fill(12, y - 4, r.w - 24, 1, 0x00FFFFFFu);
        }

        int hovered = i == g_menu_hover;

        if (hovered) {
            /*
             * Blue gel highlight under the pointer.
             */
            rrect_fill(6, y, r.w - 12, GUI64_MENU_ITEM_H - 2, 10,
                       rgb(8, 90, 190));
            rrect_gradient(7, y + 1, r.w - 14, GUI64_MENU_ITEM_H - 4, 9,
                           rgb(120, 208, 255), rgb(16, 120, 228));
            rrect_gloss(8, y + 2, r.w - 16, GUI64_MENU_ITEM_H - 6, 8,
                        10, 150, 20);
        }

        glass_ball(22, y + 13, 9, 9, bead, 255);

        int max_chars = (r.w - 52) / GUI64_FONT_W;

        if (hovered) {
            draw_text(41, y + 6, label, rgb(0, 50, 130), max_chars);
            draw_text(40, y + 5, label, 0x00FFFFFFu, max_chars);
        } else {
            draw_text(40, y + 5, label, rgb(10, 60, 120), max_chars);
        }
    }

    accent_apply(&g_menu, rect_make(0, 0, g_menu.w, g_menu.h));

    damage_add(menu_damage_rect());
}

static void menu_set_open(int open) {
    if (g_menu_open == open) {
        return;
    }

    g_menu_open = open;
    g_menu_hover = -1;
    g_taskbar_dirty = 1;

    if (open) {
        menu_render();
    } else {
        damage_add(menu_damage_rect());
    }
}

/*
 * Menu item under a screen point, or -1.
 */
static int menu_item_at(int x, int y) {
    gui64_rect_t r = menu_rect();

    if (!rect_has_point(r, x, y)) {
        return -1;
    }

    for (int i = 0; i < menu_item_count(); i++) {
        int iy = r.y + menu_item_y(i);

        if (y >= iy && y < iy + GUI64_MENU_ITEM_H) {
            return i;
        }
    }

    return -1;
}

/* -------------------------------------------------------------------------- */
/* Compositor                                                                  */
/* -------------------------------------------------------------------------- */

static const char* const g_cursor_shape[GUI64_CURSOR_ROWS] = {
    "#           ",
    "##          ",
    "#.#         ",
    "#..#        ",
    "#...#       ",
    "#....#      ",
    "#.....#     ",
    "#......#    ",
    "#.......#   ",
    "#........#  ",
    "#.........# ",
    "#......#####",
    "#...#..#    ",
    "#..# #..#   ",
    "#.#  #..#   ",
    "##    #..#  ",
    "#     #..#  ",
    "       #..# ",
    "        ##  "
};

static gui64_rect_t cursor_rect(void) {
    return rect_make(g_cursor_x, g_cursor_y, GUI64_CURSOR_W, GUI64_CURSOR_H);
}

static void draw_cursor(void) {
    /*
     * Soft shadow first, then a white arrow with a dark blue outline.
     */
    for (int y = 0; y < GUI64_CURSOR_ROWS; y++) {
        for (int x = 0; x < 12; x++) {
            if (g_cursor_shape[y][x] != ' ') {
                blend_pixel(g_cursor_x + x + 2, g_cursor_y + y + 3,
                            rgb(20, 50, 80), 60);
            }
        }
    }

    for (int y = 0; y < GUI64_CURSOR_ROWS; y++) {
        for (int x = 0; x < 12; x++) {
            char c = g_cursor_shape[y][x];

            if (c == '#') {
                put_pixel(g_cursor_x + x, g_cursor_y + y, rgb(36, 84, 124));
            } else if (c == '.') {
                put_pixel(g_cursor_x + x, g_cursor_y + y, 0x00FFFFFFu);
            }
        }
    }
}

static void draw_outline(gui64_rect_t r) {
    nq_u32 light = rgb(255, 255, 255);
    nq_u32 dark = rgb(20, 80, 104);

    border(r.x, r.y, r.w, r.h, dark);
    border(r.x + 1, r.y + 1, r.w - 2, r.h - 2, light);
    border(r.x + 2, r.y + 2, r.w - 4, r.h - 4, dark);
}

/*
 * Topmost visible window, or 0.
 */
static gui64_window_t* top_window(void) {
    for (int zi = g_z_count - 1; zi >= 0; zi--) {
        gui64_window_t* win = window_by_id(g_z[zi]);

        if (window_visible(win)) {
            return win;
        }
    }

    return 0;
}

/*
 * Soft drop shadow to the right of and below a rectangle: three thin bands
 * that get lighter with distance.
 */
static void draw_shadow(gui64_rect_t r) {
    static const nq_u32 strength[3] = { 74, 84, 93 };

    for (int i = 0; i < 3; i++) {
        darken(r.x + r.w + i * 2, r.y + GUI64_CORNER + i * 2,
               2, r.h - GUI64_CORNER, strength[i]);
        darken(r.x + GUI64_CORNER + i * 2, r.y + r.h + i * 2,
               r.w - GUI64_CORNER + 2, 2, strength[i]);
    }
}

/*
 * Rebuilds one rectangle of the screen in the back buffer and copies it to
 * the display.
 */
static void compose(gui64_rect_t r) {
    r = rect_intersect(r, screen_rect());

    if (rect_empty(r) || !g_back.px) {
        return;
    }

    draw_to(&g_back);
    draw_clip(r);

    /*
     * Occlusion: apart from its rounded corners every window is opaque. If
     * one of them covers the whole damaged area, nothing below it can show
     * through, so start there and skip the desktop and windows underneath.
     */
    int start = -1;

    for (int zi = g_z_count - 1; zi >= 0; zi--) {
        gui64_window_t* win = window_by_id(g_z[zi]);

        if (window_visible(win) && rect_contains(window_opaque_rect(win), r)) {
            start = zi;
            break;
        }
    }

    if (start < 0) {
        blit(&g_desktop, 0, 0);
    }

    for (int zi = start < 0 ? 0 : start; zi < g_z_count; zi++) {
        gui64_window_t* win = window_by_id(g_z[zi]);

        if (!window_visible(win)) {
            continue;
        }

        if (zi != start && window_has_shadow(win)) {
            draw_shadow(window_rect(win));
        }

        blit_rounded(&win->surface, win->x, win->y, window_corner(win));
    }

    /*
     * A fullscreen window on top owns the whole screen, taskbar included.
     */
    gui64_window_t* top = top_window();

    if (!(top && window_fullscreen(top))) {
        blit(&g_taskbar, 0, g_screen_h - GUI64_TASKBAR_H);
    }

    if (g_menu_open && g_menu.px) {
        gui64_rect_t m = menu_rect();

        draw_shadow(m);
        blit_rounded(&g_menu, m.x, m.y, GUI64_MENU_CORNER);
    }

    if (g_outline_visible) {
        draw_outline(g_outline);
    }

    if (mouse64_available()) {
        draw_cursor();
    }

    fb64_blit_xrgb32(
        (nq_u32)r.x,
        (nq_u32)r.y,
        (nq_u32)r.w,
        (nq_u32)r.h,
        g_back.px + (nq_u64)r.y * (nq_u64)g_back.w + r.x,
        (nq_u32)g_back.w
    );
}

static void compose_damage(void) {
    gui64_rect_t pending[GUI64_MAX_DAMAGE];
    int count = g_damage_count;

    for (int i = 0; i < count; i++) {
        pending[i] = g_damage[i];
    }

    g_damage_count = 0;

    for (int i = 0; i < count; i++) {
        compose(pending[i]);
    }
}

/* -------------------------------------------------------------------------- */
/* Window manager operations                                                   */
/* -------------------------------------------------------------------------- */

static void outline_damage(void) {
    if (!g_outline_visible) {
        return;
    }

    /*
     * Only the four 3-pixel edges change, not the area inside.
     */
    gui64_rect_t r = g_outline;

    damage_add(rect_make(r.x, r.y, r.w, 3));
    damage_add(rect_make(r.x, r.y + r.h - 3, r.w, 3));
    damage_add(rect_make(r.x, r.y, 3, r.h));
    damage_add(rect_make(r.x + r.w - 3, r.y, 3, r.h));
}

static void wm_focus(int id) {
    gui64_window_t* target = window_by_id(id);

    if (!target) {
        g_active_id = 0;

        for (int i = 0; i < GUI64_MAX_WINDOWS; i++) {
            if (g_windows[i].used && g_windows[i].active) {
                g_windows[i].active = 0;
                g_windows[i].chrome_dirty = 1;
            }
        }

        g_taskbar_dirty = 1;
        return;
    }

    for (int i = 0; i < GUI64_MAX_WINDOWS; i++) {
        gui64_window_t* win = &g_windows[i];

        if (!win->used) {
            continue;
        }

        int want = win == target;

        if (win->active != want) {
            win->active = want;
            win->chrome_dirty = 1;
        }
    }

    int was_top = g_z_count > 0 && g_z[g_z_count - 1] == id;

    z_raise(id);

    if (!was_top) {
        damage_window(target);
    }

    if (g_active_id != id) {
        g_active_id = id;
        event_push_simple(target, GUI64_EVENT_FOCUS, 0);
    }

    g_taskbar_dirty = 1;
}

/*
 * Gives focus to the topmost visible window (after one closes or is
 * minimised).
 */
static void wm_focus_top(void) {
    gui64_window_t* top = top_window();

    wm_focus(top ? top->id : 0);
}

static void wm_move(gui64_window_t* win, int x, int y) {
    if (win->x == x && win->y == y) {
        return;
    }

    damage_window(win);

    win->x = x;
    win->y = y;

    damage_window(win);
}

/*
 * Changes a window's size. The content that fits is kept; the rest is
 * filled with the background until the owner draws again.
 */
static int wm_resize(gui64_window_t* win, int w, int h) {
    if (w == win->w && h == win->h) {
        return 1;
    }

    gui64_surface_t fresh;

    if (!surface_alloc(&fresh, w, h)) {
        return 0;
    }

    gui64_rect_t old_content = window_content_local(win);
    gui64_surface_t old = win->surface;

    damage_window(win);

    win->surface = fresh;
    win->w = w;
    win->h = h;

    gui64_rect_t content = window_content_local(win);

    draw_to(&win->surface);
    fill(0, 0, w, h,
         win->kind == GUI64_WINDOW_TERMINAL ? GUI64_TERMINAL_BG : 0);

    int copy_w = old_content.w < content.w ? old_content.w : content.w;
    int copy_h = old_content.h < content.h ? old_content.h : content.h;

    if (old.px && copy_w > 0 && copy_h > 0) {
        for (int row = 0; row < copy_h; row++) {
            copy32(
                win->surface.px +
                    (nq_u64)(content.y + row) * (nq_u64)w + content.x,
                old.px +
                    (nq_u64)(old_content.y + row) * (nq_u64)old.w +
                    old_content.x,
                (nq_u64)copy_w
            );
        }
    }

    surface_free(&old);

    win->chrome_dirty = 1;

    if (win->kind == GUI64_WINDOW_TERMINAL) {
        window_render_terminal(win);
    } else {
        gui64_event_t ev;

        ev.type = GUI64_EVENT_RESIZE;
        ev.key = 0;
        ev.x = 0;
        ev.y = 0;
        ev.w = content.w;
        ev.h = content.h;

        event_push(win, &ev);
    }

    damage_window(win);

    return 1;
}

static void wm_set_maximized(gui64_window_t* win, int maximized) {
    if (window_borderless(win) || win->maximized == maximized) {
        return;
    }

    if (maximized) {
        win->restore = window_rect(win);

        damage_window(win);
        win->maximized = 1;
        win->x = 0;
        win->y = 0;

        wm_resize(win, g_screen_w, g_screen_h - GUI64_TASKBAR_H);
    } else {
        damage_window(win);
        win->maximized = 0;
        win->x = win->restore.x;
        win->y = win->restore.y;

        wm_resize(win, win->restore.w, win->restore.h);
    }

    win->chrome_dirty = 1;
    damage_window(win);
}

static void wm_set_minimized(gui64_window_t* win, int minimized) {
    if (win->minimized == minimized) {
        return;
    }

    if (minimized) {
        damage_window(win);
        win->minimized = 1;

        if (win->active) {
            win->active = 0;
            win->chrome_dirty = 1;
            g_active_id = 0;
            wm_focus_top();
        }
    } else {
        win->minimized = 0;

        if (win->content_stale && win->kind == GUI64_WINDOW_TERMINAL) {
            window_render_terminal(win);
        }

        damage_window(win);
    }

    g_taskbar_dirty = 1;
}

static void wm_destroy(gui64_window_t* win) {
    int id = win->id;

    damage_window(win);
    surface_free(&win->surface);

    if (win->owns_terminal && win->terminal) {
        terminal64_destroy(win->terminal);
    }

    z_remove(id);
    mem_zero64(win, sizeof(*win));

    if (id == g_terminal_window_id) {
        g_terminal_window_id = 0;
    }

    if (g_drag_window == id) {
        outline_damage();
        g_drag_mode = GUI64_DRAG_NONE;
        g_drag_window = 0;
        g_outline_visible = 0;
    }

    if (g_capture_window == id) {
        g_capture_window = 0;
    }

    if (g_active_id == id) {
        g_active_id = 0;
        wm_focus_top();
    }

    g_taskbar_dirty = 1;
}

static int pid_is_graphical(int pid);

/*
 * The user clicked a window's close button (or pressed the close hotkey).
 */
static void wm_request_close(gui64_window_t* win) {
    if (win->kind == GUI64_WINDOW_TERMINAL) {
        /*
         * The shell's terminal: the shell keeps running and "Terminal" in
         * the start menu brings the window back.
         *
         * An app's console: if this is the app's only window, closing it
         * ends the app (like closing a console window); if the app also
         * has its own windows this was just its output view.
         */
        int pid = win->owner_pid;

        wm_destroy(win);

        if (pid > 0 && !pid_is_graphical(pid)) {
            process64_kill((nq_u32)pid);
        }

        return;
    }

    event_push_simple(win, GUI64_EVENT_CLOSE, 0);

    if (!win->close_requested_tick) {
        win->close_requested_tick = timer64_get_ticks();

        if (!win->close_requested_tick) {
            win->close_requested_tick = 1;
        }
    }
}

static gui64_window_t* window_alloc(
    int kind,
    const char* title,
    int x,
    int y,
    int w,
    int h,
    nq_u32 flags
) {
    for (int i = 0; i < GUI64_MAX_WINDOWS; i++) {
        gui64_window_t* win = &g_windows[i];

        if (win->used) {
            continue;
        }

        mem_zero64(win, sizeof(*win));

        if (!surface_alloc(&win->surface, w, h)) {
            return 0;
        }

        win->used = 1;
        win->id = g_next_id++;
        win->kind = kind;
        win->flags = flags;
        win->x = x;
        win->y = y;
        win->w = w;
        win->h = h;
        win->chrome_dirty = 1;

        str_copy64(win->title, title ? title : "Window", sizeof(win->title));

        draw_to(&win->surface);
        fill(0, 0, w, h, kind == GUI64_WINDOW_TERMINAL ? GUI64_TERMINAL_BG : 0);

        z_raise(win->id);
        g_taskbar_dirty = 1;

        return win;
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
/* Console windows for apps                                                    */
/* -------------------------------------------------------------------------- */

static gui64_window_t* terminal_window_for(terminal64_session_t* terminal) {
    if (!terminal) {
        return 0;
    }

    for (int i = 0; i < GUI64_MAX_WINDOWS; i++) {
        if (g_windows[i].used &&
            g_windows[i].kind == GUI64_WINDOW_TERMINAL &&
            g_windows[i].terminal == terminal) {
            return &g_windows[i];
        }
    }

    return 0;
}

static void gui_pid_mark(int pid) {
    int free_slot = -1;

    for (int i = 0; i < GUI64_MAX_GUI_PIDS; i++) {
        if (g_gui_pids[i] == pid) {
            return;
        }

        if (g_gui_pids[i] == 0 && free_slot < 0) {
            free_slot = i;
        }
    }

    if (free_slot >= 0) {
        g_gui_pids[free_slot] = pid;
    }
}

static void gui_pid_forget(int pid) {
    for (int i = 0; i < GUI64_MAX_GUI_PIDS; i++) {
        if (g_gui_pids[i] == pid) {
            g_gui_pids[i] = 0;
        }
    }
}

/*
 * 1 if the process is a graphical program: it has, or has had, a window of
 * its own.
 */
static int pid_is_graphical(int pid) {
    if (pid <= 0) {
        return 0;
    }

    for (int i = 0; i < GUI64_MAX_GUI_PIDS; i++) {
        if (g_gui_pids[i] == pid) {
            return 1;
        }
    }

    for (int i = 0; i < GUI64_MAX_WINDOWS; i++) {
        if (g_windows[i].used &&
            g_windows[i].kind == GUI64_WINDOW_APP &&
            g_windows[i].owner_pid == pid) {
            return 1;
        }
    }

    return 0;
}

static int terminal_has_output(terminal64_session_t* terminal) {
    return terminal64_cursor_row(terminal) > 0 ||
           terminal64_cursor_col(terminal) > 0;
}

/*
 * Opens a window showing a terminal session (or focuses the one that
 * already shows it). pid is the process that owns the session, 0 if it has
 * already exited.
 */
static gui64_window_t* console_open(
    terminal64_session_t* terminal,
    int pid,
    const char* name
) {
    gui64_window_t* win = terminal_window_for(terminal);

    if (win) {
        if (win->minimized) {
            wm_set_minimized(win, 0);
        }

        wm_focus(win->id);
        return win;
    }

    int step = g_cascade++ % 8;
    int w = 560;
    int h = 320;
    int x = 240 + step * 36;
    int y = 110 + step * 30;

    if (x + w > g_screen_w) {
        x = g_screen_w - w;
    }

    if (y + h > g_screen_h - GUI64_TASKBAR_H) {
        y = g_screen_h - GUI64_TASKBAR_H - h;
    }

    if (x < 0) {
        x = 0;
    }

    if (y < 0) {
        y = 0;
    }

    char title[32];

    str_copy64(title, name && name[0] ? name : "App", sizeof(title));

    int len = str_len64(title);

    str_copy64(title + len, " output", sizeof(title) - (nq_u64)len);

    win = window_alloc(GUI64_WINDOW_TERMINAL, title, x, y, w, h,
                       GUI64_WIN_FLAG_BORDERED);

    if (!win) {
        return 0;
    }

    win->terminal = terminal;
    win->owner_pid = pid;

    window_render_terminal(win);
    wm_focus(win->id);
    damage_window(win);

    return win;
}

/*
 * The "output" title-bar button: show the terminal of the process that owns
 * an app window.
 */
static void console_open_for_pid(int pid) {
    process64_info_t info;

    if (pid <= 0 || !process64_info_for_pid((nq_u32)pid, &info)) {
        return;
    }

    if (!info.owns_terminal || !info.terminal) {
        return;
    }

    console_open(info.terminal, pid, info.name);
}

/*
 * Programs without a window of their own (console programs) get a console
 * window once they have printed something.
 */
static void console_scan(void) {
    nq_u64 now = timer64_get_ticks();

    for (nq_u32 slot = 0; slot < PROCESS64_MAX_TASKS; slot++) {
        process64_info_t info;

        if (!process64_task_info(slot, &info)) {
            continue;
        }

        if (info.pid == 0 || !info.owns_terminal || !info.terminal) {
            continue;
        }

        /*
         * Give graphical apps a moment to create their window first.
         */
        if (now - info.created_tick < 400) {
            continue;
        }

        if (pid_is_graphical((int)info.pid) ||
            terminal_window_for(info.terminal) ||
            !terminal_has_output(info.terminal)) {
            continue;
        }

        console_open(info.terminal, (int)info.pid, info.name);
    }
}

int gui64_adopt_terminal(terminal64_session_t* terminal, int pid, const char* name) {
    int adopted = 0;

    if (!terminal) {
        return 0;
    }

    GUI64_LOCK();

    gui64_window_t* win = terminal_window_for(terminal);

    /*
     * A graphical app's output view closes with the app.
     */
    if (!pid_is_graphical(pid)) {
        /*
         * A console program that ended before its window appeared: show
         * what it printed.
         */
        if (!win && g_active && terminal_has_output(terminal)) {
            win = console_open(terminal, 0, name);
        }

        if (win) {
            int len = str_len64(win->title);

            if (len + 9 < (int)sizeof(win->title)) {
                str_copy64(win->title + len, " (ended)",
                           sizeof(win->title) - (nq_u64)len);
            }

            win->owner_pid = 0;
            win->owns_terminal = 1;
            win->chrome_dirty = 1;
            g_taskbar_dirty = 1;

            window_render_terminal(win);
            adopted = 1;
        }
    }

    GUI64_UNLOCK();

    return adopted;
}

/* -------------------------------------------------------------------------- */
/* Mouse                                                                       */
/* -------------------------------------------------------------------------- */

/*
 * Topmost visible window under a screen point, or 0.
 */
static gui64_window_t* window_at(int x, int y) {
    for (int zi = g_z_count - 1; zi >= 0; zi--) {
        gui64_window_t* win = window_by_id(g_z[zi]);

        if (window_visible(win) && rect_has_point(window_rect(win), x, y)) {
            return win;
        }
    }

    return 0;
}

/*
 * Which part of a window a window-local point is on.
 */
static int window_part_at(const gui64_window_t* win, int lx, int ly) {
    if (window_borderless(win)) {
        return GUI64_PART_CONTENT;
    }

    if (rect_has_point(window_button_rect(win, GUI64_PART_CLOSE), lx, ly)) {
        return GUI64_PART_CLOSE;
    }

    if (rect_has_point(window_button_rect(win, GUI64_PART_MAXIMIZE), lx, ly)) {
        return GUI64_PART_MAXIMIZE;
    }

    if (rect_has_point(window_button_rect(win, GUI64_PART_MINIMIZE), lx, ly)) {
        return GUI64_PART_MINIMIZE;
    }

    if (window_has_log_button(win) &&
        rect_has_point(window_button_rect(win, GUI64_PART_LOG), lx, ly)) {
        return GUI64_PART_LOG;
    }

    if (!win->maximized) {
        /*
         * Any edge or corner resizes. The top edge is only the outermost
         * 3 pixels so the rest of the title bar still drags the window;
         * corners are larger so they are easy to grab.
         */
        int edges = 0;
        int corner = GUI64_RESIZE_CORNER;

        if (lx < GUI64_FRAME) {
            edges |= GUI64_EDGE_LEFT;
        }

        if (lx >= win->w - GUI64_FRAME) {
            edges |= GUI64_EDGE_RIGHT;
        }

        if (ly < 3) {
            edges |= GUI64_EDGE_TOP;
        }

        if (ly >= win->h - GUI64_FRAME) {
            edges |= GUI64_EDGE_BOTTOM;
        }

        if (ly >= win->h - corner) {
            if (lx < corner) {
                edges |= GUI64_EDGE_LEFT | GUI64_EDGE_BOTTOM;
            } else if (lx >= win->w - corner) {
                edges |= GUI64_EDGE_RIGHT | GUI64_EDGE_BOTTOM;
            }
        }

        if (ly < 8) {
            if (lx < 8) {
                edges |= GUI64_EDGE_LEFT | GUI64_EDGE_TOP;
            } else if (lx >= win->w - 8) {
                edges |= GUI64_EDGE_RIGHT | GUI64_EDGE_TOP;
            }
        }

        if (edges) {
            g_hit_edges = edges;
            return GUI64_PART_RESIZE;
        }
    }

    if (ly < GUI64_CONTENT_Y) {
        return GUI64_PART_TITLE;
    }

    if (rect_has_point(window_content_local(win), lx, ly)) {
        return GUI64_PART_CONTENT;
    }

    return GUI64_PART_NONE;
}

static void send_mouse_event(
    gui64_window_t* win,
    int action,
    nq_u32 changed_button
) {
    if (!win || win->kind != GUI64_WINDOW_APP) {
        return;
    }

    gui64_rect_t c = window_content_local(win);
    gui64_event_t ev;

    ev.type = GUI64_EVENT_MOUSE;
    ev.key = g_buttons;
    ev.x = g_cursor_x - win->x - c.x;
    ev.y = g_cursor_y - win->y - c.y;
    ev.w = action;
    ev.h = (int)changed_button;

    /*
     * Moves arrive far faster than apps read them: replace a queued move
     * instead of piling them up.
     */
    if (action == GUI64_MOUSE_MOVE && win->event_head != win->event_tail) {
        nq_u32 last =
            (win->event_tail + GUI64_EVENT_QUEUE_SIZE - 1u) %
            GUI64_EVENT_QUEUE_SIZE;

        if (win->events[last].type == GUI64_EVENT_MOUSE &&
            win->events[last].w == GUI64_MOUSE_MOVE) {
            win->events[last] = ev;
            return;
        }
    }

    event_push(win, &ev);
}

static void console_open_for_pid(int pid);

static void menu_activate(int index) {
    menu_set_open(0);

    if (index < 0) {
        return;
    }

    if (index < g_app_count) {
        launch_app(index);
        return;
    }

    switch (index - g_app_count) {
        case 0:
            gui64_show_kernel_terminal();
            break;

        case 1:
            g_text_mode_request = 1;
            break;

        case 2:
            power64_reboot();
            break;

        default:
            power64_shutdown();
            break;
    }
}

static void taskbar_click(int x) {
    if (x >= GUI64_TASK_START_X &&
        x < GUI64_TASK_START_X + GUI64_TASK_START_W) {
        menu_set_open(!g_menu_open);
        return;
    }

    if (x < GUI64_TASK_BUTTON_X) {
        return;
    }

    int index = (x - GUI64_TASK_BUTTON_X) / GUI64_TASK_BUTTON_PITCH;
    int inside = (x - GUI64_TASK_BUTTON_X) % GUI64_TASK_BUTTON_PITCH;

    if (inside >= GUI64_TASK_BUTTON_W) {
        return;
    }

    gui64_window_t* win = taskbar_window(index);

    if (!win) {
        return;
    }

    /*
     * Like Windows: a minimised window comes back, the active window is
     * minimised, any other window is brought to the front.
     */
    if (win->minimized) {
        wm_set_minimized(win, 0);
        wm_focus(win->id);
    } else if (win->active) {
        wm_set_minimized(win, 1);
    } else {
        wm_focus(win->id);
    }
}

static void desktop_select_icon(int index) {
    if (g_icon_selected == index) {
        return;
    }

    g_icon_selected = index;
    desktop_render();
    damage_all();
}

static void desktop_click(int x, int y) {
    nq_u64 now = timer64_get_ticks();
    int hit = -1;

    for (int i = 0; i < g_app_count; i++) {
        if (rect_has_point(icon_cell_rect(i), x, y)) {
            hit = i;
            break;
        }
    }

    if (hit >= 0 &&
        hit == g_last_icon_click &&
        now - g_last_icon_click_tick <= GUI64_DOUBLE_CLICK_TICKS) {
        g_last_icon_click = -1;
        launch_app(hit);
        return;
    }

    g_last_icon_click = hit;
    g_last_icon_click_tick = now;

    desktop_select_icon(hit);
}

/*
 * Window snapping: dragging a window until the pointer touches a screen
 * edge offers to fill that half of the screen (left/right) or all of it
 * (top). An outline shows what will happen; letting go does it.
 */
#define GUI64_SNAP_NONE  0
#define GUI64_SNAP_LEFT  1
#define GUI64_SNAP_RIGHT 2
#define GUI64_SNAP_TOP   3

static int g_snap = GUI64_SNAP_NONE;

static gui64_rect_t snap_rect(int snap) {
    int h = g_screen_h - GUI64_TASKBAR_H;
    int half = g_screen_w / 2;

    if (snap == GUI64_SNAP_LEFT) {
        return rect_make(0, 0, half, h);
    }

    if (snap == GUI64_SNAP_RIGHT) {
        return rect_make(half, 0, g_screen_w - half, h);
    }

    return rect_make(0, 0, g_screen_w, h);
}

static void snap_update(void) {
    int snap = GUI64_SNAP_NONE;

    if (g_cursor_x <= 1) {
        snap = GUI64_SNAP_LEFT;
    } else if (g_cursor_x >= g_screen_w - 2) {
        snap = GUI64_SNAP_RIGHT;
    } else if (g_cursor_y <= 1) {
        snap = GUI64_SNAP_TOP;
    }

    if (snap == g_snap) {
        return;
    }

    if (g_outline_visible) {
        outline_damage();
    }

    g_snap = snap;
    g_outline_visible = (snap != GUI64_SNAP_NONE);

    if (g_outline_visible) {
        g_outline = snap_rect(snap);
        outline_damage();
    }
}

static void drag_begin_move(gui64_window_t* win) {
    if (win->snapped) {
        /*
         * Pulling a snapped window away gives it its old size back, with
         * the same spot of the title bar still under the pointer.
         */
        int grab = g_cursor_x - win->x;
        int old_w = win->w > 0 ? win->w : 1;
        int new_w = win->restore.w;

        win->snapped = 0;

        wm_resize(win, win->restore.w, win->restore.h);
        wm_move(win, g_cursor_x - (int)(((nq_s64)grab * new_w) / old_w), win->y);
    }

    g_drag_mode = GUI64_DRAG_MOVE;
    g_drag_window = win->id;
    g_drag_dx = g_cursor_x - win->x;
    g_drag_dy = g_cursor_y - win->y;
    g_drag_origin = window_rect(win);
    g_snap = GUI64_SNAP_NONE;
}

static void drag_begin_resize(gui64_window_t* win) {
    g_drag_mode = GUI64_DRAG_RESIZE;
    g_drag_window = win->id;
    g_drag_edges = g_hit_edges;
    g_drag_origin = window_rect(win);
    g_drag_start_x = g_cursor_x;
    g_drag_start_y = g_cursor_y;

    g_outline = g_drag_origin;
    g_outline_visible = 1;
    outline_damage();
}

static void drag_update(void) {
    gui64_window_t* win = window_by_id(g_drag_window);

    if (!win) {
        g_drag_mode = GUI64_DRAG_NONE;
        return;
    }

    if (g_drag_mode == GUI64_DRAG_MOVE) {
        int x = g_cursor_x - g_drag_dx;
        int y = g_cursor_y - g_drag_dy;

        /*
         * Keep the title bar reachable.
         */
        int max_y = g_screen_h - GUI64_TASKBAR_H - GUI64_CONTENT_Y;

        if (y < 0) {
            y = 0;
        }

        if (y > max_y) {
            y = max_y;
        }

        if (x < 48 - win->w) {
            x = 48 - win->w;
        }

        if (x > g_screen_w - 48) {
            x = g_screen_w - 48;
        }

        wm_move(win, x, y);
        snap_update();
        return;
    }

    if (g_drag_mode == GUI64_DRAG_RESIZE) {
        /*
         * Each dragged edge follows the pointer; the opposite edge stays
         * where it was when the drag started.
         */
        int dx = g_cursor_x - g_drag_start_x;
        int dy = g_cursor_y - g_drag_start_y;

        int left = g_drag_origin.x;
        int top = g_drag_origin.y;
        int right = g_drag_origin.x + g_drag_origin.w;
        int bottom = g_drag_origin.y + g_drag_origin.h;

        if (g_drag_edges & GUI64_EDGE_LEFT) {
            left += dx;

            if (left > right - GUI64_MIN_WINDOW_W) {
                left = right - GUI64_MIN_WINDOW_W;
            }
        }

        if (g_drag_edges & GUI64_EDGE_RIGHT) {
            right += dx;

            if (right < left + GUI64_MIN_WINDOW_W) {
                right = left + GUI64_MIN_WINDOW_W;
            }
        }

        if (g_drag_edges & GUI64_EDGE_TOP) {
            top += dy;

            if (top < 0) {
                top = 0;
            }

            if (top > bottom - GUI64_MIN_WINDOW_H) {
                top = bottom - GUI64_MIN_WINDOW_H;
            }
        }

        if (g_drag_edges & GUI64_EDGE_BOTTOM) {
            bottom += dy;

            if (bottom < top + GUI64_MIN_WINDOW_H) {
                bottom = top + GUI64_MIN_WINDOW_H;
            }
        }

        if (right - left > g_screen_w) {
            right = left + g_screen_w;
        }

        if (bottom - top > g_screen_h) {
            bottom = top + g_screen_h;
        }

        gui64_rect_t r = rect_make(left, top, right - left, bottom - top);

        if (r.x != g_outline.x || r.y != g_outline.y ||
            r.w != g_outline.w || r.h != g_outline.h) {
            outline_damage();
            g_outline = r;
            outline_damage();
        }
    }
}

static void drag_end(void) {
    gui64_window_t* win = window_by_id(g_drag_window);

    if (g_drag_mode == GUI64_DRAG_RESIZE) {
        outline_damage();
        g_outline_visible = 0;

        if (win) {
            wm_move(win, g_outline.x, g_outline.y);
            wm_resize(win, g_outline.w, g_outline.h);
        }
    }

    if (g_drag_mode == GUI64_DRAG_MOVE && g_snap != GUI64_SNAP_NONE) {
        outline_damage();
        g_outline_visible = 0;

        if (win) {
            if (g_snap == GUI64_SNAP_TOP) {
                /*
                 * Put it back where the drag began first, so that is the
                 * place it returns to when un-maximised.
                 */
                wm_move(win, g_drag_origin.x, g_drag_origin.y);
                wm_set_maximized(win, 1);
            } else {
                gui64_rect_t r = snap_rect(g_snap);

                win->restore = g_drag_origin;
                win->snapped = 1;

                wm_move(win, r.x, r.y);
                wm_resize(win, r.w, r.h);
            }
        }

        g_snap = GUI64_SNAP_NONE;
    }

    g_drag_mode = GUI64_DRAG_NONE;
    g_drag_window = 0;
}

static void mouse_left_press(void) {
    int x = g_cursor_x;
    int y = g_cursor_y;

    if (g_menu_open) {
        if (rect_has_point(menu_rect(), x, y)) {
            menu_activate(menu_item_at(x, y));
            return;
        }

        /*
         * Clicking the start button while the menu is open just closes it.
         */
        int on_start =
            y >= g_screen_h - GUI64_TASKBAR_H &&
            x >= GUI64_TASK_START_X &&
            x < GUI64_TASK_START_X + GUI64_TASK_START_W;

        menu_set_open(0);

        if (on_start) {
            return;
        }
    }

    gui64_window_t* top = top_window();
    int taskbar_shown = !(top && window_fullscreen(top));

    if (taskbar_shown && y >= g_screen_h - GUI64_TASKBAR_H) {
        taskbar_click(x);
        return;
    }

    gui64_window_t* win = window_at(x, y);

    if (!win) {
        desktop_click(x, y);
        return;
    }

    if (!win->active) {
        wm_focus(win->id);
    }

    int part = window_part_at(win, x - win->x, y - win->y);

    switch (part) {
        case GUI64_PART_CLOSE:
            wm_request_close(win);
            break;

        case GUI64_PART_MAXIMIZE:
            wm_set_maximized(win, !win->maximized);
            break;

        case GUI64_PART_MINIMIZE:
            wm_set_minimized(win, 1);
            break;

        case GUI64_PART_RESIZE:
            drag_begin_resize(win);
            break;

        case GUI64_PART_LOG:
            console_open_for_pid(win->owner_pid);
            break;

        case GUI64_PART_TITLE: {
            nq_u64 now = timer64_get_ticks();

            if (g_last_title_click_window == win->id &&
                now - g_last_title_click_tick <= GUI64_DOUBLE_CLICK_TICKS) {
                g_last_title_click_window = 0;
                wm_set_maximized(win, !win->maximized);
                break;
            }

            g_last_title_click_window = win->id;
            g_last_title_click_tick = now;

            if (!win->maximized) {
                drag_begin_move(win);
            }

            break;
        }

        case GUI64_PART_CONTENT:
            g_capture_window = win->id;
            send_mouse_event(win, GUI64_MOUSE_PRESS, GUI64_MOUSE_BUTTON_LEFT);
            break;

        default:
            break;
    }
}

static void mouse_left_release(void) {
    if (g_drag_mode != GUI64_DRAG_NONE) {
        drag_end();
        return;
    }

    if (g_capture_window) {
        send_mouse_event(
            window_by_id(g_capture_window),
            GUI64_MOUSE_RELEASE,
            GUI64_MOUSE_BUTTON_LEFT
        );

        if (!(g_buttons & ~GUI64_MOUSE_BUTTON_LEFT)) {
            g_capture_window = 0;
        }
    }
}

/*
 * Right and middle buttons are only meaningful inside an app's content.
 */
static void mouse_other_button(nq_u32 button, int pressed) {
    gui64_window_t* win = window_by_id(g_capture_window);

    if (!win && pressed) {
        win = window_at(g_cursor_x, g_cursor_y);

        if (!win ||
            window_part_at(win, g_cursor_x - win->x, g_cursor_y - win->y) !=
                GUI64_PART_CONTENT) {
            return;
        }

        if (!win->active) {
            wm_focus(win->id);
        }

        g_capture_window = win->id;
    }

    if (!win) {
        return;
    }

    send_mouse_event(
        win,
        pressed ? GUI64_MOUSE_PRESS : GUI64_MOUSE_RELEASE,
        button
    );

    if (!pressed && !g_buttons) {
        g_capture_window = 0;
    }
}

static void mouse_moved(void) {
    if (g_drag_mode != GUI64_DRAG_NONE) {
        drag_update();
        return;
    }

    if (g_menu_open) {
        int hover = menu_item_at(g_cursor_x, g_cursor_y);

        if (hover != g_menu_hover) {
            g_menu_hover = hover;
            menu_render();
        }

        return;
    }

    if (g_capture_window) {
        send_mouse_event(window_by_id(g_capture_window), GUI64_MOUSE_MOVE, 0);
        return;
    }

    /*
     * No button held: the focused app sees the pointer while it is over
     * its own content and nothing is on top of it there.
     */
    gui64_window_t* win = window_at(g_cursor_x, g_cursor_y);

    if (win && win->active &&
        window_part_at(win, g_cursor_x - win->x, g_cursor_y - win->y) ==
            GUI64_PART_CONTENT) {
        send_mouse_event(win, GUI64_MOUSE_MOVE, 0);
    }
}

/*
 * The wheel scrolls whatever app is under the pointer, or the focused one
 * if the pointer is not over an app's content.
 */
static void mouse_wheel(int steps) {
    gui64_window_t* win = window_at(g_cursor_x, g_cursor_y);

    if (!win ||
        window_part_at(win, g_cursor_x - win->x, g_cursor_y - win->y) !=
            GUI64_PART_CONTENT) {
        win = focused_window();
    }

    send_mouse_event(win, GUI64_MOUSE_WHEEL, (nq_u32)steps);
}

static void handle_mouse(void) {
    mouse64_event_t ev;

    while (mouse64_poll(&ev)) {
        int x = g_cursor_x + ev.dx;
        int y = g_cursor_y + ev.dy;

        if (ev.absolute) {
            /*
             * A virtual machine's pointer reports where it is on the screen
             * (0..65535 on each axis) instead of how far it moved, so our
             * cursor sits exactly under the host's.
             */
            x = (int)(((nq_u64)ev.abs_x * (nq_u64)g_screen_w) >> 16);
            y = (int)(((nq_u64)ev.abs_y * (nq_u64)g_screen_h) >> 16);
        }

        if (x < 0) {
            x = 0;
        }

        if (y < 0) {
            y = 0;
        }

        if (x > g_screen_w - 1) {
            x = g_screen_w - 1;
        }

        if (y > g_screen_h - 1) {
            y = g_screen_h - 1;
        }

        nq_u8 old_buttons = g_buttons;

        if (x != g_cursor_x || y != g_cursor_y) {
            damage_add(cursor_rect());

            g_cursor_x = x;
            g_cursor_y = y;

            damage_add(cursor_rect());
            mouse_moved();
        }

        g_buttons = ev.buttons;

        nq_u8 changed = (nq_u8)(old_buttons ^ ev.buttons);

        if (changed & MOUSE64_BUTTON_LEFT) {
            if (ev.buttons & MOUSE64_BUTTON_LEFT) {
                mouse_left_press();
            } else {
                mouse_left_release();
            }
        }

        if (changed & MOUSE64_BUTTON_RIGHT) {
            mouse_other_button(
                GUI64_MOUSE_BUTTON_RIGHT,
                (ev.buttons & MOUSE64_BUTTON_RIGHT) != 0
            );
        }

        if (changed & MOUSE64_BUTTON_MIDDLE) {
            mouse_other_button(
                GUI64_MOUSE_BUTTON_MIDDLE,
                (ev.buttons & MOUSE64_BUTTON_MIDDLE) != 0
            );
        }

        if (ev.wheel) {
            mouse_wheel(ev.wheel);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Keyboard                                                                    */
/* -------------------------------------------------------------------------- */

static void shell_queue_push(char c) {
    nq_u32 next = (g_shell_tail + 1u) % GUI64_SHELL_QUEUE_SIZE;

    if (next == g_shell_head) {
        return;
    }

    g_shell_queue[g_shell_tail] = c;
    g_shell_tail = next;
}

static void handle_keyboard(void) {
    for (;;) {
        int c = kinput64_poll_char();

        if (c == 0) {
            break;
        }

        /*
         * Desktop hotkeys:
         *   Alt+Tab next window
         *   Ctrl+T  back to text mode
         *   Ctrl+Q  close the focused window
         *   Esc     close the start menu
         */
        if (c == 27 && g_menu_open) {
            menu_set_open(0);
            continue;
        }

        if (c == KINPUT64_KEY_ALT_TAB) {
            gui64_next_window();
            continue;
        }

        if (c == 20) {
            g_text_mode_request = 1;
            continue;
        }

        if (c == 17) {
            gui64_push_close_to_focused();
            continue;
        }

        gui64_window_t* win = focused_window();

        if (win && win->kind == GUI64_WINDOW_APP) {
            event_push_simple(win, GUI64_EVENT_KEY, (nq_u32)(nq_u8)c);
            continue;
        }

        if (win && win->kind == GUI64_WINDOW_TERMINAL) {
            if (win->owner_pid > 0) {
                /*
                 * An app's console: the keys are that program's input.
                 */
                process64_push_key_to_pid((nq_u32)win->owner_pid, (char)c);
            } else if (win->terminal == terminal64_kernel()) {
                shell_queue_push((char)c);
            }
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Desktop task                                                                */
/* -------------------------------------------------------------------------- */

static void tick_locked(void) {
    nq_u64 now = timer64_get_ticks();

    /*
     * Clean up after apps that exited so their windows disappear without
     * waiting for the shell.
     */
    process64_reap_zombies();

    handle_mouse();
    handle_keyboard();

    /*
     * Apps that ignore the close request are ended after a grace period.
     */
    for (int i = 0; i < GUI64_MAX_WINDOWS; i++) {
        gui64_window_t* win = &g_windows[i];

        if (!win->used || !win->close_requested_tick) {
            continue;
        }

        if (now - win->close_requested_tick >= GUI64_CLOSE_GRACE_TICKS) {
            win->close_requested_tick = 0;

            if (win->owner_pid > 0) {
                process64_kill((nq_u32)win->owner_pid);
            }
        }
    }

    if (now - g_console_scan_tick >= 100) {
        g_console_scan_tick = now;
        console_scan();
    }

    if (terminal64_take_gui_dirty()) {
        for (int i = 0; i < GUI64_MAX_WINDOWS; i++) {
            gui64_window_t* win = &g_windows[i];

            if (!win->used || win->kind != GUI64_WINDOW_TERMINAL) {
                continue;
            }

            if (win->minimized) {
                win->content_stale = 1;
            } else {
                window_render_terminal(win);
            }
        }
    }

    if (now - g_clock_checked_tick >= GUI64_CLOCK_TICKS ||
        g_clock_checked_tick == 0) {
        g_clock_checked_tick = now ? now : 1;

        if (clock_update()) {
            g_taskbar_dirty = 1;
        }
    }

    for (int i = 0; i < GUI64_MAX_WINDOWS; i++) {
        gui64_window_t* win = &g_windows[i];

        if (win->used && win->chrome_dirty) {
            window_draw_chrome(win);
            damage_window(win);
        }
    }

    if (g_taskbar_dirty) {
        taskbar_render();
    }

    compose_damage();
}

void gui64_tick(void) {
    GUI64_LOCK();

    if (g_active) {
        tick_locked();
    }

    GUI64_UNLOCK();
}

static void desktop_task(void* arg) {
    (void)arg;

    for (;;) {
        if (!g_active) {
            process64_sleep_ticks(50);
            continue;
        }

        gui64_tick();
        process64_sleep_ticks(GUI64_FRAME_TICKS);
    }
}

/* -------------------------------------------------------------------------- */
/* Lifecycle                                                                   */
/* -------------------------------------------------------------------------- */

void gui64_init(void) {
    mem_zero64(g_windows, sizeof(g_windows));

    g_z_count = 0;
    g_next_id = 1;
    g_active_id = 0;
    g_terminal_window_id = 0;
    g_cascade = 0;
    g_active = 0;
    g_task_started = 0;
    g_text_mode_request = 0;
    g_damage_count = 0;
    g_menu_open = 0;
    g_drag_mode = GUI64_DRAG_NONE;
    g_capture_window = 0;
    g_shell_head = 0;
    g_shell_tail = 0;

    mem_zero64(g_gui_pids, sizeof(g_gui_pids));
}

/*
 * (Re)creates the screen-sized surfaces if the display size changed.
 */
static int screen_prepare(void) {
    int w = (int)fb64_width();
    int h = (int)fb64_height();

    if (!fb64_available() || w <= 0 || h <= GUI64_TASKBAR_H) {
        return 0;
    }

    if (g_back.px && g_screen_w == w && g_screen_h == h) {
        return 1;
    }

    surface_free(&g_back);
    surface_free(&g_wallpaper);
    surface_free(&g_desktop);
    surface_free(&g_taskbar);

    g_screen_w = w;
    g_screen_h = h;

    if (!surface_alloc(&g_back, w, h) ||
        !surface_alloc(&g_wallpaper, w, h) ||
        !surface_alloc(&g_desktop, w, h) ||
        !surface_alloc(&g_taskbar, w, GUI64_TASKBAR_H)) {
        surface_free(&g_back);
        surface_free(&g_wallpaper);
        surface_free(&g_desktop);
        surface_free(&g_taskbar);
        return 0;
    }

    wallpaper_render();
    wallpaper_grade();

    return 1;
}

void gui64_enter(void) {
    GUI64_LOCK();

    if (!screen_prepare()) {
        GUI64_UNLOCK();
        k64_print("gui64: no usable framebuffer\n");
        return;
    }

    if (!g_cursor_placed) {
        g_cursor_x = g_screen_w / 2;
        g_cursor_y = g_screen_h / 2;
        g_cursor_placed = 1;
    }

    app_list_load();
    desktop_render();

    /*
     * The start-up sound, the first time the desktop appears.
     */
    static int chimed = 0;

    if (!chimed) {
        chimed = 1;
        sound64_play(SOUND64_STARTUP);
    }

    g_active = 1;
    g_text_mode_request = 0;
    g_menu_open = 0;
    g_taskbar_dirty = 1;

    /*
     * The desktop owns the keyboard from here; do not replay keys typed
     * before the switch.
     */
    g_shell_head = 0;
    g_shell_tail = 0;

    terminal64_set_gui_mode(1);
    terminal64_mark_gui_dirty();

    /*
     * Open the shell's terminal the first time (or if it was closed).
     * Coming back from text mode with windows already open keeps them in
     * the order they were left in.
     */
    if (!window_by_id(g_terminal_window_id)) {
        gui64_show_kernel_terminal();
    }

    for (int i = 0; i < GUI64_MAX_WINDOWS; i++) {
        if (g_windows[i].used) {
            g_windows[i].chrome_dirty = 1;
        }
    }

    damage_all();
    tick_locked();

    if (!g_task_started) {
        if (process64_create_kernel_task_ex("desktop", desktop_task, 0, 0) > 0) {
            g_task_started = 1;
        }
    }

    GUI64_UNLOCK();
}

void gui64_exit(void) {
    GUI64_LOCK();

    g_active = 0;
    g_damage_count = 0;
    g_menu_open = 0;
    g_drag_mode = GUI64_DRAG_NONE;
    g_outline_visible = 0;
    g_capture_window = 0;
    g_text_mode_request = 0;

    terminal64_set_gui_mode(0);

    GUI64_UNLOCK();
}

int gui64_active(void) {
    return g_active;
}

void gui64_request_repaint(void) {
    GUI64_LOCK();
    damage_all();
    GUI64_UNLOCK();
}

void gui64_render(void) {
    GUI64_LOCK();

    if (g_active) {
        damage_all();
        tick_locked();
    }

    GUI64_UNLOCK();
}

void gui64_flush_repaint(void) {
    /*
     * The desktop task repaints on its own. Before it exists (or if it
     * could not be created) fall back to repainting from the caller.
     */
    if (!g_task_started) {
        gui64_tick();
    }
}

void gui64_render_terminal_windows(void) {
    terminal64_mark_gui_dirty();
}

int gui64_take_text_mode_request(void) {
    int requested = g_text_mode_request;

    g_text_mode_request = 0;

    return requested;
}

int gui64_shell_key_pop(void) {
    int c = 0;

    GUI64_LOCK();

    if (g_shell_head != g_shell_tail) {
        c = (int)(nq_u8)g_shell_queue[g_shell_head];
        g_shell_head = (g_shell_head + 1u) % GUI64_SHELL_QUEUE_SIZE;
    }

    GUI64_UNLOCK();

    return c;
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
    if (!terminal || w < GUI64_MIN_WINDOW_W || h < GUI64_MIN_WINDOW_H) {
        return -1;
    }

    GUI64_LOCK();

    gui64_window_t* win = window_alloc(
        GUI64_WINDOW_TERMINAL,
        title ? title : "Terminal",
        x,
        y,
        w,
        h,
        GUI64_WIN_FLAG_BORDERED
    );

    int id = -1;

    if (win) {
        win->terminal = terminal;
        id = win->id;

        window_render_terminal(win);
        wm_focus(id);
        damage_window(win);
    }

    GUI64_UNLOCK();

    return id;
}

int gui64_show_kernel_terminal(void) {
    GUI64_LOCK();

    gui64_window_t* existing = window_by_id(g_terminal_window_id);

    if (existing) {
        if (existing->minimized) {
            wm_set_minimized(existing, 0);
        }

        wm_focus(existing->id);

        GUI64_UNLOCK();
        return existing->id;
    }

    int screen_w = g_screen_w > 0 ? g_screen_w : (int)fb64_width();
    int screen_h = g_screen_h > 0 ? g_screen_h : (int)fb64_height();

    int w = screen_w > 900 ? 720 : screen_w - 80;
    int h = screen_h > 650 ? 430 : screen_h - 150;

    if (w < 240) {
        w = 240;
    }

    if (h < 160) {
        h = 160;
    }

    g_terminal_window_id = gui64_create_terminal_window(
        terminal64_kernel(),
        "KER64 Terminal",
        120,
        52,
        w,
        h
    );

    int id = g_terminal_window_id;

    GUI64_UNLOCK();

    return id;
}

int gui64_has_focused_terminal_window(void) {
    gui64_window_t* win = focused_window();

    return win && win->kind == GUI64_WINDOW_TERMINAL && win->terminal != 0;
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
    (void)x;
    (void)y;

    if (owner_pid <= 0) {
        return -1;
    }

    GUI64_LOCK();

    int screen_w = g_screen_w > 0 ? g_screen_w : (int)fb64_width();
    int screen_h = g_screen_h > 0 ? g_screen_h : (int)fb64_height();

    int outer_w = w;
    int outer_h = h;
    int px = 0;
    int py = 0;

    if (flags & GUI64_WIN_FLAG_FULLSCREEN) {
        flags |= GUI64_WIN_FLAG_BORDERLESS;
        outer_w = screen_w;
        outer_h = screen_h;
    } else {
        if (w <= 0 || h <= 0 || w > 3840 || h > 2160) {
            GUI64_UNLOCK();
            return -1;
        }

        if (!(flags & GUI64_WIN_FLAG_BORDERLESS)) {
            outer_w = w + GUI64_FRAME * 2;
            outer_h = h + GUI64_CONTENT_Y + GUI64_FRAME;
        }

        /*
         * Cascade new windows so they do not stack exactly on each other.
         */
        int step = g_cascade++ % 8;

        px = 200 + step * 36;
        py = 70 + step * 30;

        if (px + outer_w > screen_w) {
            px = screen_w - outer_w;
        }

        if (py + outer_h > screen_h - GUI64_TASKBAR_H) {
            py = screen_h - GUI64_TASKBAR_H - outer_h;
        }

        if (px < 0) {
            px = 0;
        }

        if (py < 0) {
            py = 0;
        }
    }

    if (outer_w <= 0 || outer_h <= 0) {
        GUI64_UNLOCK();
        return -1;
    }

    gui64_window_t* win = window_alloc(
        GUI64_WINDOW_APP,
        title ? title : "App",
        px,
        py,
        outer_w,
        outer_h,
        flags
    );

    int id = -1;

    if (win) {
        win->owner_pid = owner_pid;
        id = win->id;

        gui_pid_mark(owner_pid);

        wm_focus(id);
        damage_window(win);
    }

    GUI64_UNLOCK();

    if (id > 0) {
        sound64_play(SOUND64_OPEN);
    }

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
    int ok = 0;

    GUI64_LOCK();

    gui64_window_t* win = window_by_id(id);

    if (win && pixels && win->surface.px &&
        (win->owner_pid == requester_pid || requester_pid == 0)) {
        gui64_rect_t c = window_content_local(win);

        if (c.w > 0 && c.h > 0 && width > 0 && height > 0) {
            if ((int)width == c.w && (int)height == c.h) {
                /*
                 * Usual case, one copy: straight from the app's frame into
                 * the window surface.
                 */
                for (int row = 0; row < c.h; row++) {
                    copy32(
                        win->surface.px +
                            (nq_u64)(c.y + row) * (nq_u64)win->surface.w + c.x,
                        pixels + (nq_u64)row * (nq_u64)width,
                        (nq_u64)c.w
                    );
                }
            } else {
                /*
                 * The frame is not the size of the content area (the window
                 * was resized or maximised and the app still renders at its
                 * own resolution): stretch it to fit, nearest neighbour.
                 * 16.16 fixed-point steps through the source.
                 */
                nq_u64 step_x = ((nq_u64)width << 16) / (nq_u64)c.w;
                nq_u64 step_y = ((nq_u64)height << 16) / (nq_u64)c.h;

                for (int row = 0; row < c.h; row++) {
                    nq_u64 sy = ((nq_u64)row * step_y) >> 16;

                    if (sy >= height) {
                        sy = height - 1u;
                    }

                    const nq_u32* src = pixels + sy * (nq_u64)width;
                    nq_u32* dst =
                        win->surface.px +
                        (nq_u64)(c.y + row) * (nq_u64)win->surface.w + c.x;
                    nq_u64 fx = 0;

                    for (int col = 0; col < c.w; col++) {
                        nq_u64 sx = fx >> 16;

                        dst[col] = src[sx < width ? sx : width - 1u];
                        fx += step_x;
                    }
                }
            }

            if (window_visible(win)) {
                damage_add(rect_make(win->x + c.x, win->y + c.y, c.w, c.h));
            }
        }

        ok = 1;
    }

    GUI64_UNLOCK();

    return ok;
}

int gui64_set_window_title(int id, int requester_pid, const char* title) {
    int ok = 0;

    GUI64_LOCK();

    gui64_window_t* win = window_by_id(id);

    if (win && title &&
        (win->owner_pid == requester_pid || requester_pid == 0)) {
        str_copy64(win->title, title, sizeof(win->title));
        win->chrome_dirty = 1;
        g_taskbar_dirty = 1;
        ok = 1;
    }

    GUI64_UNLOCK();

    return ok;
}

int gui64_destroy_window(int id, int requester_pid) {
    int ok = 0;

    GUI64_LOCK();

    gui64_window_t* win = window_by_id(id);

    if (win && (win->owner_pid == requester_pid || requester_pid == 0)) {
        wm_destroy(win);
        ok = 1;
    }

    GUI64_UNLOCK();

    if (ok) {
        sound64_play(SOUND64_CLOSE);
    }

    return ok;
}

void gui64_destroy_windows_for_pid(int owner_pid) {
    if (owner_pid <= 0) {
        return;
    }

    GUI64_LOCK();

    for (int i = 0; i < GUI64_MAX_WINDOWS; i++) {
        if (g_windows[i].used && g_windows[i].owner_pid == owner_pid) {
            wm_destroy(&g_windows[i]);
        }
    }

    /*
     * The process is gone; its id may be reused by a console program.
     */
    gui_pid_forget(owner_pid);

    GUI64_UNLOCK();
}

int gui64_pid_has_window(int pid) {
    if (pid <= 0) {
        return 0;
    }

    for (int i = 0; i < GUI64_MAX_WINDOWS; i++) {
        if (g_windows[i].used && g_windows[i].owner_pid == pid) {
            return 1;
        }
    }

    return 0;
}

int gui64_focus_pid_window(int pid) {
    int ok = 0;

    if (pid <= 0) {
        return 0;
    }

    GUI64_LOCK();

    for (int i = 0; i < GUI64_MAX_WINDOWS; i++) {
        gui64_window_t* win = &g_windows[i];

        if (!win->used || win->owner_pid != pid) {
            continue;
        }

        if (win->minimized) {
            wm_set_minimized(win, 0);
        }

        wm_focus(win->id);
        ok = 1;
        break;
    }

    GUI64_UNLOCK();

    return ok;
}

int gui64_window_belongs_to(int id, int pid) {
    gui64_window_t* win = window_by_id(id);

    return win && win->owner_pid == pid;
}

void gui64_set_active_window(int id) {
    GUI64_LOCK();

    gui64_window_t* win = window_by_id(id);

    if (win) {
        if (win->minimized) {
            wm_set_minimized(win, 0);
        }

        wm_focus(id);
    }

    GUI64_UNLOCK();
}

void gui64_next_window(void) {
    GUI64_LOCK();

    /*
     * Step through windows in slot order, starting after the focused one,
     * so repeated presses visit every window.
     */
    int start = -1;

    for (int i = 0; i < GUI64_MAX_WINDOWS; i++) {
        if (g_windows[i].used && g_windows[i].id == g_active_id) {
            start = i;
            break;
        }
    }

    for (int step = 1; step <= GUI64_MAX_WINDOWS; step++) {
        gui64_window_t* win =
            &g_windows[(start + step + GUI64_MAX_WINDOWS) % GUI64_MAX_WINDOWS];

        if (!win->used || win->id == g_active_id) {
            continue;
        }

        if (win->minimized) {
            wm_set_minimized(win, 0);
        }

        wm_focus(win->id);
        break;
    }

    GUI64_UNLOCK();
}

/* -------------------------------------------------------------------------- */
/* Event API                                                                   */
/* -------------------------------------------------------------------------- */

int gui64_focused_window_id(void) {
    return g_active_id;
}

int gui64_has_focused_app_window(void) {
    gui64_window_t* win = focused_window();

    return win && win->kind == GUI64_WINDOW_APP && win->owner_pid > 0;
}

int gui64_pop_event(int window_id, int requester_pid, gui64_event_t* out) {
    int ok = 0;

    if (!out) {
        return 0;
    }

    out->type = GUI64_EVENT_NONE;
    out->key = 0;
    out->x = 0;
    out->y = 0;
    out->w = 0;
    out->h = 0;

    GUI64_LOCK();

    gui64_window_t* win = window_by_id(window_id);

    if (win &&
        (win->owner_pid == requester_pid || requester_pid == 0) &&
        win->event_head != win->event_tail) {
        *out = win->events[win->event_head];
        win->event_head = (win->event_head + 1u) % GUI64_EVENT_QUEUE_SIZE;
        ok = 1;
    }

    GUI64_UNLOCK();

    return ok;
}

int gui64_push_key_to_focused(char c) {
    int ok = 0;

    GUI64_LOCK();

    gui64_window_t* win = focused_window();

    if (win && win->owner_pid > 0) {
        event_push_simple(win, GUI64_EVENT_KEY, (nq_u32)(nq_u8)c);
        ok = 1;
    }

    GUI64_UNLOCK();

    return ok;
}

int gui64_push_close_to_focused(void) {
    int ok = 0;

    GUI64_LOCK();

    gui64_window_t* win = focused_window();

    if (win) {
        wm_request_close(win);
        ok = 1;
    }

    GUI64_UNLOCK();

    return ok;
}

/* -------------------------------------------------------------------------- */
/* Settings                                                                    */
/* -------------------------------------------------------------------------- */

#define GUI64_SETTINGS_PATH "/usb/NAQOS.CFG"

/*
 * Wallpaper variants are the daytime scene with different light: the
 * finished picture is re-coloured, then given a moon and stars at night.
 */
static void wallpaper_grade(void) {
    if (!g_wallpaper.px || g_set_wallpaper == GUI64_WALLPAPER_DAY) {
        return;
    }

    int w = g_screen_w;
    int h = g_screen_h;
    int horizon = (h * 62) / 100;

    for (int y = 0; y < h; y++) {
        nq_u32* row = g_wallpaper.px + (nq_u64)y * (nq_u64)w;

        /*
         * 0 at the top of the sky, 255 at the horizon. The land gets a
         * gentler version of the horizon's light.
         */
        nq_u32 t = y < horizon ? (nq_u32)((y * 255) / horizon) : 150u;

        for (int x = 0; x < w; x++) {
            nq_u32 c = row[x];
            nq_u32 r = (c >> 16) & 0xFFu;
            nq_u32 g = (c >> 8) & 0xFFu;
            nq_u32 b = c & 0xFFu;

            if (g_set_wallpaper == GUI64_WALLPAPER_SUNSET) {
                /*
                 * Warm light: red lifted, green and blue held back, more
                 * so toward the horizon.
                 */
                r = r + ((255u - r) * (40u + t / 3u)) / 255u;
                g = (g * (215u - t / 5u)) / 255u;
                b = (b * (190u - t / 3u)) / 255u;
            } else {
                /*
                 * Night: everything dim, blue surviving best.
                 */
                r = (r * 46u) / 255u;
                g = (g * 66u) / 255u;
                b = (b * 118u) / 255u + 6u;
            }

            row[x] = rgb(r, g, b);
        }
    }

    if (g_set_wallpaper != GUI64_WALLPAPER_NIGHT) {
        return;
    }

    draw_to(&g_wallpaper);

    /*
     * Stars, only where the sky is clear (dark).
     */
    nq_u32 seed = 0x5EED1234u;
    int stars = (w * horizon) / 2600;

    for (int i = 0; i < stars; i++) {
        int x = (int)(wallpaper_random(&seed) % (nq_u32)w);
        int y = (int)(wallpaper_random(&seed) % (nq_u32)((horizon * 8) / 10 + 1));
        nq_u32 c = g_wallpaper.px[(nq_u64)y * (nq_u64)w + x];

        if ((((c >> 16) & 0xFFu) + ((c >> 8) & 0xFFu) + (c & 0xFFu)) > 150u) {
            continue;
        }

        nq_u32 brightness = 110u + wallpaper_random(&seed) % 146u;

        blend_pixel(x, y, 0x00FFFFFFu, brightness);

        if ((wallpaper_random(&seed) & 7u) == 0) {
            blend_pixel(x + 1, y, 0x00FFFFFFu, brightness / 2u);
            blend_pixel(x, y + 1, 0x00FFFFFFu, brightness / 2u);
        }
    }

    /*
     * The moon, where the sun was.
     */
    int moon_x = (w * 86) / 100;
    int moon_y = (h * 12) / 100;

    disc(moon_x, moon_y, h / 7, rgb(170, 200, 255), 22);
    disc(moon_x, moon_y, h / 11, rgb(190, 214, 255), 34);
    disc(moon_x, moon_y, h / 24, rgb(240, 244, 255), 255);
    disc(moon_x - h / 80, moon_y - h / 90, h / 110, rgb(214, 222, 240), 255);
    disc(moon_x + h / 70, moon_y + h / 120, h / 150, rgb(218, 226, 242), 255);
}

/*
 * Redraws everything a setting can affect. Call with the GUI lock held.
 */
static void settings_refresh(void) {
    if (!g_wallpaper.px) {
        /*
         * The desktop has not been shown yet; it will pick the settings
         * up when it is.
         */
        return;
    }

    wallpaper_render();
    wallpaper_grade();
    desktop_render();

    for (int i = 0; i < GUI64_MAX_WINDOWS; i++) {
        if (g_windows[i].used) {
            g_windows[i].chrome_dirty = 1;
        }
    }

    g_taskbar_dirty = 1;
    g_clock_checked_tick = 0;

    if (g_menu_open) {
        menu_render();
    }

    damage_all();
}

static void settings_append(char* out, nq_u32 size, const char* key, int value) {
    nq_u32 n = 0;

    while (out[n]) {
        n++;
    }

    for (nq_u32 i = 0; key[i] && n + 1u < size; i++) {
        out[n++] = key[i];
    }

    if (n + 4u < size) {
        out[n++] = '=';
        out[n++] = (char)('0' + (value % 10));
        out[n++] = '\n';
    }

    out[n] = 0;
}

/*
 * Writes the settings to the boot stick. Returns 1 if they were saved.
 */
static int settings_save(void) {
    char text[128];

    /*
     * Ask first, so a missing or locked stick is not reported as an error
     * on the calling program's console.
     */
    if (!vfs64_usb_mounted() || vfs64_write_locked(GUI64_SETTINGS_PATH)) {
        return 0;
    }

    text[0] = 0;

    settings_append(text, sizeof(text), "wallpaper", g_set_wallpaper);
    settings_append(text, sizeof(text), "accent", g_set_accent);
    settings_append(text, sizeof(text), "clock12", g_set_clock12);
    settings_append(text, sizeof(text), "volume", (int)sound64_volume());
    settings_append(text, sizeof(text), "sounds", sound64_effects_on());

    return vfs64_write_file(GUI64_SETTINGS_PATH, text, str_len64(text));
}

static int settings_key_is(const char* line, const char* key) {
    int i = 0;

    while (key[i]) {
        if (line[i] != key[i]) {
            return 0;
        }

        i++;
    }

    return line[i] == '=';
}

void gui64_settings_load(void) {
    char text[192];
    vfs64_stat_t st;
    nq_u64 size = 0;

    if (!vfs64_usb_mounted() ||
        !vfs64_stat(GUI64_SETTINGS_PATH, &st) ||
        st.type != VFS64_TYPE_FILE ||
        st.size >= sizeof(text)) {
        return;
    }

    if (!vfs64_read_file(GUI64_SETTINGS_PATH, text, sizeof(text) - 1u, &size)) {
        return;
    }

    text[size] = 0;

    GUI64_LOCK();

    for (nq_u64 i = 0; i < size; ) {
        const char* line = &text[i];
        nq_u64 end = i;

        while (end < size && text[end] != '\n') {
            end++;
        }

        /*
         * "key=D" with a single digit.
         */
        for (nq_u64 k = i; k + 1u < end; k++) {
            if (text[k] == '=' && text[k + 1u] >= '0' && text[k + 1u] <= '9') {
                int value = text[k + 1u] - '0';

                if (settings_key_is(line, "wallpaper") && value < GUI64_WALLPAPER_COUNT) {
                    g_set_wallpaper = value;
                } else if (settings_key_is(line, "accent") && value < GUI64_ACCENT_COUNT) {
                    g_set_accent = value;
                } else if (settings_key_is(line, "clock12") && value <= 1) {
                    g_set_clock12 = value;
                } else if (settings_key_is(line, "volume")) {
                    sound64_set_volume((nq_u32)value);
                } else if (settings_key_is(line, "sounds") && value <= 1) {
                    sound64_set_effects(value);
                }

                break;
            }
        }

        i = end + 1u;
    }

    settings_refresh();

    GUI64_UNLOCK();
}

int gui64_setting_get(int id) {
    switch (id) {
        case GUI64_SETTING_WALLPAPER: return g_set_wallpaper;
        case GUI64_SETTING_ACCENT:    return g_set_accent;
        case GUI64_SETTING_CLOCK12:   return g_set_clock12;
        case GUI64_SETTING_VOLUME:    return (int)sound64_volume();
        case GUI64_SETTING_SOUNDS:    return sound64_effects_on();
        default:                      return -1;
    }
}

int gui64_setting_set(int id, int value) {
    if (value < 0) {
        return 0;
    }

    GUI64_LOCK();

    int ok = 1;

    if (id == GUI64_SETTING_WALLPAPER && value < GUI64_WALLPAPER_COUNT) {
        g_set_wallpaper = value;
    } else if (id == GUI64_SETTING_ACCENT && value < GUI64_ACCENT_COUNT) {
        g_set_accent = value;
    } else if (id == GUI64_SETTING_CLOCK12 && value <= 1) {
        g_set_clock12 = value;
    } else if (id == GUI64_SETTING_VOLUME && value <= 9) {
        sound64_set_volume((nq_u32)value);
    } else if (id == GUI64_SETTING_SOUNDS && value <= 1) {
        sound64_set_effects(value);
    } else {
        ok = 0;
    }

    if (ok) {
        settings_refresh();
    }

    GUI64_UNLOCK();

    if (!ok) {
        return 0;
    }

    return settings_save() ? 2 : 1;
}
