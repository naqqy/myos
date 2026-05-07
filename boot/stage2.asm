[org 0x8000]
[bits 16]

%define KERNEL_LOAD_PHYS      0x00100000
%define KERNEL_ENTRY_PHYS     0x00100000

; Temporary low-memory kernel load buffer.
; Must not overlap stage2 at 0x8000, stack at 0x9000, buffers at 0xA000/0xB000,
; E820 map at 0x5000, or the final kernel at 1 MiB.
%define KERNEL_TEMP_PHYS      0x00020000
%define KERNEL_TEMP_SEG       0x2000

%define FS_START_LBA          65
%define E820_MAP_SEG          0x0000
%define E820_MAP_OFF          0x5000
%define E820_MAX_ENTRIES      32

%define FB_INFO_ADDR          0x00006000
%define FB_INFO_MAGIC         0x3142464E        ; "NFB1"

%define FONT8X16_INFO_ADDR    0x00003FF0
%define FONT8X16_ADDR         0x00004000
%define FONT8X16_MAGIC        0x3154464E        ; "NFT1"

; Common QEMU/Bochs VBE mode:
; 0x118 is commonly 1024x768x32.
%define VBE_MODE_32           0x118

%define FAT32_EOC             0x0FFFFFF8

start_stage2:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    mov si, msg_stage2
    call print_string

    mov dl, [0x7E00]
    mov [boot_drive], dl

    call enable_a20

    call collect_e820_map
    jc e820_error

    ; Read FAT32 boot sector, filesystem-relative LBA 0.
    mov ax, BUFFER_SEG
    mov es, ax
    mov bx, BUFFER_OFF
    xor eax, eax
    call read_fs_sector_lba
    jc disk_error

    ; Parse FAT32 BPB.
    mov ax, [es:BUFFER_OFF + 11]
    mov [bytes_per_sector], ax

    mov al, [es:BUFFER_OFF + 13]
    mov [sectors_per_cluster], al

    mov ax, [es:BUFFER_OFF + 14]
    mov [reserved_sectors], ax

    mov al, [es:BUFFER_OFF + 16]
    mov [num_fats], al

    mov eax, [es:BUFFER_OFF + 36]
    mov [sectors_per_fat32], eax

    mov eax, [es:BUFFER_OFF + 44]
    mov [root_cluster], eax

    ; fat_start = reserved_sectors
    movzx eax, word [reserved_sectors]
    mov [fat_start], eax

    ; data_start = reserved_sectors + num_fats * sectors_per_fat32
    xor eax, eax
    mov al, [num_fats]
    mul dword [sectors_per_fat32]
    add eax, [fat_start]
    mov [data_start], eax

    ; Restore DS/ES to 0 after BPB parsing.
    xor ax, ax
    mov ds, ax
    mov es, ax

    mov si, msg_search
    call print_string

    call find_kernel_file
    jc file_not_found

    mov si, msg_load
    call print_string

    call load_kernel_file
    jc disk_error

    ; Restore DS/ES after kernel load because loader uses segments.
    xor ax, ax
    mov ds, ax
    mov es, ax

    mov si, msg_jump
    call print_string

    ; Set graphics mode at the last possible real-mode moment.
    ; After this, BIOS text output is no longer useful.
    call copy_bios_font8x16
    call setup_vbe

    xor ax, ax
    mov ds, ax
    mov es, ax

    cli
    lgdt [gdt_descriptor]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    jmp 0x08:protected_mode_entry

file_not_found:
    mov si, msg_not_found
    call print_string
    jmp hang

disk_error:
    mov si, msg_disk_error
    call print_string
    jmp hang

e820_error:
    mov si, msg_e820_error
    call print_string
    jmp hang

hang:
    cli
    hlt
    jmp hang

; ------------------------------------------------------------
; Copy BIOS 8x16 ROM font to low memory.
;
; Writes:
;   FONT8X16_INFO_ADDR + 0 = magic "NFT1"
;   FONT8X16_ADDR          = 256 glyphs * 16 bytes = 4096 bytes
;
; Must be called in real mode before graphics mode.
; ------------------------------------------------------------

copy_bios_font8x16:
    push ax
    push bx
    push cx
    push si
    push di
    push ds
    push es

    ; Ask BIOS for 8x16 ROM font pointer.
    mov ax, 0x1130
    mov bh, 0x06
    int 0x10

    ; BIOS returns ES:BP -> font.
    mov ax, es
    mov ds, ax
    mov si, bp

    xor ax, ax
    mov es, ax
    mov di, FONT8X16_ADDR

    cld
    mov cx, 4096
    rep movsb

    ; Mark font as available.
    xor ax, ax
    mov es, ax
    mov dword [es:FONT8X16_INFO_ADDR], FONT8X16_MAGIC

    pop es
    pop ds
    pop di
    pop si
    pop cx
    pop bx
    pop ax
    ret

; ------------------------------------------------------------
; VBE setup
; ------------------------------------------------------------

setup_vbe:
    push ax
    push bx
    push cx
    push di
    push ds
    push es

    ; Clear framebuffer info first.
    xor ax, ax
    mov es, ax
    mov di, FB_INFO_ADDR
    mov cx, 6

.clear_fb_info_loop:
    mov dword [es:di], 0
    add di, 4
    loop .clear_fb_info_loop

    ; Get VBE mode info into VBE_INFO_SEG:VBE_INFO_OFF.
    mov ax, VBE_INFO_SEG
    mov es, ax
    mov di, VBE_INFO_OFF
    mov ax, 0x4F01
    mov cx, VBE_MODE_32
    int 0x10

    cmp ax, 0x004F
    jne .fail

    ; Set mode with linear framebuffer bit.
    mov ax, 0x4F02
    mov bx, VBE_MODE_32 | 0x4000
    int 0x10

    cmp ax, 0x004F
    jne .fail

    ; Store framebuffer info at FB_INFO_ADDR.
    xor ax, ax
    mov es, ax

    mov dword [es:FB_INFO_ADDR + 0], FB_INFO_MAGIC

    mov ax, VBE_INFO_SEG
    mov ds, ax

    ; VBE mode info offsets:
    ; +16 bytes_per_scanline / pitch word
    ; +18 width word
    ; +20 height word
    ; +25 bpp byte
    ; +40 physical framebuffer dword

    mov eax, [VBE_INFO_OFF + 40]
    mov [es:FB_INFO_ADDR + 4], eax

    xor eax, eax
    mov ax, [VBE_INFO_OFF + 18]
    mov [es:FB_INFO_ADDR + 8], eax

    xor eax, eax
    mov ax, [VBE_INFO_OFF + 20]
    mov [es:FB_INFO_ADDR + 12], eax

    xor eax, eax
    mov ax, [VBE_INFO_OFF + 16]
    mov [es:FB_INFO_ADDR + 16], eax

    xor eax, eax
    mov al, [VBE_INFO_OFF + 25]
    mov [es:FB_INFO_ADDR + 20], eax

    jmp .done

.fail:
    ; Leave magic cleared, so kernel knows framebuffer is unavailable.
    xor ax, ax
    mov es, ax
    mov dword [es:FB_INFO_ADDR + 0], 0

.done:
    pop es
    pop ds
    pop di
    pop cx
    pop bx
    pop ax
    ret

; ------------------------------------------------------------
; A20
; ------------------------------------------------------------

enable_a20:
    mov ax, 0x2401
    int 0x15

    in al, 0x92
    or al, 00000010b
    and al, 11111110b
    out 0x92, al
    ret

; ------------------------------------------------------------
; FAT32 root search for KERNEL.BIN
; ------------------------------------------------------------

find_kernel_file:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es

    xor ax, ax
    mov ds, ax

    mov eax, [root_cluster]
    mov [current_cluster], eax

.cluster_loop:
    mov eax, [current_cluster]
    cmp eax, FAT32_EOC
    jae .fail

    call cluster_to_lba
    mov [current_sector], eax

    xor cx, cx

.sector_loop:
    mov ax, BUFFER_SEG
    mov es, ax
    mov bx, BUFFER_OFF

    mov eax, [current_sector]
    call read_fs_sector_lba
    jc .fail

    mov di, BUFFER_OFF

.scan_entries:
    cmp byte [es:di], 0x00
    je .fail

    cmp byte [es:di], 0xE5
    je .next_entry

    mov al, [es:di + 11]

    ; Skip LFN, volume labels, directories.
    cmp al, 0x0F
    je .next_entry

    test al, 00001000b
    jnz .next_entry

    test al, 00010000b
    jnz .next_entry

    push di
    mov si, kernel_name
    mov bx, 11

.compare_loop:
    mov al, [si]
    cmp al, [es:di]
    jne .compare_fail

    inc si
    inc di
    dec bx
    jnz .compare_loop

    pop di

    ; first cluster = high word at +20, low word at +26
    xor eax, eax
    mov ax, [es:di + 20]
    shl eax, 16
    mov ax, [es:di + 26]
    mov [kernel_first_cluster], eax

    mov eax, [es:di + 28]
    mov [kernel_file_size], eax

    clc
    jmp .done

.compare_fail:
    pop di

.next_entry:
    add di, 32
    cmp di, BUFFER_OFF + 512
    jb .scan_entries

    inc dword [current_sector]
    inc cx

    movzx ax, byte [sectors_per_cluster]
    cmp cx, ax
    jb .sector_loop

    call get_next_cluster
    jc .fail

    jmp .cluster_loop

.fail:
    stc

.done:
    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; ------------------------------------------------------------
; Load kernel cluster chain to low temporary buffer at 0x20000
; ------------------------------------------------------------

load_kernel_file:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es

    mov word [kernel_temp_seg], KERNEL_TEMP_SEG

    mov eax, [kernel_first_cluster]
    mov [current_cluster], eax

.cluster_loop:
    mov eax, [current_cluster]
    cmp eax, FAT32_EOC
    jae .success

    call cluster_to_lba
    mov [current_sector], eax

    xor cx, cx

.sector_loop:
    ; Read one sector directly into the low temporary kernel buffer.
    ; Each sector advances ES by 0x20 paragraphs = 512 bytes.
    mov ax, [kernel_temp_seg]
    mov es, ax
    xor bx, bx

    mov eax, [current_sector]
    call read_fs_sector_lba
    jc .fail

    add word [kernel_temp_seg], 0x20

    inc dword [current_sector]
    inc cx

    movzx ax, byte [sectors_per_cluster]
    cmp cx, ax
    jb .sector_loop

    call get_next_cluster
    jc .fail

    jmp .cluster_loop

.success:
    clc
    jmp .done

.fail:
    stc

.done:
    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; ------------------------------------------------------------
; FAT32 helpers
; ------------------------------------------------------------

; EAX = cluster
; returns EAX = filesystem-relative first sector of cluster
cluster_to_lba:
    push ebx
    push ecx

    sub eax, 2
    xor ecx, ecx
    mov cl, [sectors_per_cluster]
    mul ecx
    add eax, [data_start]

    pop ecx
    pop ebx
    ret

get_next_cluster:
    push ax
    push bx
    push dx
    push es

    ; fat_offset = cluster * 4
    mov eax, [current_cluster]
    shl eax, 2

    ; fat_sector = fat_start + fat_offset / bytes_per_sector
    xor edx, edx
    movzx ebx, word [bytes_per_sector]
    div ebx

    add eax, [fat_start]
    mov [fat_sector], eax
    mov [fat_offset], dx

    mov ax, BUFFER_SEG
    mov es, ax
    mov bx, BUFFER_OFF

    mov eax, [fat_sector]
    call read_fs_sector_lba
    jc .fail

    mov bx, [fat_offset]
    mov eax, [es:BUFFER_OFF + bx]
    and eax, 0x0FFFFFFF
    mov [current_cluster], eax

    clc
    jmp .done

.fail:
    stc

.done:
    pop es
    pop dx
    pop bx
    pop ax
    ret

; ------------------------------------------------------------
; Disk read
; ------------------------------------------------------------

; EAX = filesystem-relative LBA
; ES:BX = destination
read_fs_sector_lba:
    add eax, FS_START_LBA
    jmp read_sector_lba

; EAX = absolute disk LBA
; ES:BX = destination
read_sector_lba:
    push eax
    push bx
    push cx
    push dx
    push si
    push di

    mov word [dap + 2], 1
    mov word [dap + 4], bx
    mov word [dap + 6], es
    mov dword [dap + 8], eax
    mov dword [dap + 12], 0

    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc .fail

    clc
    jmp .done

.fail:
    stc

.done:
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop eax
    ret

; ------------------------------------------------------------
; E820 memory map
; ------------------------------------------------------------

collect_e820_map:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push es
    push ds

    xor ax, ax
    mov ds, ax
    mov es, ax
    mov di, E820_MAP_OFF + 4
    mov dword [es:E820_MAP_OFF], 0

    xor ebx, ebx

.next_entry:
    mov eax, 0xE820
    mov edx, 0x534D4150
    mov ecx, 24
    mov dword [es:di + 20], 0
    int 0x15
    jc .fail

    cmp eax, 0x534D4150
    jne .fail

    mov eax, [es:di + 8]
    or eax, [es:di + 12]
    jz .maybe_done

    inc dword [es:E820_MAP_OFF]
    add di, 24

    mov eax, [es:E820_MAP_OFF]
    cmp eax, E820_MAX_ENTRIES
    jae .done

.maybe_done:
    test ebx, ebx
    jnz .next_entry

.done:
    clc
    jmp .exit

.fail:
    stc

.exit:
    pop ds
    pop es
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; ------------------------------------------------------------
; BIOS text print
; ------------------------------------------------------------

print_string:
.next:
    lodsb
    test al, al
    jz .done

    mov ah, 0x0E
    mov bh, 0
    mov bl, 0x07
    int 0x10

    jmp .next

.done:
    ret

; ------------------------------------------------------------
; Protected mode entry
; ------------------------------------------------------------

[bits 32]
protected_mode_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov esp, 0x90000

    ; Copy kernel from low temporary buffer to 1 MiB.
    ; This is now a normal flat protected-mode copy.
    cld
    mov esi, KERNEL_TEMP_PHYS
    mov edi, KERNEL_LOAD_PHYS
    mov ecx, [kernel_file_size]

    test ecx, ecx
    jz .copy_done

    rep movsb

.copy_done:

    jmp KERNEL_ENTRY_PHYS

[bits 16]

; ------------------------------------------------------------
; GDT
; ------------------------------------------------------------

align 8
gdt_start:
    dq 0x0000000000000000
    dq 0x00CF9A000000FFFF
    dq 0x00CF92000000FFFF
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

; ------------------------------------------------------------
; Data
; ------------------------------------------------------------

boot_drive           db 0

bytes_per_sector     dw 512
sectors_per_cluster  db 0
reserved_sectors     dw 0
num_fats             db 0

sectors_per_fat32    dd 0
root_cluster         dd 0

fat_start            dd 0
data_start           dd 0

current_sector       dd 0
current_cluster      dd 0
fat_sector           dd 0
fat_offset           dw 0

kernel_first_cluster dd 0
kernel_file_size     dd 0
kernel_temp_seg      dw 0

kernel_name db 'KERNEL  BIN'

msg_stage2     db 'Stage2: FAT32 loader', 13, 10, 0
msg_search     db 'Searching for KERNEL.BIN...', 13, 10, 0
msg_load       db 'Loading kernel...', 13, 10, 0
msg_jump       db 'Jumping to kernel...', 13, 10, 0
msg_not_found  db 'KERNEL.BIN not found!', 13, 10, 0
msg_disk_error db 'Disk read error!', 13, 10, 0
msg_e820_error db 'E820 memory map failed!', 13, 10, 0

BUFFER_SEG equ 0x1000
BUFFER_OFF equ 0x0000

VBE_INFO_SEG equ 0x1200
VBE_INFO_OFF equ 0x0000

dap:
    db 16
    db 0
    dw 1
    dw 0
    dw 0
    dd 0
    dd 0