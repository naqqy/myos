[bits 64]

global gdt64_load
global tss64_load

section .text

; void gdt64_load(gdt64_ptr_t* ptr)
; rdi = ptr
gdt64_load:
    lgdt [rdi]

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; Reload CS with a far return.
    push qword 0x08
    lea rax, [rel .reload_cs]
    push rax
    retfq

.reload_cs:
    ret

; void tss64_load(u16 selector)
; rdi = selector
tss64_load:
    mov ax, di
    ltr ax
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
