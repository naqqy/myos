#include "../k64_console.h"
#include "terminal64.h"

#include "../graphics/framebuffer64.h"
#include "font8x8.h"

#define K64_CELL_W 8u
#define K64_CELL_H 16u

#define K64_CONTENT_START_ROW 3u

#define TERM64_ATTR_DEFAULT 0x07u

struct terminal64_session {
    nq_u16 buffer[TERM64_HISTORY_ROWS][TERM64_MAX_COLS];

    nq_u32 cursor_row;
    nq_u32 cursor_col;
    nq_u32 view_top;

    nq_u32 used;
    nq_u32 id;
};

static terminal64_session_t g_terminals[TERM64_MAX_SESSIONS];
static terminal64_session_t* g_kernel_terminal = 0;
static terminal64_session_t* g_active_terminal = 0;

static nq_u32 g_cols = 0;
static nq_u32 g_rows = 0;
static nq_u32 g_visible_rows = 0;

static void k64_draw_header(void);
static void k64_render_text_area(void);

static nq_u32 k64_bg_color(void) {
    return fb64_make_color(0, 0, 0);
}

static nq_u32 k64_fg_color(void) {
    return fb64_make_color(234, 234, 234);
}

static nq_u16 term64_make_cell(char c) {
    return (nq_u16)(((nq_u16)TERM64_ATTR_DEFAULT << 8) | (nq_u8)c);
}

static char term64_cell_char(nq_u16 cell) {
    char c = (char)(cell & 0xFFu);

    if (c == 0) {
        return ' ';
    }

    return c;
}

static void mem_zero(void* ptr, nq_u64 size) {
    nq_u8* p = (nq_u8*)ptr;

    for (nq_u64 i = 0; i < size; i++) {
        p[i] = 0;
    }
}

static void terminal64_clear_session(terminal64_session_t* t) {
    if (!t) {
        return;
    }

    for (nq_u32 row = 0; row < TERM64_HISTORY_ROWS; row++) {
        for (nq_u32 col = 0; col < TERM64_MAX_COLS; col++) {
            t->buffer[row][col] = term64_make_cell(' ');
        }
    }

    t->cursor_row = 0;
    t->cursor_col = 0;
    t->view_top = 0;
}

static void terminal64_shift_history_up(terminal64_session_t* t) {
    if (!t) {
        return;
    }

    for (nq_u32 row = 1; row < TERM64_HISTORY_ROWS; row++) {
        for (nq_u32 col = 0; col < TERM64_MAX_COLS; col++) {
            t->buffer[row - 1][col] = t->buffer[row][col];
        }
    }

    for (nq_u32 col = 0; col < TERM64_MAX_COLS; col++) {
        t->buffer[TERM64_HISTORY_ROWS - 1][col] = term64_make_cell(' ');
    }

    if (t->cursor_row > 0) {
        t->cursor_row--;
    }

    if (t->view_top > 0) {
        t->view_top--;
    }
}

static void terminal64_ensure_cursor_visible(terminal64_session_t* t) {
    if (!t) {
        return;
    }

    if (g_visible_rows == 0) {
        t->view_top = 0;
        return;
    }

    if (t->cursor_row < t->view_top) {
        t->view_top = t->cursor_row;
    }

    if (t->cursor_row >= t->view_top + g_visible_rows) {
        t->view_top = t->cursor_row - g_visible_rows + 1u;
    }

    if (t->view_top >= TERM64_HISTORY_ROWS) {
        t->view_top = TERM64_HISTORY_ROWS - 1;
    }
}

static void k64_draw_char_at(nq_u32 col, nq_u32 screen_row, char c) {
    const unsigned char* glyph;
    nq_u32 px = col * K64_CELL_W;
    nq_u32 py = screen_row * K64_CELL_H;

    nq_u32 fg = k64_fg_color();
    nq_u32 bg = k64_bg_color();

    if (!fb64_available()) {
        return;
    }

    if (col >= g_cols || screen_row >= g_rows) {
        return;
    }

    glyph = font8x8_get(c);

    for (nq_u32 gy = 0; gy < 8; gy++) {
        unsigned char bits = glyph[gy];

        for (nq_u32 gx = 0; gx < 8; gx++) {
            nq_u32 color = (bits & (0x80u >> gx)) ? fg : bg;

            fb64_put_pixel(px + gx, py + gy * 2, color);
            fb64_put_pixel(px + gx, py + gy * 2 + 1, color);
        }
    }
}

static void k64_draw_terminal_cell(
    terminal64_session_t* t,
    nq_u32 buffer_row,
    nq_u32 col
) {
    if (!t || t != g_active_terminal) {
        return;
    }

    if (col >= g_cols || col >= TERM64_MAX_COLS) {
        return;
    }

    if (buffer_row < t->view_top) {
        return;
    }

    nq_u32 visible_index = buffer_row - t->view_top;

    if (visible_index >= g_visible_rows) {
        return;
    }

    nq_u32 screen_row = K64_CONTENT_START_ROW + visible_index;

    k64_draw_char_at(
        col,
        screen_row,
        term64_cell_char(t->buffer[buffer_row][col])
    );
}

static void k64_render_text_area(void) {
    terminal64_session_t* t = g_active_terminal;

    if (!fb64_available() || !t) {
        return;
    }

    nq_u32 text_y = K64_CONTENT_START_ROW * K64_CELL_H;
    nq_u32 text_h = g_visible_rows * K64_CELL_H;

    fb64_fill_rect(
        0,
        text_y,
        fb64_width(),
        text_h,
        k64_bg_color()
    );

    for (nq_u32 r = 0; r < g_visible_rows; r++) {
        nq_u32 buffer_row = t->view_top + r;

        if (buffer_row >= TERM64_HISTORY_ROWS) {
            break;
        }

        for (nq_u32 col = 0; col < g_cols && col < TERM64_MAX_COLS; col++) {
            char c = term64_cell_char(t->buffer[buffer_row][col]);

            if (c != ' ') {
                k64_draw_char_at(col, K64_CONTENT_START_ROW + r, c);
            }
        }
    }
}

static void terminal64_newline_internal(terminal64_session_t* t) {
    if (!t) {
        return;
    }

    t->cursor_col = 0;

    if (t->cursor_row + 1 >= TERM64_HISTORY_ROWS) {
        terminal64_shift_history_up(t);
        t->cursor_row = TERM64_HISTORY_ROWS - 1;
    } else {
        t->cursor_row++;
    }

    nq_u32 old_view_top = t->view_top;

    terminal64_ensure_cursor_visible(t);

    if (t == g_active_terminal && old_view_top != t->view_top) {
        k64_render_text_area();
    }
}

static void terminal64_put_char_internal(terminal64_session_t* t, char c) {
    if (!t) {
        return;
    }

    if (c == '\n') {
        terminal64_newline_internal(t);
        return;
    }

    if (c == '\r') {
        t->cursor_col = 0;
        return;
    }

    if (c == '\t') {
        do {
            terminal64_put_char_internal(t, ' ');
        } while ((t->cursor_col & 3u) != 0);

        return;
    }

    if ((nq_u8)c < 32) {
        return;
    }

    if (t->cursor_col >= g_cols || t->cursor_col >= TERM64_MAX_COLS) {
        terminal64_newline_internal(t);
    }

    if (t->cursor_row >= TERM64_HISTORY_ROWS) {
        t->cursor_row = TERM64_HISTORY_ROWS - 1;
    }

    t->buffer[t->cursor_row][t->cursor_col] = term64_make_cell(c);

    k64_draw_terminal_cell(t, t->cursor_row, t->cursor_col);

    t->cursor_col++;

    if (t->cursor_col >= g_cols || t->cursor_col >= TERM64_MAX_COLS) {
        terminal64_newline_internal(t);
    }
}

void terminal64_put_char(terminal64_session_t* t, char c) {
    terminal64_put_char_internal(t, c);
}

void terminal64_write(terminal64_session_t* t, const char* s) {
    if (!t || !s) {
        return;
    }

    while (*s) {
        terminal64_put_char_internal(t, *s++);
    }
}

void terminal64_write_len(terminal64_session_t* t, const char* s, nq_u64 len) {
    if (!t || !s) {
        return;
    }

    for (nq_u64 i = 0; i < len; i++) {
        terminal64_put_char_internal(t, s[i]);
    }
}

nq_u32 terminal64_id(terminal64_session_t* t) {
    return t ? t->id : 0;
}

static void terminal64_backspace_internal(terminal64_session_t* t) {
    if (!t) {
        return;
    }

    if (t->cursor_col == 0) {
        if (t->cursor_row == 0) {
            return;
        }

        t->cursor_row--;
        t->cursor_col = g_cols ? g_cols - 1 : 0;

        if (t->cursor_col >= TERM64_MAX_COLS) {
            t->cursor_col = TERM64_MAX_COLS - 1;
        }
    } else {
        t->cursor_col--;
    }

    t->buffer[t->cursor_row][t->cursor_col] = term64_make_cell(' ');
    terminal64_ensure_cursor_visible(t);

    k64_draw_terminal_cell(t, t->cursor_row, t->cursor_col);
}

void terminal64_init_system(nq_u32 cols, nq_u32 visible_rows) {
    mem_zero(g_terminals, sizeof(g_terminals));

    if (cols > TERM64_MAX_COLS) {
        cols = TERM64_MAX_COLS;
    }

    g_cols = cols;
    g_visible_rows = visible_rows;

    g_terminals[0].used = 1;
    g_terminals[0].id = 0;

    terminal64_clear_session(&g_terminals[0]);

    g_kernel_terminal = &g_terminals[0];
    g_active_terminal = &g_terminals[0];
}

terminal64_session_t* terminal64_create(void) {
    for (nq_u32 i = 1; i < TERM64_MAX_SESSIONS; i++) {
        if (!g_terminals[i].used) {
            g_terminals[i].used = 1;
            g_terminals[i].id = i;
            terminal64_clear_session(&g_terminals[i]);
            return &g_terminals[i];
        }
    }

    return 0;
}

void terminal64_destroy(terminal64_session_t* t) {
    if (!t || t == g_kernel_terminal) {
        return;
    }

    if (g_active_terminal == t) {
        terminal64_switch(g_kernel_terminal);
    }

    mem_zero(t, sizeof(*t));
}

terminal64_session_t* terminal64_kernel(void) {
    return g_kernel_terminal;
}

terminal64_session_t* terminal64_active(void) {
    return g_active_terminal;
}

void terminal64_switch(terminal64_session_t* t) {
    if (!t || !t->used) {
        return;
    }

    g_active_terminal = t;

    if (fb64_available()) {
        fb64_clear(k64_bg_color());
        k64_draw_header();
        k64_render_text_area();
    }
}

nq_u32 terminal64_cursor_row(terminal64_session_t* t) {
    return t ? t->cursor_row : 0;
}

nq_u32 terminal64_cursor_col(terminal64_session_t* t) {
    return t ? t->cursor_col : 0;
}

nq_u32 terminal64_view_top(terminal64_session_t* t) {
    return t ? t->view_top : 0;
}

nq_u32 terminal64_visible_rows(void) {
    return g_visible_rows;
}

nq_u32 terminal64_cols(void) {
    return g_cols;
}

void terminal64_print_summary(void) {
    terminal64_session_t* t = g_active_terminal;

    k64_print("Terminal64:\n");

    k64_print("  cols=");
    k64_print_dec64(g_cols);
    k64_print(" visible_rows=");
    k64_print_dec64(g_visible_rows);
    k64_print(" history_rows=");
    k64_print_dec64(TERM64_HISTORY_ROWS);
    k64_print("\n");

    if (t) {
        k64_print("  active_id=");
        k64_print_dec64(t->id);
        k64_print(" cursor=(");
        k64_print_dec64(t->cursor_col);
        k64_print(",");
        k64_print_dec64(t->cursor_row);
        k64_print(") view_top=");
        k64_print_dec64(t->view_top);
        k64_print("\n");
    }
}

void k64_clear_screen(void) {
    terminal64_session_t* t = g_active_terminal;

    if (!fb64_available() || !t) {
        return;
    }

    terminal64_clear_session(t);

    fb64_clear(k64_bg_color());
    k64_draw_header();
    k64_render_text_area();
}

void k64_putchar(char c) {
    terminal64_put_char_internal(g_active_terminal, c);
}

void k64_backspace(void) {
    terminal64_backspace_internal(g_active_terminal);
}

void k64_print(const char* s) {
    if (!s) {
        return;
    }

    while (*s) {
        k64_putchar(*s++);
    }
}

void k64_print_hex64(nq_u64 value) {
    static const char hex[] = "0123456789ABCDEF";

    k64_print("0x");

    for (int i = 15; i >= 0; i--) {
        k64_putchar(hex[(value >> ((nq_u32)i * 4u)) & 0xFu]);
    }
}

void k64_print_dec64(nq_u64 value) {
    char buf[32];
    int i = 0;

    if (value == 0) {
        k64_putchar('0');
        return;
    }

    while (value > 0 && i < (int)sizeof(buf)) {
        buf[i++] = (char)('0' + (value % 10u));
        value /= 10u;
    }

    while (i > 0) {
        k64_putchar(buf[--i]);
    }
}

static void k64_draw_header(void) {
    nq_u32 bar = fb64_make_color(0, 32, 64);
    nq_u32 accent = fb64_make_color(0, 220, 255);

    if (!fb64_available()) {
        return;
    }

    fb64_fill_rect(0, 0, fb64_width(), 40, bar);
    fb64_fill_rect(0, 39, fb64_width(), 2, accent);

    terminal64_session_t* saved = g_active_terminal;

    /*
     * Draw header directly, without changing terminal buffer.
     */
    const char* title = "NAQ OS 64-bit Kernel";

    nq_u32 col = 2;
    nq_u32 row = 1;

    while (*title && col < g_cols) {
        k64_draw_char_at(col, row, *title);
        title++;
        col++;
    }

    g_active_terminal = saved;
}

void k64_console_init(naq_boot_info_t* boot) {
    if (!fb64_init(boot)) {
        for (;;) {
            __asm__ volatile ("hlt");
        }
    }

    g_rows = fb64_height() / K64_CELL_H;
    g_cols = fb64_width() / K64_CELL_W;

    if (g_cols == 0) {
        g_cols = 1;
    }

    if (g_rows == 0) {
        g_rows = 1;
    }

    if (g_cols > TERM64_MAX_COLS) {
        g_cols = TERM64_MAX_COLS;
    }

    if (g_rows <= K64_CONTENT_START_ROW) {
        g_visible_rows = 1;
    } else {
        g_visible_rows = g_rows - K64_CONTENT_START_ROW;
    }

    terminal64_init_system(g_cols, g_visible_rows);

    fb64_clear(k64_bg_color());
    k64_draw_header();
    k64_render_text_area();
}