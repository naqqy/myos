[org 0x7C00]
[bits 16]

%define STAGE2_SEGMENT 0x0000
%define STAGE2_OFFSET  0x8000
%define STAGE2_SECTORS 64
%define STAGE2_LBA     1

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    mov [boot_drive], dl
    mov [0x7E00], dl

    mov si, msg_loading
    call print_string

    ; Reset disk.
    mov ah, 0x00
    mov dl, [boot_drive]
    int 0x13
    jc disk_error

    ; Load stage2 using INT 13h Extensions / LBA read.
    ; This avoids the old CHS single-track limit.
    mov si, dap_stage2
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc disk_error

    mov si, msg_jump
    call print_string

    jmp 0x0000:0x8000

disk_error:
    mov si, msg_error
    call print_string
    jmp hang

print_string:
.next:
    lodsb
    test al, al
    jz .done

    mov ah, 0x0E
    mov bh, 0x00
    mov bl, 0x07
    int 0x10

    jmp .next

.done:
    ret

hang:
    cli
    hlt
    jmp hang

boot_drive db 0

msg_loading db 'Stage1: loading stage2...', 13, 10, 0
msg_jump    db 'Stage1: jumping to stage2.', 13, 10, 0
msg_error   db 'Stage1: disk read failed!', 13, 10, 0

; Disk Address Packet for INT 13h AH=42h.
dap_stage2:
    db 16                       ; packet size
    db 0                        ; reserved
    dw STAGE2_SECTORS           ; sector count
    dw STAGE2_OFFSET            ; destination offset
    dw STAGE2_SEGMENT           ; destination segment
    dd STAGE2_LBA               ; starting LBA low
    dd 0                        ; starting LBA high

times 510-($-$$) db 0
dw 0xAA55