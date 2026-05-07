[bits 32]

extern kmain

global start_kernel

section .text

start_kernel:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov esp, 0x90000

    call kmain

.halt:
    cli
    hlt
    jmp .halt