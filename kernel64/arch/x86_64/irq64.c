#include "irq64.h"
#include "io64.h"
#include "idt64.h"
#include "timer64.h"
#include "../../k64_console.h"
#include "../../input/kinput64.h"
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

    char c = g_shift_down ? shift_keymap[scancode] : keymap[scancode];

    if (c == 0) {
        return;
    }

    /*
     * Do not handle rendering here.
     * Queue all real characters, including backspace.
     */
    kinput64_push_char(c);
}

void irq_handler64(irq_frame64_t* frame) {
    if (!frame) {
        return;
    }

    nq_u64 irq = frame->irq;

    if (irq == 0) {
        timer64_on_irq();

        pic_send_eoi((nq_u8)irq);

        /*
        * Kernel task preemption.
        * This may context-switch before returning from the IRQ.
        */
        process64_on_timer_tick();
        return;
    } else if (irq == 1) {
        nq_u8 scancode = inb(0x60);
        keyboard_handle_scancode(scancode);
    }

    pic_send_eoi((nq_u8)irq);
}

void irq64_init(nq_u32 timer_hz) {
    __asm__ volatile ("cli");

    idt64_install_irq_gates();

    pic_remap();
    pic_mask_all();

    timer64_init(timer_hz);

    pic_unmask_irq(0);
    pic_unmask_irq(1);

    bootlog64_print("IRQ64 initialized: PIC + PIT + keyboard.\n");
}