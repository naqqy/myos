#include "naq_boot_info.h"
#include "k64_console.h"

#include "arch/x86_64/gdt64.h"
#include "arch/x86_64/idt64.h"
#include "arch/x86_64/irq64.h"
#include "arch/x86_64/timer64.h"
#include "arch/x86_64/syscall64.h"

#include "mm/memmap64.h"
#include "mm/pmm64.h"
#include "mm/paging64.h"
#include "mm/kmem64.h"

#include "input/kinput64.h"

#include "shell/shell64.h"

#include "storage/block64.h"

#include "fs/vfs64.h"
#include "fs/initrd64.h"

#include "proc/process64.h"

#include "safety/safe_mode64.h"

#include "debug/bootlog64.h"
#include "debug/boot_diag64.h"

#include "gui/gui64.h"

#include "usb/usb64.h"

static void halt_forever(void) {
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

static void kernel64_test_pmm(void) {
    nq_u64 test_frame = pmm64_alloc_frame();

    k64_print("PMM64 test frame: ");
    k64_print_hex64(test_frame);
    k64_print("\n");

    if (test_frame) {
        pmm64_free_frame(test_frame);
        k64_print("PMM64 test frame freed.\n");
    }

    k64_print("\n");
}

static void kernel64_test_kmem(void) {
    void* heap_test_a = kmalloc64(64);
    void* heap_test_b = kmalloc64(4096);
    void* heap_test_c = kmalloc64(12345);

    k64_print("KMEM64 test A: ");
    k64_print_hex64((nq_u64)heap_test_a);
    k64_print("\n");

    k64_print("KMEM64 test B: ");
    k64_print_hex64((nq_u64)heap_test_b);
    k64_print("\n");

    k64_print("KMEM64 test C: ");
    k64_print_hex64((nq_u64)heap_test_c);
    k64_print("\n");

    kfree64(heap_test_a);
    kfree64(heap_test_b);
    kfree64(heap_test_c);

    k64_print("KMEM64 test allocations freed.\n");
//    kmem64_print_summary();

    k64_print("\n");
}

void kmain64(naq_boot_info_t* boot) {
    k64_console_init(boot);

    safe_mode64_init();

    if (boot) {
        block64_set_boot_volume(boot->boot_volume);
    }

    bootlog64_print("kmain64 entered after ExitBootServices.\n");

    /*
     * CPU descriptor/interrupt setup.
     */
    gdt64_init(boot);
    idt64_init();
    idt64_install_irq_gates();
    idt64_install_syscall_gate();
    syscall64_init();

    /*
     * Boot memory information.
     */
    memmap64_init(boot);

    if (!pmm64_init(boot)) {
        k64_print("PMM64 initialization failed.\n");
        halt_forever();
    }

    /*
     * IMPORTANT:
     * INITRD must be initialized before vfs64_mount_root(),
     * especially in SAFE_HW mode where VFS prefers INITRD.
     */
    if (boot && boot->initrd_base && boot->initrd_size) {
        if (initrd64_init(boot->initrd_base, boot->initrd_size)) {
            bootlog64_print("INITRD64 initialized.\n");
        } else {
            k64_print("INITRD64 invalid.\n");
        }
    } else {
        k64_print("No INITRD provided.\n");
    }

    /*
     * Paging + heap.
     */
    if (!paging64_init(boot)) {
        k64_print("Paging64 initialization failed.\n");
        halt_forever();
    }

    if (!kmem64_init(16)) {
        k64_print("KMEM64 initialization failed.\n");
        halt_forever();
    }

    /*
     * Input and hardware IRQ/timer setup.
     * IMPORTANT:
     * Without this, keyboard input will not work.
     */
    kinput64_init();
    irq64_init(1000);

    /*
     * Process/task system.
     */
    process64_init();

    /*
     * Storage/VFS policy.
     *
     * SAFE_HW should avoid disk probing entirely.
     */
    if (safe_mode64_enabled()) {
        bootlog64_print("SAFE_HW: skipping block64 disk probing.\n");
        bootlog64_print("SAFE_HW: using INITRD-first root policy.\n");
    } else {
        if (block64_init()) {
            bootlog64_print("Block64 initialized.\n");
        } else {
            bootlog64_print("Block64 unavailable.\n");
        }
    }

    if (!vfs64_mount_root()) {
        k64_print("VFS64 mount failed.\n");
    }

    /*
     * USB keyboards and mice. Runs in its own task; machines without a
     * USB 3 (xHCI) controller simply have nothing to find.
     */
    usb64_init();

    gui64_init();

    /*
     * One clean boot summary.
     */
    boot_diag64_print(boot);

    /*
     * Give the devices plugged in at boot a moment to come up, and say
     * what was found, before the prompt appears.
     */
    usb64_boot_report();

    /*
     * The boot stick is mounted by now: pick up the saved desktop settings.
     */
    gui64_settings_load();

    shell64_init();

    for (;;) {
        shell64_poll();
        __asm__ volatile ("sti");
        __asm__ volatile ("hlt");
    }
}