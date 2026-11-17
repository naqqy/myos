#ifndef GUI64_H
#define GUI64_H

#include "naq_boot_info.h"
#include "../console/terminal64.h"

/*
 * NAQ OS desktop: compositor, window manager and shell (taskbar, start menu,
 * desktop icons).
 *
 * Model:
 *   - Every window owns one cached surface holding its whole image: frame,
 *     title bar and content. Apps present straight into the content part;
 *     the frame is only redrawn when the title, focus or size changes.
 *   - The compositor keeps a list of damaged screen rectangles. For each one
 *     it copies the desktop, the visible windows (bottom to top), the taskbar,
 *     overlays and the mouse cursor into a back buffer and then copies that
 *     rectangle to the screen. Windows hidden behind an opaque window that
 *     covers the whole rectangle are skipped.
 *   - A kernel task ("desktop") drives it: it reads the mouse and keyboard,
 *     runs the window manager and repaints, independent of the shell.
 */

#define GUI64_MAX_WINDOWS 16
#define GUI64_EVENT_QUEUE_SIZE 64

#define GUI64_EVENT_NONE   0
#define GUI64_EVENT_KEY    1
#define GUI64_EVENT_FOCUS  2
#define GUI64_EVENT_CLOSE  3
#define GUI64_EVENT_RESIZE 4
#define GUI64_EVENT_MOUSE  5

/*
 * GUI64_EVENT_MOUSE:
 *   x, y  pointer position relative to the window's content area
 *   key   buttons currently held (GUI64_MOUSE_BUTTON_*)
 *   w     what happened (GUI64_MOUSE_*)
 *   h     the button that changed, for PRESS/RELEASE
 *
 * GUI64_EVENT_RESIZE:
 *   w, h  new content size in pixels; present frames of that size from now on
 */
#define GUI64_MOUSE_MOVE    0
#define GUI64_MOUSE_PRESS   1
#define GUI64_MOUSE_RELEASE 2

#define GUI64_MOUSE_BUTTON_LEFT   0x01u
#define GUI64_MOUSE_BUTTON_RIGHT  0x02u
#define GUI64_MOUSE_BUTTON_MIDDLE 0x04u

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

/* Lifecycle ---------------------------------------------------------------- */

void gui64_init(void);
void gui64_enter(void);
void gui64_exit(void);
int  gui64_active(void);

/*
 * Runs one desktop step (input, window manager, repaint). The desktop task
 * calls this; it is also safe to call directly.
 */
void gui64_tick(void);

/*
 * Repaint requests. Rendering is done by the desktop task; these only mark
 * things as needing a redraw.
 */
void gui64_render(void);
void gui64_request_repaint(void);
void gui64_flush_repaint(void);
void gui64_render_terminal_windows(void);

/* Windows ------------------------------------------------------------------ */

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

/*
 * w/h are the size of the content area (what the app draws).
 */
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
void gui64_destroy_windows_for_pid(int owner_pid);

int gui64_set_window_title(int id, int requester_pid, const char* title);

/*
 * Copies a frame into the window's content area. If the frame and the
 * content area differ in size (the window was resized or maximised and the
 * app renders at its own resolution) the frame is stretched to fit.
 */
int gui64_present_window(
    int id,
    int requester_pid,
    const nq_u32* pixels,
    nq_u32 width,
    nq_u32 height
);

int gui64_window_belongs_to(int id, int pid);
int gui64_pid_has_window(int pid);
int gui64_focus_pid_window(int pid);

void gui64_set_active_window(int id);
void gui64_next_window(void);

/* Events / input ----------------------------------------------------------- */

int gui64_focused_window_id(void);
int gui64_has_focused_app_window(void);
int gui64_pop_event(int window_id, int requester_pid, gui64_event_t* out);
int gui64_push_key_to_focused(char c);
int gui64_push_close_to_focused(void);

/*
 * While the desktop is active it owns the keyboard. Keys typed into a
 * focused terminal window are queued for the shell, which takes them here.
 * Returns 0 when there is nothing queued.
 */
int gui64_shell_key_pop(void);

/*
 * Set when the user asks to leave the desktop (start menu or Ctrl+T).
 * The shell performs the switch because it owns the text console.
 */
int gui64_take_text_mode_request(void);

#endif
