[bits 64]

global context_switch64

section .text

; ============================================================
; void context_switch64(nq_u64* old_rsp, nq_u64 new_rsp)
;
; System V AMD64 calling convention:
;   rdi = old_rsp pointer
;   rsi = new_rsp value
;
; Saves current task callee-saved registers:
;   rbx, rbp, r12, r13, r14, r15
;
; Stack layout after saving:
;   [rsp + 0x00] r15
;   [rsp + 0x08] r14
;   [rsp + 0x10] r13
;   [rsp + 0x18] r12
;   [rsp + 0x20] rbp
;   [rsp + 0x28] rbx
;   [rsp + 0x30] return address already placed by CALL
;
; The destination stack must have the same saved-register layout
; followed by a return address.
; ============================================================

context_switch64:
    ; Save callee-saved registers for current task.
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    ; Store current RSP into *old_rsp.
    mov [rdi], rsp

    ; Switch to new task stack.
    mov rsp, rsi

    ; Restore callee-saved registers for new task.
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx

    ; Return into the new task.
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
