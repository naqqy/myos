#include "irq64.h"
#include "io64.h"
#include "idt64.h"
#include "timer64.h"
#include "../../k64_console.h"
#include "../../input/kinput64.h"
#include "../../input/mouse64.h"
#include "../../proc/process64.h"
#include "../../debug/bootlog64.h"

#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

#define PIC_EOI      0x20

#define ICW1_INIT    0x10
#define ICW1_ICW4    0x01
#define ICW4_8086    0x01

static int g_shift_down = 0;
static int g_ctrl_down = 0;
static int g_alt_down = 0;

/*
 * Set after the 0xE0 prefix byte: the next scancode is an "extended" key
 * (arrows, right Ctrl, ...).
 */
static int g_extended = 0;

static const char keymap[128] = {
    [0x01]=27,
    [0x02]='1',[0x03]='2',[0x04]='3',[0x05]='4',[0x06]='5',[0x07]='6',
    [0x08]='7',[0x09]='8',[0x0A]='9',[0x0B]='0',[0x0C]='-',[0x0D]='=',
    [0x0E]='\b',
    [0x0F]='\t',
    [0x10]='q',[0x11]='w',[0x12]='e',[0x13]='r',[0x14]='t',[0x15]='y',
    [0x16]='u',[0x17]='i',[0x18]='o',[0x19]='p',[0x1A]='[',[0x1B]=']',
    [0x1C]='\n',
    [0x1E]='a',[0x1F]='s',[0x20]='d',[0x21]='f',[0x22]='g',[0x23]='h',
    [0x24]='j',[0x25]='k',[0x26]='l',[0x27]=';',[0x28]='\'',[0x29]='`',
    [0x2B]='\\',
    [0x2C]='z',[0x2D]='x',[0x2E]='c',[0x2F]='v',[0x30]='b',[0x31]='n',
    [0x32]='m',[0x33]=',',[0x34]='.',[0x35]='/',
    [0x39]=' '
};

static const char shift_keymap[128] = {
    [0x01]=27,
    [0x02]='!',[0x03]='@',[0x04]='#',[0x05]='$',[0x06]='%',[0x07]='^',
    [0x08]='&',[0x09]='*',[0x0A]='(',[0x0B]=')',[0x0C]='_',[0x0D]='+',
    [0x0E]='\b',
    [0x0F]='\t',
    [0x10]='Q',[0x11]='W',[0x12]='E',[0x13]='R',[0x14]='T',[0x15]='Y',
    [0x16]='U',[0x17]='I',[0x18]='O',[0x19]='P',[0x1A]='{',[0x1B]='}',
    [0x1C]='\n',
    [0x1E]='A',[0x1F]='S',[0x20]='D',[0x21]='F',[0x22]='G',[0x23]='H',
    [0x24]='J',[0x25]='K',[0x26]='L',[0x27]=':',[0x28]='"',[0x29]='~',
    [0x2B]='|',
    [0x2C]='Z',[0x2D]='X',[0x2E]='C',[0x2F]='V',[0x30]='B',[0x31]='N',
    [0x32]='M',[0x33]='<',[0x34]='>',[0x35]='?',
    [0x39]=' '
};

static void pic_send_eoi(nq_u8 irq) {
    if (irq >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);
    }

    outb(PIC1_COMMAND, PIC_EOI);
}

static void pic_remap(void) {
    nq_u8 mask1 = inb(PIC1_DATA);
    nq_u8 mask2 = inb(PIC2_DATA);

    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();

    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();

    outb(PIC1_DATA, 0x20);
    io_wait();

    outb(PIC2_DATA, 0x28);
    io_wait();

    outb(PIC1_DATA, 0x04);
    io_wait();

    outb(PIC2_DATA, 0x02);
    io_wait();

    outb(PIC1_DATA, ICW4_8086);
    io_wait();

    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
}

static void pic_mask_all(void) {
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

static void pic_unmask_irq(nq_u8 irq) {
    nq_u16 port;
    nq_u8 value;

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }

    value = inb(port);
    value &= (nq_u8)~(1u << irq);
    outb(port, value);
}

static void keyboard_handle_scancode(nq_u8 scancode) {
    if (scancode == 0xE0) {
        g_extended = 1;
        return;
    }

    int extended = g_extended;

    g_extended = 0;

    /*
     * Ctrl (left 0x1D, right E0 0x1D).
     */
    if (scancode == 0x1D) {
        g_ctrl_down = 1;
        return;
    }

    if (scancode == 0x9D) {
        g_ctrl_down = 0;
        return;
    }

    /*
     * Alt (left 0x38, right E0 0x38).
     */
    if (scancode == 0x38) {
        g_alt_down = 1;
        return;
    }

    if (scancode == 0xB8) {
        g_alt_down = 0;
        return;
    }

    if (extended) {
        /*
         * Navigation keys are delivered as codes above ASCII (KINPUT64_KEY_*).
         * Extended shifts (E0 2A / E0 AA) are "fake" shifts sent around
         * these keys and must not change the shift state.
         */
        char special = 0;

        switch (scancode) {
            case 0x48: special = (char)KINPUT64_KEY_UP; break;
            case 0x50: special = (char)KINPUT64_KEY_DOWN; break;
            case 0x4B: special = (char)KINPUT64_KEY_LEFT; break;
            case 0x4D: special = (char)KINPUT64_KEY_RIGHT; break;
            case 0x49: special = (char)KINPUT64_KEY_PAGE_UP; break;
            case 0x51: special = (char)KINPUT64_KEY_PAGE_DOWN; break;
            case 0x47: special = (char)KINPUT64_KEY_HOME; break;
            case 0x4F: special = (char)KINPUT64_KEY_END; break;
            case 0x53: special = (char)KINPUT64_KEY_DELETE; break;
            default: break;
        }

        if (special) {
            kinput64_push_char(special);
        }

        return;
    }

    if (scancode == 0x2A || scancode == 0x36) {
        g_shift_down = 1;
        return;
    }

    if (scancode == 0xAA || scancode == 0xB6) {
        g_shift_down = 0;
        return;
    }

    /*
     * Ignore key releases.
     */
    if (scancode & 0x80) {
        return;
    }

    if (g_alt_down && scancode == 0x0F) {
        kinput64_push_char((char)KINPUT64_KEY_ALT_TAB);
        return;
    }

    char c = g_shift_down ? shift_keymap[scancode] : keymap[scancode];

    if (c == 0) {
        return;
    }

    /*
     * Ctrl+letter produces the classic control code (Ctrl+A = 1 ...
     * Ctrl+Z = 26), which is what the desktop hotkeys Ctrl+T and Ctrl+Q
     * look for.
     */
    if (g_ctrl_down) {
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 1);
        } else if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 1);
        }
    }

    /*
     * Do not handle rendering here.
     * Queue all real characters, including backspace.
     */
    kinput64_push_char(c);
}

#define PS2_DATA_PORT   0x60
#define PS2_STATUS_PORT 0x64
#define PS2_STATUS_OUTPUT_FULL 0x01
#define PS2_STATUS_AUX_DATA    0x20

/*
 * Reads every byte waiting in the PS/2 controller.
 *
 * The controller raises IRQ1 once per byte and will not raise it again until
 * that byte has been read. If a byte arrives while the IRQ cannot be
 * delivered (the key released after the UEFI loader's "press any key", while
 * the kernel is still starting and the PIC is being re-initialised), the
 * interrupt is lost, the byte stays in the buffer and the keyboard is dead
 * for the rest of the session. Reading the buffer re-arms it.
 *
 * Must be called with interrupts disabled.
 */
static void keyboard_drain(int handle_scancodes) {
    for (int i = 0; i < 32; i++) {
        nq_u8 status = inb(PS2_STATUS_PORT);

        if (!(status & PS2_STATUS_OUTPUT_FULL)) {
            break;
        }

        nq_u8 data = inb(PS2_DATA_PORT);

        if (!handle_scancodes) {
            continue;
        }

        /*
         * Keyboard and mouse share the controller; the AUX bit says which
         * device this byte came from.
         */
        if (status & PS2_STATUS_AUX_DATA) {
            mouse64_handle_byte(data);
        } else {
            keyboard_handle_scancode(data);
        }
    }
}

void irq_handler64(irq_frame64_t* frame) {
    if (!frame) {
        return;
    }

    nq_u64 irq = frame->irq;

    if (irq == 0) {
        timer64_on_irq();

        /*
         * Safety net: if a keyboard interrupt was ever missed, pick the
         * byte up here so input can never stay stuck. Cheap enough to do
         * a few times per second.
         */
        if ((timer64_get_ticks() & 0x3Full) == 0) {
            keyboard_drain(1);
        }

        pic_send_eoi((nq_u8)irq);

        /*
        * Kernel task preemption.
        * This may context-switch before returning from the IRQ.
        */
        process64_on_timer_tick();
        return;
    } else if (irq == 12) {
        /*
         * PS/2 mouse. keyboard_drain() routes each byte by its AUX flag, so
         * the same reader serves both interrupts.
         */
        keyboard_drain(1);
    } else if (irq == 1) {
        /*
         * Only read when the controller really has a byte; the timer safety
         * net may already have taken it, and a blind read would repeat the
         * previous scancode.
         */
        keyboard_drain(1);
    }

    pic_send_eoi((nq_u8)irq);
}

void irq64_init(nq_u32 timer_hz) {
    __asm__ volatile ("cli");

    idt64_install_irq_gates();

    pic_remap();
    pic_mask_all();

    timer64_init(timer_hz);

    /*
     * PS/2 mouse: set it up while everything is still masked, then open
     * IRQ12 and the cascade line (IRQ2) that the slave PIC reports through.
     */
    if (mouse64_init()) {
        bootlog64_print("IRQ64: PS/2 mouse enabled.\n");
        pic_unmask_irq(2);
        pic_unmask_irq(12);
    } else {
        bootlog64_print("IRQ64: no PS/2 mouse.\n");
    }

    pic_unmask_irq(0);
    pic_unmask_irq(1);

    /*
     * Throw away anything the firmware left in the keyboard controller
     * (typically the release of the key pressed at the loader prompt) so the
     * first real keypress produces a fresh IRQ1.
     */
    keyboard_drain(0);
    g_shift_down = 0;
    g_ctrl_down = 0;
    g_alt_down = 0;
    g_extended = 0;

    bootlog64_print("IRQ64 initialized: PIC + PIT + keyboard.\n");
}