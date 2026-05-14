#include "include/naqos.h"

#define W 320u
#define H 200u

static u32 g_pixels_a[W * H];
static u32 g_pixels_b[W * H];

int main(void) {
    naq_gfx_info_t info;
    naq_gfx_buffer_t front;
    naq_gfx_buffer_t back;

    gfx_set_mode(GFX_MODE_GRAPHICS);

    puts("Backbuffer test\n");

    if (gfx_info(&info) < 0) {
        puts("gfx_info failed\n");
        return 1;
    }

    gfx_buffer_init(&front, g_pixels_a, W, H);
    gfx_buffer_init(&back, g_pixels_b, W, H);

    gfx_clear(0x00102030);

    for (u32 frame = 0; frame < 180; frame++) {
        naq_gfx_buffer_t* draw =
            (frame & 1) ? &front : &back;

        gfx_buffer_clear(draw, 0x00002040);

        gfx_buffer_draw_rect(draw, 8, 8, 304, 184, 0x00005090);
        gfx_buffer_draw_rect(draw, 20 + (frame % 240), 50, 50, 40, 0x0000D0FF);
        gfx_buffer_draw_rect(draw, 100, 120, 120, 25, 0x0000FF90);

        gfx_buffer_present(draw, 80, 80);

        sleep_ms(16);
    }

    puts("Backbuffer test done\n");
    return 0;
}