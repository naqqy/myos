[bits 64]

global usermode64_enter
global usermode64_test_entry

section .text

usermode64_enter:
    cli

    mov ax, 0x1B
    mov ds, ax
    mov es, ax

    xor ax, ax
    mov fs, ax
    mov gs, ax

    push qword 0x1B
    push rsi

    pushfq
    pop rax
    or rax, 0x200
    push rax

    push qword 0x23
    push rdi

    iretq


usermode64_test_entry:
    ; SYS_WRITE(msg, len)
    mov rax, 0
    lea rdi, [rel user_msg]
    mov rsi, user_msg_len
    int 0x80

    ; SYS_GET_TICKS()
    mov rax, 2
    int 0x80

.loop:
    nop
    jmp .loop


section .rodata

user_msg:
    db "Hello from ring 3 via int 0x80!", 10, 0

user_msg_len equ $ - user_msg