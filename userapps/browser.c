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
 * Pages are laid out with a real box model (browser/engine.c and
 * browser/style.c): margins, padding, borders, backgrounds, widths,
 * several text sizes in a proportional font, lists and tables. Floats,
 * flexbox, grid and positioned elements are not done properly yet.
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

/*
 * Sites whose identity could not be confirmed are not opened, unless the
 * user says to go on; that choice lasts until the browser is closed.
 */
#define TRUST_MAX 8

static char g_trusted_hosts[TRUST_MAX][128];
static int g_trusted_count = 0;
static int g_cert_refused = 0;      /* the last connection was dropped for this reason */
static char g_cert_reason[96] = "";
static char g_cert_host[128] = "";
static char g_blocked_url[URL_MAX] = "";
static int g_verified = 0;          /* the page on screen came from a verified server */

static int host_excepted(const char* host) {
    for (int i = 0; i < g_trusted_count; i++) {
        if (str_equal(g_trusted_hosts[i], host)) {
            return 1;
        }
    }

    return 0;
}

static int conn_open(const url_parts_t* u) {
    g_using_tls = u->secure;

    if (u->secure) {
        if (!tls_connect(&g_tls, u->host, u->port, FETCH_TIMEOUT_MS)) {
            set_status(tls_error(&g_tls));
            return 0;
        }

        if (!g_tls.verified && !host_excepted(u->host)) {
            g_cert_refused = 1;
            str_copy(g_cert_reason, g_tls.verify_error, sizeof(g_cert_reason));
            str_copy(g_cert_host, u->host, sizeof(g_cert_host));
            tls_close(&g_tls);
            set_status("The site's identity could not be confirmed.");
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
/* Reading tags and text                                                       */
/* -------------------------------------------------------------------------- */

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

/* -------------------------------------------------------------------------- */
/* The page engine: tree, styles, layout, painting                             */
/* -------------------------------------------------------------------------- */

#define CSS_SHEETS_MAX 8

#include "browser/engine.c"

/* -------------------------------------------------------------------------- */
/* Loading a page                                                              */
/* -------------------------------------------------------------------------- */

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
    "<p>This browser lays pages out with their own styles (boxes, colours, text sizes, "
    "tables) and runs JavaScript. Side-by-side layouts (floats, flexbox, grid) are only "
    "approximated, so heavy sites will not look as they do elsewhere. The JS button "
    "switches scripts off; Log shows what they reported.</p>"
    "<p><b>HTTPS sites are checked.</b> The browser confirms that a secure site is who it "
    "says it is (its certificate must lead to a trusted authority, name the site and be in "
    "date) and stops with a warning if not. It does not yet check whether a certificate was "
    "withdrawn early.</p>";

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

    /*
     * "Open the page anyway" on the warning page: trust that host until
     * the browser closes, and try again.
     */
    if (str_equal(target, "about:unsafe") && g_blocked_url[0]) {
        char again[URL_MAX];

        if (g_trusted_count < TRUST_MAX) {
            str_copy(g_trusted_hosts[g_trusted_count++], g_cert_host, sizeof(g_trusted_hosts[0]));
        }

        str_copy(again, g_blocked_url, sizeof(again));
        g_blocked_url[0] = 0;

        return navigate(again, 0);
    }

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

        g_cert_refused = 0;

        if (!fetch_once(url, &status, &body, &length, &headers, &headers_len)) {
            if (g_cert_refused) {
                /* a warning page, with a way to go on for those who must */
                static char page[900];

                page[0] = 0;
                str_append(page, "<title>Not verified</title><h2>This site's identity could not be confirmed</h2><p>The connection to <b>", sizeof(page));
                str_append(page, g_cert_host, sizeof(page));
                str_append(page, "</b> was stopped because: <b>", sizeof(page));
                str_append(page, g_cert_reason, sizeof(page));
                str_append(page, ".</b></p><p>Someone may be pretending to be this site, or the site may be set up "
                                 "wrongly. Nothing was sent to it. If this PC's clock is wrong, set it and try again.</p>"
                                 "<p><a href=\"about:unsafe\">Open the page anyway</a> (not safe: do not type "
                                 "passwords or anything private).</p>", sizeof(page));

                str_copy(g_blocked_url, url, sizeof(g_blocked_url));
                load_builtin(url, page, (u32)strlen(page));
                g_verified = 0;
                set_status("Stopped: the site's identity could not be confirmed.");
                return 0;
            }

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
        g_verified = g_secure && g_tls.verified;

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
                   g_secure ? (g_verified ? "HTTPS: encrypted, site identity verified."
                                          : "HTTPS: encrypted, but identity NOT verified (you chose to continue).")
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

        if (!engine_has_content()) {
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

    engine_paint();

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
        int kept = js_click(node_at(x, y));

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
