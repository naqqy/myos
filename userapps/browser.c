#include "include/naqnet.h"
#include "include/naqimage.h"

/*
 * BROWSER.ELF
 *
 * A small web browser for the NAQ OS desktop.
 *
 * It fetches pages over HTTP and HTTPS and shows their text: headings,
 * paragraphs, lists, preformatted blocks and links you can click, plus
 * PNG, JPEG and GIF pictures.
 *
 * A small part of CSS is followed: hidden elements, text colour, bold,
 * large and centred text. Layout is still plain flowing text.
 *
 * JavaScript runs in the QuickJS engine (see browser/script.c and
 * browser/dom.js): scripts can read and change the page, react to clicks
 * and keys, use timers and fetch files. Text boxes, check boxes, buttons
 * and simple forms (GET) work. The "JS" button switches scripts off.
 *
 * What it does not do: cookies, video, canvas, POST forms. Scripts get no
 * sizes or positions, so sites that depend on layout still misbehave.
 * Text outside plain ASCII is approximated or shown as "?".
 *
 * HTTPS connections are encrypted, but the server's certificate is NOT
 * verified (see naqnet.h), and the status bar says so.
 *
 * Mouse:
 *   click a link to follow it, click the address box to type a new one
 *   wheel or scrollbar to scroll, Back button for the previous page
 *
 * Keys:
 *   Ctrl+L            edit the address          Enter   go
 *   (after clicking a text box on a page, keys go to that box)
 *   Up/Down, PgUp/PgDn, Home, End               scroll
 *   Backspace         back (when not typing)    Esc     stop typing
 */

#define START_W 760u
#define START_H 520u

#define MAX_W 1100u
#define MAX_H 760u

#define TOOLBAR_H 36
#define STATUS_H 22
#define SCROLL_W 16
#define MARGIN 12

#define URL_MAX 512u
#define PAGE_MAX (2u * 1024u * 1024u)
#define TEXT_MAX (1024u * 1024u)
#define LINKS_MAX 4000u
#define LINK_TEXT_MAX (384u * 1024u)
#define ITEMS_MAX 120000u
#define HISTORY_MAX 16

#define FETCH_TIMEOUT_MS 10000u
#define MAX_REDIRECTS 5

#define INDENT_W 24

/*
 * Pictures. Each is decoded at full size into a temporary buffer, then
 * shrunk to fit the page and kept in a shared pool until the next page.
 */
#define IMAGES_MAX 40u
#define IMAGE_DECODE_PIXELS (2400u * 1000u)
#define IMAGE_SCRATCH_BYTES (14u * 1024u * 1024u)
#define IMAGE_POOL_PIXELS (5u * 1024u * 1024u)
#define IMAGE_MAX_HEIGHT 1400u

#define IMAGE_PENDING 0
#define IMAGE_LOADED  1
#define IMAGE_FAILED  2

/*
 * Text styles (bit flags).
 */
#define STYLE_BOLD 0x01u
#define STYLE_BIG  0x02u    /* headings: double size */
#define STYLE_LINK 0x04u
#define STYLE_DIM  0x08u
#define STYLE_PRE  0x10u
#define STYLE_CENTER 0x20u
#define STYLE_FIELD  0x40u   /* a text box: drawn with a frame */
#define STYLE_BUTTON 0x80u   /* a button: drawn on a raised strip */

/*
 * What the HTML is boiled down to before layout.
 */
#define ITEM_WORD    0
#define ITEM_SPACE   1
#define ITEM_NEWLINE 2
#define ITEM_PARA    3      /* newline plus a gap */
#define ITEM_RULE    4      /* horizontal line */
#define ITEM_BULLET  5
#define ITEM_IMAGE   6      /* `reserved` holds the picture's index + 1 */

typedef struct item {
    u8 type;
    u8 style;
    u8 indent;
    u8 reserved;
    u16 link;       /* 0 = none, otherwise index + 1 into g_links */
    u16 length;
    u32 text;       /* offset into g_text */
    u32 color;      /* 0 = the default for its style */
    u32 node;       /* the script-side element it belongs to, or 0 */
} item_t;

/*
 * An item with a position, after layout.
 */
typedef struct box {
    i32 x;
    i32 y;
    u32 text;
    u16 length;
    u16 link;
    u8 type;
    u8 style;
    u8 image;       /* picture index + 1, for ITEM_IMAGE */
    u32 color;
    u32 node;
} box_t;

typedef struct picture {
    int state;
    u32 width;      /* as shown */
    u32 height;
    u32 pixels;     /* offset into g_image_pool */
} picture_t;

#define COL_PAPER    0x00FFFFFFu
#define COL_TEXT     0x00202A36u
#define COL_TEXT_DIM 0x006C7C8Cu
#define COL_LINK     0x000A58C8u
#define COL_HEADING  0x000C3C6Cu
#define COL_RULE     0x00C0CCD8u
#define COL_BAR_TOP  0x00E2FAFFu
#define COL_BAR_BOT  0x0078CCEEu
#define COL_BAR_EDGE 0x002A86B4u
#define COL_TRACK    0x00DCEEF6u

static u32 g_pixels[MAX_W * MAX_H];

static u32 g_w = START_W;
static u32 g_h = START_H;
static u32 g_window_w = START_W;
static u32 g_window_h = START_H;

static int g_window = -1;

/*
 * The page as received (headers and body), and what was made of it.
 */
static u8 g_page[PAGE_MAX];

/*
 * A copy of the page's HTML, kept while its style sheets are fetched
 * (each fetch reuses g_page).
 */
static char g_html[PAGE_MAX];
static u32 g_html_len = 0;
static char g_text[TEXT_MAX];
static u32 g_text_len = 0;

static item_t g_items[ITEMS_MAX];
static u32 g_item_count = 0;

static box_t g_boxes[ITEMS_MAX];
static u32 g_box_count = 0;
static i32 g_doc_height = 0;

static char g_link_text[LINK_TEXT_MAX];
static u32 g_link_text_len = 0;
static u32 g_links[LINKS_MAX];
static u32 g_link_count = 0;

static char g_url[URL_MAX] = "";
static char g_title[96] = "";
static char g_status[160] = "";
static int g_secure = 0;

static char g_history[HISTORY_MAX][URL_MAX];
static int g_history_count = 0;

static i32 g_scroll = 0;
static int g_dragging = 0;
static int g_hover_link = 0;

static int g_editing = 0;
static char g_edit[URL_MAX];
static u32 g_edit_len = 0;

static tls_conn_t g_tls;

static picture_t g_pictures[IMAGES_MAX];
static char g_picture_urls[IMAGES_MAX][URL_MAX];
static u32 g_picture_count = 0;

static u32 g_image_pool[IMAGE_POOL_PIXELS];
static u32 g_image_pool_used = 0;

static u32 g_image_decoded[IMAGE_DECODE_PIXELS];
static u8 g_image_scratch[IMAGE_SCRATCH_BYTES];

static int g_quit = 0;

/* Defined with the rest of the script support (browser/script.c). */
static int g_js_enabled;

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

static char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/*
 * 1 if `text` starts with `prefix`, ignoring case.
 */
static int starts_with(const char* text, const char* prefix) {
    for (u32 i = 0; prefix[i]; i++) {
        if (lower(text[i]) != lower(prefix[i])) {
            return 0;
        }
    }

    return 1;
}

static int str_equal(const char* a, const char* b) {
    u32 i = 0;

    while (a[i] && b[i]) {
        if (lower(a[i]) != lower(b[i])) {
            return 0;
        }

        i++;
    }

    return a[i] == b[i];
}

static void set_status(const char* message) {
    str_copy(g_status, message, sizeof(g_status));
}

/* -------------------------------------------------------------------------- */
/* URLs                                                                        */
/* -------------------------------------------------------------------------- */

typedef struct url_parts {
    int secure;
    char host[200];
    u16 port;
    char path[URL_MAX];
} url_parts_t;

static int url_parse(const char* url, url_parts_t* out) {
    const char* p = url;

    out->secure = 1;

    if (starts_with(p, "https://")) {
        p += 8;
    } else if (starts_with(p, "http://")) {
        out->secure = 0;
        p += 7;
    }

    u32 n = 0;

    while (*p && *p != '/' && *p != ':' && *p != '?' && *p != '#' &&
           n + 1 < sizeof(out->host)) {
        out->host[n++] = lower(*p++);
    }

    out->host[n] = 0;

    if (n == 0) {
        return 0;
    }

    out->port = out->secure ? 443 : 80;

    if (*p == ':') {
        u32 port = 0;

        p++;

        while (*p >= '0' && *p <= '9') {
            port = port * 10u + (u32)(*p - '0');
            p++;
        }

        if (port == 0 || port > 65535u) {
            return 0;
        }

        out->port = (u16)port;
    }

    n = 0;

    if (*p != '/') {
        out->path[n++] = '/';
    }

    /*
     * The part after "#" is for the browser only and is not sent.
     */
    while (*p && *p != '#' && n + 1 < sizeof(out->path)) {
        out->path[n++] = *p == ' ' ? '+' : *p;
        p++;
    }

    out->path[n] = 0;

    return 1;
}

/*
 * Turns a link as written in a page into a full URL, relative to the
 * page it was found on.
 */
static void url_resolve(const char* base, const char* link, char* out, u32 size) {
    url_parts_t parts;

    out[0] = 0;

    while (*link == ' ') {
        link++;
    }

    if (starts_with(link, "http://") || starts_with(link, "https://")) {
        str_copy(out, link, size);
        return;
    }

    if (!url_parse(base, &parts)) {
        return;
    }

    const char* scheme = parts.secure ? "https://" : "http://";

    if (link[0] == '/' && link[1] == '/') {
        str_append(out, parts.secure ? "https:" : "http:", size);
        str_append(out, link, size);
        return;
    }

    str_append(out, scheme, size);
    str_append(out, parts.host, size);

    if ((parts.secure && parts.port != 443) || (!parts.secure && parts.port != 80)) {
        char digits[8];
        u32 n = 0;
        u32 port = parts.port;
        char text[8];
        u32 t = 0;

        do {
            digits[n++] = (char)('0' + port % 10u);
            port /= 10u;
        } while (port);

        text[t++] = ':';

        while (n > 0) {
            text[t++] = digits[--n];
        }

        text[t] = 0;
        str_append(out, text, size);
    }

    if (link[0] == '/') {
        str_append(out, link, size);
        return;
    }

    /*
     * Relative to the folder of the current page.
     */
    char folder[URL_MAX];
    u32 cut = 1;

    str_copy(folder, parts.path, sizeof(folder));

    for (u32 i = 0; folder[i] && folder[i] != '?'; i++) {
        if (folder[i] == '/') {
            cut = i + 1;
        }
    }

    folder[cut] = 0;

    if (link[0] == '?' || link[0] == '#') {
        /*
         * Same page with a different query.
         */
        u32 end = 0;

        while (parts.path[end] && parts.path[end] != '?') {
            end++;
        }

        parts.path[end] = 0;
        str_append(out, parts.path, size);
        str_append(out, link, size);
        return;
    }

    while (link[0] == '.' && link[1] == '/') {
        link += 2;
    }

    while (link[0] == '.' && link[1] == '.' && link[2] == '/') {
        u32 len = (u32)strlen(folder);

        if (len > 1) {
            len--;

            while (len > 0 && folder[len - 1] != '/') {
                len--;
            }

            folder[len ? len : 1] = 0;

            if (!len) {
                folder[0] = '/';
            }
        }

        link += 3;
    }

    str_append(out, folder, size);
    str_append(out, link, size);
}

/* -------------------------------------------------------------------------- */
/* Fetching                                                                    */
/* -------------------------------------------------------------------------- */

static int g_sock = -1;
static int g_using_tls = 0;

static int conn_open(const url_parts_t* u) {
    g_using_tls = u->secure;

    if (u->secure) {
        if (!tls_connect(&g_tls, u->host, u->port, FETCH_TIMEOUT_MS)) {
            set_status(tls_error(&g_tls));
            return 0;
        }

        return 1;
    }

    g_sock = tcp_connect(u->host, u->port, FETCH_TIMEOUT_MS);

    if (g_sock < 0) {
        set_status("Could not connect to the server.");
        return 0;
    }

    return 1;
}

static int conn_write(const char* data, u32 length) {
    if (g_using_tls) {
        return tls_write(&g_tls, data, length);
    }

    return tcp_write_all(g_sock, data, length, FETCH_TIMEOUT_MS);
}

/*
 * Returns the byte count, 0 at the end, negative on error.
 */
static int conn_read(u8* buffer, u32 capacity) {
    if (g_using_tls) {
        return tls_read(&g_tls, buffer, capacity);
    }

    return tcp_read(g_sock, buffer, capacity, FETCH_TIMEOUT_MS);
}

static void conn_close(void) {
    if (g_using_tls) {
        tls_close(&g_tls);
    } else if (g_sock > 0) {
        sock_close(g_sock);
        g_sock = -1;
    }
}

/*
 * Finds a header (name without the colon) in the block of header lines.
 * Copies its value and returns 1 if present.
 */
static int header_value(const char* headers, u32 length, const char* name, char* out, u32 size) {
    u32 name_len = (u32)strlen(name);

    for (u32 i = 0; i < length; ) {
        u32 end = i;

        while (end < length && headers[end] != '\n') {
            end++;
        }

        if (end - i > name_len && headers[i + name_len] == ':' && starts_with(headers + i, name)) {
            u32 at = i + name_len + 1;
            u32 n = 0;

            while (at < end && headers[at] == ' ') {
                at++;
            }

            while (at < end && headers[at] != '\r' && n + 1 < size) {
                out[n++] = headers[at++];
            }

            out[n] = 0;
            return 1;
        }

        i = end + 1;
    }

    return 0;
}

/*
 * "Chunked" bodies arrive as [hex size]\r\n[data]\r\n ... 0\r\n. Rewrites
 * the body in place without the framing. Returns the new length.
 */
static u32 unchunk(u8* body, u32 length) {
    u32 in = 0;
    u32 out = 0;

    while (in < length) {
        u32 size = 0;
        int digits = 0;

        while (in < length) {
            u8 c = body[in];
            u32 v;

            if (c >= '0' && c <= '9') {
                v = (u32)(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                v = (u32)(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                v = (u32)(c - 'A' + 10);
            } else {
                break;
            }

            size = size * 16u + v;
            digits++;
            in++;
        }

        while (in < length && body[in] != '\n') {
            in++;
        }

        in++;

        if (!digits || size == 0) {
            break;
        }

        if (in + size > length) {
            size = length > in ? length - in : 0;
        }

        for (u32 i = 0; i < size; i++) {
            body[out++] = body[in + i];
        }

        in += size + 2u;
    }

    return out;
}

/*
 * Fetches one URL (no redirect handling). On success returns 1 with the
 * body location and length, the status code, and the headers.
 */
static int fetch_once(
    const char* url,
    int* out_status,
    u8** out_body,
    u32* out_length,
    char** out_headers,
    u32* out_headers_len
) {
    url_parts_t u;
    static char request[URL_MAX + 512];

    if (!url_parse(url, &u)) {
        set_status("That address is not valid.");
        return 0;
    }

    if (!net_ready()) {
        set_status("The network is not connected.");
        return 0;
    }

    if (!conn_open(&u)) {
        return 0;
    }

    request[0] = 0;
    str_append(request, "GET ", sizeof(request));
    str_append(request, u.path, sizeof(request));
    str_append(request, " HTTP/1.1\r\nHost: ", sizeof(request));
    str_append(request, u.host, sizeof(request));
    str_append(request,
               "\r\nUser-Agent: NAQOS-Browser/0.1\r\n"
               "Accept: text/html, text/plain;q=0.9, */*;q=0.1\r\n"
               "Accept-Encoding: identity\r\n"
               "Connection: close\r\n\r\n",
               sizeof(request));

    if (!conn_write(request, (u32)strlen(request))) {
        conn_close();
        set_status("Could not send the request.");
        return 0;
    }

    u32 total = 0;

    for (;;) {
        if (total >= PAGE_MAX - 1u) {
            break;
        }

        int got = conn_read(g_page + total, PAGE_MAX - 1u - total);

        if (got <= 0) {
            break;
        }

        total += (u32)got;
    }

    conn_close();

    g_page[total] = 0;

    /*
     * Headers end at the first blank line.
     */
    u32 split = 0;

    for (u32 i = 0; i + 3 < total; i++) {
        if (g_page[i] == '\r' && g_page[i + 1] == '\n' &&
            g_page[i + 2] == '\r' && g_page[i + 3] == '\n') {
            split = i + 4;
            break;
        }
    }

    if (split == 0 || total < 12u || !starts_with((const char*)g_page, "HTTP/")) {
        set_status(total ? "The server sent something that is not a web page."
                         : "The server sent nothing back.");
        return 0;
    }

    /*
     * "HTTP/1.1 200 OK": the code is after the first space.
     */
    int status = 0;

    for (u32 i = 0; i < 16u && g_page[i]; i++) {
        if (g_page[i] == ' ') {
            for (u32 k = i + 1; g_page[k] >= '0' && g_page[k] <= '9'; k++) {
                status = status * 10 + (g_page[k] - '0');
            }

            break;
        }
    }

    char encoding[32];
    u8* body = g_page + split;
    u32 body_len = total - split;

    if (header_value((const char*)g_page, split, "Transfer-Encoding", encoding, sizeof(encoding)) &&
        starts_with(encoding, "chunked")) {
        body_len = unchunk(body, body_len);
    }

    body[body_len] = 0;

    *out_status = status;
    *out_body = body;
    *out_length = body_len;
    *out_headers = (char*)g_page;
    *out_headers_len = split;

    return 1;
}

/* -------------------------------------------------------------------------- */
/* HTML -> items                                                               */
/* -------------------------------------------------------------------------- */

/*
 * Parser state while walking the page.
 */
static u8 g_style = 0;
static u8 g_indent = 0;
static u16 g_link = 0;
static int g_pending = 0;       /* 0 none, 1 newline, 2 paragraph gap */
static int g_need_space = 0;
static int g_line_empty = 1;
static u32 g_color = 0;

/*
 * The element the text being read belongs to, when the page comes from
 * the script engine (which writes "<nq 12>" in front of each piece).
 */
static u32 g_node = 0;

/*
 * Set right after a list bullet: block elements inside the item must not
 * push its text onto the next line.
 */
static int g_after_bullet = 0;

static void emit(u8 type, u32 text, u32 length) {
    if (g_item_count >= ITEMS_MAX) {
        return;
    }

    item_t* it = &g_items[g_item_count++];

    it->type = type;
    it->style = g_style;
    it->indent = g_indent;
    it->reserved = 0;
    it->link = g_link;
    it->length = (u16)length;
    it->text = text;
    it->color = g_color;
    it->node = g_node;
}

static void flush_break(void) {
    if (g_pending && g_item_count > 0) {
        emit(g_pending == 2 ? ITEM_PARA : ITEM_NEWLINE, 0, 0);
    }

    if (g_pending) {
        g_line_empty = 1;
        g_need_space = 0;
    }

    g_pending = 0;
}

static void block_break(int level) {
    if (g_after_bullet) {
        return;
    }

    if (level > g_pending) {
        g_pending = level;
    }
}

static void emit_word(const char* word, u32 length) {
    if (!length || g_text_len + length + 1u >= TEXT_MAX) {
        return;
    }

    flush_break();

    if (g_need_space && !g_line_empty) {
        emit(ITEM_SPACE, 0, 0);
    }

    g_need_space = 0;

    for (u32 i = 0; i < length; i++) {
        g_text[g_text_len + i] = word[i];
    }

    emit(ITEM_WORD, g_text_len, length);

    g_text_len += length;
    g_line_empty = 0;
    g_after_bullet = 0;
}

/*
 * A named or numeric character reference at `p` (after the "&").
 * Writes its replacement and returns how many source bytes it used, or 0
 * if this is not a reference.
 */
static u32 decode_entity(const char* p, u32 available, char* out, u32* out_len) {
    static const char* const names[] = {
        "amp;", "lt;", "gt;", "quot;", "apos;", "nbsp;", "copy;", "reg;",
        "mdash;", "ndash;", "hellip;", "rsquo;", "lsquo;", "ldquo;", "rdquo;",
        "middot;", "bull;", "laquo;", "raquo;", "trade;", "times;", "rarr;",
        "larr;", "deg;"
    };

    static const char* const values[] = {
        "&", "<", ">", "\"", "'", " ", "(c)", "(R)",
        "--", "-", "...", "'", "'", "\"", "\"",
        "*", "*", "<<", ">>", "(TM)", "x", "->",
        "<-", " deg"
    };

    *out_len = 0;

    if (available >= 2 && p[0] == '#') {
        u32 code = 0;
        u32 i = 1;
        int hex = (p[1] == 'x' || p[1] == 'X');

        if (hex) {
            i = 2;
        }

        u32 start = i;

        while (i < available && i < 10u) {
            char c = p[i];

            if (c >= '0' && c <= '9') {
                code = code * (hex ? 16u : 10u) + (u32)(c - '0');
            } else if (hex && lower(c) >= 'a' && lower(c) <= 'f') {
                code = code * 16u + (u32)(lower(c) - 'a' + 10);
            } else {
                break;
            }

            i++;
        }

        if (i == start || i >= available || p[i] != ';') {
            return 0;
        }

        if (code >= 32u && code < 127u) {
            out[0] = (char)code;
        } else if (code == 160u) {
            out[0] = ' ';
        } else if (code == 8217u || code == 8216u) {
            out[0] = '\'';
        } else if (code == 8220u || code == 8221u) {
            out[0] = '"';
        } else if (code == 8211u || code == 8212u) {
            out[0] = '-';
        } else {
            out[0] = '?';
        }

        *out_len = 1;

        return i + 1u;
    }

    for (u32 n = 0; n < sizeof(names) / sizeof(names[0]); n++) {
        u32 len = (u32)strlen(names[n]);

        if (len <= available) {
            int match = 1;

            for (u32 i = 0; i < len; i++) {
                if (p[i] != names[n][i]) {
                    match = 0;
                    break;
                }
            }

            if (match) {
                str_copy(out, values[n], 8);
                *out_len = (u32)strlen(out);
                return len;
            }
        }
    }

    return 0;
}

/*
 * A UTF-8 sequence starting at `p`. Writes an ASCII stand-in and returns
 * how many bytes the sequence used.
 */
static u32 decode_utf8(const u8* p, u32 available, char* out, u32* out_len) {
    u32 code;
    u32 size;

    if ((p[0] & 0xE0u) == 0xC0u) {
        size = 2;
        code = p[0] & 0x1Fu;
    } else if ((p[0] & 0xF0u) == 0xE0u) {
        size = 3;
        code = p[0] & 0x0Fu;
    } else if ((p[0] & 0xF8u) == 0xF0u) {
        size = 4;
        code = p[0] & 0x07u;
    } else {
        out[0] = '?';
        *out_len = 1;
        return 1;
    }

    if (size > available) {
        size = available;
    }

    for (u32 i = 1; i < size; i++) {
        code = (code << 6) | (p[i] & 0x3Fu);
    }

    *out_len = 1;

    switch (code) {
        case 0x00A0: out[0] = ' '; break;
        case 0x2018: case 0x2019: case 0x02BC: out[0] = '\''; break;
        case 0x201C: case 0x201D: out[0] = '"'; break;
        case 0x2010: case 0x2011: case 0x2012: case 0x2013: out[0] = '-'; break;
        case 0x2022: case 0x00B7: out[0] = '*'; break;
        case 0x00D7: out[0] = 'x'; break;

        case 0x2014:
            out[0] = '-';
            out[1] = '-';
            *out_len = 2;
            break;

        case 0x2026:
            out[0] = '.';
            out[1] = '.';
            out[2] = '.';
            *out_len = 3;
            break;

        case 0x00A9:
            out[0] = '(';
            out[1] = 'c';
            out[2] = ')';
            *out_len = 3;
            break;

        default:
            /*
             * Accented Latin letters: show the plain letter.
             */
            if (code >= 0x00C0 && code <= 0x00FF) {
                static const char plain[] =
                    "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";

                out[0] = plain[code - 0x00C0];
            } else {
                out[0] = '?';
            }
            break;
    }

    return size;
}

/*
 * Reads the value of an attribute out of a tag's text ("<a href=...>").
 */
static int attribute(const char* tag, u32 length, const char* name, char* out, u32 size) {
    u32 name_len = (u32)strlen(name);

    out[0] = 0;

    for (u32 i = 0; i + name_len < length; i++) {
        if ((tag[i] != ' ' && tag[i] != '\n' && tag[i] != '\t' && tag[i] != '\r') ||
            !starts_with(tag + i + 1, name)) {
            continue;
        }

        u32 at = i + 1 + name_len;

        while (at < length && tag[at] == ' ') {
            at++;
        }

        if (at >= length || tag[at] != '=') {
            continue;
        }

        at++;

        while (at < length && tag[at] == ' ') {
            at++;
        }

        char quote = 0;

        if (at < length && (tag[at] == '"' || tag[at] == '\'')) {
            quote = tag[at++];
        }

        u32 n = 0;

        while (at < length && n + 1 < size) {
            char c = tag[at];

            if (quote ? c == quote : (c == ' ' || c == '>')) {
                break;
            }

            /*
             * Links often carry "&amp;" for "&".
             */
            if (c == '&' && at + 4 < length && starts_with(tag + at, "&amp;")) {
                out[n++] = '&';
                at += 5;
                continue;
            }

            if (c == '&' && at + 5 < length && starts_with(tag + at, "&quot;")) {
                out[n++] = '"';
                at += 6;
                continue;
            }

            out[n++] = c;
            at++;
        }

        out[n] = 0;

        return 1;
    }

    return 0;
}

/*
 * 1 if the tag has the attribute at all, with or without a value
 * ("<input checked>").
 */
static int has_flag(const char* tag, u32 length, const char* name) {
    u32 name_len = (u32)strlen(name);

    for (u32 i = 0; i + name_len < length; i++) {
        if ((tag[i] != ' ' && tag[i] != '\n' && tag[i] != '\t' && tag[i] != '\r') ||
            !starts_with(tag + i + 1, name)) {
            continue;
        }

        u32 at = i + 1 + name_len;

        if (at >= length || tag[at] == ' ' || tag[at] == '=' || tag[at] == '/' ||
            tag[at] == '\n' || tag[at] == '\t' || tag[at] == '\r') {
            return 1;
        }
    }

    return 0;
}

/*
 * Records a picture to fetch later and puts a place for it in the page.
 * Until (or unless) it loads, its alternative text is shown instead.
 */
static void emit_image(const char* tag, u32 tag_len) {
    char src[URL_MAX];
    char full[URL_MAX];
    char alt[100];
    u8 index = 0;

    if (!attribute(tag, tag_len, "src", src, sizeof(src)) || !src[0]) {
        /*
         * Pages that load pictures by script often keep the address here.
         */
        attribute(tag, tag_len, "data-src", src, sizeof(src));
    }

    full[0] = 0;

    if (src[0] && !starts_with(src, "data:")) {
        url_resolve(g_url, src, full, sizeof(full));
    }

    u32 len = (u32)strlen(full);

    /*
     * SVG is a drawing language, not one of the formats we decode.
     */
    int usable = len > 0 &&
                 !(len > 4 && starts_with(full + len - 4, ".svg"));

    /*
     * The same picture again (or the page being laid out again after a
     * script changed it): use the one already fetched.
     */
    for (u32 i = 0; usable && !index && i < g_picture_count; i++) {
        if (str_equal(g_picture_urls[i], full)) {
            index = (u8)(i + 1u);
        }
    }

    if (usable && !index && g_picture_count < IMAGES_MAX) {
        picture_t* p = &g_pictures[g_picture_count];

        p->state = IMAGE_PENDING;
        p->width = 0;
        p->height = 0;
        p->pixels = 0;

        str_copy(g_picture_urls[g_picture_count], full, URL_MAX);

        index = (u8)(++g_picture_count);
    }

    alt[0] = 0;

    if (attribute(tag, tag_len, "alt", alt + 1, sizeof(alt) - 3u) && alt[1]) {
        alt[0] = '[';
        str_append(alt, "]", sizeof(alt));
    }

    u32 alt_len = (u32)strlen(alt);

    if (!index && !alt_len) {
        return;
    }

    if (g_item_count >= ITEMS_MAX || g_text_len + alt_len + 1u >= TEXT_MAX) {
        return;
    }

    flush_break();

    if (g_need_space && !g_line_empty) {
        emit(ITEM_SPACE, 0, 0);
    }

    g_need_space = 0;

    for (u32 i = 0; i < alt_len; i++) {
        char c = alt[i];

        g_text[g_text_len + i] = ((u8)c < 32u || (u8)c > 126u) ? '?' : c;
    }

    emit(ITEM_IMAGE, g_text_len, alt_len);
    g_items[g_item_count - 1u].reserved = index;

    g_text_len += alt_len;
    g_line_empty = 0;
    g_after_bullet = 0;
}

/*
 * A form control: a text box, check box, button or list. Each becomes
 * one word, drawn in a way that shows what it is.
 */
static void emit_input(const char* tag, u32 tag_len) {
    char type[24];
    char value[160];
    char shown[200];
    u32 n = 0;
    u8 style = 0;
    u32 color = 0;

    if (!attribute(tag, tag_len, "type", type, sizeof(type)) || !type[0]) {
        str_copy(type, "text", sizeof(type));
    }

    for (u32 i = 0; type[i]; i++) {
        type[i] = lower(type[i]);
    }

    if (str_equal(type, "hidden")) {
        return;
    }

    attribute(tag, tag_len, "value", value, sizeof(value));

    for (u32 i = 0; value[i]; i++) {
        if ((u8)value[i] < 32u || (u8)value[i] > 126u) {
            value[i] = '?';
        }
    }

    shown[0] = 0;

    if (str_equal(type, "checkbox")) {
        str_copy(shown, has_flag(tag, tag_len, "checked") ? "[x]" : "[ ]", sizeof(shown));
        style = STYLE_BUTTON;
    } else if (str_equal(type, "radio")) {
        str_copy(shown, has_flag(tag, tag_len, "checked") ? "(o)" : "( )", sizeof(shown));
        style = STYLE_BUTTON;
    } else if (str_equal(type, "submit") || str_equal(type, "button") ||
               str_equal(type, "reset") || str_equal(type, "image")) {
        str_append(shown, " ", sizeof(shown));
        str_append(shown, value[0] ? value
                                   : (str_equal(type, "reset") ? "Reset"
                                   : (str_equal(type, "button") ? "Button" : "Submit")),
                   sizeof(shown));
        str_append(shown, " ", sizeof(shown));
        style = STYLE_BUTTON;
    } else if (str_equal(type, "select")) {
        /* written by the script engine for a <select>: the chosen entry */
        str_append(shown, " ", sizeof(shown));
        str_append(shown, value, sizeof(shown));
        str_append(shown, " v ", sizeof(shown));
        style = STYLE_BUTTON;
    } else {
        char number[8];
        u32 width = 20;
        int focus = has_flag(tag, tag_len, "data-nqfocus");
        u32 length = (u32)strlen(value);

        if (attribute(tag, tag_len, "size", number, sizeof(number))) {
            u32 v = 0;

            for (u32 i = 0; number[i] >= '0' && number[i] <= '9'; i++) {
                v = v * 10u + (u32)(number[i] - '0');
            }

            if (v >= 3u) {
                width = v > 60u ? 60u : v;
            }
        }

        if (length == 0 && !focus) {
            /* empty: the hint, greyed */
            attribute(tag, tag_len, "placeholder", value, sizeof(value));
            length = (u32)strlen(value);
            color = COL_TEXT_DIM;

            for (u32 i = 0; i < length; i++) {
                if ((u8)value[i] < 32u || (u8)value[i] > 126u) {
                    value[i] = '?';
                }
            }
        } else if (str_equal(type, "password")) {
            for (u32 i = 0; i < length; i++) {
                value[i] = '*';
            }
        }

        /* While typing, the end of a long text stays in view. */
        u32 room = focus ? width - 1u : width;
        u32 skip = (focus && length > room) ? length - room : 0;

        shown[n++] = ' ';

        for (u32 i = skip; i < length && n - 1u < room; i++) {
            shown[n++] = value[i];
        }

        if (focus) {
            shown[n++] = '|';
        }

        while (n - 1u < width) {
            shown[n++] = ' ';
        }

        shown[n++] = ' ';
        shown[n] = 0;
        style = STYLE_FIELD;
    }

    u8 saved_style = g_style;
    u32 saved_color = g_color;
    u16 saved_link = g_link;

    g_style = (u8)((g_style & (STYLE_CENTER | STYLE_BIG)) | style);
    g_color = color;
    g_link = 0;

    emit_word(shown, (u32)strlen(shown));

    g_style = saved_style;
    g_color = saved_color;
    g_link = saved_link;
    g_need_space = 1;
}

static u16 add_link(const char* target) {
    char full[URL_MAX];

    if (g_link_count >= LINKS_MAX) {
        return 0;
    }

    if (starts_with(target, "javascript:") || starts_with(target, "mailto:") ||
        starts_with(target, "tel:") || target[0] == 0) {
        return 0;
    }

    url_resolve(g_url, target, full, sizeof(full));

    u32 len = (u32)strlen(full);

    if (len == 0 || g_link_text_len + len + 1u >= LINK_TEXT_MAX) {
        return 0;
    }

    g_links[g_link_count] = g_link_text_len;
    str_copy(g_link_text + g_link_text_len, full, len + 1u);
    g_link_text_len += len + 1u;

    return (u16)(++g_link_count);
}

static int tag_named(const char* name, u32 length, const char* want);

/* -------------------------------------------------------------------------- */
/* CSS                                                                         */
/* -------------------------------------------------------------------------- */

/*
 * A small part of CSS: enough to hide what a page hides, and to follow
 * its colours, bold text, large text and centred text.
 *
 * Selectors understood:   tag   .class   #id   tag.class#id   and up to
 *                         three ancestors in front ("nav ul li a")
 * Properties understood:  display (none or not), visibility, color,
 *                         font-weight, font-size, text-align
 *
 * Everything else is ignored: positioning, floats, flexbox and grid,
 * margins, backgrounds, pseudo-classes, attribute selectors. Pages are
 * still laid out as flowing text.
 *
 * Names are compared by hash rather than as strings, which keeps a rule
 * small enough to hold several thousand of them.
 */
#define CSS_RULES_MAX 6000u
#define CSS_STACK_MAX 192
#define CSS_ELEM_CLASSES 8
#define CSS_SHEETS_MAX 4

#define CSS_UNSET (-1)

typedef struct css_props {
    i8 hidden;      /* 1 = display:none, 0 = shown, CSS_UNSET = not said */
    i8 bold;
    i8 big;
    i8 center;
    u8 has_color;
    u32 color;
} css_props_t;

typedef struct css_compound {
    u32 tag;        /* 0 = any */
    u32 id;         /* 0 = any */
    u32 cls[3];
    u8 class_count;
} css_compound_t;

#define CSS_CHAIN_MAX 4

typedef struct css_rule {
    /*
     * part[0] is the element itself; part[1], part[2]... are the
     * ancestors it must sit inside, nearest first.
     */
    css_compound_t part[CSS_CHAIN_MAX];
    u8 part_count;
    u8 weight;      /* 1 tag only, 2 has a class, 3 has an id */
    css_props_t props;
} css_rule_t;

/*
 * One open element of the page, with the styles in force inside it.
 */
typedef struct css_element {
    u32 tag;
    u32 id;
    u32 cls[CSS_ELEM_CLASSES];
    u8 class_count;

    u8 hidden;
    i8 bold;
    i8 big;
    i8 center;
    u32 color;
} css_element_t;

static css_rule_t g_rules[CSS_RULES_MAX];
static u32 g_rule_count = 0;

static css_element_t g_stack[CSS_STACK_MAX];
static int g_stack_depth = 0;

static void css_reset(void) {
    g_rule_count = 0;
    g_stack_depth = 0;
}

static u32 css_hash(const char* text, u32 length, int fold_case) {
    u32 h = 2166136261u;

    for (u32 i = 0; i < length; i++) {
        char c = fold_case ? lower(text[i]) : text[i];

        h = (h ^ (u8)c) * 16777619u;
    }

    return h ? h : 1u;
}

static int css_is_space(char c) {
    return c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '\f';
}

static int css_name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_';
}

static void css_props_clear(css_props_t* p) {
    p->hidden = CSS_UNSET;
    p->bold = CSS_UNSET;
    p->big = CSS_UNSET;
    p->center = CSS_UNSET;
    p->has_color = 0;
    p->color = 0;
}

static int css_props_empty(const css_props_t* p) {
    return p->hidden == CSS_UNSET && p->bold == CSS_UNSET && p->big == CSS_UNSET &&
           p->center == CSS_UNSET && !p->has_color;
}

static void css_props_merge(css_props_t* into, const css_props_t* from) {
    if (from->hidden != CSS_UNSET) {
        into->hidden = from->hidden;
    }

    if (from->bold != CSS_UNSET) {
        into->bold = from->bold;
    }

    if (from->big != CSS_UNSET) {
        into->big = from->big;
    }

    if (from->center != CSS_UNSET) {
        into->center = from->center;
    }

    if (from->has_color) {
        into->has_color = 1;
        into->color = from->color;
    }
}

static int css_word_is(const char* text, u32 length, const char* word) {
    u32 len = (u32)strlen(word);

    if (length != len) {
        return 0;
    }

    for (u32 i = 0; i < len; i++) {
        if (lower(text[i]) != word[i]) {
            return 0;
        }
    }

    return 1;
}

static int css_hex(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }

    c = lower(c);

    return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
}

/*
 * "#rgb", "#rrggbb", "rgb(r, g, b)" or one of the common colour names.
 */
static int css_parse_color(const char* v, u32 length, u32* out) {
    static const char* const names[] = {
        "black", "white", "red", "green", "blue", "gray", "grey", "silver",
        "maroon", "navy", "teal", "purple", "orange", "yellow", "olive", "brown"
    };

    static const u32 values[] = {
        0x000000u, 0xFFFFFFu, 0xE00000u, 0x008000u, 0x0000E0u, 0x808080u, 0x808080u,
        0xC0C0C0u, 0x800000u, 0x000080u, 0x008080u, 0x800080u, 0xE08000u, 0xC0A000u,
        0x808000u, 0x8B4513u
    };

    if (length >= 4 && v[0] == '#') {
        int d[6];
        u32 n = 0;

        while (n < 6 && 1 + n < length && css_hex(v[1 + n]) >= 0) {
            d[n] = css_hex(v[1 + n]);
            n++;
        }

        if (n == 3) {
            *out = ((u32)(d[0] * 17) << 16) | ((u32)(d[1] * 17) << 8) | (u32)(d[2] * 17);
            return 1;
        }

        if (n == 6) {
            *out = ((u32)(d[0] * 16 + d[1]) << 16) | ((u32)(d[2] * 16 + d[3]) << 8) |
                   (u32)(d[4] * 16 + d[5]);
            return 1;
        }

        return 0;
    }

    if (length > 4 && starts_with(v, "rgb")) {
        u32 part[3] = { 0, 0, 0 };
        u32 count = 0;
        u32 i = 3;

        while (i < length && v[i] != '(') {
            i++;
        }

        i++;

        while (i < length && count < 3) {
            while (i < length && (css_is_space(v[i]) || v[i] == ',')) {
                i++;
            }

            if (i >= length || v[i] < '0' || v[i] > '9') {
                break;
            }

            u32 n = 0;

            while (i < length && v[i] >= '0' && v[i] <= '9') {
                n = n * 10u + (u32)(v[i] - '0');
                i++;
            }

            /*
             * Skip a fraction and anything like "%".
             */
            while (i < length && v[i] != ',' && v[i] != ')' && !css_is_space(v[i])) {
                i++;
            }

            part[count++] = n > 255u ? 255u : n;
        }

        if (count == 3) {
            *out = (part[0] << 16) | (part[1] << 8) | part[2];
            return 1;
        }

        return 0;
    }

    for (u32 n = 0; n < sizeof(names) / sizeof(names[0]); n++) {
        if (css_word_is(v, length, names[n])) {
            *out = values[n];
            return 1;
        }
    }

    return 0;
}

/*
 * A CSS length as tenths of its unit: "1.5em" -> 15, with *unit set to
 * the first letter of the unit ('p' for px, 'e' for em, 'r' for rem,
 * '%' for percent).
 */
static int css_parse_number(const char* v, u32 length, u32* tenths, char* unit) {
    u32 i = 0;
    u32 whole = 0;
    u32 fraction = 0;

    if (length == 0 || !((v[0] >= '0' && v[0] <= '9') || v[0] == '.')) {
        return 0;
    }

    while (i < length && v[i] >= '0' && v[i] <= '9') {
        whole = whole * 10u + (u32)(v[i] - '0');
        i++;
    }

    if (i < length && v[i] == '.') {
        i++;

        if (i < length && v[i] >= '0' && v[i] <= '9') {
            fraction = (u32)(v[i] - '0');
        }

        while (i < length && v[i] >= '0' && v[i] <= '9') {
            i++;
        }
    }

    *tenths = whole * 10u + fraction;
    *unit = i < length ? lower(v[i]) : 0;

    return 1;
}

static void css_declaration(css_props_t* p, const char* name, u32 name_len,
                            const char* value, u32 value_len) {
    if (css_word_is(name, name_len, "display")) {
        p->hidden = css_word_is(value, value_len, "none") ? 1 : 0;
    } else if (css_word_is(name, name_len, "visibility")) {
        if (css_word_is(value, value_len, "hidden") ||
            css_word_is(value, value_len, "collapse")) {
            p->hidden = 1;
        }
    } else if (css_word_is(name, name_len, "color")) {
        u32 color = 0;

        if (css_parse_color(value, value_len, &color)) {
            /*
             * The page background here is always white, so text meant
             * for a dark background would vanish; such colours are
             * left out.
             */
            u32 light = ((color >> 16) & 0xFFu) + ((color >> 8) & 0xFFu) + (color & 0xFFu);

            if (light < 540u) {
                p->has_color = 1;
                p->color = color ? color : 0x00010101u;
            }
        }
    } else if (css_word_is(name, name_len, "font-weight")) {
        u32 tenths = 0;
        char unit = 0;

        if (css_word_is(value, value_len, "bold") || css_word_is(value, value_len, "bolder")) {
            p->bold = 1;
        } else if (css_word_is(value, value_len, "normal") ||
                   css_word_is(value, value_len, "lighter")) {
            p->bold = 0;
        } else if (css_parse_number(value, value_len, &tenths, &unit)) {
            p->bold = tenths >= 6000u ? 1 : 0;
        }
    } else if (css_word_is(name, name_len, "font-size")) {
        u32 tenths = 0;
        char unit = 0;

        if (css_word_is(value, value_len, "x-large") ||
            css_word_is(value, value_len, "xx-large") ||
            css_word_is(value, value_len, "xxx-large")) {
            p->big = 1;
        } else if (css_parse_number(value, value_len, &tenths, &unit)) {
            if (unit == 'p') {
                p->big = tenths >= 220u ? 1 : 0;
            } else if (unit == 'e' || unit == 'r') {
                p->big = tenths >= 14u ? 1 : 0;
            } else if (unit == '%') {
                p->big = tenths >= 1400u ? 1 : 0;
            }
        }
    } else if (css_word_is(name, name_len, "text-align")) {
        p->center = css_word_is(value, value_len, "center") ? 1 : 0;
    }
}

/*
 * "name: value; name: value" -> props.
 */
static void css_declarations(css_props_t* p, const char* text, u32 length) {
    u32 i = 0;

    while (i < length) {
        while (i < length && (css_is_space(text[i]) || text[i] == ';')) {
            i++;
        }

        u32 name_start = i;

        while (i < length && css_name_char(text[i])) {
            i++;
        }

        u32 name_len = i - name_start;

        while (i < length && css_is_space(text[i])) {
            i++;
        }

        if (i >= length || text[i] != ':' || name_len == 0) {
            while (i < length && text[i] != ';') {
                i++;
            }

            continue;
        }

        i++;

        while (i < length && css_is_space(text[i])) {
            i++;
        }

        u32 value_start = i;

        while (i < length && text[i] != ';') {
            i++;
        }

        u32 value_len = i - value_start;

        /*
         * Drop "!important" and trailing spaces.
         */
        for (u32 k = 0; k < value_len; k++) {
            if (text[value_start + k] == '!') {
                value_len = k;
                break;
            }
        }

        while (value_len > 0 && css_is_space(text[value_start + value_len - 1u])) {
            value_len--;
        }

        css_declaration(p, text + name_start, name_len, text + value_start, value_len);
    }
}

/*
 * One part of a selector, like "div.note#top". Returns 0 for anything
 * not understood.
 */
static int css_parse_compound(const char* s, u32 length, css_compound_t* out) {
    u32 i = 0;

    out->tag = 0;
    out->id = 0;
    out->class_count = 0;

    u32 start = i;

    while (i < length && css_name_char(s[i])) {
        i++;
    }

    if (i > start) {
        out->tag = css_hash(s + start, i - start, 1);
    }

    while (i < length) {
        char kind = s[i++];

        if (kind != '.' && kind != '#') {
            return 0;
        }

        start = i;

        while (i < length && css_name_char(s[i])) {
            i++;
        }

        if (i == start) {
            return 0;
        }

        if (kind == '#') {
            out->id = css_hash(s + start, i - start, 0);
        } else {
            if (out->class_count >= 3) {
                return 0;
            }

            out->cls[out->class_count++] = css_hash(s + start, i - start, 0);
        }
    }

    return out->tag || out->id || out->class_count;
}

static void css_add_rule(const char* s, u32 length, const css_props_t* props) {
    css_rule_t* rule;

    while (length > 0 && css_is_space(s[0])) {
        s++;
        length--;
    }

    while (length > 0 && css_is_space(s[length - 1u])) {
        length--;
    }

    if (length == 0 || g_rule_count >= CSS_RULES_MAX) {
        return;
    }

    rule = &g_rules[g_rule_count];
    rule->part_count = 0;

    /*
     * Walk the selector from its end: the last part is the element
     * itself, each part before it an ancestor. ">" is treated like a
     * space. A selector with more parts than we keep, or with anything
     * not understood in it, is dropped rather than applied too widely.
     */
    u32 end = length;

    while (end > 0) {
        u32 start = end;

        while (start > 0 && !css_is_space(s[start - 1u]) && s[start - 1u] != '>') {
            start--;
        }

        if (rule->part_count >= CSS_CHAIN_MAX ||
            !css_parse_compound(s + start, end - start, &rule->part[rule->part_count])) {
            return;
        }

        rule->part_count++;

        end = start;

        while (end > 0 && (css_is_space(s[end - 1u]) || s[end - 1u] == '>')) {
            end--;
        }
    }

    rule->weight = rule->part[0].id ? 3 : (rule->part[0].class_count ? 2 : 1);
    rule->props = *props;

    g_rule_count++;
}

/*
 * Skips a "{ ... }" block, nested ones included. `i` is just past the
 * opening brace.
 */
static u32 css_skip_block(const char* text, u32 length, u32 i) {
    int depth = 1;

    while (i < length && depth > 0) {
        if (text[i] == '{') {
            depth++;
        } else if (text[i] == '}') {
            depth--;
        }

        i++;
    }

    return i;
}

static int css_contains(const char* text, u32 length, const char* word) {
    u32 len = (u32)strlen(word);

    for (u32 i = 0; i + len <= length; i++) {
        if (starts_with(text + i, word)) {
            return 1;
        }
    }

    return 0;
}

/*
 * Reads a style sheet and adds its rules.
 */
static void css_parse(const char* text, u32 length) {
    u32 i = 0;

    while (i < length) {
        if (css_is_space(text[i]) || text[i] == '}') {
            /*
             * A stray "}" closes an @media block we went into.
             */
            i++;
            continue;
        }

        if (text[i] == '/' && i + 1 < length && text[i + 1] == '*') {
            i += 2;

            while (i + 1 < length && !(text[i] == '*' && text[i + 1] == '/')) {
                i++;
            }

            i += 2;
            continue;
        }

        if (text[i] == '<') {
            /*
             * "<!--" and "-->" are allowed around style sheets.
             */
            i++;
            continue;
        }

        u32 start = i;

        while (i < length && text[i] != '{' && text[i] != ';' && text[i] != '}') {
            i++;
        }

        if (i >= length) {
            break;
        }

        if (text[start] == '@') {
            if (text[i] != '{') {
                i++;
                continue;
            }

            i++;

            /*
             * Go inside @media blocks that apply to a wide screen; skip
             * print styles, small-screen styles and every other @-rule.
             */
            if (starts_with(text + start, "@media") &&
                !css_contains(text + start, i - start, "print") &&
                !css_contains(text + start, i - start, "max-width") &&
                !css_contains(text + start, i - start, "prefers") &&
                !css_contains(text + start, i - start, "speech")) {
                continue;
            }

            i = css_skip_block(text, length, i);
            continue;
        }

        if (text[i] != '{') {
            i++;
            continue;
        }

        u32 selector_end = i;

        i++;

        u32 body_start = i;

        while (i < length && text[i] != '}') {
            i++;
        }

        css_props_t props;

        css_props_clear(&props);
        css_declarations(&props, text + body_start, i - body_start);

        i++;

        if (css_props_empty(&props)) {
            continue;
        }

        /*
         * "a, b, c { ... }" is three rules.
         */
        u32 part = start;

        for (u32 k = start; k <= selector_end; k++) {
            if (k == selector_end || text[k] == ',') {
                css_add_rule(text + part, k - part, &props);
                part = k + 1u;
            }
        }
    }
}

static int css_matches(const css_compound_t* c, const css_element_t* e) {
    if (c->tag && c->tag != e->tag) {
        return 0;
    }

    if (c->id && c->id != e->id) {
        return 0;
    }

    for (u32 i = 0; i < c->class_count; i++) {
        int found = 0;

        for (u32 k = 0; k < e->class_count; k++) {
            if (e->cls[k] == c->cls[i]) {
                found = 1;
                break;
            }
        }

        if (!found) {
            return 0;
        }
    }

    return 1;
}

/*
 * The styles that apply to an element, from all matching rules (tag
 * rules first, then class rules, then id rules) and then its own
 * style="" attribute.
 */
static void css_compute(const css_element_t* e, const char* tag, u32 tag_len, css_props_t* out) {
    static char inline_style[512];

    css_props_clear(out);

    for (u8 weight = 1; weight <= 3; weight++) {
        for (u32 r = 0; r < g_rule_count; r++) {
            const css_rule_t* rule = &g_rules[r];

            if (rule->weight != weight || !css_matches(&rule->part[0], e)) {
                continue;
            }

            /*
             * Each further part must match some ancestor above the one
             * the previous part matched.
             */
            int depth = g_stack_depth - 1;
            int matched = 1;

            for (u32 k = 1; k < rule->part_count; k++) {
                while (depth >= 0 && !css_matches(&rule->part[k], &g_stack[depth])) {
                    depth--;
                }

                if (depth < 0) {
                    matched = 0;
                    break;
                }

                depth--;
            }

            if (!matched) {
                continue;
            }

            css_props_merge(out, &rule->props);
        }
    }

    if (attribute(tag, tag_len, "style", inline_style, sizeof(inline_style)) && inline_style[0]) {
        css_declarations(out, inline_style, (u32)strlen(inline_style));
    }
}

static int css_hidden_now(void) {
    return g_stack_depth > 0 && g_stack[g_stack_depth - 1].hidden;
}

/*
 * Tracks an opening or closing tag. Returns 1 if the tag sits in (or is)
 * a hidden part of the page and should have no effect.
 */
static int css_element(const char* name, u32 name_len, const char* tag, u32 tag_len, int closing) {
    static const char* const void_tags[] = {
        "br", "img", "hr", "meta", "link", "input", "source", "track", "wbr",
        "area", "base", "col", "embed", "param"
    };

    static const char* const self_closing[] = {
        "p", "li", "option", "tr", "td", "th", "dt", "dd"
    };

    u32 hash = css_hash(name, name_len, 1);

    if (closing) {
        int index = -1;

        for (int d = g_stack_depth - 1; d >= 0; d--) {
            if (g_stack[d].tag == hash) {
                index = d;
                break;
            }
        }

        if (index < 0) {
            return css_hidden_now();
        }

        int was_hidden = g_stack[index].hidden;

        g_stack_depth = index;

        return was_hidden;
    }

    /*
     * Some elements end when the next one of the same kind starts
     * ("<li>one<li>two").
     */
    for (u32 n = 0; n < sizeof(self_closing) / sizeof(self_closing[0]); n++) {
        if (g_stack_depth > 0 && g_stack[g_stack_depth - 1].tag == hash &&
            tag_named(name, name_len, self_closing[n])) {
            g_stack_depth--;
            break;
        }
    }

    css_element_t e;
    char value[256];

    e.tag = hash;
    e.id = 0;
    e.class_count = 0;

    if (attribute(tag, tag_len, "id", value, sizeof(value)) && value[0]) {
        e.id = css_hash(value, (u32)strlen(value), 0);
    }

    if (attribute(tag, tag_len, "class", value, sizeof(value))) {
        for (u32 i = 0; value[i] && e.class_count < CSS_ELEM_CLASSES; ) {
            while (css_is_space(value[i])) {
                i++;
            }

            u32 start = i;

            while (value[i] && !css_is_space(value[i])) {
                i++;
            }

            if (i > start) {
                e.cls[e.class_count++] = css_hash(value + start, i - start, 0);
            }
        }
    }

    css_props_t props;

    css_compute(&e, tag, tag_len, &props);

    /*
     * Start from what the enclosing element passes down.
     */
    if (g_stack_depth > 0) {
        const css_element_t* parent = &g_stack[g_stack_depth - 1];

        e.hidden = parent->hidden;
        e.bold = parent->bold;
        e.big = parent->big;
        e.center = parent->center;
        e.color = parent->color;
    } else {
        e.hidden = 0;
        e.bold = CSS_UNSET;
        e.big = CSS_UNSET;
        e.center = CSS_UNSET;
        e.color = 0;
    }

    if (props.hidden == 1) {
        e.hidden = 1;
    }

    if (props.bold != CSS_UNSET) {
        e.bold = props.bold;
    }

    if (props.big != CSS_UNSET) {
        e.big = props.big;
    }

    if (props.center != CSS_UNSET) {
        e.center = props.center;
    }

    if (props.has_color) {
        e.color = props.color;
    }

    int is_void = tag_len > 0 && tag[tag_len - 1u] == '/';

    for (u32 n = 0; n < sizeof(void_tags) / sizeof(void_tags[0]) && !is_void; n++) {
        if (tag_named(name, name_len, void_tags[n])) {
            is_void = 1;
        }
    }

    if (!is_void && g_stack_depth < CSS_STACK_MAX) {
        g_stack[g_stack_depth++] = e;
    }

    return e.hidden;
}

static int tag_is(const char* name, u32 length, const char* want) {
    return tag_named(name, length, want);
}

static int tag_named(const char* name, u32 length, const char* want) {
    u32 len = (u32)strlen(want);

    if (length != len) {
        return 0;
    }

    for (u32 i = 0; i < len; i++) {
        if (lower(name[i]) != want[i]) {
            return 0;
        }
    }

    return 1;
}

/*
 * Finds "</name" after `from`. Returns the offset just past the closing
 * ">" or the end of the page.
 */
static u32 skip_element(const char* html, u32 length, u32 from, const char* name) {
    u32 name_len = (u32)strlen(name);

    for (u32 i = from; i + name_len + 2u < length; i++) {
        if (html[i] == '<' && html[i + 1] == '/' && starts_with(html + i + 2, name)) {
            while (i < length && html[i] != '>') {
                i++;
            }

            return i + 1u;
        }
    }

    return length;
}

static void parse_html(const char* html, u32 length) {
    char word[256];
    u32 word_len = 0;
    int in_pre = 0;
    int in_title = 0;
    int heading = 0;
    int bold = 0;
    int in_button = 0;
    u32 title_len = 0;

    g_node = 0;
    g_style = 0;
    g_indent = 0;
    g_link = 0;
    g_pending = 0;
    g_need_space = 0;
    g_line_empty = 1;
    g_after_bullet = 0;
    g_color = 0;
    g_stack_depth = 0;

    for (u32 i = 0; i < length; ) {
        char c = html[i];

        if (c == '<') {
            /*
             * Comments run to "-->".
             */
            if (i + 3 < length && html[i + 1] == '!' && html[i + 2] == '-' && html[i + 3] == '-') {
                u32 end = i + 4;

                while (end + 2 < length &&
                       !(html[end] == '-' && html[end + 1] == '-' && html[end + 2] == '>')) {
                    end++;
                }

                i = end + 3;
                continue;
            }

            u32 end = i + 1;

            while (end < length && html[end] != '>') {
                end++;
            }

            if (end >= length) {
                break;
            }

            /*
             * A "<" that does not start a tag is just text.
             */
            char first = html[i + 1];

            if (!(first == '/' || first == '!' || first == '?' ||
                  (lower(first) >= 'a' && lower(first) <= 'z'))) {
                if (word_len + 1 < sizeof(word)) {
                    word[word_len++] = '<';
                }

                i++;
                continue;
            }

            if (word_len) {
                emit_word(word, word_len);
                word_len = 0;
            }

            const char* tag = html + i + 1;
            u32 tag_len = end - i - 1;
            int closing = tag[0] == '/';
            const char* name = tag + (closing ? 1 : 0);
            u32 name_len = 0;

            while (name_len < tag_len - (u32)closing &&
                   ((lower(name[name_len]) >= 'a' && lower(name[name_len]) <= 'z') ||
                    (name[name_len] >= '0' && name[name_len] <= '9'))) {
                name_len++;
            }

            i = end + 1;

            /*
             * Things that are not page text.
             */
            if (!closing) {
                if (tag_is(name, name_len, "script")) {
                    i = skip_element(html, length, i, "script");
                    continue;
                }

                if (tag_is(name, name_len, "style")) {
                    i = skip_element(html, length, i, "style");
                    continue;
                }

                if (tag_is(name, name_len, "svg")) {
                    i = skip_element(html, length, i, "svg");
                    continue;
                }

                if (tag_is(name, name_len, "template")) {
                    i = skip_element(html, length, i, "template");
                    continue;
                }
            }

            if (tag_is(name, name_len, "title")) {
                in_title = !closing;
                continue;
            }

            /*
             * "<nq 12>": the text that follows belongs to element 12 of
             * the script engine's document.
             */
            if (name_len == 2 && tag_is(name, name_len, "nq")) {
                u32 value = 0;

                for (u32 k = 3; k < tag_len && tag[k] >= '0' && tag[k] <= '9'; k++) {
                    value = value * 10u + (u32)(tag[k] - '0');
                }

                g_node = value;
                continue;
            }

            if (css_element(name, name_len, tag, tag_len, closing)) {
                continue;
            }

            if (name_len == 2 && lower(name[0]) == 'h' && name[1] >= '1' && name[1] <= '6') {
                block_break(2);
                heading = closing ? 0 : name[1] - '0';
            } else if (tag_is(name, name_len, "b") || tag_is(name, name_len, "strong")) {
                bold = !closing;
            } else if (tag_is(name, name_len, "a")) {
                if (closing) {
                    g_link = 0;
                } else {
                    char href[URL_MAX];

                    g_link = attribute(tag, tag_len, "href", href, sizeof(href))
                        ? add_link(href)
                        : 0;
                }
            } else if (tag_is(name, name_len, "br")) {
                /*
                 * Two in a row leave a blank line.
                 */
                block_break(g_pending ? 2 : 1);
            } else if (tag_is(name, name_len, "p") || tag_is(name, name_len, "table") ||
                       tag_is(name, name_len, "form") || tag_is(name, name_len, "dl") ||
                       tag_is(name, name_len, "figure")) {
                block_break(2);
            } else if (tag_is(name, name_len, "div") || tag_is(name, name_len, "tr") ||
                       tag_is(name, name_len, "section") || tag_is(name, name_len, "article") ||
                       tag_is(name, name_len, "header") || tag_is(name, name_len, "footer") ||
                       tag_is(name, name_len, "nav") || tag_is(name, name_len, "main") ||
                       tag_is(name, name_len, "dt") || tag_is(name, name_len, "dd") ||
                       tag_is(name, name_len, "option") || tag_is(name, name_len, "aside") ||
                       tag_is(name, name_len, "address") || tag_is(name, name_len, "caption") ||
                       tag_is(name, name_len, "figcaption")) {
                block_break(1);
            } else if (tag_is(name, name_len, "ul") || tag_is(name, name_len, "ol") ||
                       tag_is(name, name_len, "blockquote")) {
                block_break(tag_is(name, name_len, "blockquote") ? 2 : 1);

                if (closing) {
                    if (g_indent > 0) {
                        g_indent--;
                    }
                } else if (g_indent < 8) {
                    g_indent++;
                }
            } else if (tag_is(name, name_len, "li")) {
                if (!closing) {
                    block_break(1);
                    flush_break();
                    emit(ITEM_BULLET, 0, 0);
                    g_line_empty = 1;
                    g_after_bullet = 1;
                } else {
                    g_after_bullet = 0;
                }
            } else if (tag_is(name, name_len, "td") || tag_is(name, name_len, "th")) {
                /*
                 * Table cells on one line, spaced apart.
                 */
                if (!closing && !g_line_empty && g_text_len + 4u < TEXT_MAX) {
                    g_need_space = 0;
                    emit_word("  ", 2);
                }
            } else if (tag_is(name, name_len, "hr")) {
                block_break(1);
                flush_break();
                emit(ITEM_RULE, 0, 0);
                block_break(1);
            } else if (tag_is(name, name_len, "pre")) {
                block_break(2);
                in_pre = !closing;
            } else if (tag_is(name, name_len, "img")) {
                if (!closing) {
                    emit_image(tag, tag_len);
                }
            } else if (tag_is(name, name_len, "input")) {
                if (!closing) {
                    emit_input(tag, tag_len);
                }
            } else if (tag_is(name, name_len, "button")) {
                in_button = !closing;
                g_need_space = 1;
            }

            /*
             * Style in force from here on.
             */
            g_style = 0;

            if (heading == 1 || heading == 2) {
                g_style |= STYLE_BIG;
            }

            if (heading || bold) {
                g_style |= STYLE_BOLD;
            }

            if (g_link) {
                g_style |= STYLE_LINK;
            }

            if (in_pre) {
                g_style |= STYLE_PRE;
            }

            if (in_button) {
                g_style |= STYLE_BUTTON;
            }

            /*
             * What the page's own styles say, on top of the defaults.
             */
            g_color = 0;

            if (g_stack_depth > 0) {
                const css_element_t* top = &g_stack[g_stack_depth - 1];

                if (top->bold == 1) {
                    g_style |= STYLE_BOLD;
                } else if (top->bold == 0 && !heading) {
                    g_style &= (u8)~STYLE_BOLD;
                }

                if (top->big == 1) {
                    g_style |= STYLE_BIG;
                }

                if (top->center == 1) {
                    g_style |= STYLE_CENTER;
                }

                g_color = top->color;
            }

            continue;
        }

        /*
         * Text.
         */
        char decoded[8];
        u32 decoded_len = 1;
        u32 used = 1;

        decoded[0] = c;

        if (c == '&') {
            u32 n = decode_entity(html + i + 1, length - i - 1, decoded, &decoded_len);

            if (n) {
                used = n + 1u;
            } else {
                decoded[0] = '&';
                decoded_len = 1;
            }
        } else if ((u8)c >= 0x80u) {
            used = decode_utf8((const u8*)html + i, length - i, decoded, &decoded_len);
        }

        i += used;

        if (css_hidden_now()) {
            continue;
        }

        if (in_title) {
            for (u32 k = 0; k < decoded_len && title_len + 1 < sizeof(g_title); k++) {
                char t = decoded[k];

                if (t == '\n' || t == '\r' || t == '\t') {
                    t = ' ';
                }

                g_title[title_len++] = t;
            }

            g_title[title_len] = 0;
            continue;
        }

        for (u32 k = 0; k < decoded_len; k++) {
            char t = decoded[k];
            int is_space = (t == ' ' || t == '\n' || t == '\t' || t == '\r');

            if (in_pre && t == '\n') {
                if (word_len) {
                    emit_word(word, word_len);
                    word_len = 0;
                }

                /*
                 * A blank line inside preformatted text is kept.
                 */
                block_break(g_pending ? 2 : 1);
                continue;
            }

            if (in_pre && (t == ' ' || t == '\t')) {
                /*
                 * Spaces are significant: keep them as part of the text.
                 */
                if (word_len + 1 < sizeof(word)) {
                    word[word_len++] = ' ';
                }

                continue;
            }

            if (is_space) {
                if (word_len) {
                    emit_word(word, word_len);
                    word_len = 0;
                }

                g_need_space = 1;
                continue;
            }

            if ((u8)t < 32u || (u8)t > 126u) {
                t = '?';
            }

            word[word_len++] = t;

            if (word_len + 1 >= sizeof(word)) {
                emit_word(word, word_len);
                word_len = 0;
            }
        }
    }

    if (word_len) {
        emit_word(word, word_len);
    }
}

/*
 * Plain text: every line as it is.
 */
static void parse_plain(const char* text, u32 length) {
    char word[256];
    u32 word_len = 0;

    g_style = STYLE_PRE;
    g_indent = 0;
    g_link = 0;
    g_pending = 0;
    g_need_space = 0;
    g_line_empty = 1;

    for (u32 i = 0; i < length; i++) {
        char c = text[i];

        if (c == '\n') {
            if (word_len) {
                emit_word(word, word_len);
                word_len = 0;
            }

            block_break(g_pending ? 2 : 1);
            continue;
        }

        if (c == '\r') {
            continue;
        }

        if (c == '\t') {
            c = ' ';
        }

        if ((u8)c < 32u || (u8)c > 126u) {
            c = '?';
        }

        word[word_len++] = c;

        if (word_len + 1 >= sizeof(word)) {
            emit_word(word, word_len);
            word_len = 0;
        }
    }

    if (word_len) {
        emit_word(word, word_len);
    }
}

/* -------------------------------------------------------------------------- */
/* Layout                                                                      */
/* -------------------------------------------------------------------------- */

static i32 view_height(void) {
    return (i32)g_h - TOOLBAR_H - STATUS_H;
}

static i32 text_width(void) {
    return (i32)g_w - SCROLL_W - MARGIN * 2;
}

static void clamp_scroll(void) {
    i32 max = g_doc_height - view_height();

    if (g_scroll > max) {
        g_scroll = max;
    }

    if (g_scroll < 0) {
        g_scroll = 0;
    }
}

/*
 * Moves the boxes of a finished line so that it sits in the middle.
 * `right` is where the line's content ends.
 */
static void center_line(u32 first, i32 right, i32 width) {
    i32 shift = (width - right) / 2;

    if (shift <= 0) {
        return;
    }

    for (u32 i = first; i < g_box_count; i++) {
        g_boxes[i].x += shift;
    }
}

/*
 * Places every item: words flow left to right and wrap at the right
 * edge; breaks start new lines.
 */
static void layout(void) {
    i32 width = text_width();
    i32 x = 0;
    i32 y = 4;
    i32 line_h = 18;
    int line_used = 0;
    u32 line_first = 0;
    int line_center = 0;

    g_box_count = 0;

    for (u32 i = 0; i < g_item_count; i++) {
        const item_t* it = &g_items[i];
        i32 left = (i32)it->indent * INDENT_W;
        i32 scale = (it->style & STYLE_BIG) ? 2 : 1;

        if (!line_used) {
            x = left;
        }

        if (it->type == ITEM_NEWLINE || it->type == ITEM_PARA) {
            if (line_center) {
                center_line(line_first, x, width);
            }

            line_first = g_box_count;
            line_center = 0;

            y += line_h + (it->type == ITEM_PARA ? 10 : 0);
            x = left;
            line_h = 18;
            line_used = 0;
            continue;
        }

        if (it->type == ITEM_SPACE) {
            if (line_used) {
                x += GFX_FONT_W * scale;
            }

            continue;
        }

        box_t* b = &g_boxes[g_box_count];

        if (it->type == ITEM_RULE) {
            b->x = left;
            b->y = y + 8;
            b->text = 0;
            b->length = 0;
            b->link = 0;
            b->type = ITEM_RULE;
            b->style = 0;
            b->node = 0;
            g_box_count++;

            line_used = 1;
            continue;
        }

        if (it->type == ITEM_BULLET) {
            b->x = left - 14;
            b->y = y;
            b->text = 0;
            b->length = 0;
            b->link = 0;
            b->type = ITEM_BULLET;
            b->style = 0;
            b->node = 0;
            g_box_count++;
            continue;
        }

        if (it->type == ITEM_IMAGE) {
            const picture_t* p = it->reserved ? &g_pictures[it->reserved - 1u] : 0;

            if (p && p->state == IMAGE_LOADED) {
                i32 pw = (i32)p->width;
                i32 ph = (i32)p->height;

                if (line_used && x + pw > width) {
                    if (line_center) {
                        center_line(line_first, x, width);
                    }

                    line_first = g_box_count;
                    line_center = 0;

                    y += line_h;
                    x = left;
                    line_h = 18;
                }

                if (line_h < ph + 4) {
                    line_h = ph + 4;
                }

                b->x = x;
                b->y = y;
                b->text = 0;
                b->length = 0;
                b->link = it->link;
                b->type = ITEM_IMAGE;
                b->style = 0;
                b->image = it->reserved;
                b->color = 0;
                b->node = it->node;
                g_box_count++;

                if (it->style & STYLE_CENTER) {
                    line_center = 1;
                }

                x += pw + 4;
                line_used = 1;
                continue;
            }

            /*
             * Not loaded: fall through and place the alternative text,
             * dimmed, like a word.
             */
            if (it->length == 0) {
                continue;
            }
        }

        i32 w = (i32)it->length * GFX_FONT_W * scale;

        /*
         * Wrap, unless this is preformatted text (which keeps its lines)
         * or the word alone is wider than the page.
         */
        if (line_used && x + w > width && !(it->style & STYLE_PRE)) {
            if (line_center) {
                center_line(line_first, x, width);
            }

            line_first = g_box_count;
            line_center = 0;

            y += line_h;
            x = left;
            line_h = 18;
            line_used = 0;
        }

        if (scale == 2 && line_h < 38) {
            line_h = 38;
        }

        b->x = x;
        b->y = y;
        b->text = it->text;
        b->length = it->length;
        b->link = it->link;
        b->type = ITEM_WORD;
        b->style = it->type == ITEM_IMAGE ? (u8)(it->style | STYLE_DIM) : it->style;
        b->image = 0;
        b->color = it->color;
        b->node = it->node;
        g_box_count++;

        if (it->style & STYLE_CENTER) {
            line_center = 1;
        }

        x += w;
        line_used = 1;
    }

    if (line_center) {
        center_line(line_first, x, width);
    }

    g_doc_height = y + line_h + 16;

    clamp_scroll();
}

/* -------------------------------------------------------------------------- */
/* Loading a page                                                              */
/* -------------------------------------------------------------------------- */

/*
 * Forgets what was made of the page's HTML, but not the page itself:
 * its pictures, styles, title and scroll position stay.
 */
static void reset_items(void) {
    g_text_len = 0;
    g_item_count = 0;
    g_box_count = 0;
    g_link_count = 0;
    g_link_text_len = 0;
    g_hover_link = 0;
}

static void reset_document(void) {
    reset_items();
    g_doc_height = 0;
    g_scroll = 0;
    g_title[0] = 0;
    g_picture_count = 0;
    g_image_pool_used = 0;
    g_html_len = 0;
    css_reset();
}

static void layout(void);

/*
 * Lays the page out again from new HTML in g_html (after a script
 * changed the document), staying at the same place in it.
 */
static void rerender(u32 length) {
    i32 scroll = g_scroll;

    reset_items();
    parse_html(g_html, length);
    layout();

    g_scroll = scroll;
    clamp_scroll();
}

static void update_title(void) {
    char title[128];

    title[0] = 0;

    /*
     * The system keeps 31 characters of a window title, so a long page
     * title is cut to leave room for the program's name.
     */
    if (g_title[0]) {
        char cut[24];

        str_copy(cut, g_title, sizeof(cut));

        if (strlen(g_title) >= sizeof(cut)) {
            cut[sizeof(cut) - 3] = '.';
            cut[sizeof(cut) - 2] = '.';
        }

        str_append(title, cut, sizeof(title));
        str_append(title, " - ", sizeof(title));
    }

    str_append(title, "Web", sizeof(title));

    win_set_title(g_window, title);
}

static void show_message(const char* heading, const char* detail) {
    reset_document();

    g_style = STYLE_BIG | STYLE_BOLD;
    g_indent = 0;
    g_link = 0;
    g_pending = 0;
    g_need_space = 0;
    g_line_empty = 1;

    for (const char* p = heading; *p; ) {
        u32 n = 0;

        while (p[n] && p[n] != ' ') {
            n++;
        }

        emit_word(p, n);
        g_need_space = 1;
        p += n;

        while (*p == ' ') {
            p++;
        }
    }

    block_break(2);
    g_style = 0;

    for (const char* p = detail; *p; ) {
        u32 n = 0;

        while (p[n] && p[n] != ' ') {
            n++;
        }

        emit_word(p, n);
        g_need_space = 1;
        p += n;

        while (*p == ' ') {
            p++;
        }
    }

    layout();
}

static const char g_home_page[] =
    "<title>Start</title>"
    "<h1>NAQ OS Browser</h1>"
    "<p>Type an address in the box above and press Enter, or try one of these:</p>"
    "<ul>"
    "<li><a href=\"https://example.com/\">example.com</a> - a tiny test page</li>"
    "<li><a href=\"http://info.cern.ch/hypertext/WWW/TheProject.html\">The first web page</a>"
    " - CERN, 1991 (plain HTTP)</li>"
    "<li><a href=\"https://en.wikipedia.org/wiki/Operating_system\">Wikipedia: Operating system</a></li>"
    "<li><a href=\"https://lite.duckduckgo.com/lite/\">DuckDuckGo Lite</a></li>"
    "<li><a href=\"https://text.npr.org/\">NPR text-only news</a></li>"
    "<li><a href=\"about:demo\">JavaScript demo</a> - a page built into the browser</li>"
    "</ul>"
    "<h3>What to expect</h3>"
    "<p>This browser shows the text, links and pictures of a page, follows a little CSS, "
    "and runs JavaScript. Pages are still laid out as plain flowing text, so heavy "
    "sites will not look or work as they do elsewhere. The JS button switches scripts "
    "off; Log shows what they reported.</p>"
    "<p><b>HTTPS here is encrypted but not verified.</b> The connection cannot be read "
    "or altered in transit, but the browser does not check that the server really is "
    "the site it claims to be. Do not type passwords into it.</p>";

static const char g_demo_page[] =
    "<title>JavaScript demo</title>"
    "<h1>JavaScript demo</h1>"
    "<p>This page is built into the browser. Everything that changes on it is done by "
    "scripts running in the QuickJS engine.</p>"
    "<h3>Counter</h3>"
    "<p><button id='minus'>-</button> <b id='count'>0</b> <button id='plus'>+</button> "
    "<button id='reset'>Reset</button></p>"
    "<h3>Clock</h3>"
    "<p>The time is <b id='clock'>...</b>, kept up to date by a timer. "
    "This page has been open for <span id='up'>0</span> seconds.</p>"
    "<h3>Typing</h3>"
    "<p>Click the box and type: <input id='name' size='18' placeholder='your name'></p>"
    "<p id='hello'>Hello, stranger.</p>"
    "<h3>List</h3>"
    "<p><input id='item' size='18' placeholder='something to do'> <button id='add'>Add</button> "
    "<button id='clear'>Clear</button></p>"
    "<ul id='list'></ul>"
    "<h3>Options</h3>"
    "<p><input type='checkbox' id='big'> large text, colour: <select id='col'>"
    "<option value=''>default<option value='#c00000'>red<option value='#008040'>green"
    "<option value='#2040c0'>blue</select> (click to change)</p>"
    "<p id='sample'>The quick brown fox jumps over the lazy dog.</p>"
    "<h3>Numbers</h3>"
    "<p id='math'></p>"
    "<p><button id='ask'>Ask a question</button> <span id='answer'></span></p>"
    "<script>"
    "var $ = function (id) { return document.getElementById(id); };"
    "var n = 0;"
    "function show() { $('count').textContent = n; }"
    "$('plus').onclick = function () { n++; show(); };"
    "$('minus').onclick = function () { n--; show(); };"
    "$('reset').onclick = function () { n = 0; show(); };"
    "var started = Date.now();"
    "function tick() {"
    "  $('clock').textContent = new Date().toTimeString().slice(0, 8);"
    "  $('up').textContent = Math.round((Date.now() - started) / 1000);"
    "}"
    "tick(); setInterval(tick, 1000);"
    "$('name').addEventListener('input', function (e) {"
    "  $('hello').textContent = 'Hello, ' + (e.target.value || 'stranger') + '.';"
    "});"
    "function add() {"
    "  var v = $('item').value.trim(); if (!v) return;"
    "  var li = document.createElement('li'); li.textContent = v + ' ';"
    "  var x = document.createElement('button'); x.textContent = 'remove';"
    "  x.onclick = function () { li.remove(); };"
    "  li.appendChild(x); $('list').appendChild(li); $('item').value = '';"
    "}"
    "$('add').onclick = add;"
    "$('item').addEventListener('keydown', function (e) { if (e.key === 'Enter') add(); });"
    "$('clear').onclick = function () { $('list').innerHTML = ''; };"
    "function restyle() {"
    "  var s = $('sample'); s.style.fontSize = $('big').checked ? '2em' : '';"
    "  s.style.color = $('col').value;"
    "}"
    "$('big').onchange = restyle; $('col').onchange = restyle;"
    "var primes = [];"
    "for (var i = 2; primes.length < 12; i++) {"
    "  if (primes.every(function (p) { return i % p; })) primes.push(i);"
    "}"
    "$('math').textContent = 'The first primes: ' + primes.join(', ') + '. 2 to the power 64 is ' +"
    "  (2n ** 64n) + '. The square root of 2 is ' + Math.SQRT2 + '.';"
    "$('ask').onclick = function () {"
    "  $('answer').textContent = confirm('Do you like NAQ OS?') ? 'Glad to hear it!' : 'Fair enough.';"
    "};"
    "</script>";

static void paint_now(void);
static void js_stop(void);
static void js_run_page(void);
static int js_follow(void);
static u32 js_log_page(char* out, u32 size);

/*
 * Shrinks the freshly decoded picture to fit the page and stores it in
 * the pool. Each output pixel is the average of the block of source
 * pixels it covers.
 */
static int keep_picture(picture_t* p, u32 width, u32 height) {
    u32 max_w = (u32)text_width();
    u32 out_w = width;
    u32 out_h = height;

    if (max_w < 64u) {
        max_w = 64u;
    }

    if (out_w > max_w) {
        out_h = (u32)(((u64)height * max_w) / width);
        out_w = max_w;
    }

    if (out_h > IMAGE_MAX_HEIGHT) {
        out_w = (u32)(((u64)out_w * IMAGE_MAX_HEIGHT) / out_h);
        out_h = IMAGE_MAX_HEIGHT;
    }

    if (out_w == 0 || out_h == 0 ||
        (u64)g_image_pool_used + (u64)out_w * out_h > IMAGE_POOL_PIXELS) {
        return 0;
    }

    u32* dst = g_image_pool + g_image_pool_used;

    for (u32 y = 0; y < out_h; y++) {
        u32 y0 = (u32)(((u64)y * height) / out_h);
        u32 y1 = (u32)(((u64)(y + 1u) * height) / out_h);

        if (y1 <= y0) {
            y1 = y0 + 1u;
        }

        for (u32 x = 0; x < out_w; x++) {
            u32 x0 = (u32)(((u64)x * width) / out_w);
            u32 x1 = (u32)(((u64)(x + 1u) * width) / out_w);

            if (x1 <= x0) {
                x1 = x0 + 1u;
            }

            u32 r = 0;
            u32 g = 0;
            u32 b = 0;
            u32 n = 0;

            for (u32 sy = y0; sy < y1 && sy < height; sy++) {
                const u32* row = g_image_decoded + (u64)sy * width;

                for (u32 sx = x0; sx < x1 && sx < width; sx++) {
                    u32 c = row[sx];

                    r += (c >> 16) & 0xFFu;
                    g += (c >> 8) & 0xFFu;
                    b += c & 0xFFu;
                    n++;
                }
            }

            if (n == 0) {
                n = 1;
            }

            dst[y * out_w + x] = ((r / n) << 16) | ((g / n) << 8) | (b / n);
        }
    }

    p->width = out_w;
    p->height = out_h;
    p->pixels = g_image_pool_used;
    p->state = IMAGE_LOADED;

    g_image_pool_used += out_w * out_h;

    return 1;
}

/*
 * Fetches a URL, following redirects. Leaves the body in g_page.
 */
static int fetch_file(const char* start, u8** out_body, u32* out_length) {
    char url[URL_MAX];

    str_copy(url, start, sizeof(url));

    for (int hop = 0; hop <= 3; hop++) {
        int status = 0;
        char* headers = 0;
        u32 headers_len = 0;

        if (!fetch_once(url, &status, out_body, out_length, &headers, &headers_len)) {
            return 0;
        }

        if (status >= 300 && status < 400) {
            char location[URL_MAX];
            char next[URL_MAX];

            if (!header_value(headers, headers_len, "Location", location, sizeof(location))) {
                return 0;
            }

            url_resolve(url, location, next, sizeof(next));

            if (!next[0]) {
                return 0;
            }

            str_copy(url, next, sizeof(url));
            continue;
        }

        return status == 200;
    }

    return 0;
}

/*
 * Finds the value of an attribute inside one tag of raw HTML text.
 */
static int raw_attribute(const char* html, u32 tag_start, u32 tag_end, const char* name,
                         char* out, u32 size) {
    return attribute(html + tag_start, tag_end - tag_start, name, out, size);
}

/*
 * Shows an HTML page: gathers its style sheets (those written in the
 * page and up to a few linked ones), then turns the HTML into items.
 */
static void load_html(const u8* body, u32 length) {
    char sheets[CSS_SHEETS_MAX][URL_MAX];
    u32 sheet_count = 0;

    if (length >= PAGE_MAX) {
        length = PAGE_MAX - 1u;
    }

    for (u32 i = 0; i < length; i++) {
        g_html[i] = (char)body[i];
    }

    g_html[length] = 0;
    g_html_len = length;

    css_reset();

    for (u32 i = 0; i + 7u < length; i++) {
        if (g_html[i] != '<') {
            continue;
        }

        if (starts_with(g_html + i, "<style")) {
            u32 start = i;

            while (start < length && g_html[start] != '>') {
                start++;
            }

            start++;

            u32 end = start;

            while (end + 7u < length && !starts_with(g_html + end, "</style")) {
                end++;
            }

            if (start < end) {
                css_parse(g_html + start, end - start);
            }

            i = end;
        } else if (starts_with(g_html + i, "<link") && sheet_count < CSS_SHEETS_MAX) {
            u32 end = i;
            char rel[32];
            char href[URL_MAX];

            while (end < length && g_html[end] != '>') {
                end++;
            }

            if (raw_attribute(g_html, i + 1u, end, "rel", rel, sizeof(rel)) &&
                starts_with(rel, "stylesheet") &&
                raw_attribute(g_html, i + 1u, end, "href", href, sizeof(href)) && href[0]) {
                url_resolve(g_url, href, sheets[sheet_count], URL_MAX);

                if (sheets[sheet_count][0]) {
                    sheet_count++;
                }
            }

            i = end;
        }
    }

    for (u32 n = 0; n < sheet_count; n++) {
        u8* sheet = 0;
        u32 sheet_len = 0;

        set_status("Loading styles...");
        paint_now();

        if (fetch_file(sheets[n], &sheet, &sheet_len)) {
            css_parse((const char*)sheet, sheet_len);
        }
    }

    parse_html(g_html, length);
}

/*
 * 1 if the user did something that should stop the pictures loading
 * (a key, a click, or closing the window).
 */
static int interrupted(void) {
    naq_win_event_t ev;
    int stop = 0;

    while (win_get_event(g_window, &ev) == 1) {
        if (ev.type == NAQ_WIN_EVENT_CLOSE) {
            g_quit = 1;
            stop = 1;
        } else if (ev.type == NAQ_WIN_EVENT_KEY) {
            stop = 1;
        } else if (ev.type == NAQ_WIN_EVENT_MOUSE && ev.w == NAQ_MOUSE_PRESS) {
            stop = 1;
        } else if (ev.type == NAQ_WIN_EVENT_MOUSE && ev.w == NAQ_MOUSE_WHEEL) {
            g_scroll += ev.h * 54;
            clamp_scroll();
        }
    }

    return stop;
}

/*
 * Fetches and decodes the page's pictures one at a time. The text is
 * already on screen; each picture appears as it arrives.
 */
static int pictures_pending(void) {
    for (u32 i = 0; i < g_picture_count; i++) {
        if (g_pictures[i].state == IMAGE_PENDING) {
            return 1;
        }
    }

    return 0;
}

static void load_pictures(void) {
    char final_status[160];

    if (!pictures_pending()) {
        return;
    }

    str_copy(final_status, g_status, sizeof(final_status));

    u32 shown = 0;
    u32 not_fetched = 0;
    u32 not_decoded = 0;

    for (u32 i = 0; i < g_picture_count; i++) {
        picture_t* p = &g_pictures[i];
        u8* body = 0;
        u32 length = 0;
        u32 width = 0;
        u32 height = 0;

        if (p->state != IMAGE_PENDING) {
            continue;
        }

        g_status[0] = 0;
        str_append(g_status, "Loading picture ", sizeof(g_status));

        char number[12];
        u32 n = 0;
        u32 value = i + 1u;
        char digits[8];
        u32 d = 0;

        do {
            digits[d++] = (char)('0' + value % 10u);
            value /= 10u;
        } while (value);

        while (d > 0) {
            number[n++] = digits[--d];
        }

        number[n] = 0;
        str_append(g_status, number, sizeof(g_status));
        str_append(g_status, " (any key stops)", sizeof(g_status));

        paint_now();

        if (interrupted()) {
            /* the rest are given up, not tried again later */
            for (u32 k = i; k < g_picture_count; k++) {
                if (g_pictures[k].state == IMAGE_PENDING) {
                    g_pictures[k].state = IMAGE_FAILED;
                }
            }

            break;
        }

        p->state = IMAGE_FAILED;

        if (!fetch_file(g_picture_urls[i], &body, &length)) {
            not_fetched++;
            continue;
        }

        if (image_decode(body, length, g_image_decoded, IMAGE_DECODE_PIXELS,
                         g_image_scratch, IMAGE_SCRATCH_BYTES, &width, &height) != IMAGE_OK) {
            not_decoded++;
            continue;
        }

        /*
         * One-pixel "tracking" pictures and tiny spacers are not worth
         * showing.
         */
        if (width < 8u || height < 8u) {
            continue;
        }

        if (keep_picture(p, width, height)) {
            shown++;
            layout();
        }
    }

    /*
     * Say how the pictures fared, after the security note.
     */
    str_copy(g_status, final_status, sizeof(g_status));

    if (not_fetched || not_decoded) {
        char note[80];
        u32 counts[3];
        const char* labels[3] = { " Pictures: ", " shown, ", " not loaded, " };

        counts[0] = shown;
        counts[1] = not_fetched;
        counts[2] = not_decoded;

        note[0] = 0;

        for (int k = 0; k < 3; k++) {
            char digits[8];
            char text[8];
            u32 d = 0;
            u32 t = 0;
            u32 value = counts[k];

            do {
                digits[d++] = (char)('0' + value % 10u);
                value /= 10u;
            } while (value);

            while (d > 0) {
                text[t++] = digits[--d];
            }

            text[t] = 0;

            str_append(note, labels[k], sizeof(note));
            str_append(note, text, sizeof(note));
        }

        str_append(note, " in an unsupported format.", sizeof(note));
        str_append(g_status, note, sizeof(g_status));
    }
}

/*
 * Shows one of the pages built into the browser.
 */
static void load_builtin(const char* url, const char* html, u32 length) {
    js_stop();
    reset_document();
    str_copy(g_url, url, sizeof(g_url));
    g_secure = 0;

    if (length >= PAGE_MAX) {
        length = PAGE_MAX - 1u;
    }

    if (html != g_html) {
        memcpy(g_html, html, length);
    }

    g_html[length] = 0;
    g_html_len = length;

    parse_html(g_html, length);
    layout();
    update_title();
    set_status("Ready.");
}

static void load_home(void) {
    load_builtin("about:home", g_home_page, sizeof(g_home_page) - 1u);
}

/*
 * Pages that scripts send the browser to straight away are followed,
 * but not forever.
 */
static int g_script_hops = 0;

/*
 * Fetches and shows a URL. Returns 1 if a page (of any kind) is showing.
 */
static int navigate(const char* target, int remember) {
    char url[URL_MAX];
    char previous[URL_MAX];

    str_copy(previous, g_url, sizeof(previous));

    if (starts_with(target, "about:")) {
        if (remember && previous[0] && !str_equal(previous, target) &&
            g_history_count < HISTORY_MAX) {
            str_copy(g_history[g_history_count++], previous, URL_MAX);
        }

        if (str_equal(target, "about:demo")) {
            load_builtin("about:demo", g_demo_page, sizeof(g_demo_page) - 1u);
            paint_now();
            js_run_page();
        } else if (str_equal(target, "about:console")) {
            js_stop();
            load_builtin("about:console", g_html, js_log_page(g_html, PAGE_MAX));
        } else {
            load_home();
        }

        return 1;
    }

    js_stop();

    /*
     * No scheme typed: assume HTTPS.
     */
    url[0] = 0;

    if (!starts_with(target, "http://") && !starts_with(target, "https://")) {
        str_append(url, "https://", sizeof(url));
    }

    str_append(url, target, sizeof(url));

    for (int hop = 0; hop <= MAX_REDIRECTS; hop++) {
        int status = 0;
        u8* body = 0;
        u32 length = 0;
        char* headers = 0;
        u32 headers_len = 0;

        g_status[0] = 0;
        str_append(g_status, "Loading ", sizeof(g_status));
        str_append(g_status, url, sizeof(g_status));
        paint_now();

        if (!fetch_once(url, &status, &body, &length, &headers, &headers_len)) {
            char detail[200];

            str_copy(detail, g_status, sizeof(detail));
            str_copy(g_url, url, sizeof(g_url));
            g_secure = 0;
            show_message("This page could not be loaded", detail);
            g_title[0] = 0;
            update_title();
            return 0;
        }

        if (status >= 300 && status < 400) {
            char location[URL_MAX];
            char next[URL_MAX];

            if (header_value(headers, headers_len, "Location", location, sizeof(location))) {
                url_resolve(url, location, next, sizeof(next));

                if (next[0]) {
                    str_copy(url, next, sizeof(url));
                    continue;
                }
            }
        }

        /*
         * This is the page.
         */
        char type[64];

        if (remember && previous[0] && g_history_count < HISTORY_MAX) {
            str_copy(g_history[g_history_count++], previous, URL_MAX);
        } else if (remember && previous[0]) {
            for (int i = 1; i < HISTORY_MAX; i++) {
                str_copy(g_history[i - 1], g_history[i], URL_MAX);
            }

            str_copy(g_history[HISTORY_MAX - 1], previous, URL_MAX);
        }

        str_copy(g_url, url, sizeof(g_url));
        g_secure = starts_with(url, "https://");

        reset_document();

        int is_html = 0;

        type[0] = 0;
        header_value(headers, headers_len, "Content-Type", type, sizeof(type));

        if (type[0] == 0 || starts_with(type, "text/html") ||
            starts_with(type, "application/xhtml")) {
            is_html = 1;
            load_html(body, length);
        } else if (starts_with(type, "text/") || starts_with(type, "application/json")) {
            parse_plain((const char*)body, length);
        } else {
            char detail[160];

            detail[0] = 0;
            str_append(detail, "It is a file of type ", sizeof(detail));
            str_append(detail, type, sizeof(detail));
            str_append(detail, ", which this browser cannot show.", sizeof(detail));
            show_message("Not a web page", detail);
            update_title();
            set_status("Done.");
            return 1;
        }

        layout();
        update_title();

        g_status[0] = 0;

        if (status != 200) {
            str_append(g_status, "The server answered with an error code. ", sizeof(g_status));
        }

        str_append(g_status,
                   g_secure ? "HTTPS: encrypted, site identity NOT verified."
                            : "HTTP: not encrypted.",
                   sizeof(g_status));

        /*
         * The page as sent is on screen; now its scripts get to run,
         * and may change it or send the browser somewhere else.
         */
        if (is_html) {
            paint_now();
            js_run_page();

            if (g_script_hops < 4) {
                g_script_hops++;

                int moved = js_follow();

                g_script_hops--;

                if (moved) {
                    return 1;
                }
            }
        }

        if (g_item_count == 0) {
            char keep[160];

            str_copy(keep, g_status, sizeof(keep));
            show_message("Nothing to show",
                         "The page has no text this browser can display.");
            str_copy(g_status, keep, sizeof(g_status));
        }

        load_pictures();

        return 1;
    }

    show_message("This page could not be loaded", "It redirected too many times.");
    return 0;
}

static void go_back(void) {
    if (g_history_count == 0) {
        return;
    }

    char target[URL_MAX];

    str_copy(target, g_history[--g_history_count], sizeof(target));
    navigate(target, 0);
}

/* -------------------------------------------------------------------------- */
/* Drawing                                                                     */
/* -------------------------------------------------------------------------- */

extern const unsigned char* font8x8_get(char c);

static u32 mix(u32 a, u32 b, u32 t, u32 max_t) {
    if (!max_t) {
        return a;
    }

    u32 r = (((a >> 16) & 0xFFu) * (max_t - t) + ((b >> 16) & 0xFFu) * t) / max_t;
    u32 g = (((a >> 8) & 0xFFu) * (max_t - t) + ((b >> 8) & 0xFFu) * t) / max_t;
    u32 bl = ((a & 0xFFu) * (max_t - t) + (b & 0xFFu) * t) / max_t;

    return (r << 16) | (g << 8) | bl;
}

/*
 * Clip rectangle for page content, so text never draws over the bars.
 */
static i32 g_clip_top = 0;
static i32 g_clip_bottom = 0;
static i32 g_clip_right = 0;

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
 * Page text: clipped to the page area, optionally double size and bold
 * (drawn twice, one pixel apart).
 */
static void page_text(i32 x, i32 y, const char* s, u32 length, u32 color, i32 scale, int bold) {
    for (u32 i = 0; i < length; i++) {
        const unsigned char* glyph = font8x8_get(s[i]);
        i32 cx = x + (i32)i * GFX_FONT_W * scale;

        if (!glyph || cx >= g_clip_right || cx + GFX_FONT_W * scale <= 0) {
            continue;
        }

        for (i32 gy = 0; gy < 8; gy++) {
            unsigned char bits = glyph[gy];

            if (!bits) {
                continue;
            }

            for (i32 sy = 0; sy < 2 * scale; sy++) {
                i32 py = y + gy * 2 * scale + sy;

                if (py < g_clip_top || py >= g_clip_bottom) {
                    continue;
                }

                u32* row = g_pixels + (u32)py * g_w;

                for (i32 gx = 0; gx < 8; gx++) {
                    if (!(bits & (0x80u >> gx))) {
                        continue;
                    }

                    for (i32 sx = 0; sx < scale + (bold ? 1 : 0); sx++) {
                        i32 px = cx + gx * scale + sx;

                        if (px >= 0 && px < g_clip_right) {
                            row[px] = color;
                        }
                    }
                }
            }
        }
    }
}

static void button(i32 x, i32 y, i32 w, i32 h, const char* label, int enabled) {
    gradient(x, y, w, h, 0x00FFFFFFu, 0x008CD4F0u);
    rect(x + 1, y + 1, w - 2, h / 2 - 1, 0x00F4FDFFu);
    frame(x, y, w, h, COL_BAR_EDGE);

    i32 len = (i32)strlen(label);

    text(x + (w - len * GFX_FONT_W) / 2, y + (h - GFX_FONT_H) / 2, label,
         enabled ? 0x0016445Eu : 0x0090B0C0u);
}

static void scrollbar_geometry(i32* track_y, i32* track_h, i32* thumb_y, i32* thumb_h) {
    i32 view = view_height();

    *track_y = TOOLBAR_H;
    *track_h = view;

    if (g_doc_height <= view || view <= 0) {
        *thumb_y = *track_y;
        *thumb_h = *track_h;
        return;
    }

    *thumb_h = (i32)(((i64)view * view) / g_doc_height);

    if (*thumb_h < 24) {
        *thumb_h = 24;
    }

    *thumb_y = *track_y +
        (i32)(((i64)(*track_h - *thumb_h) * g_scroll) / (g_doc_height - view));
}

static void paint(void) {
    i32 view = view_height();
    i32 area_w = (i32)g_w - SCROLL_W;

    /*
     * Page.
     */
    rect(0, TOOLBAR_H, area_w, view, COL_PAPER);

    g_clip_top = TOOLBAR_H;
    g_clip_bottom = TOOLBAR_H + view;
    g_clip_right = area_w;

    for (u32 i = 0; i < g_box_count; i++) {
        const box_t* b = &g_boxes[i];
        i32 y = TOOLBAR_H + b->y - g_scroll;

        if (y > g_clip_bottom) {
            break;
        }

        i32 box_h = 40;

        if (b->type == ITEM_IMAGE) {
            box_h = (i32)g_pictures[b->image - 1u].height;
        }

        if (y + box_h < TOOLBAR_H) {
            continue;
        }

        i32 x = MARGIN + b->x;

        if (b->type == ITEM_IMAGE) {
            const picture_t* p = &g_pictures[b->image - 1u];
            const u32* src = g_image_pool + p->pixels;

            for (u32 row = 0; row < p->height; row++) {
                i32 py = y + (i32)row;

                if (py < g_clip_top || py >= g_clip_bottom) {
                    continue;
                }

                u32* dst = g_pixels + (u32)py * g_w;

                for (u32 col = 0; col < p->width; col++) {
                    i32 px = x + (i32)col;

                    if (px >= 0 && px < g_clip_right) {
                        dst[px] = src[row * p->width + col];
                    }
                }
            }

            if (b->link) {
                frame(x - 1, y < g_clip_top ? g_clip_top : y, 1, 1, COL_LINK);
            }

            continue;
        }

        if (b->type == ITEM_RULE) {
            if (y >= g_clip_top && y < g_clip_bottom) {
                rect(x, y, text_width() - b->x, 1, COL_RULE);
            }

            continue;
        }

        if (b->type == ITEM_BULLET) {
            if (y + 6 >= g_clip_top && y + 11 < g_clip_bottom) {
                rect(x + 2, y + 6, 5, 5, COL_TEXT_DIM);
            }

            continue;
        }

        i32 scale = (b->style & STYLE_BIG) ? 2 : 1;
        u32 color = COL_TEXT;

        if (b->style & STYLE_LINK) {
            color = COL_LINK;
        } else if (b->color) {
            color = b->color;
        } else if (b->style & STYLE_DIM) {
            color = COL_TEXT_DIM;
        } else if (b->style & STYLE_BIG) {
            color = COL_HEADING;
        }

        if (b->style & (STYLE_FIELD | STYLE_BUTTON)) {
            /*
             * Form controls sit on a small panel. A button made of
             * several words gets one strip running under all of them.
             */
            i32 cw = (i32)b->length * GFX_FONT_W * scale;
            i32 ch = 16 * scale + 2;
            int field = (b->style & STYLE_FIELD) != 0;
            int joined = !field && i + 1u < g_box_count &&
                         (g_boxes[i + 1u].style & STYLE_BUTTON) &&
                         g_boxes[i + 1u].node == b->node && g_boxes[i + 1u].y == b->y;
            i32 right = x + cw + (joined ? GFX_FONT_W * scale : 0);

            if (right > g_clip_right) {
                right = g_clip_right;
            }

            if (y - 1 >= g_clip_top && y - 1 + ch < g_clip_bottom && right > x) {
                if (field) {
                    rect(x, y - 1, right - x, ch, 0x00FFFFFFu);
                    frame(x, y - 1, right - x, ch, 0x0078A8C0u);
                } else {
                    int first = !(i > 0 && (g_boxes[i - 1u].style & STYLE_BUTTON) &&
                                  g_boxes[i - 1u].node == b->node && g_boxes[i - 1u].y == b->y);

                    /* a little room at each end of the strip */
                    i32 left = first ? x - 3 : x;

                    if (!joined && right + 3 <= g_clip_right) {
                        right += 3;
                    }

                    if (left < 0) {
                        left = 0;
                    }

                    gradient(left, y - 1, right - left, ch, 0x00FFFFFFu, 0x00A8DCF4u);
                    rect(left, y - 1, right - left, 1, COL_BAR_EDGE);
                    rect(left, y - 2 + ch, right - left, 1, COL_BAR_EDGE);

                    if (first) {
                        rect(left, y - 1, 1, ch, COL_BAR_EDGE);
                    }

                    if (!joined) {
                        rect(right - 1, y - 1, 1, ch, COL_BAR_EDGE);
                    }
                }
            }

            if (!b->color) {
                color = 0x0016445Eu;
            }
        }

        page_text(x, y, g_text + b->text, b->length, color, scale,
                  (b->style & STYLE_BOLD) != 0);

        if (b->link) {
            i32 uy = y + 16 * scale - 1;

            if (uy >= g_clip_top && uy < g_clip_bottom) {
                i32 uw = (i32)b->length * GFX_FONT_W * scale;

                if (x + uw > g_clip_right) {
                    uw = g_clip_right - x;
                }

                if (uw > 0) {
                    rect(x, uy, uw, 1,
                         b->link == (u16)g_hover_link ? 0x00E06000u : COL_LINK);
                }
            }
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
    gradient(area_w + 2, thumb_y + 1, SCROLL_W - 4, thumb_h - 2, 0x00B8E6FAu, 0x004AA8DCu);
    frame(area_w + 2, thumb_y + 1, SCROLL_W - 4, thumb_h - 2, COL_BAR_EDGE);

    /*
     * Toolbar: Back, Home, address box.
     */
    gradient(0, 0, (i32)g_w, TOOLBAR_H, COL_BAR_TOP, COL_BAR_BOT);
    rect(0, 1, (i32)g_w, TOOLBAR_H / 2 - 2, 0x00F0FCFFu);
    rect(0, TOOLBAR_H - 1, (i32)g_w, 1, COL_BAR_EDGE);

    button(8, 6, 52, 24, "Back", g_history_count > 0);
    button(64, 6, 52, 24, "Home", 1);

    /*
     * Right end: scripts on/off, and what they logged.
     */
    button((i32)g_w - 116, 6, 52, 24, g_js_enabled ? "JS on" : "JS off", 1);
    button((i32)g_w - 60, 6, 52, 24, "Log", 1);

    i32 box_x = 124;
    i32 box_w = (i32)g_w - box_x - 8 - 116;

    if (box_w > 32) {
        const char* shown = g_editing ? g_edit : g_url;
        u32 len = (u32)strlen(shown);
        u32 fit = (u32)((box_w - 16) / GFX_FONT_W);
        u32 skip = (g_editing && len > fit) ? len - fit : 0;

        rect(box_x, 6, box_w, 24, COL_PAPER);
        frame(box_x, 6, box_w, 24, g_editing ? 0x00FF9A1Eu : COL_BAR_EDGE);
        text_n(box_x + 8, 10, shown + skip, fit, 0x0016445Eu);

        if (g_editing) {
            rect(box_x + 8 + (i32)(len - skip) * GFX_FONT_W, 9, 2, 18, 0x0016445Eu);
        }
    }

    /*
     * Status bar: a link's target while the pointer is over it.
     */
    i32 sy = (i32)g_h - STATUS_H;
    const char* status = g_status;

    if (g_hover_link > 0 && g_hover_link <= (int)g_link_count) {
        status = g_link_text + g_links[g_hover_link - 1];
    }

    gradient(0, sy, (i32)g_w, STATUS_H, 0x00EAF8FEu, 0x00B8E2F4u);
    rect(0, sy, (i32)g_w, 1, COL_BAR_EDGE);
    text_n(8, sy + 3, status, (u32)(((i32)g_w - 16) / GFX_FONT_W), 0x0016445Eu);
}

static void paint_now(void) {
    paint();
    win_present(g_window, g_pixels, g_w, g_h);
}

/* -------------------------------------------------------------------------- */
/* JavaScript                                                                  */
/* -------------------------------------------------------------------------- */

#include "browser/script.c"

/* -------------------------------------------------------------------------- */
/* Input                                                                       */
/* -------------------------------------------------------------------------- */

/*
 * The word or picture under a point of the window: its index in g_boxes,
 * or -1.
 */
static int box_at(i32 x, i32 y) {
    if (y < TOOLBAR_H || y >= (i32)g_h - STATUS_H || x >= (i32)g_w - SCROLL_W) {
        return -1;
    }

    i32 doc_x = x - MARGIN;
    i32 doc_y = y - TOOLBAR_H + g_scroll;

    for (u32 i = 0; i < g_box_count; i++) {
        const box_t* b = &g_boxes[i];

        if (b->y > doc_y) {
            break;
        }

        if (b->type != ITEM_WORD && b->type != ITEM_IMAGE) {
            continue;
        }

        i32 scale = (b->style & STYLE_BIG) ? 2 : 1;
        i32 w = (i32)b->length * GFX_FONT_W * scale + GFX_FONT_W;
        i32 h = 16 * scale + 2;

        if (b->type == ITEM_IMAGE) {
            w = (i32)g_pictures[b->image - 1u].width;
            h = (i32)g_pictures[b->image - 1u].height;
        }

        if (doc_x >= b->x && doc_x < b->x + w && doc_y >= b->y && doc_y < b->y + h) {
            return (int)i;
        }
    }

    return -1;
}

/*
 * The link under a point of the window, or 0.
 */
static int link_at(i32 x, i32 y) {
    if (y < TOOLBAR_H || y >= (i32)g_h - STATUS_H || x >= (i32)g_w - SCROLL_W) {
        return 0;
    }

    i32 doc_x = x - MARGIN;
    i32 doc_y = y - TOOLBAR_H + g_scroll;

    for (u32 i = 0; i < g_box_count; i++) {
        const box_t* b = &g_boxes[i];

        if (b->y > doc_y) {
            break;
        }

        if (!b->link || (b->type != ITEM_WORD && b->type != ITEM_IMAGE)) {
            continue;
        }

        i32 scale = (b->style & STYLE_BIG) ? 2 : 1;
        i32 w = (i32)b->length * GFX_FONT_W * scale;
        i32 h = 16 * scale + 2;

        if (b->type == ITEM_IMAGE) {
            w = (i32)g_pictures[b->image - 1u].width;
            h = (i32)g_pictures[b->image - 1u].height;
        }

        /*
         * A little slack to the right, so the gap between two words of
         * one link is part of it.
         */
        if (doc_x >= b->x && doc_x < b->x + w + GFX_FONT_W &&
            doc_y >= b->y && doc_y < b->y + h) {
            return b->link;
        }
    }

    return 0;
}

static void scroll_to_drag(i32 y) {
    i32 track_y;
    i32 track_h;
    i32 thumb_y;
    i32 thumb_h;
    i32 view = view_height();

    scrollbar_geometry(&track_y, &track_h, &thumb_y, &thumb_h);

    if (g_doc_height <= view || track_h <= thumb_h) {
        g_scroll = 0;
        return;
    }

    i32 at = y - track_y - thumb_h / 2;

    if (at < 0) {
        at = 0;
    }

    if (at > track_h - thumb_h) {
        at = track_h - thumb_h;
    }

    g_scroll = (i32)(((i64)at * (g_doc_height - view)) / (track_h - thumb_h));
}

static void start_editing(void) {
    g_editing = 1;

    if (str_equal(g_url, "about:home")) {
        g_edit[0] = 0;
    } else {
        str_copy(g_edit, g_url, sizeof(g_edit));
    }

    g_edit_len = (u32)strlen(g_edit);
}

static void handle_press(i32 x, i32 y) {
    if (y < TOOLBAR_H) {
        if (x >= 8 && x < 60 && y >= 6 && y < 30) {
            g_editing = 0;
            go_back();
        } else if (x >= 64 && x < 116 && y >= 6 && y < 30) {
            g_editing = 0;
            navigate("about:home", 1);
        } else if (x >= (i32)g_w - 116 && x < (i32)g_w - 64 && y >= 6 && y < 30) {
            /*
             * Scripts on or off: the page is fetched again either way.
             */
            g_editing = 0;
            g_js_enabled = !g_js_enabled;

            if (starts_with(g_url, "http") || str_equal(g_url, "about:demo")) {
                char again[URL_MAX];

                str_copy(again, g_url, sizeof(again));
                navigate(again, 0);
            } else {
                js_stop();
            }

            if (!g_js_enabled) {
                set_status("JavaScript is off.");
            }
        } else if (x >= (i32)g_w - 60 && x < (i32)g_w - 8 && y >= 6 && y < 30) {
            g_editing = 0;
            navigate("about:console", 1);
        } else if (x >= 124 && x < (i32)g_w - 124) {
            start_editing();
        }

        return;
    }

    g_editing = 0;

    if (y >= (i32)g_h - STATUS_H) {
        return;
    }

    if (x >= (i32)g_w - SCROLL_W) {
        g_dragging = 1;
        scroll_to_drag(y);
        return;
    }

    int link = link_at(x, y);

    /*
     * The page's scripts see the click first, and may keep it.
     */
    if (g_js_active) {
        int hit = box_at(x, y);
        int kept = js_click(hit >= 0 ? g_boxes[hit].node : 0u);

        if (js_follow() || kept) {
            return;
        }
    }

    if (link > 0 && link <= (int)g_link_count) {
        char target[URL_MAX];

        str_copy(target, g_link_text + g_links[link - 1], sizeof(target));
        navigate(target, 1);
    }
}

static void handle_key(u32 key) {
    if (g_editing) {
        if (key == NAQ_KEY_ESCAPE) {
            g_editing = 0;
        } else if (key == NAQ_KEY_ENTER) {
            g_editing = 0;

            if (g_edit_len) {
                char target[URL_MAX];

                str_copy(target, g_edit, sizeof(target));
                navigate(target, 1);
            }
        } else if (key == NAQ_KEY_BACKSPACE) {
            if (g_edit_len > 0) {
                g_edit[--g_edit_len] = 0;
            }
        } else if (key == 21u) {            /* Ctrl+U: clear */
            g_edit_len = 0;
            g_edit[0] = 0;
        } else if (key > 32 && key < 127 && g_edit_len + 1 < URL_MAX) {
            g_edit[g_edit_len++] = (char)key;
            g_edit[g_edit_len] = 0;
        }

        return;
    }

    /*
     * Keys go to the page's scripts first: to the text box being typed
     * in, or to a page that handles keys itself.
     */
    if (g_js_active) {
        int used = js_key(key);

        if (js_follow() || used) {
            return;
        }
    }

    i32 page = view_height() - 32;

    switch (key) {
        case 12u:                           /* Ctrl+L */
            start_editing();
            g_edit_len = 0;
            g_edit[0] = 0;
            break;

        case NAQ_KEY_UP:
            g_scroll -= 36;
            break;

        case NAQ_KEY_DOWN:
            g_scroll += 36;
            break;

        case NAQ_KEY_PAGE_UP:
            g_scroll -= page;
            break;

        case NAQ_KEY_PAGE_DOWN:
        case ' ':
            g_scroll += page;
            break;

        case NAQ_KEY_HOME:
            g_scroll = 0;
            break;

        case NAQ_KEY_END:
            g_scroll = g_doc_height;
            break;

        case NAQ_KEY_BACKSPACE:
            go_back();
            break;

        default:
            break;
    }

    clamp_scroll();
}

static void set_size(u32 window_w, u32 window_h) {
    g_window_w = window_w ? window_w : 1;
    g_window_h = window_h ? window_h : 1;

    g_w = g_window_w > MAX_W ? MAX_W : g_window_w;
    g_h = g_window_h > MAX_H ? MAX_H : g_window_h;

    if (g_w < 320) {
        g_w = 320;
    }

    if (g_h < 160) {
        g_h = 160;
    }

    layout();
}

int main(void) {
    char arg[URL_MAX];

    g_window = win_create(150, 60, START_W, START_H, "Browser");

    if (g_window < 0) {
        puts("browser: win_create failed\n");
        return 1;
    }

    load_home();

    /*
     * Started with an address (for example from another program).
     */
    if (get_arg(arg, sizeof(arg)) > 0) {
        navigate(arg, 0);
    }

    int running = 1;
    int dirty = 1;

    while (running && !g_quit) {
        naq_win_event_t ev;

        while (win_get_event(g_window, &ev) == 1) {
            i32 x = (i32)(((i64)ev.x * (i64)g_w) / (i64)g_window_w);
            i32 y = (i32)(((i64)ev.y * (i64)g_h) / (i64)g_window_h);

            if (ev.type == NAQ_WIN_EVENT_CLOSE) {
                running = 0;
            } else if (ev.type == NAQ_WIN_EVENT_KEY) {
                handle_key(ev.key);
                dirty = 1;
            } else if (ev.type == NAQ_WIN_EVENT_RESIZE) {
                set_size((u32)ev.w, (u32)ev.h);
                dirty = 1;
            } else if (ev.type == NAQ_WIN_EVENT_MOUSE) {
                if (ev.w == NAQ_MOUSE_PRESS && ev.h == (i32)NAQ_MOUSE_BUTTON_LEFT) {
                    handle_press(x, y);
                    dirty = 1;
                } else if (ev.w == NAQ_MOUSE_RELEASE) {
                    g_dragging = 0;
                } else if (ev.w == NAQ_MOUSE_WHEEL) {
                    g_scroll += ev.h * 54;
                    clamp_scroll();
                    dirty = 1;
                } else if (ev.w == NAQ_MOUSE_MOVE) {
                    if (g_dragging && (ev.key & NAQ_MOUSE_BUTTON_LEFT)) {
                        scroll_to_drag(y);
                        dirty = 1;
                    } else {
                        int hover = link_at(x, y);

                        g_dragging = 0;

                        if (hover != g_hover_link) {
                            g_hover_link = hover;
                            dirty = 1;
                        }
                    }
                }
            } else if (ev.type == NAQ_WIN_EVENT_FOCUS) {
                dirty = 1;
            }
        }

        /*
         * Timers of the page's scripts, and anything they changed.
         */
        if (g_js_active && js_pump()) {
            dirty = 1;
        }

        if (dirty) {
            paint_now();
            dirty = 0;
        }

        sleep_ms(16);
    }

    win_destroy(g_window);

    return 0;
}
