PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}

ENTRY(start_kernel)

SECTIONS
{
    . = 0x00100000;

    .text :
    {
        *(.text*)
    } :text

    .rodata :
    {
        *(.rodata*)
    } :text

    .data :
    {
        *(.data*)
    } :data

    .bss :
    {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __kernel_end = .;
    } :data
}
