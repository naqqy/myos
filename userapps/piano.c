#include "include/naqos.h"

#include <math.h>

/*
 * PIANO.ELF
 *
 * A small keyboard for trying out sound.
 *
 * Keys (the middle row of the keyboard is the white keys):
 *   A S D F G H J K L ;     C D E F G A B C D E
 *   W E   T Y U   O P       the black keys between them
 *   Z / X                   octave down / up
 *   1 2 3 4                 tone: bell, pure, square, triangle
 *   Esc                     quit
 *
 * The mouse plays the keys too. The buttons along the bottom play the
 * system sounds, and "Tune" plays a short melody that this program
 * computes itself and sends as samples, to exercise that path as well.
 */

#define WIN_W 600u
#define WIN_H 300u

#define KEYS_X 20
#define KEYS_Y 60
#define WHITE_W 40
#define WHITE_H 150
#define BLACK_W 26
#define BLACK_H 92
#define WHITE_COUNT 14

#define BUTTON_Y 226
#define BUTTON_W 76
#define BUTTON_H 26
#define BUTTON_COUNT 7

#define COL_PAPER 0x00F6FBFEu
#define COL_TEXT  0x0016445Eu
#define COL_DIM   0x006890A4u
#define COL_EDGE  0x002A86B4u
#define COL_LIT   0x00FFB040u

static u32 g_pixels[WIN_W * WIN_H];

static int g_octave = 4;
static u32 g_wave = NAQ_WAVE_SOFT;
static int g_has_sound = 0;

/* Which key (semitones above the octave's C) is lit, and until when. */
static int g_lit = -1;
static u64 g_lit_until = 0;

/* Semitones above C for the 14 white keys (two octaves). */
static const int g_white_note[WHITE_COUNT] = { 0, 2, 4, 5, 7, 9, 11, 12, 14, 16, 17, 19, 21, 23 };

/* Pitches of the octave starting at middle C (octave 4), in Hz * 100. */
static const u32 g_pitch[12] = {
    26163, 27718, 29366, 31113, 32963, 34923, 36999, 39200, 41530, 44000, 46616, 49388
};

static const char* const g_buttons[BUTTON_COUNT] = {
    "Start", "Click", "Error", "Notify", "Open", "Close", "Tune"
};

static const char* const g_wave_names[4] = { "pure", "bell", "square", "triangle" };

/* -------------------------------------------------------------------------- */
/* Drawing                                                                     */
/* -------------------------------------------------------------------------- */

static void rect(i32 x, i32 y, i32 w, i32 h, u32 color) {
    for (i32 yy = y < 0 ? 0 : y; yy < y + h && yy < (i32)WIN_H; yy++) {
        for (i32 xx = x < 0 ? 0 : x; xx < x + w && xx < (i32)WIN_W; xx++) {
            g_pixels[(u32)yy * WIN_W + (u32)xx] = color;
        }
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

/* Is there a black key to the right of white key `i`? */
static int has_black(int i) {
    int step = i % 7;

    return step != 2 && step != 6 && i < WHITE_COUNT - 1;
}

static void paint(void) {
    char line[96];

    rect(0, 0, WIN_W, WIN_H, COL_PAPER);

    if (!g_has_sound) {
        text(20, 16, "No sound hardware was found, so nothing will be heard.", 0x00C03020u);
    } else {
        text(20, 16, "Mouse, or keys A S D F G H J K L ;   black keys: W E T Y U O P", COL_TEXT);
    }

    line[0] = 0;

    const char* parts[5] = { "Octave ", "", "  (Z / X)     Tone: ", "", "  (1-4)" };
    char digit[2] = { (char)('0' + g_octave), 0 };

    parts[1] = digit;
    parts[3] = g_wave_names[g_wave & 3u];

    u32 n = 0;

    for (int p = 0; p < 5; p++) {
        for (const char* c = parts[p]; *c && n + 1 < sizeof(line); c++) {
            line[n++] = *c;
        }
    }

    line[n] = 0;
    text(20, 36, line, COL_DIM);

    for (int i = 0; i < WHITE_COUNT; i++) {
        i32 x = KEYS_X + i * WHITE_W;

        rect(x, KEYS_Y, WHITE_W, WHITE_H, g_lit == g_white_note[i] ? COL_LIT : 0x00FFFFFFu);
        frame(x, KEYS_Y, WHITE_W, WHITE_H, COL_EDGE);
    }

    for (int i = 0; i < WHITE_COUNT; i++) {
        if (has_black(i)) {
            i32 x = KEYS_X + (i + 1) * WHITE_W - BLACK_W / 2;

            rect(x, KEYS_Y, BLACK_W, BLACK_H, g_lit == g_white_note[i] + 1 ? COL_LIT : 0x00203040u);
        }
    }

    for (int i = 0; i < BUTTON_COUNT; i++) {
        i32 x = KEYS_X + i * (BUTTON_W + 4);
        i32 len = (i32)strlen(g_buttons[i]);

        rect(x, BUTTON_Y, BUTTON_W, BUTTON_H, 0x00D8F0FCu);
        frame(x, BUTTON_Y, BUTTON_W, BUTTON_H, COL_EDGE);
        text(x + (BUTTON_W - len * GFX_FONT_W) / 2, BUTTON_Y + 5, g_buttons[i], COL_TEXT);
    }

    text(20, 268, "The buttons play the system sounds; Tune sends computed samples.", COL_DIM);
}

/* -------------------------------------------------------------------------- */
/* Sound                                                                       */
/* -------------------------------------------------------------------------- */

/* Plays the key `semitone` steps above the current octave's C. */
static void play_key(int semitone) {
    u32 hz100 = g_pitch[semitone % 12];
    int octave = g_octave + semitone / 12;

    for (int o = 4; o < octave; o++) hz100 *= 2u;
    for (int o = octave; o < 4; o++) hz100 /= 2u;

    sound_tone((hz100 + 50u) / 100u, g_wave == NAQ_WAVE_SOFT ? 900u : 350u, 70, g_wave);

    g_lit = semitone;
    g_lit_until = get_ticks() + 180u;
}

/*
 * The tune: a melody computed here as samples and handed to the system a
 * piece at a time, the way a music or video player would.
 */
#define TUNE_NOTES 8
#define TUNE_NOTE_FRAMES (NAQ_SOUND_RATE / 5u)      /* 0.2 s each */
#define TUNE_FRAMES (TUNE_NOTES * TUNE_NOTE_FRAMES)

static const double g_tune[TUNE_NOTES] = {
    523.25, 587.33, 659.25, 783.99, 659.25, 783.99, 1046.50, 783.99
};

static u32 g_tune_at = TUNE_FRAMES;     /* next frame to send; at the end = idle */
static i16 g_chunk[1024 * 2];
static u32 g_chunk_frames = 0;          /* frames in g_chunk not yet accepted */
static u32 g_chunk_sent = 0;

static void tune_pump(void) {
    for (int rounds = 0; rounds < 8; rounds++) {
        if (g_chunk_sent == g_chunk_frames) {
            if (g_tune_at >= TUNE_FRAMES) {
                return;
            }

            /* compute the next piece */
            g_chunk_frames = 0;
            g_chunk_sent = 0;

            while (g_chunk_frames < 1024u && g_tune_at < TUNE_FRAMES) {
                u32 note = g_tune_at / TUNE_NOTE_FRAMES;
                u32 within = g_tune_at % TUNE_NOTE_FRAMES;
                double t = (double)within / NAQ_SOUND_RATE;
                double fade = 1.0 - (double)within / TUNE_NOTE_FRAMES;
                double attack = within < 200u ? within / 200.0 : 1.0;
                double wave = sin(6.283185307179586 * g_tune[note] * t) +
                              0.3 * sin(6.283185307179586 * g_tune[note] * 2.0 * t);
                double value = wave * fade * fade * attack * 9000.0;

                /* swing gently from the left speaker to the right */
                double pan = (double)g_tune_at / TUNE_FRAMES;

                g_chunk[g_chunk_frames * 2u] = (i16)(value * (1.0 - pan * 0.8));
                g_chunk[g_chunk_frames * 2u + 1u] = (i16)(value * (0.2 + pan * 0.8));

                g_chunk_frames++;
                g_tune_at++;
            }
        }

        u32 taken = sound_write(g_chunk + g_chunk_sent * 2u, g_chunk_frames - g_chunk_sent);

        g_chunk_sent += taken;

        if (g_chunk_sent < g_chunk_frames) {
            return;     /* the queue is full for now */
        }
    }
}

static void press_button(int index) {
    if (index == BUTTON_COUNT - 1) {
        g_tune_at = 0;
        g_chunk_frames = 0;
        g_chunk_sent = 0;
    } else {
        sound_play((u32)index);
    }
}

/* -------------------------------------------------------------------------- */
/* Input                                                                       */
/* -------------------------------------------------------------------------- */

static int key_to_semitone(u32 key) {
    static const char white[] = "asdfghjkl;";
    static const char black[] = "we tyu op";

    if (key >= 'A' && key <= 'Z') {
        key += 32u;
    }

    for (int i = 0; white[i]; i++) {
        if (key == (u32)white[i]) {
            return g_white_note[i];
        }
    }

    for (int i = 0; black[i]; i++) {
        if (black[i] != ' ' && key == (u32)black[i]) {
            return g_white_note[i] + 1;
        }
    }

    return -1;
}

static void handle_click(i32 x, i32 y) {
    if (y >= BUTTON_Y && y < BUTTON_Y + BUTTON_H) {
        for (int i = 0; i < BUTTON_COUNT; i++) {
            i32 bx = KEYS_X + i * (BUTTON_W + 4);

            if (x >= bx && x < bx + BUTTON_W) {
                press_button(i);
                return;
            }
        }

        return;
    }

    if (y < KEYS_Y || y >= KEYS_Y + WHITE_H || x < KEYS_X) {
        return;
    }

    /* black keys lie on top of the white ones */
    if (y < KEYS_Y + BLACK_H) {
        for (int i = 0; i < WHITE_COUNT; i++) {
            i32 bx = KEYS_X + (i + 1) * WHITE_W - BLACK_W / 2;

            if (has_black(i) && x >= bx && x < bx + BLACK_W) {
                play_key(g_white_note[i] + 1);
                return;
            }
        }
    }

    int index = (x - KEYS_X) / WHITE_W;

    if (index < WHITE_COUNT) {
        play_key(g_white_note[index]);
    }
}

int main(void) {
    int win = win_create(240, 200, WIN_W, WIN_H, "Piano");

    if (win < 0) {
        puts("piano: win_create failed\n");
        return 1;
    }

    g_has_sound = sound_present();

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
                int semitone = key_to_semitone(ev.key);

                if (ev.key == NAQ_KEY_ESCAPE) {
                    running = 0;
                } else if (semitone >= 0) {
                    play_key(semitone);
                } else if ((ev.key == 'z' || ev.key == 'Z') && g_octave > 2) {
                    g_octave--;
                } else if ((ev.key == 'x' || ev.key == 'X') && g_octave < 6) {
                    g_octave++;
                } else if (ev.key == '1') {
                    g_wave = NAQ_WAVE_SOFT;
                } else if (ev.key == '2') {
                    g_wave = NAQ_WAVE_SINE;
                } else if (ev.key == '3') {
                    g_wave = NAQ_WAVE_SQUARE;
                } else if (ev.key == '4') {
                    g_wave = NAQ_WAVE_TRIANGLE;
                }

                dirty = 1;
            } else if (ev.type == NAQ_WIN_EVENT_RESIZE) {
                window_w = ev.w > 0 ? (u32)ev.w : 1u;
                window_h = ev.h > 0 ? (u32)ev.h : 1u;
                dirty = 1;
            } else if (ev.type == NAQ_WIN_EVENT_MOUSE) {
                if (ev.w == NAQ_MOUSE_PRESS && ev.h == (i32)NAQ_MOUSE_BUTTON_LEFT) {
                    handle_click((i32)(((i64)ev.x * (i64)WIN_W) / (i64)window_w),
                                 (i32)(((i64)ev.y * (i64)WIN_H) / (i64)window_h));
                    dirty = 1;
                }
            } else if (ev.type == NAQ_WIN_EVENT_FOCUS) {
                dirty = 1;
            }
        }

        tune_pump();

        if (g_lit >= 0 && get_ticks() >= g_lit_until) {
            g_lit = -1;
            dirty = 1;
        }

        if (dirty) {
            paint();
            win_present(win, g_pixels, WIN_W, WIN_H);
            dirty = 0;
        }

        sleep_ms(10);
    }

    win_destroy(win);

    return 0;
}
