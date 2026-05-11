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

    k64_print("kmain64 entered after ExitBootServices.\n");

    gdt64_init(boot);
    idt64_init();

    idt64_install_syscall_gate();
    syscall64_init();

    memmap64_init(boot);
    k64_print("UEFI memory map parsed.\n");
//    memmap64_print_summary();
    k64_print("\n");

    if (!pmm64_init(boot)) {
        k64_print("PMM64 initialization failed.\n");
        halt_forever();
    }

    k64_print("Kernel reserved base: ");
    k64_print_hex64(boot->kernel_load_addr);
    k64_print("\n");

    k64_print("Kernel reserved minimum end: ");
    k64_print_hex64(boot->kernel_load_addr + (4ull * 1024ull * 1024ull));
    k64_print("\n");

//    pmm64_print_summary();
    kernel64_test_pmm();

    if (boot->initrd_base && boot->initrd_size) {
        if (initrd64_init(boot->initrd_base, boot->initrd_size)) {
            k64_print("INITRD64 initialized.\n");
            initrd64_print_summary();
        } else {
            k64_print("INITRD64 invalid.\n");
        }
    } else {
        k64_print("No INITRD provided.\n");
    }

    if (!paging64_init(boot)) {
        k64_print("Paging64 initialization failed.\n");
        halt_forever();
    }

//    paging64_print_summary();
    k64_print("\n");

    if (!kmem64_init(16)) {
        k64_print("KMEM64 initialization failed.\n");
        halt_forever();
    }

//    kmem64_print_summary();
    kernel64_test_kmem();

    k64_print("Kernel64 foundation initialized.\n");

    kinput64_init();
    irq64_init(100);

    k64_print("Timer Frequency: ");
    k64_print_dec64(timer64_get_frequency());
    k64_print(" Hz\n");

    k64_print("Interrupts enabled.\n");

    if (block64_init()) {
        k64_print("Block64 initialized.\n");
    } else {
        k64_print("Block64: no legacy ATA disk detected.\n");
    }

    /*
     * UEFI image is currently a raw FAT32 filesystem image,
     * so FAT32 starts at disk LBA 0.
     */
    if (vfs64_mount_root()) {
        vfs64_print_summary();
    } else {
        k64_print("VFS64 mount failed.\n");
    }

    process64_init();

    k64_print("\n");

    shell64_init();

    for (;;) {
        shell64_poll();
        __asm__ volatile ("sti");
        __asm__ volatile ("hlt");
    }
}