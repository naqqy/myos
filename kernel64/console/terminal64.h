#ifndef TERMINAL64_H
#define TERMINAL64_H

#include "naq_boot_info.h"

#define TERM64_HISTORY_ROWS 256
#define TERM64_MAX_COLS     160
#define TERM64_MAX_SESSIONS 8

typedef struct terminal64_session terminal64_session_t;

void terminal64_init_system(nq_u32 cols, nq_u32 visible_rows);

terminal64_session_t* terminal64_create(void);
void terminal64_destroy(terminal64_session_t* t);

terminal64_session_t* terminal64_kernel(void);
terminal64_session_t* terminal64_active(void);
void terminal64_switch(terminal64_session_t* t);

void terminal64_put_char(terminal64_session_t* t, char c);
void terminal64_write(terminal64_session_t* t, const char* s);
void terminal64_write_len(terminal64_session_t* t, const char* s, nq_u64 len);

nq_u32 terminal64_id(terminal64_session_t* t);
nq_u32 terminal64_cursor_row(terminal64_session_t* t);
nq_u32 terminal64_cursor_col(terminal64_session_t* t);
nq_u32 terminal64_view_top(terminal64_session_t* t);
nq_u32 terminal64_visible_rows(void);
nq_u32 terminal64_cols(void);

void terminal64_print_summary(void);

/*
 * GUI terminal rendering support.
 * When GUI mode is enabled, terminal writes update the session buffer but
 * do not draw directly to the framebuffer text console.
 */
void terminal64_set_gui_mode(int enabled);
int  terminal64_gui_mode(void);
void terminal64_mark_gui_dirty(void);
int  terminal64_take_gui_dirty(void);

void terminal64_render_into_rect(
    terminal64_session_t* t,
    int x,
    int y,
    int w,
    int h
);

void terminal64_render_into_buffer(
    terminal64_session_t* t,
    nq_u32* dst,
    nq_u32 dst_width,
    nq_u32 dst_height,
    int x,
    int y,
    int w,
    int h
);

#endif