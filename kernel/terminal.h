#ifndef TERMINAL_H
#define TERMINAL_H

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

#define TERM_COLS 128
#define TERM_ROWS 48
#define TERM_HISTORY_ROWS 256
#define TERM_DEFAULT_ATTR 0x07

typedef struct terminal_session {
    u16 buffer[TERM_HISTORY_ROWS][TERM_COLS];
    u32 cursor_row;
    u32 cursor_col;
    u32 view_top;
} terminal_session_t;

void terminal_init(void);
terminal_session_t* terminal_create(void);
void terminal_destroy(terminal_session_t* t);

terminal_session_t* terminal_kernel(void);
terminal_session_t* terminal_active(void);
void terminal_switch(terminal_session_t* t);

#endif