.include "include/syscall64.inc"

.global _start

.section .text

_start:
    mov rax, SYS64_WRITE
    lea rdi, [rip + msg]
    mov rsi, OFFSET msg_len
    int 0x80

.loop:
    jmp .loop

.section .rodata

msg:
    .ascii "LOOP.ELF started and is now looping.\n"
msg_len = . - msg