#ifndef SOUND64_H
#define SOUND64_H

#include "naq_boot_info.h"

/*
 * Sound: a mixer on top of the HD Audio driver (hda64.h).
 *
 * Everything that makes a noise goes through here and is added together
 * at 48 kHz, 16-bit stereo:
 *
 *   tones     a note of a given pitch and length, with a soft start and a
 *             fading end. The system sounds are short tunes of these.
 *   samples   recorded or computed sound from an app, queued in a buffer.
 *
 * A kernel task ("sound") keeps the hardware supplied a fraction of a
 * second ahead of what is being played.
 */

/* System sounds for sound64_play(). */
#define SOUND64_STARTUP   0
#define SOUND64_CLICK     1
#define SOUND64_ERROR     2
#define SOUND64_NOTIFY    3
#define SOUND64_OPEN      4
#define SOUND64_CLOSE     5
#define SOUND64_COUNT     6

/* Wave shapes for sound64_tone(). */
#define SOUND64_WAVE_SINE     0     /* pure */
#define SOUND64_WAVE_SOFT     1     /* sine with a little overtone: bell-like */
#define SOUND64_WAVE_SQUARE   2     /* old computer beep */
#define SOUND64_WAVE_TRIANGLE 3

/* Finds the sound hardware and starts the mixer. */
void sound64_init(void);

int sound64_present(void);

/*
 * Plays a note. volume is 0..100. Returns 0 if there is no sound
 * hardware or too many notes are already playing.
 */
int sound64_tone(nq_u32 frequency_hz, nq_u32 length_ms, nq_u32 volume, nq_u32 wave);

/*
 * Plays a system sound. These are skipped when "system sounds" is off
 * in Settings.
 */
void sound64_play(nq_u32 sound);

/*
 * Queues samples: `frames` pairs of 16-bit left/right values at 48 kHz.
 * Returns how many were taken (the queue holds about 0.7 s); call again
 * with the rest.
 */
nq_u32 sound64_write(const nq_s16* samples, nq_u32 frames);

nq_u32 sound64_queued(void);    /* frames waiting to be played */
nq_u32 sound64_room(void);      /* frames that would fit right now */

/*
 * A second way out of the mixer, for a sink that is not the sound card
 * (Bluetooth). While capture is on, everything mixed is also put in a
 * queue that sound64_capture_read() empties: stereo, 16-bit, 48 kHz.
 * On a machine without a sound card the mixer then runs off the clock.
 */
void sound64_capture(int on);
nq_u32 sound64_capture_count(void);
nq_u32 sound64_capture_read(nq_s16* out, nq_u32 frames);

/* Master volume, 0..9 as kept in the settings file (9 is full). */
nq_u32 sound64_volume(void);
void sound64_set_volume(nq_u32 level);

int sound64_effects_on(void);
void sound64_set_effects(int on);

/* Shell: "sound", "sound test", "sound vol N", "beep [hz] [ms]". */
void sound64_shell(const char* args);
void sound64_shell_beep(const char* args);

#endif
