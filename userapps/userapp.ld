ENTRY(_start)

SECTIONS
{
    /*
     * Must match VMEM64_USER_BASE in kernel64/mm/vmem64.h.
     * The address is above 2 GiB, so apps are compiled with -fpie
     * (RIP-relative addressing) and linked here as a fixed-address
     * executable.
     */
    . = 0x0000008000000000;

    .text : ALIGN(0x1000)
    {
        *(.text.start)
        *(.text*)
    }

    .rodata : ALIGN(0x1000)
    {
        *(.rodata*)
    }

    .data : ALIGN(0x1000)
    {
        *(.data*)
    }

    .bss : ALIGN(0x1000)
    {
        *(COMMON)
        *(.bss*)
    }
}