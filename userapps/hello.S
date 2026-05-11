.include "include/syscall64.inc"

.global _start

.section .text

_start:
    mov rax, SYS64_WRITE
    lea rdi, [rip + msg]
    mov rsi, OFFSET msg_len
    int 0x80

    mov rax, SYS64_EXIT
    xor rdi, rdi
    int 0x80

.loop:
    jmp .loop

.section .rodata

msg:
    .ascii "Hello from ELF64 user app via int 0x80!\n"
msg_len = . - msg