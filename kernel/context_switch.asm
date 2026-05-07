[bits 32]
global context_switch

; void context_switch(u32* old_esp, u32 new_esp)

context_switch:
    push ebp
    push ebx
    push esi
    push edi

    mov eax, [esp + 20]    ; old_esp pointer
    mov edx, [esp + 24]    ; new_esp value

    mov [eax], esp
    mov esp, edx

    pop edi
    pop esi
    pop ebx
    pop ebp
    ret