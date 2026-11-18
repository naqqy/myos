/*
 * NAQ OS Browser: styles (CSS).
 *
 * Part of BROWSER.ELF; engine.c includes this file.
 *
 * A style sheet is a list of rules: "these elements get these
 * properties". This file reads sheets into rules, and for every element
 * of the page works out its final set of properties (the "computed
 * style") from all the rules that match it, the element's own style=""
 * attribute, what it inherits from its parent, and the browser's built-in
 * defaults for each kind of element.
 *
 * What is understood:
 *   selectors    tag, .class, #id, [attr] with = ~= |= ^= $= *=, combined,
 *                with descendant (space), child (>), next (+) and later (~)
 *                sibling relations, comma lists, :first-child, :last-child,
 *                :only-child, :nth-child(), :first/last-of-type, :not() with
 *                one simple selector inside, :link, :root
 *   at-rules     @media (screen, widths, colour scheme), @supports, @layer
 *   values       px em rem % pt vw vh, colours by name, #hex, rgb(), hsl(),
 *                var(--name, fallback) for variables set on :root/html/body
 *   properties   display, colours, fonts, text alignment and decoration,
 *                margin, padding, border, width/height limits, float,
 *                position (only to leave out floating panels), and a few more
 *
 * Rules that use anything else in their selector (:hover, ::before ...)
 * are dropped whole, which is the safe choice: half-applying them would
 * style the wrong elements.
 */

#define RULES_MAX      16000u
#define DECLS_MAX      120000u
#define CHAIN_MAX      5u
#define BUCKETS        2048u
#define VAR_POOL_MAX   (768u * 1024u)
#define VARS_MAX       1024u
#define MATCH_MAX      96u
#define TEMP_DECLS     640u

enum {
    D_NONE, D_BLOCK, D_INLINE, D_INLINE_BLOCK, D_LIST_ITEM, D_TABLE,
    D_ROW_GROUP, D_ROW, D_CELL, D_FLEX, D_GRID
};

enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT };
enum { WHITE_NORMAL, WHITE_NOWRAP, WHITE_PRE };
enum { JUSTIFY_START, JUSTIFY_CENTER, JUSTIFY_END, JUSTIFY_BETWEEN, JUSTIFY_AROUND };
enum { CROSS_STRETCH, CROSS_CENTER, CROSS_END, CROSS_START };

enum { U_PX, U_EM, U_REM, U_PCT, U_AUTO, U_NUM, U_NONE };

enum {
    P_DISPLAY, P_COLOR, P_BG, P_FONT_SIZE, P_BOLD, P_ITALIC, P_UNDERLINE,
    P_ALIGN, P_WHITE, P_LIST, P_VISIBILITY, P_MONO, P_UPPER, P_BOX_SIZING,
    P_POSITION, P_FLOAT, P_FLEX_DIR, P_LINE_H,
    P_MARGIN_T, P_MARGIN_R, P_MARGIN_B, P_MARGIN_L,
    P_PADDING_T, P_PADDING_R, P_PADDING_B, P_PADDING_L,
    P_BORDER_T, P_BORDER_R, P_BORDER_B, P_BORDER_L, P_BORDER_COLOR,
    P_WIDTH, P_MAX_W, P_MIN_W, P_HEIGHT,
    P_CLEAR, P_FLEX_GROW, P_FLEX_SHRINK, P_FLEX_BASIS, P_JUSTIFY, P_ALIGN_ITEMS,
    P_WRAP, P_GAP,
    P_TOP, P_RIGHT, P_BOTTOM, P_LEFT, P_ZINDEX, P_CLIPPED, P_OVERFLOW,
    P_GRID_COLS, P_GRID_SPAN, P_ROW_GAP, P_RADIUS, P_CONTENT,
    P_DEFERRED      /* contains var(): worked out when it is applied */
};

#define NO_COLOR 0xFF000000u    /* "transparent" / not set */

typedef struct style {
    u8 display, bold, italic, underline, align, white, list_none, hidden;
    u8 mono, upper, border_box, floating, flex_column;
    u8 out_of_flow;         /* position: 0 normal, 1 relative, 2 absolute, 3 fixed */
    u8 margin_auto;         /* bit 1: right, bit 3: left */
    u8 width_unit, max_unit, min_unit, height_unit;
    u8 clear;               /* bit 0: below left floats, bit 1: below right floats */
    u8 flex_grow, flex_shrink, basis_unit;
    u8 justify, align_items, wrap;
    u8 inset_set;           /* which of top/right/bottom/left are given (bits 0..3) */
    u8 inset_pct;           /* ... and which of those are percentages */
    u8 behind;              /* z-index below zero */
    u8 clipped;             /* cut down to nothing (clip, opacity 0): not shown */
    u8 overflow_hidden;
    i16 inset[4];
    u8 radius;              /* rounded corners, px (255: as round as possible) */
    u8 grid_span;           /* columns a grid item covers (0 = 1; 255 = the whole row) */
    i16 row_gap;
    u32 grid_cols;          /* grid-template-columns: offset + 1 of its text in g_var_pool */
    i16 gap;
    i32 basis;
    u8 border[4];           /* top, right, bottom, left widths */
    u16 font_px;
    i16 line_h;             /* px; 0 = normal */
    i16 margin[4];
    i16 padding[4];
    u32 color;
    u32 bg;                 /* NO_COLOR = none */
    u32 border_color;
    i32 width, max_width, min_width, height;
} style_t;

typedef struct decl {
    u8 prop;
    u8 unit;
    u8 important;
    u8 pad;
    i32 v;
} decl_t;

#define PSEUDO_FIRST      1u
#define PSEUDO_LAST       2u
#define PSEUDO_LINK       4u
#define PSEUDO_ROOT       8u
#define PSEUDO_FIRST_TYPE 16u
#define PSEUDO_LAST_TYPE  32u

/* how a compound relates to the next one in the stored chain */
#define REL_ANCESTOR 0u
#define REL_PARENT   1u
#define REL_PREVIOUS 2u     /* a + b: b comes straight after a */
#define REL_EARLIER  3u     /* a ~ b: b comes somewhere after a */

#define NOT_NONE  0u
#define NOT_TAG   1u
#define NOT_ID    2u
#define NOT_CLASS 3u
#define NOT_ATTR  4u

typedef struct compound {
    u32 tag;                /* 0 = any */
    u32 id;
    u32 cls[4];
    u32 attr_name[2];
    u32 attr_value[2];      /* '=': hash of the value; other tests: its text in g_var_pool */
    u32 not_hash;           /* :not(x): what must not match */
    u8 attr_op[2];          /* 0 present, '=', '~', '|', '^', '$', '*' */
    u8 class_count;
    u8 attr_count;
    u8 pseudo;
    u8 child;               /* REL_...: how the next compound in the chain relates */
    u8 not_kind;
    u8 nth;                 /* 0 none, 1 :nth-child, 2 :nth-last-child */
    u8 pe;                  /* 0 the element, 1 its ::before, 2 its ::after */
    i8 nth_a, nth_b;        /* position must be a*n + b */
} compound_t;

typedef struct rule {
    compound_t c[CHAIN_MAX];    /* c[0] is the element itself, then its ancestors */
    u8 count;
    u8 pe;                      /* which part of the element it styles: see compound_t */
    u32 specificity;
    u32 decl_start;
    u32 decl_count;
    i32 next;                   /* next rule in the same bucket, or -1 */
} rule_t;

static rule_t g_rules[RULES_MAX];
static u32 g_rule_count = 0;
static decl_t g_decls[DECLS_MAX];
static u32 g_decl_count = 0;
static i32 g_buckets[BUCKETS];
static i32 g_universal = -1;

/* the same again for ::before / ::after rules, which are looked up apart */
static i32 g_buckets_pe[BUCKETS];
static i32 g_universal_pe = -1;
static u32 g_pe_rules = 0;

/* Text kept from the sheets: deferred declarations ("name\0value\0") and
   the values of variables. */
static char g_var_pool[VAR_POOL_MAX];
static u32 g_var_pool_len = 0;

static struct {
    u32 name;
    u32 value;      /* offset into g_var_pool */
} g_vars[VARS_MAX];
static u32 g_var_count = 0;

static u32 g_css_dropped = 0;

static i32 g_view_w = 760;      /* page width, for vw and percentages */

/* -------------------------------------------------------------------------- */
/* Small helpers                                                               */
/* -------------------------------------------------------------------------- */

static int css_space(char c) {
    return c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '\f';
}

static int css_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || (u8)c >= 0x80u || c == '\\';
}

static u32 css_hash(const char* text, u32 length, int fold) {
    u32 h = 2166136261u;

    for (u32 i = 0; i < length; i++) {
        h ^= (u8)(fold ? lower(text[i]) : text[i]);
        h *= 16777619u;
    }

    return h ? h : 1u;
}

/* Does text[0..length) equal `word`, ignoring case? */
static int css_is(const char* text, u32 length, const char* word) {
    u32 i = 0;

    for (; i < length && word[i]; i++) {
        if (lower(text[i]) != word[i]) {
            return 0;
        }
    }

    return i == length && !word[i];
}

static int css_has(const char* text, u32 length, const char* word) {
    u32 n = (u32)strlen(word);

    for (u32 i = 0; i + n <= length; i++) {
        u32 k = 0;

        while (k < n && lower(text[i + k]) == word[k]) {
            k++;
        }

        if (k == n) {
            return 1;
        }
    }

    return 0;
}

static void css_reset(void) {
    g_rule_count = 0;
    g_decl_count = 0;
    g_var_pool_len = 0;
    g_var_count = 0;
    g_css_dropped = 0;
    g_universal = -1;
    g_universal_pe = -1;
    g_pe_rules = 0;

    for (u32 i = 0; i < BUCKETS; i++) {
        g_buckets[i] = -1;
        g_buckets_pe[i] = -1;
    }
}

/* -------------------------------------------------------------------------- */
/* Values                                                                      */
/* -------------------------------------------------------------------------- */

static int css_hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static const struct {
    const char* name;
    u32 color;
} g_color_names[] = {
    { "black", 0x000000 }, { "white", 0xFFFFFF }, { "red", 0xFF0000 }, { "green", 0x008000 },
    { "blue", 0x0000FF }, { "gray", 0x808080 }, { "grey", 0x808080 }, { "silver", 0xC0C0C0 },
    { "maroon", 0x800000 }, { "navy", 0x000080 }, { "teal", 0x008080 }, { "olive", 0x808000 },
    { "purple", 0x800080 }, { "yellow", 0xFFFF00 }, { "orange", 0xFFA500 }, { "lime", 0x00FF00 },
    { "aqua", 0x00FFFF }, { "cyan", 0x00FFFF }, { "fuchsia", 0xFF00FF }, { "magenta", 0xFF00FF },
    { "pink", 0xFFC0CB }, { "brown", 0xA52A2A }, { "gold", 0xFFD700 }, { "lightgray", 0xD3D3D3 },
    { "lightgrey", 0xD3D3D3 }, { "darkgray", 0xA9A9A9 }, { "darkgrey", 0xA9A9A9 },
    { "dimgray", 0x696969 }, { "dimgrey", 0x696969 }, { "whitesmoke", 0xF5F5F5 },
    { "gainsboro", 0xDCDCDC }, { "darkblue", 0x00008B }, { "darkred", 0x8B0000 },
    { "darkgreen", 0x006400 }, { "crimson", 0xDC143C }, { "tomato", 0xFF6347 },
    { "coral", 0xFF7F50 }, { "salmon", 0xFA8072 }, { "khaki", 0xF0E68C }, { "indigo", 0x4B0082 },
    { "violet", 0xEE82EE }, { "beige", 0xF5F5DC }, { "ivory", 0xFFFFF0 }, { "lightblue", 0xADD8E6 },
    { "skyblue", 0x87CEEB }, { "steelblue", 0x4682B4 }, { "royalblue", 0x4169E1 },
    { "slategray", 0x708090 }, { "slategrey", 0x708090 }, { "tan", 0xD2B48C },
    { "firebrick", 0xB22222 }, { "orangered", 0xFF4500 }, { "seagreen", 0x2E8B57 },
    { "forestgreen", 0x228B22 }, { "lavender", 0xE6E6FA }, { "lightyellow", 0xFFFFE0 },
    { "lightgreen", 0x90EE90 }, { "darkorange", 0xFF8C00 }, { "dodgerblue", 0x1E90FF },
    { "aliceblue", 0xF0F8FF }, { "ghostwhite", 0xF8F8FF }, { "snow", 0xFFFAFA },
    { "midnightblue", 0x191970 }, { "darkslategray", 0x2F4F4F }, { "lightslategray", 0x778899 },
};

/* Reads a number like "12", "1.5" or "-.25" as thousandths. Returns chars used. */
static u32 css_number(const char* v, u32 length, i32* out) {
    u32 i = 0;
    int negative = 0;
    i64 whole = 0;
    i64 frac = 0;
    i64 scale = 1;
    int digits = 0;

    if (i < length && (v[i] == '-' || v[i] == '+')) {
        negative = v[i] == '-';
        i++;
    }

    while (i < length && v[i] >= '0' && v[i] <= '9') {
        if (whole < 1000000) {
            whole = whole * 10 + (v[i] - '0');
        }

        i++;
        digits++;
    }

    if (i < length && v[i] == '.') {
        i++;

        while (i < length && v[i] >= '0' && v[i] <= '9') {
            if (scale < 1000) {
                frac = frac * 10 + (v[i] - '0');
                scale *= 10;
            }

            i++;
            digits++;
        }
    }

    if (!digits) {
        return 0;
    }

    i64 value = whole * 1000 + (frac * 1000) / scale;

    *out = (i32)(negative ? -value : value);

    return i;
}

static u32 hsl_part(i32 p, i32 q, i32 h) {
    /* h in 0..359; p, q in 0..255000 */
    if (h < 0) h += 360;
    if (h >= 360) h -= 360;

    i64 v;

    if (h < 60) v = p + ((i64)(q - p) * h) / 60;
    else if (h < 180) v = q;
    else if (h < 240) v = p + ((i64)(q - p) * (240 - h)) / 60;
    else v = p;

    v /= 1000;

    return (u32)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

/*
 * A colour. Returns 1 and the colour, 2 for "transparent" (or fully
 * see-through), 0 if this is not a colour.
 */
static int css_color(const char* v, u32 length, u32* out) {
    while (length && css_space(v[0])) { v++; length--; }
    while (length && css_space(v[length - 1u])) { length--; }

    if (!length) {
        return 0;
    }

    if (v[0] == '#') {
        int d[8];
        u32 n = length - 1u;

        if (n != 3u && n != 4u && n != 6u && n != 8u) {
            return 0;
        }

        for (u32 i = 0; i < n; i++) {
            d[i] = css_hex(v[1u + i]);

            if (d[i] < 0) {
                return 0;
            }
        }

        if (n <= 4u) {
            *out = ((u32)(d[0] * 17) << 16) | ((u32)(d[1] * 17) << 8) | (u32)(d[2] * 17);
            return (n == 4u && d[3] == 0) ? 2 : 1;
        }

        *out = ((u32)(d[0] * 16 + d[1]) << 16) | ((u32)(d[2] * 16 + d[3]) << 8) | (u32)(d[4] * 16 + d[5]);
        return (n == 8u && d[6] == 0 && d[7] == 0) ? 2 : 1;
    }

    int is_rgb = length > 4u && lower(v[0]) == 'r' && lower(v[1]) == 'g' && lower(v[2]) == 'b';
    int is_hsl = length > 4u && lower(v[0]) == 'h' && lower(v[1]) == 's' && lower(v[2]) == 'l';

    if (is_rgb || is_hsl) {
        i32 part[4] = { 0, 0, 0, 1000 };
        int percent[4] = { 0, 0, 0, 0 };
        u32 count = 0;
        u32 i = 3;

        while (i < length && v[i] != '(') {
            i++;
        }

        i++;

        while (i < length && count < 4u) {
            while (i < length && (css_space(v[i]) || v[i] == ',' || v[i] == '/')) {
                i++;
            }

            u32 used = css_number(v + i, length - i, &part[count]);

            if (!used) {
                break;
            }

            i += used;

            if (i < length && v[i] == '%') {
                percent[count] = 1;
                i++;
            }

            while (i < length && css_ident_char(v[i])) {
                i++;        /* "deg" and the like */
            }

            count++;
        }

        if (count < 3u) {
            return 0;
        }

        if (count == 4u && part[3] == 0) {
            return 2;
        }

        if (is_rgb) {
            u32 c[3];

            for (int k = 0; k < 3; k++) {
                i32 value = percent[k] ? (part[k] * 255) / 100000 : part[k] / 1000;

                c[k] = (u32)(value < 0 ? 0 : (value > 255 ? 255 : value));
            }

            *out = (c[0] << 16) | (c[1] << 8) | c[2];
            return 1;
        }

        /* hue in degrees, saturation and lightness in percent */
        i32 h = (part[0] / 1000) % 360;
        i32 s = part[1] / 100;      /* 0..1000 */
        i32 l = part[2] / 100;

        if (s > 1000) s = 1000;
        if (l > 1000) l = 1000;

        i32 q = l < 500 ? (l * (1000 + s)) / 1000 : l + s - (l * s) / 1000;
        i32 p = 2 * l - q;

        q = q * 255;        /* now 0..255000 */
        p = p * 255;

        *out = (hsl_part(p, q, h + 120) << 16) | (hsl_part(p, q, h) << 8) | hsl_part(p, q, h - 120);
        return 1;
    }

    if (css_is(v, length, "transparent")) {
        return 2;
    }

    for (u32 i = 0; i < sizeof(g_color_names) / sizeof(g_color_names[0]); i++) {
        if (css_is(v, length, g_color_names[i].name)) {
            *out = g_color_names[i].color;
            return 1;
        }
    }

    return 0;
}

/*
 * A length: "12px", "1.5em", "50%", "auto", "0". Fills a declaration's
 * unit and value (px whole; em/rem in hundredths; percent whole).
 */
static int css_length(const char* v, u32 length, decl_t* d) {
    i32 n = 0;

    while (length && css_space(v[0])) { v++; length--; }
    while (length && css_space(v[length - 1u])) { length--; }

    if (css_is(v, length, "auto")) {
        d->unit = U_AUTO;
        d->v = 0;
        return 1;
    }

    u32 used = css_number(v, length, &n);

    if (!used) {
        return 0;
    }

    const char* unit = v + used;
    u32 ulen = length - used;

    if (ulen == 0) {
        /* a bare number is only a length when it is 0 */
        d->unit = n == 0 ? U_PX : U_NUM;
        d->v = n == 0 ? 0 : n / 10;
        return 1;
    }

    if (css_is(unit, ulen, "px")) {
        d->unit = U_PX;
        d->v = (n + (n >= 0 ? 500 : -500)) / 1000;

        /* a hairline is still a line */
        if (d->v == 0 && n > 0) {
            d->v = 1;
        }
    } else if (css_is(unit, ulen, "em")) {
        d->unit = U_EM;
        d->v = n / 10;
    } else if (css_is(unit, ulen, "rem")) {
        d->unit = U_REM;
        d->v = n / 10;
    } else if (css_is(unit, ulen, "%")) {
        d->unit = U_PCT;
        d->v = n / 1000;
    } else if (css_is(unit, ulen, "pt")) {
        d->unit = U_PX;
        d->v = (n * 4) / 3000;
    } else if (css_is(unit, ulen, "vw")) {
        d->unit = U_PX;
        d->v = (i32)(((i64)n * g_view_w) / 100000);
    } else if (css_is(unit, ulen, "vh")) {
        d->unit = U_PX;
        d->v = (i32)(((i64)n * 500) / 100000);
    } else if (css_is(unit, ulen, "ch") || css_is(unit, ulen, "ex")) {
        d->unit = U_EM;
        d->v = n / 20;
    } else {
        return 0;
    }

    return 1;
}

/* -------------------------------------------------------------------------- */
/* Declarations                                                                */
/* -------------------------------------------------------------------------- */

typedef struct decl_out {
    decl_t* list;
    u32 count;
    u32 max;
} decl_out_t;

static void put_decl(decl_out_t* out, u32 prop, u32 unit, i32 v, int important) {
    if (out->count < out->max) {
        decl_t* d = &out->list[out->count++];

        d->prop = (u8)prop;
        d->unit = (u8)unit;
        d->important = (u8)important;
        d->pad = 0;
        d->v = v;
    }
}

/* The next space-separated piece of a value; brackets keep things together. */
static u32 css_token(const char* v, u32 length, u32* at, const char** token) {
    u32 i = *at;

    while (i < length && css_space(v[i])) {
        i++;
    }

    u32 start = i;
    int depth = 0;

    while (i < length && (depth > 0 || !css_space(v[i]))) {
        if (v[i] == '(') depth++;
        if (v[i] == ')') depth--;
        i++;
    }

    *token = v + start;
    *at = i;

    return i - start;
}

static void put_length(decl_out_t* out, u32 prop, const char* v, u32 length, int important) {
    decl_t d;

    if (css_length(v, length, &d) && d.unit != U_NUM) {
        put_decl(out, prop, d.unit, d.v, important);
    }
}

/* margin / padding: one to four values for top, right, bottom, left. */
static void put_sides(decl_out_t* out, u32 first_prop, const char* v, u32 length, int important) {
    const char* tok[4];
    u32 len[4];
    u32 count = 0;
    u32 at = 0;

    while (count < 4u) {
        len[count] = css_token(v, length, &at, &tok[count]);

        if (!len[count]) {
            break;
        }

        count++;
    }

    if (!count) {
        return;
    }

    static const u8 pick[4][4] = {
        { 0, 0, 0, 0 }, { 0, 1, 0, 1 }, { 0, 1, 2, 1 }, { 0, 1, 2, 3 }
    };

    for (u32 side = 0; side < 4u; side++) {
        u32 k = pick[count - 1u][side];

        put_length(out, first_prop + side, tok[k], len[k], important);
    }
}

/* border: "1px solid #ccc" in any order; `sides` is a bit for each side. */
static void put_border(decl_out_t* out, u32 sides, const char* v, u32 length, int important) {
    const char* tok;
    u32 at = 0;
    i32 width = -1;
    int none = 0;
    int styled = 0;
    u32 color = 0;
    int has_color = 0;

    for (;;) {
        u32 n = css_token(v, length, &at, &tok);
        decl_t d;
        u32 c;

        if (!n) {
            break;
        }

        if (css_is(tok, n, "none") || css_is(tok, n, "hidden")) {
            none = 1;
        } else if (css_is(tok, n, "solid") || css_is(tok, n, "dashed") || css_is(tok, n, "dotted") ||
                   css_is(tok, n, "double") || css_is(tok, n, "groove") || css_is(tok, n, "ridge") ||
                   css_is(tok, n, "inset") || css_is(tok, n, "outset")) {
            styled = 1;
        } else if (css_is(tok, n, "thin")) {
            width = 1;
        } else if (css_is(tok, n, "medium")) {
            width = 2;
        } else if (css_is(tok, n, "thick")) {
            width = 4;
        } else if (css_length(tok, n, &d) && (d.unit == U_PX || d.unit == U_EM || d.unit == U_REM)) {
            width = d.unit == U_PX ? d.v : (d.v * 16) / 100;
        } else if (css_color(tok, n, &c) == 1) {
            color = c;
            has_color = 1;
        }
    }

    if (none || (width < 0 && !styled && !has_color)) {
        width = none ? 0 : width;
    }

    if (width < 0 && styled) {
        width = 2;
    }

    if (width > 20) {
        width = 20;
    }

    for (u32 side = 0; side < 4u; side++) {
        if ((sides & (1u << side)) && width >= 0) {
            put_decl(out, P_BORDER_T + side, U_PX, width, important);
        }
    }

    if (has_color) {
        put_decl(out, P_BORDER_COLOR, U_PX, (i32)color, important);
    }
}

static void css_declaration(decl_out_t* out, const char* name, u32 nlen,
                            const char* v, u32 vlen, int important);

/* Records a declaration whose value needs variables: "name\0value\0". */
static void put_deferred(decl_out_t* out, const char* name, u32 nlen,
                         const char* v, u32 vlen, int important) {
    if (g_var_pool_len + nlen + vlen + 2u > VAR_POOL_MAX) {
        return;
    }

    u32 at = g_var_pool_len;

    for (u32 i = 0; i < nlen; i++) g_var_pool[g_var_pool_len++] = lower(name[i]);
    g_var_pool[g_var_pool_len++] = 0;
    for (u32 i = 0; i < vlen; i++) g_var_pool[g_var_pool_len++] = v[i];
    g_var_pool[g_var_pool_len++] = 0;

    put_decl(out, P_DEFERRED, 0, (i32)at, important);
}

/* One "name: value" pair, turned into the simple properties above. */
static void css_declaration(decl_out_t* out, const char* name, u32 nlen,
                            const char* v, u32 vlen, int important) {
    decl_t d;
    u32 color;

    while (vlen && css_space(v[0])) { v++; vlen--; }
    while (vlen && css_space(v[vlen - 1u])) { vlen--; }

    if (!vlen || !nlen) {
        return;
    }

    /* "!important" at the end */
    if (vlen >= 10u && css_is(v + vlen - 10u, 10, "!important")) {
        important = 1;
        vlen -= 10u;

        while (vlen && css_space(v[vlen - 1u])) { vlen--; }
    }

    if (css_has(v, vlen, "var(")) {
        put_deferred(out, name, nlen, v, vlen, important);
        return;
    }

    if (css_is(v, vlen, "inherit") || css_is(v, vlen, "initial") || css_is(v, vlen, "unset") ||
        css_is(v, vlen, "revert")) {
        return;
    }

#define NAME(s) css_is(name, nlen, s)

    if (NAME("display")) {
        u32 value = D_BLOCK;

        if (css_is(v, vlen, "none")) value = D_NONE;
        else if (css_is(v, vlen, "inline") || css_is(v, vlen, "contents")) value = D_INLINE;
        else if (css_is(v, vlen, "inline-block")) value = D_INLINE_BLOCK;
        else if (css_is(v, vlen, "list-item")) value = D_LIST_ITEM;
        else if (css_is(v, vlen, "table") || css_is(v, vlen, "inline-table")) value = D_TABLE;
        else if (css_is(v, vlen, "table-row")) value = D_ROW;
        else if (css_is(v, vlen, "table-cell")) value = D_CELL;
        else if (css_has(v, vlen, "-group")) value = D_ROW_GROUP;
        else if (css_is(v, vlen, "flex")) value = D_FLEX;
        else if (css_is(v, vlen, "grid")) value = D_GRID;
        else if (css_is(v, vlen, "inline-flex") || css_is(v, vlen, "inline-grid")) value = D_INLINE_BLOCK;

        put_decl(out, P_DISPLAY, 0, (i32)value, important);
    } else if (NAME("color")) {
        if (css_color(v, vlen, &color) == 1) {
            put_decl(out, P_COLOR, 0, (i32)color, important);
        }
    } else if (NAME("background-color") || NAME("background")) {
        /* the first colour in the value; pictures and gradients are skipped */
        const char* tok;
        u32 at = 0;
        int found = 0;

        for (;;) {
            u32 n = css_token(v, vlen, &at, &tok);

            if (!n) {
                break;
            }

            int kind = css_color(tok, n, &color);

            if (kind == 1) {
                put_decl(out, P_BG, 0, (i32)color, important);
                found = 1;
                break;
            }

            if (kind == 2 || css_is(tok, n, "none")) {
                put_decl(out, P_BG, 0, (i32)NO_COLOR, important);
                found = 1;
                break;
            }
        }

        (void)found;
    } else if (NAME("font-size")) {
        if (css_is(v, vlen, "xx-small")) put_decl(out, P_FONT_SIZE, U_PX, 9, important);
        else if (css_is(v, vlen, "x-small")) put_decl(out, P_FONT_SIZE, U_PX, 10, important);
        else if (css_is(v, vlen, "small")) put_decl(out, P_FONT_SIZE, U_PX, 13, important);
        else if (css_is(v, vlen, "medium")) put_decl(out, P_FONT_SIZE, U_PX, 16, important);
        else if (css_is(v, vlen, "large")) put_decl(out, P_FONT_SIZE, U_PX, 18, important);
        else if (css_is(v, vlen, "x-large")) put_decl(out, P_FONT_SIZE, U_PX, 24, important);
        else if (css_is(v, vlen, "xx-large")) put_decl(out, P_FONT_SIZE, U_PX, 32, important);
        else if (css_is(v, vlen, "smaller")) put_decl(out, P_FONT_SIZE, U_EM, 83, important);
        else if (css_is(v, vlen, "larger")) put_decl(out, P_FONT_SIZE, U_EM, 120, important);
        else if (css_length(v, vlen, &d) && d.unit != U_AUTO && d.unit != U_NUM) {
            put_decl(out, P_FONT_SIZE, d.unit == U_PCT ? U_EM : d.unit, d.v, important);
        }
    } else if (NAME("font-weight")) {
        i32 n = 0;

        if (css_is(v, vlen, "bold") || css_is(v, vlen, "bolder")) {
            put_decl(out, P_BOLD, 0, 1, important);
        } else if (css_is(v, vlen, "normal") || css_is(v, vlen, "lighter")) {
            put_decl(out, P_BOLD, 0, 0, important);
        } else if (css_number(v, vlen, &n)) {
            put_decl(out, P_BOLD, 0, n >= 600000, important);
        }
    } else if (NAME("font-style")) {
        put_decl(out, P_ITALIC, 0, css_is(v, vlen, "italic") || css_is(v, vlen, "oblique"), important);
    } else if (NAME("font-family")) {
        put_decl(out, P_MONO, 0, css_has(v, vlen, "mono") || css_has(v, vlen, "courier") ||
                                 css_has(v, vlen, "consolas"), important);
    } else if (NAME("font")) {
        /* the pieces that matter: bold, italic, the size, a fixed-width family */
        const char* tok;
        u32 at = 0;

        put_decl(out, P_MONO, 0, css_has(v, vlen, "mono") || css_has(v, vlen, "courier"), important);

        for (;;) {
            u32 n = css_token(v, vlen, &at, &tok);

            if (!n) {
                break;
            }

            if (css_is(tok, n, "bold")) {
                put_decl(out, P_BOLD, 0, 1, important);
            } else if (css_is(tok, n, "italic")) {
                put_decl(out, P_ITALIC, 0, 1, important);
            } else if (tok[0] >= '0' && tok[0] <= '9') {
                u32 size_len = 0;

                while (size_len < n && tok[size_len] != '/') {
                    size_len++;
                }

                if (css_length(tok, size_len, &d) && d.unit != U_NUM && d.unit != U_AUTO) {
                    put_decl(out, P_FONT_SIZE, d.unit == U_PCT ? U_EM : d.unit, d.v, important);
                    break;
                }
            }
        }
    } else if (NAME("text-align")) {
        if (css_is(v, vlen, "center")) put_decl(out, P_ALIGN, 0, ALIGN_CENTER, important);
        else if (css_is(v, vlen, "right") || css_is(v, vlen, "end")) put_decl(out, P_ALIGN, 0, ALIGN_RIGHT, important);
        else put_decl(out, P_ALIGN, 0, ALIGN_LEFT, important);
    } else if (NAME("text-decoration") || NAME("text-decoration-line")) {
        if (css_has(v, vlen, "underline")) put_decl(out, P_UNDERLINE, 0, 1, important);
        else if (css_has(v, vlen, "none")) put_decl(out, P_UNDERLINE, 0, 0, important);
    } else if (NAME("white-space")) {
        if (css_is(v, vlen, "nowrap")) put_decl(out, P_WHITE, 0, WHITE_NOWRAP, important);
        else if (css_is(v, vlen, "pre") || css_is(v, vlen, "pre-wrap")) put_decl(out, P_WHITE, 0, WHITE_PRE, important);
        else put_decl(out, P_WHITE, 0, WHITE_NORMAL, important);
    } else if (NAME("list-style") || NAME("list-style-type")) {
        put_decl(out, P_LIST, 0, css_has(v, vlen, "none"), important);
    } else if (NAME("visibility")) {
        put_decl(out, P_VISIBILITY, 0, !css_is(v, vlen, "visible"), important);
    } else if (NAME("text-transform")) {
        put_decl(out, P_UPPER, 0, css_is(v, vlen, "uppercase"), important);
    } else if (NAME("box-sizing")) {
        put_decl(out, P_BOX_SIZING, 0, css_is(v, vlen, "border-box"), important);
    } else if (NAME("position")) {
        put_decl(out, P_POSITION, 0, css_is(v, vlen, "absolute") ? 2 : (css_is(v, vlen, "fixed") ? 3
                                   : (css_is(v, vlen, "relative") ? 1 : 0)), important);
    } else if (NAME("top")) {
        put_length(out, P_TOP, v, vlen, important);
    } else if (NAME("right") || NAME("inset-inline-end")) {
        put_length(out, P_RIGHT, v, vlen, important);
    } else if (NAME("bottom")) {
        put_length(out, P_BOTTOM, v, vlen, important);
    } else if (NAME("left") || NAME("inset-inline-start")) {
        put_length(out, P_LEFT, v, vlen, important);
    } else if (NAME("inset")) {
        put_sides(out, P_TOP, v, vlen, important);
    } else if (NAME("z-index")) {
        i32 n = 0;

        if (css_number(v, vlen, &n)) {
            put_decl(out, P_ZINDEX, 0, n < 0, important);
        }
    } else if (NAME("clip")) {
        put_decl(out, P_CLIPPED, 0, css_has(v, vlen, "rect("), important);
    } else if (NAME("clip-path")) {
        put_decl(out, P_CLIPPED, 0, css_has(v, vlen, "inset(50%") || css_has(v, vlen, "inset(100%"), important);
    } else if (NAME("opacity")) {
        i32 n = 1000;

        if (css_number(v, vlen, &n)) {
            put_decl(out, P_CLIPPED, 0, n == 0, important);
        }
    } else if (NAME("overflow") || NAME("overflow-x") || NAME("overflow-y")) {
        put_decl(out, P_OVERFLOW, 0, css_has(v, vlen, "hidden") || css_has(v, vlen, "clip"), important);
    } else if (NAME("float")) {
        put_decl(out, P_FLOAT, 0, css_is(v, vlen, "left") ? 1 : (css_is(v, vlen, "right") ? 2 : 0), important);
    } else if (NAME("flex-direction") || NAME("flex-flow")) {
        put_decl(out, P_FLEX_DIR, 0, css_has(v, vlen, "column"), important);

        if (NAME("flex-flow")) {
            put_decl(out, P_WRAP, 0, css_has(v, vlen, "wrap") && !css_has(v, vlen, "nowrap"), important);
        }
    } else if (NAME("flex-wrap")) {
        put_decl(out, P_WRAP, 0, css_has(v, vlen, "wrap") && !css_has(v, vlen, "nowrap"), important);
    } else if (NAME("clear")) {
        put_decl(out, P_CLEAR, 0, css_is(v, vlen, "left") ? 1 : (css_is(v, vlen, "right") ? 2
                                : (css_is(v, vlen, "none") ? 0 : 3)), important);
    } else if (NAME("flex-grow") || NAME("flex-shrink")) {
        i32 n = 0;

        if (css_number(v, vlen, &n)) {
            n = (n + 500) / 1000;
            put_decl(out, NAME("flex-grow") ? P_FLEX_GROW : P_FLEX_SHRINK, 0,
                     n < 0 ? 0 : (n > 200 ? 200 : n), important);
        }
    } else if (NAME("flex-basis")) {
        put_length(out, P_FLEX_BASIS, v, vlen, important);
    } else if (NAME("flex")) {
        /* "1", "none", "auto", "0 0 200px", "1 1 0%" ... */
        if (css_is(v, vlen, "none")) {
            put_decl(out, P_FLEX_GROW, 0, 0, important);
            put_decl(out, P_FLEX_SHRINK, 0, 0, important);
            put_decl(out, P_FLEX_BASIS, U_AUTO, 0, important);
        } else if (css_is(v, vlen, "auto")) {
            put_decl(out, P_FLEX_GROW, 0, 1, important);
            put_decl(out, P_FLEX_SHRINK, 0, 1, important);
            put_decl(out, P_FLEX_BASIS, U_AUTO, 0, important);
        } else {
            const char* tok;
            u32 at = 0;
            u32 numbers = 0;
            int basis_set = 0;

            for (;;) {
                u32 n = css_token(v, vlen, &at, &tok);

                if (!n) {
                    break;
                }

                if (css_length(tok, n, &d)) {
                    if (d.unit == U_NUM || (d.unit == U_PX && d.v == 0 && numbers < 2u && n == 1u)) {
                        i32 value = d.unit == U_NUM ? (d.v + 50) / 100 : 0;

                        put_decl(out, numbers == 0 ? P_FLEX_GROW : P_FLEX_SHRINK, 0,
                                 value > 200 ? 200 : value, important);
                        numbers++;
                    } else {
                        put_decl(out, P_FLEX_BASIS, d.unit, d.v, important);
                        basis_set = 1;
                    }
                }
            }

            if (numbers == 1u) {
                put_decl(out, P_FLEX_SHRINK, 0, 1, important);
            }

            /* "flex: 1" means: start from nothing and share the space */
            if (numbers && !basis_set) {
                put_decl(out, P_FLEX_BASIS, U_PX, 0, important);
            }
        }
    } else if (NAME("justify-content")) {
        u32 value = JUSTIFY_START;

        if (css_has(v, vlen, "center")) value = JUSTIFY_CENTER;
        else if (css_has(v, vlen, "between")) value = JUSTIFY_BETWEEN;
        else if (css_has(v, vlen, "around") || css_has(v, vlen, "evenly")) value = JUSTIFY_AROUND;
        else if (css_has(v, vlen, "end") || css_is(v, vlen, "right")) value = JUSTIFY_END;

        put_decl(out, P_JUSTIFY, 0, (i32)value, important);
    } else if (NAME("align-items")) {
        u32 value = CROSS_STRETCH;

        if (css_has(v, vlen, "center")) value = CROSS_CENTER;
        else if (css_has(v, vlen, "end")) value = CROSS_END;
        else if (css_has(v, vlen, "start") || css_has(v, vlen, "baseline")) value = CROSS_START;

        put_decl(out, P_ALIGN_ITEMS, 0, (i32)value, important);
    } else if (NAME("gap") || NAME("column-gap") || NAME("grid-gap")) {
        /* one value, or "row column": the column gap is what a row needs */
        const char* tok;
        const char* last = 0;
        u32 last_len = 0;
        u32 at = 0;

        for (;;) {
            u32 n = css_token(v, vlen, &at, &tok);

            if (!n) {
                break;
            }

            last = tok;
            last_len = n;
        }

        if (last) {
            put_length(out, P_GAP, last, last_len, important);
        }

        /* the first value is the gap between rows */
        at = 0;

        u32 first_len = css_token(v, vlen, &at, &tok);

        if (first_len && !NAME("column-gap")) {
            put_length(out, P_ROW_GAP, tok, first_len, important);
        }
    } else if (NAME("row-gap")) {
        put_length(out, P_ROW_GAP, v, vlen, important);
    } else if (NAME("grid-template-columns") || NAME("content")) {
        /* kept as text: worked out when the element is laid out */
        if (g_var_pool_len + vlen + 1u <= VAR_POOL_MAX) {
            u32 at_pool = g_var_pool_len;

            for (u32 i = 0; i < vlen; i++) g_var_pool[g_var_pool_len++] = v[i];
            g_var_pool[g_var_pool_len++] = 0;

            put_decl(out, NAME("content") ? P_CONTENT : P_GRID_COLS, 0, (i32)at_pool, important);
        }
    } else if (NAME("grid-column") || NAME("grid-column-end")) {
        /* "span 2", "1 / -1" (the whole row), "1 / span 3" */
        i32 span = 1;

        for (u32 i = 0; i + 4u < vlen; i++) {
            if (css_is(v + i, 4, "span")) {
                i32 n = 0;
                u32 k = i + 4u;

                while (k < vlen && css_space(v[k])) k++;

                if (css_number(v + k, vlen - k, &n)) span = n / 1000;
            }
        }

        if (css_has(v, vlen, "-1")) {
            span = 255;
        }

        put_decl(out, P_GRID_SPAN, 0, span < 1 ? 1 : (span > 255 ? 255 : span), important);
    } else if (NAME("border-radius")) {
        const char* tok;
        u32 at = 0;
        u32 n = css_token(v, vlen, &at, &tok);

        if (n && css_length(tok, n, &d)) {
            if (d.unit == U_PCT) {
                put_decl(out, P_RADIUS, U_PX, d.v >= 40 ? 255 : 6, important);
            } else if (d.unit != U_AUTO && d.unit != U_NUM) {
                put_decl(out, P_RADIUS, d.unit, d.v, important);
            }
        }
    } else if (NAME("line-height")) {
        if (css_is(v, vlen, "normal")) {
            put_decl(out, P_LINE_H, U_PX, 0, important);
        } else if (css_length(v, vlen, &d) && d.unit != U_AUTO) {
            put_decl(out, P_LINE_H, d.unit, d.v, important);
        }
    } else if (NAME("margin")) {
        put_sides(out, P_MARGIN_T, v, vlen, important);
    } else if (NAME("padding")) {
        put_sides(out, P_PADDING_T, v, vlen, important);
    } else if (NAME("margin-top") || NAME("margin-block-start")) {
        put_length(out, P_MARGIN_T, v, vlen, important);
    } else if (NAME("margin-right") || NAME("margin-inline-end")) {
        put_length(out, P_MARGIN_R, v, vlen, important);
    } else if (NAME("margin-bottom") || NAME("margin-block-end")) {
        put_length(out, P_MARGIN_B, v, vlen, important);
    } else if (NAME("margin-left") || NAME("margin-inline-start")) {
        put_length(out, P_MARGIN_L, v, vlen, important);
    } else if (NAME("margin-inline")) {
        put_length(out, P_MARGIN_L, v, vlen, important);
        put_length(out, P_MARGIN_R, v, vlen, important);
    } else if (NAME("margin-block")) {
        put_length(out, P_MARGIN_T, v, vlen, important);
        put_length(out, P_MARGIN_B, v, vlen, important);
    } else if (NAME("padding-top") || NAME("padding-block-start")) {
        put_length(out, P_PADDING_T, v, vlen, important);
    } else if (NAME("padding-right") || NAME("padding-inline-end")) {
        put_length(out, P_PADDING_R, v, vlen, important);
    } else if (NAME("padding-bottom") || NAME("padding-block-end")) {
        put_length(out, P_PADDING_B, v, vlen, important);
    } else if (NAME("padding-left") || NAME("padding-inline-start")) {
        put_length(out, P_PADDING_L, v, vlen, important);
    } else if (NAME("padding-inline")) {
        put_length(out, P_PADDING_L, v, vlen, important);
        put_length(out, P_PADDING_R, v, vlen, important);
    } else if (NAME("padding-block")) {
        put_length(out, P_PADDING_T, v, vlen, important);
        put_length(out, P_PADDING_B, v, vlen, important);
    } else if (NAME("border") || NAME("border-style") || NAME("border-width")) {
        put_border(out, 15u, v, vlen, important);
    } else if (NAME("border-top")) {
        put_border(out, 1u, v, vlen, important);
    } else if (NAME("border-right")) {
        put_border(out, 2u, v, vlen, important);
    } else if (NAME("border-bottom")) {
        put_border(out, 4u, v, vlen, important);
    } else if (NAME("border-left")) {
        put_border(out, 8u, v, vlen, important);
    } else if (NAME("border-top-width") || NAME("border-top-style")) {
        put_border(out, 1u, v, vlen, important);
    } else if (NAME("border-right-width") || NAME("border-right-style")) {
        put_border(out, 2u, v, vlen, important);
    } else if (NAME("border-bottom-width") || NAME("border-bottom-style")) {
        put_border(out, 4u, v, vlen, important);
    } else if (NAME("border-left-width") || NAME("border-left-style")) {
        put_border(out, 8u, v, vlen, important);
    } else if (NAME("border-color") || NAME("border-top-color") || NAME("border-bottom-color") ||
               NAME("border-left-color") || NAME("border-right-color")) {
        const char* tok;
        u32 at = 0;
        u32 n = css_token(v, vlen, &at, &tok);

        if (n && css_color(tok, n, &color) == 1) {
            put_decl(out, P_BORDER_COLOR, 0, (i32)color, important);
        }
    } else if (NAME("width") || NAME("inline-size")) {
        put_length(out, P_WIDTH, v, vlen, important);
    } else if (NAME("max-width")) {
        if (css_is(v, vlen, "none")) put_decl(out, P_MAX_W, U_NONE, 0, important);
        else put_length(out, P_MAX_W, v, vlen, important);
    } else if (NAME("min-width")) {
        put_length(out, P_MIN_W, v, vlen, important);
    } else if (NAME("height")) {
        put_length(out, P_HEIGHT, v, vlen, important);
    }

#undef NAME
}

/* Remembers "--name: value" for var(). */
static void css_set_variable(const char* name, u32 nlen, const char* v, u32 vlen) {
    u32 hash = css_hash(name, nlen, 0);
    u32 slot = g_var_count;

    while (vlen && css_space(v[0])) { v++; vlen--; }
    while (vlen && css_space(v[vlen - 1u])) { vlen--; }

    for (u32 i = 0; i < g_var_count; i++) {
        if (g_vars[i].name == hash) {
            slot = i;
        }
    }

    if (slot == VARS_MAX || g_var_pool_len + vlen + 1u > VAR_POOL_MAX) {
        return;
    }

    g_vars[slot].name = hash;
    g_vars[slot].value = g_var_pool_len;

    for (u32 i = 0; i < vlen; i++) g_var_pool[g_var_pool_len++] = v[i];
    g_var_pool[g_var_pool_len++] = 0;

    if (slot == g_var_count) {
        g_var_count++;
    }
}

/*
 * Replaces every var(--name, fallback) in `v` with its value. Returns the
 * new length, or 0 if a variable is unknown and has no fallback.
 */
static u32 css_substitute(const char* v, u32 vlen, char* out, u32 size, int depth) {
    u32 n = 0;

    for (u32 i = 0; i < vlen; ) {
        if (i + 4u <= vlen && lower(v[i]) == 'v' && v[i + 1u] == 'a' && v[i + 2u] == 'r' && v[i + 3u] == '(') {
            u32 start = i + 4u;
            u32 end = start;
            int level = 1;

            while (end < vlen && level > 0) {
                if (v[end] == '(') level++;
                if (v[end] == ')') level--;
                end++;
            }

            /* inside: name, then optionally ", fallback" */
            u32 inner_end = end - 1u;
            u32 name_start = start;

            while (name_start < inner_end && css_space(v[name_start])) name_start++;

            u32 name_end = name_start;

            while (name_end < inner_end && v[name_end] != ',' && !css_space(v[name_end])) name_end++;

            u32 comma = name_end;

            while (comma < inner_end && v[comma] != ',') comma++;

            u32 hash = css_hash(v + name_start, name_end - name_start, 0);
            const char* value = 0;
            u32 value_len = 0;

            for (u32 k = 0; k < g_var_count; k++) {
                if (g_vars[k].name == hash) {
                    value = g_var_pool + g_vars[k].value;
                    value_len = (u32)strlen(value);
                }
            }

            if (!value && comma < inner_end) {
                value = v + comma + 1u;
                value_len = inner_end - comma - 1u;
            }

            if (!value || depth > 4) {
                return 0;
            }

            /* the value may itself use variables */
            char inner[256];
            u32 inner_len = css_substitute(value, value_len, inner, sizeof(inner), depth + 1);

            if (!inner_len) {
                return 0;
            }

            for (u32 k = 0; k < inner_len && n + 1u < size; k++) {
                out[n++] = inner[k];
            }

            i = end;
        } else {
            if (n + 1u < size) {
                out[n++] = v[i];
            }

            i++;
        }
    }

    out[n] = 0;

    return n;
}

/* A block of "name: value; name: value" pairs. */
static void css_declarations(decl_out_t* out, const char* text, u32 length, int variables_too) {
    u32 i = 0;

    while (i < length) {
        while (i < length && (css_space(text[i]) || text[i] == ';')) {
            i++;
        }

        u32 name_start = i;

        while (i < length && text[i] != ':' && text[i] != ';') {
            i++;
        }

        if (i >= length || text[i] == ';') {
            continue;
        }

        u32 name_end = i;

        while (name_end > name_start && css_space(text[name_end - 1u])) {
            name_end--;
        }

        i++;

        u32 value_start = i;
        int depth = 0;
        char quote = 0;

        while (i < length) {
            char c = text[i];

            if (quote) {
                if (c == quote) quote = 0;
            } else if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '(') {
                depth++;
            } else if (c == ')') {
                depth--;
            } else if (c == ';' && depth <= 0) {
                break;
            }

            i++;
        }

        const char* name = text + name_start;
        u32 nlen = name_end - name_start;

        if (nlen > 2u && name[0] == '-' && name[1] == '-') {
            if (variables_too) {
                css_set_variable(name, nlen, text + value_start, i - value_start);
            }
        } else {
            css_declaration(out, name, nlen, text + value_start, i - value_start, 0);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Selectors                                                                   */
/* -------------------------------------------------------------------------- */

/* "2n+1", "odd", "3", "-n+4": fills a and b. Returns 0 if not understood. */
static int css_nth(const char* v, u32 length, i8* a, i8* b) {
    char text[16];
    u32 n = 0;

    for (u32 i = 0; i < length && n + 1u < sizeof(text); i++) {
        if (!css_space(v[i])) {
            text[n++] = lower(v[i]);
        }
    }

    text[n] = 0;

    if (css_is(text, n, "odd")) { *a = 2; *b = 1; return 1; }
    if (css_is(text, n, "even")) { *a = 2; *b = 0; return 1; }

    u32 i = 0;
    int sign = 1;
    i32 value = 0;
    int digits = 0;

    if (text[i] == '-' || text[i] == '+') {
        sign = text[i] == '-' ? -1 : 1;
        i++;
    }

    while (text[i] >= '0' && text[i] <= '9') {
        value = value * 10 + (text[i] - '0');
        i++;
        digits = 1;
    }

    if (text[i] != 'n') {
        /* a plain number */
        if (!digits || text[i] || value > 100) {
            return 0;
        }

        *a = 0;
        *b = (i8)(sign * value);
        return 1;
    }

    if (value > 100) {
        return 0;
    }

    *a = (i8)(sign * (digits ? value : 1));
    *b = 0;
    i++;

    if (text[i] == '+' || text[i] == '-') {
        int bsign = text[i] == '-' ? -1 : 1;
        i32 bv = 0;

        i++;

        while (text[i] >= '0' && text[i] <= '9') {
            bv = bv * 10 + (text[i] - '0');
            i++;
        }

        if (bv > 100) {
            return 0;
        }

        *b = (i8)(bsign * bv);
    }

    return text[i] == 0;
}

/* Reads one compound ("div.note#x[href^=http]:nth-child(2)"). Returns chars used, 0 if unsupported. */
static u32 css_compound(const char* s, u32 length, compound_t* c) {
    u32 i = 0;

    memset(c, 0, sizeof(*c));

    if (i < length && s[i] == '*') {
        i++;
    } else {
        u32 start = i;

        while (i < length && css_ident_char(s[i])) {
            i++;
        }

        if (i > start) {
            c->tag = css_hash(s + start, i - start, 1);
        }
    }

    while (i < length) {
        char kind = s[i];

        if (kind == '.' || kind == '#') {
            u32 start = ++i;

            while (i < length && css_ident_char(s[i])) {
                i++;
            }

            if (i == start) {
                return 0;
            }

            if (kind == '#') {
                c->id = css_hash(s + start, i - start, 0);
            } else if (c->class_count < 4u) {
                c->cls[c->class_count++] = css_hash(s + start, i - start, 0);
            } else {
                return 0;
            }
        } else if (kind == '[') {
            u32 start = ++i;

            while (i < length && css_ident_char(s[i])) {
                i++;
            }

            if (i == start || c->attr_count >= 2u) {
                return 0;
            }

            u32 slot = c->attr_count;

            c->attr_name[slot] = css_hash(s + start, i - start, 1);
            c->attr_value[slot] = 0;
            c->attr_op[slot] = 0;

            if (i + 1u < length && s[i + 1u] == '=' &&
                (s[i] == '~' || s[i] == '|' || s[i] == '^' || s[i] == '$' || s[i] == '*')) {
                c->attr_op[slot] = (u8)s[i];
                i++;
            } else if (i < length && s[i] == '=') {
                c->attr_op[slot] = '=';
            }

            if (c->attr_op[slot]) {
                i++;

                char quote = (i < length && (s[i] == '"' || s[i] == '\'')) ? s[i++] : 0;
                u32 vstart = i;

                while (i < length && (quote ? s[i] != quote : (s[i] != ']' && !css_space(s[i])))) {
                    i++;
                }

                u32 vlen = i - vstart;

                if (c->attr_op[slot] == '=') {
                    c->attr_value[slot] = css_hash(s + vstart, vlen, 0);
                } else {
                    /* the other tests compare text: keep the text */
                    if (!vlen || g_var_pool_len + vlen + 1u > VAR_POOL_MAX) {
                        return 0;
                    }

                    c->attr_value[slot] = g_var_pool_len;

                    for (u32 k = 0; k < vlen; k++) g_var_pool[g_var_pool_len++] = s[vstart + k];
                    g_var_pool[g_var_pool_len++] = 0;
                }

                if (quote && i < length) {
                    i++;
                }

                /* " i" (ignore case) and spaces before the bracket */
                while (i < length && (css_space(s[i]) || s[i] == 'i' || s[i] == 's')) {
                    i++;
                }
            }

            if (i >= length || s[i] != ']') {
                return 0;
            }

            i++;
            c->attr_count++;
        } else if (kind == ':') {
            i++;

            if (i < length && s[i] == ':') {
                i++;
            }

            u32 start = i;

            while (i < length && css_ident_char(s[i])) {
                i++;
            }

            const char* name = s + start;
            u32 nlen = i - start;
            const char* arg = 0;
            u32 alen = 0;

            if (i < length && s[i] == '(') {
                int depth = 1;
                u32 astart = ++i;

                while (i < length && depth > 0) {
                    if (s[i] == '(') depth++;
                    if (s[i] == ')') depth--;
                    i++;
                }

                if (depth) {
                    return 0;
                }

                arg = s + astart;
                alen = i - 1u - astart;

                while (alen && css_space(arg[0])) { arg++; alen--; }
                while (alen && css_space(arg[alen - 1u])) { alen--; }
            }

            if (css_is(name, nlen, "first-child")) c->pseudo |= PSEUDO_FIRST;
            else if (css_is(name, nlen, "last-child")) c->pseudo |= PSEUDO_LAST;
            else if (css_is(name, nlen, "only-child")) c->pseudo |= PSEUDO_FIRST | PSEUDO_LAST;
            else if (css_is(name, nlen, "first-of-type")) c->pseudo |= PSEUDO_FIRST_TYPE;
            else if (css_is(name, nlen, "last-of-type")) c->pseudo |= PSEUDO_LAST_TYPE;
            else if (css_is(name, nlen, "link") || css_is(name, nlen, "any-link")) c->pseudo |= PSEUDO_LINK;
            else if (css_is(name, nlen, "root")) c->pseudo |= PSEUDO_ROOT;
            else if ((css_is(name, nlen, "nth-child") || css_is(name, nlen, "nth-last-child")) && arg && !c->nth) {
                if (!css_nth(arg, alen, &c->nth_a, &c->nth_b)) {
                    return 0;
                }

                c->nth = css_is(name, nlen, "nth-child") ? 1 : 2;
            } else if (css_is(name, nlen, "not") && arg && alen && !c->not_kind) {
                /* one simple selector inside: .class, #id, tag or [attr] */
                u32 k = (arg[0] == '.' || arg[0] == '#' || arg[0] == '[') ? 1u : 0u;
                u32 from = k;

                while (k < alen && css_ident_char(arg[k])) {
                    k++;
                }

                if (k == from || (arg[0] == '[' ? !(k + 1u == alen && arg[k] == ']') : k != alen)) {
                    return 0;
                }

                c->not_kind = arg[0] == '.' ? NOT_CLASS : (arg[0] == '#' ? NOT_ID : (arg[0] == '[' ? NOT_ATTR : NOT_TAG));
                c->not_hash = css_hash(arg + from, k - from, c->not_kind == NOT_TAG || c->not_kind == NOT_ATTR);
            } else if (css_is(name, nlen, "before") && !arg) {
                c->pe = 1;
            } else if (css_is(name, nlen, "after") && !arg) {
                c->pe = 2;
            } else {
                return 0;       /* :hover, ::marker, :is(...) and the rest */
            }
        } else {
            break;
        }
    }

    return i;
}

static void css_add_rule(const char* s, u32 length, u32 decl_start, u32 decl_count) {
    compound_t chain[CHAIN_MAX];
    u32 count = 0;
    u32 i = 0;
    u8 relation = REL_ANCESTOR;

    while (length && css_space(s[length - 1u])) {
        length--;
    }

    /* Read left to right; store right to left. */
    for (;;) {
        while (i < length && css_space(s[i])) {
            i++;
        }

        if (i >= length) {
            break;
        }

        if (s[i] == '>' || s[i] == '+' || s[i] == '~') {
            relation = s[i] == '>' ? REL_PARENT : (s[i] == '+' ? REL_PREVIOUS : REL_EARLIER);
            i++;
            continue;
        }

        if (count >= CHAIN_MAX) {
            g_css_dropped++;
            return;
        }

        compound_t c;
        u32 used = css_compound(s + i, length - i, &c);

        if (!used) {
            g_css_dropped++;
            return;
        }

        /* this compound's relation to the one before it belongs to the
           one after it in stored (reversed) order: note it on the previous */
        for (u32 k = count; k > 0; k--) {
            chain[k] = chain[k - 1u];
        }

        chain[0] = c;

        if (count > 0) {
            chain[0].child = relation;
        }

        count++;
        relation = REL_ANCESTOR;
        i += used;
    }

    if (!count || g_rule_count >= RULES_MAX) {
        return;
    }

    rule_t* r = &g_rules[g_rule_count];
    u32 ids = 0;
    u32 classes = 0;
    u32 tags = 0;

    for (u32 k = 0; k < count; k++) {
        r->c[k] = chain[k];
        ids += chain[k].id ? 1u : 0u;
        classes += chain[k].class_count + chain[k].attr_count + (chain[k].pseudo ? 1u : 0u) +
                   (chain[k].nth ? 1u : 0u) + (chain[k].not_kind == NOT_CLASS || chain[k].not_kind == NOT_ATTR ? 1u : 0u);
        ids += chain[k].not_kind == NOT_ID ? 1u : 0u;
        tags += chain[k].tag ? 1u : 0u;
    }

    r->count = (u8)count;
    r->pe = chain[0].pe;

    for (u32 k = 1; k < count; k++) {
        if (chain[k].pe) {
            g_css_dropped++;
            return;         /* ::before in the middle of a selector means nothing */
        }
    }
    r->specificity = (ids << 16) | (classes << 8) | tags;
    r->decl_start = decl_start;
    r->decl_count = decl_count;

    /* Filed under the most telling part of the element's own compound. */
    const compound_t* own = &r->c[0];
    u32 key = own->id ? own->id : (own->class_count ? own->cls[0] : own->tag);

    i32* head = r->pe ? (key ? &g_buckets_pe[key % BUCKETS] : &g_universal_pe)
                      : (key ? &g_buckets[key % BUCKETS] : &g_universal);

    r->next = *head;
    *head = (i32)g_rule_count;

    if (r->pe) {
        g_pe_rules++;
    }

    g_rule_count++;
}

/* Is this "@media ..." condition true for a screen of our width? */
static int css_media_applies(const char* q, u32 length) {
    /* any one of the comma-separated queries is enough */
    u32 start = 0;

    for (u32 i = 0; i <= length; i++) {
        if (i < length && q[i] != ',') {
            continue;
        }

        const char* part = q + start;
        u32 plen = i - start;
        int ok = 1;

        if (css_has(part, plen, "print") || css_has(part, plen, "speech") ||
            css_has(part, plen, "dark") || css_has(part, plen, "hover: hover") ||
            css_has(part, plen, "forced-colors") || css_has(part, plen, "prefers-contrast: more")) {
            ok = 0;
        }

        for (u32 k = 0; ok && k + 10u < plen; k++) {
            int is_min = css_has(part + k, 10, "min-width:") && lower(part[k]) == 'm' && lower(part[k + 1u]) == 'i';
            int is_max = css_has(part + k, 10, "max-width:") && lower(part[k]) == 'm' && lower(part[k + 1u]) == 'a';

            if (!is_min && !is_max) {
                continue;
            }

            u32 vstart = k + 10u;
            u32 vend = vstart;

            while (vend < plen && part[vend] != ')') {
                vend++;
            }

            decl_t d;

            if (css_length(part + vstart, vend - vstart, &d)) {
                i32 px = d.unit == U_PX ? d.v : (d.unit == U_EM || d.unit == U_REM ? (d.v * 16) / 100 : 0);

                if (is_min && g_view_w < px) ok = 0;
                if (is_max && g_view_w > px) ok = 0;
            }
        }

        if (css_has(part, plen, "not ")) {
            ok = !ok;
        }

        if (ok) {
            return 1;
        }

        start = i + 1u;
    }

    return 0;
}

/* Skips a { ... } block starting at its "{". Returns the index after its "}". */
static u32 css_skip_block(const char* text, u32 length, u32 i) {
    int depth = 0;

    for (; i < length; i++) {
        if (text[i] == '{') {
            depth++;
        } else if (text[i] == '}') {
            depth--;

            if (depth == 0) {
                return i + 1u;
            }
        }
    }

    return length;
}

/* Is this selector one whose variables apply to the whole page? */
static int css_is_page_selector(const char* s, u32 length) {
    while (length && css_space(s[0])) { s++; length--; }
    while (length && css_space(s[length - 1u])) { length--; }

    return css_is(s, length, ":root") || css_is(s, length, "html") || css_is(s, length, "body") ||
           css_is(s, length, "*") || css_is(s, length, ":root,:host") || css_is(s, length, ":root, :host");
}

static void css_parse(const char* text, u32 length) {
    u32 i = 0;

    while (i < length) {
        while (i < length && css_space(text[i])) {
            i++;
        }

        if (i >= length) {
            break;
        }

        if (text[i] == '/' && i + 1u < length && text[i + 1u] == '*') {
            i += 2u;

            while (i + 1u < length && !(text[i] == '*' && text[i + 1u] == '/')) {
                i++;
            }

            i += 2u;
            continue;
        }

        if (text[i] == '}') {
            i++;
            continue;
        }

        if (text[i] == '<') {
            i++;        /* "<!--" left in old pages */
            continue;
        }

        /* the prelude runs to "{" or ";" */
        u32 start = i;

        while (i < length && text[i] != '{' && text[i] != ';' && text[i] != '}') {
            i++;
        }

        if (i >= length || text[i] != '{') {
            i++;
            continue;
        }

        u32 prelude_end = i;

        if (text[start] == '@') {
            const char* at = text + start;
            u32 at_len = prelude_end - start;

            if ((at_len >= 6u && css_has(at, 6, "@media") && css_media_applies(at + 6, at_len - 6u)) ||
                (at_len >= 9u && css_has(at, 9, "@supports")) ||
                (at_len >= 6u && css_has(at, 6, "@layer"))) {
                /* the rules inside apply: carry on reading within the block */
                i++;
            } else {
                i = css_skip_block(text, length, i);
            }

            continue;
        }

        u32 body_start = i + 1u;
        u32 body_end = css_skip_block(text, length, i) - 1u;

        if (body_end > length) {
            body_end = length;
        }

        i = body_end + 1u;

        /* a block with blocks inside it (nesting) is beyond this reader */
        int nested = 0;

        for (u32 k = body_start; k < body_end; k++) {
            if (text[k] == '{') {
                nested = 1;
                break;
            }
        }

        if (nested || body_end < body_start) {
            continue;
        }

        decl_out_t out;

        out.list = g_decls + g_decl_count;
        out.count = 0;
        out.max = DECLS_MAX - g_decl_count;

        css_declarations(&out, text + body_start, body_end - body_start,
                         css_is_page_selector(text + start, prelude_end - start));

        if (!out.count) {
            continue;
        }

        u32 decl_start = g_decl_count;

        g_decl_count += out.count;

        /* one rule per comma-separated selector, sharing the declarations */
        u32 sel = start;
        int depth = 0;

        for (u32 k = start; k <= prelude_end; k++) {
            if (k < prelude_end) {
                if (text[k] == '(' || text[k] == '[') depth++;
                if (text[k] == ')' || text[k] == ']') depth--;

                if (text[k] != ',' || depth > 0) {
                    continue;
                }
            }

            while (sel < k && css_space(text[sel])) {
                sel++;
            }

            if (k > sel) {
                css_add_rule(text + sel, k - sel, decl_start, out.count);
            }

            sel = k + 1u;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Matching rules to elements                                                  */
/* -------------------------------------------------------------------------- */

static int node_has_class(const node_t* n, u32 hash) {
    for (u32 i = 0; i < n->class_count; i++) {
        if (g_classes[n->class_start + i] == hash) {
            return 1;
        }
    }

    return 0;
}

/* Finds an attribute of an element by the hash of its name; gives its value's text. */
static int node_attr_text(const node_t* n, u32 name_hash, const char** value, u32* value_len) {
    const char* tag = g_src + n->src;
    u32 length = n->src_len;
    u32 i = 0;

    /* skip the tag name */
    while (i < length && !css_space(tag[i])) {
        i++;
    }

    while (i < length) {
        while (i < length && (css_space(tag[i]) || tag[i] == '/')) {
            i++;
        }

        u32 start = i;

        while (i < length && !css_space(tag[i]) && tag[i] != '=' && tag[i] != '/') {
            i++;
        }

        if (i == start) {
            break;
        }

        int match = css_hash(tag + start, i - start, 1) == name_hash;
        u32 vstart = i;
        u32 vend = i;

        if (i < length && tag[i] == '=') {
            i++;

            char quote = (i < length && (tag[i] == '"' || tag[i] == '\'')) ? tag[i++] : 0;

            vstart = i;

            while (i < length && (quote ? tag[i] != quote : !css_space(tag[i]))) {
                i++;
            }

            vend = i;

            if (quote && i < length) {
                i++;
            }
        }

        if (match) {
            *value = tag + vstart;
            *value_len = vend - vstart;
            return 1;
        }
    }

    return 0;
}

static int text_starts(const char* v, u32 vlen, const char* want, u32 wlen) {
    if (wlen > vlen) {
        return 0;
    }

    for (u32 i = 0; i < wlen; i++) {
        if (v[i] != want[i]) {
            return 0;
        }
    }

    return 1;
}

/* One [attr op value] test. */
static int attr_test(const node_t* n, u32 name, u8 op, u32 wanted) {
    const char* v = 0;
    u32 vlen = 0;

    if (!node_attr_text(n, name, &v, &vlen)) {
        return 0;
    }

    if (!op) {
        return 1;
    }

    if (op == '=') {
        return css_hash(v, vlen, 0) == wanted;
    }

    const char* want = g_var_pool + wanted;
    u32 wlen = (u32)strlen(want);

    switch (op) {
        case '^': return text_starts(v, vlen, want, wlen);
        case '$': return wlen <= vlen && text_starts(v + vlen - wlen, wlen, want, wlen);
        case '|': return text_starts(v, vlen, want, wlen) && (vlen == wlen || v[wlen] == '-');

        case '*':
            for (u32 i = 0; i + wlen <= vlen; i++) {
                if (text_starts(v + i, vlen - i, want, wlen)) return 1;
            }

            return 0;

        case '~':
            /* one of the space-separated words */
            for (u32 i = 0; i < vlen; ) {
                while (i < vlen && css_space(v[i])) i++;

                u32 start = i;

                while (i < vlen && !css_space(v[i])) i++;

                if (i - start == wlen && text_starts(v + start, wlen, want, wlen)) return 1;
            }

            return 0;
    }

    return 0;
}

/* Where an element stands among its parent's elements, counted from 1. */
static i32 element_position(const node_t* n, int from_end, int same_tag) {
    i32 position = 1;

    for (u32 s = from_end ? n->next : n->prev; s; s = from_end ? g_nodes[s].next : g_nodes[s].prev) {
        if (g_nodes[s].type == N_ELEMENT && (!same_tag || g_nodes[s].tag == n->tag)) {
            position++;
        }
    }

    return position;
}

static int compound_matches(const compound_t* c, u32 index) {
    const node_t* n = &g_nodes[index];

    if (n->type != N_ELEMENT) {
        return 0;
    }

    if (c->tag && c->tag != n->tag) {
        return 0;
    }

    if (c->id && c->id != n->id) {
        return 0;
    }

    for (u32 i = 0; i < c->class_count; i++) {
        if (!node_has_class(n, c->cls[i])) {
            return 0;
        }
    }

    for (u32 i = 0; i < c->attr_count; i++) {
        if (!attr_test(n, c->attr_name[i], c->attr_op[i], c->attr_value[i])) {
            return 0;
        }
    }

    switch (c->not_kind) {
        case NOT_TAG:   if (n->tag == c->not_hash) return 0; break;
        case NOT_ID:    if (n->id == c->not_hash) return 0; break;
        case NOT_CLASS: if (node_has_class(n, c->not_hash)) return 0; break;
        case NOT_ATTR:  if (attr_test(n, c->not_hash, 0, 0)) return 0; break;
        default: break;
    }

    if (c->pseudo) {
        if ((c->pseudo & PSEUDO_ROOT) && n->tagid != T_HTML) {
            return 0;
        }

        if ((c->pseudo & PSEUDO_LINK) && !(n->tagid == T_A && n->has_href)) {
            return 0;
        }

        if ((c->pseudo & PSEUDO_FIRST) && element_position(n, 0, 0) != 1) return 0;
        if ((c->pseudo & PSEUDO_LAST) && element_position(n, 1, 0) != 1) return 0;
        if ((c->pseudo & PSEUDO_FIRST_TYPE) && element_position(n, 0, 1) != 1) return 0;
        if ((c->pseudo & PSEUDO_LAST_TYPE) && element_position(n, 1, 1) != 1) return 0;
    }

    if (c->nth) {
        /* is the position a*k + b for some k >= 0? */
        i32 position = element_position(n, c->nth == 2, 0);
        i32 rest = position - c->nth_b;

        if (c->nth_a == 0) {
            if (rest != 0) return 0;
        } else if (rest % c->nth_a != 0 || rest / c->nth_a < 0) {
            return 0;
        }
    }

    return 1;
}

/* Do compounds k.. of the rule match, given that compound k-1 matched `index`? */
static int chain_matches(const rule_t* r, u32 k, u32 index) {
    if (k >= r->count) {
        return 1;
    }

    u8 relation = r->c[k - 1u].child;

    if (relation == REL_PREVIOUS || relation == REL_EARLIER) {
        for (u32 s = g_nodes[index].prev; s; s = g_nodes[s].prev) {
            if (g_nodes[s].type != N_ELEMENT) {
                continue;
            }

            if (compound_matches(&r->c[k], s) && chain_matches(r, k + 1u, s)) {
                return 1;
            }

            if (relation == REL_PREVIOUS) {
                break;
            }
        }

        return 0;
    }

    for (u32 a = g_nodes[index].parent; a; a = g_nodes[a].parent) {
        if (compound_matches(&r->c[k], a) && chain_matches(r, k + 1u, a)) {
            return 1;
        }

        if (relation == REL_PARENT) {
            break;
        }
    }

    return 0;
}

static u32 g_matched[MATCH_MAX];
static u32 g_matched_count = 0;

static void collect_bucket(i32 head, u32 index, u32 pe) {
    for (i32 r = head; r >= 0; r = g_rules[r].next) {
        if (g_rules[r].pe == pe && g_matched_count < MATCH_MAX &&
            compound_matches(&g_rules[r].c[0], index) && chain_matches(&g_rules[r], 1, index)) {
            g_matched[g_matched_count++] = (u32)r;
        }
    }
}

/*
 * All rules that match the element, least important first. pe: 0 for the
 * element itself, 1 for its ::before, 2 for its ::after.
 */
static void collect_rules(u32 index, u32 pe) {
    const node_t* n = &g_nodes[index];
    const i32* buckets = pe ? g_buckets_pe : g_buckets;

    g_matched_count = 0;

    collect_bucket(pe ? g_universal_pe : g_universal, index, pe);
    collect_bucket(buckets[n->tag % BUCKETS], index, pe);

    if (n->id) {
        /* (the same bucket may be visited twice; duplicates are removed below) */
        collect_bucket(buckets[n->id % BUCKETS], index, pe);
    }

    for (u32 i = 0; i < n->class_count; i++) {
        collect_bucket(buckets[g_classes[n->class_start + i] % BUCKETS], index, pe);
    }

    /* sort by specificity, then by position in the sheets; drop repeats */
    for (u32 i = 1; i < g_matched_count; i++) {
        u32 r = g_matched[i];
        u32 k = i;

        while (k > 0 && (g_rules[g_matched[k - 1u]].specificity > g_rules[r].specificity ||
                         (g_rules[g_matched[k - 1u]].specificity == g_rules[r].specificity &&
                          g_matched[k - 1u] > r))) {
            g_matched[k] = g_matched[k - 1u];
            k--;
        }

        g_matched[k] = r;
    }

    u32 kept = 0;

    for (u32 i = 0; i < g_matched_count; i++) {
        if (kept == 0 || g_matched[kept - 1u] != g_matched[i]) {
            g_matched[kept++] = g_matched[i];
        }
    }

    g_matched_count = kept;
}

/* -------------------------------------------------------------------------- */
/* Computing an element's style                                                */
/* -------------------------------------------------------------------------- */

static i32 decl_px(const decl_t* d, const style_t* s) {
    switch (d->unit) {
        case U_PX:  return d->v;
        case U_EM:  return (d->v * (i32)s->font_px) / 100;
        case U_REM: return (d->v * 16) / 100;
        case U_PCT: return (d->v * g_view_w) / 100;
        default:    return 0;
    }
}

static i16 clamp16(i32 v) {
    return (i16)(v > 4000 ? 4000 : (v < -4000 ? -4000 : v));
}

static void apply_decl(style_t* s, const style_t* parent, const decl_t* d, int font_pass) {
    if (font_pass) {
        if (d->prop == P_FONT_SIZE) {
            i32 px = d->unit == U_PX ? d->v
                   : (d->unit == U_EM ? (d->v * (i32)parent->font_px) / 100
                   : (d->unit == U_REM ? (d->v * 16) / 100 : (i32)parent->font_px));

            s->font_px = (u16)(px < 6 ? 6 : (px > 96 ? 96 : px));
        }

        return;
    }

    switch (d->prop) {
        case P_DISPLAY:    s->display = (u8)d->v; break;
        case P_COLOR:      s->color = (u32)d->v; break;
        case P_BG:         s->bg = (u32)d->v; break;
        case P_BOLD:       s->bold = (u8)d->v; break;
        case P_ITALIC:     s->italic = (u8)d->v; break;
        case P_UNDERLINE:  s->underline = (u8)d->v; break;
        case P_ALIGN:      s->align = (u8)d->v; break;
        case P_WHITE:      s->white = (u8)d->v; break;
        case P_LIST:       s->list_none = (u8)d->v; break;
        case P_VISIBILITY: s->hidden = (u8)d->v; break;
        case P_MONO:       s->mono = (u8)d->v; break;
        case P_UPPER:      s->upper = (u8)d->v; break;
        case P_BOX_SIZING: s->border_box = (u8)d->v; break;
        case P_POSITION:   s->out_of_flow = (u8)d->v; break;
        case P_FLOAT:      s->floating = (u8)d->v; break;
        case P_FLEX_DIR:   s->flex_column = (u8)d->v; break;
        case P_CLEAR:      s->clear = (u8)d->v; break;
        case P_ZINDEX:     s->behind = (u8)d->v; break;
        case P_GRID_COLS:  s->grid_cols = (u32)d->v + 1u; break;
        case P_GRID_SPAN:  s->grid_span = (u8)d->v; break;
        case P_ROW_GAP:    s->row_gap = clamp16(d->unit == U_AUTO ? 0 : decl_px(d, s)); break;

        case P_RADIUS: {
            i32 px = decl_px(d, s);

            s->radius = (u8)(px < 0 ? 0 : (px > 255 ? 255 : px));
            break;
        }
        case P_CLIPPED:    s->clipped = (u8)d->v; break;
        case P_OVERFLOW:   s->overflow_hidden = (u8)d->v; break;

        case P_TOP: case P_RIGHT: case P_BOTTOM: case P_LEFT: {
            u32 side = d->prop - P_TOP;
            u8 bit = (u8)(1u << side);

            if (d->unit == U_AUTO) {
                s->inset_set &= (u8)~bit;
            } else {
                s->inset_set |= bit;
                s->inset[side] = clamp16(d->unit == U_PCT ? d->v : decl_px(d, s));

                if (d->unit == U_PCT) s->inset_pct |= bit;
                else s->inset_pct &= (u8)~bit;
            }

            break;
        }
        case P_FLEX_GROW:  s->flex_grow = (u8)d->v; break;
        case P_FLEX_SHRINK: s->flex_shrink = (u8)d->v; break;
        case P_JUSTIFY:    s->justify = (u8)d->v; break;
        case P_ALIGN_ITEMS: s->align_items = (u8)d->v; break;
        case P_WRAP:       s->wrap = (u8)d->v; break;
        case P_GAP:        s->gap = clamp16(d->unit == U_AUTO ? 0 : decl_px(d, s)); break;

        case P_FLEX_BASIS:
            s->basis_unit = (u8)(d->unit == U_PCT ? U_PCT : (d->unit == U_AUTO ? U_AUTO : U_PX));
            s->basis = d->unit == U_PCT ? d->v : decl_px(d, s);
            break;

        case P_LINE_H:
            if (d->unit == U_NUM) s->line_h = clamp16((d->v * (i32)s->font_px) / 100);
            else if (d->unit == U_PCT) s->line_h = clamp16((d->v * (i32)s->font_px) / 100);
            else s->line_h = clamp16(decl_px(d, s));
            break;

        case P_MARGIN_T: case P_MARGIN_R: case P_MARGIN_B: case P_MARGIN_L: {
            u32 side = d->prop - P_MARGIN_T;

            if (d->unit == U_AUTO) {
                s->margin[side] = 0;
                s->margin_auto |= (u8)(1u << side);
            } else {
                s->margin[side] = clamp16(decl_px(d, s));
                s->margin_auto &= (u8)~(1u << side);
            }

            break;
        }

        case P_PADDING_T: case P_PADDING_R: case P_PADDING_B: case P_PADDING_L: {
            i32 px = decl_px(d, s);

            s->padding[d->prop - P_PADDING_T] = clamp16(px < 0 ? 0 : px);
            break;
        }

        case P_BORDER_T: case P_BORDER_R: case P_BORDER_B: case P_BORDER_L:
            s->border[d->prop - P_BORDER_T] = (u8)(d->v < 0 ? 0 : (d->v > 20 ? 20 : d->v));
            break;

        case P_BORDER_COLOR: s->border_color = (u32)d->v; break;

        case P_WIDTH:
            s->width_unit = (u8)(d->unit == U_PCT ? U_PCT : (d->unit == U_AUTO ? U_AUTO : U_PX));
            s->width = d->unit == U_PCT ? d->v : decl_px(d, s);
            break;

        case P_MAX_W:
            s->max_unit = (u8)(d->unit == U_PCT ? U_PCT : (d->unit == U_NONE || d->unit == U_AUTO ? U_NONE : U_PX));
            s->max_width = d->unit == U_PCT ? d->v : decl_px(d, s);
            break;

        case P_MIN_W:
            s->min_unit = (u8)(d->unit == U_PCT ? U_PCT : (d->unit == U_AUTO ? U_AUTO : U_PX));
            s->min_width = d->unit == U_PCT ? d->v : decl_px(d, s);
            break;

        case P_HEIGHT:
            s->height_unit = (u8)(d->unit == U_AUTO ? U_AUTO : U_PX);
            s->height = d->unit == U_PCT ? 0 : decl_px(d, s);

            if (d->unit == U_PCT) {
                s->height_unit = U_AUTO;
            }

            break;

        default:
            break;
    }
}

static decl_t g_temp[TEMP_DECLS];

/* Applies a list of declarations; deferred ones are worked out here. */
static void apply_list(style_t* s, const style_t* parent, const decl_t* list, u32 count,
                       int want_important, int font_pass) {
    for (u32 i = 0; i < count; i++) {
        const decl_t* d = &list[i];

        if ((d->important != 0) != (want_important != 0)) {
            continue;
        }

        if (d->prop != P_DEFERRED) {
            apply_decl(s, parent, d, font_pass);
            continue;
        }

        const char* name = g_var_pool + d->v;
        const char* value = name + strlen(name) + 1u;
        char resolved[320];
        u32 n = css_substitute(value, (u32)strlen(value), resolved, sizeof(resolved), 0);

        if (!n) {
            continue;
        }

        decl_out_t out;
        decl_t local[12];

        out.list = local;
        out.count = 0;
        out.max = 12;

        u32 keep = g_var_pool_len;

        css_declaration(&out, name, (u32)strlen(name), resolved, n, 0);
        g_var_pool_len = keep;      /* nothing new is kept from this */

        for (u32 k = 0; k < out.count; k++) {
            if (local[k].prop != P_DEFERRED) {
                apply_decl(s, parent, &local[k], font_pass);
            }
        }
    }
}

static void ua_defaults(u32 index, style_t* s, const style_t* parent);

/* The style of the page before any element: what the root inherits. */
static void style_root(style_t* s) {
    memset(s, 0, sizeof(*s));

    s->display = D_BLOCK;
    s->font_px = 16;
    s->color = 0x00202122u;
    s->bg = NO_COLOR;
    s->border_color = 0x00808080u;
    s->width_unit = U_AUTO;
    s->max_unit = U_NONE;
    s->min_unit = U_AUTO;
    s->height_unit = U_AUTO;
    s->basis_unit = U_AUTO;
    s->flex_shrink = 1;
}

/* Works out the style of element `index` from its parent's style. */
static void style_compute(u32 index, style_t* s, const style_t* parent) {
    const node_t* n = &g_nodes[index];

    /* inherited from the parent */
    style_root(s);
    s->display = D_INLINE;
    s->font_px = parent->font_px;
    s->color = parent->color;
    s->bold = parent->bold;
    s->italic = parent->italic;
    s->underline = parent->underline;
    s->align = parent->align;
    s->white = parent->white;
    s->list_none = parent->list_none;
    s->hidden = parent->hidden;
    s->mono = parent->mono;
    s->upper = parent->upper;
    s->line_h = parent->line_h;
    s->border_color = parent->color;

    ua_defaults(index, s, parent);

    collect_rules(index, 0);

    /* the element's own style="" */
    decl_out_t own;
    char text[1024];

    own.list = g_temp;
    own.count = 0;
    own.max = TEMP_DECLS;

    u32 pool_before = g_var_pool_len;

    if (n->has_style && attribute(g_src + n->src, n->src_len, "style", text, sizeof(text))) {
        /* pieces that use variables go into the pool until applied below */
        css_declarations(&own, text, (u32)strlen(text), 0);
    }

    for (int font_pass = 1; font_pass >= 0; font_pass--) {
        for (u32 i = 0; i < g_matched_count; i++) {
            const rule_t* r = &g_rules[g_matched[i]];

            apply_list(s, parent, g_decls + r->decl_start, r->decl_count, 0, font_pass);
        }

        apply_list(s, parent, own.list, own.count, 0, font_pass);

        for (u32 i = 0; i < g_matched_count; i++) {
            const rule_t* r = &g_rules[g_matched[i]];

            apply_list(s, parent, g_decls + r->decl_start, r->decl_count, 1, font_pass);
        }

        apply_list(s, parent, own.list, own.count, 1, font_pass);

        /* sizes in em in the UA defaults were set against the inherited
           size; nothing to redo for them */
    }

    g_var_pool_len = pool_before;

    if (n->hidden_attr) {
        s->display = D_NONE;
    }
}

/*
 * The text a page asks to be put before (pe 1) or after (pe 2) an
 * element's contents with ::before / ::after and "content". Returns the
 * offset + 1 of the raw value in g_var_pool, or 0 for none.
 */
static u32 style_pseudo(u32 index, u32 pe) {
    u32 content = 0;
    int shown = 1;

    if (!g_pe_rules) {
        return 0;
    }

    collect_rules(index, pe);

    for (int important = 0; important <= 1; important++) {
        for (u32 i = 0; i < g_matched_count; i++) {
            const rule_t* r = &g_rules[g_matched[i]];

            for (u32 k = 0; k < r->decl_count; k++) {
                const decl_t* d = &g_decls[r->decl_start + k];

                if ((d->important != 0) != (important != 0)) {
                    continue;
                }

                if (d->prop == P_CONTENT) {
                    content = (u32)d->v + 1u;
                } else if (d->prop == P_DISPLAY) {
                    shown = d->v != D_NONE;
                } else if (d->prop == P_CLIPPED && d->v) {
                    shown = 0;
                } else if (d->prop == P_POSITION && d->v >= 2) {
                    shown = 0;      /* decorations placed by hand: not text */
                }
            }
        }
    }

    return shown ? content : 0;
}
