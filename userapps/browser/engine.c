/*
 * NAQ OS Browser: the page engine.
 *
 * Part of BROWSER.ELF; browser.c includes this file.
 *
 * Turning a page into pixels takes four steps:
 *
 *   1. Tree     the HTML text becomes a tree of elements and pieces of
 *               text (build_tree).
 *   2. Style    every element gets its final properties: size of text,
 *               colours, margins and so on (style.c).
 *   3. Layout   every element becomes a box with a position and a size.
 *               Blocks stack downwards; text and other inline things flow
 *               along lines and wrap; tables become grids. The result is
 *               a flat "display list" of rectangles, runs of text and
 *               pictures (layout).
 *   4. Paint    the part of the display list that is in view is drawn
 *               (engine_paint).
 *
 * Floats work (text runs round them) and so do flex rows (grow, shrink,
 * justify, align, wrap, gap), flex columns, grids with a column template,
 * position: absolute / relative, ::before / ::after text and rounded
 * corners. Not done yet: grid rows and named areas, fixed elements that
 * stay put while scrolling, background pictures.
 */

#include "browser_font.h"

#define NODES_MAX     60000u
#define TPOOL_MAX     (1536u * 1024u)
#define CLASSES_MAX   200000u
#define OPS_MAX       160000u
#define DEPTH_MAX     200u
#define LINE_TOKENS   320u

#define N_ELEMENT 1u
#define N_TEXT    2u

enum {
    T_UNKNOWN, T_HTML, T_HEAD, T_BODY, T_A, T_P, T_DIV, T_SPAN,
    T_H1, T_H2, T_H3, T_H4, T_H5, T_H6, T_UL, T_OL, T_LI, T_BR, T_HR, T_IMG,
    T_B, T_STRONG, T_I, T_EM, T_U, T_S, T_PRE, T_CODE, T_BLOCKQUOTE,
    T_TABLE, T_THEAD, T_TBODY, T_TFOOT, T_TR, T_TD, T_TH, T_CAPTION,
    T_INPUT, T_BUTTON, T_SELECT, T_TEXTAREA, T_FORM, T_SMALL, T_BIG,
    T_SUB, T_SUP, T_CENTER, T_DL, T_DT, T_DD, T_BLOCK, T_FONT, T_LABEL,
    T_DETAILS, T_SUMMARY, T_OPTION, T_CITE, T_MARK, T_FIELDSET, T_SKIP,
    T_VOID, T_NOSCRIPT, T_TITLE, T_NQ
};

static const struct {
    const char* name;
    u8 id;
} g_tag_table[] = {
    { "html", T_HTML }, { "head", T_HEAD }, { "body", T_BODY }, { "a", T_A }, { "p", T_P },
    { "div", T_DIV }, { "span", T_SPAN }, { "h1", T_H1 }, { "h2", T_H2 }, { "h3", T_H3 },
    { "h4", T_H4 }, { "h5", T_H5 }, { "h6", T_H6 }, { "ul", T_UL }, { "ol", T_OL },
    { "menu", T_UL }, { "li", T_LI }, { "br", T_BR }, { "hr", T_HR }, { "img", T_IMG },
    { "b", T_B }, { "strong", T_STRONG }, { "i", T_I }, { "em", T_EM }, { "u", T_U },
    { "ins", T_U }, { "s", T_S }, { "del", T_S }, { "strike", T_S }, { "pre", T_PRE },
    { "code", T_CODE }, { "tt", T_CODE }, { "kbd", T_CODE }, { "samp", T_CODE },
    { "blockquote", T_BLOCKQUOTE }, { "table", T_TABLE }, { "thead", T_THEAD },
    { "tbody", T_TBODY }, { "tfoot", T_TFOOT }, { "tr", T_TR }, { "td", T_TD }, { "th", T_TH },
    { "caption", T_CAPTION }, { "input", T_INPUT }, { "button", T_BUTTON },
    { "select", T_SELECT }, { "textarea", T_TEXTAREA }, { "form", T_FORM },
    { "small", T_SMALL }, { "big", T_BIG }, { "sub", T_SUB }, { "sup", T_SUP },
    { "center", T_CENTER }, { "dl", T_DL }, { "dt", T_DT }, { "dd", T_DD },
    { "section", T_BLOCK }, { "article", T_BLOCK }, { "header", T_BLOCK }, { "footer", T_BLOCK },
    { "nav", T_BLOCK }, { "main", T_BLOCK }, { "aside", T_BLOCK }, { "figure", T_BLOCK },
    { "figcaption", T_BLOCK }, { "address", T_BLOCK }, { "hgroup", T_BLOCK },
    { "legend", T_BLOCK }, { "search", T_BLOCK }, { "dialog", T_SKIP },
    { "font", T_FONT }, { "label", T_LABEL }, { "details", T_DETAILS }, { "summary", T_SUMMARY },
    { "option", T_OPTION }, { "optgroup", T_BLOCK }, { "cite", T_CITE }, { "var", T_CITE },
    { "dfn", T_CITE }, { "mark", T_MARK }, { "fieldset", T_FIELDSET },
    { "script", T_SKIP }, { "style", T_SKIP }, { "svg", T_SKIP }, { "template", T_SKIP },
    { "iframe", T_SKIP }, { "object", T_SKIP }, { "canvas", T_SKIP }, { "audio", T_SKIP },
    { "video", T_SKIP }, { "datalist", T_SKIP }, { "math", T_SKIP }, { "noscript", T_NOSCRIPT },
    { "title", T_TITLE }, { "nq", T_NQ },
    { "meta", T_VOID }, { "link", T_VOID }, { "base", T_VOID }, { "source", T_VOID },
    { "track", T_VOID }, { "wbr", T_VOID }, { "area", T_VOID }, { "col", T_VOID },
    { "embed", T_VOID }, { "param", T_VOID },
};

#define TAG_COUNT (sizeof(g_tag_table) / sizeof(g_tag_table[0]))

typedef struct node {
    u8 type;
    u8 tagid;
    u8 has_href, has_style, hidden_attr;
    u8 class_count;
    u16 pad;
    u32 tag;            /* hash of the tag name */
    u32 id;             /* hash of the id attribute, or 0 */
    u32 class_start;    /* into g_classes */
    u32 parent, first, last, next, prev;
    u32 src, src_len;   /* element: its tag text in the page; text: in g_tpool */
    u32 js;             /* element number on the script side, or 0 */
} node_t;

static node_t g_nodes[NODES_MAX];
static u32 g_node_count = 0;
static u32 g_classes[CLASSES_MAX];
static u32 g_class_count = 0;
static char g_tpool[TPOOL_MAX];
static u32 g_tpool_len = 0;
static const char* g_src = "";
static u32 g_tag_hashes[TAG_COUNT];
static int g_tags_ready = 0;

#include "style.c"

static style_t g_styles[NODES_MAX];

/* ::before / ::after text for each element: offset + 1 of the value in g_var_pool */
static u32 g_before[NODES_MAX];
static u32 g_after[NODES_MAX];

/* ---- the display list ---- */

#define OP_RECT  1u
#define OP_TEXT  2u
#define OP_IMAGE 3u

#define OPF_UNDERLINE 1u
#define OPF_ITALIC    2u
#define OPF_STRIKE    4u

typedef struct op {
    u8 type;
    u8 face;
    u8 flags;
    u8 space;       /* text: width of the gap before it that belongs to it */
    i32 x, y;
    i32 w, h;
    u32 color;
    u32 bg;         /* text: colour behind it, or NO_COLOR */
    u32 text;       /* text: offset into g_tpool; image: picture index */
    u16 len;
    u16 link;       /* 0 = none, else index + 1 into g_links */
    u32 node;       /* script-side element, or 0 */
} op_t;

static op_t g_ops[OPS_MAX];
static u32 g_op_count = 0;
static u32 g_page_bg = 0x00FFFFFFu;

/* While measuring how wide something wants to be: the furthest right reached. */
static int g_measuring = 0;
static i32 g_measure_right = 0;

/* -------------------------------------------------------------------------- */
/* Text                                                                        */
/* -------------------------------------------------------------------------- */

/* Unicode characters that have a place in codes 128..159 of our font. */
static const struct {
    u16 unicode;
    u8 code;
} g_cp1252[] = {
    { 0x20AC, 0x80 }, { 0x201A, 0x82 }, { 0x0192, 0x83 }, { 0x201E, 0x84 }, { 0x2026, 0x85 },
    { 0x2020, 0x86 }, { 0x2021, 0x87 }, { 0x02C6, 0x88 }, { 0x2030, 0x89 }, { 0x0160, 0x8A },
    { 0x2039, 0x8B }, { 0x0152, 0x8C }, { 0x017D, 0x8E }, { 0x2018, 0x91 }, { 0x2019, 0x92 },
    { 0x201C, 0x93 }, { 0x201D, 0x94 }, { 0x2022, 0x95 }, { 0x2013, 0x96 }, { 0x2014, 0x97 },
    { 0x02DC, 0x98 }, { 0x2122, 0x99 }, { 0x0161, 0x9A }, { 0x203A, 0x9B }, { 0x0153, 0x9C },
    { 0x017E, 0x9E }, { 0x0178, 0x9F }, { 0x2212, '-' }, { 0x2010, '-' }, { 0x2011, '-' },
    { 0x2012, 0x96 }, { 0x2015, 0x97 }, { 0x2032, '\'' }, { 0x2033, '"' }, { 0x00A0, ' ' },
    { 0x2002, ' ' }, { 0x2003, ' ' }, { 0x2009, ' ' }, { 0x200A, ' ' }, { 0x202F, ' ' },
    { 0x2190, '<' }, { 0x2192, '>' }, { 0x2191, '^' }, { 0x2193, 'v' }, { 0x25CF, 0x95 },
    { 0x25E6, 0x95 }, { 0x2023, 0x95 }, { 0x00B7, 0xB7 }, { 0x2713, 'v' }, { 0x2715, 'x' },
    { 0x2716, 'x' }, { 0x00D7, 0xD7 },
};

/*
 * Copies page text into the pool as single-byte characters (Windows-1252,
 * which the font covers), turning "&amp;" and friends and UTF-8 sequences
 * into them. Returns the number of bytes written.
 */
static u32 text_convert(const char* src, u32 length, char* out, u32 room, int entities) {
    u32 n = 0;

    for (u32 i = 0; i < length && n + 8u < room; ) {
        u8 c = (u8)src[i];

        if (c == '&' && entities) {
            /* numeric references are done here, so they can use the whole font */
            if (i + 2u < length && src[i + 1u] == '#') {
                u32 k = i + 2u;
                u32 code = 0;
                int hex = k < length && (src[k] == 'x' || src[k] == 'X');

                if (hex) {
                    k++;
                }

                u32 digits = k;

                while (k < length && (hex ? css_hex(src[k]) >= 0 : (src[k] >= '0' && src[k] <= '9'))) {
                    code = hex ? code * 16u + (u32)css_hex(src[k]) : code * 10u + (u32)(src[k] - '0');
                    k++;

                    if (code > 0x10FFFFu) {
                        code = 0x10FFFFu;
                    }
                }

                if (k > digits) {
                    if (k < length && src[k] == ';') {
                        k++;
                    }

                    char found = '?';

                    if (code < 128u || (code >= 160u && code < 256u)) {
                        found = (char)code;
                    } else if (code == 0x200Bu || code == 0x200Eu || code == 0x200Fu || code == 0xFEFFu) {
                        found = 0;
                    } else {
                        for (u32 t = 0; t < sizeof(g_cp1252) / sizeof(g_cp1252[0]); t++) {
                            if (g_cp1252[t].unicode == code) {
                                found = (char)g_cp1252[t].code;
                            }
                        }
                    }

                    if (found) {
                        out[n++] = found;
                    }

                    i = k;
                    continue;
                }
            }

            char decoded[8];
            u32 decoded_len = 0;
            u32 used = decode_entity(src + i + 1u, length - i - 1u, decoded, &decoded_len);

            if (used) {
                for (u32 k = 0; k < decoded_len; k++) {
                    out[n++] = decoded[k];
                }

                i += used + 1u;
                continue;
            }

            out[n++] = '&';
            i++;
            continue;
        }

        if (c < 0x80u) {
            out[n++] = (char)c;
            i++;
            continue;
        }

        /* UTF-8 */
        u32 need = (c & 0xE0u) == 0xC0u ? 1u : ((c & 0xF0u) == 0xE0u ? 2u : ((c & 0xF8u) == 0xF0u ? 3u : 0u));
        u32 code = need == 1u ? (c & 0x1Fu) : (need == 2u ? (c & 0x0Fu) : (c & 0x07u));

        if (need == 0 || i + need >= length) {
            /* not UTF-8 (an old page in Latin-1): the byte is already the character */
            out[n++] = c >= 0xA0u ? (char)c : '?';
            i++;
            continue;
        }

        int good = 1;

        for (u32 k = 1; k <= need; k++) {
            u8 next = (u8)src[i + k];

            if ((next & 0xC0u) != 0x80u) {
                good = 0;
                break;
            }

            code = (code << 6) | (next & 0x3Fu);
        }

        if (!good) {
            out[n++] = c >= 0xA0u ? (char)c : '?';
            i++;
            continue;
        }

        i += need + 1u;

        if (code >= 160u && code < 256u) {
            out[n++] = (char)code;
            continue;
        }

        /* invisible marks: nothing at all */
        if (code == 0x200Bu || code == 0x200Cu || code == 0x200Du || code == 0x200Eu ||
            code == 0x200Fu || code == 0xFEFFu || code == 0x00ADu || (code >= 0xFE00u && code <= 0xFE0Fu)) {
            continue;
        }

        char found = '?';

        for (u32 t = 0; t < sizeof(g_cp1252) / sizeof(g_cp1252[0]); t++) {
            if (g_cp1252[t].unicode == code) {
                found = (char)g_cp1252[t].code;
            }
        }

        out[n++] = found;
    }

    return n;
}

static const font_face_t* face_get(u32 index) {
    return &g_font_faces[index < FONT_FACE_COUNT ? index : 0u];
}

/* The face closest to a size in pixels. */
static u8 face_for(u32 px, int bold, int mono) {
    u32 best = 0;
    u32 best_diff = 1000;

    for (u32 i = 0; i < FONT_FACE_COUNT; i++) {
        const font_face_t* f = &g_font_faces[i];

        if (f->mono != (mono ? 1 : 0) || f->bold != (bold ? 1 : 0)) {
            continue;
        }

        u32 diff = f->size > px ? f->size - px : px - f->size;

        if (diff < best_diff) {
            best_diff = diff;
            best = i;
        }
    }

    return (u8)best;
}

static i32 text_measure(u8 face, const char* text, u32 length) {
    const font_glyph_t* glyphs = face_get(face)->glyphs;
    i32 w = 0;

    for (u32 i = 0; i < length; i++) {
        u8 c = (u8)text[i];

        w += glyphs[c >= 32u ? c - 32u : 0u].advance;
    }

    return w;
}

/* -------------------------------------------------------------------------- */
/* Building the tree                                                           */
/* -------------------------------------------------------------------------- */

static u32 g_js_now = 0;        /* the script-side element that following text belongs to */

static u8 tag_lookup(u32 hash) {
    if (!g_tags_ready) {
        for (u32 i = 0; i < TAG_COUNT; i++) {
            g_tag_hashes[i] = css_hash(g_tag_table[i].name, (u32)strlen(g_tag_table[i].name), 1);
        }

        g_tags_ready = 1;
    }

    for (u32 i = 0; i < TAG_COUNT; i++) {
        if (g_tag_hashes[i] == hash) {
            return g_tag_table[i].id;
        }
    }

    return T_UNKNOWN;
}

static u32 node_new(u32 parent, u32 type) {
    if (g_node_count >= NODES_MAX) {
        return 0;
    }

    u32 index = g_node_count++;
    node_t* n = &g_nodes[index];

    memset(n, 0, sizeof(*n));
    n->type = (u8)type;
    n->parent = parent;
    n->js = g_js_now;

    node_t* p = &g_nodes[parent];

    n->prev = p->last;

    if (p->last) {
        g_nodes[p->last].next = index;
    } else {
        p->first = index;
    }

    p->last = index;

    return index;
}

static void tree_reset(void) {
    g_node_count = 1;
    g_class_count = 0;
    g_tpool_len = 0;
    g_op_count = 0;
    g_js_now = 0;

    memset(&g_nodes[0], 0, sizeof(g_nodes[0]));
    g_nodes[0].type = N_ELEMENT;
    g_nodes[0].tagid = T_BLOCK;
    g_nodes[0].tag = 1;
}

/* Elements inside which text made only of spaces means nothing. */
static int drops_space(u8 tagid) {
    return tagid == T_TABLE || tagid == T_THEAD || tagid == T_TBODY || tagid == T_TFOOT ||
           tagid == T_TR || tagid == T_UL || tagid == T_OL || tagid == T_HTML ||
           tagid == T_HEAD || tagid == T_DL || tagid == T_SELECT;
}

static void add_text(u32 parent, const char* src, u32 length, int entities) {
    if (!length || g_tpool_len + length + 16u >= TPOOL_MAX) {
        return;
    }

    char* out = g_tpool + g_tpool_len;
    u32 n = text_convert(src, length, out, TPOOL_MAX - g_tpool_len, entities);
    int blank = 1;

    for (u32 i = 0; i < n; i++) {
        if (!css_space(out[i])) {
            blank = 0;
            break;
        }
    }

    if (!n || (blank && drops_space(g_nodes[parent].tagid))) {
        return;
    }

    u32 t = node_new(parent, N_TEXT);

    if (t) {
        g_nodes[t].src = g_tpool_len;
        g_nodes[t].src_len = n;
        g_tpool_len += n;
    }
}

/* Finds "</name" from `from`; returns the index just after its ">". */
static u32 skip_to_close(const char* html, u32 length, u32 from, const char* name, u32* content_end) {
    u32 n = (u32)strlen(name);

    for (u32 i = from; i + n + 2u <= length; i++) {
        if (html[i] != '<' || html[i + 1u] != '/') {
            continue;
        }

        u32 k = 0;

        while (k < n && lower(html[i + 2u + k]) == name[k]) {
            k++;
        }

        if (k == n) {
            *content_end = i;

            while (i < length && html[i] != '>') {
                i++;
            }

            return i + 1u;
        }
    }

    *content_end = length;

    return length;
}

static void build_tree(const char* html, u32 length) {
    u32 stack[DEPTH_MAX];
    u32 depth = 0;
    u32 i = 0;
    u32 text_start = 0;

    tree_reset();
    g_src = html;
    stack[depth++] = 0;

#define CUR stack[depth - 1u]
#define FLUSH(end) do { if ((end) > text_start) add_text(CUR, html + text_start, (end) - text_start, 1); } while (0)

    while (i < length) {
        if (html[i] != '<') {
            i++;
            continue;
        }

        /* comment */
        if (i + 3u < length && html[i + 1u] == '!' && html[i + 2u] == '-' && html[i + 3u] == '-') {
            FLUSH(i);
            i += 4u;

            while (i + 2u < length && !(html[i] == '-' && html[i + 1u] == '-' && html[i + 2u] == '>')) {
                i++;
            }

            i += 3u;

            if (i > length) {
                i = length;
            }

            text_start = i;
            continue;
        }

        char first = i + 1u < length ? html[i + 1u] : 0;
        int closing = first == '/';

        if (!(closing || first == '!' || first == '?' || (lower(first) >= 'a' && lower(first) <= 'z'))) {
            i++;        /* a "<" that is just text */
            continue;
        }

        /* the end of the tag, minding quotes */
        u32 end = i + 1u;
        char quote = 0;

        while (end < length && (quote || html[end] != '>')) {
            if (quote) {
                if (html[end] == quote) quote = 0;
            } else if (html[end] == '"' || html[end] == '\'') {
                quote = html[end];
            }

            end++;
        }

        if (end >= length) {
            break;
        }

        FLUSH(i);

        const char* tag = html + i + 1u + (closing ? 1u : 0u);
        u32 tag_len = end - i - 1u - (closing ? 1u : 0u);
        u32 name_len = 0;

        while (name_len < tag_len && css_ident_char(tag[name_len]) && tag[name_len] != '\\') {
            name_len++;
        }

        i = end + 1u;
        text_start = i;

        if (first == '!' || first == '?' || !name_len) {
            continue;
        }

        u32 hash = css_hash(tag, name_len, 1);
        u8 tagid = tag_lookup(hash);

        if (closing) {
            for (u32 d = depth; d > 1u; d--) {
                if (g_nodes[stack[d - 1u]].tag == hash) {
                    depth = d - 1u;
                    break;
                }
            }

            continue;
        }

        if (tagid == T_NQ) {
            u32 value = 0;

            for (u32 k = name_len + 1u; k < tag_len && tag[k] >= '0' && tag[k] <= '9'; k++) {
                value = value * 10u + (u32)(tag[k] - '0');
            }

            g_js_now = value;
            continue;
        }

        if (tagid == T_TITLE) {
            u32 content_end = 0;
            u32 after = skip_to_close(html, length, i, "title", &content_end);
            char text[sizeof(g_title)];
            u32 n = text_convert(html + i, content_end - i > 200u ? 200u : content_end - i,
                                 text, sizeof(text), 1);
            u32 k = 0;

            for (u32 t = 0; t < n && k + 1u < sizeof(g_title); t++) {
                char c = css_space(text[t]) ? ' ' : text[t];

                if ((u8)c < 32u || (u8)c > 126u) {
                    c = '?';
                }

                if (c == ' ' && (k == 0 || g_title[k - 1u] == ' ')) {
                    continue;
                }

                g_title[k++] = c;
            }

            g_title[k] = 0;
            i = after;
            text_start = i;
            continue;
        }

        if (tagid == T_SKIP || (tagid == T_NOSCRIPT && g_js_enabled)) {
            char name[16];
            u32 content_end = 0;
            u32 k = 0;

            for (; k < name_len && k + 1u < sizeof(name); k++) {
                name[k] = lower(tag[k]);
            }

            name[k] = 0;
            i = skip_to_close(html, length, i, name, &content_end);
            text_start = i;
            continue;
        }

        /* elements that end the one before them */
        if (tagid == T_LI) {
            for (u32 d = depth; d > 1u; d--) {
                u8 t = g_nodes[stack[d - 1u]].tagid;

                if (t == T_LI) { depth = d - 1u; break; }
                if (t == T_UL || t == T_OL) break;
            }
        } else if (tagid == T_TR) {
            for (u32 d = depth; d > 1u; d--) {
                u8 t = g_nodes[stack[d - 1u]].tagid;

                if (t == T_TR) { depth = d - 1u; break; }
                if (t == T_TABLE) break;
            }
        } else if (tagid == T_TD || tagid == T_TH) {
            for (u32 d = depth; d > 1u; d--) {
                u8 t = g_nodes[stack[d - 1u]].tagid;

                if (t == T_TD || t == T_TH) { depth = d - 1u; break; }
                if (t == T_TR || t == T_TABLE) break;
            }
        } else if (tagid == T_DT || tagid == T_DD) {
            u8 t = g_nodes[CUR].tagid;

            if (t == T_DT || t == T_DD) depth--;
        } else if (tagid == T_OPTION) {
            if (g_nodes[CUR].tagid == T_OPTION) depth--;
        } else if (g_nodes[CUR].tagid == T_P &&
                   (tagid == T_P || tagid == T_DIV || (tagid >= T_H1 && tagid <= T_OL) ||
                    tagid == T_PRE || tagid == T_BLOCKQUOTE || tagid == T_TABLE || tagid == T_HR ||
                    tagid == T_FORM || tagid == T_BLOCK || tagid == T_DL || tagid == T_FIELDSET)) {
            depth--;
        }

        if (depth < 1u) {
            depth = 1u;
        }

        u32 n = node_new(CUR, N_ELEMENT);

        if (!n) {
            break;
        }

        node_t* node = &g_nodes[n];
        char value[512];

        node->tag = hash;
        node->tagid = tagid;
        node->src = (u32)(tag - html);
        node->src_len = tag_len;

        if (tag_len > name_len) {
            if (attribute(tag, tag_len, "id", value, sizeof(value)) && value[0]) {
                node->id = css_hash(value, (u32)strlen(value), 0);
            }

            if (attribute(tag, tag_len, "class", value, sizeof(value))) {
                node->class_start = g_class_count;

                for (u32 k = 0; value[k] && node->class_count < 24u && g_class_count < CLASSES_MAX; ) {
                    while (css_space(value[k])) {
                        k++;
                    }

                    u32 start = k;

                    while (value[k] && !css_space(value[k])) {
                        k++;
                    }

                    if (k > start) {
                        g_classes[g_class_count++] = css_hash(value + start, k - start, 0);
                        node->class_count++;
                    }
                }
            }

            node->has_style = (u8)has_flag(tag, tag_len, "style");
            node->hidden_attr = (u8)has_flag(tag, tag_len, "hidden");
            node->has_href = (u8)has_flag(tag, tag_len, "href");
        }

        int is_void = tagid == T_BR || tagid == T_HR || tagid == T_IMG || tagid == T_INPUT ||
                      tagid == T_VOID || (tag_len && tag[tag_len - 1u] == '/' && tagid == T_UNKNOWN);

        if (tagid == T_TEXTAREA || tagid == T_SELECT) {
            /* shown as one control; what is inside is not page text */
            u32 content_end = 0;

            i = skip_to_close(html, length, i, tagid == T_SELECT ? "select" : "textarea", &content_end);
            text_start = i;
            continue;
        }

        if (!is_void && depth < DEPTH_MAX) {
            stack[depth++] = n;
        }
    }

    FLUSH(length);

#undef CUR
#undef FLUSH
}

/* -------------------------------------------------------------------------- */
/* Built-in styles for each kind of element                                    */
/* -------------------------------------------------------------------------- */

static i16 em(const style_t* s, i32 hundredths) {
    return (i16)(((i32)s->font_px * hundredths) / 100);
}

static void ua_defaults(u32 index, style_t* s, const style_t* parent) {
    const node_t* n = &g_nodes[index];
    char value[64];

    (void)parent;

    switch (n->tagid) {
        case T_HTML: case T_DIV: case T_BLOCK: case T_FORM: case T_DT: case T_DETAILS:
        case T_SUMMARY: case T_CAPTION: case T_OPTION:
            s->display = D_BLOCK;
            break;

        case T_BODY:
            s->display = D_BLOCK;
            s->margin[0] = s->margin[1] = s->margin[2] = s->margin[3] = 8;
            break;

        case T_HEAD: case T_SKIP: case T_VOID: case T_TITLE: case T_NOSCRIPT:
            s->display = D_NONE;
            break;

        case T_P: case T_DL:
            s->display = D_BLOCK;
            s->margin[0] = s->margin[2] = em(s, 100);
            break;

        case T_H1: case T_H2: case T_H3: case T_H4: case T_H5: case T_H6: {
            static const u16 size[6] = { 200, 150, 117, 100, 83, 67 };
            static const u16 gap[6] = { 67, 83, 100, 133, 167, 233 };
            u32 level = n->tagid - T_H1;

            s->display = D_BLOCK;
            s->bold = 1;
            s->font_px = (u16)(((u32)s->font_px * size[level]) / 100u);
            s->margin[0] = s->margin[2] = em(s, gap[level]);
            break;
        }

        case T_UL: case T_OL:
            s->display = D_BLOCK;
            s->padding[3] = 32;
            s->list_none = 0;

            /* a list inside a list has no space above and below */
            if (g_nodes[n->parent].tagid != T_LI) {
                s->margin[0] = s->margin[2] = em(s, 100);
            }

            break;

        case T_LI:
            s->display = D_LIST_ITEM;
            break;

        case T_DD:
            s->display = D_BLOCK;
            s->margin[3] = 32;
            break;

        case T_BLOCKQUOTE:
            s->display = D_BLOCK;
            s->margin[0] = s->margin[2] = em(s, 100);
            s->margin[1] = s->margin[3] = 32;
            break;

        case T_PRE:
            s->display = D_BLOCK;
            s->white = WHITE_PRE;
            s->mono = 1;
            s->margin[0] = s->margin[2] = em(s, 100);
            break;

        case T_CODE:
            s->mono = 1;
            break;

        case T_HR:
            s->display = D_BLOCK;
            s->margin[0] = s->margin[2] = 8;
            s->border[0] = 1;
            s->border_color = 0x00A8B4C0u;
            break;

        case T_A:
            if (n->has_href) {
                s->color = 0x000645ADu;
                s->underline = 1;
            }

            break;

        case T_B: case T_STRONG:
            s->bold = 1;
            break;

        case T_I: case T_EM: case T_CITE:
            s->italic = 1;
            break;

        case T_U:
            s->underline = 1;
            break;

        case T_SMALL: case T_SUB: case T_SUP:
            s->font_px = (u16)(((u32)s->font_px * 83u) / 100u);
            break;

        case T_BIG:
            s->font_px = (u16)(((u32)s->font_px * 120u) / 100u);
            break;

        case T_MARK:
            s->bg = 0x00FFF2A8u;
            break;

        case T_CENTER:
            s->display = D_BLOCK;
            s->align = ALIGN_CENTER;
            break;

        case T_TABLE:
            s->display = D_TABLE;
            s->align = ALIGN_LEFT;
            s->white = WHITE_NORMAL;
            break;

        case T_THEAD: case T_TBODY: case T_TFOOT:
            s->display = D_ROW_GROUP;
            break;

        case T_TR:
            s->display = D_ROW;
            break;

        case T_TD: case T_TH: {
            s->display = D_CELL;
            s->padding[0] = s->padding[1] = s->padding[2] = s->padding[3] = 2;

            if (n->tagid == T_TH) {
                s->bold = 1;
                s->align = ALIGN_CENTER;
            }

            /* <table border="1"> draws lines round its cells */
            for (u32 a = n->parent; a; a = g_nodes[a].parent) {
                if (g_nodes[a].tagid == T_TABLE) {
                    if (attribute(g_src + g_nodes[a].src, g_nodes[a].src_len, "border", value, sizeof(value)) &&
                        value[0] && value[0] != '0') {
                        s->border[0] = s->border[1] = s->border[2] = s->border[3] = 1;
                        s->border_color = 0x00A0A8B0u;
                        s->padding[1] = s->padding[3] = 4;
                    }

                    if (attribute(g_src + g_nodes[a].src, g_nodes[a].src_len, "cellpadding", value, sizeof(value)) &&
                        value[0] >= '0' && value[0] <= '9') {
                        i16 pad = (i16)(value[0] - '0');

                        if (value[1] >= '0' && value[1] <= '9') {
                            pad = (i16)(pad * 10 + (value[1] - '0'));
                        }

                        s->padding[0] = s->padding[1] = s->padding[2] = s->padding[3] = pad;
                    }

                    break;
                }
            }

            break;
        }

        case T_FIELDSET:
            s->display = D_BLOCK;
            s->border[0] = s->border[1] = s->border[2] = s->border[3] = 1;
            s->border_color = 0x00A8B4C0u;
            s->padding[0] = s->padding[1] = s->padding[2] = s->padding[3] = 8;
            s->margin[0] = s->margin[2] = 8;
            break;

        case T_BUTTON:
            s->display = D_INLINE_BLOCK;
            s->padding[0] = s->padding[2] = 2;
            s->padding[1] = s->padding[3] = 8;
            s->border[0] = s->border[1] = s->border[2] = s->border[3] = 1;
            s->border_color = 0x006E9AB8u;
            s->bg = 0x00E4F1FAu;
            s->color = 0x0016445Eu;
            s->font_px = 14;
            s->bold = 0;
            s->underline = 0;
            s->align = ALIGN_CENTER;
            break;

        case T_INPUT: case T_SELECT: case T_TEXTAREA:
            s->display = D_INLINE_BLOCK;
            s->font_px = 14;
            s->bold = 0;
            s->italic = 0;
            s->underline = 0;
            break;

        case T_IMG:
            s->display = D_INLINE_BLOCK;
            break;

        default:
            break;
    }

    /* old-style attributes that set presentation */
    if (n->src_len > 6u && (n->tagid == T_DIV || n->tagid == T_P || n->tagid == T_TD || n->tagid == T_TH ||
                            n->tagid == T_TABLE || n->tagid == T_TR || n->tagid == T_BODY ||
                            n->tagid == T_FONT || (n->tagid >= T_H1 && n->tagid <= T_H6))) {
        const char* tag = g_src + n->src;
        u32 color;

        if (n->tagid != T_TABLE && attribute(tag, n->src_len, "align", value, sizeof(value))) {
            if (css_is(value, (u32)strlen(value), "center")) s->align = ALIGN_CENTER;
            else if (css_is(value, (u32)strlen(value), "right")) s->align = ALIGN_RIGHT;
            else if (css_is(value, (u32)strlen(value), "left")) s->align = ALIGN_LEFT;
        }

        if (attribute(tag, n->src_len, "bgcolor", value, sizeof(value)) &&
            css_color(value, (u32)strlen(value), &color) == 1) {
            s->bg = color;
        }

        if (n->tagid == T_FONT && attribute(tag, n->src_len, "color", value, sizeof(value)) &&
            css_color(value, (u32)strlen(value), &color) == 1) {
            s->color = color;
        }

        if ((n->tagid == T_TABLE || n->tagid == T_TD || n->tagid == T_TH) &&
            attribute(tag, n->src_len, "width", value, sizeof(value))) {
            decl_t d;
            u32 len = (u32)strlen(value);

            if (len && value[len - 1u] >= '0' && value[len - 1u] <= '9') {
                d.unit = U_PX;
                d.v = 0;

                for (u32 k = 0; k < len && value[k] >= '0' && value[k] <= '9'; k++) {
                    d.v = d.v * 10 + (value[k] - '0');
                }

                s->width_unit = U_PX;
                s->width = d.v;
            } else if (css_length(value, len, &d) && d.unit == U_PCT) {
                s->width_unit = U_PCT;
                s->width = d.v;
            }
        }
    }
}

static void compute_styles(u32 index, const style_t* parent) {
    for (u32 c = g_nodes[index].first; c; c = g_nodes[c].next) {
        if (g_nodes[c].type != N_ELEMENT) {
            continue;
        }

        style_compute(c, &g_styles[c], parent);
        g_before[c] = style_pseudo(c, 1);
        g_after[c] = style_pseudo(c, 2);

        if (g_styles[c].display != D_NONE) {
            compute_styles(c, &g_styles[c]);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Layout                                                                      */
/* -------------------------------------------------------------------------- */

#define TOK_WORD 1u
#define TOK_ATOM 2u     /* a box placed as a unit: picture, control, inline block */

typedef struct token {
    u8 kind, face, flags, pad;
    i16 space;          /* gap before it */
    i16 w, h;
    u32 text;
    u16 len, link;
    u32 color, bg, node;
    u32 op_first, op_count;     /* atom: its display items, placed at 0,0 for now */
} token_t;

typedef struct line {
    i32 x0, width;      /* the strip that lines fill */
    i32 y;              /* top of the line being built */
    i32 used;           /* width taken so far */
    i32 pending_margin; /* bottom margin of the block just placed */
    u8 align, white;
    i16 line_h;         /* minimum line height from the block's style */
    int space;          /* a space is owed before the next word */
    i32 lead;           /* extra gap owed before the next thing (an inline box's edge) */
    u32 count;
    token_t tokens[LINE_TOKENS];
} line_t;

/* ---- floats ----
 *
 * A float is a box pushed to the left or right edge; the lines of text
 * beside it are made shorter so that they run round it. The floats in
 * force are kept in a list, in page coordinates. Some boxes start a fresh
 * list for their insides (table cells, inline blocks, flex items, floats
 * themselves): floats outside do not reach in, and theirs do not leak out.
 */
#define FLOATS_MAX 256u

typedef struct float_box {
    i32 x0, x1, y0, y1;
    u8 right;
} float_box_t;

static float_box_t g_floats[FLOATS_MAX];
static u32 g_float_count = 0;
static u32 g_float_base = 0;        /* where the current list starts */
static int g_new_context = 0;       /* the next block starts a fresh list */

/* Narrows left..right to what the floats leave free at height y..y+h. */
static void float_strip(i32 y, i32 h, i32* left, i32* right) {
    for (u32 i = g_float_base; i < g_float_count; i++) {
        const float_box_t* f = &g_floats[i];

        if (f->y0 >= y + h || f->y1 <= y) {
            continue;
        }

        if (f->right) {
            if (f->x0 < *right) *right = f->x0;
        } else if (f->x1 > *left) {
            *left = f->x1;
        }
    }
}

/* The first height at or below y that is clear of floats on those sides. */
static i32 float_clear_y(i32 y, u32 sides) {
    for (u32 i = g_float_base; i < g_float_count; i++) {
        const float_box_t* f = &g_floats[i];

        if (((f->right ? 2u : 1u) & sides) && f->y1 > y) {
            y = f->y1;
        }
    }

    return y;
}

/* The nearest float bottom below y (y itself if there is none). */
static i32 float_next_y(i32 y) {
    i32 best = y;

    for (u32 i = g_float_base; i < g_float_count; i++) {
        const float_box_t* f = &g_floats[i];

        if (f->y1 > y && f->y0 <= y && (best == y || f->y1 < best)) {
            best = f->y1;
        }
    }

    return best;
}

/*
 * The strip the current line may fill: the block's width, less whatever
 * floats take at this height. An empty line that has no room moves down
 * past the floats.
 */
static void line_strip(line_t* L, i32* x, i32* w) {
    i32 left = L->x0;
    i32 right = L->x0 + L->width;

    if (g_float_count > g_float_base) {
        for (int tries = 0; tries < 16; tries++) {
            i32 y = L->y + L->pending_margin;

            left = L->x0;
            right = L->x0 + L->width;
            float_strip(y, 16, &left, &right);

            if (right - left >= 60 || L->count) {
                break;
            }

            i32 below = float_next_y(y);

            if (below <= y) {
                break;
            }

            L->y += below - y;
        }
    }

    if (right - left < 8) {
        right = left + 8;
    }

    *x = left;
    *w = right - left;
}

static op_t* op_new(u32 type) {
    static op_t spare;

    if (g_op_count >= OPS_MAX) {
        return &spare;
    }

    op_t* o = &g_ops[g_op_count++];

    memset(o, 0, sizeof(*o));
    o->type = (u8)type;
    o->bg = NO_COLOR;

    return o;
}

static void op_rect(i32 x, i32 y, i32 w, i32 h, u32 color, u32 node) {
    if (w <= 0 || h <= 0) {
        return;
    }

    op_t* o = op_new(OP_RECT);

    o->x = x;
    o->y = y;
    o->w = w;
    o->h = h;
    o->color = color;
    o->node = node;
}

/* Puts the tokens gathered so far on a line and starts the next. */
static void line_flush(line_t* L) {
    if (!L->count) {
        return;
    }

    L->y += L->pending_margin;
    L->pending_margin = 0;

    i32 ascent = 0;
    i32 descent = 0;

    for (u32 i = 0; i < L->count; i++) {
        const token_t* t = &L->tokens[i];

        if (t->kind == TOK_WORD) {
            const font_face_t* f = face_get(t->face);

            if (f->ascent > ascent) ascent = f->ascent;
            if (f->height - f->ascent > descent) descent = f->height - f->ascent;
        } else if (t->h > ascent) {
            ascent = t->h;
        }
    }

    i32 height = ascent + descent;
    i32 extra = L->line_h > height ? L->line_h - height : (L->line_h ? 0 : height / 6);
    i32 strip_x;
    i32 strip_w;

    line_strip(L, &strip_x, &strip_w);

    i32 x = strip_x;

    if (L->align == ALIGN_CENTER) {
        x += (strip_w - L->used) / 2;
    } else if (L->align == ALIGN_RIGHT) {
        x += strip_w - L->used;
    }

    if (x < strip_x) {
        x = strip_x;
    }

    i32 top = L->y + extra / 2;

    for (u32 i = 0; i < L->count; i++) {
        const token_t* t = &L->tokens[i];

        x += t->space;

        if (t->kind == TOK_WORD) {
            const font_face_t* f = face_get(t->face);
            op_t* o = op_new(OP_TEXT);

            int joined = i > 0 && L->tokens[i - 1u].kind == TOK_WORD &&
                         L->tokens[i - 1u].link == t->link && L->tokens[i - 1u].bg == t->bg &&
                         L->tokens[i - 1u].flags == t->flags && L->tokens[i - 1u].node == t->node;

            o->face = t->face;
            o->flags = t->flags;
            o->space = (u8)(joined ? (t->space > 255 ? 255 : t->space) : 0);
            o->x = x;
            o->y = top + ascent - f->ascent;
            o->w = t->w;
            o->h = f->height;
            o->color = t->color;
            o->bg = t->bg;
            o->text = t->text;
            o->len = t->len;
            o->link = t->link;
            o->node = t->node;
        } else {
            /* the atom's items were laid out at 0,0: move them into place */
            i32 dy = top + ascent - t->h;

            for (u32 k = t->op_first; k < t->op_first + t->op_count && k < g_op_count; k++) {
                g_ops[k].x += x;
                g_ops[k].y += dy;
            }
        }

        x += t->w;
    }

    if (g_measuring && strip_x + L->used > g_measure_right) {
        g_measure_right = strip_x + L->used;
    }

    L->y += height + extra;
    L->count = 0;
    L->used = 0;
    L->space = 0;
    L->lead = 0;
}

static token_t* line_token(line_t* L, i32 w, int breakable) {
    i32 space = (L->space && L->count) ? 1 : 0;

    /* (the caller turns the owed space into pixels; here only wrapping) */
    (void)space;

    if (breakable && L->count && L->white == WHITE_NORMAL && L->used + w > L->width) {
        line_flush(L);
    }

    if (L->count >= LINE_TOKENS) {
        line_flush(L);
    }

    token_t* t = &L->tokens[L->count++];

    memset(t, 0, sizeof(*t));
    t->bg = NO_COLOR;

    return t;
}

/* The inherited things a run of text needs. */
typedef struct run {
    const style_t* s;
    u16 link;
    u32 node;
} run_t;

static void add_word(line_t* L, const run_t* r, u32 text, u32 length, int space_before) {
    const style_t* s = r->s;
    u8 face = face_for(s->font_px, s->bold, s->mono);
    i32 w = text_measure(face, g_tpool + text, length);
    i32 gap = 0;

    if (space_before && L->count) {
        gap = face_get(face)->glyphs[0].advance;
    }

    gap += L->lead;
    L->lead = 0;

    i32 strip_x;
    i32 strip_w;

    line_strip(L, &strip_x, &strip_w);

    /* does it fit with its gap? if not it starts the next line without one */
    if (L->count && L->white == WHITE_NORMAL && L->used + gap + w > strip_w) {
        line_flush(L);
        gap = 0;
    }

    token_t* t = line_token(L, 0, 0);

    t->kind = TOK_WORD;
    t->face = face;
    t->flags = (u8)((s->underline ? OPF_UNDERLINE : 0u) | (s->italic ? OPF_ITALIC : 0u));
    t->space = (i16)gap;
    t->w = (i16)(w > 30000 ? 30000 : w);
    t->text = text;
    t->len = (u16)length;
    t->link = r->link;
    t->color = s->color;
    t->bg = s->display == D_INLINE ? s->bg : NO_COLOR;
    t->node = r->node;

    L->used += gap + w;
}

/* A piece of page text, split into words according to the white-space mode. */
static void add_text_run(line_t* L, const run_t* r, u32 text, u32 length) {
    const style_t* s = r->s;

    if (s->hidden) {
        return;
    }

    if (s->upper) {
        /* a private upper-case copy */
        if (g_tpool_len + length + 1u < TPOOL_MAX) {
            for (u32 i = 0; i < length; i++) {
                char c = g_tpool[text + i];

                g_tpool[g_tpool_len + i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
            }

            text = g_tpool_len;
            g_tpool_len += length;
        }
    }

    if (s->white == WHITE_PRE) {
        u32 start = 0;

        for (u32 i = 0; i <= length; i++) {
            if (i < length && g_tpool[text + i] != '\n') {
                continue;
            }

            if (i > start) {
                u32 n = i - start;

                if (n && g_tpool[text + i - 1u] == '\r') {
                    n--;
                }

                /* long lines go in pieces a token can hold */
                for (u32 at = 0; at < n; at += 200u) {
                    add_word(L, r, text + start + at, n - at > 200u ? 200u : n - at, 0);
                }
            }

            if (i < length) {
                if (!L->count) {
                    /* an empty line still takes a line's height */
                    L->y += face_get(face_for(s->font_px, 0, s->mono))->height;
                }

                line_flush(L);
            }

            start = i + 1u;
        }

        return;
    }

    u32 i = 0;

    while (i < length) {
        if (css_space(g_tpool[text + i])) {
            L->space = 1;
            i++;
            continue;
        }

        u32 start = i;

        while (i < length && !css_space(g_tpool[text + i])) {
            i++;
        }

        /* a very long "word" (an address, say) is cut so that it can wrap */
        for (u32 at = start; at < i; at += 60u) {
            add_word(L, r, text + at, i - at > 60u ? 60u : i - at, at == start ? L->space : 0);
        }

        L->space = 0;
    }
}

static void add_atom(line_t* L, u32 op_first, i32 w, i32 h) {
    i32 gap = ((L->space && L->count) ? 4 : 0) + L->lead;

    L->lead = 0;

    i32 strip_x;
    i32 strip_w;

    line_strip(L, &strip_x, &strip_w);

    if (L->count && L->white == WHITE_NORMAL && L->used + gap + w > strip_w) {
        line_flush(L);
        gap = 0;
    }

    token_t* t = line_token(L, 0, 0);

    t->kind = TOK_ATOM;
    t->space = (i16)gap;
    t->w = (i16)(w > 30000 ? 30000 : w);
    t->h = (i16)(h > 30000 ? 30000 : h);
    t->op_first = op_first;
    t->op_count = g_op_count - op_first;

    L->used += gap + w;
    L->space = 0;
}

static i32 layout_block(u32 index, i32 x, i32 y, i32 avail, u16 link, int shrink, i32* out_width, u32* out_box);
static void flow(u32 index, line_t* L, const run_t* r);

/* How wide the contents of an element would like to be, up to `limit`. */
static i32 measure(u32 index, i32 limit, u16 link) {
    if (g_measuring >= 3) {
        return limit;
    }

    u32 ops = g_op_count;
    u32 links = g_link_count;
    u32 link_text = g_link_text_len;
    u32 pool = g_tpool_len;
    i32 saved = g_measure_right;
    i32 width = 0;

    g_measuring++;
    g_measure_right = 0;

    g_new_context = 1;
    layout_block(index, 0, 0, limit, link, 0, &width, 0);

    i32 result = g_measure_right;

    g_measuring--;
    g_measure_right = saved;
    g_op_count = ops;
    g_link_count = links;
    g_link_text_len = link_text;
    g_tpool_len = pool;

    return result < limit ? result : limit;
}

/* The picture an <img> shows: its index + 1 in g_pictures, or 0. */
static u32 picture_for(const char* tag, u32 tag_len) {
    char src[URL_MAX];
    char full[URL_MAX];

    if (!attribute(tag, tag_len, "src", src, sizeof(src)) || !src[0] || starts_with(src, "data:")) {
        /* pages that load pictures by script often keep the address here */
        if (!attribute(tag, tag_len, "data-src", src, sizeof(src))) {
            return 0;
        }
    }

    if (!src[0] || starts_with(src, "data:")) {
        return 0;
    }

    full[0] = 0;
    url_resolve(g_url, src, full, sizeof(full));

    u32 len = (u32)strlen(full);

    /* SVG is a drawing language, not one of the formats we decode */
    if (!len || (len > 4u && starts_with(full + len - 4u, ".svg"))) {
        return 0;
    }

    for (u32 i = 0; i < g_picture_count; i++) {
        if (str_equal(g_picture_urls[i], full)) {
            return i + 1u;
        }
    }

    if (g_picture_count >= IMAGES_MAX) {
        return 0;
    }

    picture_t* p = &g_pictures[g_picture_count];

    p->state = IMAGE_PENDING;
    p->width = 0;
    p->height = 0;
    p->pixels = 0;
    str_copy(g_picture_urls[g_picture_count], full, URL_MAX);

    return ++g_picture_count;
}

static i32 attr_number(const node_t* n, const char* name) {
    char value[16];
    i32 v = 0;

    if (!attribute(g_src + n->src, n->src_len, name, value, sizeof(value))) {
        return 0;
    }

    for (u32 i = 0; value[i] >= '0' && value[i] <= '9'; i++) {
        v = v * 10 + (value[i] - '0');
    }

    return v;
}

/* Copies a short string into the text pool; returns its offset. */
static u32 pool_text(const char* text, u32 length) {
    u32 at = g_tpool_len;

    if (g_tpool_len + length + 1u >= TPOOL_MAX) {
        return 0;
    }

    for (u32 i = 0; i < length; i++) {
        g_tpool[g_tpool_len++] = text[i];
    }

    return at;
}

static void op_text(i32 x, i32 y, u8 face, u32 color, const char* text, u32 length, u32 node, u16 link) {
    op_t* o = op_new(OP_TEXT);

    o->face = face;
    o->x = x;
    o->y = y;
    o->w = text_measure(face, text, length);
    o->h = face_get(face)->height;
    o->color = color;
    o->text = pool_text(text, length);
    o->len = (u16)length;
    o->node = node;
    o->link = link;
}

static void op_frame(i32 x, i32 y, i32 w, i32 h, u32 color, u32 node) {
    op_rect(x, y, w, 1, color, node);
    op_rect(x, y + h - 1, w, 1, color, node);
    op_rect(x, y, 1, h, color, node);
    op_rect(x + w - 1, y, 1, h, color, node);
}

/* A picture, as a unit on the line. */
static void atom_image(line_t* L, u32 index, const run_t* r) {
    const node_t* n = &g_nodes[index];
    const style_t* s = &g_styles[index];
    u32 picture = picture_for(g_src + n->src, n->src_len);
    i32 w = s->width_unit == U_PX ? s->width : attr_number(n, "width");
    i32 h = s->height_unit == U_PX ? s->height : attr_number(n, "height");

    if (picture && g_pictures[picture - 1u].state == IMAGE_LOADED) {
        const picture_t* p = &g_pictures[picture - 1u];

        /* one side given: keep the picture's shape */
        if (w > 0 && h <= 0) h = (i32)(((i64)p->height * w) / (p->width ? p->width : 1u));
        else if (h > 0 && w <= 0) w = (i32)(((i64)p->width * h) / (p->height ? p->height : 1u));
        else if (w <= 0 && h <= 0) { w = (i32)p->width; h = (i32)p->height; }

        if (w > L->width && L->width > 16) {
            h = (i32)(((i64)h * L->width) / w);
            w = L->width;
        }

        if (w < 1) w = 1;
        if (h < 1) h = 1;

        u32 first = g_op_count;
        op_t* o = op_new(OP_IMAGE);

        o->w = w;
        o->h = h;
        o->text = picture - 1u;
        o->link = r->link;
        o->node = n->js ? n->js : r->node;

        add_atom(L, first, w, h);
        return;
    }

    /* not loaded (yet): the described text, greyed */
    char alt[96];

    alt[0] = 0;

    if (attribute(g_src + n->src, n->src_len, "alt", alt, sizeof(alt)) && alt[0]) {
        char clean[100];
        u32 k = text_convert(alt, (u32)strlen(alt), clean, sizeof(clean), 1);
        u32 at = pool_text(clean, k);
        style_t dim = *r->s;
        run_t rr;

        dim.color = 0x006C7C8Cu;
        dim.font_px = 13;
        dim.bold = 0;
        rr.s = &dim;
        rr.link = r->link;
        rr.node = r->node;

        if (k) {
            add_text_run(L, &rr, at, k);
        }
    }
}

/* Text boxes, check boxes and buttons made with <input>, and the stand-ins
   for <select> and <textarea>. */
static void atom_control(line_t* L, u32 index, const run_t* r) {
    const node_t* n = &g_nodes[index];
    const style_t* s = &g_styles[index];
    const char* tag = g_src + n->src;
    char type[24];
    char value[160];
    char clean[168];
    u8 face = face_for(14, 0, 0);
    const font_face_t* f = face_get(face);
    u32 node = n->js ? n->js : r->node;
    u32 first = g_op_count;
    i32 h = f->height + 6;

    if (!attribute(tag, n->src_len, "type", type, sizeof(type)) || !type[0]) {
        str_copy(type, "text", sizeof(type));
    }

    for (u32 i = 0; type[i]; i++) {
        type[i] = lower(type[i]);
    }

    if (str_equal(type, "hidden")) {
        return;
    }

    value[0] = 0;
    attribute(tag, n->src_len, "value", value, sizeof(value));

    u32 len = text_convert(value, (u32)strlen(value), clean, sizeof(clean) - 4u, 1);

    if (str_equal(type, "checkbox") || str_equal(type, "radio")) {
        op_rect(0, 3, 13, 13, 0x00FFFFFFu, node);
        op_frame(0, 3, 13, 13, 0x006E8AA0u, node);

        if (has_flag(tag, n->src_len, "checked")) {
            op_rect(3, 6, 7, 7, 0x001878C8u, node);
        }

        add_atom(L, first, 15, 17);
        return;
    }

    if (str_equal(type, "submit") || str_equal(type, "button") || str_equal(type, "reset") ||
        str_equal(type, "image")) {
        const char* label = len ? clean : (str_equal(type, "reset") ? "Reset"
                                        : (str_equal(type, "button") ? "Button" : "Submit"));
        u32 label_len = len ? len : (u32)strlen(label);
        i32 w = text_measure(face, label, label_len) + 18;

        op_rect(0, 0, w, h, 0x00E4F1FAu, node);
        op_frame(0, 0, w, h, 0x006E9AB8u, node);
        op_text(9, 3, face, 0x0016445Eu, label, label_len, node, 0);
        add_atom(L, first, w, h);
        return;
    }

    if (str_equal(type, "select")) {
        clean[len++] = ' ';
        clean[len++] = ' ';
        clean[len++] = 'v';

        i32 w = text_measure(face, clean, len) + 14;

        op_rect(0, 0, w, h, 0x00F4FAFDu, node);
        op_frame(0, 0, w, h, 0x006E9AB8u, node);
        op_text(7, 3, face, 0x0016445Eu, clean, len, node, 0);
        add_atom(L, first, w, h);
        return;
    }

    /* a text box */
    i32 size = attr_number(n, "size");
    i32 w = s->width_unit == U_PX && s->width > 20 ? s->width : (size >= 3 ? size : 20) * 8 + 10;
    int focus = has_flag(tag, n->src_len, "data-nqfocus");
    u32 color = 0x0016445Eu;

    if (w > L->width && L->width > 60) {
        w = L->width;
    }

    if (!len && !focus) {
        value[0] = 0;
        attribute(tag, n->src_len, "placeholder", value, sizeof(value));
        len = text_convert(value, (u32)strlen(value), clean, sizeof(clean) - 4u, 1);
        color = 0x00809AAAu;
    } else if (str_equal(type, "password")) {
        for (u32 i = 0; i < len; i++) {
            clean[i] = '*';
        }
    }

    if (focus) {
        clean[len++] = '|';
    }

    /* while typing, the end of a long text stays in view */
    u32 start = 0;

    while (start < len && text_measure(face, clean + start, len - start) > w - 10) {
        if (focus) {
            start++;
        } else {
            len--;
        }
    }

    op_rect(0, 0, w, h, 0x00FFFFFFu, node);
    op_frame(0, 0, w, h, focus ? 0x00FF9A1Eu : 0x007A9AB0u, node);

    if (len > start) {
        op_text(5, 3, face, color, clean + start, len - start, node, 0);
    }

    add_atom(L, first, w, h);
}

/* Starts a link for an <a href>; returns its index + 1 in g_links, or 0. */
static u16 link_for(const node_t* n) {
    char href[URL_MAX];
    char full[URL_MAX];

    if (!attribute(g_src + n->src, n->src_len, "href", href, sizeof(href)) || !href[0] ||
        g_link_count >= LINKS_MAX || starts_with(href, "javascript:") ||
        starts_with(href, "mailto:") || starts_with(href, "tel:")) {
        return 0;
    }

    /* the browser's own pages are not addresses on the site */
    if (starts_with(href, "about:")) {
        str_copy(full, href, sizeof(full));
    } else {
        url_resolve(g_url, href, full, sizeof(full));
    }

    u32 len = (u32)strlen(full);

    if (!len || g_link_text_len + len + 1u >= LINK_TEXT_MAX) {
        return 0;
    }

    g_links[g_link_count] = g_link_text_len;
    str_copy(g_link_text + g_link_text_len, full, len + 1u);
    g_link_text_len += len + 1u;

    return (u16)(++g_link_count);
}

static void line_start(line_t* L, i32 x, i32 y, i32 width, const style_t* s);

/*
 * A floated element: laid out at its content's width, pushed to the left
 * or right edge at the current height (or lower, if there is no room),
 * and recorded so that lines run round it.
 */
static void place_float(line_t* L, u32 c, const run_t* r) {
    const node_t* n = &g_nodes[c];
    const style_t* s = &g_styles[c];
    u32 first = g_op_count;
    i32 want;
    i32 height;

    if (g_float_count >= FLOATS_MAX) {
        return;
    }

    if (n->tagid == T_IMG || n->tagid == T_INPUT || n->tagid == T_SELECT || n->tagid == T_TEXTAREA) {
        /* a picture or control: made at 0,0 on a line of its own, then moved */
        static line_t one;
        u32 base = g_float_base;

        g_float_base = g_float_count;
        line_start(&one, 0, 0, L->width, s);
        one.align = ALIGN_LEFT;

        if (n->tagid == T_IMG) {
            atom_image(&one, c, r);
        } else {
            atom_control(&one, c, r);
        }

        want = one.used + s->margin[1] + s->margin[3];
        line_flush(&one);
        height = one.y;
        g_float_base = base;

        if (first == g_op_count) {
            return;     /* nothing to show */
        }
    } else {
        want = measure(c, L->width, r->link);
        height = -1;
    }

    if (want > L->width) {
        want = L->width;
    }

    i32 y = L->y + L->pending_margin;
    i32 left = L->x0;
    i32 right = L->x0 + L->width;

    if (s->clear) {
        y = float_clear_y(y, s->clear);
    }

    for (int tries = 0; tries < 32; tries++) {
        left = L->x0;
        right = L->x0 + L->width;
        float_strip(y, 20, &left, &right);

        i32 below = float_next_y(y);

        if (right - left >= want || below <= y) {
            break;
        }

        y = below;
    }

    i32 fx = s->floating == 2 ? right - want : left;

    if (fx < L->x0) {
        fx = L->x0;
    }

    if (height >= 0) {
        for (u32 k = first; k < g_op_count; k++) {
            g_ops[k].x += fx + s->margin[3];
            g_ops[k].y += y + s->margin[0];
        }
    } else {
        g_new_context = 1;
        height = layout_block(c, fx, y + s->margin[0], want, r->link, 0, 0, 0);
    }

    float_box_t* f = &g_floats[g_float_count++];

    f->x0 = fx;
    f->x1 = fx + want;
    f->y0 = y;
    f->y1 = y + s->margin[0] + height + s->margin[2];
    f->right = (u8)(s->floating == 2);

    if (g_measuring && f->x1 > g_measure_right) {
        g_measure_right = f->x1;
    }
}

/* ---- positioned elements ----
 *
 * position: absolute takes an element out of the page's flow and places
 * it by its top/left/right/bottom against the nearest ancestor that is
 * itself positioned (or the page). Such elements are noted where they are
 * met and laid out when that ancestor is finished, so they paint on top.
 * position: fixed is treated the same against the page: it does not stay
 * put when the page scrolls.
 */
#define PENDING_MAX 256u

typedef struct pending {
    u32 node;
    u32 container;
    i32 x, y, w;        /* where it would have gone in the flow */
    u16 link;
} pending_t;

static pending_t g_pending[PENDING_MAX];
static u32 g_pending_count = 0;

/* Elements that pages hide by squeezing or clipping them to nothing. */
static int hidden_trick(const style_t* s) {
    if (s->clipped) {
        return 1;
    }

    if (s->overflow_hidden && ((s->height_unit == U_PX && s->height <= 1) ||
                               (s->width_unit == U_PX && s->width <= 1))) {
        return 1;
    }

    return s->out_of_flow >= 2 && s->behind;
}

static void defer_positioned(u32 c, i32 x, i32 y, i32 w, u16 link) {
    if (g_measuring || g_pending_count >= PENDING_MAX) {
        return;
    }

    pending_t* p = &g_pending[g_pending_count++];
    u32 container = 0;

    if (g_styles[c].out_of_flow == 2) {
        for (u32 a = g_nodes[c].parent; a; a = g_nodes[a].parent) {
            if (g_styles[a].out_of_flow) {
                container = a;
                break;
            }
        }
    }

    p->node = c;
    p->container = container;
    p->x = x;
    p->y = y;
    p->w = w;
    p->link = link;
}

static i32 inset_px(const style_t* s, u32 side, i32 whole) {
    return (s->inset_pct & (1u << side)) ? (whole * s->inset[side]) / 100 : s->inset[side];
}

/* Lays out the positioned elements waiting for box `index`, whose padding
   box is at bx, by and bw wide, bh high. */
static void layout_pending(u32 index, i32 bx, i32 by, i32 bw, i32 bh) {
    for (u32 i = 0; i < g_pending_count; ) {
        if (g_pending[i].container != index) {
            i++;
            continue;
        }

        pending_t p = g_pending[i];

        g_pending[i] = g_pending[--g_pending_count];

        const style_t* s = &g_styles[p.node];
        int has_l = (s->inset_set & 8u) != 0;
        int has_r = (s->inset_set & 2u) != 0;
        int has_t = (s->inset_set & 1u) != 0;
        int has_b = (s->inset_set & 4u) != 0;
        i32 l = has_l ? inset_px(s, 3, bw) : 0;
        i32 r = has_r ? inset_px(s, 1, bw) : 0;
        i32 t = has_t ? inset_px(s, 0, bh) : 0;
        i32 b = has_b ? inset_px(s, 2, bh) : 0;
        i32 want;
        i32 x;

        if (has_l && has_r && s->width_unit == U_AUTO) {
            want = bw - l - r;
            x = bx + l;
        } else {
            want = measure(p.node, bw > 40 ? bw : (i32)g_w - SCROLL_W, p.link);
            x = has_l ? bx + l : (has_r ? bx + bw - r - want : p.x);
        }

        /* parked off the page, or squeezed to nothing: not meant to be seen */
        if (want < 4 || x + want <= 0 || x >= (i32)g_w - SCROLL_W || (has_t && by + t < -200)) {
            continue;
        }

        u32 first = g_op_count;
        i32 y = has_t ? by + t : (has_b ? 0 : p.y);

        g_new_context = 1;

        i32 h = layout_block(p.node, x, y + s->margin[0], want, p.link, 0, 0, 0);

        if (!has_t && has_b) {
            i32 down = by + bh - b - h - s->margin[2];

            for (u32 o = first; o < g_op_count; o++) {
                g_ops[o].y += down;
            }
        }
    }
}

static void gen_run(line_t* L, const run_t* r, u32 raw);

static int is_block_level(const style_t* s) {
    return s->display == D_BLOCK || s->display == D_LIST_ITEM || s->display == D_TABLE ||
           s->display == D_FLEX || s->display == D_ROW || s->display == D_ROW_GROUP ||
           s->display == D_CELL || s->display == D_GRID;
}

/*
 * Places the children of an element: inline ones on lines, block ones
 * below one another.
 */
static void flow(u32 index, line_t* L, const run_t* r) {
    for (u32 c = g_nodes[index].first; c; c = g_nodes[c].next) {
        const node_t* n = &g_nodes[c];

        if (n->type == N_TEXT) {
            run_t rr;

            rr.s = r->s;
            rr.link = r->link;
            rr.node = n->js ? n->js : r->node;
            add_text_run(L, &rr, n->src, n->src_len);
            continue;
        }

        const style_t* s = &g_styles[c];

        if (s->display == D_NONE || hidden_trick(s)) {
            continue;
        }

        if (s->out_of_flow >= 2) {
            defer_positioned(c, L->x0, L->y + L->pending_margin, L->width, r->link);
            continue;
        }

        run_t rr;

        rr.s = s;
        rr.link = r->link;
        rr.node = n->js ? n->js : r->node;

        if (n->tagid == T_A && n->has_href) {
            u16 link = link_for(n);

            if (link) {
                rr.link = link;
            }
        }

        if (s->floating) {
            place_float(L, c, &rr);
            continue;
        }

        if (n->tagid == T_BR) {
            if (!L->count) {
                L->y += L->pending_margin + face_get(face_for(r->s->font_px, 0, r->s->mono))->height;
                L->pending_margin = 0;
            }

            line_flush(L);
            continue;
        }

        if (n->tagid == T_IMG) {
            atom_image(L, c, &rr);
            continue;
        }

        if (n->tagid == T_INPUT || n->tagid == T_SELECT || n->tagid == T_TEXTAREA) {
            atom_control(L, c, &rr);
            continue;
        }

        if (is_block_level(s)) {
            line_flush(L);

            /* the gap between two blocks is the larger of their margins */
            i32 gap = L->pending_margin > s->margin[0] ? L->pending_margin : s->margin[0];

            L->y += gap;
            L->pending_margin = 0;

            if (s->clear) {
                L->y = float_clear_y(L->y, s->clear);
            }

            i32 bx = L->x0;
            i32 bw = L->width;

            /* tables and flex rows do not slide under floats: they sit beside them */
            if ((s->display == D_TABLE || s->display == D_FLEX) && g_float_count > g_float_base) {
                i32 left = bx;
                i32 right = bx + bw;

                float_strip(L->y, 40, &left, &right);

                if (right - left >= 160) {
                    bx = left;
                    bw = right - left;
                } else {
                    L->y = float_clear_y(L->y, 3);
                }
            }

            u32 block_ops = g_op_count;
            i32 h = layout_block(c, bx, L->y, bw, rr.link, 0, 0, 0);

            if (s->out_of_flow == 1 && s->inset_set && !g_measuring) {
                i32 dx = (s->inset_set & 8u) ? inset_px(s, 3, bw) : ((s->inset_set & 2u) ? -inset_px(s, 1, bw) : 0);
                i32 dy = (s->inset_set & 1u) ? inset_px(s, 0, 0) : ((s->inset_set & 4u) ? -inset_px(s, 2, 0) : 0);

                for (u32 o = block_ops; o < g_op_count && (dx || dy); o++) {
                    g_ops[o].x += dx;
                    g_ops[o].y += dy;
                }
            }

            L->y += h;
            L->pending_margin = s->margin[2];
            continue;
        }

        if (s->display == D_INLINE_BLOCK) {
            /* laid out on its own at 0,0, then placed on the line as a unit */
            i32 want = measure(c, L->width, rr.link);
            u32 first = g_op_count;
            i32 used = 0;
            i32 h = layout_block(c, 0, 0, want, rr.link, 0, &used, 0);

            add_atom(L, first, used + s->margin[1] + s->margin[3], h);
            continue;
        }

        /* inline: its children join this line, between its side spacing */
        i32 before = s->margin[3] + s->padding[3] + s->border[3];
        i32 after = s->margin[1] + s->padding[1] + s->border[1];

        if (before > 0) {
            L->lead += before;
        }

        /* list items shown in a row ("a b c") are separate words */
        if (n->tagid == T_LI) {
            L->space = 1;
        }

        gen_run(L, &rr, g_before[c]);
        flow(c, L, &rr);
        gen_run(L, &rr, g_after[c]);

        if (after > 0) {
            L->lead += after;
        }

        if (n->tagid == T_LI) {
            L->space = 1;
        }
    }
}

static void line_start(line_t* L, i32 x, i32 y, i32 width, const style_t* s) {
    L->x0 = x;
    L->width = width > 8 ? width : 8;
    L->y = y;
    L->used = 0;
    L->pending_margin = 0;
    L->align = s->align;
    L->white = s->white;
    L->line_h = s->line_h;
    L->space = 0;
    L->lead = 0;
    L->count = 0;
}

#define COLS_MAX 32u

static int is_row(u32 c) {
    return g_nodes[c].type == N_ELEMENT && g_styles[c].display == D_ROW;
}

/* The row of a table after `cur` (0: the first), looking inside row groups. */
static u32 table_next_row(u32 table, u32 cur) {
    u32 c = g_nodes[table].first;

    if (cur) {
        u32 parent = g_nodes[cur].parent;

        if (parent != table) {
            for (u32 r = g_nodes[cur].next; r; r = g_nodes[r].next) {
                if (is_row(r)) {
                    return r;
                }
            }

            c = g_nodes[parent].next;
        } else {
            c = g_nodes[cur].next;
        }
    }

    for (; c; c = g_nodes[c].next) {
        if (g_nodes[c].type != N_ELEMENT) {
            continue;
        }

        if (is_row(c)) {
            return c;
        }

        if (g_styles[c].display == D_ROW_GROUP) {
            for (u32 r = g_nodes[c].first; r; r = g_nodes[r].next) {
                if (is_row(r)) {
                    return r;
                }
            }
        }
    }

    return 0;
}

/* A table: columns sized to their contents, cells placed on a grid. */
static i32 layout_table(u32 index, i32 x, i32 y, i32 width, u16 link) {
    i32 pref[COLS_MAX];
    i32 col_x[COLS_MAX + 1u];
    u32 cols = 0;
    i32 top = y;

    /* a caption goes above the grid */
    for (u32 c = g_nodes[index].first; c; c = g_nodes[c].next) {
        if (g_nodes[c].type == N_ELEMENT && g_nodes[c].tagid == T_CAPTION &&
            g_styles[c].display != D_NONE) {
            y += layout_block(c, x, y, width, link, 0, 0, 0);
        }
    }

    for (u32 i = 0; i < COLS_MAX; i++) {
        pref[i] = 0;
    }

    /* how wide each column would like to be */
    for (u32 row = table_next_row(index, 0); row; row = table_next_row(index, row)) {
        u32 col = 0;

        for (u32 c = g_nodes[row].first; c && col < COLS_MAX; c = g_nodes[c].next) {
            if (g_nodes[c].type != N_ELEMENT || g_styles[c].display != D_CELL) {
                continue;
            }

            i32 span = attr_number(&g_nodes[c], "colspan");

            if (span < 1) span = 1;

            if (span == 1) {
                const style_t* s = &g_styles[c];
                i32 w = s->width_unit == U_PX ? s->width
                      : measure(c, width, link);

                if (w < 16) w = 16;
                if (w > pref[col]) pref[col] = w;
            }

            col += (u32)span;

            if (col > cols) {
                cols = col > COLS_MAX ? COLS_MAX : col;
            }
        }
    }

    if (!cols) {
        return y - top;
    }

    i32 total = 0;

    for (u32 i = 0; i < cols; i++) {
        if (pref[i] < 16) pref[i] = 16;
        total += pref[i];
    }

    /* too wide: shrink every column in proportion; with a set width: stretch */
    const style_t* ts = &g_styles[index];
    i32 target = total;

    if (total > width) {
        target = width;
    } else if (ts->width_unit == U_PX || ts->width_unit == U_PCT) {
        target = width;
    }

    col_x[0] = x;

    for (u32 i = 0; i < cols; i++) {
        i32 w = (i32)(((i64)pref[i] * target) / (total ? total : 1));

        col_x[i + 1u] = col_x[i] + (w < 8 ? 8 : w);
    }

    for (u32 row = table_next_row(index, 0); row; row = table_next_row(index, row)) {
        u32 col = 0;
        i32 row_h = 0;
        u32 boxes[COLS_MAX];
        u32 cells[COLS_MAX];
        u32 cell_count = 0;
        const style_t* rs = &g_styles[row];
        u32 row_bg = g_op_count;

        if (rs->bg != NO_COLOR) {
            op_rect(col_x[0], y, col_x[cols] - col_x[0], 1, rs->bg, 0);
        }

        for (u32 c = g_nodes[row].first; c && col < cols; c = g_nodes[c].next) {
            if (g_nodes[c].type != N_ELEMENT || g_styles[c].display != D_CELL) {
                continue;
            }

            i32 span = attr_number(&g_nodes[c], "colspan");

            if (span < 1) span = 1;

            u32 last = col + (u32)span > cols ? cols : col + (u32)span;
            u32 box = 0;
            i32 h = layout_block(c, col_x[col], y, col_x[last] - col_x[col], link, 0, 0, &box);

            if (h > row_h) row_h = h;

            if (cell_count < COLS_MAX) {
                boxes[cell_count] = box;
                cells[cell_count++] = c;
            }

            col = last;
        }

        /* every cell's background and frame reach the bottom of the row */
        for (u32 k = 0; k < cell_count; k++) {
            const style_t* cs = &g_styles[cells[k]];
            u32 b = boxes[k];

            if (!b) {
                continue;
            }

            b--;

            for (u32 o = b; o < b + 5u && o < g_op_count; o++) {
                op_t* op = &g_ops[o];

                if (op->type != OP_RECT || op->text != 0xB0C5u) {
                    break;
                }

                if (op->len == 0 || op->len == 2u || op->len == 4u) {
                    op->h = row_h;                          /* background, left, right */
                } else if (op->len == 3u) {
                    op->y = y + row_h - cs->border[2];      /* bottom edge */
                }
            }
        }

        if (rs->bg != NO_COLOR && row_bg < g_op_count) {
            g_ops[row_bg].h = row_h;
        }

        y += row_h;
    }

    if (g_measuring && col_x[cols] > g_measure_right) {
        g_measure_right = col_x[cols];
    }

    return y - top;
}

#define FLEX_ITEMS 48u

typedef struct flex_item {
    u32 node;
    i32 base;           /* width it starts from, margins included */
    i32 size;           /* width it ends up with */
    i32 height;
    u32 op_first, op_last;
    u32 box;
} flex_item_t;

/*
 * A flex row. Each child starts from its own width (set, or what its
 * contents need); spare room is shared out among those that grow, a
 * shortage among those that shrink; what is left is placed by
 * justify-content or swallowed by "auto" margins. Then the children are
 * lined up top, middle, bottom or stretched to the row's height.
 */
static i32 layout_flex_row(u32 index, i32 x, i32 y, i32 width, u16 link) {
    const style_t* fs = &g_styles[index];
    flex_item_t items[FLEX_ITEMS];
    u32 count = 0;
    i32 top = y;
    i32 gap = fs->gap > 0 ? fs->gap : 0;

    for (u32 c = g_nodes[index].first; c && count < FLEX_ITEMS; c = g_nodes[c].next) {
        if (g_nodes[c].type != N_ELEMENT) {
            continue;
        }

        const style_t* s = &g_styles[c];

        if (s->display == D_NONE || hidden_trick(s)) {
            continue;
        }

        if (s->out_of_flow >= 2) {
            defer_positioned(c, x, y, width, link);
            continue;
        }

        i32 edges = s->border[1] + s->border[3] + s->padding[1] + s->padding[3];
        i32 margins = s->margin[1] + s->margin[3];
        i32 base;

        if (s->basis_unit == U_PX && (s->basis > 0 || s->flex_grow)) {
            base = s->basis + edges + margins;
        } else if (s->basis_unit == U_PCT) {
            base = (width * s->basis) / 100;
        } else if (s->width_unit == U_PX) {
            base = s->width + (s->border_box ? 0 : edges) + margins;
        } else if (s->width_unit == U_PCT) {
            base = (width * s->width) / 100;
        } else {
            base = measure(c, width, link);
        }

        if (base > width) base = width;
        if (base < edges + margins) base = edges + margins;

        items[count].node = c;
        items[count].base = base;
        items[count].size = base;
        items[count].height = 0;
        items[count].box = 0;
        count++;
    }

    for (u32 i = 0; i < count; ) {
        /* the children that share this row */
        u32 j = i;
        i32 total = 0;

        while (j < count) {
            i32 add = items[j].base + (j > i ? gap : 0);

            if (fs->wrap && j > i && total + add > width) {
                break;
            }

            total += add;
            j++;
        }

        i32 spare = width - total;
        i32 grow = 0;
        i64 shrink = 0;
        u32 autos = 0;

        for (u32 k = i; k < j; k++) {
            const style_t* s = &g_styles[items[k].node];

            grow += s->flex_grow;
            shrink += (i64)s->flex_shrink * items[k].base;
            autos += ((s->margin_auto & 8u) ? 1u : 0u) + ((s->margin_auto & 2u) ? 1u : 0u);
        }

        if (spare > 0 && grow > 0) {
            for (u32 k = i; k < j; k++) {
                items[k].size = items[k].base + (spare * g_styles[items[k].node].flex_grow) / grow;
            }

            spare = 0;
        } else if (spare < 0 && shrink > 0) {
            for (u32 k = i; k < j; k++) {
                i64 part = ((i64)g_styles[items[k].node].flex_shrink * items[k].base * (-spare)) / shrink;
                i32 size = items[k].base - (i32)part;

                items[k].size = size < 24 ? 24 : size;
            }

            spare = 0;
        }

        if (spare < 0) {
            spare = 0;
        }

        i32 cx = x;
        i32 between = gap;
        u32 n = j - i;

        if (spare > 0 && !autos) {
            if (fs->justify == JUSTIFY_CENTER) cx += spare / 2;
            else if (fs->justify == JUSTIFY_END) cx += spare;
            else if (fs->justify == JUSTIFY_BETWEEN && n > 1u) between += spare / (i32)(n - 1u);
            else if (fs->justify == JUSTIFY_AROUND) { cx += spare / (i32)(n * 2u); between += spare / (i32)n; }
        }

        i32 row_h = 0;

        for (u32 k = i; k < j; k++) {
            const style_t* s = &g_styles[items[k].node];

            if (spare > 0 && autos && (s->margin_auto & 8u)) {
                cx += spare / (i32)autos;
            }

            items[k].op_first = g_op_count;
            g_new_context = 1;
            items[k].height = layout_block(items[k].node, cx, y + s->margin[0], items[k].size,
                                           link, 0, 0, &items[k].box) + s->margin[0] + s->margin[2];
            items[k].op_last = g_op_count;

            if (items[k].height > row_h) {
                row_h = items[k].height;
            }

            cx += items[k].size + between;

            if (spare > 0 && autos && (s->margin_auto & 2u)) {
                cx += spare / (i32)autos;
            }
        }

        for (u32 k = i; k < j; k++) {
            const style_t* s = &g_styles[items[k].node];
            i32 slack = row_h - items[k].height;

            if (slack <= 0) {
                continue;
            }

            if (fs->align_items == CROSS_CENTER || fs->align_items == CROSS_END) {
                i32 down = fs->align_items == CROSS_CENTER ? slack / 2 : slack;

                for (u32 o = items[k].op_first; o < items[k].op_last && o < g_op_count; o++) {
                    g_ops[o].y += down;
                }
            } else if (fs->align_items == CROSS_STRETCH && items[k].box && s->height_unit != U_PX) {
                /* the child's background and frame reach the bottom of the row */
                u32 b = items[k].box - 1u;

                for (u32 o = b; o < b + 5u && o < g_op_count; o++) {
                    op_t* op = &g_ops[o];

                    if (op->type != OP_RECT || op->text != 0xB0C5u) {
                        break;
                    }

                    if (op->len == 0 || op->len == 2u || op->len == 4u) {
                        op->h += slack;
                    } else if (op->len == 3u) {
                        op->y += slack;
                    }
                }
            }
        }

        y += row_h;

        if (j < count) {
            y += gap;
        }

        i = j;
    }

    if (g_measuring) {
        /* when measuring, a row is as wide as its children side by side */
        i32 total = 0;

        for (u32 k = 0; k < count; k++) {
            total += items[k].base + (k ? gap : 0);
        }

        if (fs->wrap && total > width) {
            total = width;
        }

        if (x + total > g_measure_right) {
            g_measure_right = x + total;
        }
    }

    return y - top;
}

/*
 * Text a page adds with ::before / ::after. `raw` is the CSS value: only
 * plain quoted strings are used, and only if the font has every character
 * (icon fonts and counters would come out as rubbish).
 */
static void gen_run(line_t* L, const run_t* r, u32 raw) {
    const char* v = g_var_pool + raw - 1u;
    char text[96];
    u32 n = 0;

    if (!raw) {
        return;
    }

    for (u32 i = 0; v[i]; ) {
        if (css_space(v[i])) {
            i++;
            continue;
        }

        if (v[i] != '"' && v[i] != '\'') {
            return;     /* none, attr(), counter(), url() ... */
        }

        char quote = v[i++];

        while (v[i] && v[i] != quote && n + 8u < sizeof(text)) {
            if (v[i] == '\\') {
                /* \2022 style: a character by its number */
                u32 code = 0;
                u32 digits = 0;

                i++;

                while (digits < 6u && css_hex(v[i]) >= 0) {
                    code = code * 16u + (u32)css_hex(v[i]);
                    i++;
                    digits++;
                }

                if (!digits) {
                    if (v[i]) text[n++] = v[i++];
                    continue;
                }

                if (v[i] == ' ') {
                    i++;
                }

                char found = 0;

                if ((code >= 32u && code < 127u) || (code >= 160u && code < 256u)) {
                    found = (char)code;
                } else {
                    for (u32 t = 0; t < sizeof(g_cp1252) / sizeof(g_cp1252[0]); t++) {
                        if (g_cp1252[t].unicode == code) found = (char)g_cp1252[t].code;
                    }
                }

                if (!found) {
                    return;
                }

                text[n++] = found;
            } else if ((u8)v[i] >= 0x80u) {
                /* a character written out: must be one the font has */
                char one[8];
                u32 len = 1;

                while (((u8)v[i + len] & 0xC0u) == 0x80u) {
                    len++;
                }

                u32 got = text_convert(v + i, len, one, sizeof(one), 0);

                if (got != 1u || one[0] == '?') {
                    return;
                }

                text[n++] = one[0];
                i += len;
            } else {
                text[n++] = v[i++];
            }
        }

        if (v[i] == quote) {
            i++;
        }
    }

    if (!n) {
        return;
    }

    /* spaces at the ends count: " | " is a separator */
    if (text[0] == ' ') {
        L->space = 1;
    }

    u32 at = pool_text(text, n);

    add_text_run(L, r, at, n);

    if (text[n - 1u] == ' ') {
        L->space = 1;
    }
}

/* Makes a box's background and frame `slack` pixels taller (see layout_block). */
static void stretch_box(u32 box, i32 slack) {
    if (!box || slack <= 0) {
        return;
    }

    for (u32 o = box - 1u; o < box + 5u && o < g_op_count; o++) {
        op_t* op = &g_ops[o];

        if (op->type != OP_RECT || op->text != 0xB0C5u) {
            break;
        }

        if (op->len == 0 || op->len == 2u || op->len == 4u || op->len == 5u) {
            op->h += slack;
        } else if (op->len == 3u) {
            op->y += slack;
        }
    }
}

#define GRID_COLS 16u

/*
 * Works out a grid's column widths from grid-template-columns: fixed
 * sizes, percentages, "fr" shares of what is left, repeat(n, ...), and
 * repeat(auto-fill, minmax(200px, 1fr)) (as many columns as fit).
 */
static u32 grid_columns(const style_t* s, i32 width, i32 gap, i32* out) {
    const char* v = g_var_pool + s->grid_cols - 1u;
    u32 vlen = (u32)strlen(v);
    i32 fixed[GRID_COLS];
    i32 share[GRID_COLS];
    u32 count = 0;
    u32 at = 0;

    for (;;) {
        const char* tok;
        u32 n = css_token(v, vlen, &at, &tok);
        u32 times = 1;

        if (!n || count >= GRID_COLS) {
            break;
        }

        /* repeat(count, track): take the count, then look at the track */
        if (n > 7u && css_is(tok, 7, "repeat(")) {
            const char* inner = tok + 7;
            u32 ilen = n - 8u;
            u32 comma = 0;

            while (comma < ilen && inner[comma] != ',') comma++;

            const char* track = inner + comma + 1u;
            u32 tlen = comma < ilen ? ilen - comma - 1u : 0;

            while (tlen && css_space(track[0])) { track++; tlen--; }

            if (css_has(inner, comma, "auto-f")) {
                /* as many as fit at the smallest size named */
                i32 least = 120;
                decl_t d;

                for (u32 k = 0; k + 2u < tlen; k++) {
                    if (track[k] >= '0' && track[k] <= '9') {
                        u32 e = k;

                        while (e < tlen && track[e] != ',' && track[e] != ')' && !css_space(track[e])) e++;

                        if (css_length(track + k, e - k, &d) && d.unit != U_NUM && d.unit != U_PCT) {
                            least = d.unit == U_PX ? d.v : (d.v * (i32)s->font_px) / 100;
                        }

                        break;
                    }
                }

                if (least < 40) least = 40;

                times = (u32)((width + gap) / (least + gap));
            } else {
                i32 number = 0;

                css_number(inner, comma, &number);
                times = (u32)(number / 1000);
            }

            if (times < 1u) times = 1u;

            tok = track;
            n = tlen;

            /* several tracks inside a repeat: use the first */
            for (u32 k = 0, depth = 0; k < n; k++) {
                if (tok[k] == '(') depth++;
                else if (tok[k] == ')') depth--;
                else if (css_space(tok[k]) && depth == 0) { n = k; break; }
            }
        }

        /* minmax(a, b): the upper size decides */
        if (n > 7u && css_is(tok, 7, "minmax(")) {
            u32 comma = 7;

            while (comma < n && tok[comma] != ',') comma++;

            if (comma + 1u < n) {
                tok += comma + 1u;
                n -= comma + 2u;

                while (n && css_space(tok[0])) { tok++; n--; }
            }
        }

        i32 f = 0;
        i32 sh = 0;
        decl_t d;
        i32 number = 0;
        u32 used = css_number(tok, n, &number);

        if (used && used + 2u == n && lower(tok[used]) == 'f' && lower(tok[used + 1u]) == 'r') {
            sh = number > 0 ? number : 1000;
        } else if (css_length(tok, n, &d) && d.unit != U_NUM && d.unit != U_AUTO) {
            f = d.unit == U_PX ? d.v : (d.unit == U_PCT ? (width * d.v) / 100 : (d.v * (i32)s->font_px) / 100);
        } else {
            sh = 1000;      /* auto, min-content, fit-content(...): an equal share */
        }

        for (u32 k = 0; k < times && count < GRID_COLS; k++) {
            fixed[count] = f;
            share[count] = sh;
            count++;
        }
    }

    if (!count) {
        return 0;
    }

    i32 left = width - gap * (i32)(count - 1u);
    i32 shares = 0;

    for (u32 k = 0; k < count; k++) {
        left -= fixed[k];
        shares += share[k];
    }

    if (left < 0) left = 0;

    for (u32 k = 0; k < count; k++) {
        i32 w = fixed[k] + (shares ? (i32)(((i64)left * share[k]) / shares) : 0);

        out[k] = w < 16 ? 16 : w;
    }

    return count;
}

/* A grid: children fill the columns left to right, row after row. */
static i32 layout_grid(u32 index, i32 x, i32 y, i32 width, u16 link) {
    const style_t* gs = &g_styles[index];
    i32 col_w[GRID_COLS];
    i32 gap = gs->gap > 0 ? gs->gap : 0;
    i32 row_gap = gs->row_gap > 0 ? gs->row_gap : 0;
    u32 cols = grid_columns(gs, width, gap, col_w);
    i32 top = y;
    u32 col = 0;
    i32 row_h = 0;
    u32 row_box[GRID_COLS];
    i32 row_item_h[GRID_COLS];
    u32 row_items = 0;

    if (!cols) {
        cols = 1;
        col_w[0] = width;
    }

    for (u32 c = g_nodes[index].first; ; c = g_nodes[c].next) {
        const style_t* s = c ? &g_styles[c] : 0;
        int usable = c && g_nodes[c].type == N_ELEMENT && s->display != D_NONE && !hidden_trick(s);

        if (usable && s->out_of_flow >= 2) {
            defer_positioned(c, x, y, width, link);
            usable = 0;
        }

        u32 span = usable ? (s->grid_span ? s->grid_span : 1u) : 0;

        if (span > cols) span = cols;

        /* the row is full, or there is nothing more: finish it */
        if (row_items && (!c || (usable && col + span > cols))) {
            for (u32 k = 0; k < row_items; k++) {
                stretch_box(row_box[k], row_h - row_item_h[k]);
            }

            y += row_h + row_gap;
            col = 0;
            row_h = 0;
            row_items = 0;
        }

        if (!c) {
            break;
        }

        if (!usable) {
            continue;
        }

        i32 cx = x;
        i32 w = 0;

        for (u32 k = 0; k < col; k++) cx += col_w[k] + gap;
        for (u32 k = col; k < col + span; k++) w += col_w[k] + (k > col ? gap : 0);

        u32 box = 0;

        g_new_context = 1;

        i32 h = layout_block(c, cx, y + s->margin[0], w, link, 0, 0, &box) + s->margin[0] + s->margin[2];

        if (h > row_h) row_h = h;

        if (row_items < GRID_COLS) {
            row_box[row_items] = box;
            row_item_h[row_items] = h;
            row_items++;
        }

        col += span;
    }

    if (y > top) {
        y -= row_gap;       /* no gap after the last row */
    }

    if (g_measuring && x + width > g_measure_right) {
        g_measure_right = x + width;
    }

    return y - top;
}

/* A flex column: children stacked, with the gap and sideways alignment asked for. */
static i32 layout_flex_column(u32 index, i32 x, i32 y, i32 width, u16 link) {
    const style_t* fs = &g_styles[index];
    i32 gap = fs->row_gap > 0 ? fs->row_gap : (fs->gap > 0 ? fs->gap : 0);
    i32 top = y;
    int any = 0;

    for (u32 c = g_nodes[index].first; c; c = g_nodes[c].next) {
        if (g_nodes[c].type != N_ELEMENT) {
            continue;
        }

        const style_t* s = &g_styles[c];

        if (s->display == D_NONE || hidden_trick(s)) {
            continue;
        }

        if (s->out_of_flow >= 2) {
            defer_positioned(c, x, y, width, link);
            continue;
        }

        i32 cx = x;
        i32 w = width;

        if (fs->align_items != CROSS_STRETCH) {
            i32 want = measure(c, width, link);

            if (want < width) {
                if (fs->align_items == CROSS_CENTER) cx = x + (width - want) / 2;
                else if (fs->align_items == CROSS_END) cx = x + width - want;

                w = want;
            }
        }

        if (any) {
            y += gap;
        }

        any = 1;
        y += s->margin[0];
        g_new_context = 1;
        y += layout_block(c, cx, y, w, link, 0, 0, 0) + s->margin[2];
    }

    return y - top;
}

/*
 * Lays out one block box whose margin edge starts at x, y, with `avail`
 * pixels to fill. Returns the height of its border box (margins are the
 * caller's business). shrink: be only as wide as the contents need.
 */
static i32 layout_block(u32 index, i32 x, i32 y, i32 avail, u16 link, int shrink,
                        i32* out_width, u32* out_box) {
    const node_t* n = &g_nodes[index];
    const style_t* s = &g_styles[index];
    i32 edge_l = s->border[3] + s->padding[3];
    i32 edge_r = s->border[1] + s->padding[1];
    i32 edges = edge_l + edge_r;
    i32 room = avail - s->margin[1] - s->margin[3];
    i32 content_w;
    int sized = 0;

    if (room < 8) {
        room = 8;
    }

    /*
     * display: table on something that has no rows (pages use it to make a
     * box only as wide as its contents): an ordinary block of that width.
     */
    int real_table = s->display == D_TABLE && table_next_row(index, 0) != 0;

    if (s->display == D_TABLE && !real_table && !g_measuring) {
        shrink = 1;
    }

    int own_floats = g_new_context || index == 0 || s->display == D_CELL || s->display == D_INLINE_BLOCK ||
                     s->floating || s->display == D_FLEX || s->display == D_GRID;
    u32 float_base_before = g_float_base;

    g_new_context = 0;

    if (s->width_unit == U_PX || s->width_unit == U_PCT) {
        content_w = s->width_unit == U_PX ? s->width : (room * s->width) / 100;

        if (s->border_box || s->width_unit == U_PCT || s->display == D_CELL) {
            content_w -= edges;
        }

        sized = 1;
    } else if (shrink && s->display != D_CELL) {
        content_w = measure(index, room, link) - edges - s->margin[1] - s->margin[3];

        if (content_w > room - edges) {
            content_w = room - edges;
        }

        sized = 1;
    } else {
        content_w = room - edges;
    }

    if (s->display == D_CELL) {
        content_w = avail - edges;
        sized = 0;
    }

    if (s->max_unit == U_PX || s->max_unit == U_PCT) {
        i32 max = s->max_unit == U_PCT ? (room * s->max_width) / 100 : s->max_width;

        if (s->border_box) max -= edges;

        if (max > 0 && content_w > max) {
            content_w = max;
            sized = 1;
        }
    }

    if (s->min_unit == U_PX && content_w < s->min_width && s->min_width <= room) {
        content_w = s->min_width;
    }

    if (content_w > room - edges && s->display != D_CELL) {
        content_w = room - edges;       /* never wider than the space there is */
    }

    if (content_w < 8) {
        content_w = 8;
    }

    i32 box_w = content_w + edges;
    i32 bx = x + s->margin[3];

    if (g_measuring) {
        /* only the size matters */
    } else if (sized && (s->margin_auto & 2u) && (s->margin_auto & 8u)) {
        bx = x + (avail - box_w) / 2;
    } else if ((sized && (s->margin_auto & 8u)) || s->floating == 2) {
        bx = x + avail - box_w - s->margin[1];
    }

    if (bx < x) {
        bx = x;
    }

    /*
     * Background and frame go in first so the contents paint over them;
     * their height is filled in at the end. They are tagged (text/len) so
     * a table can find and stretch them.
     */
    u32 box_ops = g_op_count;
    int has_box = (s->bg != NO_COLOR) || s->border[0] || s->border[1] || s->border[2] || s->border[3];

    if (has_box && !s->hidden) {
        u32 bg = s->bg != NO_COLOR ? s->bg : 0;

        for (u32 k = 0; k < 5u; k++) {
            op_t* o = op_new(OP_RECT);

            o->text = 0xB0C5u;
            o->len = (u16)k;
            o->node = n->js;
            o->x = bx;
            o->y = y;
            o->color = k == 0 ? bg : s->border_color;

            if (k == 0) { o->w = s->bg != NO_COLOR ? box_w : 0; }
            else if (k == 1u) { o->w = box_w; o->h = s->border[0]; }
            else if (k == 2u) { o->x = bx + box_w - s->border[1]; o->w = s->border[1]; }
            else if (k == 3u) { o->w = box_w; o->h = s->border[2]; }
            else { o->w = s->border[3]; }
        }

        if (s->radius >= 2u) {
            int framed = s->border[0] || s->border[1] || s->border[2] || s->border[3];
            op_t* outer = &g_ops[box_ops];

            for (u32 k = 1; k < 5u; k++) {
                g_ops[box_ops + k].w = 0;
            }

            outer->w = box_w;
            outer->face = s->radius;
            outer->color = framed ? s->border_color : bg;

            if (framed) {
                op_t* inner = op_new(OP_RECT);

                inner->text = 0xB0C5u;
                inner->len = 5;
                inner->node = n->js;
                inner->x = bx + s->border[3];
                inner->y = y + s->border[0];
                inner->w = box_w - s->border[1] - s->border[3];
                inner->face = (u8)(s->radius > 2u ? s->radius - 1u : s->radius);
                inner->color = s->bg != NO_COLOR ? s->bg : g_page_bg;
            } else if (s->bg == NO_COLOR) {
                outer->w = 0;
            }
        }

        if (out_box) {
            *out_box = box_ops + 1u;
        }
    } else if (out_box) {
        *out_box = 0;
    }

    i32 cx = bx + edge_l;
    i32 cy = y + s->border[0] + s->padding[0];
    i32 content_h;

    if (own_floats) {
        g_float_base = g_float_count;
    }
    i32 measure_outer = g_measure_right;

    g_measure_right = 0;

    int flex_text = 0;
    u32 content_ops = g_op_count;

    if (s->display == D_FLEX) {
        for (u32 c = n->first; c && !flex_text; c = g_nodes[c].next) {
            if (g_nodes[c].type != N_TEXT) {
                continue;
            }

            for (u32 k = 0; k < g_nodes[c].src_len; k++) {
                if (!css_space(g_tpool[g_nodes[c].src + k])) {
                    flex_text = 1;
                    break;
                }
            }
        }
    }

    if (real_table) {
        content_h = layout_table(index, cx, cy, content_w, link);
    } else if (s->display == D_FLEX && !s->flex_column && !flex_text) {
        content_h = layout_flex_row(index, cx, cy, content_w, link);
    } else if (s->display == D_FLEX && !flex_text) {
        content_h = layout_flex_column(index, cx, cy, content_w, link);
    } else if (s->display == D_GRID && s->grid_cols) {
        content_h = layout_grid(index, cx, cy, content_w, link);
    } else {
        line_t L;
        run_t r;

        line_start(&L, cx, cy, content_w, s);

        if (flex_text && s->justify == JUSTIFY_CENTER) {
            L.align = ALIGN_CENTER;
        } else if (flex_text && s->justify == JUSTIFY_END) {
            L.align = ALIGN_RIGHT;
        }

        r.s = s;
        r.link = link;
        r.node = n->js;

        if (n->tagid == T_A && n->has_href && !link) {
            r.link = link_for(n);
        }

        gen_run(&L, &r, index ? g_before[index] : 0u);
        flow(index, &L, &r);
        gen_run(&L, &r, index ? g_after[index] : 0u);
        line_flush(&L);
        content_h = L.y + L.pending_margin - cy;

        if (s->display == D_LIST_ITEM && !s->list_none && !s->hidden) {
            u8 face = face_for(s->font_px, 0, 0);
            const font_face_t* f = face_get(face);
            const node_t* p = &g_nodes[n->parent];

            if (p->tagid == T_OL) {
                char number[12];
                u32 value = (u32)attr_number(p, "start");
                u32 len = 0;
                char digits[10];
                u32 d = 0;

                if (!value) value = 1;

                for (u32 sib = n->prev; sib; sib = g_nodes[sib].prev) {
                    if (g_nodes[sib].type == N_ELEMENT && g_nodes[sib].tagid == T_LI) value++;
                }

                do {
                    digits[d++] = (char)('0' + value % 10u);
                    value /= 10u;
                } while (value && d < 9u);

                while (d) number[len++] = digits[--d];

                number[len++] = '.';
                op_text(cx - 6 - text_measure(face, number, len), cy, face, s->color, number, len, 0, 0);
            } else {
                i32 size = f->size >= 16 ? 5 : 4;

                op_rect(cx - 14, cy + f->ascent - size - 2, size, size, s->color, 0);
            }

            if (content_h < f->height) {
                content_h = f->height;
            }
        }
    }

    if (own_floats) {
        /* the box grows to hold its own floats, then forgets them */
        for (u32 i = g_float_base; i < g_float_count; i++) {
            if (g_floats[i].y1 - cy > content_h) {
                content_h = g_floats[i].y1 - cy;
            }
        }

        g_float_count = g_float_base;
        g_float_base = float_base_before;
    }

    if (s->height_unit == U_PX && s->height > content_h && s->display != D_TABLE) {
        /* text in a taller flex box: middle or bottom, if asked */
        if (flex_text && (s->align_items == CROSS_CENTER || s->align_items == CROSS_END)) {
            i32 down = s->height - content_h;

            if (s->align_items == CROSS_CENTER) {
                down /= 2;
            }

            for (u32 o = content_ops; o < g_op_count; o++) {
                g_ops[o].y += down;
            }
        }

        content_h = s->height;
    }

    i32 box_h = s->border[0] + s->padding[0] + content_h + s->padding[2] + s->border[2];

    if (has_box && !s->hidden && box_ops + 5u <= g_op_count) {
        g_ops[box_ops].h = box_h;

        if (box_ops + 5u < g_op_count && g_ops[box_ops + 5u].text == 0xB0C5u && g_ops[box_ops + 5u].len == 5u) {
            g_ops[box_ops + 5u].h = box_h - s->border[0] - s->border[2];
        }

        g_ops[box_ops + 2u].h = box_h;
        g_ops[box_ops + 3u].y = y + box_h - s->border[2];
        g_ops[box_ops + 4u].h = box_h;
    }

    /*
     * When measuring: a box with a set width counts whole; otherwise it
     * reaches as far as its contents did, plus its own right edge.
     */
    {
        i32 mine = sized ? bx + box_w
                 : (g_measure_right ? g_measure_right + edge_r : bx + edges);

        mine += s->margin[1];
        g_measure_right = mine > measure_outer ? mine : measure_outer;
    }

    if ((index == 0 || s->out_of_flow) && !g_measuring && g_pending_count) {
        layout_pending(index, bx + s->border[3], y + s->border[0],
                       box_w - s->border[1] - s->border[3], box_h - s->border[0] - s->border[2]);
    }

    if (out_width) {
        *out_width = box_w;
    }

    return box_h;
}

/* -------------------------------------------------------------------------- */
/* What browser.c calls                                                        */
/* -------------------------------------------------------------------------- */

static i32 page_width(void) {
    return (i32)g_w - SCROLL_W;
}

static void parse_html(const char* html, u32 length) {
    style_t root;

    g_view_w = page_width();

    build_tree(html, length);

    style_root(&root);
    g_styles[0] = root;
    g_styles[0].display = D_BLOCK;

    compute_styles(0, &root);

    /* a page without a <body> still gets the usual space round its edge */
    int has_body = 0;

    for (u32 i = 1; i < g_node_count && i < 200u; i++) {
        if (g_nodes[i].type == N_ELEMENT && g_nodes[i].tagid == T_BODY) {
            has_body = 1;
            break;
        }
    }

    if (!has_body) {
        g_styles[0].padding[0] = g_styles[0].padding[1] = 8;
        g_styles[0].padding[2] = g_styles[0].padding[3] = 8;
    }
}

/* Plain text: the whole file as one preformatted block. */
static void parse_plain(const char* text, u32 length) {
    static const char pre_tag[] = "pre";
    style_t root;

    g_view_w = page_width();
    tree_reset();
    g_src = pre_tag;

    u32 pre = node_new(0, N_ELEMENT);

    g_nodes[pre].tagid = T_PRE;
    g_nodes[pre].tag = css_hash("pre", 3, 1);
    g_nodes[pre].src = 0;
    g_nodes[pre].src_len = 3;

    if (length > TPOOL_MAX / 2u) {
        length = TPOOL_MAX / 2u;
    }

    u32 t = node_new(pre, N_TEXT);

    g_nodes[t].src = g_tpool_len;
    g_nodes[t].src_len = text_convert(text, length, g_tpool, TPOOL_MAX - 16u, 0);
    g_tpool_len += g_nodes[t].src_len;

    style_root(&root);
    g_styles[0] = root;
    compute_styles(0, &root);
    g_styles[pre].margin[3] = 8;
}

static u32 g_layout_pool = 0;       /* where page text ends and layout's own text begins */
static int g_layout_pool_set = 0;

static void layout(void) {
    /* text made during an earlier layout (numbers, labels) is dropped */
    if (g_layout_pool_set && g_layout_pool <= g_tpool_len) {
        g_tpool_len = g_layout_pool;
    }

    g_layout_pool = g_tpool_len;
    g_layout_pool_set = 1;

    g_op_count = 0;
    g_link_count = 0;
    g_link_text_len = 0;
    g_hover_link = 0;
    g_measuring = 0;
    g_pending_count = 0;
    g_float_count = 0;
    g_float_base = 0;
    g_new_context = 0;
    g_page_bg = 0x00FFFFFFu;

    if (g_node_count <= 1u) {
        g_doc_height = 0;
        clamp_scroll();
        return;
    }

    /* the page's own background: the body's, or failing that the root's */
    for (u32 i = 1; i < g_node_count && i < 40u; i++) {
        if (g_nodes[i].type == N_ELEMENT && (g_nodes[i].tagid == T_BODY || g_nodes[i].tagid == T_HTML) &&
            g_styles[i].bg != NO_COLOR) {
            g_page_bg = g_styles[i].bg;
        }
    }

    i32 h = layout_block(0, 0, 0, page_width(), 0, 0, 0, 0);

    g_doc_height = h + 24;
    clamp_scroll();
}

static void reset_items(void) {
    g_op_count = 0;
    g_link_count = 0;
    g_link_text_len = 0;
    g_hover_link = 0;
    g_layout_pool_set = 0;
}

static void reset_document(void) {
    reset_items();
    tree_reset();
    g_doc_height = 0;
    g_scroll = 0;
    g_title[0] = 0;
    g_picture_count = 0;
    g_image_pool_used = 0;
    g_html_len = 0;
    css_reset();
}

/*
 * Lays the page out again from new HTML in g_html (after a script changed
 * the document), staying at the same place in it.
 */
static void rerender(u32 length) {
    i32 scroll = g_scroll;

    reset_items();
    parse_html(g_html, length);
    layout();

    g_scroll = scroll;
    clamp_scroll();
}

/* A page of the browser's own: a heading and a sentence. */
static void show_message(const char* heading, const char* detail) {
    static char page[640];
    u32 n = 0;
    const char* parts[5] = { "<h2>", heading, "</h2><p>", detail, "</p>" };

    reset_document();

    for (u32 p = 0; p < 5u; p++) {
        for (const char* c = parts[p]; *c && n + 6u < sizeof(page); c++) {
            if ((p == 1u || p == 3u) && *c == '<') {
                page[n++] = '&'; page[n++] = 'l'; page[n++] = 't'; page[n++] = ';';
            } else {
                page[n++] = *c;
            }
        }
    }

    page[n] = 0;
    parse_html(page, n);
    layout();
}

static int engine_has_content(void) {
    for (u32 i = 0; i < g_op_count; i++) {
        if (g_ops[i].type != OP_RECT) {
            return 1;
        }
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
/* Painting                                                                    */
/* -------------------------------------------------------------------------- */

static void fill(i32 x, i32 y, i32 w, i32 h, u32 color, i32 top, i32 bottom, i32 right) {
    i32 x0 = x < 0 ? 0 : x;
    i32 y0 = y < top ? top : y;
    i32 x1 = x + w > right ? right : x + w;
    i32 y1 = y + h > bottom ? bottom : y + h;

    for (i32 yy = y0; yy < y1; yy++) {
        u32* row = g_pixels + (u32)yy * g_w;

        for (i32 xx = x0; xx < x1; xx++) {
            row[xx] = color;
        }
    }
}

/* A filled box with rounded corners of radius r. */
static void fill_round(i32 x, i32 y, i32 w, i32 h, u32 color, i32 r, i32 top, i32 bottom, i32 right) {
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;

    if (r < 2) {
        fill(x, y, w, h, color, top, bottom, right);
        return;
    }

    /* the straight middle, then each curved row */
    fill(x, y + r, w, h - 2 * r, color, top, bottom, right);

    for (i32 row = 0; row < r; row++) {
        i32 dy = r - row;           /* distance from the circle's centre line */
        i32 dx = r;

        /* the largest inset whose corner point is outside the circle */
        while (dx > 0 && (r - dx) * (r - dx) + dy * dy > r * r) {
            dx--;
        }

        i32 inset = r - dx;

        /* (r - inset)^2 + dy^2 <= r^2 : find where the arc is */
        inset = 0;

        while (inset < r && (r - inset) * (r - inset) + dy * dy > r * r + r) {
            inset++;
        }

        fill(x + inset, y + row, w - 2 * inset, 1, color, top, bottom, right);
        fill(x + inset, y + h - 1 - row, w - 2 * inset, 1, color, top, bottom, right);
    }
}

static void draw_text(const op_t* o, i32 x, i32 y, i32 top, i32 bottom, i32 right) {
    const font_face_t* f = face_get(o->face);
    const char* text = g_tpool + o->text;
    u32 cr = (o->color >> 16) & 0xFFu;
    u32 cg = (o->color >> 8) & 0xFFu;
    u32 cb = o->color & 0xFFu;
    i32 pen = x;

    for (u32 i = 0; i < o->len; i++) {
        u8 c = (u8)text[i];
        const font_glyph_t* g = &f->glyphs[c >= 32u ? c - 32u : 0u];

        if (g->w && pen + g->left < right && pen + g->left + g->w + 8 > 0) {
            const u8* data = g_font_pixels + g->offset;
            u32 stride = ((u32)g->w + 1u) / 2u;

            for (u32 gy = 0; gy < g->h; gy++) {
                i32 py = y + g->top + (i32)gy;

                if (py < top || py >= bottom) {
                    continue;
                }

                /* slanted text: rows lean right the higher they are */
                i32 lean = (o->flags & OPF_ITALIC) ? ((i32)f->ascent - (g->top + (i32)gy)) / 4 : 0;
                u32* row = g_pixels + (u32)py * g_w;
                const u8* src = data + gy * stride;

                for (u32 gx = 0; gx < g->w; gx++) {
                    u32 a = (gx & 1u) ? (src[gx / 2u] & 0x0Fu) : (src[gx / 2u] >> 4);

                    if (!a) {
                        continue;
                    }

                    i32 px = pen + g->left + (i32)gx + lean;

                    if (px < 0 || px >= right) {
                        continue;
                    }

                    if (a == 15u) {
                        row[px] = o->color;
                    } else {
                        u32 d = row[px];
                        u32 r = (((d >> 16) & 0xFFu) * (15u - a) + cr * a) / 15u;
                        u32 gg = (((d >> 8) & 0xFFu) * (15u - a) + cg * a) / 15u;
                        u32 b = ((d & 0xFFu) * (15u - a) + cb * a) / 15u;

                        row[px] = (r << 16) | (gg << 8) | b;
                    }
                }
            }
        }

        pen += g->advance;
    }
}

/* Draws the visible part of the page into the window's page area. */
static void engine_paint(void) {
    i32 top = TOOLBAR_H;
    i32 bottom = TOOLBAR_H + view_height();
    i32 right = page_width();

    fill(0, top, right, bottom - top, g_page_bg, top, bottom, right);

    for (u32 i = 0; i < g_op_count; i++) {
        const op_t* o = &g_ops[i];
        i32 y = TOOLBAR_H + o->y - g_scroll;

        if (y >= bottom || y + o->h <= top) {
            continue;
        }

        if (o->type == OP_RECT) {
            if (o->face) {
                fill_round(o->x, y, o->w, o->h, o->color, o->face, top, bottom, right);
            } else {
                fill(o->x, y, o->w, o->h, o->color, top, bottom, right);
            }
        } else if (o->type == OP_TEXT) {
            const font_face_t* f = face_get(o->face);

            if (o->bg != NO_COLOR) {
                fill(o->x - o->space, y, o->w + o->space, o->h, o->bg, top, bottom, right);
            }

            draw_text(o, o->x, y, top, bottom, right);

            if (o->flags & OPF_UNDERLINE) {
                u32 color = (o->link && o->link == (u16)g_hover_link) ? 0x00E06000u : o->color;

                fill(o->x - o->space, y + f->ascent + 1, o->w + o->space, f->size >= 21 ? 2 : 1,
                     color, top, bottom, right);
            }
        } else if (o->type == OP_IMAGE && o->text < g_picture_count) {
            const picture_t* p = &g_pictures[o->text];
            const u32* src = g_image_pool + p->pixels;

            if (p->state != IMAGE_LOADED || !p->width || !p->height || o->w <= 0 || o->h <= 0) {
                continue;
            }

            for (i32 row = 0; row < o->h; row++) {
                i32 py = y + row;

                if (py < top || py >= bottom) {
                    continue;
                }

                u32 sy = (u32)(((i64)row * p->height) / o->h);
                u32* dst = g_pixels + (u32)py * g_w;

                for (i32 col = 0; col < o->w; col++) {
                    i32 px = o->x + col;

                    if (px >= 0 && px < right) {
                        dst[px] = src[sy * p->width + (u32)(((i64)col * p->width) / o->w)];
                    }
                }
            }
        }
    }
}

/* The display item under a point of the window, or -1. Later items are on top. */
static int op_at(i32 x, i32 y) {
    if (y < TOOLBAR_H || y >= (i32)g_h - STATUS_H || x >= page_width()) {
        return -1;
    }

    i32 dy = y - TOOLBAR_H + g_scroll;

    for (u32 i = g_op_count; i > 0; i--) {
        const op_t* o = &g_ops[i - 1u];

        if (o->type == OP_RECT && !o->node) {
            continue;
        }

        i32 left = o->type == OP_TEXT ? o->x - o->space : o->x;

        if (x >= left && x < o->x + o->w && dy >= o->y && dy < o->y + o->h) {
            /* backgrounds of ordinary boxes are not things to click */
            if (o->type == OP_RECT && o->text == 0xB0C5u) {
                continue;
            }

            return (int)(i - 1u);
        }
    }

    return -1;
}

static int link_at(i32 x, i32 y) {
    int o = op_at(x, y);

    return o >= 0 ? g_ops[o].link : 0;
}

static u32 node_at(i32 x, i32 y) {
    int o = op_at(x, y);

    return o >= 0 ? g_ops[o].node : 0u;
}
