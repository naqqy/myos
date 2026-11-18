#ifndef NIC64_H
#define NIC64_H

#include "naq_boot_info.h"

/*
 * Network card driver for Intel gigabit / 2.5-gigabit Ethernet controllers
 * of the "igb" and "igc" families:
 *
 *   I225 / I226   (2.5 GbE on current desktop boards)
 *   I210 / I211, I350, 82576   (1 GbE; QEMU emulates the 82576 as "igb")
 *
 * These share one register layout for their packet rings, so one driver
 * covers them. Older Intel cards (e1000, e1000e) use a different layout
 * and are not handled.
 *
 * Polled: nothing here runs from an interrupt. Callers must hold the
 * task-switch guard (process64_preempt_disable) around send and receive.
 */

#define NIC64_MAX_FRAME 1518u

/*
 * Finds and starts the first supported card. Returns 1 if one is running.
 */
int nic64_init(void);

int nic64_present(void);
const char* nic64_name(void);

/*
 * The card's 6-byte hardware (MAC) address.
 */
const nq_u8* nic64_mac(void);

/*
 * 1 if the cable is connected and the link is up; *out_mbps gets the
 * speed.
 */
int nic64_link(nq_u32* out_mbps);

/*
 * Nudges the card's transceiver to renegotiate the link. For a card that
 * was left without a link after the reset.
 */
void nic64_restart_link(void);

/*
 * For a link that came up slower than gigabit: makes sure the card is
 * offering gigabit and negotiates again. (Windows leaves the card at a
 * low-power speed when it shuts down, for wake-on-LAN, and the setting
 * survives a restart.) The link drops for a few seconds while it does.
 * Returns 1 if a renegotiation was started.
 */
int nic64_upgrade_link(void);

/*
 * Sends one Ethernet frame (destination, source, type, payload; the card
 * adds the checksum). Returns 0 if the transmit ring is full.
 */
int nic64_send(const void* frame, nq_u32 length);

/*
 * Takes one received frame. Returns 0 if none is waiting.
 */
int nic64_receive(void* buffer, nq_u32 capacity, nq_u32* out_length);

nq_u64 nic64_rx_count(void);
nq_u64 nic64_tx_count(void);

#endif
