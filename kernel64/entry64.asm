[bits 64]

global kernel64_start
extern kmain64

%define NAQ_BOOT_INFO_STACK_TOP 80

section .text.entry

kernel64_start:
    ; UEFI loader jumps here with:
    ;   RDI = naq_boot_info_t*

    cli

    ; Preserve boot info pointer while switching stacks.
    mov rbx, rdi

    ; Switch to the kernel stack allocated by the UEFI loader.
    mov rsp, [rbx + NAQ_BOOT_INFO_STACK_TOP]
    and rsp, -16

    ; Restore first argument for kmain64.
    mov rdi, rbx

    call kmain64

.hang:
    hlt
    jmp .hang