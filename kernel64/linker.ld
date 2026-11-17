OUTPUT_FORMAT(elf64-x86-64)
ENTRY(kernel64_start)

SECTIONS
{
    . = 0x00100000;

    .text : ALIGN(4K)
    {
        *(.text.entry)
        *(.text*)
    }

    .rodata : ALIGN(4K)
    {
        *(.rodata*)
    }

    .data : ALIGN(4K)
    {
        *(.data*)
    }

    .bss : ALIGN(4K)
    {
        *(COMMON)
        *(.bss*)
    }

    __kernel64_end = .;
}

/*
 * The UEFI loader only reserves UEFI_KERNEL64_ALLOC_PAGES (4 MiB) at the
 * load address, and KERNEL64.BIN does not contain .bss. Anything past that
 * is memory the firmware may have handed to something else (INITRD, boot
 * info, stack). Keep big buffers on the kernel heap, or raise
 * UEFI_KERNEL64_ALLOC_PAGES in uefi/bootx64.c together with this limit.
 */
ASSERT(__kernel64_end <= 0x00500000,
       "kernel64 image + bss is larger than the 4 MiB the UEFI loader reserves")
