#ifndef GUI64_H
#define GUI64_H

#include "naq_boot_info.h"
#include "../console/terminal64.h"

#define GUI64_MAX_WINDOWS 8
#define GUI64_EVENT_QUEUE_SIZE 32

#define GUI64_EVENT_NONE   0
#define GUI64_EVENT_KEY    1
#define GUI64_EVENT_FOCUS  2
#define GUI64_EVENT_CLOSE  3
#define GUI64_EVENT_RESIZE 4
#define GUI64_EVENT_MOUSE  5

#define GUI64_WINDOW_NORMAL   0
#define GUI64_WINDOW_APP      1
#define GUI64_WINDOW_TERMINAL 2

#define GUI64_WIN_FLAG_BORDERED    0x00000000u
#define GUI64_WIN_FLAG_BORDERLESS  0x00000001u
#define GUI64_WIN_FLAG_FULLSCREEN  0x00000002u
#define GUI64_WIN_FLAG_GAME        0x00000004u

typedef struct gui64_event {
    nq_u32 type;
    nq_u32 key;
    int x;
    int y;
    int w;
    int h;
} gui64_event_t;

typedef struct gui64_window {
    int used;
    int id;
    int active;
    int chrome_drawn;
    int kind;
    nq_u32 flags;

    int owner_pid;
    terminal64_session_t* terminal;

    int x;
    int y;
    int w;
    int h;

    char title[32];

    /*
     * Stored as XRGB32: 0x00RRGGBB.
     */
    nq_u32 bg_color;

    /*
     * Kernel-owned window surface.
     *
     * Stored as XRGB32: 0x00RRGGBB.
     */
    nq_u32* buffer;
    nq_u32 buffer_width;
    nq_u32 buffer_height;
    nq_u32 buffer_pitch;
    nq_u64 buffer_size;

    int dirty;
    int dirty_x0;
    int dirty_y0;
    int dirty_x1;
    int dirty_y1;

    gui64_event_t events[GUI64_EVENT_QUEUE_SIZE];
    nq_u32 event_head;
    nq_u32 event_tail;
} gui64_window_t;

void gui64_init(void);
void gui64_enter(void);
void gui64_exit(void);
int  gui64_active(void);

int  gui64_create_window(
    const char* title,
    int x,
    int y,
    int w,
    int h,
    nq_u32 bg_color
);

void gui64_set_active_window(int id);
void gui64_next_window(void);
void gui64_render(void);
void gui64_request_repaint(void);
void gui64_flush_repaint(void);
void gui64_render_terminal_windows(void);

void gui64_draw_char(int x, int y, char c, nq_u32 fg);
void gui64_draw_text(int x, int y, const char* text, nq_u32 fg);
void gui64_draw_label(int x, int y, const char* text);
void gui64_draw_panel(int x, int y, int w, int h, nq_u32 color);
void gui64_draw_button(int x, int y, int w, int h, const char* text, int pressed);

int gui64_create_terminal_window(
    terminal64_session_t* terminal,
    const char* title,
    int x,
    int y,
    int w,
    int h
);

int gui64_show_kernel_terminal(void);
int gui64_has_focused_terminal_window(void);
void gui64_render_focused_terminal_window(void);
void gui64_render_focused_terminal_content(void);

int gui64_create_app_window_ex(
    int owner_pid,
    const char* title,
    int x,
    int y,
    int w,
    int h,
    nq_u32 flags
);

int gui64_create_app_window(
    int owner_pid,
    const char* title,
    int x,
    int y,
    int w,
    int h
);

int gui64_destroy_window(int id, int requester_pid);

int gui64_set_window_title(
    int id,
    int requester_pid,
    const char* title
);

int gui64_present_window(
    int id,
    int requester_pid,
    const nq_u32* pixels,
    nq_u32 width,
    nq_u32 height
);

int gui64_window_belongs_to(int id, int pid);

void gui64_destroy_windows_for_pid(int owner_pid);

int gui64_focused_window_id(void);
int gui64_has_focused_app_window(void);
int gui64_pop_event(int window_id, int requester_pid, gui64_event_t* out);
int gui64_push_key_to_focused(char c);
int gui64_push_close_to_focused(void);

#endif
