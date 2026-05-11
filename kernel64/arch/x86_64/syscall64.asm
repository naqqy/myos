[bits 64]

global syscall64_stub

extern syscall64_handler

section .text

; ============================================================
; syscall64_stub
;
; Entry through int 0x80 from ring 3.
;
; User ABI:
;   rax = syscall number
;   rdi = arg0
;   rsi = arg1
;   rdx = arg2
;   r10 = arg3
;   r8  = arg4
;   r9  = arg5
;
; Return:
;   rax = return value
; ============================================================

syscall64_stub:
    push qword 0x80

    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp
    call syscall64_handler

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax

    add rsp, 8

    iretq