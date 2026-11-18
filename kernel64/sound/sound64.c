#include "sound64.h"
#include "hda64.h"

#include "../k64_console.h"
#include "../proc/process64.h"
#include "../arch/x86_64/timer64.h"

/*
 * The mixer. See sound64.h.
 *
 * No floating point is used in the kernel, so everything here is whole
 * numbers: samples are 16-bit, levels are fractions of 256 or 65536, and
 * the sine wave comes from a table worked out once at start-up.
 */

#define RATE            HDA64_RATE
#define MAX_VOICES      24u
#define CHUNK_FRAMES    256u
#define QUEUE_FRAMES    32768u                  /* about 0.7 s of app sound */

/* How far ahead of the playing point the mixer keeps the hardware filled. */
#define LEAD_BYTES      (RATE * 4u * 70u / 1000u)   /* 70 ms */
#define RESTART_BYTES   (RATE * 4u * 10u / 1000u)   /* after falling behind */

#define SINE_SIZE       1024u
#define ATTACK_FRAMES   (RATE * 3u / 1000u)     /* 3 ms fade in: no click */
#define RELEASE_FRAMES  (RATE * 8u / 1000u)

typedef struct voice {
    nq_u8 active;
    nq_u8 wave;
    nq_u16 level;           /* 0..256 */
    nq_u32 delay;           /* frames to wait before it starts */
    nq_u32 phase;           /* position in the wave: the whole 32 bits is one cycle */
    nq_u32 step;            /* added to phase every frame */
    nq_u32 total;           /* length in frames */
    nq_u32 done;
} voice_t;

typedef struct note {
    nq_u16 frequency;
    nq_u16 start_ms;
    nq_u16 length_ms;
    nq_u8 volume;
    nq_u8 wave;
} note_t;

/* The system sounds: a few notes each. */
static const note_t g_startup[] = {
    { 523, 0, 520, 46, SOUND64_WAVE_SOFT },         /* C5 */
    { 659, 150, 520, 46, SOUND64_WAVE_SOFT },       /* E5 */
    { 784, 300, 560, 46, SOUND64_WAVE_SOFT },       /* G5 */
    { 1047, 450, 1300, 50, SOUND64_WAVE_SOFT },     /* C6 */
    { 262, 450, 1300, 26, SOUND64_WAVE_SINE },      /* C4 underneath */
};

static const note_t g_click[] = {
    { 1400, 0, 14, 30, SOUND64_WAVE_SINE },
};

static const note_t g_error[] = {
    { 392, 0, 190, 58, SOUND64_WAVE_SOFT },         /* G4 */
    { 277, 160, 420, 58, SOUND64_WAVE_SOFT },       /* C#4: a falling, "wrong" step */
};

static const note_t g_notify[] = {
    { 880, 0, 170, 46, SOUND64_WAVE_SOFT },         /* A5 */
    { 1319, 120, 480, 46, SOUND64_WAVE_SOFT },      /* E6 */
};

static const note_t g_open[] = {
    { 660, 0, 60, 24, SOUND64_WAVE_SINE },
    { 990, 40, 110, 24, SOUND64_WAVE_SINE },
};

static const note_t g_close[] = {
    { 990, 0, 60, 24, SOUND64_WAVE_SINE },
    { 660, 40, 110, 24, SOUND64_WAVE_SINE },
};

static const struct {
    const note_t* notes;
    nq_u32 count;
} g_sounds[SOUND64_COUNT] = {
    { g_startup, sizeof(g_startup) / sizeof(g_startup[0]) },
    { g_click, sizeof(g_click) / sizeof(g_click[0]) },
    { g_error, sizeof(g_error) / sizeof(g_error[0]) },
    { g_notify, sizeof(g_notify) / sizeof(g_notify[0]) },
    { g_open, sizeof(g_open) / sizeof(g_open[0]) },
    { g_close, sizeof(g_close) / sizeof(g_close[0]) },
};

/* Volume levels 0..9 as a fraction of 256, evenly spaced to the ear. */
static const nq_u16 g_volume_gain[10] = { 0, 5, 10, 18, 30, 48, 74, 110, 168, 256 };

static int g_present = 0;         /* something can be heard: a sound card or a capture sink */
static int g_card = 0;            /* an HD Audio card is running */
static int g_started = 0;
static nq_u32 g_volume = 7;

#define CAPTURE_FRAMES 16384u

static int g_capture_on = 0;
static nq_s16 g_capture[CAPTURE_FRAMES * 2u];
static nq_u32 g_capture_read = 0;
static nq_u32 g_capture_count = 0;
static nq_u64 g_clock_tick = 0;   /* when the clock-driven mixer last ran */
static int g_effects = 1;

static nq_s16 g_sine[SINE_SIZE];
static voice_t g_voices[MAX_VOICES];

/* Samples queued by apps: a ring of stereo frames. */
static nq_s16 g_queue[QUEUE_FRAMES * 2u];
static nq_u32 g_queue_read = 0;
static nq_u32 g_queue_count = 0;

static nq_u32 g_write = 0;          /* where in the hardware ring to write next */
static nq_u64 g_underruns = 0;
static nq_s32 g_mix[CHUNK_FRAMES * 2u];

/* -------------------------------------------------------------------------- */
/* Waves                                                                       */
/* -------------------------------------------------------------------------- */

/*
 * One cycle of a sine wave, by stepping a point round a circle with
 * whole-number arithmetic (x and y in units of 2^-30).
 */
static void sine_build(void) {
    nq_s64 x = 1ll << 30;       /* cosine */
    nq_s64 y = 0;               /* sine */
    const nq_s64 turn = 6588387;    /* 2 * sin(pi / 1024) * 2^30 */

    for (nq_u32 i = 0; i < SINE_SIZE; i++) {
        nq_s64 value = (y * 32767) >> 30;

        if (value > 32767) value = 32767;
        if (value < -32767) value = -32767;

        g_sine[i] = (nq_s16)value;

        x -= (turn * y) >> 30;
        y += (turn * x) >> 30;
    }
}

static nq_s32 sine_at(nq_u32 phase) {
    return g_sine[phase >> 22];
}

/* One sample of a voice, -32767..32767, before its level is applied. */
static nq_s32 voice_sample(const voice_t* v) {
    nq_u32 p = v->phase;

    switch (v->wave) {
        case SOUND64_WAVE_SOFT:
            return (sine_at(p) * 8 + sine_at(p * 2u) * 2 + sine_at(p * 3u)) / 11;

        case SOUND64_WAVE_SQUARE:
            return (p & 0x80000000u) ? -12000 : 12000;

        case SOUND64_WAVE_TRIANGLE: {
            nq_s32 t = (nq_s32)(p >> 16);       /* 0..65535 */

            t = t < 32768 ? t : 65535 - t;      /* up, then down */

            return (t - 16384) * 2;
        }

        default:
            return sine_at(p);
    }
}

/* How loud a voice is at this moment, 0..65536. */
static nq_u32 voice_envelope(const voice_t* v) {
    nq_u32 left = v->total - v->done;
    nq_u32 gain = 65536u;

    if (v->wave == SOUND64_WAVE_SOFT) {
        /* struck, then dying away: the square of what is left */
        nq_u64 part = ((nq_u64)left << 16) / v->total;

        gain = (nq_u32)((part * part) >> 16);
    } else if (left < RELEASE_FRAMES) {
        gain = (nq_u32)(((nq_u64)left << 16) / RELEASE_FRAMES);
    }

    if (v->done < ATTACK_FRAMES) {
        gain = (nq_u32)(((nq_u64)gain * v->done) / ATTACK_FRAMES);
    }

    return gain;
}

/* -------------------------------------------------------------------------- */
/* Mixing                                                                      */
/* -------------------------------------------------------------------------- */

/* Adds `frames` frames of everything that is playing into g_mix. */
static void mix_chunk(nq_u32 frames) {
    for (nq_u32 i = 0; i < frames * 2u; i++) {
        g_mix[i] = 0;
    }

    for (nq_u32 n = 0; n < MAX_VOICES; n++) {
        voice_t* v = &g_voices[n];

        if (!v->active) {
            continue;
        }

        for (nq_u32 i = 0; i < frames; i++) {
            if (v->delay) {
                v->delay--;
                continue;
            }

            if (v->done >= v->total) {
                v->active = 0;
                break;
            }

            nq_s64 s = voice_sample(v);

            s = (s * (nq_s64)voice_envelope(v)) >> 16;
            s = (s * v->level) >> 8;

            /* tones play at half scale, so several at once do not clip */
            g_mix[i * 2u] += (nq_s32)(s / 2);
            g_mix[i * 2u + 1u] += (nq_s32)(s / 2);

            v->phase += v->step;
            v->done++;
        }
    }

    for (nq_u32 i = 0; i < frames && g_queue_count; i++) {
        g_mix[i * 2u] += g_queue[g_queue_read * 2u];
        g_mix[i * 2u + 1u] += g_queue[g_queue_read * 2u + 1u];

        g_queue_read = (g_queue_read + 1u) % QUEUE_FRAMES;
        g_queue_count--;
    }
}

/* Adds mixed, volume-adjusted frames to the capture queue. */
static void capture_push(const nq_s32* mix, nq_u32 frames, nq_u32 gain) {
    for (nq_u32 i = 0; i < frames && g_capture_count < CAPTURE_FRAMES; i++) {
        nq_u32 at = (g_capture_read + g_capture_count) % CAPTURE_FRAMES;

        for (nq_u32 c = 0; c < 2u; c++) {
            nq_s32 s = (mix[i * 2u + c] * (nq_s32)gain) / 256;

            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;

            g_capture[at * 2u + c] = (nq_s16)s;
        }

        g_capture_count++;
    }
}

/*
 * Without a sound card there is no hardware asking for samples, so the
 * timer decides how many are due: 48 for every millisecond that passed.
 */
static void mixer_run_clocked(void) {
    nq_u64 now = timer64_get_ticks();
    nq_u64 hz = timer64_get_frequency();
    nq_u64 due = ((now - g_clock_tick) * RATE) / (hz ? hz : 1000u);

    if (!g_capture_on || due > RATE / 5u) {
        /* nobody listening, or a long gap: start counting from now */
        g_clock_tick = now;
        return;
    }

    if (due < 48u) {
        return;
    }

    g_clock_tick += (due * (hz ? hz : 1000u)) / RATE;

    nq_u32 gain = g_volume_gain[g_volume > 9u ? 9u : g_volume];

    while (due) {
        nq_u32 frames = due > CHUNK_FRAMES ? CHUNK_FRAMES : (nq_u32)due;

        mix_chunk(frames);
        capture_push(g_mix, frames, gain);
        due -= frames;
    }
}

/* Mixes ahead of the hardware's playing position. */
static void mixer_run(void) {
    if (!g_card) {
        mixer_run_clocked();
        return;
    }

    nq_u32 size = hda64_ring_bytes();

    if (!size) {
        return;
    }

    nq_u32 position = hda64_position();
    nq_u32 ahead = (g_write + size - position) % size;

    /*
     * A lead of more than half the ring means the player overtook the
     * writer (this task was kept waiting too long). Start again just in
     * front of the player.
     */
    if (ahead > size / 2u) {
        g_write = (position + RESTART_BYTES) % size;
        g_write &= ~3u;
        ahead = RESTART_BYTES;
        g_underruns++;
    }

    nq_u32 gain = g_volume_gain[g_volume > 9u ? 9u : g_volume];

    while (ahead < LEAD_BYTES) {
        nq_u32 frames = (LEAD_BYTES - ahead) / 4u;

        if (frames > CHUNK_FRAMES) {
            frames = CHUNK_FRAMES;
        }

        if (!frames) {
            break;
        }

        mix_chunk(frames);

        if (g_capture_on) {
            capture_push(g_mix, frames, gain);
        }

        for (nq_u32 i = 0; i < frames * 2u; i++) {
            nq_s32 s = (g_mix[i] * (nq_s32)gain) / 256;

            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;

            nq_u8* page = hda64_ring_page(g_write >> 12);

            *(nq_s16*)(page + (g_write & 0xFFFu)) = (nq_s16)s;

            g_write += 2u;

            if (g_write >= size) {
                g_write = 0;
            }
        }

        ahead += frames * 4u;
    }
}

static void sound_task(void* arg) {
    (void)arg;

    for (;;) {
        process64_preempt_disable();
        mixer_run();
        process64_preempt_enable();

        process64_sleep_ticks(4);
    }
}

/* -------------------------------------------------------------------------- */
/* Making sounds                                                               */
/* -------------------------------------------------------------------------- */

static int voice_add(nq_u32 frequency, nq_u32 delay_ms, nq_u32 length_ms,
                     nq_u32 volume, nq_u32 wave) {
    if (!g_present || frequency < 20u || frequency > 20000u || length_ms == 0) {
        return 0;
    }

    if (length_ms > 10000u) {
        length_ms = 10000u;
    }

    if (volume > 100u) {
        volume = 100u;
    }

    int added = 0;

    process64_preempt_disable();

    for (nq_u32 n = 0; n < MAX_VOICES; n++) {
        voice_t* v = &g_voices[n];

        if (v->active) {
            continue;
        }

        v->wave = (nq_u8)(wave > SOUND64_WAVE_TRIANGLE ? SOUND64_WAVE_SINE : wave);
        v->level = (nq_u16)((volume * 256u) / 100u);
        v->delay = (delay_ms * RATE) / 1000u;
        v->phase = 0;
        v->step = (nq_u32)(((nq_u64)frequency << 32) / RATE);
        v->total = (length_ms * RATE) / 1000u;
        v->done = 0;
        v->active = 1;

        added = 1;
        break;
    }

    process64_preempt_enable();

    return added;
}

int sound64_tone(nq_u32 frequency_hz, nq_u32 length_ms, nq_u32 volume, nq_u32 wave) {
    return voice_add(frequency_hz, 0, length_ms, volume, wave);
}

void sound64_play(nq_u32 sound) {
    if (!g_present || !g_effects || sound >= SOUND64_COUNT) {
        return;
    }

    for (nq_u32 i = 0; i < g_sounds[sound].count; i++) {
        const note_t* n = &g_sounds[sound].notes[i];

        voice_add(n->frequency, n->start_ms, n->length_ms, n->volume, n->wave);
    }
}

nq_u32 sound64_write(const nq_s16* samples, nq_u32 frames) {
    if (!g_present || !samples) {
        return 0;
    }

    process64_preempt_disable();

    nq_u32 room = QUEUE_FRAMES - g_queue_count;
    nq_u32 take = frames < room ? frames : room;
    nq_u32 at = (g_queue_read + g_queue_count) % QUEUE_FRAMES;

    for (nq_u32 i = 0; i < take; i++) {
        g_queue[at * 2u] = samples[i * 2u];
        g_queue[at * 2u + 1u] = samples[i * 2u + 1u];

        at = (at + 1u) % QUEUE_FRAMES;
    }

    g_queue_count += take;

    process64_preempt_enable();

    return take;
}

nq_u32 sound64_queued(void) {
    return g_queue_count;
}

nq_u32 sound64_room(void) {
    return g_present ? QUEUE_FRAMES - g_queue_count : 0;
}

void sound64_capture(int on) {
    process64_preempt_disable();

    g_capture_on = on ? 1 : 0;
    g_capture_read = 0;
    g_capture_count = 0;
    g_clock_tick = timer64_get_ticks();
    g_present = g_card || g_capture_on;

    process64_preempt_enable();
}

nq_u32 sound64_capture_count(void) {
    return g_capture_count;
}

nq_u32 sound64_capture_read(nq_s16* out, nq_u32 frames) {
    process64_preempt_disable();

    nq_u32 take = frames < g_capture_count ? frames : g_capture_count;

    for (nq_u32 i = 0; i < take; i++) {
        out[i * 2u] = g_capture[g_capture_read * 2u];
        out[i * 2u + 1u] = g_capture[g_capture_read * 2u + 1u];
        g_capture_read = (g_capture_read + 1u) % CAPTURE_FRAMES;
    }

    g_capture_count -= take;

    process64_preempt_enable();

    return take;
}

nq_u32 sound64_volume(void) {
    return g_volume;
}

void sound64_set_volume(nq_u32 level) {
    g_volume = level > 9u ? 9u : level;
}

int sound64_effects_on(void) {
    return g_effects;
}

void sound64_set_effects(int on) {
    g_effects = on ? 1 : 0;
}

int sound64_present(void) {
    return g_present;
}

void sound64_init(void) {
    if (g_started) {
        return;
    }

    g_started = 1;
    sine_build();

    if (hda64_init()) {
        /* Start a little ahead of wherever the stream already is. */
        g_write = (hda64_position() + RESTART_BYTES) % hda64_ring_bytes();
        g_write &= ~3u;

        g_card = 1;
        g_present = 1;

        k64_print("SOUND: ");
        k64_print(hda64_name());
        k64_print(", ");
        k64_print_dec64(hda64_output_count());
        k64_print(hda64_output_count() == 1u ? " output\n" : " outputs\n");
    }

    /*
     * The mixer runs even with no sound card: a Bluetooth headset may
     * be connected later.
     */
    g_clock_tick = timer64_get_ticks();

    if (process64_create_kernel_task_ex("sound", sound_task, 0, 0) <= 0) {
        k64_print("SOUND: could not start the mixer task\n");
        g_card = 0;
        g_present = 0;
    }
}

/* -------------------------------------------------------------------------- */
/* Shell                                                                       */
/* -------------------------------------------------------------------------- */

static const char* skip_spaces(const char* p) {
    while (*p == ' ') {
        p++;
    }

    return p;
}

static const char* read_number(const char* p, nq_u32* out, int* found) {
    nq_u32 value = 0;

    p = skip_spaces(p);
    *found = 0;

    while (*p >= '0' && *p <= '9') {
        value = value * 10u + (nq_u32)(*p - '0');
        *found = 1;
        p++;
    }

    *out = value;

    return p;
}

static int word_is(const char* p, const char* word) {
    while (*word) {
        if (*p++ != *word++) {
            return 0;
        }
    }

    return *p == 0 || *p == ' ';
}

static void print_hex16(nq_u32 value) {
    char text[5];

    for (int i = 0; i < 4; i++) {
        text[i] = "0123456789abcdef"[(value >> (12 - 4 * i)) & 0xFu];
    }

    text[4] = 0;
    k64_print(text);
}

void sound64_shell(const char* args) {
    args = skip_spaces(args);

    if (!g_card && g_capture_on) {
        k64_print("Output: Bluetooth headset (no sound card).\nVolume: ");
        k64_print_dec64(g_volume);
        k64_print(" of 9\n");

        if (word_is(skip_spaces(args), "test")) {
            int effects = g_effects;

            g_effects = 1;
            sound64_play(SOUND64_STARTUP);
            g_effects = effects;
        }

        return;
    }

    if (!g_present) {
        k64_print("No sound: ");
        k64_print(hda64_why());
        k64_print(".\nAn HD Audio controller with an analogue output is needed;\n"
                  "HDMI and USB sound are not supported.\n");
        return;
    }

    if (word_is(args, "vol") || word_is(args, "volume")) {
        nq_u32 level = 0;
        int found = 0;

        read_number(args + (word_is(args, "vol") ? 3 : 6), &level, &found);

        if (found) {
            sound64_set_volume(level);
        }

        k64_print("Volume: ");
        k64_print_dec64(g_volume);
        k64_print(" of 9\n");
        return;
    }

    if (word_is(args, "test")) {
        k64_print("Playing the start-up sound.\n");

        int effects = g_effects;

        g_effects = 1;
        sound64_play(SOUND64_STARTUP);
        g_effects = effects;
        return;
    }

    if (*args >= '0' && *args <= '9') {
        nq_u32 which = 0;
        int found = 0;
        int effects = g_effects;

        read_number(args, &which, &found);

        g_effects = 1;
        sound64_play(which);
        g_effects = effects;
        return;
    }

    k64_print("Device: ");
    k64_print(hda64_name());
    k64_print("\nCodec:  ");
    print_hex16(hda64_codec_id() >> 16);
    k64_print(":");
    print_hex16(hda64_codec_id() & 0xFFFFu);
    k64_print(", ");
    k64_print_dec64(hda64_output_count());
    k64_print(" analogue output(s) in use\nFormat: 48000 Hz, 16-bit stereo\nVolume: ");
    k64_print_dec64(g_volume);
    k64_print(" of 9, system sounds ");
    k64_print(g_effects ? "on" : "off");
    k64_print("\nLate refills: ");
    k64_print_dec64(g_underruns);
    k64_print("\nCommands: sound test | sound 0..5 | sound vol 0..9 | beep [hz] [ms]\n");
}

void sound64_shell_beep(const char* args) {
    nq_u32 frequency = 880;
    nq_u32 length = 200;
    nq_u32 value = 0;
    int found = 0;

    if (!g_present) {
        k64_print("No sound hardware.\n");
        return;
    }

    args = read_number(args, &value, &found);

    if (found) {
        frequency = value;
        read_number(args, &value, &found);

        if (found) {
            length = value;
        }
    }

    if (!sound64_tone(frequency, length, 60, SOUND64_WAVE_SINE)) {
        k64_print("Usage: beep [20..20000 hz] [ms]\n");
    }
}
