#include "include/naqos.h"

/*
 * SETTINGS.ELF
 *
 * Desktop settings: wallpaper, accent colour and clock format.
 *
 * Every choice takes effect at once. It is also saved to the USB stick the
 * system started from, so it is still there after a restart; without a
 * writable stick the choice lasts until shutdown.
 *
 * Mouse: click an option.
 * Keys:  Q or Esc closes the window.
 */

#define WIN_W 470u
#define WIN_H 400u

#define WALLPAPER_COUNT 3
#define ACCENT_COUNT 7

#define COL_PAPER    0x00F6FBFEu
#define COL_TEXT     0x0016445Eu
#define COL_TEXT_DIM 0x006890A4u
#define COL_EDGE     0x002A86B4u
#define COL_RULE     0x00C8E2F0u
#define COL_SELECT   0x00FF9A1Eu

#define MARGIN 20

#define WALL_Y 44
#define WALL_W 130
#define WALL_H 66
#define WALL_GAP 14

#define ACCENT_Y 176
#define ACCENT_SIZE 40
#define ACCENT_GAP 14

#define CLOCK_Y 280
#define CLOCK_W 110
#define CLOCK_H 28
#define CLOCK_GAP 12

static u32 g_pixels[WIN_W * WIN_H];

static int g_wallpaper = 0;
static int g_accent = 0;
static int g_clock12 = 0;

static char g_status[96] = "";
static naq_gfx_info_t g_screen;

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

/* -------------------------------------------------------------------------- */
/* Drawing                                                                     */
/* -------------------------------------------------------------------------- */

static u32 make_rgb(u32 r, u32 g, u32 b) {
    return (r << 16) | (g << 8) | b;
}

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
    i32 x1 = x + w > (i32)WIN_W ? (i32)WIN_W : x + w;
    i32 y1 = y + h > (i32)WIN_H ? (i32)WIN_H : y + h;

    for (i32 yy = y0; yy < y1; yy++) {
        u32* row = g_pixels + (u32)yy * WIN_W;

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
    gfx_text(g_pixels, WIN_W, WIN_H, x, y, s, color);
}

/*
 * Marks the chosen option: a thick orange outline just outside it.
 */
static void selection(i32 x, i32 y, i32 w, i32 h) {
    for (i32 i = 2; i <= 4; i++) {
        frame(x - i, y - i, w + i * 2, h + i * 2, COL_SELECT);
    }
}

static const char* wallpaper_name(int index) {
    switch (index) {
        case 0:  return "Day";
        case 1:  return "Sunset";
        default: return "Night";
    }
}

static const char* accent_name(int index) {
    switch (index) {
        case 0:  return "Blue";
        case 1:  return "Teal";
        case 2:  return "Green";
        case 3:  return "Purple";
        case 4:  return "Pink";
        case 5:  return "Orange";
        default: return "Graphite";
    }
}

/*
 * The colour an accent turns the desktop's blue glass into. Mirrors the
 * channel swaps the desktop itself uses.
 */
static u32 accent_color(int index, u32 r, u32 g, u32 b) {
    switch (index) {
        case 1:  return make_rgb(r, b, (g + b) / 2u);
        case 2:  return make_rgb(r, b, g);
        case 3:  return make_rgb(g, r, b);
        case 4:  return make_rgb(b, r, g);
        case 5:  return make_rgb(b, g, r);
        case 6: {
            u32 v = (r + g + b) / 3u;
            return make_rgb(v, v, v + (255u - v) / 10u);
        }
        default: return make_rgb(r, g, b);
    }
}

static i32 wallpaper_x(int index) {
    return MARGIN + index * (WALL_W + WALL_GAP);
}

static i32 accent_x(int index) {
    return MARGIN + index * (ACCENT_SIZE + ACCENT_GAP);
}

static i32 clock_x(int index) {
    return MARGIN + index * (CLOCK_W + CLOCK_GAP);
}

/*
 * A thumbnail of a wallpaper variant: sky, sun or moon, and a hill.
 */
static void wallpaper_tile(int index) {
    i32 x = wallpaper_x(index);
    i32 y = WALL_Y;
    i32 sky_h = (WALL_H * 62) / 100;

    u32 sky_top;
    u32 sky_low;
    u32 land_top;
    u32 land_low;
    u32 light;

    if (index == 0) {
        sky_top = 0x000854D6u;
        sky_low = 0x00AAE8FFu;
        land_top = 0x0090E030u;
        land_low = 0x002E9C1Cu;
        light = 0x00FFFFFFu;
    } else if (index == 1) {
        sky_top = 0x002E469Fu;
        sky_low = 0x00E89A62u;
        land_top = 0x00B49620u;
        land_low = 0x00584C10u;
        light = 0x00FFE2A8u;
    } else {
        sky_top = 0x00020E28u;
        sky_low = 0x001C3C72u;
        land_top = 0x00183C16u;
        land_low = 0x00081808u;
        light = 0x00F0F4FFu;
    }

    gradient(x, y, WALL_W, sky_h, sky_top, sky_low);
    gradient(x, y + sky_h, WALL_W, WALL_H - sky_h, land_top, land_low);

    /*
     * Sun or moon.
     */
    rect(x + WALL_W - 30, y + 9, 10, 10, light);
    rect(x + WALL_W - 32, y + 11, 14, 6, light);
    rect(x + WALL_W - 28, y + 7, 6, 14, light);

    if (index == 2) {
        static const i32 stars[6][2] = {
            { 12, 8 }, { 34, 20 }, { 52, 6 }, { 70, 16 }, { 22, 30 }, { 60, 30 }
        };

        for (int i = 0; i < 6; i++) {
            rect(x + stars[i][0], y + stars[i][1], 2, 2, 0x00FFFFFFu);
        }
    }

    frame(x, y, WALL_W, WALL_H, COL_EDGE);

    i32 len = (i32)strlen(wallpaper_name(index));

    text(x + (WALL_W - len * GFX_FONT_W) / 2, y + WALL_H + 8, wallpaper_name(index),
         index == g_wallpaper ? COL_TEXT : COL_TEXT_DIM);

    if (index == g_wallpaper) {
        selection(x, y, WALL_W, WALL_H);
    }
}

static void accent_tile(int index) {
    i32 x = accent_x(index);
    i32 y = ACCENT_Y;

    /*
     * The same top-to-bottom glass shading the window frames have.
     */
    gradient(x, y, ACCENT_SIZE, ACCENT_SIZE,
             accent_color(index, 120, 200, 250),
             accent_color(index, 20, 110, 220));
    rect(x + 3, y + 3, ACCENT_SIZE - 6, ACCENT_SIZE / 2 - 4,
         mix(accent_color(index, 120, 200, 250), 0x00FFFFFFu, 1, 2));
    frame(x, y, ACCENT_SIZE, ACCENT_SIZE, accent_color(index, 10, 70, 150));

    if (index == g_accent) {
        selection(x, y, ACCENT_SIZE, ACCENT_SIZE);
    }
}

static void clock_tile(int index) {
    i32 x = clock_x(index);
    const char* label = index ? "12-hour" : "24-hour";
    i32 len = (i32)strlen(label);

    gradient(x, CLOCK_Y, CLOCK_W, CLOCK_H, 0x00FFFFFFu, 0x008CD4F0u);
    rect(x + 1, CLOCK_Y + 1, CLOCK_W - 2, CLOCK_H / 2 - 1, 0x00F4FDFFu);
    frame(x, CLOCK_Y, CLOCK_W, CLOCK_H, COL_EDGE);

    text(x + (CLOCK_W - len * GFX_FONT_W) / 2, CLOCK_Y + (CLOCK_H - GFX_FONT_H) / 2,
         label, COL_TEXT);

    if (index == g_clock12) {
        selection(x, CLOCK_Y, CLOCK_W, CLOCK_H);
    }
}

static void heading(i32 y, const char* label) {
    text(MARGIN, y, label, COL_TEXT);
    rect(MARGIN + ((i32)strlen(label) + 1) * GFX_FONT_W, y + 8,
         (i32)WIN_W - MARGIN * 2 - ((i32)strlen(label) + 1) * GFX_FONT_W, 1, COL_RULE);
}

static void paint(void) {
    char line[96];

    rect(0, 0, WIN_W, WIN_H, COL_PAPER);

    heading(16, "Wallpaper");

    for (int i = 0; i < WALLPAPER_COUNT; i++) {
        wallpaper_tile(i);
    }

    heading(148, "Accent colour");

    for (int i = 0; i < ACCENT_COUNT; i++) {
        accent_tile(i);
    }

    line[0] = 0;
    str_append(line, accent_name(g_accent), sizeof(line));
    text(MARGIN, ACCENT_Y + ACCENT_SIZE + 10, line, COL_TEXT_DIM);

    heading(256, "Clock");

    for (int i = 0; i < 2; i++) {
        clock_tile(i);
    }

    heading(326, "Display");

    line[0] = 0;
    str_append(line, "Screen is ", sizeof(line));
    append_number(line, g_screen.width, sizeof(line));
    str_append(line, " x ", sizeof(line));
    append_number(line, g_screen.height, sizeof(line));
    str_append(line, ". Press D at startup to change it.", sizeof(line));
    text(MARGIN, 346, line, COL_TEXT_DIM);

    /*
     * Status bar.
     */
    gradient(0, (i32)WIN_H - 24, WIN_W, 24, 0x00EAF8FEu, 0x00B8E2F4u);
    rect(0, (i32)WIN_H - 24, WIN_W, 1, COL_EDGE);
    text(8, (i32)WIN_H - 20, g_status, COL_TEXT);
}

/* -------------------------------------------------------------------------- */
/* Input                                                                       */
/* -------------------------------------------------------------------------- */

static void apply(int id, int value) {
    int result = setting_set(id, value);

    if (result == NAQ_SETTING_SAVED) {
        str_copy(g_status, "Saved.", sizeof(g_status));
    } else if (result == NAQ_SETTING_APPLIED) {
        str_copy(g_status, "Applied until shutdown (no writable boot stick to save to).",
                 sizeof(g_status));
    } else {
        str_copy(g_status, "That setting could not be changed.", sizeof(g_status));
    }

    g_wallpaper = setting_get(NAQ_SETTING_WALLPAPER);
    g_accent = setting_get(NAQ_SETTING_ACCENT);
    g_clock12 = setting_get(NAQ_SETTING_CLOCK12);
}

static int inside(i32 x, i32 y, i32 rx, i32 ry, i32 rw, i32 rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static int handle_click(i32 x, i32 y) {
    for (int i = 0; i < WALLPAPER_COUNT; i++) {
        if (inside(x, y, wallpaper_x(i), WALL_Y, WALL_W, WALL_H + 24)) {
            apply(NAQ_SETTING_WALLPAPER, i);
            return 1;
        }
    }

    for (int i = 0; i < ACCENT_COUNT; i++) {
        if (inside(x, y, accent_x(i), ACCENT_Y, ACCENT_SIZE, ACCENT_SIZE)) {
            apply(NAQ_SETTING_ACCENT, i);
            return 1;
        }
    }

    for (int i = 0; i < 2; i++) {
        if (inside(x, y, clock_x(i), CLOCK_Y, CLOCK_W, CLOCK_H)) {
            apply(NAQ_SETTING_CLOCK12, i);
            return 1;
        }
    }

    return 0;
}

int main(void) {
    int win = win_create(220, 110, WIN_W, WIN_H, "Settings");

    if (win < 0) {
        puts("settings: win_create failed\n");
        return 1;
    }

    gfx_info(&g_screen);

    g_wallpaper = setting_get(NAQ_SETTING_WALLPAPER);
    g_accent = setting_get(NAQ_SETTING_ACCENT);
    g_clock12 = setting_get(NAQ_SETTING_CLOCK12);

    str_copy(g_status, "Click an option to apply it.", sizeof(g_status));

    /*
     * The window can be resized; our frame is then stretched to fit, so
     * clicks are scaled back to frame pixels.
     */
    u32 window_w = WIN_W;
    u32 window_h = WIN_H;

    int running = 1;
    int dirty = 1;

    while (running) {
        naq_win_event_t ev;

        while (win_get_event(win, &ev) == 1) {
            if (ev.type == NAQ_WIN_EVENT_CLOSE) {
                running = 0;
            } else if (ev.type == NAQ_WIN_EVENT_KEY) {
                if (ev.key == 'q' || ev.key == 'Q' || ev.key == NAQ_KEY_ESCAPE) {
                    running = 0;
                }
            } else if (ev.type == NAQ_WIN_EVENT_RESIZE) {
                window_w = ev.w > 0 ? (u32)ev.w : 1u;
                window_h = ev.h > 0 ? (u32)ev.h : 1u;
                dirty = 1;
            } else if (ev.type == NAQ_WIN_EVENT_MOUSE) {
                if (ev.w == NAQ_MOUSE_PRESS &&
                    ev.h == (i32)NAQ_MOUSE_BUTTON_LEFT) {
                    i32 x = (i32)(((i64)ev.x * (i64)WIN_W) / (i64)window_w);
                    i32 y = (i32)(((i64)ev.y * (i64)WIN_H) / (i64)window_h);

                    if (handle_click(x, y)) {
                        dirty = 1;
                    }
                }
            } else if (ev.type == NAQ_WIN_EVENT_FOCUS) {
                dirty = 1;
            }
        }

        if (dirty) {
            paint();
            win_present(win, g_pixels, WIN_W, WIN_H);
            dirty = 0;
        }

        sleep_ms(16);
    }

    win_destroy(win);

    return 0;
}
