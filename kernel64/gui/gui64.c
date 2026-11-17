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
#define GUI64_TASKBAR_H 34
#define GUI64_SHADOW 6

#define GUI64_MIN_WINDOW_W 160
#define GUI64_MIN_WINDOW_H 96

#define GUI64_BUTTON_W 18
#define GUI64_BUTTON_H 16
#define GUI64_BUTTON_Y 7
#define GUI64_BUTTON_GAP 2

#define GUI64_RESIZE_CORNER 14

#define GUI64_FRAME_TICKS 10ull         /* desktop task period                */
#define GUI64_DOUBLE_CLICK_TICKS 450ull
#define GUI64_CLOSE_GRACE_TICKS 2000ull /* then an unresponsive app is killed */
#define GUI64_CLOCK_TICKS 1000ull

#define GUI64_MAX_DAMAGE 24
#define GUI64_MAX_APPS 16
#define GUI64_APP_NAME_MAX 32
#define GUI64_SHELL_QUEUE_SIZE 256u

#define GUI64_TASK_START_X 8
#define GUI64_TASK_START_W 58
#define GUI64_TASK_BUTTON_X 74
#define GUI64_TASK_BUTTON_W 118
#define GUI64_TASK_BUTTON_PITCH 124
#define GUI64_TASK_CLOCK_W 62

#define GUI64_MENU_W 196
#define GUI64_MENU_HEADER_H 28
#define GUI64_MENU_ITEM_H 24
#define GUI64_MENU_FIXED_ITEMS 4

#define GUI64_ICON_CELL_W 88
#define GUI64_ICON_CELL_H 84
#define GUI64_ICON_SIZE 40
#define GUI64_ICON_ORIGIN_X 16
#define GUI64_ICON_ORIGIN_Y 44

#define GUI64_CURSOR_W 12
#define GUI64_CURSOR_H 19

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
    gui64_rect_t restore;

    char title[32];

    /*
     * w x h image of the window: frame, title bar and content.
     */
    gui64_surface_t surface;
    int chrome_dirty;
    int content_stale;

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

static char g_clock_text[8] = "--:--";
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

/*
 * Title-bar button rectangle in window-local coordinates.
 * which: GUI64_PART_CLOSE / MAXIMIZE / MINIMIZE.
 */
static gui64_rect_t window_button_rect(const gui64_window_t* win, int which) {
    int slot = 0;

    if (which == GUI64_PART_MAXIMIZE) {
        slot = 1;
    } else if (which == GUI64_PART_MINIMIZE) {
        slot = 2;
    }

    int x =
        win->w - GUI64_FRAME - 4 - GUI64_BUTTON_W -
        slot * (GUI64_BUTTON_W + GUI64_BUTTON_GAP);

    return rect_make(x, GUI64_BUTTON_Y, GUI64_BUTTON_W, GUI64_BUTTON_H);
}

static void draw_title_button(gui64_rect_t r, int which, int active) {
    nq_u32 top;
    nq_u32 bottom;

    if (which == GUI64_PART_CLOSE) {
        top = active ? rgb(255, 150, 140) : rgb(200, 160, 160);
        bottom = active ? rgb(200, 50, 50) : rgb(140, 100, 104);
    } else {
        top = active ? rgb(210, 250, 252) : rgb(190, 220, 222);
        bottom = active ? rgb(40, 150, 188) : rgb(70, 124, 150);
    }

    vgradient(r.x, r.y, r.w, r.h, top, bottom);
    border(r.x, r.y, r.w, r.h, active ? rgb(16, 78, 104) : rgb(60, 100, 116));
    fill(r.x + 1, r.y + 1, r.w - 2, 1, rgb(255, 255, 255));

    nq_u32 ink = rgb(255, 255, 255);
    int cx = r.x + r.w / 2;
    int cy = r.y + r.h / 2;

    if (which == GUI64_PART_CLOSE) {
        for (int i = -3; i <= 3; i++) {
            put_pixel(cx + i, cy + i, ink);
            put_pixel(cx + i + 1, cy + i, ink);
            put_pixel(cx + i, cy - i, ink);
            put_pixel(cx + i + 1, cy - i, ink);
        }
    } else if (which == GUI64_PART_MAXIMIZE) {
        border(cx - 4, cy - 3, 9, 7, ink);
        fill(cx - 4, cy - 3, 9, 2, ink);
    } else {
        fill(cx - 4, cy + 2, 9, 2, ink);
    }
}

/*
 * Draws the frame and title bar into the window surface. The content area
 * is left untouched.
 */
static void window_draw_chrome(gui64_window_t* win) {
    win->chrome_dirty = 0;

    if (!win->surface.px || window_borderless(win)) {
        return;
    }

    int w = win->w;
    int h = win->h;
    int active = win->active;

    draw_to(&win->surface);

    nq_u32 outer = active ? rgb(235, 255, 249) : rgb(174, 226, 220);
    nq_u32 inner = active ? rgb(113, 218, 224) : rgb(93, 169, 180);
    nq_u32 rim_dark = active ? rgb(31, 132, 151) : rgb(48, 111, 124);
    nq_u32 inner_edge = active ? rgb(232, 255, 250) : rgb(197, 235, 229);

    nq_u32 title_top = active ? rgb(255, 255, 255) : rgb(220, 245, 241);
    nq_u32 title_mid = active ? rgb(119, 226, 235) : rgb(104, 181, 192);
    nq_u32 title_bot = active ? rgb(32, 151, 192) : rgb(56, 119, 150);

    /*
     * Four 1-pixel rings make up the glass frame.
     */
    border(0, 0, w, h, outer);
    border(1, 1, w - 2, h - 2, active ? rgb(255, 255, 255) : rgb(214, 240, 236));
    border(2, 2, w - 4, h - 4, inner);
    border(3, 3, w - 6, h - 6, inner_edge);

    /*
     * Darker lower/right edge for depth.
     */
    fill(2, h - 3, w - 4, 1, rim_dark);
    fill(w - 3, 2, 1, h - 4, rim_dark);

    /*
     * Glossy title bar: light to mid over the top half, mid to dark below.
     */
    int half = GUI64_TITLE_H / 2;

    vgradient(GUI64_FRAME, GUI64_FRAME, w - GUI64_FRAME * 2, half,
              title_top, title_mid);
    vgradient(GUI64_FRAME, GUI64_FRAME + half, w - GUI64_FRAME * 2,
              GUI64_TITLE_H - half, title_mid, title_bot);

    fill(6, 6, w - 12, 1, rgb(255, 255, 255));
    fill(GUI64_FRAME, GUI64_CONTENT_Y - 1, w - GUI64_FRAME * 2, 1, rim_dark);

    /*
     * Title text, clipped so it never runs under the buttons.
     */
    gui64_rect_t min_button = window_button_rect(win, GUI64_PART_MINIMIZE);
    int max_chars = (min_button.x - 16) / GUI64_FONT_W;

    if (max_chars > 0) {
        draw_text(13, 9, win->title, rgb(22, 91, 112), max_chars);
        draw_text(12, 8, win->title, rgb(255, 255, 255), max_chars);
    }

    draw_title_button(window_button_rect(win, GUI64_PART_CLOSE),
                      GUI64_PART_CLOSE, active);
    draw_title_button(window_button_rect(win, GUI64_PART_MAXIMIZE),
                      GUI64_PART_MAXIMIZE, active);
    draw_title_button(min_button, GUI64_PART_MINIMIZE, active);

    /*
     * Resize grip in the bottom-right corner of the frame.
     */
    if (!win->maximized) {
        for (int i = 0; i < 3; i++) {
            put_pixel(w - 3 - i * 3, h - 3, rim_dark);
            put_pixel(w - 3, h - 3 - i * 3, rim_dark);
        }
    }
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

static nq_u32 icon_color(const char* name) {
    static const nq_u32 palette[6] = {
        0x00308FC8u, 0x00C8683Au, 0x0052A860u,
        0x009A62C8u, 0x00C8A030u, 0x00C84870u
    };

    nq_u32 hash = 0;

    for (int i = 0; name[i]; i++) {
        hash = hash * 31u + (nq_u8)name[i];
    }

    return palette[hash % 6u];
}

static void desktop_draw_icon(int index) {
    char label[GUI64_APP_NAME_MAX];
    gui64_rect_t cell = icon_cell_rect(index);

    app_label(g_apps[index], label, sizeof(label));

    if (index == g_icon_selected) {
        darken(cell.x, cell.y, cell.w, cell.h, 78);
        border(cell.x, cell.y, cell.w, cell.h, rgb(214, 244, 226));
    }

    int ix = cell.x + (cell.w - GUI64_ICON_SIZE) / 2;
    int iy = cell.y + 6;
    nq_u32 base = icon_color(g_apps[index]);

    /*
     * Glossy tile with the app's initial.
     */
    darken(ix + 3, iy + 3, GUI64_ICON_SIZE, GUI64_ICON_SIZE, 70);
    vgradient(ix, iy, GUI64_ICON_SIZE, GUI64_ICON_SIZE,
              mix(base, 0x00FFFFFFu, 2, 5), base);
    border(ix, iy, GUI64_ICON_SIZE, GUI64_ICON_SIZE, mix(base, 0, 2, 5));
    fill(ix + 2, iy + 2, GUI64_ICON_SIZE - 4, 1, 0x00FFFFFFu);

    draw_char(ix + 12, iy + 4, label[0], rgb(20, 40, 52), 2);
    draw_char(ix + 11, iy + 3, label[0], 0x00FFFFFFu, 2);

    int len = str_len64(label);
    int max_chars = (cell.w - 4) / GUI64_FONT_W;

    if (len > max_chars) {
        len = max_chars;
    }

    int tx = cell.x + (cell.w - len * GUI64_FONT_W) / 2;
    int ty = iy + GUI64_ICON_SIZE + 6;

    draw_text(tx + 1, ty + 1, label, rgb(34, 70, 48), len);
    draw_text(tx, ty, label, 0x00FFFFFFu, len);
}

/*
 * Renders the wallpaper and icons into the cached desktop surface.
 */
static void desktop_render(void) {
    if (!g_desktop.px) {
        return;
    }

    draw_to(&g_desktop);

    vgradient(0, 0, g_screen_w, g_screen_h,
              rgb(148, 194, 138), rgb(96, 150, 128));

    draw_text(18, 14, "NAQ OS", rgb(70, 118, 74), -1);

    for (int i = 0; i < g_app_count; i++) {
        desktop_draw_icon(i);
    }
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

    char text[8];

    text[0] = (char)('0' + hour / 10);
    text[1] = (char)('0' + hour % 10);
    text[2] = ':';
    text[3] = (char)('0' + minute / 10);
    text[4] = (char)('0' + minute % 10);
    text[5] = 0;

    for (int i = 0; i < 6; i++) {
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

static void draw_glass_button(
    int x,
    int y,
    int w,
    int h,
    const char* text,
    int pressed,
    int dim
) {
    nq_u32 top = pressed ? rgb(16, 84, 116) : rgb(96, 200, 220);
    nq_u32 bottom = pressed ? rgb(30, 118, 150) : rgb(24, 116, 150);

    if (dim) {
        top = rgb(64, 150, 170);
        bottom = rgb(30, 110, 136);
    }

    vgradient(x, y, w, h, top, bottom);
    border(x, y, w, h, rgb(12, 70, 96));

    if (!pressed) {
        fill(x + 1, y + 1, w - 2, 1, rgb(214, 250, 252));
    }

    int max_chars = (w - 8) / GUI64_FONT_W;
    int len = str_len64(text);

    if (len > max_chars) {
        len = max_chars;
    }

    if (len > 0) {
        int tx = x + (w - len * GUI64_FONT_W) / 2;
        int ty = y + (h - GUI64_FONT_H) / 2;

        draw_text(tx, ty, text, dim ? rgb(200, 232, 236) : 0x00FFFFFFu, len);
    }
}

static void taskbar_render(void) {
    g_taskbar_dirty = 0;

    if (!g_taskbar.px) {
        return;
    }

    int w = g_taskbar.w;
    int h = GUI64_TASKBAR_H;

    draw_to(&g_taskbar);

    fill(0, 0, w, h, rgb(28, 134, 162));
    vgradient(0, 1, w, 9, rgb(170, 238, 244), rgb(90, 196, 214));
    fill(0, 10, w, h - 12, rgb(35, 157, 181));
    fill(0, 0, w, 1, rgb(245, 255, 255));
    fill(0, h - 2, w, 2, rgb(16, 91, 125));

    draw_glass_button(GUI64_TASK_START_X, 6, GUI64_TASK_START_W, 22,
                      "NAQ", g_menu_open, 0);

    int limit = w - GUI64_TASK_CLOCK_W - 12;

    for (int i = 0; ; i++) {
        gui64_window_t* win = taskbar_window(i);
        int x = GUI64_TASK_BUTTON_X + i * GUI64_TASK_BUTTON_PITCH;

        if (!win || x + GUI64_TASK_BUTTON_W > limit) {
            break;
        }

        draw_glass_button(x, 6, GUI64_TASK_BUTTON_W, 22, win->title,
                          win->active && !win->minimized, win->minimized);
    }

    int cx = w - GUI64_TASK_CLOCK_W - 6;

    fill(cx, 6, GUI64_TASK_CLOCK_W, 22, rgb(22, 104, 132));
    border(cx, 6, GUI64_TASK_CLOCK_W, 22, rgb(140, 226, 236));
    draw_text(cx + (GUI64_TASK_CLOCK_W - 5 * GUI64_FONT_W) / 2, 9,
              g_clock_text, 0x00FFFFFFu, -1);

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

static int menu_item_count(void) {
    return g_app_count + GUI64_MENU_FIXED_ITEMS;
}

static gui64_rect_t menu_rect(void) {
    int h = GUI64_MENU_HEADER_H + menu_item_count() * GUI64_MENU_ITEM_H + 10;

    return rect_make(4, g_screen_h - GUI64_TASKBAR_H - h, GUI64_MENU_W, h);
}

/*
 * Screen area the open menu paints: the menu plus its shadow on the right.
 * Damage must cover all of it, or closing the menu leaves the shadow behind.
 */
static gui64_rect_t menu_damage_rect(void) {
    gui64_rect_t r = menu_rect();

    r.w += GUI64_SHADOW;

    return r;
}

/*
 * Y offset of an item inside the menu surface. The fixed items sit below a
 * separator.
 */
static int menu_item_y(int index) {
    int y = GUI64_MENU_HEADER_H + 2 + index * GUI64_MENU_ITEM_H;

    if (index >= g_app_count) {
        y += 6;
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

    fill(0, 0, r.w, r.h, rgb(232, 248, 246));
    border(0, 0, r.w, r.h, rgb(18, 96, 124));
    border(1, 1, r.w - 2, r.h - 2, rgb(255, 255, 255));

    vgradient(2, 2, r.w - 4, GUI64_MENU_HEADER_H - 2,
              rgb(120, 222, 232), rgb(32, 148, 188));
    draw_text(13, 8, "NAQ OS", rgb(18, 84, 108), -1);
    draw_text(12, 7, "NAQ OS", 0x00FFFFFFu, -1);

    for (int i = 0; i < menu_item_count(); i++) {
        char label[GUI64_APP_NAME_MAX];
        int y = menu_item_y(i);

        if (i < g_app_count) {
            app_label(g_apps[i], label, sizeof(label));
        } else {
            str_copy64(label, menu_fixed_label(i - g_app_count), sizeof(label));
        }

        if (i == g_app_count) {
            fill(8, y - 4, r.w - 16, 1, rgb(150, 196, 200));
        }

        if (i == g_menu_hover) {
            vgradient(4, y, r.w - 8, GUI64_MENU_ITEM_H - 2,
                      rgb(110, 208, 226), rgb(40, 150, 190));
            draw_text(14, y + 3, label, 0x00FFFFFFu, (r.w - 24) / GUI64_FONT_W);
        } else {
            draw_text(14, y + 3, label, rgb(20, 62, 80),
                      (r.w - 24) / GUI64_FONT_W);
        }
    }

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

static const char* const g_cursor_shape[GUI64_CURSOR_H] = {
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
    for (int y = 0; y < GUI64_CURSOR_H; y++) {
        for (int x = 0; x < GUI64_CURSOR_W; x++) {
            char c = g_cursor_shape[y][x];

            if (c == '#') {
                put_pixel(g_cursor_x + x, g_cursor_y + y, 0x00000000u);
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
     * Occlusion: every window is an opaque rectangle. If one of them covers
     * the whole damaged area, nothing below it can show through, so start
     * there and skip the desktop and the windows underneath.
     */
    int start = -1;

    for (int zi = g_z_count - 1; zi >= 0; zi--) {
        gui64_window_t* win = window_by_id(g_z[zi]);

        if (window_visible(win) && rect_contains(window_rect(win), r)) {
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
            darken(win->x + win->w, win->y + GUI64_SHADOW,
                   GUI64_SHADOW, win->h, 72);
            darken(win->x + GUI64_SHADOW, win->y + win->h,
                   win->w - GUI64_SHADOW, GUI64_SHADOW, 72);
        }

        blit(&win->surface, win->x, win->y);
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

        darken(m.x + m.w, m.y + GUI64_SHADOW, GUI64_SHADOW, m.h - GUI64_SHADOW, 72);
        blit(&g_menu, m.x, m.y);
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

/*
 * The user clicked a window's close button (or pressed the close hotkey).
 */
static void wm_request_close(gui64_window_t* win) {
    if (win->kind == GUI64_WINDOW_TERMINAL) {
        /*
         * The shell keeps running; "Terminal" in the start menu brings the
         * window back.
         */
        wm_destroy(win);
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

    if (!win->maximized) {
        if ((lx >= win->w - GUI64_RESIZE_CORNER &&
             ly >= win->h - GUI64_RESIZE_CORNER) ||
            lx >= win->w - GUI64_FRAME ||
            ly >= win->h - GUI64_FRAME) {
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

static void drag_begin_move(gui64_window_t* win) {
    g_drag_mode = GUI64_DRAG_MOVE;
    g_drag_window = win->id;
    g_drag_dx = g_cursor_x - win->x;
    g_drag_dy = g_cursor_y - win->y;
}

static void drag_begin_resize(gui64_window_t* win) {
    g_drag_mode = GUI64_DRAG_RESIZE;
    g_drag_window = win->id;

    /*
     * Offset from the cursor to the window's bottom-right corner.
     */
    g_drag_dx = win->x + win->w - g_cursor_x;
    g_drag_dy = win->y + win->h - g_cursor_y;

    g_outline = window_rect(win);
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
        return;
    }

    if (g_drag_mode == GUI64_DRAG_RESIZE) {
        int w = g_cursor_x + g_drag_dx - win->x;
        int h = g_cursor_y + g_drag_dy - win->y;

        if (w < GUI64_MIN_WINDOW_W) {
            w = GUI64_MIN_WINDOW_W;
        }

        if (h < GUI64_MIN_WINDOW_H) {
            h = GUI64_MIN_WINDOW_H;
        }

        if (w > g_screen_w) {
            w = g_screen_w;
        }

        if (h > g_screen_h) {
            h = g_screen_h;
        }

        if (w != g_outline.w || h != g_outline.h) {
            outline_damage();
            g_outline = rect_make(win->x, win->y, w, h);
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
            wm_resize(win, g_outline.w, g_outline.h);
        }
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
         *   Tab     next window
         *   Ctrl+T  back to text mode
         *   Ctrl+Q  close the focused window
         *   Esc     close the start menu
         */
        if (c == 27 && g_menu_open) {
            menu_set_open(0);
            continue;
        }

        if (c == '\t') {
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
            shell_queue_push((char)c);
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
    surface_free(&g_desktop);
    surface_free(&g_taskbar);

    g_screen_w = w;
    g_screen_h = h;

    if (!surface_alloc(&g_back, w, h) ||
        !surface_alloc(&g_desktop, w, h) ||
        !surface_alloc(&g_taskbar, w, GUI64_TASKBAR_H)) {
        surface_free(&g_back);
        surface_free(&g_desktop);
        surface_free(&g_taskbar);
        return 0;
    }

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

    gui64_show_kernel_terminal();

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

        wm_focus(id);
        damage_window(win);
    }

    GUI64_UNLOCK();

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
