#include "shell.h"
#include "fat32.h"
#include "timer.h"
#include "kmem.h"
#include "paging.h"
#include "process.h"
#include "power.h"
#include "terminal.h"
#include "graphics/image.h"
#include "memmap.h"
#include "pmm.h"
#include "gdt.h"
#include "graphics/framebuffer.h"

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

typedef struct {
    u16 offset_low;
    u16 selector;
    u8  zero;
    u8  type_attr;
    u16 offset_high;
} __attribute__((packed)) idt_entry_t;

typedef struct {
    u16 limit;
    u32 base;
} __attribute__((packed)) idtr_t;

typedef struct {
    u32 gs, fs, es, ds;
    u32 edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    u32 vector;
    u32 error_code;
    u32 eip, cs, eflags;
} interrupt_frame_t;

typedef struct {
    u32 gs, fs, es, ds;
    u32 edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    u32 irq;
    u32 eip, cs, eflags;
} irq_frame_t;

extern void idt_load(idtr_t* idtr);
extern u32 isr_stub_table[];
extern void irq_stub_0(void);
extern void irq_stub_1(void);
extern u8 __kernel_end;
extern void syscall_stub_80(void);

static volatile u16* const VGA = (u16*)0xB8000;

#define VGA_TEXT_COLS 80
#define VGA_TEXT_ROWS 25

#define GFX_TERM_FG     0x00EAEAEAu
#define GFX_TERM_BG     0x00000000u
#define GFX_TERM_CURSOR 0x00EAEAEAu
#define GFX_CELL_W      8
#define GFX_CELL_H      16

#define SPLASH_IMAGE_PATH "/boot/splash.ni"

#define SPLASH_IMAGE_BUFFER      ((u8*)0x00800000u)
#define SPLASH_IMAGE_BUFFER_SIZE (4u * 1024u * 1024u)

static terminal_session_t kernel_terminal;
static terminal_session_t* active_terminal = 0;

static volatile int terminal_batch_depth = 0;
static volatile int terminal_dirty = 0;
static volatile int terminal_render_suspended = 0;

static u16 gfx_shadow[TERM_ROWS][TERM_COLS];
static int gfx_shadow_valid = 0;
static terminal_session_t* gfx_shadow_terminal = 0;
static u32 gfx_shadow_view_top = 0xFFFFFFFFu;

static int gfx_cursor_valid = 0;
static u32 gfx_cursor_row = 0;
static u32 gfx_cursor_col = 0;

static int shift_down = 0;
static int swallow_next_key = 0;
static int extended_scancode = 0;

static volatile int ctrl_down = 0;
volatile int kill_current_process_requested = 0;
volatile int foreground_escape_requested = 0;

static idt_entry_t idt[256];
static idtr_t idtr;
static volatile int key_pressed_flag = 0;

volatile int schedule_requested = 0;
volatile int scheduler_enabled = 0;

#define KERNEL_HEAP_START ((void*)0x400000)
#define KERNEL_HEAP_SIZE  (1024 * 1024)

#define RAW_KEY_QUEUE_SIZE 64

static volatile char raw_key_queue[RAW_KEY_QUEUE_SIZE];
static volatile u32 raw_key_head = 0;
static volatile u32 raw_key_tail = 0;
static volatile int raw_input_mode = 0;

static const char* exception_names[32] = {
    "#DE Divide Error", "#DB Debug", "NMI Interrupt", "#BP Breakpoint",
    "#OF Overflow", "#BR Bound Range Exceeded", "#UD Invalid Opcode", "#NM Device Not Available",
    "#DF Double Fault", "Coprocessor Segment Overrun", "#TS Invalid TSS", "#NP Segment Not Present",
    "#SS Stack Fault", "#GP General Protection Fault", "#PF Page Fault", "Reserved",
    "#MF x87 Floating Point", "#AC Alignment Check", "#MC Machine Check", "#XM SIMD Floating Point",
    "Virtualization Exception", "Control Protection Exception", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor Injection Exception", "VMM Communication Exception", "Security Exception", "Reserved"
};

static const char keymap[128] = {
    [0x01]=27,[0x02]='1',[0x03]='2',[0x04]='3',[0x05]='4',[0x06]='5',[0x07]='6',
    [0x08]='7',[0x09]='8',[0x0A]='9',[0x0B]='0',[0x0C]='-',[0x0D]='=',[0x0E]='\b',
    [0x0F]='\t',[0x10]='q',[0x11]='w',[0x12]='e',[0x13]='r',[0x14]='t',[0x15]='y',
    [0x16]='u',[0x17]='i',[0x18]='o',[0x19]='p',[0x1A]='[',[0x1B]=']',[0x1C]='\n',
    [0x1E]='a',[0x1F]='s',[0x20]='d',[0x21]='f',[0x22]='g',[0x23]='h',[0x24]='j',
    [0x25]='k',[0x26]='l',[0x27]=';',[0x28]='\'',[0x29]='`',[0x2B]='\\',[0x2C]='z',
    [0x2D]='x',[0x2E]='c',[0x2F]='v',[0x30]='b',[0x31]='n',[0x32]='m',[0x33]=',',
    [0x34]='.',[0x35]='/',[0x39]=' '
};

static const char shift_keymap[128] = {
    [0x01]=27,[0x02]='!',[0x03]='@',[0x04]='#',[0x05]='$',[0x06]='%',[0x07]='^',
    [0x08]='&',[0x09]='*',[0x0A]='(',[0x0B]=')',[0x0C]='_',[0x0D]='+',[0x0E]='\b',
    [0x0F]='\t',[0x10]='Q',[0x11]='W',[0x12]='E',[0x13]='R',[0x14]='T',[0x15]='Y',
    [0x16]='U',[0x17]='I',[0x18]='O',[0x19]='P',[0x1A]='{',[0x1B]='}',[0x1C]='\n',
    [0x1E]='A',[0x1F]='S',[0x20]='D',[0x21]='F',[0x22]='G',[0x23]='H',[0x24]='J',
    [0x25]='K',[0x26]='L',[0x27]=':',[0x28]='"',[0x29]='~',[0x2B]='|',[0x2C]='Z',
    [0x2D]='X',[0x2E]='C',[0x2F]='V',[0x30]='B',[0x31]='N',[0x32]='M',[0x33]='<',
    [0x34]='>',[0x35]='?',[0x39]=' '
};

static void raw_key_push(char c) {
    u32 next_tail;

    if (c == 0) {
        return;
    }

    next_tail = (raw_key_tail + 1) % RAW_KEY_QUEUE_SIZE;

    /*
     * If full, drop the key.
     */
    if (next_tail == raw_key_head) {
        return;
    }

    raw_key_queue[raw_key_tail] = c;
    raw_key_tail = next_tail;
}

static int raw_key_pop(void) {
    char c;

    if (raw_key_head == raw_key_tail) {
        return 0;
    }

    c = raw_key_queue[raw_key_head];
    raw_key_head = (raw_key_head + 1) % RAW_KEY_QUEUE_SIZE;

    return (int)c;
}

static u32 irq_save_disable(void) {
    u32 flags;

    __asm__ volatile ("pushf; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static void irq_restore(u32 flags) {
    if (flags & 0x200u) {
        __asm__ volatile ("sti");
    }
}

static void raw_key_clear(void) {
    u32 flags = irq_save_disable();

    raw_key_head = 0;
    raw_key_tail = 0;

    irq_restore(flags);
}

int kernel_poll_key(void) {
    int c;

    preempt_disable();
    c = raw_key_pop();
    preempt_enable();

    return c;
}

int kernel_read_key(void) {
    int c;

    for (;;) {
        c = kernel_poll_key();

        if (c != 0) {
            return c;
        }

        process_schedule_if_requested();

        __asm__ volatile ("sti");
        __asm__ volatile ("hlt");
    }
}

void kernel_set_raw_input(int enabled) {
    raw_input_mode = enabled ? 1 : 0;

    /*
     * Clear stale keys when switching modes.
     */
    raw_key_clear();
}

static void outb(u16 port, u8 value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static u32 align_up_u32(u32 value, u32 align) {
    return (value + align - 1) & ~(align - 1);
}

void console_lock(void) {
    preempt_disable();
}

void console_unlock(void) {
    preempt_enable();
}

terminal_session_t* terminal_kernel(void) {
    return &kernel_terminal;
}

terminal_session_t* terminal_active(void) {
    return active_terminal ? active_terminal : &kernel_terminal;
}

static terminal_session_t* term_active(void) {
    return terminal_active();
}

static void term_hw_cursor_enable(void) {
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x0E);
    outb(0x3D4, 0x0B);
    outb(0x3D5, 0x0F);
}

static void term_hw_cursor_disable(void) {
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x20);
}

static void term_hw_cursor_set(u32 row, u32 col) {
    if (row >= VGA_TEXT_ROWS) {
        row = VGA_TEXT_ROWS - 1;
    }

    if (col >= VGA_TEXT_COLS) {
        col = VGA_TEXT_COLS - 1;
    }

    u16 pos = (u16)(row * VGA_TEXT_COLS + col);

    outb(0x3D4, 0x0F);
    outb(0x3D5, (u8)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (u8)((pos >> 8) & 0xFF));
}

static void term_follow_bottom(void) {
    terminal_session_t* t = term_active();

    if (t->cursor_row >= TERM_ROWS) {
        t->view_top = t->cursor_row - (TERM_ROWS - 1);
    } else {
        t->view_top = 0;
    }
}

static int term_is_at_bottom(void) {
    terminal_session_t* t = term_active();

    u32 bottom_top = 0;
    if (t->cursor_row >= TERM_ROWS) {
        bottom_top = t->cursor_row - (TERM_ROWS - 1);
    }

    return t->view_top == bottom_top;
}

static void gfx_invalidate(void) {
    gfx_shadow_valid = 0;
    gfx_shadow_terminal = 0;
    gfx_shadow_view_top = 0xFFFFFFFFu;

    gfx_cursor_valid = 0;
    gfx_cursor_row = 0;
    gfx_cursor_col = 0;
}

static void gfx_draw_cell(terminal_session_t* t, u32 screen_row, u32 col) {
    u32 src_row;
    u16 cell;
    char ch;

    if (!t) {
        return;
    }

    if (screen_row >= TERM_ROWS || col >= TERM_COLS) {
        return;
    }

    src_row = t->view_top + screen_row;

    if (src_row < TERM_HISTORY_ROWS) {
        cell = t->buffer[src_row][col];
    } else {
        cell = (TERM_DEFAULT_ATTR << 8) | ' ';
    }

    ch = (char)(cell & 0xFF);

    fb_draw_char8x16(
        col * GFX_CELL_W,
        screen_row * GFX_CELL_H,
        ch,
        GFX_TERM_FG,
        GFX_TERM_BG
    );

    gfx_shadow[screen_row][col] = cell;
}

static void term_render(void) {
    terminal_session_t* t = term_active();

    if (!t) {
        return;
    }

    if (fb_available()) {
        u32 visible_cols = fb_width() / GFX_CELL_W;
        u32 visible_rows = fb_height() / GFX_CELL_H;

        if (visible_cols > TERM_COLS) {
            visible_cols = TERM_COLS;
        }

        if (visible_rows > TERM_ROWS) {
            visible_rows = TERM_ROWS;
        }

        int full_redraw = 0;

        if (!gfx_shadow_valid ||
            gfx_shadow_terminal != t ||
            gfx_shadow_view_top != t->view_top) {
            full_redraw = 1;
        }

        if (full_redraw) {
            gfx_shadow_valid = 1;
            gfx_shadow_terminal = t;
            gfx_shadow_view_top = t->view_top;
            gfx_cursor_valid = 0;
        }

        for (u32 row = 0; row < visible_rows; row++) {
            u32 src_row = t->view_top + row;

            for (u32 col = 0; col < visible_cols; col++) {
                u16 cell;

                if (src_row < TERM_HISTORY_ROWS) {
                    cell = t->buffer[src_row][col];
                } else {
                    cell = (TERM_DEFAULT_ATTR << 8) | ' ';
                }

                if (full_redraw || gfx_shadow[row][col] != cell) {
                    gfx_draw_cell(t, row, col);
                }
            }
        }

        int cursor_now_valid = 0;
        u32 cursor_screen_row = 0;
        u32 cursor_screen_col = 0;

        if (term_is_at_bottom()) {
            cursor_screen_row = t->cursor_row - t->view_top;
            cursor_screen_col = t->cursor_col;

            if (cursor_screen_row < visible_rows &&
                cursor_screen_col < visible_cols) {
                cursor_now_valid = 1;
            }
        }

        if (gfx_cursor_valid) {
            if (!cursor_now_valid ||
                gfx_cursor_row != cursor_screen_row ||
                gfx_cursor_col != cursor_screen_col) {
                if (gfx_cursor_row < visible_rows &&
                    gfx_cursor_col < visible_cols) {
                    gfx_draw_cell(t, gfx_cursor_row, gfx_cursor_col);
                }
            }
        }

        if (cursor_now_valid) {
            fb_draw_cursor8x16(
                cursor_screen_col * GFX_CELL_W,
                cursor_screen_row * GFX_CELL_H,
                GFX_TERM_CURSOR
            );

            gfx_cursor_valid = 1;
            gfx_cursor_row = cursor_screen_row;
            gfx_cursor_col = cursor_screen_col;
        } else {
            gfx_cursor_valid = 0;
        }

        return;
    }

    for (u32 row = 0; row < VGA_TEXT_ROWS; row++) {
        u32 src_row = t->view_top + row;

        for (u32 col = 0; col < VGA_TEXT_COLS; col++) {
            if (src_row < TERM_HISTORY_ROWS && col < TERM_COLS) {
                VGA[row * VGA_TEXT_COLS + col] = t->buffer[src_row][col];
            } else {
                VGA[row * VGA_TEXT_COLS + col] = (TERM_DEFAULT_ATTR << 8) | ' ';
            }
        }
    }

    if (term_is_at_bottom()) {
        u32 screen_row = t->cursor_row - t->view_top;

        if (screen_row < VGA_TEXT_ROWS && t->cursor_col < VGA_TEXT_COLS) {
            term_hw_cursor_enable();
            term_hw_cursor_set(screen_row, t->cursor_col);
        } else {
            term_hw_cursor_disable();
        }
    } else {
        term_hw_cursor_disable();
    }
}

static void term_request_render(void) {
    if (terminal_render_suspended) {
        terminal_dirty = 1;
        return;
    }

    if (terminal_batch_depth > 0) {
        terminal_dirty = 1;
        return;
    }

    term_render();
}

void kernel_begin_frame(void) {
    console_lock();

    terminal_batch_depth++;

    console_unlock();
}

static void terminal_flush_after_batch(void) {
    if (terminal_render_suspended) {
        terminal_dirty = 1;
        return;
    }

    if (terminal_batch_depth == 0 && terminal_dirty) {
        terminal_dirty = 0;
        term_render();
    }
}

void kernel_end_frame(void) {
    console_lock();

    if (terminal_batch_depth > 0) {
        terminal_batch_depth--;
    }

    terminal_flush_after_batch();

    console_unlock();
}

static void terminal_init_session(terminal_session_t* t) {
    if (!t) {
        return;
    }

    for (u32 row = 0; row < TERM_HISTORY_ROWS; row++) {
        for (u32 col = 0; col < TERM_COLS; col++) {
            t->buffer[row][col] = (0x07 << 8) | ' ';
        }
    }

    t->cursor_row = 0;
    t->cursor_col = 0;
    t->view_top = 0;
}

void terminal_init(void) {
    terminal_init_session(&kernel_terminal);
    active_terminal = &kernel_terminal;
    gfx_invalidate();
    term_render();
}

terminal_session_t* terminal_create(void) {
    terminal_session_t* t;

    t = (terminal_session_t*)kmalloc(sizeof(terminal_session_t));
    if (!t) {
        return 0;
    }

    terminal_init_session(t);
    return t;
}

void terminal_destroy(terminal_session_t* t) {
    if (!t || t == &kernel_terminal) {
        return;
    }

    if (active_terminal == t) {
        active_terminal = &kernel_terminal;
        gfx_invalidate();
        term_render();
    }

    kfree(t);
}

void terminal_switch(terminal_session_t* t) {
    if (!t) {
        t = terminal_kernel();
    }

    active_terminal = t;
    gfx_invalidate();
    term_render();
}

static void term_history_scroll_up(u32 lines) {
    terminal_session_t* t = term_active();

    if (lines > t->view_top) {
        t->view_top = 0;
    } else {
        t->view_top -= lines;
    }

    term_request_render();
}

static void term_history_scroll_down(u32 lines) {
    terminal_session_t* t = term_active();

    u32 bottom_top = 0;
    if (t->cursor_row >= TERM_ROWS) {
        bottom_top = t->cursor_row - (TERM_ROWS - 1);
    }

    t->view_top += lines;
    if (t->view_top > bottom_top) {
        t->view_top = bottom_top;
    }

    term_request_render();
}

static void term_history_scroll_home(void) {
    terminal_session_t* t = term_active();

    t->view_top = 0;
    term_request_render();
}

static void term_history_scroll_end(void) {
    term_follow_bottom();
    term_request_render();
}

static void term_advance_line(void) {
    terminal_session_t* t = term_active();

    t->cursor_col = 0;
    t->cursor_row++;

    if (t->cursor_row >= TERM_HISTORY_ROWS) {
        for (u32 row = 1; row < TERM_HISTORY_ROWS; row++) {
            for (u32 col = 0; col < TERM_COLS; col++) {
                t->buffer[row - 1][col] = t->buffer[row][col];
            }
        }

        for (u32 col = 0; col < TERM_COLS; col++) {
            t->buffer[TERM_HISTORY_ROWS - 1][col] = (0x07 << 8) | ' ';
        }

        t->cursor_row = TERM_HISTORY_ROWS - 1;

        if (t->view_top > 0) {
            t->view_top--;
        }
    }
}

static u8 inb(u16 port) {
    u8 value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

void term_clear(void) {
    terminal_session_t* t = term_active();

    if (!t) {
        return;
    }

    for (u32 row = 0; row < TERM_HISTORY_ROWS; row++) {
        for (u32 col = 0; col < TERM_COLS; col++) {
            t->buffer[row][col] =
                (TERM_DEFAULT_ATTR << 8) | ' ';
        }
    }

    t->cursor_row = 0;
    t->cursor_col = 0;
    t->view_top = 0;

    /*
     * Do NOT call gfx_invalidate() here.
     *
     * Apps like wolf3d may clear once per frame. If we invalidate the whole
     * framebuffer shadow every clear, every frame becomes a full 128x48
     * redraw. Keeping the shadow valid lets term_render() only redraw cells
     * that actually changed.
     */
    term_request_render();
}

void kernel_clear(void) {
    console_lock();

    terminal_batch_depth++;

    term_clear();

    if (terminal_batch_depth > 0) {
        terminal_batch_depth--;
    }

    terminal_flush_after_batch();

    console_unlock();
}

static void splash_put_text(u32 row, u32 col, const char* text) {
    terminal_session_t* t = terminal_active();

    if (!t || !text) {
        return;
    }

    if (row >= TERM_HISTORY_ROWS || col >= TERM_COLS) {
        return;
    }

    for (u32 i = 0; text[i] && (col + i) < TERM_COLS; i++) {
        t->buffer[row][col + i] =
            (TERM_DEFAULT_ATTR << 8) | (u8)text[i];
    }
}

static void splash_center_text(u32 row, const char* text) {
    u32 len = 0;

    if (!text) {
        return;
    }

    while (text[len]) {
        len++;
    }

    u32 col = 0;
    if (len < TERM_COLS) {
        col = (TERM_COLS - len) / 2;
    }

    splash_put_text(row, col, text);
}

static void splash_center_text_in_box(u32 row, u32 box_left, u32 box_width, const char* text) {
    u32 len = 0;

    if (!text || box_width == 0) {
        return;
    }

    while (text[len]) {
        len++;
    }

    u32 col = box_left;

    if (len < box_width) {
        col = box_left + ((box_width - len) / 2);
    }

    splash_put_text(row, col, text);
}

static void splash_draw_box(u32 top, u32 left, u32 width, u32 height) {
    terminal_session_t* t = terminal_active();

    if (!t) {
        return;
    }

    if (width < 2 || height < 2) {
        return;
    }

    if (top >= TERM_HISTORY_ROWS || left >= TERM_COLS) {
        return;
    }

    if (left + width > TERM_COLS) {
        width = TERM_COLS - left;
    }

    if (top + height > TERM_HISTORY_ROWS) {
        height = TERM_HISTORY_ROWS - top;
    }

    u32 bottom = top + height - 1;
    u32 right  = left + width - 1;

    for (u32 x = left + 1; x < right; x++) {
        t->buffer[top][x] =
            (TERM_DEFAULT_ATTR << 8) | '-';

        t->buffer[bottom][x] =
            (TERM_DEFAULT_ATTR << 8) | '-';
    }

    for (u32 y = top + 1; y < bottom; y++) {
        t->buffer[y][left] =
            (TERM_DEFAULT_ATTR << 8) | '|';

        t->buffer[y][right] =
            (TERM_DEFAULT_ATTR << 8) | '|';
    }

    t->buffer[top][left] =
        (TERM_DEFAULT_ATTR << 8) | '+';

    t->buffer[top][right] =
        (TERM_DEFAULT_ATTR << 8) | '+';

    t->buffer[bottom][left] =
        (TERM_DEFAULT_ATTR << 8) | '+';

    t->buffer[bottom][right] =
        (TERM_DEFAULT_ATTR << 8) | '+';
}

static void splash_text_fallback(void) {
    u32 box_w = 72;
    u32 box_h = 22;

    u32 box_left = 0;
    u32 box_top  = 0;

    if (TERM_COLS > box_w) {
        box_left = (TERM_COLS - box_w) / 2;
    }

    if (TERM_ROWS > box_h) {
        box_top = (TERM_ROWS - box_h) / 2;
    }

    terminal_render_suspended = 1;

    kernel_clear();

    splash_draw_box(box_top, box_left, box_w, box_h);

    splash_center_text_in_box(box_top + 2, box_left, box_w, "NAQ OS");
    splash_center_text_in_box(box_top + 3, box_left, box_w, "Developed by naqqydev");

    u32 text_left = box_left + 18;

    splash_put_text(box_top + 6,  text_left, "[ OK ] Bootloader");
    splash_put_text(box_top + 7,  text_left, "[ OK ] Protected Mode");
    splash_put_text(box_top + 8,  text_left, "[ OK ] IDT / IRQ");
    splash_put_text(box_top + 9,  text_left, "[ OK ] PIT Timer");
    splash_put_text(box_top + 10, text_left, "[ OK ] FAT32 Filesystem");
    splash_put_text(box_top + 11, text_left, "[ OK ] Paging");
    splash_put_text(box_top + 12, text_left, "[ OK ] Kernel Heap");
    splash_put_text(box_top + 13, text_left, "[ OK ] Process Table");
    splash_put_text(box_top + 14, text_left, "[ OK ] Scheduler");
    splash_put_text(box_top + 15, text_left, "[ OK ] Per-Process Terminals");

    splash_center_text_in_box(box_top + 19, box_left, box_w, "Press any key to enter KER");

    terminal_render_suspended = 0;
    terminal_dirty = 0;

    gfx_invalidate();
    term_render();
}

static void splash_screen(void) {
    if (!fb_available()) {
        splash_text_fallback();
        return;
    }

    /*
     * We are drawing directly to the framebuffer, not through the terminal.
     * Suspend terminal redraw so it does not overwrite the image.
     */
    terminal_render_suspended = 1;
    terminal_dirty = 0;

    if (!image_draw_nimg_file_centered(
            SPLASH_IMAGE_PATH,
            SPLASH_IMAGE_BUFFER,
            SPLASH_IMAGE_BUFFER_SIZE)) {
        terminal_render_suspended = 0;
        splash_text_fallback();
        return;
    }

    /*
     * Optional tiny prompt over the image.
     * Remove this block if you want a pure image-only splash.
     */
    const char* prompt = "Press any key to enter KER";
    u32 len = 0;
    while (prompt[len]) {
        len++;
    }

    u32 x = 0;
    if (fb_width() > len * 8) {
        x = (fb_width() - len * 8) / 2;
    }

    u32 y = fb_height() - 48;

    for (u32 i = 0; prompt[i]; i++) {
        fb_draw_char8x16(x + i * 8, y, prompt[i], 0x00EAEAEAu, 0x00000000u);
    }

    /*
     * After this image, the framebuffer no longer matches terminal shadow.
     * Invalidate before returning to KER.
     */
    gfx_invalidate();

    terminal_render_suspended = 0;
    terminal_dirty = 0;
}

void term_put_char(char c) {
    terminal_session_t* t = term_active();
    int was_at_bottom;

    if (!t) {
        return;
    }

    was_at_bottom = term_is_at_bottom();

    if (t->cursor_row >= TERM_HISTORY_ROWS) {
        t->cursor_row = TERM_HISTORY_ROWS - 1;
    }

    if (t->cursor_col >= TERM_COLS) {
        t->cursor_col = TERM_COLS - 1;
    }

    if (c == '\n') {
        term_advance_line();

        if (was_at_bottom) {
            term_follow_bottom();
        }

        term_request_render();
        return;
    }

    if (c == '\t') {
        u32 next_tab = (t->cursor_col + 4) & ~3U;

        while (t->cursor_col < next_tab) {
            t->buffer[t->cursor_row][t->cursor_col] =
                (TERM_DEFAULT_ATTR << 8) | ' ';

            t->cursor_col++;

            if (t->cursor_col >= TERM_COLS) {
                term_advance_line();

                if (t->cursor_row >= TERM_HISTORY_ROWS) {
                    break;
                }
            }
        }

        if (was_at_bottom) {
            term_follow_bottom();
        }

        term_request_render();
        return;
    }

    if (c == '\r') {
        t->cursor_col = 0;

        term_request_render();
        return;
    }

    if ((u8)c < 32) {
        return;
    }

    t->buffer[t->cursor_row][t->cursor_col] =
        (TERM_DEFAULT_ATTR << 8) | (u8)c;

    t->cursor_col++;

    if (t->cursor_col >= TERM_COLS) {
        term_advance_line();
    }

    if (was_at_bottom) {
        term_follow_bottom();
    }

    term_request_render();
}

void kernel_put_char(char c) {
    console_lock();

    terminal_batch_depth++;

    term_put_char(c);

    if (terminal_batch_depth > 0) {
        terminal_batch_depth--;
    }

    terminal_flush_after_batch();

    console_unlock();
}

void term_print(const char* s) {
    if (!s) {
        return;
    }

    console_lock();

    terminal_batch_depth++;

    while (*s) {
        term_put_char(*s++);
    }

    if (terminal_batch_depth > 0) {
        terminal_batch_depth--;
    }

    terminal_flush_after_batch();

    console_unlock();
}

void kernel_print(const char* s) {
    if (!s) {
        return;
    }

    console_lock();

    terminal_batch_depth++;

    while (*s) {
        term_put_char(*s++);
    }

    if (terminal_batch_depth > 0) {
        terminal_batch_depth--;
    }

    terminal_flush_after_batch();

    console_unlock();
}

void print_hex32(u32 value) {
    const char* hex = "0123456789ABCDEF";

    console_lock();

    terminal_batch_depth++;

    term_put_char('0');
    term_put_char('x');

    for (int i = 7; i >= 0; i--) {
        term_put_char(hex[(value >> (i * 4)) & 0xF]);
    }

    if (terminal_batch_depth > 0) {
        terminal_batch_depth--;
    }

    terminal_flush_after_batch();

    console_unlock();
}

static void wait_for_any_key(void) {
    raw_key_clear();
    swallow_next_key = 1;
    key_pressed_flag = 0;

    while (!key_pressed_flag) {
        __asm__ volatile ("hlt");
    }

    key_pressed_flag = 0;
    swallow_next_key = 0;
    raw_key_clear();
}

void term_backspace(void) {
    terminal_session_t* t = term_active();

    if (!t) {
        return;
    }

    if (t->cursor_row == 0 && t->cursor_col == 0) {
        return;
    }

    if (t->cursor_col == 0) {
        t->cursor_row--;
        t->cursor_col = TERM_COLS - 1;
    } else {
        t->cursor_col--;
    }

    t->buffer[t->cursor_row][t->cursor_col] =
        (TERM_DEFAULT_ATTR << 8) | ' ';

    term_follow_bottom();
    term_request_render();
}

void kernel_backspace(void) {
    console_lock();
    term_backspace();
    console_unlock();
}

static void print_dec32(u32 value) {
    char buf[16];
    int i = 0;

    console_lock();

    terminal_batch_depth++;

    if (value == 0) {
        term_put_char('0');
    } else {
        while (value > 0) {
            buf[i++] = '0' + (value % 10);
            value /= 10;
        }

        while (i > 0) {
            term_put_char(buf[--i]);
        }
    }

    if (terminal_batch_depth > 0) {
        terminal_batch_depth--;
    }

    terminal_flush_after_batch();

    console_unlock();
}

static void print_dec64_parts(u32 high, u32 low) {
    if (high == 0) {
        print_dec32(low);
        return;
    }

    kernel_print("0x");
    print_hex32(high);
    print_hex32(low);
}

static void idt_set_gate(int vector, u32 handler) {
    idt[vector].offset_low  = handler & 0xFFFF;
    idt[vector].selector    = 0x08;
    idt[vector].zero        = 0;
    idt[vector].type_attr   = 0x8E;
    idt[vector].offset_high = (handler >> 16) & 0xFFFF;
}

static void idt_set_user_gate(int vector, u32 handler) {
    idt[vector].offset_low  = handler & 0xFFFF;
    idt[vector].selector    = 0x08;
    idt[vector].zero        = 0;

    /*
     * 0xEE = present, DPL 3, 32-bit interrupt gate.
     * This lets ring 3 code call int 0x80 later.
     */
    idt[vector].type_attr   = 0xEE;

    idt[vector].offset_high = (handler >> 16) & 0xFFFF;
}

static void pic_remap(void) {
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, 0x20);
    outb(0xA1, 0x28);
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
}

static void pic_unmask_timer_keyboard(void) {
    u8 mask = inb(0x21);
    mask &= (u8)~((1 << 0) | (1 << 1));
    outb(0x21, mask);
}

static void pic_send_eoi(u32 irq) {
    if (irq >= 8) outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

static void idt_init(void) {
    for (int i = 0; i < 256; i++) {
        idt[i].offset_low = 0;
        idt[i].selector = 0;
        idt[i].zero = 0;
        idt[i].type_attr = 0;
        idt[i].offset_high = 0;
    }

    for (int i = 0; i < 32; i++) idt_set_gate(i, isr_stub_table[i]);
    idt_set_gate(32, (u32)irq_stub_0);
    idt_set_gate(33, (u32)irq_stub_1);
    idt_set_user_gate(0x80, (u32)syscall_stub_80);

    idtr.limit = (u16)(sizeof(idt) - 1);
    idtr.base = (u32)idt;
    idt_load(&idtr);
}

void exception_handler_c(interrupt_frame_t* frame) {
    __asm__ volatile ("cli");

    kernel_print("\n\nCPU EXCEPTION\n");

    kernel_print("Vector: ");
    print_dec32(frame->vector);
    kernel_print(" ");

    if (frame->vector < 32) {
        kernel_print(exception_names[frame->vector]);
    } else {
        kernel_print("Unknown");
    }

    kernel_print("\nError Code: ");
    print_hex32(frame->error_code);

    kernel_print("\nEIP: ");
    print_hex32(frame->eip);

    kernel_print("\nCS: ");
    print_hex32(frame->cs);

    kernel_print("\nEFLAGS: ");
    print_hex32(frame->eflags);

    if (frame->vector == 14) {
        u32 cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));

        kernel_print("\nCR2: ");
        print_hex32(cr2);
    }

    kernel_print("\nHALTED AFTER EXCEPTION\n");

    for (;;) {
        __asm__ volatile ("hlt");
    }
}

void irq_handler_c(irq_frame_t* frame) {
    if (frame->irq == 0) {
        timer_on_irq();

        if (scheduler_enabled && process_current() != 0) {
            schedule_requested = 1;

            if (preempt_can_switch() &&
                (process_kill_requested() ||
                 process_foreground_escape_requested() ||
                 process_has_runnable())) {
                pic_send_eoi(frame->irq);
                process_preempt_from_irq();
                return;
            }
        }
    }
    else if (frame->irq == 1) {
        u8 scancode = inb(0x60);

        if (scancode == 0xE0) {
            extended_scancode = 1;
        }
        else if (extended_scancode) {
            extended_scancode = 0;

            /*
             * Do not scroll/render from IRQ while an app is in raw input mode.
             * Games like wolf3d should only receive queued raw keys here.
             */
            if (!raw_input_mode) {
                switch (scancode) {
                    case 0x49: /* Page Up */
                        term_history_scroll_up(10);
                        break;
                    case 0x51: /* Page Down */
                        term_history_scroll_down(10);
                        break;
                    case 0x47: /* Home */
                        term_history_scroll_home();
                        break;
                    case 0x4F: /* End */
                        term_history_scroll_end();
                        break;
                    default:
                        break;
                }
            }
        }
        else if (scancode == 0x1D) {          /* Ctrl press */
            ctrl_down = 1;
        }
        else if (scancode == 0x9D) {          /* Ctrl release */
            ctrl_down = 0;
        }
        else if (scancode == 0x2A || scancode == 0x36) {
            shift_down = 1;
        }
        else if (scancode == 0xAA || scancode == 0xB6) {
            shift_down = 0;
        }
        else if (scancode & 0x80) {
            /*
             * Key release. Nothing else to do for now.
             */
        }
        else {
            if (ctrl_down && scancode == 0x1E) {   /* Ctrl+A */
                u32 fg = process_foreground_pid();

                if (fg != 0) {
                    foreground_escape_requested = 1;
                    schedule_requested = 1;
                    shell_pause_app_input_for_ker();
                }
            }
            else if (ctrl_down && scancode == 0x2E) {   /* Ctrl+C */
                u32 fg = process_foreground_pid();

                if (fg != 0) {
                    process_request_kill(fg);
                    shell_cancel_app_read_line();
                }
                else if (process_current() != 0) {
                    kill_current_process_requested = 1;
                    shell_cancel_app_read_line();
                }

                kernel_set_raw_input(0);
            }
            else {
                char c = shift_down ? shift_keymap[scancode] : keymap[scancode];

                if (c != 0) {
                    key_pressed_flag = 1;
                    raw_key_push(c);

                    /*
                     * Important:
                     * In raw input mode, do not call term_history_scroll_end()
                     * and do not render/echo from IRQ context.
                     *
                     * Wolf3D reads from the raw key queue and renders from
                     * its normal app loop, which is the safe path.
                     */
                    if (!raw_input_mode) {
                        term_history_scroll_end();

                        if (!swallow_next_key) {
                            shell_putc(c);
                        } else {
                            swallow_next_key = 0;
                        }
                    }
                }
            }
        }
    }

    pic_send_eoi(frame->irq);
}

static void print_paging_info(void) {
    kernel_print("Paging check 0x00100000: ");
    kernel_print(paging_is_mapped(0x00100000) ? "mapped" : "not mapped");
    kernel_print(" phys=");
    print_hex32(paging_get_physical(0x00100000));
    kernel_print("\n");

    kernel_print("Paging check 0x00400000: ");
    kernel_print(paging_is_mapped(0x00400000) ? "mapped" : "not mapped");
    kernel_print(" phys=");
    print_hex32(paging_get_physical(0x00400000));
    kernel_print("\n");

    if (fb_available()) {
        kernel_print("Framebuffer: available phys=");
        print_hex32(fb_phys_addr());
        kernel_print(" size=");
        print_dec32(fb_size_bytes());
        kernel_print(" bytes\n");
    } else {
        kernel_print("Framebuffer: not available\n");
    }
}

static void print_pmm_info(void) {
    kernel_print("PMM total usable frames: ");
    print_dec32(pmm_total_frames());
    kernel_print("\n");

    kernel_print("PMM used frames: ");
    print_dec32(pmm_used_frames());
    kernel_print("\n");

    kernel_print("PMM free frames: ");
    print_dec32(pmm_free_frames());
    kernel_print("\n");
}

static void print_kmem_info(void) {
    kernel_print("Heap committed: ");
    print_dec32(kmem_committed_size());
    kernel_print(" bytes\n");

    kernel_print("Heap max: ");
    print_dec32(kmem_max_size());
    kernel_print(" bytes\n");
}

static void terminal_suspend_render(void) {
    console_lock();

    terminal_render_suspended = 1;
    terminal_dirty = 1;

    console_unlock();
}

static void terminal_resume_render(void) {
    console_lock();

    terminal_render_suspended = 0;

    if (terminal_dirty) {
        terminal_dirty = 0;
        term_render();
    }

    console_unlock();
}

void kmain(void) {
    fb_init();

    terminal_init();

    /*
     * Do not redraw the framebuffer for every boot message.
     * Boot messages still go into the terminal buffer.
     */
    terminal_suspend_render();

    kernel_clear();

    gdt_init();
    kernel_print("GDT/TSS loaded.\n");

    idt_init();
    kernel_print("IDT loaded.\n");

    pic_remap();
    kernel_print("PIC remapped.\n");

    pic_unmask_timer_keyboard();
    kernel_print("Keyboard IRQ enabled.\n");

    if (!timer_init(100)) {
        kernel_print("Timer initialization failed.\n");
    }

    kernel_print("PIT timer enabled at 100 Hz.\n");

    fat32_init();
    memmap_init();
    pmm_init();

    pmm_reserve_region(0x00000000, 0x00100000);
    pmm_reserve_region((u32)SPLASH_IMAGE_BUFFER, SPLASH_IMAGE_BUFFER_SIZE);

    if (fb_available()) {
        pmm_reserve_region(fb_phys_addr(), fb_size_bytes());
    }

    u32 kernel_start = 0x00100000;
    u32 kernel_end   = align_up_u32((u32)&__kernel_end, PMM_FRAME_SIZE);
    pmm_reserve_region(kernel_start, kernel_end - kernel_start);

    paging_init();

    kernel_print("Paging enabled.\n");

    print_paging_info();

    kmem_init(KERNEL_HEAP_START, KERNEL_HEAP_SIZE);
    kernel_print("Kernel heap initialized.\n");
    print_kmem_info();

    process_table_init();
    kernel_print("Process table initialized.\n");

    scheduler_enabled = 1;

    memmap_print();
    print_pmm_info();

    __asm__ volatile ("sti");

    terminal_resume_render();

    splash_screen();

    wait_for_any_key();

    kernel_clear();
    shell_init();

    for (;;) {
        shell_poll();
        __asm__ volatile ("hlt");
    }
}
