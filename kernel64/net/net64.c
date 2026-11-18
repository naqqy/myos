#include "net64.h"
#include "nic64.h"

#include "../k64_console.h"
#include "../arch/x86_64/timer64.h"
#include "../proc/process64.h"
#include "../mm/kmem64.h"

/*
 * Network stack. See net64.h.
 *
 * Layers, outermost first, for a frame arriving from the wire:
 *
 *   Ethernet   [dst mac][src mac][type]            type says what follows
 *   ARP        "who has IP x?" / "x is at mac y"   IP address -> MAC address
 *   IPv4       [header: src ip, dst ip, protocol]
 *   ICMP       echo request / reply                ping
 *   UDP        [src port][dst port][length]        carries DHCP and DNS
 *
 * Multi-byte fields on the wire are big-endian ("network order"); get16,
 * put16 and friends convert.
 *
 * Locking: the card and the tables below are touched by the network task
 * and by whichever task runs a shell command. Both take the task-switch
 * guard around every such section, and nothing sleeps while holding it.
 */

#define ETH_TYPE_IPV4 0x0800u
#define ETH_TYPE_ARP  0x0806u

#define IP_PROTO_ICMP 1u
#define IP_PROTO_TCP  6u
#define IP_PROTO_UDP  17u

#define PORT_DNS         53u
#define PORT_DHCP_SERVER 67u
#define PORT_DHCP_CLIENT 68u

#define ETH_HEADER 14u
#define IP_HEADER  20u
#define UDP_HEADER 8u

#define FRAME_MAX 1514u

#define ARP_ENTRIES 16u

/*
 * DHCP client states.
 */
#define DHCP_IDLE       0   /* no link */
#define DHCP_DISCOVER   1   /* asking who can give us an address */
#define DHCP_REQUEST    2   /* asking for the address that was offered */
#define DHCP_BOUND      3   /* have an address */

#define DHCP_MSG_DISCOVER 1u
#define DHCP_MSG_OFFER    2u
#define DHCP_MSG_REQUEST  3u
#define DHCP_MSG_ACK      5u
#define DHCP_MSG_NAK      6u

static const nq_u8 g_broadcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/*
 * Our configuration.
 */
static volatile int g_have_ip = 0;
static nq_u32 g_ip = 0;
static nq_u32 g_mask = 0;
static nq_u32 g_gateway = 0;
static nq_u32 g_dns = 0;

typedef struct arp_entry {
    int valid;
    nq_u32 ip;
    nq_u8 mac[6];
} arp_entry_t;

static arp_entry_t g_arp[ARP_ENTRIES];
static nq_u32 g_arp_next = 0;

/*
 * DHCP.
 */
static int g_dhcp_state = DHCP_IDLE;
static nq_u32 g_dhcp_xid = 0;
static nq_u32 g_dhcp_offer_ip = 0;
static nq_u32 g_dhcp_server = 0;
static nq_u64 g_dhcp_next_send = 0;
static nq_u32 g_dhcp_tries = 0;
static nq_u64 g_dhcp_renew_at = 0;

/*
 * The ping and the DNS query in flight (one of each at a time).
 */
static volatile int g_ping_waiting = 0;
static volatile int g_ping_done = 0;
static nq_u16 g_ping_id = 0;
static nq_u16 g_ping_seq = 0;

static volatile int g_dns_waiting = 0;
static volatile int g_dns_done = 0;
static nq_u16 g_dns_id = 0;
static nq_u16 g_dns_port = 0;
static nq_u32 g_dns_answer = 0;

static nq_u16 g_ip_id = 1;
static nq_u32 g_random = 0x13579BDFu;

static int g_link_was_up = 0;
static nq_u64 g_link_kick_at = 0;
static int g_link_kicked = 0;
static int g_link_upgrade_tried = 0;

static void tcp_input(nq_u32 src_ip, const nq_u8* segment, nq_u32 length);
static void tcp_tick(void);

static nq_u8 g_rx_frame[FRAME_MAX + 4u];
static nq_u8 g_tx_frame[FRAME_MAX + 4u];

/* -------------------------------------------------------------------------- */
/* Helpers                                                                     */
/* -------------------------------------------------------------------------- */

static nq_u16 get16(const nq_u8* p) {
    return (nq_u16)(((nq_u16)p[0] << 8) | p[1]);
}

static nq_u32 get32(const nq_u8* p) {
    return ((nq_u32)p[0] << 24) | ((nq_u32)p[1] << 16) | ((nq_u32)p[2] << 8) | p[3];
}

static void put16(nq_u8* p, nq_u16 v) {
    p[0] = (nq_u8)(v >> 8);
    p[1] = (nq_u8)v;
}

static void put32(nq_u8* p, nq_u32 v) {
    p[0] = (nq_u8)(v >> 24);
    p[1] = (nq_u8)(v >> 16);
    p[2] = (nq_u8)(v >> 8);
    p[3] = (nq_u8)v;
}

static void copy_bytes(nq_u8* dst, const nq_u8* src, nq_u32 count) {
    for (nq_u32 i = 0; i < count; i++) {
        dst[i] = src[i];
    }
}

static void zero_bytes(nq_u8* dst, nq_u32 count) {
    for (nq_u32 i = 0; i < count; i++) {
        dst[i] = 0;
    }
}

static nq_u64 now_ms(void) {
    nq_u64 hz = timer64_get_frequency();

    return (timer64_get_ticks() * 1000u) / (hz ? hz : 1000u);
}

static void sleep_ms(nq_u32 ms) {
    nq_u64 hz = timer64_get_frequency();
    nq_u64 ticks = ((nq_u64)ms * (hz ? hz : 1000u) + 999u) / 1000u;

    process64_sleep_ticks(ticks ? ticks : 1u);
}

static nq_u32 next_random(void) {
    g_random = g_random * 1664525u + 1013904223u + (nq_u32)timer64_get_ticks();

    return g_random;
}

/*
 * The Internet checksum: one's-complement sum of 16-bit words.
 */
static nq_u16 checksum(const nq_u8* data, nq_u32 length) {
    nq_u32 sum = 0;

    for (nq_u32 i = 0; i + 1u < length; i += 2u) {
        sum += get16(data + i);
    }

    if (length & 1u) {
        sum += (nq_u32)data[length - 1u] << 8;
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }

    return (nq_u16)~sum;
}

static void print_ip(nq_u32 ip) {
    k64_print_dec64((ip >> 24) & 0xFFu);
    k64_print(".");
    k64_print_dec64((ip >> 16) & 0xFFu);
    k64_print(".");
    k64_print_dec64((ip >> 8) & 0xFFu);
    k64_print(".");
    k64_print_dec64(ip & 0xFFu);
}

/*
 * "192.168.1.1" -> number. Returns 0 if the text is not an address.
 */
static int parse_ip(const char* text, nq_u32* out) {
    nq_u32 ip = 0;
    int parts = 0;

    for (;;) {
        nq_u32 value = 0;
        int digits = 0;

        while (*text >= '0' && *text <= '9') {
            value = value * 10u + (nq_u32)(*text - '0');
            digits++;
            text++;

            if (value > 255u) {
                return 0;
            }
        }

        if (!digits) {
            return 0;
        }

        ip = (ip << 8) | value;
        parts++;

        if (*text == '.' && parts < 4) {
            text++;
            continue;
        }

        break;
    }

    if (parts != 4 || *text != 0) {
        return 0;
    }

    *out = ip;

    return 1;
}

/* -------------------------------------------------------------------------- */
/* Ethernet and ARP                                                            */
/* -------------------------------------------------------------------------- */

/*
 * Sends g_tx_frame, whose payload (after the Ethernet header) is already
 * in place. Call with the guard held.
 */
static int eth_send(const nq_u8* dst_mac, nq_u16 type, nq_u32 payload_length) {
    copy_bytes(g_tx_frame, dst_mac, 6);
    copy_bytes(g_tx_frame + 6, nic64_mac(), 6);
    put16(g_tx_frame + 12, type);

    return nic64_send(g_tx_frame, ETH_HEADER + payload_length);
}

static void arp_store(nq_u32 ip, const nq_u8* mac) {
    if (ip == 0) {
        return;
    }

    for (nq_u32 i = 0; i < ARP_ENTRIES; i++) {
        if (g_arp[i].valid && g_arp[i].ip == ip) {
            copy_bytes(g_arp[i].mac, mac, 6);
            return;
        }
    }

    arp_entry_t* slot = &g_arp[g_arp_next];

    g_arp_next = (g_arp_next + 1u) % ARP_ENTRIES;

    slot->valid = 1;
    slot->ip = ip;
    copy_bytes(slot->mac, mac, 6);
}

static int arp_lookup(nq_u32 ip, nq_u8* out_mac) {
    for (nq_u32 i = 0; i < ARP_ENTRIES; i++) {
        if (g_arp[i].valid && g_arp[i].ip == ip) {
            copy_bytes(out_mac, g_arp[i].mac, 6);
            return 1;
        }
    }

    return 0;
}

/*
 * ARP packet for Ethernet + IPv4:
 *   hardware type 1, protocol 0x0800, sizes 6 and 4, operation,
 *   sender mac, sender ip, target mac, target ip.
 */
static void arp_send(nq_u16 operation, const nq_u8* target_mac, nq_u32 target_ip) {
    nq_u8* p = g_tx_frame + ETH_HEADER;

    put16(p + 0, 1);
    put16(p + 2, ETH_TYPE_IPV4);
    p[4] = 6;
    p[5] = 4;
    put16(p + 6, operation);
    copy_bytes(p + 8, nic64_mac(), 6);
    put32(p + 14, g_ip);

    if (operation == 1u) {
        zero_bytes(p + 18, 6);
    } else {
        copy_bytes(p + 18, target_mac, 6);
    }

    put32(p + 24, target_ip);

    eth_send(operation == 1u ? g_broadcast_mac : target_mac, ETH_TYPE_ARP, 28);
}

static void arp_input(const nq_u8* p, nq_u32 length) {
    if (length < 28u || get16(p) != 1u || get16(p + 2) != ETH_TYPE_IPV4) {
        return;
    }

    nq_u16 operation = get16(p + 6);
    const nq_u8* sender_mac = p + 8;
    nq_u32 sender_ip = get32(p + 14);
    nq_u32 target_ip = get32(p + 24);

    /*
     * Learn from anything addressed to us, and from replies.
     */
    if (g_have_ip && (target_ip == g_ip || operation == 2u)) {
        arp_store(sender_ip, sender_mac);
    }

    if (operation == 1u && g_have_ip && target_ip == g_ip) {
        arp_send(2, sender_mac, sender_ip);
    }
}

/*
 * The address a packet for `ip` goes to on the local network: the host
 * itself if it is on our subnet, otherwise the gateway.
 */
static nq_u32 next_hop(nq_u32 ip) {
    if ((ip & g_mask) == (g_ip & g_mask) || g_gateway == 0) {
        return ip;
    }

    return g_gateway;
}

/* -------------------------------------------------------------------------- */
/* IPv4 and UDP output                                                         */
/* -------------------------------------------------------------------------- */

/*
 * Sends an IPv4 packet whose payload is already at
 * g_tx_frame + ETH_HEADER + IP_HEADER. Call with the guard held.
 *
 * Returns 0 if the destination's MAC address is not known yet; an ARP
 * request has then been sent, and the caller should try again shortly.
 */
static int ip_send(nq_u32 src_ip, nq_u32 dst_ip, nq_u8 protocol, nq_u32 payload_length) {
    nq_u8 mac[6];
    nq_u8* ip = g_tx_frame + ETH_HEADER;

    if (dst_ip == 0xFFFFFFFFu) {
        copy_bytes(mac, g_broadcast_mac, 6);
    } else if (!arp_lookup(next_hop(dst_ip), mac)) {
        arp_send(1, 0, next_hop(dst_ip));
        return 0;
    }

    ip[0] = 0x45;                   /* version 4, 5-word header */
    ip[1] = 0;
    put16(ip + 2, (nq_u16)(IP_HEADER + payload_length));
    put16(ip + 4, g_ip_id++);
    put16(ip + 6, 0x4000);          /* do not fragment */
    ip[8] = 64;                     /* time to live */
    ip[9] = protocol;
    put16(ip + 10, 0);
    put32(ip + 12, src_ip);
    put32(ip + 16, dst_ip);
    put16(ip + 10, checksum(ip, IP_HEADER));

    return eth_send(mac, ETH_TYPE_IPV4, IP_HEADER + payload_length);
}

/*
 * UDP payload goes at udp_payload(); then udp_send().
 */
static nq_u8* udp_payload(void) {
    return g_tx_frame + ETH_HEADER + IP_HEADER + UDP_HEADER;
}

static int udp_send(
    nq_u32 src_ip,
    nq_u32 dst_ip,
    nq_u16 src_port,
    nq_u16 dst_port,
    nq_u32 length
) {
    nq_u8* udp = g_tx_frame + ETH_HEADER + IP_HEADER;

    put16(udp + 0, src_port);
    put16(udp + 2, dst_port);
    put16(udp + 4, (nq_u16)(UDP_HEADER + length));
    put16(udp + 6, 0);              /* checksum is optional over IPv4 */

    return ip_send(src_ip, dst_ip, IP_PROTO_UDP, UDP_HEADER + length);
}

/* -------------------------------------------------------------------------- */
/* DHCP                                                                        */
/* -------------------------------------------------------------------------- */

/*
 * A DHCP message is a fixed 236-byte header (most of it unused here), a
 * 4-byte "magic cookie", then options as [code][length][data...].
 */
static void dhcp_send(nq_u8 message_type) {
    nq_u8* p = udp_payload();
    nq_u32 n = 240;

    zero_bytes(p, 300);

    p[0] = 1;                       /* request */
    p[1] = 1;                       /* Ethernet */
    p[2] = 6;                       /* address length */
    put32(p + 4, g_dhcp_xid);
    put16(p + 10, 0x8000);          /* please answer by broadcast */
    copy_bytes(p + 28, nic64_mac(), 6);
    put32(p + 236, 0x63825363u);

    p[n++] = 53;                    /* message type */
    p[n++] = 1;
    p[n++] = message_type;

    if (message_type == DHCP_MSG_REQUEST) {
        p[n++] = 50;                /* the address we want */
        p[n++] = 4;
        put32(p + n, g_dhcp_offer_ip);
        n += 4;

        p[n++] = 54;                /* the server we are answering */
        p[n++] = 4;
        put32(p + n, g_dhcp_server);
        n += 4;
    }

    p[n++] = 55;                    /* what we want to be told */
    p[n++] = 3;
    p[n++] = 1;                     /* subnet mask */
    p[n++] = 3;                     /* router */
    p[n++] = 6;                     /* DNS server */

    p[n++] = 255;                   /* end */

    if (n < 300u) {
        n = 300;
    }

    udp_send(0, 0xFFFFFFFFu, PORT_DHCP_CLIENT, PORT_DHCP_SERVER, n);
}

static void dhcp_restart(void) {
    g_dhcp_state = DHCP_DISCOVER;
    g_dhcp_xid = next_random();
    g_dhcp_tries = 0;
    g_dhcp_next_send = 0;
}

static void dhcp_input(const nq_u8* p, nq_u32 length) {
    if (length < 240u || p[0] != 2 || get32(p + 4) != g_dhcp_xid ||
        get32(p + 236) != 0x63825363u) {
        return;
    }

    nq_u32 your_ip = get32(p + 16);
    nq_u32 message_type = 0;
    nq_u32 mask = 0;
    nq_u32 router = 0;
    nq_u32 dns = 0;
    nq_u32 server = 0;
    nq_u32 lease = 0;

    for (nq_u32 i = 240; i < length; ) {
        nq_u8 code = p[i];

        if (code == 255) {
            break;
        }

        if (code == 0) {
            i++;
            continue;
        }

        if (i + 1u >= length) {
            break;
        }

        nq_u32 size = p[i + 1u];
        const nq_u8* data = p + i + 2u;

        if (i + 2u + size > length) {
            break;
        }

        if (code == 53 && size >= 1u) {
            message_type = data[0];
        } else if (code == 1 && size >= 4u) {
            mask = get32(data);
        } else if (code == 3 && size >= 4u) {
            router = get32(data);
        } else if (code == 6 && size >= 4u) {
            dns = get32(data);
        } else if (code == 54 && size >= 4u) {
            server = get32(data);
        } else if (code == 51 && size >= 4u) {
            lease = get32(data);
        }

        i += 2u + size;
    }

    if (message_type == DHCP_MSG_OFFER && g_dhcp_state == DHCP_DISCOVER) {
        g_dhcp_offer_ip = your_ip;
        g_dhcp_server = server;
        g_dhcp_state = DHCP_REQUEST;
        g_dhcp_tries = 0;
        g_dhcp_next_send = 0;
    } else if (message_type == DHCP_MSG_ACK && g_dhcp_state == DHCP_REQUEST) {
        g_ip = your_ip;
        g_mask = mask ? mask : 0xFFFFFF00u;
        g_gateway = router;
        g_dns = dns ? dns : router;
        g_have_ip = 1;
        g_dhcp_state = DHCP_BOUND;

        /*
         * Ask again half-way through the lease.
         */
        if (lease == 0 || lease > 7u * 24u * 3600u) {
            lease = 24u * 3600u;
        }

        g_dhcp_renew_at = now_ms() + (nq_u64)lease * 500u;
    } else if (message_type == DHCP_MSG_NAK) {
        dhcp_restart();
    }
}

/*
 * Called a few times a second by the network task, guard held.
 */
static void dhcp_tick(void) {
    nq_u64 now = now_ms();

    if (g_dhcp_state == DHCP_BOUND) {
        if (now >= g_dhcp_renew_at) {
            /*
             * Keep using the address while a fresh one is negotiated.
             */
            dhcp_restart();
        }

        return;
    }

    if (g_dhcp_state == DHCP_IDLE || now < g_dhcp_next_send) {
        return;
    }

    /*
     * No answer to several requests: start over with a new discover.
     */
    if (g_dhcp_state == DHCP_REQUEST && g_dhcp_tries >= 4u) {
        dhcp_restart();
    }

    dhcp_send(g_dhcp_state == DHCP_DISCOVER ? DHCP_MSG_DISCOVER : DHCP_MSG_REQUEST);

    g_dhcp_tries++;

    /*
     * 1, 2, 4, then every 8 seconds.
     */
    nq_u32 shift = g_dhcp_tries < 4u ? g_dhcp_tries - 1u : 3u;

    g_dhcp_next_send = now + (1000u << shift);
}

/* -------------------------------------------------------------------------- */
/* DNS                                                                         */
/* -------------------------------------------------------------------------- */

/*
 * Skips a name in a DNS message. Names are length-prefixed labels ending
 * in a zero, or a 2-byte pointer to an earlier name.
 */
static nq_u32 dns_skip_name(const nq_u8* p, nq_u32 length, nq_u32 at) {
    while (at < length) {
        nq_u8 size = p[at];

        if (size == 0) {
            return at + 1u;
        }

        if ((size & 0xC0u) == 0xC0u) {
            return at + 2u;
        }

        at += 1u + size;
    }

    return length;
}

static void dns_input(const nq_u8* p, nq_u32 length) {
    if (!g_dns_waiting || length < 12u || get16(p) != g_dns_id) {
        return;
    }

    nq_u16 flags = get16(p + 2);
    nq_u32 questions = get16(p + 4);
    nq_u32 answers = get16(p + 6);
    nq_u32 at = 12;

    g_dns_answer = 0;

    /*
     * Bit 15: this is a response. Low 4 bits: error code.
     */
    if ((flags & 0x8000u) && (flags & 0x000Fu) == 0) {
        for (nq_u32 i = 0; i < questions && at < length; i++) {
            at = dns_skip_name(p, length, at) + 4u;
        }

        for (nq_u32 i = 0; i < answers && at + 10u <= length; i++) {
            at = dns_skip_name(p, length, at);

            if (at + 10u > length) {
                break;
            }

            nq_u16 type = get16(p + at);
            nq_u16 klass = get16(p + at + 2u);
            nq_u32 size = get16(p + at + 8u);

            at += 10u;

            if (at + size > length) {
                break;
            }

            /*
             * Type 1 (A), class 1 (Internet), 4 bytes: an IPv4 address.
             */
            if (type == 1u && klass == 1u && size == 4u) {
                g_dns_answer = get32(p + at);
                break;
            }

            at += size;
        }
    }

    g_dns_waiting = 0;
    g_dns_done = 1;
}

/*
 * Builds and sends a query for the A record of `name`. Guard held.
 * Returns 0 if the name is malformed or the server's MAC is not known yet.
 */
static int dns_send_query(const char* name) {
    nq_u8* p = udp_payload();
    nq_u32 n = 12;

    zero_bytes(p, 12);

    put16(p + 0, g_dns_id);
    put16(p + 2, 0x0100);           /* recursion desired */
    put16(p + 4, 1);                /* one question */

    /*
     * "www.example.com" -> [3]www[7]example[3]com[0]
     */
    for (const char* label = name; *label; ) {
        nq_u32 size = 0;

        while (label[size] && label[size] != '.') {
            size++;
        }

        if (size == 0 || size > 63u || n + size + 6u > 300u) {
            return 0;
        }

        p[n++] = (nq_u8)size;

        for (nq_u32 i = 0; i < size; i++) {
            p[n++] = (nq_u8)label[i];
        }

        label += size;

        if (*label == '.') {
            label++;
        }
    }

    p[n++] = 0;
    put16(p + n, 1);                /* type A */
    put16(p + n + 2u, 1);           /* class Internet */
    n += 4u;

    return udp_send(g_ip, g_dns, g_dns_port, PORT_DNS, n);
}

/* -------------------------------------------------------------------------- */
/* Input                                                                       */
/* -------------------------------------------------------------------------- */

static void icmp_input(nq_u32 src_ip, const nq_u8* src_mac, const nq_u8* p, nq_u32 length) {
    if (length < 8u || checksum(p, length) != 0) {
        return;
    }

    if (p[0] == 8 && g_have_ip) {
        /*
         * Echo request: send the same data back as an echo reply. The
         * sender's MAC is right there in the frame, so learn it.
         */
        if (length > 1400u) {
            return;
        }

        arp_store(next_hop(src_ip), src_mac);

        nq_u8* out = g_tx_frame + ETH_HEADER + IP_HEADER;

        copy_bytes(out, p, length);
        out[0] = 0;
        put16(out + 2, 0);
        put16(out + 2, checksum(out, length));

        ip_send(g_ip, src_ip, IP_PROTO_ICMP, length);
    } else if (p[0] == 0 && g_ping_waiting &&
               get16(p + 4) == g_ping_id && get16(p + 6) == g_ping_seq) {
        g_ping_waiting = 0;
        g_ping_done = 1;
    }
}

static void ip_input(const nq_u8* src_mac, const nq_u8* p, nq_u32 length) {
    if (length < IP_HEADER || (p[0] >> 4) != 4) {
        return;
    }

    nq_u32 header = (nq_u32)(p[0] & 0x0Fu) * 4u;
    nq_u32 total = get16(p + 2);

    if (header < IP_HEADER || total < header || total > length) {
        return;
    }

    /*
     * Fragments are not reassembled.
     */
    if (get16(p + 6) & 0x3FFFu) {
        return;
    }

    nq_u32 src_ip = get32(p + 12);
    nq_u32 dst_ip = get32(p + 16);
    const nq_u8* payload = p + header;
    nq_u32 payload_length = total - header;

    int for_us = g_have_ip && dst_ip == g_ip;
    int broadcast = dst_ip == 0xFFFFFFFFu ||
                    (g_have_ip && dst_ip == (g_ip | ~g_mask));

    if (p[9] == IP_PROTO_ICMP) {
        if (for_us) {
            icmp_input(src_ip, src_mac, payload, payload_length);
        }

        return;
    }

    if (p[9] == IP_PROTO_TCP) {
        if (for_us) {
            tcp_input(src_ip, payload, payload_length);
        }

        return;
    }

    if (p[9] != IP_PROTO_UDP || payload_length < UDP_HEADER) {
        return;
    }

    nq_u16 src_port = get16(payload);
    nq_u16 dst_port = get16(payload + 2);
    nq_u32 udp_length = get16(payload + 4);

    if (udp_length < UDP_HEADER || udp_length > payload_length) {
        return;
    }

    const nq_u8* data = payload + UDP_HEADER;
    nq_u32 data_length = udp_length - UDP_HEADER;

    /*
     * DHCP answers arrive before we have an address, so they are accepted
     * whatever they are addressed to.
     */
    if (dst_port == PORT_DHCP_CLIENT && src_port == PORT_DHCP_SERVER) {
        dhcp_input(data, data_length);
    } else if (for_us && !broadcast && src_port == PORT_DNS && dst_port == g_dns_port) {
        dns_input(data, data_length);
    }
}

static void frame_input(const nq_u8* frame, nq_u32 length) {
    if (length < ETH_HEADER) {
        return;
    }

    nq_u16 type = get16(frame + 12);

    if (type == ETH_TYPE_ARP) {
        arp_input(frame + ETH_HEADER, length - ETH_HEADER);
    } else if (type == ETH_TYPE_IPV4) {
        ip_input(frame + 6, frame + ETH_HEADER, length - ETH_HEADER);
    }
}

/* -------------------------------------------------------------------------- */
/* Task                                                                        */
/* -------------------------------------------------------------------------- */

static void link_tick(void) {
    nq_u32 speed = 0;
    int up = nic64_link(&speed);

    /*
     * Up, but slower than gigabit: try once to do better. If it works
     * the link drops and comes back, which is handled below like any
     * unplug and replug.
     */
    if (up && speed < 1000u && !g_link_upgrade_tried) {
        g_link_upgrade_tried = 1;

        if (nic64_upgrade_link()) {
            return;
        }
    }

    if (up && !g_link_was_up) {
        /*
         * Cable in (or link negotiated): get an address.
         */
        dhcp_restart();
    } else if (!up && g_link_was_up) {
        g_dhcp_state = DHCP_IDLE;
        g_have_ip = 0;

        for (nq_u32 i = 0; i < ARP_ENTRIES; i++) {
            g_arp[i].valid = 0;
        }
    }

    /*
     * Still no link a few seconds after start-up: ask the transceiver to
     * negotiate again, once.
     */
    if (!up && !g_link_kicked && now_ms() >= g_link_kick_at) {
        g_link_kicked = 1;
        nic64_restart_link();
    }

    g_link_was_up = up;
}

static void net_task(void* arg) {
    (void)arg;

    nq_u64 last_slow = 0;

    for (;;) {
        process64_preempt_disable();

        nq_u32 length = 0;

        for (nq_u32 budget = 0; budget < 64u; budget++) {
            if (!nic64_receive(g_rx_frame, FRAME_MAX, &length)) {
                break;
            }

            frame_input(g_rx_frame, length);
        }

        nq_u64 now = now_ms();

        if (now - last_slow >= 200u) {
            last_slow = now;

            link_tick();
            dhcp_tick();
        }

        tcp_tick();

        process64_preempt_enable();

        process64_sleep_ticks(1);
    }
}

void net64_init(void) {
    if (!nic64_init()) {
        return;
    }

    const nq_u8* mac = nic64_mac();

    g_random ^= ((nq_u32)mac[2] << 24) | ((nq_u32)mac[3] << 16) |
                ((nq_u32)mac[4] << 8) | mac[5];

    g_link_kick_at = now_ms() + 4000u;

    k64_print("NET: ");
    k64_print(nic64_name());
    k64_print(" network card found\n");

    if (process64_create_kernel_task_ex("net", net_task, 0, 0) <= 0) {
        k64_print("NET: could not start the network task\n");
    }
}

int net64_ready(void) {
    return g_have_ip;
}

/* -------------------------------------------------------------------------- */
/* Requests from other tasks                                                   */
/* -------------------------------------------------------------------------- */

int net64_resolve(const char* name, nq_u32* out_ip) {
    if (!name || !out_ip) {
        return 0;
    }

    if (parse_ip(name, out_ip)) {
        return 1;
    }

    if (!g_have_ip || !g_dns) {
        return 0;
    }

    process64_preempt_disable();

    g_dns_id = (nq_u16)next_random();
    g_dns_port = (nq_u16)(49152u + (next_random() % 16000u));
    g_dns_done = 0;
    g_dns_waiting = 1;

    process64_preempt_enable();

    /*
     * Ask up to four times over about four seconds. The first attempt
     * often only gets as far as finding the server's MAC address.
     */
    nq_u64 start = now_ms();
    nq_u64 next_send = 0;

    while (!g_dns_done && now_ms() - start < 4000u) {
        if (now_ms() >= next_send) {
            process64_preempt_disable();

            int sent = dns_send_query(name);

            process64_preempt_enable();

            next_send = now_ms() + (sent ? 1000u : 150u);
        }

        sleep_ms(5);
    }

    g_dns_waiting = 0;

    if (!g_dns_done || g_dns_answer == 0) {
        return 0;
    }

    *out_ip = g_dns_answer;

    return 1;
}

int net64_ping(nq_u32 ip, nq_u32 timeout_ms, nq_u32* out_ms) {
    if (!g_have_ip) {
        return 0;
    }

    process64_preempt_disable();

    g_ping_id = (nq_u16)next_random();
    g_ping_seq++;
    g_ping_done = 0;
    g_ping_waiting = 1;

    process64_preempt_enable();

    nq_u64 start = now_ms();
    nq_u64 sent_at = 0;
    int sent = 0;

    while (!g_ping_done && now_ms() - start < timeout_ms) {
        if (!sent) {
            process64_preempt_disable();

            /*
             * ICMP echo request: type 8, code 0, checksum, id, sequence,
             * then 32 bytes of filler.
             */
            nq_u8* p = g_tx_frame + ETH_HEADER + IP_HEADER;

            p[0] = 8;
            p[1] = 0;
            put16(p + 2, 0);
            put16(p + 4, g_ping_id);
            put16(p + 6, g_ping_seq);

            for (nq_u32 i = 0; i < 32u; i++) {
                p[8u + i] = (nq_u8)('a' + (i % 26u));
            }

            put16(p + 2, checksum(p, 40));

            sent = ip_send(g_ip, ip, IP_PROTO_ICMP, 40);
            sent_at = now_ms();

            process64_preempt_enable();

            /*
             * Not sent means the MAC address is still being looked up;
             * try again in a moment.
             */
            if (!sent) {
                sleep_ms(50);
                continue;
            }
        }

        sleep_ms(1);
    }

    g_ping_waiting = 0;

    if (!g_ping_done) {
        return 0;
    }

    if (out_ms) {
        *out_ms = (nq_u32)(now_ms() - sent_at);
    }

    return 1;
}

/* -------------------------------------------------------------------------- */
/* Shell commands                                                              */
/* -------------------------------------------------------------------------- */

void net64_print_status(void) {
    static const char hex[] = "0123456789ABCDEF";

    if (!nic64_present()) {
        k64_print("No supported network card found.\n");
        return;
    }

    const nq_u8* mac = nic64_mac();
    nq_u32 speed = 0;
    int up = nic64_link(&speed);

    k64_print("Card:     ");
    k64_print(nic64_name());
    k64_print("\n");

    k64_print("Address:  ");

    for (int i = 0; i < 6; i++) {
        k64_putchar(hex[mac[i] >> 4]);
        k64_putchar(hex[mac[i] & 0xFu]);

        if (i < 5) {
            k64_putchar(':');
        }
    }

    k64_print("\n");

    k64_print("Link:     ");

    if (up) {
        k64_print("up, ");
        k64_print_dec64(speed);
        k64_print(" Mb/s\n");
    } else {
        k64_print("down (no cable?)\n");
    }

    if (g_have_ip) {
        k64_print("IP:       ");
        print_ip(g_ip);
        k64_print("\nMask:     ");
        print_ip(g_mask);
        k64_print("\nGateway:  ");
        print_ip(g_gateway);
        k64_print("\nDNS:      ");
        print_ip(g_dns);
        k64_print("\n");
    } else if (up) {
        k64_print("IP:       waiting for an address (DHCP)\n");
    } else {
        k64_print("IP:       none\n");
    }

    k64_print("Frames:   ");
    k64_print_dec64(nic64_rx_count());
    k64_print(" received, ");
    k64_print_dec64(nic64_tx_count());
    k64_print(" sent\n");
}

void net64_shell_dns(const char* name) {
    nq_u32 ip = 0;

    if (!g_have_ip) {
        k64_print("The network is not up yet. Try 'net'.\n");
        return;
    }

    if (!net64_resolve(name, &ip)) {
        k64_print("Could not find ");
        k64_print(name);
        k64_print("\n");
        return;
    }

    k64_print(name);
    k64_print(" is ");
    print_ip(ip);
    k64_print("\n");
}

void net64_shell_ping(const char* host) {
    nq_u32 ip = 0;
    nq_u32 received = 0;

    if (!g_have_ip) {
        k64_print("The network is not up yet. Try 'net'.\n");
        return;
    }

    if (!net64_resolve(host, &ip)) {
        k64_print("Could not find ");
        k64_print(host);
        k64_print("\n");
        return;
    }

    k64_print("Pinging ");
    print_ip(ip);
    k64_print("\n");

    for (nq_u32 i = 0; i < 4u; i++) {
        nq_u32 ms = 0;

        if (net64_ping(ip, 2000, &ms)) {
            k64_print("  reply in ");
            k64_print_dec64(ms);
            k64_print(" ms\n");
            received++;
        } else {
            k64_print("  no reply\n");
        }

        if (i < 3u) {
            sleep_ms(500);
        }
    }

    k64_print_dec64(received);
    k64_print(" of 4 answered\n");
}

/* -------------------------------------------------------------------------- */
/* TCP                                                                         */
/* -------------------------------------------------------------------------- */

/*
 * TCP turns the network's unreliable packets into a reliable stream of
 * bytes in each direction. Every byte has a sequence number; the receiver
 * acknowledges the number of the next byte it expects, and the sender
 * keeps anything not yet acknowledged so it can send it again.
 *
 * This is a client-only implementation (it opens connections, it does not
 * accept them), kept deliberately simple:
 *   - data that arrives out of order is dropped and re-requested rather
 *     than held,
 *   - when an acknowledgement is overdue everything unacknowledged is
 *     sent again ("go back N"), with the wait doubling each time,
 *   - no window scaling, so at most 64 KiB is in flight toward us.
 *
 * A segment is [20-byte header][data]:
 *   ports, sequence number, acknowledgement number, header length and
 *   flags, window (how much more we can take), checksum.
 */

#define TCP_HEADER 20u

#define TCP_FIN 0x01u
#define TCP_SYN 0x02u
#define TCP_RST 0x04u
#define TCP_PSH 0x08u
#define TCP_ACK 0x10u

#define TCP_SOCKETS 8
#define TCP_RX_SIZE 65536u
#define TCP_TX_SIZE 16384u
#define TCP_MSS     1460u

#define TCP_FREE       0
#define TCP_CONNECTING 1
#define TCP_OPEN       2
#define TCP_CLOSED     3

typedef struct tcp_sock {
    int state;
    int error;
    int owner_pid;

    nq_u32 remote_ip;
    nq_u16 remote_port;
    nq_u16 local_port;

    /*
     * snd_una: first byte we sent that is not acknowledged yet.
     * snd_nxt: next sequence number we will send.
     * rcv_nxt: next byte we expect from the other side.
     */
    nq_u32 snd_una;
    nq_u32 snd_nxt;
    nq_u32 rcv_nxt;

    nq_u32 peer_window;
    nq_u32 mss;

    /*
     * Received bytes the program has not read yet (a ring), and bytes it
     * wrote that are not acknowledged yet (starting at snd_una).
     */
    nq_u8* rx;
    nq_u32 rx_head;
    nq_u32 rx_count;

    nq_u8* tx;
    nq_u32 tx_count;

    int peer_fin;       /* the other side has finished sending */
    int fin_wanted;     /* we want to finish once our data is out */
    int fin_sent;
    int detached;       /* the program has let go of it */

    nq_u64 last_send;
    nq_u32 rto;
    nq_u32 retries;
    nq_u64 free_at;
} tcp_sock_t;

static tcp_sock_t g_tcp[TCP_SOCKETS];

static tcp_sock_t* tcp_from_handle(int handle) {
    if (handle < 1 || handle > TCP_SOCKETS) {
        return 0;
    }

    tcp_sock_t* s = &g_tcp[handle - 1];

    return s->state == TCP_FREE ? 0 : s;
}

static nq_u32 tcp_window(const tcp_sock_t* s) {
    nq_u32 space = TCP_RX_SIZE - s->rx_count;

    return space > 65535u ? 65535u : space;
}

/*
 * The TCP checksum also covers a "pseudo header" of the IP addresses,
 * the protocol number and the segment length.
 */
static nq_u16 tcp_checksum(nq_u32 src_ip, nq_u32 dst_ip, const nq_u8* segment, nq_u32 length) {
    nq_u32 sum = 0;

    sum += (src_ip >> 16) + (src_ip & 0xFFFFu);
    sum += (dst_ip >> 16) + (dst_ip & 0xFFFFu);
    sum += IP_PROTO_TCP;
    sum += length;

    for (nq_u32 i = 0; i + 1u < length; i += 2u) {
        sum += get16(segment + i);
    }

    if (length & 1u) {
        sum += (nq_u32)segment[length - 1u] << 8;
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }

    return (nq_u16)~sum;
}

/*
 * Builds and sends one segment. Guard held. Returns 0 if it could not go
 * out (the MAC address is still being looked up, or the ring is full).
 */
static int tcp_send_segment(
    tcp_sock_t* s,
    nq_u32 seq,
    nq_u8 flags,
    const nq_u8* data,
    nq_u32 length
) {
    nq_u8* tcp = g_tx_frame + ETH_HEADER + IP_HEADER;
    nq_u32 header = TCP_HEADER;

    /*
     * The opening segment announces the largest segment we accept.
     */
    if (flags & TCP_SYN) {
        tcp[20] = 2;
        tcp[21] = 4;
        put16(tcp + 22, (nq_u16)TCP_MSS);
        header += 4u;
    }

    for (nq_u32 i = 0; i < length; i++) {
        tcp[header + i] = data[i];
    }

    put16(tcp + 0, s->local_port);
    put16(tcp + 2, s->remote_port);
    put32(tcp + 4, seq);
    put32(tcp + 8, (flags & TCP_ACK) ? s->rcv_nxt : 0);
    tcp[12] = (nq_u8)((header / 4u) << 4);
    tcp[13] = flags;
    put16(tcp + 14, (nq_u16)tcp_window(s));
    put16(tcp + 16, 0);
    put16(tcp + 18, 0);
    put16(tcp + 16, tcp_checksum(g_ip, s->remote_ip, tcp, header + length));

    return ip_send(g_ip, s->remote_ip, IP_PROTO_TCP, header + length);
}

/*
 * Sends whatever may go out now: new data within the other side's
 * window, then our FIN once the data is all sent.
 */
static void tcp_output(tcp_sock_t* s) {
    if (s->state != TCP_OPEN && s->state != TCP_CLOSED) {
        return;
    }

    if (s->error) {
        return;
    }

    nq_u32 in_flight = s->snd_nxt - s->snd_una;
    nq_u32 sent = in_flight > s->tx_count ? s->tx_count : in_flight;
    nq_u32 window = s->peer_window;

    if (window > 8u * s->mss) {
        window = 8u * s->mss;
    }

    if (window == 0) {
        /*
         * A closed window is probed with one segment.
         */
        window = s->mss;
    }

    while (sent < s->tx_count && sent < window) {
        nq_u32 count = s->tx_count - sent;

        if (count > s->mss) {
            count = s->mss;
        }

        if (count > window - sent) {
            count = window - sent;
        }

        if (!tcp_send_segment(s, s->snd_una + sent, TCP_ACK | TCP_PSH, s->tx + sent, count)) {
            break;
        }

        sent += count;
        s->snd_nxt = s->snd_una + sent;
        s->last_send = now_ms();
    }

    if (s->fin_wanted && !s->fin_sent && sent == s->tx_count) {
        if (tcp_send_segment(s, s->snd_una + sent, TCP_ACK | TCP_FIN, 0, 0)) {
            s->fin_sent = 1;
            s->snd_nxt = s->snd_una + sent + 1u;
            s->last_send = now_ms();
        }
    }
}

static void tcp_input(nq_u32 src_ip, const nq_u8* tcp, nq_u32 length) {
    if (length < TCP_HEADER) {
        return;
    }

    nq_u16 src_port = get16(tcp + 0);
    nq_u16 dst_port = get16(tcp + 2);
    nq_u32 seq = get32(tcp + 4);
    nq_u32 ack = get32(tcp + 8);
    nq_u32 header = (nq_u32)(tcp[12] >> 4) * 4u;
    nq_u8 flags = tcp[13];
    nq_u32 window = get16(tcp + 14);

    if (header < TCP_HEADER || header > length) {
        return;
    }

    if (tcp_checksum(src_ip, g_ip, tcp, length) != 0) {
        return;
    }

    tcp_sock_t* s = 0;

    for (int i = 0; i < TCP_SOCKETS; i++) {
        if (g_tcp[i].state != TCP_FREE &&
            g_tcp[i].remote_ip == src_ip &&
            g_tcp[i].remote_port == src_port &&
            g_tcp[i].local_port == dst_port) {
            s = &g_tcp[i];
            break;
        }
    }

    if (!s) {
        return;
    }

    if (flags & TCP_RST) {
        s->error = 1;
        s->state = TCP_CLOSED;
        return;
    }

    const nq_u8* data = tcp + header;
    nq_u32 data_length = length - header;

    if (s->state == TCP_CONNECTING) {
        if ((flags & (TCP_SYN | TCP_ACK)) != (TCP_SYN | TCP_ACK) || ack != s->snd_nxt) {
            return;
        }

        /*
         * Options: kind 2 is the other side's maximum segment size.
         */
        for (nq_u32 i = TCP_HEADER; i + 1u < header; ) {
            nq_u8 kind = tcp[i];

            if (kind == 0) {
                break;
            }

            if (kind == 1) {
                i++;
                continue;
            }

            nq_u32 size = tcp[i + 1u];

            if (size < 2u || i + size > header) {
                break;
            }

            if (kind == 2 && size == 4u) {
                nq_u32 mss = get16(tcp + i + 2u);

                if (mss >= 200u && mss < s->mss) {
                    s->mss = mss;
                }
            }

            i += size;
        }

        s->rcv_nxt = seq + 1u;
        s->snd_una = ack;
        s->peer_window = window;
        s->state = TCP_OPEN;
        s->rto = 500;
        s->retries = 0;

        tcp_send_segment(s, s->snd_nxt, TCP_ACK, 0, 0);

        return;
    }

    /*
     * Acknowledgement: drop what the other side now has.
     */
    if (flags & TCP_ACK) {
        nq_u32 acked = ack - s->snd_una;
        nq_u32 in_flight = s->snd_nxt - s->snd_una;

        if (acked > 0 && acked <= in_flight) {
            nq_u32 data_acked = acked > s->tx_count ? s->tx_count : acked;

            for (nq_u32 i = data_acked; i < s->tx_count; i++) {
                s->tx[i - data_acked] = s->tx[i];
            }

            s->tx_count -= data_acked;
            s->snd_una = ack;
            s->rto = 500;
            s->retries = 0;
            s->last_send = now_ms();
        }
    }

    s->peer_window = window;

    int reply = 0;

    if (data_length > 0) {
        /*
         * How far into this segment our expected byte lies. Zero for the
         * usual in-order segment; positive when part of it is a repeat.
         */
        nq_u32 skip = s->rcv_nxt - seq;

        if (skip < data_length) {
            nq_u32 count = data_length - skip;
            nq_u32 space = TCP_RX_SIZE - s->rx_count;

            if (count > space) {
                count = space;
            }

            nq_u32 tail = (s->rx_head + s->rx_count) % TCP_RX_SIZE;

            for (nq_u32 i = 0; i < count; i++) {
                s->rx[(tail + i) % TCP_RX_SIZE] = data[skip + i];
            }

            s->rx_count += count;
            s->rcv_nxt += count;
        }

        /*
         * Always answer data, in order or not: the acknowledgement tells
         * the sender where we really are.
         */
        reply = 1;
    }

    if ((flags & TCP_FIN) && !s->peer_fin && seq + data_length == s->rcv_nxt) {
        s->rcv_nxt++;
        s->peer_fin = 1;
        reply = 1;
    }

    if (reply) {
        tcp_send_segment(s, s->snd_nxt, TCP_ACK, 0, 0);
    }

    tcp_output(s);

    if (s->peer_fin && s->fin_sent && s->snd_una == s->snd_nxt) {
        s->state = TCP_CLOSED;
    }
}

static void tcp_release(tcp_sock_t* s) {
    kfree64(s->rx);
    kfree64(s->tx);

    s->rx = 0;
    s->tx = 0;
    s->state = TCP_FREE;
}

/*
 * Retransmission and clean-up. Called by the network task on every pass,
 * guard held.
 */
static void tcp_tick(void) {
    nq_u64 now = now_ms();

    for (int i = 0; i < TCP_SOCKETS; i++) {
        tcp_sock_t* s = &g_tcp[i];

        if (s->state == TCP_FREE) {
            continue;
        }

        if (s->detached &&
            (s->state == TCP_CLOSED || s->error || now >= s->free_at)) {
            /*
             * Let go without a goodbye only if the connection is not
             * already finished: tell the other side with a reset.
             */
            if (s->state != TCP_CLOSED && !s->error && s->state == TCP_OPEN) {
                tcp_send_segment(s, s->snd_nxt, TCP_RST | TCP_ACK, 0, 0);
            }

            tcp_release(s);
            continue;
        }

        if (s->error || s->state == TCP_CLOSED) {
            continue;
        }

        if (s->snd_una == s->snd_nxt && s->state != TCP_CONNECTING) {
            /*
             * Nothing in flight. There may still be data waiting for the
             * window to open.
             */
            if (s->tx_count || (s->fin_wanted && !s->fin_sent)) {
                if (now - s->last_send >= 200u) {
                    s->last_send = now;
                    tcp_output(s);
                }
            }

            continue;
        }

        if (now - s->last_send < s->rto) {
            continue;
        }

        /*
         * Overdue: send again, and wait twice as long next time.
         */
        if (++s->retries > 7u) {
            s->error = 1;
            s->state = TCP_CLOSED;
            continue;
        }

        s->last_send = now;

        if (s->rto < 8000u) {
            s->rto *= 2u;
        }

        if (s->state == TCP_CONNECTING) {
            tcp_send_segment(s, s->snd_una, TCP_SYN, 0, 0);
        } else {
            s->snd_nxt = s->snd_una;
            s->fin_sent = 0;
            tcp_output(s);
        }
    }
}

int net64_tcp_open(nq_u32 ip, nq_u16 port, int owner_pid) {
    if (!g_have_ip || ip == 0 || port == 0) {
        return -1;
    }

    nq_u8* rx = (nq_u8*)kmalloc64(TCP_RX_SIZE);
    nq_u8* tx = (nq_u8*)kmalloc64(TCP_TX_SIZE);

    if (!rx || !tx) {
        kfree64(rx);
        kfree64(tx);
        return -1;
    }

    process64_preempt_disable();

    int handle = -1;

    for (int i = 0; i < TCP_SOCKETS; i++) {
        if (g_tcp[i].state == TCP_FREE) {
            handle = i + 1;
            break;
        }
    }

    if (handle < 0) {
        process64_preempt_enable();
        kfree64(rx);
        kfree64(tx);
        return -1;
    }

    tcp_sock_t* s = &g_tcp[handle - 1];
    nq_u8* raw = (nq_u8*)s;

    for (nq_u32 i = 0; i < sizeof(*s); i++) {
        raw[i] = 0;
    }

    s->rx = rx;
    s->tx = tx;
    s->owner_pid = owner_pid;
    s->remote_ip = ip;
    s->remote_port = port;
    s->local_port = (nq_u16)(49152u + (next_random() % 16000u));
    s->snd_una = next_random();
    s->snd_nxt = s->snd_una + 1u;
    s->mss = TCP_MSS;
    s->peer_window = TCP_MSS;
    s->state = TCP_CONNECTING;

    /*
     * The first try often only gets as far as looking up the MAC address,
     * so the first retry comes quickly.
     */
    s->rto = 250;
    s->last_send = now_ms();

    tcp_send_segment(s, s->snd_una, TCP_SYN, 0, 0);

    process64_preempt_enable();

    return handle;
}

static tcp_sock_t* tcp_owned(int handle, int owner_pid) {
    tcp_sock_t* s = tcp_from_handle(handle);

    if (!s || s->detached || s->owner_pid != owner_pid) {
        return 0;
    }

    return s;
}

int net64_tcp_state(int handle, int owner_pid) {
    process64_preempt_disable();

    tcp_sock_t* s = tcp_owned(handle, owner_pid);
    int result;

    if (!s || s->error) {
        result = NET64_TCP_ERROR;
    } else if (s->state == TCP_CONNECTING) {
        result = NET64_TCP_CONNECTING;
    } else if (s->state == TCP_OPEN && !s->peer_fin) {
        result = NET64_TCP_OPEN;
    } else {
        result = NET64_TCP_CLOSED;
    }

    process64_preempt_enable();

    return result;
}

int net64_tcp_send(int handle, int owner_pid, const void* data, nq_u32 length) {
    process64_preempt_disable();

    tcp_sock_t* s = tcp_owned(handle, owner_pid);
    int accepted = -1;

    if (s && !s->error && s->state == TCP_OPEN && !s->fin_wanted) {
        nq_u32 space = TCP_TX_SIZE - s->tx_count;
        nq_u32 count = length < space ? length : space;
        const nq_u8* src = (const nq_u8*)data;

        for (nq_u32 i = 0; i < count; i++) {
            s->tx[s->tx_count + i] = src[i];
        }

        s->tx_count += count;
        accepted = (int)count;

        tcp_output(s);
    }

    process64_preempt_enable();

    return accepted;
}

int net64_tcp_recv(int handle, int owner_pid, void* buffer, nq_u32 capacity) {
    process64_preempt_disable();

    tcp_sock_t* s = tcp_owned(handle, owner_pid);
    int result;

    if (!s) {
        result = NET64_RECV_ERROR;
    } else if (s->rx_count > 0) {
        nq_u32 count = s->rx_count < capacity ? s->rx_count : capacity;
        nq_u8* dst = (nq_u8*)buffer;
        nq_u32 window_before = tcp_window(s);

        for (nq_u32 i = 0; i < count; i++) {
            dst[i] = s->rx[(s->rx_head + i) % TCP_RX_SIZE];
        }

        s->rx_head = (s->rx_head + count) % TCP_RX_SIZE;
        s->rx_count -= count;

        /*
         * If we had been telling the sender there was almost no room,
         * say that there is room again.
         */
        if (window_before < s->mss && tcp_window(s) >= s->mss &&
            s->state == TCP_OPEN && !s->error) {
            tcp_send_segment(s, s->snd_nxt, TCP_ACK, 0, 0);
        }

        result = (int)count;
    } else if (s->error) {
        result = NET64_RECV_ERROR;
    } else if (s->peer_fin || s->state == TCP_CLOSED) {
        result = NET64_RECV_END;
    } else {
        result = 0;
    }

    process64_preempt_enable();

    return result;
}

static void tcp_detach(tcp_sock_t* s) {
    s->detached = 1;
    s->fin_wanted = 1;
    s->free_at = now_ms() + 3000u;

    tcp_output(s);
}

void net64_tcp_close(int handle, int owner_pid) {
    process64_preempt_disable();

    tcp_sock_t* s = tcp_owned(handle, owner_pid);

    if (s) {
        tcp_detach(s);
    }

    process64_preempt_enable();
}

void net64_tcp_close_pid(int owner_pid) {
    process64_preempt_disable();

    for (int i = 0; i < TCP_SOCKETS; i++) {
        tcp_sock_t* s = &g_tcp[i];

        if (s->state != TCP_FREE && !s->detached && s->owner_pid == owner_pid) {
            tcp_detach(s);
        }
    }

    process64_preempt_enable();
}

/*
 * "http <host> [path]": fetches a page over plain HTTP and prints the
 * start of the answer. A quick end-to-end check of TCP.
 */
void net64_shell_http(const char* arg) {
    char host[96];
    char request[256];
    static nq_u8 chunk[1024];
    nq_u32 n = 0;
    nq_u32 ip = 0;

    while (arg[n] && arg[n] != ' ' && n + 1u < sizeof(host)) {
        host[n] = arg[n];
        n++;
    }

    host[n] = 0;

    const char* path = arg + n;

    while (*path == ' ') {
        path++;
    }

    if (!*path) {
        path = "/";
    }

    if (!g_have_ip) {
        k64_print("The network is not up yet. Try 'net'.\n");
        return;
    }

    if (!host[0] || !net64_resolve(host, &ip)) {
        k64_print("Could not find that host.\n");
        return;
    }

    int handle = net64_tcp_open(ip, 80, 0);

    if (handle < 0) {
        k64_print("Could not open a connection.\n");
        return;
    }

    nq_u64 start = now_ms();

    while (net64_tcp_state(handle, 0) == NET64_TCP_CONNECTING && now_ms() - start < 6000u) {
        sleep_ms(5);
    }

    if (net64_tcp_state(handle, 0) != NET64_TCP_OPEN) {
        k64_print("The server did not answer.\n");
        net64_tcp_close(handle, 0);
        return;
    }

    /*
     * "GET <path> HTTP/1.0" with a Host line; the server closes the
     * connection when it has sent everything.
     */
    nq_u32 r = 0;
    const char* parts[6] = { "GET ", path, " HTTP/1.0\r\nHost: ", host,
                             "\r\nUser-Agent: NAQOS\r\nConnection: close\r\n\r\n", 0 };

    for (int p = 0; parts[p]; p++) {
        for (nq_u32 i = 0; parts[p][i] && r + 1u < sizeof(request); i++) {
            request[r++] = parts[p][i];
        }
    }

    net64_tcp_send(handle, 0, request, r);

    nq_u32 total = 0;
    nq_u32 shown = 0;

    start = now_ms();

    while (now_ms() - start < 10000u) {
        int got = net64_tcp_recv(handle, 0, chunk, sizeof(chunk));

        if (got == NET64_RECV_END || got == NET64_RECV_ERROR) {
            break;
        }

        if (got == 0) {
            sleep_ms(5);
            continue;
        }

        for (int i = 0; i < got && shown < 700u; i++, shown++) {
            char c = (char)chunk[i];

            if (c == '\n' || (c >= 32 && c < 127)) {
                k64_putchar(c);
            }
        }

        total += (nq_u32)got;
        start = now_ms();
    }

    net64_tcp_close(handle, 0);

    k64_print("\n[");
    k64_print_dec64(total);
    k64_print(" bytes received]\n");
}
