[bits 32]

section .note.GNU-stack,"",@progbits

global _start
extern app_main

_start:
    call app_main

    ; If app_main returns, exit(0)
    mov eax, 0      ; SYS_EXIT
    mov ebx, 0      ; exit code
    int 0x80

.hang:
    cli
    hlt
    jmp .hang