[bits 64]

extern exception_handler64
extern irq_handler64

global idt_load64
global isr64_stub_table

global irq64_stub_0
global irq64_stub_1

section .text

; void idt_load64(idt64_ptr_t* idtr)
idt_load64:
    lidt [rdi]
    ret

; ============================================================
; CPU exception stubs
; ============================================================

%macro ISR_NOERR 1
global isr64_stub_%1
isr64_stub_%1:
    push qword 0
    push qword %1
    jmp isr64_common
%endmacro

%macro ISR_ERR 1
global isr64_stub_%1
isr64_stub_%1:
    push qword %1
    jmp isr64_common
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30
ISR_NOERR 31

isr64_common:
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

    ; RDI = interrupt_frame64_t*
    mov rdi, rsp

    ; Align stack before calling C.
    mov rax, rsp
    and rax, 15
    jz .isr_call_aligned

    sub rsp, 8
    call exception_handler64
    add rsp, 8
    jmp .isr_call_done

.isr_call_aligned:
    call exception_handler64

.isr_call_done:
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

    ; Remove vector + error_code.
    add rsp, 16

    iretq

; ============================================================
; IRQ stubs
; ============================================================

irq64_stub_0:
    push qword 0
    jmp irq64_common

irq64_stub_1:
    push qword 1
    jmp irq64_common

irq64_common:
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

    ; RDI = irq_frame64_t*
    mov rdi, rsp

    ; Align stack before calling C.
    ; This is important for IRQs because the IRQ stub only pushes one qword.
    mov rax, rsp
    and rax, 15
    jz .irq_call_aligned

    sub rsp, 8
    call irq_handler64
    add rsp, 8
    jmp .irq_call_done

.irq_call_aligned:
    call irq_handler64

.irq_call_done:
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

    ; Remove irq number.
    add rsp, 8

    iretq

section .rodata

isr64_stub_table:
    dq isr64_stub_0
    dq isr64_stub_1
    dq isr64_stub_2
    dq isr64_stub_3
    dq isr64_stub_4
    dq isr64_stub_5
    dq isr64_stub_6
    dq isr64_stub_7
    dq isr64_stub_8
    dq isr64_stub_9
    dq isr64_stub_10
    dq isr64_stub_11
    dq isr64_stub_12
    dq isr64_stub_13
    dq isr64_stub_14
    dq isr64_stub_15
    dq isr64_stub_16
    dq isr64_stub_17
    dq isr64_stub_18
    dq isr64_stub_19
    dq isr64_stub_20
    dq isr64_stub_21
    dq isr64_stub_22
    dq isr64_stub_23
    dq isr64_stub_24
    dq isr64_stub_25
    dq isr64_stub_26
    dq isr64_stub_27
    dq isr64_stub_28
    dq isr64_stub_29
    dq isr64_stub_30
    dq isr64_stub_31

section .note.GNU-stack noalloc noexec nowrite progbits
