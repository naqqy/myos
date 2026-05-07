[bits 32]

extern __bss_start
extern __kernel_end
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

    cld
    xor eax, eax
    mov edi, __bss_start
    mov ecx, __kernel_end
    sub ecx, edi
    rep stosb

    mov esp, kernel_stack_top

    call kmain

.halt:
    cli
    hlt
    jmp .halt

section .bss
align 16
kernel_stack_bottom:
    resb 65536
kernel_stack_top:
