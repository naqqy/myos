[bits 32]

extern exception_handler_c
extern irq_handler_c

global idt_load
global isr_stub_table
global irq_stub_0
global irq_stub_1

section .text

; void idt_load(void* idtr)
idt_load:
    mov eax, [esp + 4]
    lidt [eax]
    ret

; ============================================================
; CPU exception stubs
; ============================================================

%macro ISR_NOERR 1
global isr_stub_%1
isr_stub_%1:
    push dword 0
    push dword %1
    jmp isr_common_stub
%endmacro

%macro ISR_ERR 1
global isr_stub_%1
isr_stub_%1:
    push dword %1
    jmp isr_common_stub
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

isr_common_stub:
    pusha

    push ds
    push es
    push fs
    push gs

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp
    call exception_handler_c
    add esp, 4

    pop gs
    pop fs
    pop es
    pop ds

    popa
    add esp, 8
    iretd

; ============================================================
; IRQ0 PIT timer stub
; CPU pushes: EIP, CS, EFLAGS
; We push: irq number
; ============================================================

irq_stub_0:
    push dword 0
    pusha

    push ds
    push es
    push fs
    push gs

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp
    call irq_handler_c
    add esp, 4

    pop gs
    pop fs
    pop es
    pop ds

    popa
    add esp, 4
    iretd

; ============================================================
; IRQ1 keyboard stub
; CPU pushes: EIP, CS, EFLAGS
; We push: irq number
; ============================================================

irq_stub_1:
    push dword 1
    pusha

    push ds
    push es
    push fs
    push gs

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp
    call irq_handler_c
    add esp, 4

    pop gs
    pop fs
    pop es
    pop ds

    popa
    add esp, 4
    iretd

section .rodata

isr_stub_table:
    dd isr_stub_0,  isr_stub_1,  isr_stub_2,  isr_stub_3
    dd isr_stub_4,  isr_stub_5,  isr_stub_6,  isr_stub_7
    dd isr_stub_8,  isr_stub_9,  isr_stub_10, isr_stub_11
    dd isr_stub_12, isr_stub_13, isr_stub_14, isr_stub_15
    dd isr_stub_16, isr_stub_17, isr_stub_18, isr_stub_19
    dd isr_stub_20, isr_stub_21, isr_stub_22, isr_stub_23
    dd isr_stub_24, isr_stub_25, isr_stub_26, isr_stub_27
    dd isr_stub_28, isr_stub_29, isr_stub_30, isr_stub_31