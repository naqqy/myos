[bits 32]

section .note.GNU-stack,"",@progbits

global syscall_stub_80
extern syscall_handler_c

; int 0x80 syscall ABI:
;   eax = syscall number
;   ebx = arg1
;   ecx = arg2
;   edx = arg3
;   esi = arg4
;   edi = arg5
;
; return:
;   eax = result

syscall_stub_80:
    push ds
    push es
    push fs
    push gs

    pushad

    ; Switch to kernel data selectors while inside kernel C.
    ; This clobbers AX/EAX, so we must read the original syscall
    ; registers from the pushad frame, not from live registers.
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; pushad layout at current ESP:
    ; [esp + 0]  edi
    ; [esp + 4]  esi
    ; [esp + 8]  ebp
    ; [esp + 12] original esp
    ; [esp + 16] ebx
    ; [esp + 20] edx
    ; [esp + 24] ecx
    ; [esp + 28] eax

    ; Copy original syscall registers out of the pushad frame.
    mov eax, [esp + 28]    ; num
    mov ebx, [esp + 16]    ; arg1
    mov ecx, [esp + 24]    ; arg2
    mov edx, [esp + 20]    ; arg3
    mov esi, [esp + 4]     ; arg4
    mov edi, [esp + 0]     ; arg5

    ; syscall_handler_c(num, a, b, c, d, e)
    ; C args are pushed right-to-left.
    push edi
    push esi
    push edx
    push ecx
    push ebx
    push eax

    call syscall_handler_c

    add esp, 24

    ; Store return value into saved EAX slot.
    mov [esp + 28], eax

    popad

    pop gs
    pop fs
    pop es
    pop ds

    iretd