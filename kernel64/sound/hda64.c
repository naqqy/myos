#include "hda64.h"

#include "../hw/pci64.h"
#include "../mm/paging64.h"
#include "../mm/pmm64.h"
#include "../mm/vmem64.h"
#include "../arch/x86_64/timer64.h"
#include "../proc/process64.h"

/*
 * Intel HD Audio driver. See hda64.h.
 *
 * Talking to a codec: the driver puts 32-bit commands ("verbs") into a
 * ring in memory (the CORB) and the controller puts the answers into
 * another (the RIRB). A few controllers lack the rings; for those there
 * is a slower one-command-at-a-time register pair.
 *
 * Playing: a stream is described by a list of memory blocks (the buffer
 * descriptor list). The controller plays through the list and starts
 * again at the top, forever. The mixer (sound64.c) keeps writing new
 * sound just ahead of the point being played.
 *
 * All memory handed to the controller comes from the physical frame
 * allocator, which is identity mapped and below 4 GiB.
 */

#define MMIO_MAP_SIZE    0x4000u

/* Controller registers */
#define REG_GCAP         0x00u
#define REG_GCTL         0x08u
#define REG_WAKEEN       0x0Cu
#define REG_STATESTS     0x0Eu
#define REG_INTCTL       0x20u
#define REG_CORBLBASE    0x40u
#define REG_CORBUBASE    0x44u
#define REG_CORBWP       0x48u
#define REG_CORBRP       0x4Au
#define REG_CORBCTL      0x4Cu
#define REG_CORBSIZE     0x4Eu
#define REG_RIRBLBASE    0x50u
#define REG_RIRBUBASE    0x54u
#define REG_RIRBWP       0x58u
#define REG_RINTCNT      0x5Au
#define REG_RIRBCTL      0x5Cu
#define REG_RIRBSTS      0x5Du
#define REG_RIRBSIZE     0x5Eu
#define REG_ICW          0x60u
#define REG_IRR          0x64u
#define REG_ICS          0x68u
#define REG_STREAMS      0x80u

/* Stream registers, relative to the stream's block */
#define SD_CTL           0x00u
#define SD_CTL_TAG       0x02u      /* third byte of the control register */
#define SD_STS           0x03u
#define SD_LPIB          0x04u
#define SD_CBL           0x08u
#define SD_LVI           0x0Cu
#define SD_FMT           0x12u
#define SD_BDPL          0x18u
#define SD_BDPU          0x1Cu

#define GCTL_RESET       0x00000001u
#define SD_CTL_RESET     0x01u
#define SD_CTL_RUN       0x02u

/* Verbs */
#define VERB_GET_PARAM       0xF00u
#define VERB_GET_CONN_SELECT 0xF01u
#define VERB_SET_CONN_SELECT 0x701u
#define VERB_GET_CONN_LIST   0xF02u
#define VERB_SET_POWER       0x705u
#define VERB_SET_STREAM      0x706u
#define VERB_SET_PIN_CTL     0x707u
#define VERB_SET_EAPD        0x70Cu
#define VERB_GET_CONFIG      0xF1Cu
#define VERB4_SET_FORMAT     0x2u
#define VERB4_SET_AMP        0x3u

/* Parameters for VERB_GET_PARAM */
#define PARAM_VENDOR         0x00u
#define PARAM_NODE_COUNT     0x04u
#define PARAM_GROUP_TYPE     0x05u
#define PARAM_WIDGET_CAPS    0x09u
#define PARAM_PIN_CAPS       0x0Cu
#define PARAM_IN_AMP_CAPS    0x0Du
#define PARAM_CONN_LENGTH    0x0Eu
#define PARAM_OUT_AMP_CAPS   0x12u

#define WIDGET_DAC           0u
#define WIDGET_MIXER         2u
#define WIDGET_SELECTOR      3u
#define WIDGET_PIN           4u

#define WCAP_IN_AMP          0x00000002u
#define WCAP_OUT_AMP         0x00000004u
#define WCAP_AMP_OVERRIDE    0x00000008u
#define WCAP_CONN_LIST       0x00000100u
#define WCAP_DIGITAL         0x00000200u

#define PINCAP_OUTPUT        0x00000010u
#define PINCAP_EAPD          0x00010000u

/* 48 kHz, 16 bits, 2 channels */
#define STREAM_FORMAT        0x0011u
#define STREAM_TAG           1u

#define RING_PAGES           16u
#define MAX_WIDGETS          128u
#define MAX_CONNS            16u
#define MAX_PATH             8u

typedef struct bdl_entry {
    nq_u64 address;
    nq_u32 length;
    nq_u32 flags;
} __attribute__((packed)) bdl_entry_t;

typedef struct widget {
    nq_u8 type;
    nq_u8 conn_count;
    nq_u8 visiting;
    nq_u8 pad;
    nq_u32 caps;
    nq_u8 conns[MAX_CONNS];
} widget_t;

static int g_present = 0;
static nq_u64 g_mmio = 0;
static nq_u32 g_stream = 0;         /* register block of the output stream */
static char g_name[48] = "none";
static const char* g_why = "no HD Audio controller on the PCI bus";

static nq_u32* g_corb = 0;
static nq_u64* g_rirb = 0;
static nq_u32 g_corb_size = 0;
static nq_u32 g_rirb_size = 0;
static nq_u32 g_rirb_read = 0;
static int g_use_rings = 0;

static nq_u8* g_ring[RING_PAGES];
static nq_u32 g_ring_bytes = 0;

static nq_u32 g_codec = 0;          /* address of the codec in use */
static nq_u32 g_codec_id = 0;
static nq_u32 g_outputs = 0;

static nq_u32 g_afg = 0;            /* the codec's audio function group */
static nq_u32 g_first_widget = 0;
static nq_u32 g_widget_count = 0;
static widget_t g_widgets[MAX_WIDGETS];
static nq_u32 g_default_out_amp = 0;
static nq_u32 g_default_in_amp = 0;

/* Some controllers do not report the play position; then time is used. */
static int g_position_by_clock = 0;
static nq_u64 g_start_tick = 0;

static nq_u8 r8(nq_u32 reg) { return *(volatile nq_u8*)(g_mmio + reg); }
static nq_u16 r16(nq_u32 reg) { return *(volatile nq_u16*)(g_mmio + reg); }
static nq_u32 r32(nq_u32 reg) { return *(volatile nq_u32*)(g_mmio + reg); }
static void w8(nq_u32 reg, nq_u8 v) { *(volatile nq_u8*)(g_mmio + reg) = v; }
static void w16(nq_u32 reg, nq_u16 v) { *(volatile nq_u16*)(g_mmio + reg) = v; }
static void w32(nq_u32 reg, nq_u32 v) { *(volatile nq_u32*)(g_mmio + reg) = v; }

static void sleep_ms(nq_u32 ms) {
    nq_u64 hz = timer64_get_frequency();
    nq_u64 ticks = ((nq_u64)ms * (hz ? hz : 1000u) + 999u) / 1000u;

    process64_sleep_ticks(ticks ? ticks : 1u);
}

static void* alloc_zeroed_page(void) {
    nq_u8* page = (nq_u8*)pmm64_alloc_frame();

    if (page) {
        for (nq_u32 i = 0; i < 4096u; i++) {
            page[i] = 0;
        }
    }

    return page;
}

static void set_name(const char* text) {
    nq_u32 i = 0;

    for (; text[i] && i + 1u < sizeof(g_name); i++) {
        g_name[i] = text[i];
    }

    g_name[i] = 0;
}

/* -------------------------------------------------------------------------- */
/* Commands to the codec                                                       */
/* -------------------------------------------------------------------------- */

/* The one-at-a-time way. */
static int command_direct(nq_u32 verb, nq_u32* out) {
    for (nq_u32 i = 0; i < 20u && (r16(REG_ICS) & 1u); i++) {
        sleep_ms(1);
    }

    w16(REG_ICS, 0x0002u);          /* clear "answer valid" */
    w32(REG_ICW, verb);
    w16(REG_ICS, 0x0001u);          /* go */

    for (nq_u32 i = 0; i < 30u; i++) {
        nq_u16 status = r16(REG_ICS);

        if (!(status & 1u) && (status & 2u)) {
            *out = r32(REG_IRR);
            return 1;
        }

        sleep_ms(1);
    }

    return 0;
}

/* Through the command and answer rings. */
static int command_rings(nq_u32 verb, nq_u32* out) {
    nq_u32 write = ((nq_u32)(r16(REG_CORBWP) & 0xFFu) + 1u) % g_corb_size;

    /*
     * The controller counts answers and stops taking commands when the
     * count reaches the limit set in rings_start() (one), until the
     * "answer arrived" flag is cleared. So clear it before every command.
     * (The flag is only a status bit here: interrupts themselves are off.)
     */
    w8(REG_RIRBSTS, 0x05u);

    g_corb[write] = verb;
    w16(REG_CORBWP, (nq_u16)write);

    for (nq_u32 waited = 0; waited < 60u; waited++) {
        /*
         * Usually the answer is there within microseconds: look a few
         * times before giving the processor away.
         */
        for (nq_u32 spin = 0; spin < 2000u; spin++) {
            nq_u32 at = (nq_u32)(r16(REG_RIRBWP) & 0xFFu) % g_rirb_size;

            while (g_rirb_read != at) {
                g_rirb_read = (g_rirb_read + 1u) % g_rirb_size;

                nq_u64 entry = g_rirb[g_rirb_read];

                /* bit 4 of the upper half: sent by the codec unasked */
                if (!((entry >> 32) & 0x10u)) {
                    *out = (nq_u32)entry;
                    w8(REG_RIRBSTS, 0x05u);
                    return 1;
                }

                w8(REG_RIRBSTS, 0x05u);
            }

            __asm__ volatile ("pause");
        }

        sleep_ms(1);
    }

    return 0;
}

static nq_u32 command(nq_u32 node, nq_u32 verb, nq_u32 payload) {
    nq_u32 word = (g_codec << 28) | (node << 20) | (verb << 8) | (payload & 0xFFu);
    nq_u32 answer = 0;

    if (g_use_rings) {
        if (command_rings(word, &answer)) {
            return answer;
        }

        return 0;
    }

    command_direct(word, &answer);

    return answer;
}

/* Verbs with a 4-bit number and 16 bits of data. */
static void command4(nq_u32 node, nq_u32 verb, nq_u32 payload) {
    nq_u32 word = (g_codec << 28) | (node << 20) | (verb << 16) | (payload & 0xFFFFu);
    nq_u32 answer = 0;

    if (g_use_rings) {
        command_rings(word, &answer);
    } else {
        command_direct(word, &answer);
    }
}

static nq_u32 param(nq_u32 node, nq_u32 id) {
    return command(node, VERB_GET_PARAM, id);
}

/* -------------------------------------------------------------------------- */
/* Controller                                                                  */
/* -------------------------------------------------------------------------- */

static int wait16(nq_u32 reg, nq_u16 mask, nq_u16 want, nq_u32 ms) {
    for (nq_u32 waited = 0; ; waited++) {
        if ((r16(reg) & mask) == want) {
            return 1;
        }

        if (waited >= ms) {
            return 0;
        }

        sleep_ms(1);
    }
}

static int controller_reset(void) {
    /* Nothing may be running across a reset. */
    w8(REG_CORBCTL, 0);
    w8(REG_RIRBCTL, 0);
    w32(REG_INTCTL, 0);

    w32(REG_GCTL, r32(REG_GCTL) & ~GCTL_RESET);

    for (nq_u32 i = 0; i < 100u && (r32(REG_GCTL) & GCTL_RESET); i++) {
        sleep_ms(1);
    }

    sleep_ms(2);
    w32(REG_GCTL, r32(REG_GCTL) | GCTL_RESET);

    for (nq_u32 i = 0; i < 100u && !(r32(REG_GCTL) & GCTL_RESET); i++) {
        sleep_ms(1);
    }

    if (!(r32(REG_GCTL) & GCTL_RESET)) {
        return 0;
    }

    /* Codecs announce themselves within a millisecond; be generous. */
    sleep_ms(10);
    w16(REG_WAKEEN, 0);

    return 1;
}

/* Picks the largest ring size a size register offers; returns the entry count. */
static nq_u32 ring_size(nq_u32 reg) {
    nq_u8 value = r8(reg);
    nq_u8 choice = 0;
    nq_u32 entries = 2;

    if (value & 0x40u) {
        choice = 2;
        entries = 256;
    } else if (value & 0x20u) {
        choice = 1;
        entries = 16;
    }

    w8(reg, (nq_u8)((value & 0xFCu) | choice));

    return entries;
}

static void rings_start(void) {
    nq_u8* page = (nq_u8*)alloc_zeroed_page();

    g_use_rings = 0;

    if (!page) {
        return;
    }

    /* 256 commands of 4 bytes, then 256 answers of 8 bytes: one page. */
    g_corb = (nq_u32*)page;
    g_rirb = (nq_u64*)(page + 1024u);

    w8(REG_CORBCTL, 0);
    w8(REG_RIRBCTL, 0);

    g_corb_size = ring_size(REG_CORBSIZE);
    g_rirb_size = ring_size(REG_RIRBSIZE);

    w32(REG_CORBLBASE, (nq_u32)(nq_u64)g_corb);
    w32(REG_CORBUBASE, (nq_u32)((nq_u64)g_corb >> 32));
    w32(REG_RIRBLBASE, (nq_u32)(nq_u64)g_rirb);
    w32(REG_RIRBUBASE, (nq_u32)((nq_u64)g_rirb >> 32));

    /* Reset the read pointer of the command ring (set the bit, then clear it). */
    w16(REG_CORBRP, 0x8000u);
    wait16(REG_CORBRP, 0x8000u, 0x8000u, 5);
    w16(REG_CORBRP, 0);
    wait16(REG_CORBRP, 0x8000u, 0, 5);
    w16(REG_CORBWP, 0);

    w16(REG_RIRBWP, 0x8000u);
    w16(REG_RINTCNT, 1);
    g_rirb_read = 0;

    /*
     * Command ring running; answer ring running with its "answer arrived"
     * flag enabled (see command_rings()).
     */
    w8(REG_CORBCTL, 0x02u);
    w8(REG_RIRBCTL, 0x03u);

    g_use_rings = 1;
}

/* -------------------------------------------------------------------------- */
/* The codec's widgets                                                         */
/* -------------------------------------------------------------------------- */

static widget_t* widget(nq_u32 node) {
    if (node < g_first_widget || node >= g_first_widget + g_widget_count) {
        return 0;
    }

    return &g_widgets[node - g_first_widget];
}

static void read_connections(nq_u32 node, widget_t* w) {
    w->conn_count = 0;

    if (!(w->caps & WCAP_CONN_LIST)) {
        return;
    }

    nq_u32 info = param(node, PARAM_CONN_LENGTH);
    nq_u32 total = info & 0x7Fu;
    int wide = (info & 0x80u) != 0;
    nq_u32 per_answer = wide ? 2u : 4u;
    nq_u32 previous = 0;

    for (nq_u32 at = 0; at < total; at += per_answer) {
        nq_u32 answer = command(node, VERB_GET_CONN_LIST, at);

        for (nq_u32 k = 0; k < per_answer && at + k < total; k++) {
            nq_u32 entry = wide ? ((answer >> (16u * k)) & 0xFFFFu)
                                : ((answer >> (8u * k)) & 0xFFu);
            nq_u32 range_bit = wide ? 0x8000u : 0x80u;
            nq_u32 value = entry & (range_bit - 1u);

            /*
             * An entry with the top bit set means "everything from the
             * previous entry up to this one".
             */
            if ((entry & range_bit) && previous && value > previous) {
                for (nq_u32 n = previous + 1u; n <= value && w->conn_count < MAX_CONNS; n++) {
                    w->conns[w->conn_count++] = (nq_u8)n;
                }
            } else if (w->conn_count < MAX_CONNS) {
                w->conns[w->conn_count++] = (nq_u8)value;
            }

            previous = value;
        }
    }
}

/* Reads the audio function group of the current codec. Returns 1 if it has one. */
static int codec_read(void) {
    nq_u32 root = param(0, PARAM_NODE_COUNT);
    nq_u32 first_group = (root >> 16) & 0xFFu;
    nq_u32 group_count = root & 0xFFu;

    g_afg = 0;

    for (nq_u32 g = first_group; g < first_group + group_count; g++) {
        if ((param(g, PARAM_GROUP_TYPE) & 0xFFu) == 1u) {
            g_afg = g;
            break;
        }
    }

    if (!g_afg) {
        return 0;
    }

    command(g_afg, VERB_SET_POWER, 0);
    sleep_ms(10);

    nq_u32 nodes = param(g_afg, PARAM_NODE_COUNT);

    g_first_widget = (nodes >> 16) & 0xFFu;
    g_widget_count = nodes & 0xFFu;

    if (g_widget_count > MAX_WIDGETS) {
        g_widget_count = MAX_WIDGETS;
    }

    g_default_out_amp = param(g_afg, PARAM_OUT_AMP_CAPS);
    g_default_in_amp = param(g_afg, PARAM_IN_AMP_CAPS);

    for (nq_u32 i = 0; i < g_widget_count; i++) {
        nq_u32 node = g_first_widget + i;
        widget_t* w = &g_widgets[i];

        w->caps = param(node, PARAM_WIDGET_CAPS);
        w->type = (nq_u8)((w->caps >> 20) & 0xFu);
        w->visiting = 0;

        read_connections(node, w);
    }

    return 1;
}

/*
 * Looks for a way from `node` back to a converter (DAC), following the
 * connection lists. On success path[0..] holds the nodes from `node` to
 * the DAC, and via[k] the input of path[k] that leads to path[k + 1].
 */
static nq_u32 find_path(nq_u32 node, nq_u8* path, nq_u8* via, nq_u32 depth) {
    widget_t* w = widget(node);

    if (!w || w->visiting || depth >= MAX_PATH || (w->caps & WCAP_DIGITAL)) {
        return 0;
    }

    path[depth] = (nq_u8)node;

    if (w->type == WIDGET_DAC) {
        return depth + 1u;
    }

    if (w->type != WIDGET_PIN && w->type != WIDGET_MIXER && w->type != WIDGET_SELECTOR) {
        return 0;
    }

    /* a pin in the middle of a path would be an input socket */
    if (w->type == WIDGET_PIN && depth > 0) {
        return 0;
    }

    w->visiting = 1;

    nq_u32 found = 0;

    for (nq_u32 c = 0; c < w->conn_count && !found; c++) {
        via[depth] = (nq_u8)c;
        found = find_path(w->conns[c], path, via, depth + 1u);
    }

    w->visiting = 0;

    return found;
}

/* The gain setting that means "no change in level" for an amplifier. */
static nq_u32 unity_gain(nq_u32 amp_caps) {
    nq_u32 offset = amp_caps & 0x7Fu;
    nq_u32 steps = (amp_caps >> 8) & 0x7Fu;

    return offset ? offset : steps;
}

static void open_node(nq_u32 node, nq_u32 input, int has_input) {
    widget_t* w = widget(node);

    if (!w) {
        return;
    }

    command(node, VERB_SET_POWER, 0);

    if (w->caps & WCAP_OUT_AMP) {
        nq_u32 caps = (w->caps & WCAP_AMP_OVERRIDE) ? param(node, PARAM_OUT_AMP_CAPS)
                                                    : g_default_out_amp;

        /* output amplifier, left and right, not muted */
        command4(node, VERB4_SET_AMP, 0xB000u | unity_gain(caps));
    }

    if (has_input && (w->caps & WCAP_IN_AMP)) {
        nq_u32 caps = (w->caps & WCAP_AMP_OVERRIDE) ? param(node, PARAM_IN_AMP_CAPS)
                                                    : g_default_in_amp;

        /* input amplifier for the input in use */
        command4(node, VERB4_SET_AMP, 0x7000u | ((input & 0xFu) << 8) | unity_gain(caps));
    }

    /* mixers add all their inputs together; the others choose one */
    if (has_input && w->type != WIDGET_MIXER && w->conn_count > 1u) {
        command(node, VERB_SET_CONN_SELECT, input);
    }
}

/*
 * Routes the stream to every analogue output socket of the codec.
 * Returns how many sockets were set up.
 */
static nq_u32 codec_route(void) {
    nq_u32 outputs = 0;

    for (nq_u32 i = 0; i < g_widget_count; i++) {
        nq_u32 node = g_first_widget + i;
        widget_t* w = &g_widgets[i];

        if (w->type != WIDGET_PIN || (w->caps & WCAP_DIGITAL)) {
            continue;
        }

        nq_u32 pin_caps = param(node, PARAM_PIN_CAPS);

        if (!(pin_caps & PINCAP_OUTPUT)) {
            continue;
        }

        /*
         * The board's own description of the socket: skip those that are
         * not wired to anything, and those meant for input (microphone,
         * line in), which can often drive output too but should not.
         */
        nq_u32 config = command(node, VERB_GET_CONFIG, 0);
        nq_u32 connected = (config >> 30) & 3u;
        nq_u32 device = (config >> 20) & 0xFu;

        if (connected == 1u || device > 2u) {
            continue;
        }

        nq_u8 path[MAX_PATH];
        nq_u8 via[MAX_PATH];
        nq_u32 length = find_path(node, path, via, 0);

        if (!length) {
            continue;
        }

        for (nq_u32 k = 0; k < length; k++) {
            open_node(path[k], via[k], k + 1u < length);
        }

        /* the converter at the end of the path plays our stream */
        nq_u32 dac = path[length - 1u];

        command4(dac, VERB4_SET_FORMAT, STREAM_FORMAT);
        command(dac, VERB_SET_STREAM, (STREAM_TAG << 4) | 0u);

        /* socket: output on, headphone drive on, external amplifier on */
        command(node, VERB_SET_PIN_CTL, 0xC0u);

        if (pin_caps & PINCAP_EAPD) {
            command(node, VERB_SET_EAPD, 0x02u);
        }

        outputs++;
    }

    return outputs;
}

/* -------------------------------------------------------------------------- */
/* The stream                                                                  */
/* -------------------------------------------------------------------------- */

static int stream_start(void) {
    nq_u32 gcap = r16(REG_GCAP);
    nq_u32 inputs = (gcap >> 8) & 0xFu;
    nq_u32 outputs = (gcap >> 12) & 0xFu;

    if (!outputs) {
        return 0;
    }

    /* Output streams come after the input streams. */
    g_stream = REG_STREAMS + 0x20u * inputs;

    bdl_entry_t* list = (bdl_entry_t*)alloc_zeroed_page();

    if (!list) {
        return 0;
    }

    for (nq_u32 i = 0; i < RING_PAGES; i++) {
        g_ring[i] = (nq_u8*)alloc_zeroed_page();

        if (!g_ring[i]) {
            return 0;
        }

        list[i].address = (nq_u64)g_ring[i];
        list[i].length = 4096u;
        list[i].flags = 0;
    }

    g_ring_bytes = RING_PAGES * 4096u;

    /* Stop and reset the stream. */
    w8(g_stream + SD_CTL, 0);
    sleep_ms(1);
    w8(g_stream + SD_CTL, SD_CTL_RESET);

    for (nq_u32 i = 0; i < 20u && !(r8(g_stream + SD_CTL) & SD_CTL_RESET); i++) {
        sleep_ms(1);
    }

    w8(g_stream + SD_CTL, 0);

    for (nq_u32 i = 0; i < 20u && (r8(g_stream + SD_CTL) & SD_CTL_RESET); i++) {
        sleep_ms(1);
    }

    w8(g_stream + SD_STS, 0x1Cu);                   /* clear old status */
    w32(g_stream + SD_BDPL, (nq_u32)(nq_u64)list);
    w32(g_stream + SD_BDPU, (nq_u32)((nq_u64)list >> 32));
    w32(g_stream + SD_CBL, g_ring_bytes);
    w16(g_stream + SD_LVI, (nq_u16)(RING_PAGES - 1u));
    w16(g_stream + SD_FMT, STREAM_FORMAT);
    w8(g_stream + SD_CTL_TAG, (nq_u8)(STREAM_TAG << 4));

    w8(g_stream + SD_CTL, SD_CTL_RUN);
    g_start_tick = timer64_get_ticks();

    /*
     * The position should now be moving. If it is not, fall back on the
     * clock: the stream plays at a known rate.
     */
    nq_u32 before = r32(g_stream + SD_LPIB);

    g_position_by_clock = 1;

    for (nq_u32 i = 0; i < 60u; i++) {
        sleep_ms(1);

        if (r32(g_stream + SD_LPIB) != before) {
            g_position_by_clock = 0;
            break;
        }
    }

    return (r8(g_stream + SD_CTL) & SD_CTL_RUN) != 0;
}

nq_u32 hda64_position(void) {
    if (!g_present && !g_ring_bytes) {
        return 0;
    }

    if (g_position_by_clock) {
        nq_u64 hz = timer64_get_frequency();
        nq_u64 ticks = timer64_get_ticks() - g_start_tick;
        nq_u64 bytes = (ticks * HDA64_RATE / (hz ? hz : 1000u)) * HDA64_FRAME_BYTES;

        return (nq_u32)(bytes % g_ring_bytes);
    }

    nq_u32 position = r32(g_stream + SD_LPIB);

    if (position >= g_ring_bytes) {
        position = 0;
    }

    return position & ~(HDA64_FRAME_BYTES - 1u);
}

/* -------------------------------------------------------------------------- */
/* Start-up                                                                    */
/* -------------------------------------------------------------------------- */

static int start_controller(const pci64_device_t* pci) {
    int is_mmio = 0;
    int is_64 = 0;
    nq_u64 bar = pci64_read_bar(*pci, 0, &is_mmio, &is_64);

    if (!bar || !is_mmio) {
        g_why = "the controller has no memory registers";
        return 0;
    }

    /* Memory access and bus mastering on; interrupt line off (we poll). */
    nq_u16 pci_command = pci64_config_read16(pci->bus, pci->device, pci->function, 0x04);

    pci_command |= 0x0006u | 0x0400u;
    pci64_config_write16(pci->bus, pci->device, pci->function, 0x04, pci_command);

    /*
     * Intel controllers: select traffic class 0 (bits 2:0 of the TCSEL
     * register at 0x44), or the stream can stutter.
     */
    if (pci->vendor_id == 0x8086u) {
        nq_u16 tcsel = pci64_config_read16(pci->bus, pci->device, pci->function, 0x44);

        pci64_config_write16(pci->bus, pci->device, pci->function, 0x44, (nq_u16)(tcsel & ~7u));
    }

    if (!paging64_map_direct_range(bar, MMIO_MAP_SIZE)) {
        return 0;
    }

    g_mmio = vmem64_phys_to_direct(bar);

    if (r16(REG_GCAP) == 0xFFFFu || !controller_reset()) {
        g_why = "the controller did not come out of reset";
        return 0;
    }

    nq_u16 codecs = r16(REG_STATESTS);

    if (!codecs) {
        g_why = "the controller reports no codec chip";
        return 0;
    }

    g_why = "the codec did not answer";

    rings_start();

    for (nq_u32 address = 0; address < 15u; address++) {
        if (!(codecs & (1u << address))) {
            continue;
        }

        g_codec = address;

        nq_u32 id = param(0, PARAM_VENDOR);

        /* No answer through the rings: try the other way once. */
        if ((id == 0 || id == 0xFFFFFFFFu) && g_use_rings) {
            w8(REG_CORBCTL, 0);
            w8(REG_RIRBCTL, 0);
            g_use_rings = 0;
            id = param(0, PARAM_VENDOR);
        }

        if (id == 0 || id == 0xFFFFFFFFu) {
            continue;
        }

        if (!codec_read()) {
            g_why = "the codec has no audio function";
            continue;
        }

        nq_u32 outputs = codec_route();

        if (!outputs) {
            /* for example a graphics card's HDMI codec */
            g_why = "the codec has no analogue output (HDMI only?)";
            continue;
        }

        g_codec_id = id;
        g_outputs = outputs;

        if (!stream_start()) {
            g_why = "the output stream would not start";
            return 0;
        }

        return 1;
    }

    return 0;
}

int hda64_init(void) {
    if (g_present) {
        return 1;
    }

    /*
     * PCI class 04 (multimedia), subclass 03 (HD Audio). A machine often
     * has two: the board's own, and one on the graphics card for HDMI.
     */
    for (nq_u32 n = 0; n < 8u; n++) {
        pci64_device_t pci;

        if (!pci64_find_class_nth(0x04u, 0x03u, 0x00u, n, &pci)) {
            break;
        }

        if (start_controller(&pci)) {
            switch (g_codec_id >> 16) {
                case 0x10ECu: set_name("HD Audio (Realtek codec)"); break;
                case 0x1AF4u: set_name("HD Audio (QEMU codec)"); break;
                case 0x8086u: set_name("HD Audio (Intel codec)"); break;
                case 0x14F1u: set_name("HD Audio (Conexant codec)"); break;
                case 0x1106u: set_name("HD Audio (VIA codec)"); break;
                case 0x11D4u: set_name("HD Audio (Analog Devices codec)"); break;
                case 0x1013u: set_name("HD Audio (Cirrus Logic codec)"); break;
                case 0x111Du: set_name("HD Audio (IDT codec)"); break;
                default:      set_name("HD Audio"); break;
            }

            g_present = 1;
            return 1;
        }
    }

    return 0;
}

const char* hda64_why(void) {
    return g_why;
}

int hda64_present(void) {
    return g_present;
}

const char* hda64_name(void) {
    return g_name;
}

nq_u32 hda64_codec_id(void) {
    return g_codec_id;
}

nq_u32 hda64_output_count(void) {
    return g_outputs;
}

nq_u32 hda64_ring_bytes(void) {
    return g_present ? g_ring_bytes : 0;
}

nq_u8* hda64_ring_page(nq_u32 index) {
    return index < RING_PAGES ? g_ring[index] : 0;
}
