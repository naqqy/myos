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
