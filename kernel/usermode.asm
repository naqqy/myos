[bits 32]

global usermode_enter

; void usermode_enter(u32 entry, u32 user_stack)
;
; stack:
;   [esp + 4] = entry eip
;   [esp + 8] = user esp

usermode_enter:
    cli

    mov eax, [esp + 4]    ; user entry
    mov ecx, [esp + 8]    ; user stack

    ; Load user data selectors before iret.
    ; 0x23 = user data selector with RPL 3.
    mov dx, 0x23
    mov ds, dx
    mov es, dx
    mov fs, dx
    mov gs, dx

    ; Build an iret frame:
    ; SS
    ; ESP
    ; EFLAGS
    ; CS
    ; EIP

    push 0x23             ; user SS
    push ecx              ; user ESP

    pushfd
    pop edx
    or edx, 0x200         ; enable IF
    push edx              ; EFLAGS

    push 0x1B             ; user CS selector with RPL 3
    push eax              ; user EIP

    iretd