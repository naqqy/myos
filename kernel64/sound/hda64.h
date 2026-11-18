#ifndef HDA64_H
#define HDA64_H

#include "naq_boot_info.h"

/*
 * Intel High Definition Audio ("HDA") driver.
 *
 * HDA is the sound hardware of almost every PC since about 2005. It has
 * two parts:
 *
 *   the controller   a PCI device that moves sample data between memory
 *                    and a serial link. The same design from every maker.
 *   the codec        a chip on that link (often a Realtek ALCxxx) with
 *                    the converters and the sockets. Codecs describe
 *                    themselves as a graph of "widgets", so one driver
 *                    can work out how to reach the sockets on any of them.
 *
 * This driver plays one stream of 48 kHz, 16-bit stereo sound through
 * every analogue output it can find (line out, headphones, speakers).
 * There is no recording, and no sound over HDMI or DisplayPort: those
 * outputs belong to the graphics card and need its display driver.
 * Sound chips connected by USB (some high-end boards) are not HDA either.
 *
 * Polled: nothing runs from an interrupt.
 */

#define HDA64_RATE        48000u
#define HDA64_FRAME_BYTES 4u        /* two channels of 16 bits */

/*
 * Finds a controller whose codec has an analogue output and starts a
 * stream playing from a ring of memory. Returns 1 if sound is running.
 */
int hda64_init(void);

int hda64_present(void);

/* When hda64_init() failed: the reason, for the "sound" command. */
const char* hda64_why(void);

/* One line about the device, for the shell. */
const char* hda64_name(void);
nq_u32 hda64_codec_id(void);        /* vendor in the top 16 bits */
nq_u32 hda64_output_count(void);    /* sockets being driven */

/*
 * The ring the controller plays from, round and round. It is made of
 * separate 4 KiB pages; hda64_ring_page(n) is page n.
 */
nq_u32 hda64_ring_bytes(void);
nq_u8* hda64_ring_page(nq_u32 index);

/* Where in the ring (in bytes) the controller is playing right now. */
nq_u32 hda64_position(void);

#endif
