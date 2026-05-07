[bits 32]

global gdt_flush
global tss_flush

; void gdt_flush(u32 gdt_ptr_addr)
gdt_flush:
    mov eax, [esp + 4]
    lgdt [eax]

    mov ax, 0x10        ; kernel data selector
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    jmp 0x08:.flush_cs  ; kernel code selector

.flush_cs:
    ret

tss_flush:
    mov ax, 0x28        ; TSS selector
    ltr ax
    ret