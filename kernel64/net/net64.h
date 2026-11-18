#ifndef NET64_H
#define NET64_H

#include "naq_boot_info.h"

/*
 * Network stack: Ethernet, ARP, IPv4, ICMP (ping), UDP, DHCP and DNS,
 * on top of the card driver in nic64.c.
 *
 * A kernel task ("net") receives frames, answers what needs answering
 * and keeps the address lease alive. IPv4 addresses are passed around as
 * 32-bit numbers in host order: 192.168.1.20 is 0xC0A80114.
 *
 * TCP is client-only: it opens connections but does not accept them.
 */

/*
 * Starts the card and the network task. Safe to call on a machine with no
 * supported card. Prints one line saying what was found.
 */
void net64_init(void);

/*
 * 1 once an address has been obtained (by DHCP).
 */
int net64_ready(void);

/*
 * Turns a host name or a dotted address ("192.168.1.1") into an IPv4
 * address. Waits up to a few seconds for the DNS server. Returns 1 on
 * success.
 */
int net64_resolve(const char* name, nq_u32* out_ip);

/*
 * Sends one ICMP echo request and waits for the reply.
 * Returns 1 and the round-trip time on success.
 */
int net64_ping(nq_u32 ip, nq_u32 timeout_ms, nq_u32* out_ms);

/*
 * TCP connections (outgoing only). None of these calls wait: a program
 * opens a connection, then polls its state and moves data when it can.
 *
 * owner_pid ties a connection to the program that opened it (0 for the
 * kernel itself); the other calls refuse a handle that is not the
 * caller's, and net64_tcp_close_pid() closes everything a program left
 * open when it exits.
 */
#define NET64_TCP_CONNECTING 0
#define NET64_TCP_OPEN       1
#define NET64_TCP_CLOSED     2   /* the other side has finished sending */
#define NET64_TCP_ERROR      (-1)

#define NET64_RECV_END   (-1)    /* everything has been read; closed */
#define NET64_RECV_ERROR (-2)

/*
 * Starts connecting. Returns a handle (1 or more), or -1.
 */
int net64_tcp_open(nq_u32 ip, nq_u16 port, int owner_pid);
int net64_tcp_state(int handle, int owner_pid);

/*
 * Queues bytes to send. Returns how many were accepted (possibly fewer
 * than asked, or 0 when the buffer is full), or -1 on error.
 */
int net64_tcp_send(int handle, int owner_pid, const void* data, nq_u32 length);

/*
 * Takes received bytes. Returns the count, 0 if nothing has arrived yet,
 * or NET64_RECV_END / NET64_RECV_ERROR.
 */
int net64_tcp_recv(int handle, int owner_pid, void* buffer, nq_u32 capacity);

void net64_tcp_close(int handle, int owner_pid);
void net64_tcp_close_pid(int owner_pid);

/*
 * Shell commands: "net", "ping <host>", "dns <name>", "http <host> [path]".
 */
void net64_shell_http(const char* arg);

void net64_print_status(void);
void net64_shell_ping(const char* host);
void net64_shell_dns(const char* name);

#endif
