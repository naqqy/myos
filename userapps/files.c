#include "include/naqos.h"
#include "include/render3d.h"

/*
 * FILES.ELF
 *
 * File manager for the NAQ OS desktop.
 *
 * Browse folders, start programs and look inside files:
 *   folder      opens it
 *   .ELF        starts the program
 *   .NI         shows the image
 *   anything else is shown as text (first part of large files)
 *
 * Mouse:
 *   click a row to select it, double-click to open
 *   "Up" button goes to the parent folder (or back from a preview)
 *   scrollbar arrows and track scroll the list
 *
 * Keys:
 *   Up/Down, PgUp/PgDn   move the selection / scroll a preview
 *   Enter                open
 *   Backspace            parent folder / back
 *   Q                    quit
 */

#define START_W 560u
#define START_H 380u

#define MAX_W 960u
#define MAX_H 680u

#define MAX_ENTRIES 256u
#define PATH_MAX 120u

#define TOOLBAR_H 36
#define STATUS_H 22
#define ROW_H 22
#define SCROLL_W 16

#define PREVIEW_MAX (256u * 1024u)

#define DOUBLE_CLICK_TICKS 450u

#define MODE_LIST  0
#define MODE_TEXT  1
#define MODE_IMAGE 2

/* Aero palette */
#define COL_PAPER      0x00FFFFFFu
#define COL_ROW_ALT    0x00EEF8FEu
#define COL_TEXT       0x0016445Eu
#define COL_TEXT_DIM   0x006890A4u
#define COL_SELECT_TOP 0x0096E0F8u
#define COL_SELECT_BOT 0x002E9CD6u
#define COL_BAR_TOP    0x00E2FAFFu
#define COL_BAR_BOT    0x0078CCEEu
#define COL_BAR_EDGE   0x002A86B4u

static u32 g_pixels[MAX_W * MAX_H];

static u32 g_w = START_W;
static u32 g_h = START_H;
static u32 g_window_w = START_W;
static u32 g_window_h = START_H;

static naq_dirent_t g_entries[MAX_ENTRIES];
static int g_count = 0;
static int g_selected = -1;
static int g_scroll = 0;

static char g_path[PATH_MAX] = "/";
static char g_status[96] = "";

static int g_mode = MODE_LIST;

/*
 * Preview data. u32 storage keeps it 4-byte aligned for the image parser.
 */
static u32 g_preview[PREVIEW_MAX / 4u + 1u];
static u64 g_preview_size = 0;
static char g_preview_name[NAQ_DIRENT_NAME_MAX];
static r3d_texture_t g_image;
static int g_text_scroll = 0;

static int g_last_click_row = -1;
static u64 g_last_click_tick = 0;

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

static char upper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

static int str_compare_ci(const char* a, const char* b) {
    u32 i = 0;

    while (a[i] && b[i]) {
        char x = upper(a[i]);
        char y = upper(b[i]);

        if (x != y) {
            return x < y ? -1 : 1;
        }

        i++;
    }

    if (a[i] == b[i]) {
        return 0;
    }

    return a[i] ? 1 : -1;
}

static int has_extension(const char* name, const char* ext) {
    u32 len = (u32)strlen(name);
    u32 ext_len = (u32)strlen(ext);

    if (len <= ext_len || name[len - ext_len - 1] != '.') {
        return 0;
    }

    return str_compare_ci(name + len - ext_len, ext) == 0;
}

static void append_number(char* dst, u64 value, u32 size) {
    char digits[24];
    int n = 0;

    if (value == 0) {
        digits[n++] = '0';
    }

    while (value > 0 && n < 23) {
        digits[n++] = (char)('0' + value % 10u);
        value /= 10u;
    }

    char text[24];
    int i = 0;

    while (n > 0) {
        text[i++] = digits[--n];
    }

    text[i] = 0;

    str_append(dst, text, size);
}

static void format_size(char* dst, u64 bytes, u32 size) {
    dst[0] = 0;

    if (bytes < 1024u) {
        append_number(dst, bytes, size);
        str_append(dst, " B", size);
    } else if (bytes < 1024u * 1024u) {
        append_number(dst, (bytes + 512u) / 1024u, size);
        str_append(dst, " KB", size);
    } else {
        append_number(dst, bytes / (1024u * 1024u), size);
        str_append(dst, ".", size);
        append_number(dst, ((bytes % (1024u * 1024u)) * 10u) / (1024u * 1024u),
                      size);
        str_append(dst, " MB", size);
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
 * Glossy button with centred label.
 */
static void button(i32 x, i32 y, i32 w, i32 h, const char* label) {
    gradient(x, y, w, h, 0x00FFFFFFu, 0x008CD4F0u);
    rect(x + 1, y + 1, w - 2, h / 2 - 1, 0x00F4FDFFu);
    frame(x, y, w, h, COL_BAR_EDGE);

    i32 len = (i32)strlen(label);

    text(x + (w - len * GFX_FONT_W) / 2, y + (h - GFX_FONT_H) / 2, label,
         COL_TEXT);
}

/*
 * 16x16 entry icon: folder, program, picture or document.
 */
static void entry_icon(i32 x, i32 y, const naq_dirent_t* e) {
    if (e->type == NAQ_STAT_TYPE_DIR) {
        rect(x + 1, y + 2, 6, 3, 0x00F2C24Au);
        gradient(x, y + 4, 16, 10, 0x00FFE48Au, 0x00F0B040u);
        frame(x, y + 4, 16, 10, 0x00B07818u);
        return;
    }

    if (has_extension(e->name, "ELF")) {
        gradient(x + 1, y + 1, 14, 14, 0x00B4F0FFu, 0x002C9CE0u);
        frame(x + 1, y + 1, 14, 14, 0x00186C9Cu);
        rect(x + 4, y + 5, 8, 6, 0x00FFFFFFu);
        return;
    }

    if (has_extension(e->name, "NI")) {
        rect(x + 1, y + 2, 14, 12, 0x00FFFFFFu);
        frame(x + 1, y + 2, 14, 12, 0x00708CA0u);
        rect(x + 3, y + 8, 10, 4, 0x0058B860u);
        rect(x + 9, y + 4, 3, 3, 0x00F0C030u);
        return;
    }

    rect(x + 3, y + 1, 10, 14, 0x00FFFFFFu);
    frame(x + 3, y + 1, 10, 14, 0x00708CA0u);
    rect(x + 5, y + 5, 6, 1, 0x0090A8B8u);
    rect(x + 5, y + 8, 6, 1, 0x0090A8B8u);
    rect(x + 5, y + 11, 4, 1, 0x0090A8B8u);
}

/* -------------------------------------------------------------------------- */
/* Directory handling                                                          */
/* -------------------------------------------------------------------------- */

static int entry_before(const naq_dirent_t* a, const naq_dirent_t* b) {
    int a_dir = a->type == NAQ_STAT_TYPE_DIR;
    int b_dir = b->type == NAQ_STAT_TYPE_DIR;

    if (a_dir != b_dir) {
        return a_dir;
    }

    return str_compare_ci(a->name, b->name) < 0;
}

/*
 * Folders first, then by name.
 */
static void sort_entries(void) {
    for (int i = 1; i < g_count; i++) {
        naq_dirent_t key = g_entries[i];
        int j = i - 1;

        while (j >= 0 && entry_before(&key, &g_entries[j])) {
            g_entries[j + 1] = g_entries[j];
            j--;
        }

        g_entries[j + 1] = key;
    }
}

static int visible_rows(void) {
    int rows = ((int)g_h - TOOLBAR_H - STATUS_H) / ROW_H;

    return rows < 1 ? 1 : rows;
}

static void clamp_scroll(void) {
    int rows = visible_rows();

    if (g_scroll > g_count - rows) {
        g_scroll = g_count - rows;
    }

    if (g_scroll < 0) {
        g_scroll = 0;
    }
}

static void select_row(int index) {
    int rows = visible_rows();

    if (g_count == 0) {
        g_selected = -1;
        return;
    }

    if (index < 0) {
        index = 0;
    }

    if (index >= g_count) {
        index = g_count - 1;
    }

    g_selected = index;

    if (g_selected < g_scroll) {
        g_scroll = g_selected;
    }

    if (g_selected >= g_scroll + rows) {
        g_scroll = g_selected - rows + 1;
    }

    clamp_scroll();
}

static void load_directory(void) {
    int n = readdir(g_path, g_entries, MAX_ENTRIES);

    g_mode = MODE_LIST;
    g_count = n > 0 ? n : 0;
    g_scroll = 0;
    g_selected = g_count > 0 ? 0 : -1;
    g_last_click_row = -1;

    sort_entries();

    g_status[0] = 0;

    if (n < 0) {
        str_copy(g_status, "Cannot open this folder", sizeof(g_status));
    } else {
        append_number(g_status, (u64)g_count, sizeof(g_status));
        str_append(g_status, g_count == 1 ? " item" : " items", sizeof(g_status));
    }
}

static void make_path(char* out, u32 size, const char* name) {
    str_copy(out, g_path, size);

    if (out[0] && out[strlen(out) - 1] != '/') {
        str_append(out, "/", size);
    }

    str_append(out, name, size);
}

static void go_up(void) {
    if (g_mode != MODE_LIST) {
        g_mode = MODE_LIST;
        return;
    }

    u32 len = (u32)strlen(g_path);

    if (len <= 1) {
        return;
    }

    /*
     * Cut the last component.
     */
    while (len > 1 && g_path[len - 1] != '/') {
        len--;
    }

    if (len > 1) {
        len--;
    }

    g_path[len] = 0;

    if (!g_path[0]) {
        str_copy(g_path, "/", sizeof(g_path));
    }

    load_directory();
}

static void open_entry(int index) {
    char path[PATH_MAX];

    if (index < 0 || index >= g_count) {
        return;
    }

    const naq_dirent_t* e = &g_entries[index];

    make_path(path, sizeof(path), e->name);

    if (e->type == NAQ_STAT_TYPE_DIR) {
        str_copy(g_path, path, sizeof(g_path));
        load_directory();
        return;
    }

    if (has_extension(e->name, "ELF")) {
        int pid = spawn(path);

        g_status[0] = 0;

        if (pid > 0) {
            str_append(g_status, "Started ", sizeof(g_status));
            str_append(g_status, e->name, sizeof(g_status));
        } else {
            str_append(g_status, "Could not start ", sizeof(g_status));
            str_append(g_status, e->name, sizeof(g_status));
        }

        return;
    }

    /*
     * Preview. Large files show their first PREVIEW_MAX bytes.
     */
    u64 want = e->size < PREVIEW_MAX ? e->size : PREVIEW_MAX;
    int fd = open(path, 0);

    g_preview_size = 0;

    if (fd >= 0) {
        while (g_preview_size < want) {
            i64 got = read(
                fd,
                (u8*)g_preview + g_preview_size,
                want - g_preview_size
            );

            if (got <= 0) {
                break;
            }

            g_preview_size += (u64)got;
        }

        close(fd);
    }

    if (fd < 0 || (e->size > 0 && g_preview_size == 0)) {
        g_status[0] = 0;
        str_append(g_status, "Could not read ", sizeof(g_status));
        str_append(g_status, e->name, sizeof(g_status));
        return;
    }

    str_copy(g_preview_name, e->name, sizeof(g_preview_name));
    g_text_scroll = 0;

    if (has_extension(e->name, "NI") &&
        r3d_texture_parse(g_preview, g_preview_size, &g_image)) {
        g_mode = MODE_IMAGE;
    } else {
        g_mode = MODE_TEXT;
    }
}

/* -------------------------------------------------------------------------- */
/* Painting                                                                    */
/* -------------------------------------------------------------------------- */

static void paint_toolbar(void) {
    gradient(0, 0, (i32)g_w, TOOLBAR_H, COL_BAR_TOP, COL_BAR_BOT);
    rect(0, 1, (i32)g_w, TOOLBAR_H / 2 - 2, 0x00F0FCFFu);
    rect(0, TOOLBAR_H - 1, (i32)g_w, 1, COL_BAR_EDGE);

    button(8, 6, 44, 24, g_mode == MODE_LIST ? "Up" : "Back");

    /*
     * Address box.
     */
    i32 box_x = 60;
    i32 box_w = (i32)g_w - box_x - 8;

    if (box_w > 16) {
        rect(box_x, 6, box_w, 24, COL_PAPER);
        frame(box_x, 6, box_w, 24, COL_BAR_EDGE);

        if (g_mode == MODE_LIST) {
            text_n(box_x + 8, 10, g_path, (u32)((box_w - 16) / GFX_FONT_W),
                   COL_TEXT);
        } else {
            text_n(box_x + 8, 10, g_preview_name,
                   (u32)((box_w - 16) / GFX_FONT_W), COL_TEXT);
        }
    }
}

static void paint_status(void) {
    i32 y = (i32)g_h - STATUS_H;

    gradient(0, y, (i32)g_w, STATUS_H, 0x00EAF8FEu, 0x00B8E2F4u);
    rect(0, y, (i32)g_w, 1, COL_BAR_EDGE);
    text_n(8, y + 3, g_status, (u32)(((i32)g_w - 16) / GFX_FONT_W), COL_TEXT);
}

static void scrollbar_geometry(i32* track_y, i32* track_h, i32* thumb_y, i32* thumb_h) {
    int rows = visible_rows();

    *track_y = TOOLBAR_H + SCROLL_W;
    *track_h = (i32)g_h - TOOLBAR_H - STATUS_H - SCROLL_W * 2;

    if (*track_h < 8) {
        *track_h = 8;
    }

    if (g_count <= rows) {
        *thumb_y = *track_y;
        *thumb_h = *track_h;
        return;
    }

    *thumb_h = (*track_h * rows) / g_count;

    if (*thumb_h < 16) {
        *thumb_h = 16;
    }

    *thumb_y = *track_y +
        ((*track_h - *thumb_h) * g_scroll) / (g_count - rows);
}

static void paint_list(void) {
    int rows = visible_rows();
    i32 list_y = TOOLBAR_H;
    i32 list_h = (i32)g_h - TOOLBAR_H - STATUS_H;
    i32 list_w = (i32)g_w - SCROLL_W;

    rect(0, list_y, (i32)g_w, list_h, COL_PAPER);

    for (int r = 0; r < rows; r++) {
        int index = g_scroll + r;
        i32 y = list_y + r * ROW_H;

        if (index >= g_count) {
            break;
        }

        const naq_dirent_t* e = &g_entries[index];
        u32 ink = COL_TEXT;
        u32 dim = COL_TEXT_DIM;

        if (index == g_selected) {
            gradient(2, y + 1, list_w - 4, ROW_H - 2, COL_SELECT_TOP,
                     COL_SELECT_BOT);
            frame(2, y + 1, list_w - 4, ROW_H - 2, 0x001E82BCu);
            ink = 0x00FFFFFFu;
            dim = 0x00E6F8FFu;
        } else if (r & 1) {
            rect(0, y, list_w, ROW_H, COL_ROW_ALT);
        }

        entry_icon(8, y + 3, e);

        char size_text[24];

        size_text[0] = 0;

        if (e->type == NAQ_STAT_TYPE_DIR) {
            str_copy(size_text, "Folder", sizeof(size_text));
        } else {
            format_size(size_text, e->size, sizeof(size_text));
        }

        i32 size_w = (i32)strlen(size_text) * GFX_FONT_W;
        i32 size_x = list_w - size_w - 10;
        i32 name_chars = (size_x - 32 - 8) / GFX_FONT_W;

        if (name_chars > 0) {
            text_n(32, y + 3, e->name, (u32)name_chars, ink);
        }

        if (size_x > 120) {
            text(size_x, y + 3, size_text, dim);
        }
    }

    if (g_count == 0) {
        text(16, list_y + 12, "This folder is empty", COL_TEXT_DIM);
    }

    /*
     * Scrollbar: arrow buttons and a thumb.
     */
    i32 sx = list_w;
    i32 track_y;
    i32 track_h;
    i32 thumb_y;
    i32 thumb_h;

    scrollbar_geometry(&track_y, &track_h, &thumb_y, &thumb_h);

    rect(sx, list_y, SCROLL_W, list_h, 0x00E4F2F8u);
    rect(sx, list_y, 1, list_h, 0x00B4D4E2u);

    button(sx + 1, list_y, SCROLL_W - 1, SCROLL_W, "^");
    button(sx + 1, list_y + list_h - SCROLL_W, SCROLL_W - 1, SCROLL_W, "v");

    gradient(sx + 3, thumb_y, SCROLL_W - 5, thumb_h, 0x00B8E8FAu, 0x0058B4E0u);
    frame(sx + 3, thumb_y, SCROLL_W - 5, thumb_h, COL_BAR_EDGE);
}

static void paint_text(void) {
    i32 y0 = TOOLBAR_H;
    i32 area_h = (i32)g_h - TOOLBAR_H - STATUS_H;
    int cols = ((int)g_w - 16) / GFX_FONT_W;
    int rows = area_h / GFX_FONT_H;

    rect(0, y0, (i32)g_w, area_h, COL_PAPER);

    if (cols < 1) {
        return;
    }

    const char* data = (const char*)g_preview;
    int line = 0;
    int col = 0;
    char cell[2] = { 0, 0 };

    /*
     * Lay the text out line by line, wrapping at the window width.
     * Bytes that are not printable ASCII are shown as dots.
     */
    for (u64 i = 0; i < g_preview_size; i++) {
        char c = data[i];

        if (c == '\r') {
            continue;
        }

        if (c == '\n') {
            line++;
            col = 0;
            continue;
        }

        if (col >= cols) {
            line++;
            col = 0;
        }

        if (line >= g_text_scroll + rows) {
            break;
        }

        if (line >= g_text_scroll) {
            cell[0] = (c >= 32 && c <= 126) ? c : (c == '\t' ? ' ' : '.');

            text(8 + col * GFX_FONT_W,
                 y0 + (line - g_text_scroll) * GFX_FONT_H, cell, COL_TEXT);
        }

        col++;
    }

    if (g_preview_size == 0) {
        text(16, y0 + 12, "(empty file)", COL_TEXT_DIM);
    }
}

static void paint_image(void) {
    i32 y0 = TOOLBAR_H;
    i32 area_w = (i32)g_w;
    i32 area_h = (i32)g_h - TOOLBAR_H - STATUS_H;

    /*
     * Checkerboard behind the picture.
     */
    for (i32 y = 0; y < area_h; y++) {
        for (i32 x = 0; x < area_w; x++) {
            g_pixels[(u32)(y0 + y) * g_w + (u32)x] =
                (((x >> 4) + (y >> 4)) & 1) ? 0x00E8F4FAu : 0x00D4E8F2u;
        }
    }

    if (!g_image.pixels || area_w < 16 || area_h < 16) {
        return;
    }

    /*
     * Largest whole-number zoom that fits, so pixels stay crisp.
     */
    i32 zoom_x = (area_w - 16) / (i32)g_image.width;
    i32 zoom_y = (area_h - 16) / (i32)g_image.height;
    i32 zoom = zoom_x < zoom_y ? zoom_x : zoom_y;

    if (zoom < 1) {
        zoom = 1;
    }

    i32 draw_w = (i32)g_image.width * zoom;
    i32 draw_h = (i32)g_image.height * zoom;
    i32 dx = (area_w - draw_w) / 2;
    i32 dy = y0 + (area_h - draw_h) / 2;

    for (i32 y = 0; y < draw_h; y++) {
        i32 py = dy + y;

        if (py < y0 || py >= y0 + area_h) {
            continue;
        }

        const u32* src = g_image.pixels + (u32)(y / zoom) * g_image.width;

        for (i32 x = 0; x < draw_w; x++) {
            i32 px = dx + x;

            if (px >= 0 && px < area_w) {
                g_pixels[(u32)py * g_w + (u32)px] = src[x / zoom];
            }
        }
    }

    frame(dx - 1, dy - 1, draw_w + 2, draw_h + 2, COL_BAR_EDGE);
}

static void paint(void) {
    if (g_mode == MODE_LIST) {
        paint_list();
    } else if (g_mode == MODE_TEXT) {
        paint_text();
    } else {
        paint_image();
    }

    paint_toolbar();

    if (g_mode == MODE_IMAGE) {
        g_status[0] = 0;
        append_number(g_status, g_image.width, sizeof(g_status));
        str_append(g_status, " x ", sizeof(g_status));
        append_number(g_status, g_image.height, sizeof(g_status));
        str_append(g_status, " pixels", sizeof(g_status));
    } else if (g_mode == MODE_TEXT) {
        g_status[0] = 0;
        format_size(g_status, g_preview_size, sizeof(g_status));
        str_append(g_status, " shown - Up/Down scroll, Backspace returns",
                   sizeof(g_status));
    }

    paint_status();
}

/* -------------------------------------------------------------------------- */
/* Input                                                                       */
/* -------------------------------------------------------------------------- */

static void scroll_by(int rows) {
    if (g_mode == MODE_TEXT) {
        g_text_scroll += rows;

        if (g_text_scroll < 0) {
            g_text_scroll = 0;
        }

        return;
    }

    g_scroll += rows;
    clamp_scroll();
}

static void handle_click(i32 x, i32 y) {
    if (y < TOOLBAR_H) {
        if (x >= 8 && x < 52 && y >= 6 && y < 30) {
            go_up();
        }

        return;
    }

    if (g_mode != MODE_LIST || y >= (i32)g_h - STATUS_H) {
        return;
    }

    int rows = visible_rows();

    if (x >= (i32)g_w - SCROLL_W) {
        i32 list_y = TOOLBAR_H;
        i32 list_h = (i32)g_h - TOOLBAR_H - STATUS_H;
        i32 track_y;
        i32 track_h;
        i32 thumb_y;
        i32 thumb_h;

        scrollbar_geometry(&track_y, &track_h, &thumb_y, &thumb_h);

        if (y < list_y + SCROLL_W) {
            scroll_by(-1);
        } else if (y >= list_y + list_h - SCROLL_W) {
            scroll_by(1);
        } else if (y < thumb_y) {
            scroll_by(-rows);
        } else if (y >= thumb_y + thumb_h) {
            scroll_by(rows);
        }

        return;
    }

    int index = g_scroll + (y - TOOLBAR_H) / ROW_H;

    if (index < 0 || index >= g_count) {
        g_selected = -1;
        g_last_click_row = -1;
        return;
    }

    u64 now = get_ticks();

    if (index == g_last_click_row &&
        now - g_last_click_tick <= DOUBLE_CLICK_TICKS) {
        g_last_click_row = -1;
        open_entry(index);
        return;
    }

    g_selected = index;
    g_last_click_row = index;
    g_last_click_tick = now;
}

static void handle_key(u32 key, int* running) {
    int rows = visible_rows();

    if (key == 'q' || key == 'Q') {
        *running = 0;
    } else if (key == NAQ_KEY_BACKSPACE || key == NAQ_KEY_ESCAPE) {
        go_up();
    } else if (g_mode == MODE_LIST) {
        if (key == NAQ_KEY_UP) {
            select_row(g_selected - 1);
        } else if (key == NAQ_KEY_DOWN) {
            select_row(g_selected + 1);
        } else if (key == NAQ_KEY_PAGE_UP) {
            select_row(g_selected - rows);
        } else if (key == NAQ_KEY_PAGE_DOWN) {
            select_row(g_selected + rows);
        } else if (key == NAQ_KEY_HOME) {
            select_row(0);
        } else if (key == NAQ_KEY_END) {
            select_row(g_count - 1);
        } else if (key == NAQ_KEY_ENTER) {
            open_entry(g_selected);
        }
    } else if (g_mode == MODE_TEXT) {
        int page = ((int)g_h - TOOLBAR_H - STATUS_H) / GFX_FONT_H;

        if (key == NAQ_KEY_UP) {
            scroll_by(-1);
        } else if (key == NAQ_KEY_DOWN) {
            scroll_by(1);
        } else if (key == NAQ_KEY_PAGE_UP) {
            scroll_by(-page);
        } else if (key == NAQ_KEY_PAGE_DOWN) {
            scroll_by(page);
        } else if (key == NAQ_KEY_HOME) {
            g_text_scroll = 0;
        }
    }
}

static void set_size(u32 window_w, u32 window_h) {
    g_window_w = window_w ? window_w : 1;
    g_window_h = window_h ? window_h : 1;

    g_w = g_window_w > MAX_W ? MAX_W : g_window_w;
    g_h = g_window_h > MAX_H ? MAX_H : g_window_h;

    if (g_w < 120) {
        g_w = 120;
    }

    if (g_h < 100) {
        g_h = 100;
    }

    clamp_scroll();
}

int main(void) {
    int win = win_create(140, 80, START_W, START_H, "Files");

    if (win < 0) {
        puts("files: win_create failed\n");
        return 1;
    }

    load_directory();

    int running = 1;
    int dirty = 1;

    while (running) {
        naq_win_event_t ev;

        while (win_get_event(win, &ev) == 1) {
            if (ev.type == NAQ_WIN_EVENT_CLOSE) {
                running = 0;
            } else if (ev.type == NAQ_WIN_EVENT_KEY) {
                handle_key(ev.key, &running);
                dirty = 1;
            } else if (ev.type == NAQ_WIN_EVENT_RESIZE) {
                set_size((u32)ev.w, (u32)ev.h);
                dirty = 1;
            } else if (ev.type == NAQ_WIN_EVENT_MOUSE) {
                if (ev.w == NAQ_MOUSE_PRESS &&
                    ev.h == (i32)NAQ_MOUSE_BUTTON_LEFT) {
                    /*
                     * Window pixels -> frame pixels (they differ only when
                     * the window is larger than our biggest frame).
                     */
                    handle_click(
                        (i32)(((i64)ev.x * (i64)g_w) / (i64)g_window_w),
                        (i32)(((i64)ev.y * (i64)g_h) / (i64)g_window_h)
                    );
                    dirty = 1;
                }
            } else if (ev.type == NAQ_WIN_EVENT_FOCUS) {
                dirty = 1;
            }
        }

        /*
         * Only redraw when something changed; otherwise just wait.
         */
        if (dirty) {
            paint();
            win_present(win, g_pixels, g_w, g_h);
            dirty = 0;
        }

        sleep_ms(16);
    }

    win_destroy(win);

    return 0;
}
