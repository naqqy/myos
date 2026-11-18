#include "bt_internal64.h"
#include "sbc64.h"

#include "../sound/sound64.h"

/*
 * Bluetooth audio: what runs over a connection to a headset.
 *
 *   L2CAP    splits the one radio link into numbered channels. Channel 1
 *            is for setting the others up: "connect me to service X",
 *            "here are my options".
 *   SDP      the headset may ask what services this computer offers. We
 *            answer "nothing listed", which is enough.
 *   AVDTP    the audio set-up conversation, on its own channel: which
 *            stream endpoints do you have, what can they do, use these
 *            settings, open, start.
 *   media    a second channel that carries the sound: packets of SBC
 *            frames with a small RTP header.
 *
 * Either side may start any of these conversations, so every request the
 * headset can make is answered as well as made.
 */

#define PSM_SDP    0x0001u
#define PSM_AVCTP  0x0017u
#define PSM_AVDTP  0x0019u

#define CID_SIGNAL 0x0001u

/* L2CAP signalling commands */
#define L2_REJECT        0x01u
#define L2_CONN_REQ      0x02u
#define L2_CONN_RSP      0x03u
#define L2_CONFIG_REQ    0x04u
#define L2_CONFIG_RSP    0x05u
#define L2_DISCONN_REQ   0x06u
#define L2_DISCONN_RSP   0x07u
#define L2_ECHO_REQ      0x08u
#define L2_ECHO_RSP      0x09u
#define L2_INFO_REQ      0x0Au
#define L2_INFO_RSP      0x0Bu

#define KIND_SDP    1u
#define KIND_AVCTP  2u
#define KIND_SIGNAL 3u      /* AVDTP signalling */
#define KIND_MEDIA  4u

#define MAX_CHANNELS 6u
#define OUR_MTU      1017u
#define FRAME_MAX    1100u

typedef struct channel {
    nq_u8 used;
    nq_u8 kind;
    nq_u8 ours_done;        /* the headset accepted our options */
    nq_u8 theirs_done;      /* we accepted the headset's options */
    nq_u8 open;
    nq_u8 pad;
    nq_u16 local;           /* our number for the channel */
    nq_u16 remote;          /* the headset's number for it */
    nq_u16 out_mtu;         /* largest packet the headset takes on it */
} channel_t;

/* AVDTP signals */
#define AV_DISCOVER      0x01u
#define AV_GET_CAPS      0x02u
#define AV_SET_CONFIG    0x03u
#define AV_GET_CONFIG    0x04u
#define AV_RECONFIGURE   0x05u
#define AV_OPEN          0x06u
#define AV_START         0x07u
#define AV_CLOSE         0x08u
#define AV_SUSPEND       0x09u
#define AV_ABORT         0x0Au
#define AV_GET_ALL_CAPS  0x0Cu
#define AV_DELAY_REPORT  0x0Du

#define AV_COMMAND  0u
#define AV_GENERAL_REJECT 1u
#define AV_ACCEPT   2u
#define AV_REJECT   3u

#define OUR_SEID    1u

/* SBC capability bits */
#define SBC_FREQ_48      0x10u
#define SBC_FREQ_44      0x20u
#define SBC_MODE_STEREO  0x02u
#define SBC_BLOCKS_16    0x10u
#define SBC_SUBBANDS_8   0x04u
#define SBC_LOUDNESS     0x01u

#define BITPOOL_WANTED   51u

enum {
    ST_IDLE,            /* no link */
    ST_WAITING,         /* link up; giving the headset a moment to start */
    ST_CHANNEL,         /* opening the signalling channel */
    ST_DISCOVER,
    ST_CAPABILITIES,
    ST_CONFIGURE,
    ST_CONFIGURED,      /* settings agreed (possibly at the headset's request) */
    ST_OPENING,
    ST_MEDIA,           /* waiting for the media channel */
    ST_STARTING,
    ST_STREAMING,
    ST_FAILED
};

static const char* const g_state_names[] = {
    "not connected", "waiting for the headset", "opening the audio channel",
    "asking what the headset has", "asking what it can do", "choosing settings",
    "settings agreed", "opening the stream", "opening the sound channel",
    "starting", "playing", "audio set-up failed"
};

static channel_t g_channels[MAX_CHANNELS];
static nq_u8 g_next_id = 1;             /* L2CAP signalling command numbers */

static int g_state = ST_IDLE;
static nq_u64 g_state_since = 0;
static nq_u8 g_label = 0;               /* AVDTP transaction numbers */
static nq_u8 g_remote_seid = 0;
static nq_u8 g_remote_caps[4];
static int g_have_caps = 0;
static int g_remote_opened = 0;         /* the headset sent OPEN, so it makes the media channel */

static sbc64_t g_sbc;
static nq_u32 g_bitpool = BITPOOL_WANTED;
static nq_u16 g_sequence = 0;
static nq_u32 g_timestamp = 0;
static nq_u64 g_sent = 0;
static nq_u64 g_skipped = 0;

static nq_u8 g_out[FRAME_MAX];
static nq_s16 g_pcm[SBC64_FRAME_SAMPLES * 2u];

static void set_state(int state) {
    if (state != g_state) {
        g_state = state;
        g_state_since = bt_now_ms();

        bt_trace("audio: ");
        bt_trace(g_state_names[state]);
        bt_trace("\n");
    }
}

/* -------------------------------------------------------------------------- */
/* L2CAP                                                                       */
/* -------------------------------------------------------------------------- */

static void put16(nq_u8* p, nq_u32 v) {
    p[0] = (nq_u8)(v & 0xFFu);
    p[1] = (nq_u8)((v >> 8) & 0xFFu);
}

static nq_u32 get16(const nq_u8* p) {
    return (nq_u32)p[0] | ((nq_u32)p[1] << 8);
}

/* Sends `length` bytes of payload on the headset's channel `cid`. */
static int l2cap_send(nq_u32 cid, const nq_u8* payload, nq_u32 length) {
    static nq_u8 frame[FRAME_MAX + 4u];

    if (length > FRAME_MAX) {
        return 0;
    }

    put16(frame, length);
    put16(frame + 2, cid);

    for (nq_u32 i = 0; i < length; i++) {
        frame[4u + i] = payload[i];
    }

    return bt_acl_send(frame, length + 4u);
}

static void signal_send(nq_u32 code, nq_u32 id, const nq_u8* data, nq_u32 length) {
    nq_u8 packet[64];

    if (length > sizeof(packet) - 4u) {
        return;
    }

    packet[0] = (nq_u8)code;
    packet[1] = (nq_u8)id;
    put16(packet + 2, length);

    for (nq_u32 i = 0; i < length; i++) {
        packet[4u + i] = data[i];
    }

    l2cap_send(CID_SIGNAL, packet, length + 4u);
}

static nq_u8 next_id(void) {
    g_next_id++;

    if (g_next_id == 0) {
        g_next_id = 1;
    }

    return g_next_id;
}

static channel_t* channel_new(nq_u32 kind) {
    for (nq_u32 i = 0; i < MAX_CHANNELS; i++) {
        channel_t* c = &g_channels[i];

        if (!c->used) {
            c->used = 1;
            c->kind = (nq_u8)kind;
            c->ours_done = 0;
            c->theirs_done = 0;
            c->open = 0;
            c->local = (nq_u16)(0x0040u + i);
            c->remote = 0;
            c->out_mtu = 672;

            return c;
        }
    }

    return 0;
}

static channel_t* channel_by_local(nq_u32 cid) {
    for (nq_u32 i = 0; i < MAX_CHANNELS; i++) {
        if (g_channels[i].used && g_channels[i].local == cid) {
            return &g_channels[i];
        }
    }

    return 0;
}

static channel_t* channel_of_kind(nq_u32 kind) {
    for (nq_u32 i = 0; i < MAX_CHANNELS; i++) {
        if (g_channels[i].used && g_channels[i].kind == kind) {
            return &g_channels[i];
        }
    }

    return 0;
}

/* "These are my options for the channel": just the packet size we accept. */
static void send_config_request(const channel_t* c) {
    nq_u8 data[8];

    put16(data, c->remote);
    put16(data + 2, 0);
    data[4] = 0x01;         /* option: maximum packet size */
    data[5] = 2;
    put16(data + 6, OUR_MTU);

    signal_send(L2_CONFIG_REQ, next_id(), data, 8);
}

/* Asks the headset for a channel to one of its services. */
static channel_t* l2cap_connect(nq_u32 psm, nq_u32 kind) {
    channel_t* c = channel_new(kind);
    nq_u8 data[4];

    if (!c) {
        return 0;
    }

    put16(data, psm);
    put16(data + 2, c->local);
    signal_send(L2_CONN_REQ, next_id(), data, 4);

    return c;
}

static void avdtp_channel_open(void);
static void media_channel_open(void);

static void channel_maybe_open(channel_t* c) {
    if (c->open || !c->ours_done || !c->theirs_done) {
        return;
    }

    c->open = 1;

    if (c->kind == KIND_SIGNAL) {
        avdtp_channel_open();
    } else if (c->kind == KIND_MEDIA) {
        media_channel_open();
    }
}

static void signal_input(const nq_u8* data, nq_u32 length) {
    nq_u32 at = 0;

    while (at + 4u <= length) {
        nq_u32 code = data[at];
        nq_u32 id = data[at + 1u];
        nq_u32 len = get16(data + at + 2u);
        const nq_u8* p = data + at + 4u;
        nq_u8 reply[24];

        if (at + 4u + len > length) {
            break;
        }

        switch (code) {
            case L2_CONN_REQ: {
                if (len < 4u) {
                    break;
                }

                nq_u32 psm = get16(p);
                nq_u32 their_cid = get16(p + 2);
                nq_u32 kind = 0;

                if (psm == PSM_SDP) {
                    kind = KIND_SDP;
                } else if (psm == PSM_AVCTP) {
                    kind = KIND_AVCTP;
                } else if (psm == PSM_AVDTP) {
                    /* the first such channel is for signalling, the second for sound */
                    kind = channel_of_kind(KIND_SIGNAL) ? KIND_MEDIA : KIND_SIGNAL;
                }

                channel_t* c = kind ? channel_new(kind) : 0;

                bt_trace("l2cap: headset asks for service 0x");
                bt_trace_hex(psm, 4);
                bt_trace(c ? ": accepted\n" : ": refused\n");

                put16(reply, c ? c->local : 0u);
                put16(reply + 2, their_cid);
                put16(reply + 4, c ? 0u : (kind ? 4u : 2u));    /* ok / no room / no such service */
                put16(reply + 6, 0);
                signal_send(L2_CONN_RSP, id, reply, 8);

                if (c) {
                    c->remote = (nq_u16)their_cid;
                    send_config_request(c);

                    if (kind == KIND_SIGNAL && g_state <= ST_CHANNEL) {
                        set_state(ST_CHANNEL);
                    }
                }

                break;
            }

            case L2_CONN_RSP: {
                if (len < 8u) {
                    break;
                }

                channel_t* c = channel_by_local(get16(p + 2));
                nq_u32 result = get16(p + 4);

                if (!c) {
                    break;
                }

                if (result == 0) {
                    c->remote = (nq_u16)get16(p);
                    send_config_request(c);
                } else if (result != 1u) {      /* 1 = "wait, still deciding" */
                    bt_trace("l2cap: headset refused a channel, result ");
                    bt_trace_dec(result);
                    bt_trace("\n");

                    if (c->kind == KIND_SIGNAL || c->kind == KIND_MEDIA) {
                        set_state(ST_FAILED);
                    }

                    c->used = 0;
                }

                break;
            }

            case L2_CONFIG_REQ: {
                if (len < 4u) {
                    break;
                }

                channel_t* c = channel_by_local(get16(p));

                if (!c) {
                    break;
                }

                /* options: type, length, value ... */
                for (nq_u32 o = 4; o + 2u <= len; ) {
                    nq_u32 type = p[o] & 0x7Fu;
                    nq_u32 olen = p[o + 1u];

                    if (type == 0x01u && olen == 2u && o + 4u <= len) {
                        c->out_mtu = (nq_u16)get16(p + o + 2u);
                    }

                    o += 2u + olen;
                }

                put16(reply, c->remote);
                put16(reply + 2, 0);
                put16(reply + 4, 0);        /* accepted */
                signal_send(L2_CONFIG_RSP, id, reply, 6);

                c->theirs_done = 1;
                channel_maybe_open(c);
                break;
            }

            case L2_CONFIG_RSP: {
                if (len < 6u) {
                    break;
                }

                channel_t* c = channel_by_local(get16(p));

                if (c && get16(p + 4) == 0) {
                    c->ours_done = 1;
                    channel_maybe_open(c);
                } else if (c) {
                    bt_trace("l2cap: headset rejected our channel options\n");
                }

                break;
            }

            case L2_DISCONN_REQ: {
                if (len < 4u) {
                    break;
                }

                channel_t* c = channel_by_local(get16(p));

                put16(reply, get16(p));
                put16(reply + 2, get16(p + 2));
                signal_send(L2_DISCONN_RSP, id, reply, 4);

                if (c) {
                    if (c->kind == KIND_MEDIA || c->kind == KIND_SIGNAL) {
                        bt_trace("l2cap: headset closed the audio channel\n");

                        if (g_state == ST_STREAMING) {
                            sound64_capture(0);
                        }

                        set_state(c->kind == KIND_SIGNAL ? ST_FAILED : ST_CONFIGURED);
                    }

                    c->used = 0;
                }

                break;
            }

            case L2_ECHO_REQ:
                signal_send(L2_ECHO_RSP, id, p, len > 20u ? 20u : len);
                break;

            case L2_INFO_REQ: {
                if (len < 2u) {
                    break;
                }

                nq_u32 what = get16(p);

                put16(reply, what);

                if (what == 2u) {           /* extra features: none */
                    put16(reply + 2, 0);
                    put16(reply + 4, 0);
                    put16(reply + 6, 0);
                    signal_send(L2_INFO_RSP, id, reply, 8);
                } else if (what == 3u) {    /* fixed channels: signalling only */
                    put16(reply + 2, 0);
                    reply[4] = 0x02;

                    for (nq_u32 i = 5; i < 12u; i++) {
                        reply[i] = 0;
                    }

                    signal_send(L2_INFO_RSP, id, reply, 12);
                } else {
                    put16(reply + 2, 1);    /* not supported */
                    signal_send(L2_INFO_RSP, id, reply, 4);
                }

                break;
            }

            default:
                /* answers we do not wait for, and anything unknown, are ignored */
                break;
        }

        at += 4u + len;
    }
}

/* -------------------------------------------------------------------------- */
/* SDP: "what services do you offer?"                                          */
/* -------------------------------------------------------------------------- */

static void sdp_input(const channel_t* c, const nq_u8* data, nq_u32 length) {
    nq_u8 reply[16];

    if (length < 5u) {
        return;
    }

    reply[1] = data[1];     /* transaction number, echoed */
    reply[2] = data[2];

    if (data[0] == 0x06u) {
        /* search + attributes: an empty list */
        reply[0] = 0x07;
        reply[3] = 0;
        reply[4] = 5;
        reply[5] = 0;
        reply[6] = 2;
        reply[7] = 0x35;    /* a sequence ... */
        reply[8] = 0x00;    /* ... of nothing */
        reply[9] = 0;       /* no more to come */
        l2cap_send(c->remote, reply, 10);
    } else if (data[0] == 0x02u) {
        /* search: no matches */
        reply[0] = 0x03;
        reply[3] = 0;
        reply[4] = 5;
        reply[5] = 0;
        reply[6] = 0;
        reply[7] = 0;
        reply[8] = 0;
        reply[9] = 0;
        l2cap_send(c->remote, reply, 10);
    } else {
        /* anything else: "no such record" */
        reply[0] = 0x01;
        reply[3] = 0;
        reply[4] = 2;
        reply[5] = 0;
        reply[6] = 2;
        l2cap_send(c->remote, reply, 7);
    }
}

/* -------------------------------------------------------------------------- */
/* AVDTP                                                                       */
/* -------------------------------------------------------------------------- */

static void avdtp_send(nq_u32 label, nq_u32 type, nq_u32 signal,
                       const nq_u8* data, nq_u32 length) {
    channel_t* c = channel_of_kind(KIND_SIGNAL);
    nq_u8 packet[48];

    if (!c || !c->open || length > sizeof(packet) - 2u) {
        return;
    }

    packet[0] = (nq_u8)((label << 4) | type);
    packet[1] = (nq_u8)signal;

    for (nq_u32 i = 0; i < length; i++) {
        packet[2u + i] = data[i];
    }

    l2cap_send(c->remote, packet, length + 2u);
}

static void avdtp_command(nq_u32 signal, const nq_u8* data, nq_u32 length) {
    g_label = (nq_u8)((g_label + 1u) & 0xFu);
    avdtp_send(g_label, AV_COMMAND, signal, data, length);
}

/* The one kind of stream we can produce, as a capability list. */
static nq_u32 our_capabilities(nq_u8* out, nq_u32 max_bitpool) {
    out[0] = 0x01;      /* media transport */
    out[1] = 0;
    out[2] = 0x07;      /* media codec */
    out[3] = 6;
    out[4] = 0x00;      /* audio */
    out[5] = 0x00;      /* SBC */
    out[6] = SBC_FREQ_48 | SBC_MODE_STEREO;
    out[7] = SBC_BLOCKS_16 | SBC_SUBBANDS_8 | SBC_LOUDNESS;
    out[8] = 2;
    out[9] = (nq_u8)max_bitpool;

    return 10;
}

static void stream_begin(void) {
    sbc64_init(&g_sbc, 48000u, g_bitpool);
    g_sequence = 0;
    g_timestamp = 0;

    sound64_capture(1);
    set_state(ST_STREAMING);

    bt_trace("audio: SBC 48 kHz stereo, bitpool ");
    bt_trace_dec(g_bitpool);
    bt_trace("\n");

    /* so there is something to hear straight away */
    sound64_play(SOUND64_NOTIFY);
}

static void avdtp_channel_open(void) {
    bt_trace("audio: signalling channel open\n");

    if (g_state < ST_DISCOVER) {
        /* give the headset a moment in case it wants to lead */
        set_state(ST_CHANNEL);
        g_state_since = bt_now_ms();
    }
}

static void media_channel_open(void) {
    bt_trace("audio: sound channel open\n");

    if (g_state == ST_MEDIA || g_state == ST_OPENING || g_state == ST_CONFIGURED) {
        nq_u8 seid = (nq_u8)(g_remote_seid << 2);

        if (g_remote_opened) {
            /* the headset is leading; it should send START. Wait for it. */
            set_state(ST_MEDIA);
            g_state_since = bt_now_ms();
        } else {
            set_state(ST_STARTING);
            avdtp_command(AV_START, &seid, 1);
        }
    }
}

/* Chooses settings from the headset's capabilities and proposes them. */
static void send_configuration(void) {
    nq_u8 data[12];
    nq_u32 max_bitpool = g_remote_caps[3];

    if (!(g_remote_caps[0] & SBC_FREQ_48) || !(g_remote_caps[0] & SBC_MODE_STEREO) ||
        !(g_remote_caps[1] & SBC_BLOCKS_16) || !(g_remote_caps[1] & SBC_SUBBANDS_8) ||
        !(g_remote_caps[1] & SBC_LOUDNESS)) {
        bt_trace("audio: the headset lacks a setting this encoder needs, caps ");
        bt_trace_hex(g_remote_caps[0], 2);
        bt_trace_hex(g_remote_caps[1], 2);
        bt_trace("\n");
        set_state(ST_FAILED);
        return;
    }

    g_bitpool = max_bitpool < BITPOOL_WANTED ? max_bitpool : BITPOOL_WANTED;

    if (g_bitpool < g_remote_caps[2]) {
        g_bitpool = g_remote_caps[2];
    }

    data[0] = (nq_u8)(g_remote_seid << 2);
    data[1] = (nq_u8)(OUR_SEID << 2);
    our_capabilities(data + 2, g_bitpool);
    data[2 + 8] = (nq_u8)(g_remote_caps[2] > 2u ? g_remote_caps[2] : 2u);

    set_state(ST_CONFIGURE);
    avdtp_command(AV_SET_CONFIG, data, 12);
}

static void avdtp_reject_log(nq_u32 signal, const nq_u8* p, nq_u32 len) {
    bt_trace("audio: headset rejected step ");
    bt_trace_dec(signal);
    bt_trace(", code 0x");
    bt_trace_hex(len ? p[len - 1u] : 0u, 2);
    bt_trace("\n");
}

static void avdtp_response(nq_u32 type, nq_u32 signal, const nq_u8* p, nq_u32 len) {
    nq_u8 seid = (nq_u8)(g_remote_seid << 2);

    if (type != AV_ACCEPT) {
        avdtp_reject_log(signal, p, len);

        /*
         * A refused set-up usually means the headset has meanwhile set
         * the stream up itself; then its own requests carry on.
         */
        if (g_state != ST_STREAMING && g_state != ST_CONFIGURED && g_state != ST_MEDIA) {
            set_state(ST_FAILED);
        }

        return;
    }

    /*
     * An answer to a step we are no longer on (the headset took over the
     * set-up in the meantime) is not acted on.
     */
    if ((signal == AV_DISCOVER && g_state != ST_DISCOVER) ||
        ((signal == AV_GET_CAPS || signal == AV_GET_ALL_CAPS) && g_state != ST_CAPABILITIES) ||
        (signal == AV_SET_CONFIG && g_state != ST_CONFIGURE) ||
        (signal == AV_OPEN && g_state != ST_OPENING) ||
        (signal == AV_START && g_state != ST_STARTING)) {
        return;
    }

    switch (signal) {
        case AV_DISCOVER:
            g_remote_seid = 0;

            for (nq_u32 i = 0; i + 2u <= len; i += 2u) {
                nq_u32 id = p[i] >> 2;
                int in_use = (p[i] >> 1) & 1u;
                int audio = (p[i + 1u] >> 4) == 0;
                int sink = (p[i + 1u] >> 3) & 1u;

                bt_trace("audio: endpoint ");
                bt_trace_dec(id);
                bt_trace(sink ? " sink" : " source");
                bt_trace(in_use ? " (busy)\n" : "\n");

                if (audio && sink && !in_use && !g_remote_seid) {
                    g_remote_seid = (nq_u8)id;
                }
            }

            if (!g_remote_seid) {
                bt_trace("audio: the headset offers nothing to play to\n");
                set_state(ST_FAILED);
                break;
            }

            seid = (nq_u8)(g_remote_seid << 2);
            set_state(ST_CAPABILITIES);
            avdtp_command(AV_GET_CAPS, &seid, 1);
            break;

        case AV_GET_CAPS:
        case AV_GET_ALL_CAPS:
            g_have_caps = 0;

            for (nq_u32 i = 0; i + 2u <= len; ) {
                nq_u32 category = p[i];
                nq_u32 clen = p[i + 1u];

                if (i + 2u + clen > len) {
                    break;
                }

                /* media codec, audio, SBC */
                if (category == 0x07u && clen >= 6u && (p[i + 2u] >> 4) == 0 && p[i + 3u] == 0) {
                    for (nq_u32 k = 0; k < 4u; k++) {
                        g_remote_caps[k] = p[i + 4u + k];
                    }

                    g_have_caps = 1;
                }

                i += 2u + clen;
            }

            if (!g_have_caps) {
                bt_trace("audio: this endpoint does not take SBC\n");
                set_state(ST_FAILED);
                break;
            }

            bt_trace("audio: headset SBC abilities ");

            for (nq_u32 k = 0; k < 4u; k++) {
                bt_trace_hex(g_remote_caps[k], 2);
            }

            bt_trace("\n");
            send_configuration();
            break;

        case AV_SET_CONFIG:
            g_remote_opened = 0;
            set_state(ST_OPENING);
            avdtp_command(AV_OPEN, &seid, 1);
            break;

        case AV_OPEN:
            set_state(ST_MEDIA);
            l2cap_connect(PSM_AVDTP, KIND_MEDIA);
            break;

        case AV_START:
            stream_begin();
            break;

        default:
            break;
    }
}

/* A request from the headset. */
static void avdtp_request(nq_u32 label, nq_u32 signal, const nq_u8* p, nq_u32 len) {
    nq_u8 reply[16];

    switch (signal) {
        case AV_DISCOVER:
            /* one endpoint: an audio source, free unless already set up */
            reply[0] = (nq_u8)((OUR_SEID << 2) | (g_state >= ST_CONFIGURED && g_state != ST_FAILED ? 2u : 0u));
            reply[1] = 0x00;
            avdtp_send(label, AV_ACCEPT, signal, reply, 2);
            break;

        case AV_GET_CAPS:
        case AV_GET_ALL_CAPS:
            avdtp_send(label, AV_ACCEPT, signal, reply, our_capabilities(reply, BITPOOL_WANTED + 2u));
            break;

        case AV_SET_CONFIG: {
            /*
             * The headset proposes settings: its endpoint, ours, then a
             * capability list. Accept only the one form the encoder makes.
             */
            int ok = 0;
            nq_u32 max_bitpool = BITPOOL_WANTED;

            for (nq_u32 i = 2; i + 2u <= len; ) {
                nq_u32 category = p[i];
                nq_u32 clen = p[i + 1u];

                if (i + 2u + clen > len) {
                    break;
                }

                if (category == 0x07u && clen >= 6u && p[i + 3u] == 0) {
                    nq_u32 a = p[i + 4u];
                    nq_u32 b = p[i + 5u];

                    ok = (a == (SBC_FREQ_48 | SBC_MODE_STEREO)) &&
                         (b == (SBC_BLOCKS_16 | SBC_SUBBANDS_8 | SBC_LOUDNESS));
                    max_bitpool = p[i + 7u];
                }

                i += 2u + clen;
            }

            if (ok && len >= 2u) {
                g_remote_seid = (nq_u8)(p[1] >> 2);
                g_bitpool = max_bitpool < BITPOOL_WANTED ? max_bitpool : BITPOOL_WANTED;

                bt_trace("audio: headset chose the settings itself\n");
                avdtp_send(label, AV_ACCEPT, signal, reply, 0);
                set_state(ST_CONFIGURED);
            } else {
                bt_trace("audio: headset proposed settings the encoder cannot make\n");
                reply[0] = 0x07;    /* the codec part */
                reply[1] = 0x29;    /* unsupported configuration */
                avdtp_send(label, AV_REJECT, signal, reply, 2);
            }

            break;
        }

        case AV_OPEN:
            g_remote_opened = 1;
            avdtp_send(label, AV_ACCEPT, signal, reply, 0);
            set_state(ST_MEDIA);
            break;

        case AV_START:
            avdtp_send(label, AV_ACCEPT, signal, reply, 0);

            if (g_state != ST_STREAMING) {
                stream_begin();
            }

            break;

        case AV_SUSPEND:
            avdtp_send(label, AV_ACCEPT, signal, reply, 0);

            if (g_state == ST_STREAMING) {
                sound64_capture(0);
                set_state(ST_MEDIA);
                g_remote_opened = 1;
            }

            break;

        case AV_CLOSE:
        case AV_ABORT:
            avdtp_send(label, AV_ACCEPT, signal, reply, 0);

            if (g_state == ST_STREAMING) {
                sound64_capture(0);
            }

            set_state(ST_CHANNEL);
            g_state_since = bt_now_ms();
            break;

        case AV_DELAY_REPORT:
        case AV_RECONFIGURE:
            avdtp_send(label, AV_ACCEPT, signal, reply, 0);
            break;

        default:
            avdtp_send(label, AV_GENERAL_REJECT, signal, reply, 0);
            break;
    }
}

static void avdtp_input(const nq_u8* data, nq_u32 length) {
    if (length < 2u) {
        return;
    }

    nq_u32 label = data[0] >> 4;
    nq_u32 packet_type = (data[0] >> 2) & 3u;
    nq_u32 type = data[0] & 3u;
    nq_u32 signal = data[1] & 0x3Fu;

    if (packet_type != 0) {
        return;     /* a message split over several packets: not expected here */
    }

    if (type == AV_COMMAND) {
        avdtp_request(label, signal, data + 2, length - 2u);
    } else {
        avdtp_response(type, signal, data + 2, length - 2u);
    }
}

/* -------------------------------------------------------------------------- */
/* The stream                                                                  */
/* -------------------------------------------------------------------------- */

static void stream_pump(void) {
    channel_t* c = channel_of_kind(KIND_MEDIA);

    if (!c || !c->open) {
        return;
    }

    nq_u32 frame_bytes = sbc64_frame_bytes(&g_sbc);
    nq_u32 limit = c->out_mtu < OUR_MTU ? c->out_mtu : OUR_MTU;
    nq_u32 per_packet = (limit - 13u) / frame_bytes;

    if (per_packet > 6u) {
        per_packet = 6u;        /* 16 ms of sound per packet at most */
    }

    if (per_packet == 0) {
        return;
    }

    for (nq_u32 packets = 0; packets < 3u; packets++) {
        if (sound64_capture_count() < per_packet * SBC64_FRAME_SAMPLES) {
            return;
        }

        /* RTP header, then a byte saying how many SBC frames follow */
        nq_u32 n = 0;

        g_out[n++] = 0x80;
        g_out[n++] = 0x60;
        g_out[n++] = (nq_u8)(g_sequence >> 8);
        g_out[n++] = (nq_u8)g_sequence;
        g_out[n++] = (nq_u8)(g_timestamp >> 24);
        g_out[n++] = (nq_u8)(g_timestamp >> 16);
        g_out[n++] = (nq_u8)(g_timestamp >> 8);
        g_out[n++] = (nq_u8)g_timestamp;
        g_out[n++] = 0;
        g_out[n++] = 0;
        g_out[n++] = 0;
        g_out[n++] = 1;
        g_out[n++] = (nq_u8)per_packet;

        for (nq_u32 f = 0; f < per_packet; f++) {
            sound64_capture_read(g_pcm, SBC64_FRAME_SAMPLES);
            n += sbc64_encode(&g_sbc, g_pcm, g_out + n);
        }

        g_sequence++;
        g_timestamp += per_packet * SBC64_FRAME_SAMPLES;

        /* with no room in the controller the packet is dropped, not queued:
           late sound is worse than a short gap */
        if (bt_acl_room() > 1u && l2cap_send(c->remote, g_out, n)) {
            g_sent++;
        } else {
            g_skipped++;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Called from bt64.c                                                          */
/* -------------------------------------------------------------------------- */

static void reset_all(void) {
    for (nq_u32 i = 0; i < MAX_CHANNELS; i++) {
        g_channels[i].used = 0;
    }

    g_remote_seid = 0;
    g_have_caps = 0;
    g_remote_opened = 0;
}

void a2dp_link_up(int we_called) {
    reset_all();
    set_state(ST_WAITING);

    /* if we made the call we also start the audio set-up, at once */
    if (we_called) {
        g_state_since = 0;
    }
}

void a2dp_link_down(void) {
    if (g_state == ST_STREAMING) {
        sound64_capture(0);
    }

    reset_all();
    set_state(ST_IDLE);
}

void a2dp_input(const nq_u8* frame, nq_u32 length) {
    if (length < 4u) {
        return;
    }

    nq_u32 len = get16(frame);
    nq_u32 cid = get16(frame + 2);

    if (len + 4u > length) {
        return;
    }

    if (cid == CID_SIGNAL) {
        signal_input(frame + 4, len);
        return;
    }

    channel_t* c = channel_by_local(cid);

    if (!c) {
        return;
    }

    if (c->kind == KIND_SDP) {
        sdp_input(c, frame + 4, len);
    } else if (c->kind == KIND_SIGNAL) {
        avdtp_input(frame + 4, len);
    }

    /* AVCTP (remote control buttons) is accepted but not acted on */
}

void a2dp_tick(void) {
    nq_u64 now = bt_now_ms();
    nq_u8 seid = (nq_u8)(g_remote_seid << 2);

    switch (g_state) {
        case ST_WAITING:
            /* nobody has opened the audio channel yet: do it ourselves */
            if (now - g_state_since >= 1500u && !channel_of_kind(KIND_SIGNAL)) {
                set_state(ST_CHANNEL);
                l2cap_connect(PSM_AVDTP, KIND_SIGNAL);
            }

            break;

        case ST_CHANNEL: {
            channel_t* c = channel_of_kind(KIND_SIGNAL);

            if (c && c->open && now - g_state_since >= 400u) {
                set_state(ST_DISCOVER);
                avdtp_command(AV_DISCOVER, 0, 0);
            } else if (!c && now - g_state_since >= 3000u) {
                l2cap_connect(PSM_AVDTP, KIND_SIGNAL);
                g_state_since = now;
            }

            break;
        }

        case ST_CONFIGURED:
            /* the headset set things up but has not opened the stream */
            if (now - g_state_since >= 2000u && g_remote_seid) {
                g_remote_opened = 0;
                set_state(ST_OPENING);
                avdtp_command(AV_OPEN, &seid, 1);
            }

            break;

        case ST_MEDIA: {
            channel_t* c = channel_of_kind(KIND_MEDIA);

            /* channel open and the headset has not started the stream */
            if (c && c->open && g_remote_opened && now - g_state_since >= 1500u) {
                set_state(ST_STARTING);
                avdtp_command(AV_START, &seid, 1);
            }

            break;
        }

        case ST_DISCOVER:
        case ST_CAPABILITIES:
        case ST_CONFIGURE:
        case ST_OPENING:
        case ST_STARTING:
            if (now - g_state_since >= 6000u) {
                bt_trace("audio: no answer from the headset\n");
                set_state(ST_FAILED);
            }

            break;

        case ST_STREAMING:
            stream_pump();
            break;

        default:
            break;
    }
}

int a2dp_streaming(void) {
    return g_state == ST_STREAMING;
}

const char* a2dp_state(void) {
    return g_state_names[g_state];
}

nq_u64 a2dp_packets_sent(void) {
    return g_sent;
}

nq_u64 a2dp_packets_skipped(void) {
    return g_skipped;
}

nq_u32 a2dp_bitpool(void) {
    return g_bitpool;
}
