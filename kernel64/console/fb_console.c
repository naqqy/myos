#include "../k64_console.h"
#include "font8x8.h"
#include "naq_boot_info.h"

#define K64_CELL_W 8u
#define K64_CELL_H 16u

static naq_boot_info_t* g_boot = 0;

static nq_u32 g_cols = 0;
static nq_u32 g_rows = 0;
static nq_u32 g_cursor_col = 0;
static nq_u32 g_cursor_row = 0;

static nq_u32 k64_make_pixel(nq_u8 r, nq_u8 g, nq_u8 b) {
    if (!g_boot) {
        return 0;
    }

    /*
     * UEFI GOP:
     * 0 = RGBx
     * 1 = BGRx
     */
    if (g_boot->framebuffer_pixel_format == 0) {
        return ((nq_u32)r) |
               ((nq_u32)g << 8) |
               ((nq_u32)b << 16);
    }

    return ((nq_u32)b) |
           ((nq_u32)g << 8) |
           ((nq_u32)r << 16);
}

static void k64_put_pixel(nq_u32 x, nq_u32 y, nq_u32 color) {
    if (!g_boot ||
        g_boot->magic != NAQ_BOOT_INFO_MAGIC ||
        g_boot->version != NAQ_BOOT_INFO_VERSION) {
        return;
    }

    if (x >= g_boot->framebuffer_width ||
        y >= g_boot->framebuffer_height) {
        return;
    }

    volatile nq_u32* fb = (volatile nq_u32*)g_boot->framebuffer_base;

    nq_u64 index =
        ((nq_u64)y * g_boot->framebuffer_pixels_per_scanline) + x;

    fb[index] = color;
}

static void k64_fill_rect(
    nq_u32 x,
    nq_u32 y,
    nq_u32 w,
    nq_u32 h,
    nq_u32 color
) {
    if (!g_boot) {
        return;
    }

    if (x >= g_boot->framebuffer_width ||
        y >= g_boot->framebuffer_height) {
        return;
    }

    if (w > g_boot->framebuffer_width - x) {
        w = g_boot->framebuffer_width - x;
    }

    if (h > g_boot->framebuffer_height - y) {
        h = g_boot->framebuffer_height - y;
    }

    for (nq_u32 yy = 0; yy < h; yy++) {
        for (nq_u32 xx = 0; xx < w; xx++) {
            k64_put_pixel(x + xx, y + yy, color);
        }
    }
}

void k64_clear_screen(void) {
    nq_u32 bg = k64_make_pixel(0, 0, 0);

    k64_fill_rect(
        0,
        0,
        g_boot->framebuffer_width,
        g_boot->framebuffer_height,
        bg
    );

    g_cursor_col = 0;
    g_cursor_row = 0;
}

static void k64_draw_char_at(nq_u32 col, nq_u32 row, char c) {
    const unsigned char* glyph;
    nq_u32 px = col * K64_CELL_W;
    nq_u32 py = row * K64_CELL_H;

    nq_u32 fg = k64_make_pixel(234, 234, 234);
    nq_u32 bg = k64_make_pixel(0, 0, 0);

    if (col >= g_cols || row >= g_rows) {
        return;
    }

    glyph = font8x8_get(c);

    for (nq_u32 gy = 0; gy < 8; gy++) {
        unsigned char bits = glyph[gy];

        for (nq_u32 gx = 0; gx < 8; gx++) {
            nq_u32 color = (bits & (0x80u >> gx)) ? fg : bg;

            /*
             * Draw 8x8 font doubled vertically into 8x16 cells.
             */
            k64_put_pixel(px + gx, py + gy * 2, color);
            k64_put_pixel(px + gx, py + gy * 2 + 1, color);
        }
    }
}

static void k64_newline(void) {
    g_cursor_col = 0;
    g_cursor_row++;

    /*
     * Temporary behavior:
     * When reaching the bottom, clear and restart below the header.
     * This avoids ending on a blank black screen.
     */
    if (g_cursor_row >= g_rows) {
        k64_clear_screen();
        g_cursor_col = 0;
        g_cursor_row = 3;
    }
}

void k64_putchar(char c) {
    if (c == '\n') {
        k64_newline();
        return;
    }

    if (c == '\r') {
        g_cursor_col = 0;
        return;
    }

    if (c == '\t') {
        do {
            k64_putchar(' ');
        } while ((g_cursor_col & 3u) != 0);
        return;
    }

    if ((nq_u8)c < 32) {
        return;
    }

    k64_draw_char_at(g_cursor_col, g_cursor_row, c);

    g_cursor_col++;

    if (g_cursor_col >= g_cols) {
        k64_newline();
    }
}

void k64_backspace(void) {
    if (!g_boot) {
        return;
    }

    if (g_cursor_col == 0) {
        if (g_cursor_row == 0) {
            return;
        }

        g_cursor_row--;
        g_cursor_col = g_cols - 1;
    } else {
        g_cursor_col--;
    }

    k64_draw_char_at(g_cursor_col, g_cursor_row, ' ');
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
    nq_u32 bar = k64_make_pixel(0, 32, 64);
    nq_u32 accent = k64_make_pixel(0, 220, 255);

    k64_fill_rect(0, 0, g_boot->framebuffer_width, 40, bar);
    k64_fill_rect(0, 39, g_boot->framebuffer_width, 2, accent);

    g_cursor_col = 2;
    g_cursor_row = 1;

    k64_print("NAQ OS 64-bit Kernel");

    g_cursor_col = 0;
    g_cursor_row = 3;
}

void k64_console_init(naq_boot_info_t* boot) {
    g_boot = boot;

    if (!g_boot ||
        g_boot->magic != NAQ_BOOT_INFO_MAGIC ||
        g_boot->version != NAQ_BOOT_INFO_VERSION) {
        for (;;) {
            __asm__ volatile ("hlt");
        }
    }

    g_cols = g_boot->framebuffer_width / K64_CELL_W;
    g_rows = g_boot->framebuffer_height / K64_CELL_H;

    if (g_cols == 0) {
        g_cols = 1;
    }

    if (g_rows == 0) {
        g_rows = 1;
    }

    k64_clear_screen();
    k64_draw_header();
}