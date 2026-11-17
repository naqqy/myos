#include "include/naqos.h"

/*
 * EDIT.ELF
 *
 * Plain text editor for the NAQ OS desktop.
 *
 * Started on its own it opens an empty document; started with a file path
 * as its launch argument (Files does this) it opens that file.
 *
 * Mouse:
 *   click in the text to place the cursor
 *   toolbar buttons: New, Open, Save, Save As
 *   scrollbar: click or drag; the wheel scrolls too
 *
 * Keys:
 *   arrows, Home, End, PgUp, PgDn   move
 *   Backspace, Delete, Enter, Tab   edit (Tab inserts spaces)
 *   Ctrl+S   save          Ctrl+O   open
 *   Ctrl+N   new           Esc      cancel a prompt
 *
 * Files can only be saved where the system can write: a USB stick, under
 * /usb.
 */

#define START_W 640u
#define START_H 440u

#define MAX_W 1100u
#define MAX_H 760u

#define TEXT_MAX (256u * 1024u)
#define PATH_MAX 120u

#define TOOLBAR_H 36
#define STATUS_H 22
#define GUTTER_W 48
#define SCROLL_W 16
#define TEXT_PAD 4

#define TAB_WIDTH 4u

#define BLINK_TICKS 500u

#define PROMPT_NONE 0
#define PROMPT_OPEN 1
#define PROMPT_SAVE 2

/*
 * Actions that throw away unsaved changes ask to be repeated first.
 */
#define ARM_NONE  0
#define ARM_NEW   1
#define ARM_OPEN  2
#define ARM_CLOSE 3

/* Aero palette, shared with Files */
#define COL_PAPER     0x00FFFFFFu
#define COL_LINE      0x00EEF8FEu
#define COL_TEXT      0x0016445Eu
#define COL_TEXT_DIM  0x006890A4u
#define COL_GUTTER    0x00E6F4FBu
#define COL_BAR_TOP   0x00E2FAFFu
#define COL_BAR_BOT   0x0078CCEEu
#define COL_BAR_EDGE  0x002A86B4u
#define COL_TRACK     0x00DCEEF6u
#define COL_THUMB_TOP 0x00B8E6FAu
#define COL_THUMB_BOT 0x004AA8DCu

static u32 g_pixels[MAX_W * MAX_H];

static u32 g_w = START_W;
static u32 g_h = START_H;
static u32 g_window_w = START_W;
static u32 g_window_h = START_H;

/*
 * The document: one flat buffer, lines separated by '\n'.
 */
static char g_text[TEXT_MAX];
static u32 g_len = 0;
static u32 g_cursor = 0;

static u32 g_top = 0;       /* first visible line */
static u32 g_left = 0;      /* first visible column */
static int g_want_col = -1; /* column Up/Down try to stay in */

static int g_modified = 0;
static int g_crlf = 0;      /* file used Windows line endings */

static char g_path[PATH_MAX] = "";
static char g_status[120] = "";

static int g_prompt = PROMPT_NONE;
static char g_prompt_text[PATH_MAX];
static u32 g_prompt_len = 0;

static int g_armed = ARM_NONE;
static int g_dragging = 0;

static int g_window = -1;

/*
 * Scratch space for saving with Windows line endings.
 */
static char g_save[TEXT_MAX * 2u];

/* -------------------------------------------------------------------------- */
/* Strings                                                                     */
/* -------------------------------------------------------------------------- */

static void str_copy(char* dst, const char* src, u32 size) {
    u32 i = 0;

    if (!size) {
        return;
    }

    while (src[i] && i + 1 < size) {
        dst[i] = src[i];
        i++;
    }

    dst[i] = 0;
}

static void str_append(char* dst, const char* src, u32 size) {
    u32 len = (u32)strlen(dst);

    if (len < size) {
        str_copy(dst + len, src, size - len);
    }
}

static void append_number(char* dst, u64 value, u32 size) {
    char digits[24];
    u32 n = 0;

    do {
        digits[n++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value && n < sizeof(digits));

    u32 len = (u32)strlen(dst);

    while (n > 0 && len + 1 < size) {
        dst[len++] = digits[--n];
    }

    dst[len] = 0;
}

static void set_status(const char* message) {
    str_copy(g_status, message, sizeof(g_status));
}

static const char* file_name(void) {
    const char* name = g_path;

    for (u32 i = 0; g_path[i]; i++) {
        if (g_path[i] == '/') {
            name = g_path + i + 1;
        }
    }

    return name;
}

static void update_title(void) {
    char title[64];

    title[0] = 0;

    if (g_path[0]) {
        str_append(title, file_name(), sizeof(title));
        str_append(title, " - ", sizeof(title));
    }

    str_append(title, "Editor", sizeof(title));

    win_set_title(g_window, title);
}

/* -------------------------------------------------------------------------- */
/* Document                                                                    */
/* -------------------------------------------------------------------------- */

static u32 line_start(u32 offset) {
    while (offset > 0 && g_text[offset - 1] != '\n') {
        offset--;
    }

    return offset;
}

static u32 line_end(u32 offset) {
    while (offset < g_len && g_text[offset] != '\n') {
        offset++;
    }

    return offset;
}

static u32 line_of(u32 offset) {
    u32 line = 0;

    for (u32 i = 0; i < offset; i++) {
        if (g_text[i] == '\n') {
            line++;
        }
    }

    return line;
}

static u32 line_count(void) {
    return line_of(g_len) + 1u;
}

static u32 offset_of_line(u32 line) {
    u32 offset = 0;

    while (line > 0 && offset < g_len) {
        if (g_text[offset] == '\n') {
            line--;
        }

        offset++;
    }

    return offset;
}

/*
 * Screen column of an offset within its line: a tab advances to the next
 * multiple of TAB_WIDTH, everything else is one cell.
 */
static u32 column_of(u32 start, u32 offset) {
    u32 column = 0;

    for (u32 i = start; i < offset; i++) {
        if (g_text[i] == '\t') {
            column += TAB_WIDTH - (column % TAB_WIDTH);
        } else {
            column++;
        }
    }

    return column;
}

/*
 * The offset in the line starting at `start` that sits at (or nearest
 * before the end of the line to) the given screen column.
 */
static u32 offset_at_column(u32 start, u32 column) {
    u32 at = 0;
    u32 offset = start;

    while (offset < g_len && g_text[offset] != '\n') {
        u32 width = g_text[offset] == '\t' ? TAB_WIDTH - (at % TAB_WIDTH) : 1u;

        if (at + width > column) {
            break;
        }

        at += width;
        offset++;
    }

    return offset;
}

static int insert_at(u32 offset, const char* data, u32 count) {
    if (g_len + count >= TEXT_MAX) {
        set_status("The document is full (256 KB limit).");
        return 0;
    }

    for (u32 i = g_len; i > offset; i--) {
        g_text[i + count - 1] = g_text[i - 1];
    }

    for (u32 i = 0; i < count; i++) {
        g_text[offset + i] = data[i];
    }

    g_len += count;
    g_modified = 1;

    return 1;
}

static void delete_at(u32 offset) {
    if (offset >= g_len) {
        return;
    }

    for (u32 i = offset; i + 1 < g_len; i++) {
        g_text[i] = g_text[i + 1];
    }

    g_len--;
    g_modified = 1;
}

/* -------------------------------------------------------------------------- */
/* Layout                                                                      */
/* -------------------------------------------------------------------------- */

static int visible_rows(void) {
    int rows = ((i32)g_h - TOOLBAR_H - STATUS_H) / GFX_FONT_H;

    return rows < 1 ? 1 : rows;
}

static int visible_columns(void) {
    int columns = ((i32)g_w - GUTTER_W - SCROLL_W - TEXT_PAD * 2) / GFX_FONT_W;

    return columns < 1 ? 1 : columns;
}

static void clamp_top(void) {
    u32 lines = line_count();
    u32 rows = (u32)visible_rows();

    if (lines <= rows) {
        g_top = 0;
    } else if (g_top > lines - rows) {
        g_top = lines - rows;
    }
}

/*
 * Scrolls so the cursor is on screen.
 */
static void show_cursor(void) {
    u32 line = line_of(g_cursor);
    u32 column = column_of(line_start(g_cursor), g_cursor);
    u32 rows = (u32)visible_rows();
    u32 columns = (u32)visible_columns();

    if (line < g_top) {
        g_top = line;
    } else if (line >= g_top + rows) {
        g_top = line - rows + 1u;
    }

    if (column < g_left) {
        g_left = column;
    } else if (column >= g_left + columns) {
        g_left = column - columns + 1u;
    }
}

/* -------------------------------------------------------------------------- */
/* Files                                                                       */
/* -------------------------------------------------------------------------- */

static void new_document(void) {
    g_len = 0;
    g_cursor = 0;
    g_top = 0;
    g_left = 0;
    g_want_col = -1;
    g_modified = 0;
    g_crlf = 0;
    g_path[0] = 0;

    update_title();
}

static int load_file(const char* path) {
    naq_stat_t st;

    if (stat(path, &st) < 0 || st.type != NAQ_STAT_TYPE_FILE) {
        set_status("Could not open that file.");
        return 0;
    }

    if (st.size >= TEXT_MAX) {
        set_status("That file is too large to edit (256 KB limit).");
        return 0;
    }

    /*
     * Read into the save scratch first, so a failure leaves the current
     * document untouched.
     */
    i64 got = read_file(path, g_save, TEXT_MAX);

    if (got < 0) {
        set_status("Could not read that file.");
        return 0;
    }

    u32 out = 0;
    int crlf = 0;

    for (i64 i = 0; i < got; i++) {
        char c = g_save[i];

        if (c == 0) {
            set_status("That does not look like a text file.");
            return 0;
        }

        /*
         * Windows line endings are "\r\n": drop the "\r" while editing
         * and put it back when saving.
         */
        if (c == '\r' && i + 1 < got && g_save[i + 1] == '\n') {
            crlf = 1;
            continue;
        }

        g_save[out++] = c;
    }

    for (u32 i = 0; i < out; i++) {
        g_text[i] = g_save[i];
    }

    g_len = out;
    g_cursor = 0;
    g_top = 0;
    g_left = 0;
    g_want_col = -1;
    g_modified = 0;
    g_crlf = crlf;

    str_copy(g_path, path, sizeof(g_path));
    update_title();

    g_status[0] = 0;
    str_append(g_status, "Opened ", sizeof(g_status));
    str_append(g_status, file_name(), sizeof(g_status));

    return 1;
}

static void start_prompt(int kind);

static int save_file(void) {
    if (!g_path[0]) {
        start_prompt(PROMPT_SAVE);
        return 0;
    }

    const char* data = g_text;
    u32 size = g_len;

    if (g_crlf) {
        u32 out = 0;

        for (u32 i = 0; i < g_len; i++) {
            if (g_text[i] == '\n') {
                g_save[out++] = '\r';
            }

            g_save[out++] = g_text[i];
        }

        data = g_save;
        size = out;
    }

    i64 written = write_file(g_path, data, size);

    if (written == NAQ_ERR_LOCKED) {
        set_status("Disk writes are locked. In the Terminal type: diskunlock confirm");
        return 0;
    }

    if (written < 0) {
        set_status("Could not save there. Files can be saved under /usb");
        return 0;
    }

    g_modified = 0;
    update_title();

    g_status[0] = 0;
    str_append(g_status, "Saved ", sizeof(g_status));
    str_append(g_status, file_name(), sizeof(g_status));
    str_append(g_status, " (", sizeof(g_status));
    append_number(g_status, (u64)written, sizeof(g_status));
    str_append(g_status, " bytes)", sizeof(g_status));

    return 1;
}

/*
 * Returns 1 if it is fine to discard the document now. With unsaved
 * changes the first request only warns; repeating it goes ahead.
 */
static int may_discard(int action) {
    if (!g_modified || g_armed == action) {
        g_armed = ARM_NONE;
        return 1;
    }

    g_armed = action;
    set_status("Unsaved changes. Ctrl+S saves; repeat the action to discard them.");

    return 0;
}

/* -------------------------------------------------------------------------- */
/* Prompt (the path box used by Open and Save As)                              */
/* -------------------------------------------------------------------------- */

static void start_prompt(int kind) {
    g_prompt = kind;

    if (g_path[0]) {
        str_copy(g_prompt_text, g_path, sizeof(g_prompt_text));

        /*
         * Opening: start from the folder of the current file.
         */
        if (kind == PROMPT_OPEN) {
            u32 cut = 0;

            for (u32 i = 0; g_prompt_text[i]; i++) {
                if (g_prompt_text[i] == '/') {
                    cut = i + 1;
                }
            }

            g_prompt_text[cut] = 0;
        }
    } else {
        str_copy(g_prompt_text, "/usb/", sizeof(g_prompt_text));
    }

    g_prompt_len = (u32)strlen(g_prompt_text);

    set_status(kind == PROMPT_OPEN
        ? "Type the file's path, then Enter. Esc cancels."
        : "Type where to save, then Enter. For example /usb/My notes.txt");
}

static void finish_prompt(void) {
    int kind = g_prompt;

    g_prompt = PROMPT_NONE;

    if (g_prompt_len == 0 || g_prompt_text[0] != '/') {
        set_status("A path starts with /, for example /usb/NOTE.TXT");
        return;
    }

    if (kind == PROMPT_OPEN) {
        load_file(g_prompt_text);
    } else {
        char previous[PATH_MAX];

        str_copy(previous, g_path, sizeof(previous));
        str_copy(g_path, g_prompt_text, sizeof(g_path));

        if (!save_file()) {
            str_copy(g_path, previous, sizeof(g_path));
        }

        update_title();
    }
}

static void prompt_key(u32 key) {
    if (key == NAQ_KEY_ESCAPE) {
        g_prompt = PROMPT_NONE;
        set_status("Cancelled.");
    } else if (key == NAQ_KEY_ENTER) {
        finish_prompt();
    } else if (key == NAQ_KEY_BACKSPACE) {
        if (g_prompt_len > 0) {
            g_prompt_text[--g_prompt_len] = 0;
        }
    } else if (key >= 32 && key < 127 && g_prompt_len + 1 < PATH_MAX) {
        g_prompt_text[g_prompt_len++] = (char)key;
        g_prompt_text[g_prompt_len] = 0;
    }
}

/* -------------------------------------------------------------------------- */
/* Drawing                                                                     */
/* -------------------------------------------------------------------------- */

static u32 mix(u32 a, u32 b, u32 t, u32 max_t) {
    if (!max_t) {
        return a;
    }

    u32 r = (((a >> 16) & 0xFFu) * (max_t - t) + ((b >> 16) & 0xFFu) * t) / max_t;
    u32 g = (((a >> 8) & 0xFFu) * (max_t - t) + ((b >> 8) & 0xFFu) * t) / max_t;
    u32 bl = ((a & 0xFFu) * (max_t - t) + (b & 0xFFu) * t) / max_t;

    return (r << 16) | (g << 8) | bl;
}

static void rect(i32 x, i32 y, i32 w, i32 h, u32 color) {
    i32 x0 = x < 0 ? 0 : x;
    i32 y0 = y < 0 ? 0 : y;
    i32 x1 = x + w > (i32)g_w ? (i32)g_w : x + w;
    i32 y1 = y + h > (i32)g_h ? (i32)g_h : y + h;

    for (i32 yy = y0; yy < y1; yy++) {
        u32* row = g_pixels + (u32)yy * g_w;

        for (i32 xx = x0; xx < x1; xx++) {
            row[xx] = color;
        }
    }
}

static void gradient(i32 x, i32 y, i32 w, i32 h, u32 top, u32 bottom) {
    for (i32 row = 0; row < h; row++) {
        rect(x, y + row, w, 1, mix(top, bottom, (u32)row, (u32)(h > 1 ? h - 1 : 1)));
    }
}

static void frame(i32 x, i32 y, i32 w, i32 h, u32 color) {
    rect(x, y, w, 1, color);
    rect(x, y + h - 1, w, 1, color);
    rect(x, y, 1, h, color);
    rect(x + w - 1, y, 1, h, color);
}

static void text(i32 x, i32 y, const char* s, u32 color) {
    gfx_text(g_pixels, g_w, g_h, x, y, s, color);
}

static void text_n(i32 x, i32 y, const char* s, u32 max_chars, u32 color) {
    gfx_text_n(g_pixels, g_w, g_h, x, y, s, max_chars, color);
}

/*
 * Toolbar buttons, left to right.
 */
#define BUTTON_COUNT 4

static const char* button_label(int index) {
    switch (index) {
        case 0:  return "New";
        case 1:  return "Open";
        case 2:  return "Save";
        default: return "Save As";
    }
}

static const i32 g_button_x[BUTTON_COUNT] = { 8, 56, 112, 168 };
static const i32 g_button_w[BUTTON_COUNT] = { 44, 52, 52, 76 };

static void button(i32 x, i32 y, i32 w, i32 h, const char* label) {
    gradient(x, y, w, h, 0x00FFFFFFu, 0x008CD4F0u);
    rect(x + 1, y + 1, w - 2, h / 2 - 1, 0x00F4FDFFu);
    frame(x, y, w, h, COL_BAR_EDGE);

    i32 len = (i32)strlen(label);

    text(x + (w - len * GFX_FONT_W) / 2, y + (h - GFX_FONT_H) / 2, label,
         COL_TEXT);
}

static void paint_toolbar(int caret_on) {
    gradient(0, 0, (i32)g_w, TOOLBAR_H, COL_BAR_TOP, COL_BAR_BOT);
    rect(0, 1, (i32)g_w, TOOLBAR_H / 2 - 2, 0x00F0FCFFu);
    rect(0, TOOLBAR_H - 1, (i32)g_w, 1, COL_BAR_EDGE);

    if (g_prompt != PROMPT_NONE) {
        /*
         * The path box replaces the buttons while it is open.
         */
        const char* label = g_prompt == PROMPT_OPEN ? "Open:" : "Save as:";
        i32 box_x = 16 + (i32)strlen(label) * GFX_FONT_W;
        i32 box_w = (i32)g_w - box_x - 8;

        text(8, 10, label, COL_TEXT);

        if (box_w > 24) {
            u32 fit = (u32)((box_w - 16) / GFX_FONT_W);
            u32 skip = g_prompt_len > fit ? g_prompt_len - fit : 0;

            rect(box_x, 6, box_w, 24, COL_PAPER);
            frame(box_x, 6, box_w, 24, COL_BAR_EDGE);
            text_n(box_x + 8, 10, g_prompt_text + skip, fit, COL_TEXT);

            if (caret_on) {
                rect(box_x + 8 + (i32)(g_prompt_len - skip) * GFX_FONT_W, 9, 2, 18,
                     COL_TEXT);
            }
        }

        return;
    }

    for (int i = 0; i < BUTTON_COUNT; i++) {
        button(g_button_x[i], 6, g_button_w[i], 24, button_label(i));
    }

    /*
     * File name on the right; a star marks unsaved changes.
     */
    char name[64];

    name[0] = 0;
    str_append(name, g_path[0] ? file_name() : "Untitled", sizeof(name));

    if (g_modified) {
        str_append(name, " *", sizeof(name));
    }

    i32 name_x = g_button_x[BUTTON_COUNT - 1] + g_button_w[BUTTON_COUNT - 1] + 16;
    i32 room = ((i32)g_w - name_x - 8) / GFX_FONT_W;

    if (room > 0) {
        text_n(name_x, 10, name, (u32)room, COL_TEXT_DIM);
    }
}

static void paint_status(void) {
    i32 y = (i32)g_h - STATUS_H;
    char where[40];

    gradient(0, y, (i32)g_w, STATUS_H, 0x00EAF8FEu, 0x00B8E2F4u);
    rect(0, y, (i32)g_w, 1, COL_BAR_EDGE);

    where[0] = 0;
    str_append(where, "Ln ", sizeof(where));
    append_number(where, line_of(g_cursor) + 1u, sizeof(where));
    str_append(where, ", Col ", sizeof(where));
    append_number(where, column_of(line_start(g_cursor), g_cursor) + 1u, sizeof(where));

    i32 where_w = (i32)strlen(where) * GFX_FONT_W;
    i32 where_x = (i32)g_w - where_w - 8;
    i32 room = (where_x - 16) / GFX_FONT_W;

    if (room > 0) {
        text_n(8, y + 3, g_status, (u32)room, COL_TEXT);
    }

    if (where_x > 0) {
        text(where_x, y + 3, where, COL_TEXT_DIM);
    }
}

static void scrollbar_geometry(i32* track_y, i32* track_h, i32* thumb_y, i32* thumb_h) {
    u32 lines = line_count();
    u32 rows = (u32)visible_rows();

    *track_y = TOOLBAR_H;
    *track_h = (i32)g_h - TOOLBAR_H - STATUS_H;

    if (*track_h < 8) {
        *track_h = 8;
    }

    if (lines <= rows) {
        *thumb_y = *track_y;
        *thumb_h = *track_h;
        return;
    }

    *thumb_h = (i32)(((u64)*track_h * rows) / lines);

    if (*thumb_h < 24) {
        *thumb_h = 24;
    }

    if (*thumb_h > *track_h) {
        *thumb_h = *track_h;
    }

    *thumb_y = *track_y +
        (i32)(((u64)(*track_h - *thumb_h) * g_top) / (lines - rows));
}

static void paint_text(int caret_on) {
    int rows = visible_rows();
    int columns = visible_columns();
    i32 text_x = GUTTER_W + TEXT_PAD;
    i32 area_h = (i32)g_h - TOOLBAR_H - STATUS_H;
    i32 area_w = (i32)g_w - SCROLL_W;

    u32 cursor_line = line_of(g_cursor);
    u32 offset = offset_of_line(g_top);
    u32 lines = line_count();

    rect(0, TOOLBAR_H, area_w, area_h, COL_PAPER);
    rect(0, TOOLBAR_H, GUTTER_W, area_h, COL_GUTTER);
    rect(GUTTER_W - 1, TOOLBAR_H, 1, area_h, 0x00C4E0EEu);

    for (int row = 0; row < rows; row++) {
        u32 line = g_top + (u32)row;
        i32 y = TOOLBAR_H + row * GFX_FONT_H;

        if (line >= lines) {
            break;
        }

        if (line == cursor_line) {
            rect(GUTTER_W, y, area_w - GUTTER_W, GFX_FONT_H, COL_LINE);
        }

        /*
         * Line number, right-aligned in the gutter.
         */
        char number[12];

        number[0] = 0;
        append_number(number, line + 1u, sizeof(number));

        text(GUTTER_W - 8 - (i32)strlen(number) * GFX_FONT_W, y, number,
             line == cursor_line ? COL_TEXT : COL_TEXT_DIM);

        /*
         * The visible part of the line, with tabs expanded.
         */
        char cells[160];
        u32 count = 0;
        u32 column = 0;
        u32 limit = (u32)columns < sizeof(cells) - 1 ? (u32)columns : (u32)sizeof(cells) - 1;

        while (offset < g_len && g_text[offset] != '\n') {
            char c = g_text[offset];
            u32 width = c == '\t' ? TAB_WIDTH - (column % TAB_WIDTH) : 1u;

            for (u32 i = 0; i < width; i++) {
                u32 at = column + i;

                if (at >= g_left && at - g_left < limit) {
                    char shown = c == '\t' ? ' ' : c;

                    if ((unsigned char)shown < 32 || (unsigned char)shown > 126) {
                        shown = '.';
                    }

                    while (count < at - g_left) {
                        cells[count++] = ' ';
                    }

                    cells[count++] = shown;
                }
            }

            column += width;
            offset++;
        }

        cells[count] = 0;

        if (count) {
            text(text_x, y, cells, COL_TEXT);
        }

        if (line == cursor_line && caret_on && g_prompt == PROMPT_NONE) {
            u32 at = column_of(line_start(g_cursor), g_cursor);

            if (at >= g_left && at - g_left <= (u32)columns) {
                rect(text_x + (i32)(at - g_left) * GFX_FONT_W, y, 2, GFX_FONT_H,
                     COL_TEXT);
            }
        }

        /*
         * Step over the newline to the next line.
         */
        if (offset < g_len) {
            offset++;
        }
    }

    /*
     * Scrollbar.
     */
    i32 track_y;
    i32 track_h;
    i32 thumb_y;
    i32 thumb_h;

    scrollbar_geometry(&track_y, &track_h, &thumb_y, &thumb_h);

    rect(area_w, track_y, SCROLL_W, track_h, COL_TRACK);
    rect(area_w, track_y, 1, track_h, 0x00B4D4E4u);
    gradient(area_w + 2, thumb_y + 1, SCROLL_W - 4, thumb_h - 2, COL_THUMB_TOP,
             COL_THUMB_BOT);
    frame(area_w + 2, thumb_y + 1, SCROLL_W - 4, thumb_h - 2, COL_BAR_EDGE);
}

static void paint(int caret_on) {
    paint_text(caret_on);
    paint_toolbar(caret_on);
    paint_status();
}

/* -------------------------------------------------------------------------- */
/* Input                                                                       */
/* -------------------------------------------------------------------------- */

static void move_vertical(int delta) {
    u32 start = line_start(g_cursor);
    u32 line = line_of(g_cursor);
    u32 lines = line_count();

    if (g_want_col < 0) {
        g_want_col = (int)column_of(start, g_cursor);
    }

    i64 target = (i64)line + delta;

    if (target < 0) {
        target = 0;
    }

    if (target >= (i64)lines) {
        target = (i64)lines - 1;
    }

    g_cursor = offset_at_column(offset_of_line((u32)target), (u32)g_want_col);
}

static void scroll_to_drag(i32 y) {
    i32 track_y;
    i32 track_h;
    i32 thumb_y;
    i32 thumb_h;

    scrollbar_geometry(&track_y, &track_h, &thumb_y, &thumb_h);

    u32 lines = line_count();
    u32 rows = (u32)visible_rows();

    if (lines <= rows || track_h <= thumb_h) {
        g_top = 0;
        return;
    }

    i32 at = y - track_y - thumb_h / 2;

    if (at < 0) {
        at = 0;
    }

    if (at > track_h - thumb_h) {
        at = track_h - thumb_h;
    }

    g_top = (u32)(((u64)at * (lines - rows)) / (u32)(track_h - thumb_h));
}

static void button_pressed(int index, int* running) {
    (void)running;

    if (index == 0) {
        if (may_discard(ARM_NEW)) {
            new_document();
            set_status("New document.");
        }
    } else if (index == 1) {
        if (may_discard(ARM_OPEN)) {
            start_prompt(PROMPT_OPEN);
        }
    } else if (index == 2) {
        save_file();
    } else {
        start_prompt(PROMPT_SAVE);
    }
}

static void handle_press(i32 x, i32 y, int* running) {
    if (y < TOOLBAR_H) {
        if (g_prompt != PROMPT_NONE) {
            return;
        }

        for (int i = 0; i < BUTTON_COUNT; i++) {
            if (x >= g_button_x[i] && x < g_button_x[i] + g_button_w[i] &&
                y >= 6 && y < 30) {
                button_pressed(i, running);
                return;
            }
        }

        return;
    }

    if (y >= (i32)g_h - STATUS_H) {
        return;
    }

    if (x >= (i32)g_w - SCROLL_W) {
        g_dragging = 1;
        scroll_to_drag(y);
        return;
    }

    /*
     * Click in the text: put the cursor at the nearest character.
     */
    u32 line = g_top + (u32)((y - TOOLBAR_H) / GFX_FONT_H);
    u32 lines = line_count();

    if (line >= lines) {
        line = lines - 1u;
    }

    i32 column = (x - GUTTER_W - TEXT_PAD + GFX_FONT_W / 2) / GFX_FONT_W;

    if (column < 0) {
        column = 0;
    }

    g_cursor = offset_at_column(offset_of_line(line), g_left + (u32)column);
    g_want_col = -1;
    g_armed = ARM_NONE;
}

static void handle_key(u32 key, int* running) {
    (void)running;

    if (g_prompt != PROMPT_NONE) {
        prompt_key(key);
        return;
    }

    int keep_column = 0;

    /*
     * Ctrl+letter arrives as the letter's position in the alphabet.
     */
    if (key == 19u) {               /* Ctrl+S */
        save_file();
        return;
    }

    if (key == 15u) {               /* Ctrl+O */
        if (may_discard(ARM_OPEN)) {
            start_prompt(PROMPT_OPEN);
        }

        return;
    }

    if (key == 14u) {               /* Ctrl+N */
        if (may_discard(ARM_NEW)) {
            new_document();
            set_status("New document.");
        }

        return;
    }

    g_armed = ARM_NONE;

    switch (key) {
        case NAQ_KEY_LEFT:
            if (g_cursor > 0) {
                g_cursor--;
            }
            break;

        case NAQ_KEY_RIGHT:
            if (g_cursor < g_len) {
                g_cursor++;
            }
            break;

        case NAQ_KEY_UP:
            move_vertical(-1);
            keep_column = 1;
            break;

        case NAQ_KEY_DOWN:
            move_vertical(1);
            keep_column = 1;
            break;

        case NAQ_KEY_PAGE_UP:
            move_vertical(-visible_rows());
            keep_column = 1;
            break;

        case NAQ_KEY_PAGE_DOWN:
            move_vertical(visible_rows());
            keep_column = 1;
            break;

        case NAQ_KEY_HOME:
            g_cursor = line_start(g_cursor);
            break;

        case NAQ_KEY_END:
            g_cursor = line_end(g_cursor);
            break;

        case NAQ_KEY_DELETE:
            delete_at(g_cursor);
            break;

        case NAQ_KEY_BACKSPACE:
            if (g_cursor > 0) {
                g_cursor--;
                delete_at(g_cursor);
            }
            break;

        case NAQ_KEY_ENTER: {
            /*
             * Carry the indentation of the current line onto the new one.
             */
            char insert[64];
            u32 count = 0;
            u32 start = line_start(g_cursor);

            insert[count++] = '\n';

            for (u32 i = start; i < g_cursor && count < sizeof(insert); i++) {
                if (g_text[i] != ' ' && g_text[i] != '\t') {
                    break;
                }

                insert[count++] = g_text[i];
            }

            if (insert_at(g_cursor, insert, count)) {
                g_cursor += count;
            }

            break;
        }

        case '\t': {
            /*
             * Spaces up to the next tab stop.
             */
            u32 column = column_of(line_start(g_cursor), g_cursor);
            u32 count = TAB_WIDTH - (column % TAB_WIDTH);

            if (insert_at(g_cursor, "    ", count)) {
                g_cursor += count;
            }

            break;
        }

        default:
            if (key >= 32 && key < 127) {
                char c = (char)key;

                if (insert_at(g_cursor, &c, 1)) {
                    g_cursor++;
                }
            }
            break;
    }

    if (!keep_column) {
        g_want_col = -1;
    }

    show_cursor();
}

static void set_size(u32 window_w, u32 window_h) {
    g_window_w = window_w ? window_w : 1;
    g_window_h = window_h ? window_h : 1;

    g_w = g_window_w > MAX_W ? MAX_W : g_window_w;
    g_h = g_window_h > MAX_H ? MAX_H : g_window_h;

    if (g_w < 260) {
        g_w = 260;
    }

    if (g_h < 120) {
        g_h = 120;
    }

    clamp_top();
}

int main(void) {
    char arg[PATH_MAX];

    g_window = win_create(180, 90, START_W, START_H, "Editor");

    if (g_window < 0) {
        puts("edit: win_create failed\n");
        return 1;
    }

    set_status("Ctrl+S saves, Ctrl+O opens. Files are saved under /usb.");

    if (get_arg(arg, sizeof(arg)) > 0) {
        load_file(arg);
    }

    int running = 1;
    int dirty = 1;
    int caret_on = 1;
    u64 blink_at = get_ticks() + BLINK_TICKS;

    while (running) {
        naq_win_event_t ev;

        while (win_get_event(g_window, &ev) == 1) {
            /*
             * Window pixels -> frame pixels (they differ only when the
             * window is larger than our biggest frame).
             */
            i32 x = (i32)(((i64)ev.x * (i64)g_w) / (i64)g_window_w);
            i32 y = (i32)(((i64)ev.y * (i64)g_h) / (i64)g_window_h);

            if (ev.type == NAQ_WIN_EVENT_CLOSE) {
                if (may_discard(ARM_CLOSE)) {
                    running = 0;
                }

                dirty = 1;
            } else if (ev.type == NAQ_WIN_EVENT_KEY) {
                handle_key(ev.key, &running);

                /*
                 * Keep the caret solid while typing.
                 */
                caret_on = 1;
                blink_at = get_ticks() + BLINK_TICKS;
                dirty = 1;
            } else if (ev.type == NAQ_WIN_EVENT_RESIZE) {
                set_size((u32)ev.w, (u32)ev.h);
                dirty = 1;
            } else if (ev.type == NAQ_WIN_EVENT_MOUSE) {
                if (ev.w == NAQ_MOUSE_PRESS &&
                    ev.h == (i32)NAQ_MOUSE_BUTTON_LEFT) {
                    handle_press(x, y, &running);
                    caret_on = 1;
                    blink_at = get_ticks() + BLINK_TICKS;
                    dirty = 1;
                } else if (ev.w == NAQ_MOUSE_RELEASE) {
                    g_dragging = 0;
                } else if (ev.w == NAQ_MOUSE_MOVE && g_dragging) {
                    if (ev.key & NAQ_MOUSE_BUTTON_LEFT) {
                        scroll_to_drag(y);
                        dirty = 1;
                    } else {
                        g_dragging = 0;
                    }
                } else if (ev.w == NAQ_MOUSE_WHEEL) {
                    i64 top = (i64)g_top + (i64)ev.h * 3;

                    g_top = top < 0 ? 0u : (u32)top;
                    clamp_top();
                    dirty = 1;
                }
            } else if (ev.type == NAQ_WIN_EVENT_FOCUS) {
                dirty = 1;
            }
        }

        if (get_ticks() >= blink_at) {
            caret_on = !caret_on;
            blink_at = get_ticks() + BLINK_TICKS;
            dirty = 1;
        }

        /*
         * Only redraw when something changed; otherwise just wait.
         */
        if (dirty) {
            paint(caret_on);
            win_present(g_window, g_pixels, g_w, g_h);
            dirty = 0;
        }

        sleep_ms(16);
    }

    win_destroy(g_window);

    return 0;
}
