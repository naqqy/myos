#ifndef BT_INTERNAL64_H
#define BT_INTERNAL64_H

#include "naq_boot_info.h"

/*
 * Shared between the two halves of the Bluetooth stack:
 *
 *   bt64.c       USB transport, the controller (HCI), finding, connecting
 *                and pairing, the task and the shell command
 *   bta2dp64.c   what runs over a connection: L2CAP channels, the audio
 *                signalling (AVDTP) and the SBC stream itself
 */

/* The trace log shown by "bt log". */
void bt_trace(const char* text);
void bt_trace_hex(nq_u32 value, nq_u32 digits);
void bt_trace_dec(nq_u64 value);

nq_u64 bt_now_ms(void);

/*
 * Sends one L2CAP frame (its 4-byte header included) to the connected
 * device, split into controller-sized pieces. Returns 0 if the
 * controller has no room for it right now.
 */
int bt_acl_send(const nq_u8* frame, nq_u32 length);

/* How many more packets the controller will take at the moment. */
nq_u32 bt_acl_room(void);

/* ---- bta2dp64.c ---- */

/* The link is up and encrypted. we_called: this side made the connection. */
void a2dp_link_up(int we_called);
void a2dp_link_down(void);

/* A complete L2CAP frame from the other device. */
void a2dp_input(const nq_u8* frame, nq_u32 length);

/* Called every millisecond or two by the Bluetooth task. */
void a2dp_tick(void);

int a2dp_streaming(void);
const char* a2dp_state(void);
nq_u64 a2dp_packets_sent(void);
nq_u64 a2dp_packets_skipped(void);
nq_u32 a2dp_bitpool(void);

#endif
