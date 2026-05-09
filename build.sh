#!/usr/bin/env bash
set -euo pipefail

# ============================================================
# Config
# ============================================================

BUILD_DIR="build"

BOOT_ASM="boot/boot.asm"
STAGE2_ASM="boot/stage2.asm"

APP_DIR="apps"
APP_LD="$APP_DIR/app.ld"
README_TXT="$BUILD_DIR/README.TXT"

PROCESS_C="kernel/process.c"
PROCESS_H="kernel/process.h"
PROCESS_OBJ="$BUILD_DIR/process.o"

USERCOPY_C="kernel/usercopy.c"
USERCOPY_H="kernel/usercopy.h"
USERCOPY_OBJ="$BUILD_DIR/usercopy.o"

POWER_C="kernel/power.c"
POWER_H="kernel/power.h"
POWER_OBJ="$BUILD_DIR/power.o"

SYSCALL_C="kernel/syscall.c"
SYSCALL_H="kernel/syscall.h"
SYSCALL_OBJ="$BUILD_DIR/syscall.o"

SYSCALL_ASM="kernel/syscall_stub.asm"
SYSCALL_STUB_OBJ="$BUILD_DIR/syscall_stub.o"

USERMODE_ASM="kernel/usermode.asm"
USERMODE_OBJ="$BUILD_DIR/usermode.o"

PAGING_C="kernel/paging.c"
PAGING_H="kernel/paging.h"
PAGING_OBJ="$BUILD_DIR/paging.o"

PMM_C="kernel/pmm.c"
PMM_H="kernel/pmm.h"
PMM_OBJ="$BUILD_DIR/pmm.o"

MEMMAP_C="kernel/memmap.c"
MEMMAP_H="kernel/memmap.h"
MEMMAP_OBJ="$BUILD_DIR/memmap.o"

GDT_C="kernel/gdt.c"
GDT_H="kernel/gdt.h"
GDT_OBJ="$BUILD_DIR/gdt.o"

GDT_FLUSH_ASM="kernel/gdt_flush.asm"
GDT_FLUSH_OBJ="$BUILD_DIR/gdt_flush.o"

EXEC_C="kernel/exec.c"
EXEC_H="kernel/exec.h"
EXEC_OBJ="$BUILD_DIR/exec.o"

TIMER_C="kernel/timer.c"
TIMER_H="kernel/timer.h"
TIMER_OBJ="$BUILD_DIR/timer.o"

ENTRY_ASM="kernel/entry.asm"
ISR_ASM="kernel/isr.asm"
CONTEXT_ASM="kernel/context_switch.asm"
KERNEL_C="kernel/kernel.c"
README_TXT="$BUILD_DIR/README.TXT"
SHELL_C="kernel/shell.c"
LINKER_LD="kernel/linker.ld"

FAT32_C="kernel/fat32.c"
FAT32_H="kernel/fat32.h"
FAT32_OBJ="$BUILD_DIR/fat32.o"

APP_ENTRY_ASM="$APP_DIR/app_entry.asm"
APP_ENTRY_OBJ="$BUILD_DIR/app_entry.o"

KMEM_C="kernel/kmem.c"
KMEM_H="kernel/kmem.h"
KMEM_OBJ="$BUILD_DIR/kmem.o"

IMAGE_C="graphics/image.c"
IMAGE_H="graphics/image.h"
IMAGE_OBJ="$BUILD_DIR/image.o"

SPLASH_PNG="assets/splash.png"
SPLASH_NIMG="$BUILD_DIR/SPLASH.NI"
PYTHON3="${PYTHON3:-python3}"

FB_C="graphics/framebuffer.c"
FB_H="graphics/framebuffer.h"
FB_OBJ="$BUILD_DIR/framebuffer.o"

FONT8X8_C="graphics/font8x8.c"
FONT8X8_H="graphics/font8x8.h"
FONT8X8_OBJ="$BUILD_DIR/font8x8.o"

UEFI_DIR="uefi"
UEFI_C="$UEFI_DIR/bootx64.c"
UEFI_OBJ="$BUILD_DIR/bootx64.o"
UEFI_SO="$BUILD_DIR/bootx64.so"
UEFI_EFI="$BUILD_DIR/BOOTX64.EFI"
UEFI_IMG="$BUILD_DIR/uefi.img"
UEFI_IMG_MB=64

KERNEL64_DIR="kernel64"
KERNEL64_ENTRY_ASM="$KERNEL64_DIR/entry64.asm"
KERNEL64_C="$KERNEL64_DIR/main.c"
KERNEL64_LD="$KERNEL64_DIR/linker.ld"

KERNEL64_ENTRY_OBJ="$BUILD_DIR/entry64.o"
KERNEL64_OBJ="$BUILD_DIR/kernel64.o"
KERNEL64_ELF="$BUILD_DIR/kernel64.elf"
KERNEL64_BIN="$BUILD_DIR/KERNEL64.BIN"

KERNEL64_FONT8X8_OBJ="$BUILD_DIR/font8x8_64.o"

WIN_EXPORT_DIR="${WIN_EXPORT_DIR:-/mnt/c/naqos}"
WIN_EXPORT_IMG="$WIN_EXPORT_DIR/os.img"
WIN_EXPORT_VDI="$WIN_EXPORT_DIR/naqos.vdi"
WIN_VBOXMANAGE="${WIN_VBOXMANAGE:-/mnt/c/Program Files/Oracle/VirtualBox/VBoxManage.exe}"

BOOT_BIN="$BUILD_DIR/boot.bin"
STAGE2_BIN="$BUILD_DIR/stage2.bin"

ENTRY_OBJ="$BUILD_DIR/entry.o"
CONTEXT_OBJ="$BUILD_DIR/context_switch.o"
ISR_OBJ="$BUILD_DIR/isr.o"
KERNEL_OBJ="$BUILD_DIR/kernel.o"
SHELL_OBJ="$BUILD_DIR/shell.o"
KERNEL_ELF="$BUILD_DIR/kernel.elf"
KERNEL_BIN="$BUILD_DIR/kernel.bin"

FS_IMG="$BUILD_DIR/fs.img"
OS_IMG="$BUILD_DIR/os.img"

QEMU64="${QEMU64:-qemu-system-x86_64}"

OVMF_CODE="${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}"
OVMF_VARS_TEMPLATE="${OVMF_VARS_TEMPLATE:-/usr/share/OVMF/OVMF_VARS_4M.fd}"
OVMF_VARS="$BUILD_DIR/OVMF_VARS.fd"

KERNEL64_IDT_C="$KERNEL64_DIR/arch/x86_64/idt64.c"
KERNEL64_ISR_ASM="$KERNEL64_DIR/arch/x86_64/isr64.asm"

KERNEL64_IDT_OBJ="$BUILD_DIR/idt64.o"
KERNEL64_ISR_OBJ="$BUILD_DIR/isr64.o"

KERNEL64_MEMMAP_C="$KERNEL64_DIR/mm/memmap64.c"
KERNEL64_MEMMAP_OBJ="$BUILD_DIR/memmap64.o"

KERNEL64_PMM_C="$KERNEL64_DIR/mm/pmm64.c"
KERNEL64_PMM_OBJ="$BUILD_DIR/pmm64.o"

KERNEL64_PAGING_C="$KERNEL64_DIR/mm/paging64.c"
KERNEL64_PAGING_OBJ="$BUILD_DIR/paging64.o"

KERNEL64_KMEM_C="$KERNEL64_DIR/mm/kmem64.c"
KERNEL64_KMEM_OBJ="$BUILD_DIR/kmem64.o"

KERNEL64_GDT_C="$KERNEL64_DIR/arch/x86_64/gdt64.c"
KERNEL64_GDT_ASM="$KERNEL64_DIR/arch/x86_64/gdt64.asm"

KERNEL64_GDT_OBJ="$BUILD_DIR/gdt64.o"
KERNEL64_GDT_ASM_OBJ="$BUILD_DIR/gdt64_asm.o"

KERNEL64_CONSOLE_C="$KERNEL64_DIR/console/fb_console.c"
KERNEL64_CONSOLE_OBJ="$BUILD_DIR/fb_console64.o"

KERNEL64_IRQ_C="$KERNEL64_DIR/arch/x86_64/irq64.c"
KERNEL64_IRQ_OBJ="$BUILD_DIR/irq64.o"

KERNEL64_INPUT_C="$KERNEL64_DIR/input/kinput64.c"
KERNEL64_INPUT_OBJ="$BUILD_DIR/kinput64.o"

KERNEL64_POWER_C="$KERNEL64_DIR/arch/x86_64/power64.c"
KERNEL64_POWER_OBJ="$BUILD_DIR/power64.o"

KERNEL64_TIMER_C="$KERNEL64_DIR/arch/x86_64/timer64.c"
KERNEL64_TIMER_OBJ="$BUILD_DIR/timer64.o"

KERNEL64_SHELL_C="$KERNEL64_DIR/shell/shell64.c"
KERNEL64_SHELL_OBJ="$BUILD_DIR/shell64.o"

KERNEL64_BLOCK_C="$KERNEL64_DIR/storage/block64.c"
KERNEL64_BLOCK_OBJ="$BUILD_DIR/block64.o"

KERNEL64_FAT32_C="$KERNEL64_DIR/fs/fat32_64.c"
KERNEL64_FAT32_OBJ="$BUILD_DIR/fat32_64.o"

KERNEL64_VFS_C="$KERNEL64_DIR/fs/vfs64.c"
KERNEL64_VFS_OBJ="$BUILD_DIR/vfs64.o"

VBOXMANAGE="${VBOXMANAGE:-VBoxManage}"
VBOX_VM_NAME="${VBOX_VM_NAME:-NAQ OS}"
VBOX_VDI="$BUILD_DIR/naqos.vdi"

# Disk layout
STAGE2_SECTORS=64
FS_START_LBA=65

# Image sizes
FS_IMG_MB=64
OS_IMG_MB=68

# Tools
NASM="${NASM:-nasm}"
GCC="${GCC:-gcc}"
LD="${LD:-ld}"
OBJCOPY="${OBJCOPY:-objcopy}"
QEMU="${QEMU:-qemu-system-i386}"
MKFS_FAT="${MKFS_FAT:-mkfs.fat}"
MCOPY="${MCOPY:-mcopy}"
MDIR="${MDIR:-mdir}"
MMD="${MMD:-mmd}"
BOCHS="${BOCHS:-bochs}"
BOCHS_CFG="$BUILD_DIR/bochsrc.txt"
BOCHS_RC="$BUILD_DIR/bochs.rc"

# Flags
GCC_FLAGS=(
  -m32
  -I.
  -ffreestanding
  -fno-pie
  -fno-stack-protector
  -fno-asynchronous-unwind-tables
  -fno-unwind-tables
  -nostdlib
  -c
)

LD_FLAGS=(
  -m elf_i386
  -z noexecstack
  -T "$LINKER_LD"
)

# ============================================================
# Helpers
# ============================================================

msg() {
  echo
  echo "==> $1"
}

need_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Error: required command not found: $1" >&2
    exit 1
  fi
}

find_ovmf_file() {
  local kind="$1"
  local found=""

  found="$(find /usr/share -type f -name "*OVMF*${kind}*.fd" 2>/dev/null \
    | grep -vi secboot \
    | grep -vi snakeoil \
    | grep -vi '\.ms\.' \
    | head -n 1 || true)"

  if [[ -n "$found" ]]; then
    echo "$found"
    return 0
  fi

  return 1
}

clean() {
  msg "Cleaning build directory"
  rm -rf "$BUILD_DIR"
  mkdir -p "$BUILD_DIR"
}

check_tools() {
  msg "Checking required tools"
  need_cmd "$NASM"
  need_cmd "$GCC"
  need_cmd "$LD"
  need_cmd "$OBJCOPY"
  need_cmd "$MKFS_FAT"
  need_cmd "$MCOPY"
  need_cmd "$MDIR"
  need_cmd "$MMD"
  need_cmd "$QEMU"
  need_cmd "$BOCHS"
}

build_bootloader() {
  msg "Building stage 1 boot sector"
  "$NASM" -f bin "$BOOT_ASM" -o "$BOOT_BIN"

  local boot_size
  boot_size=$(stat -c%s "$BOOT_BIN")
  if [[ "$boot_size" -ne 512 ]]; then
    echo "Error: boot.bin must be exactly 512 bytes, got $boot_size" >&2
    exit 1
  fi

  msg "Building stage 2 loader"
  "$NASM" -f bin "$STAGE2_ASM" -o "$STAGE2_BIN"

  local stage2_target_size=$((STAGE2_SECTORS * 512))

  local stage2_real_size
  stage2_real_size=$(stat -c%s "$STAGE2_BIN")

  if [[ "$stage2_real_size" -gt "$stage2_target_size" ]]; then
    echo "Error: stage2.bin is too large." >&2
    echo "  stage2.bin size      : $stage2_real_size bytes" >&2
    echo "  configured capacity  : $stage2_target_size bytes" >&2
    echo "  STAGE2_SECTORS       : $STAGE2_SECTORS" >&2
    echo >&2
    echo "Increase STAGE2_SECTORS in build.sh and boot/boot.asm," >&2
    echo "then update FS_START_LBA in build.sh and boot/stage2.asm." >&2
    exit 1
  fi

  truncate -s "$stage2_target_size" "$STAGE2_BIN"

  local stage2_size
  stage2_size=$(stat -c%s "$STAGE2_BIN")
  if [[ "$stage2_size" -ne "$stage2_target_size" ]]; then
    echo "Error: stage2.bin size mismatch, expected $stage2_target_size got $stage2_size" >&2
    exit 1
  fi
}

build_kernel() {
  msg "Building kernel entry assembly"
  "$NASM" -f elf32 "$ENTRY_ASM" -o "$ENTRY_OBJ"

  msg "Building ISR assembly"
  "$NASM" -f elf32 "$ISR_ASM" -o "$ISR_OBJ"

  msg "Building context switch assembly"
  "$NASM" -f elf32 "$CONTEXT_ASM" -o "$CONTEXT_OBJ"

  msg "Building GDT/TSS assembly"
  "$NASM" -f elf32 "$GDT_FLUSH_ASM" -o "$GDT_FLUSH_OBJ"

  msg "Building usermode entry assembly"
  "$NASM" -f elf32 "$USERMODE_ASM" -o "$USERMODE_OBJ"

  msg "Building syscall assembly"
  "$NASM" -f elf32 "$SYSCALL_ASM" -o "$SYSCALL_STUB_OBJ"

  msg "Compiling C kernel"
  "$GCC" "${GCC_FLAGS[@]}" "$KERNEL_C" -o "$KERNEL_OBJ"

  msg "Compiling GDT/TSS"
  "$GCC" "${GCC_FLAGS[@]}" "$GDT_C" -o "$GDT_OBJ"

  msg "Compiling shell"
  "$GCC" "${GCC_FLAGS[@]}" "$SHELL_C" -o "$SHELL_OBJ"

  msg "Compiling FAT32"
  "$GCC" "${GCC_FLAGS[@]}" "$FAT32_C" -o "$FAT32_OBJ"

  msg "Compiling process"
  "$GCC" "${GCC_FLAGS[@]}" "$PROCESS_C" -o "$PROCESS_OBJ"

  msg "Compiling power"
  "$GCC" "${GCC_FLAGS[@]}" "$POWER_C" -o "$POWER_OBJ"

  msg "Compiling paging"
  "$GCC" "${GCC_FLAGS[@]}" "$PAGING_C" -o "$PAGING_OBJ"

  msg "Compiling timer"
  "$GCC" "${GCC_FLAGS[@]}" "$TIMER_C" -o "$TIMER_OBJ"

  msg "Compiling usercopy"
 "$GCC" "${GCC_FLAGS[@]}" "$USERCOPY_C" -o "$USERCOPY_OBJ"

  msg "Compiling kmem"
  "$GCC" "${GCC_FLAGS[@]}" "$KMEM_C" -o "$KMEM_OBJ"

  msg "Compiling exec"
  "$GCC" "${GCC_FLAGS[@]}" "$EXEC_C" -o "$EXEC_OBJ"

  msg "Compiling pmm"
  "$GCC" "${GCC_FLAGS[@]}" "$PMM_C" -o "$PMM_OBJ"

  msg "Compiling memmap"
  "$GCC" "${GCC_FLAGS[@]}" "$MEMMAP_C" -o "$MEMMAP_OBJ"

  msg "Compiling syscall"
  "$GCC" "${GCC_FLAGS[@]}" "$SYSCALL_C" -o "$SYSCALL_OBJ"

  msg "Compiling framebuffer"
  "$GCC" "${GCC_FLAGS[@]}" "$FB_C" -o "$FB_OBJ"

  msg "Compiling font8x8"
  "$GCC" "${GCC_FLAGS[@]}" "$FONT8X8_C" -o "$FONT8X8_OBJ"

  msg "Compiling image loader"
  "$GCC" "${GCC_FLAGS[@]}" "$IMAGE_C" -o "$IMAGE_OBJ"

  msg "Linking kernel ELF"
  "$LD" "${LD_FLAGS[@]}" -o "$KERNEL_ELF" \
    "$ENTRY_OBJ" \
    "$ISR_OBJ" \
    "$KERNEL_OBJ" \
    "$SHELL_OBJ" \
    "$FAT32_OBJ" \
    "$POWER_OBJ" \
    "$TIMER_OBJ" \
    "$KMEM_OBJ" \
    "$EXEC_OBJ" \
    "$PMM_OBJ" \
    "$PAGING_OBJ" \
    "$MEMMAP_OBJ" \
    "$PROCESS_OBJ" \
    "$CONTEXT_OBJ" \
    "$GDT_OBJ" \
    "$GDT_FLUSH_OBJ" \
    "$USERMODE_OBJ" \
    "$SYSCALL_OBJ" \
    "$SYSCALL_STUB_OBJ" \
    "$USERCOPY_OBJ" \
    "$FB_OBJ" \
    "$FONT8X8_OBJ" \
    "$IMAGE_OBJ"

  msg "Converting kernel ELF to flat binary"
  "$OBJCOPY" -O binary "$KERNEL_ELF" "$KERNEL_BIN"

  local kernel_size
  kernel_size=$(stat -c%s "$KERNEL_BIN")
  echo "Kernel binary size: $kernel_size bytes"
}

build_kernel64() {
  msg "Building 64-bit kernel entry assembly"
  "$NASM" -f elf64 "$KERNEL64_ENTRY_ASM" -o "$KERNEL64_ENTRY_OBJ"

  msg "Compiling 64-bit kernel"
  "$GCC" \
    -m64 \
    -I. \
    -Iinclude \
    -Igraphics \
    -ffreestanding \
    -fno-pie \
    -fno-stack-protector \
    -mno-red-zone \
    -nostdlib \
    -c "$KERNEL64_C" \
    -o "$KERNEL64_OBJ"

  msg "Compiling 64-bit font8x8"
  "$GCC" \
    -m64 \
    -I. \
    -Iinclude \
    -Igraphics \
    -ffreestanding \
    -fno-pie \
    -fno-stack-protector \
    -mno-red-zone \
    -nostdlib \
    -c "$FONT8X8_C" \
    -o "$KERNEL64_FONT8X8_OBJ"

  msg "Building 64-bit ISR assembly"
  "$NASM" -f elf64 "$KERNEL64_ISR_ASM" -o "$KERNEL64_ISR_OBJ"

  msg "Compiling 64-bit memory map parser"
  "$GCC" \
    -m64 \
    -I. \
    -Iinclude \
    -Ikernel64 \
    -Igraphics \
    -ffreestanding \
    -fno-pie \
    -fno-stack-protector \
    -mno-red-zone \
    -mgeneral-regs-only \
    -nostdlib \
    -c "$KERNEL64_MEMMAP_C" \
    -o "$KERNEL64_MEMMAP_OBJ"

  msg "Compiling 64-bit IDT"
  "$GCC" \
    -m64 \
    -I. \
    -Iinclude \
    -Ikernel64 \
    -Igraphics \
    -ffreestanding \
    -fno-pie \
    -fno-stack-protector \
    -mno-red-zone \
    -mgeneral-regs-only \
    -nostdlib \
    -c "$KERNEL64_IDT_C" \
    -o "$KERNEL64_IDT_OBJ"

  msg "Compiling 64-bit PMM"
  "$GCC" \
    -m64 \
    -I. \
    -Iinclude \
    -Ikernel64 \
    -Igraphics \
    -ffreestanding \
    -fno-pie \
    -fno-stack-protector \
    -mno-red-zone \
    -mgeneral-regs-only \
    -nostdlib \
    -c "$KERNEL64_PMM_C" \
    -o "$KERNEL64_PMM_OBJ"

  msg "Compiling 64-bit paging"
  "$GCC" \
    -m64 \
    -I. \
    -Iinclude \
    -Ikernel64 \
    -Igraphics \
    -ffreestanding \
    -fno-pie \
    -fno-stack-protector \
    -mno-red-zone \
    -mgeneral-regs-only \
    -nostdlib \
    -c "$KERNEL64_PAGING_C" \
    -o "$KERNEL64_PAGING_OBJ"

  msg "Compiling 64-bit kernel heap"
  "$GCC" \
    -m64 \
    -I. \
    -Iinclude \
    -Ikernel64 \
    -Igraphics \
    -ffreestanding \
    -fno-pie \
    -fno-stack-protector \
    -mno-red-zone \
    -mgeneral-regs-only \
    -nostdlib \
    -c "$KERNEL64_KMEM_C" \
    -o "$KERNEL64_KMEM_OBJ"

      msg "Building 64-bit GDT assembly"
  "$NASM" -f elf64 "$KERNEL64_GDT_ASM" -o "$KERNEL64_GDT_ASM_OBJ"

  msg "Compiling 64-bit GDT/TSS"
  "$GCC" \
    -m64 \
    -I. \
    -Iinclude \
    -Ikernel64 \
    -Igraphics \
    -ffreestanding \
    -fno-pie \
    -fno-stack-protector \
    -mno-red-zone \
    -mgeneral-regs-only \
    -nostdlib \
    -c "$KERNEL64_GDT_C" \
    -o "$KERNEL64_GDT_OBJ"

  msg "Compiling 64-bit framebuffer console"
  "$GCC" \
    -m64 \
    -I. \
    -Iinclude \
    -Ikernel64 \
    -Igraphics \
    -ffreestanding \
    -fno-pie \
    -fno-stack-protector \
    -mno-red-zone \
    -mgeneral-regs-only \
    -nostdlib \
    -c "$KERNEL64_CONSOLE_C" \
    -o "$KERNEL64_CONSOLE_OBJ"

  msg "Compiling 64-bit IRQ/PIC/PIT/keyboard"
  "$GCC" \
    -m64 \
    -I. \
    -Iinclude \
    -Ikernel64 \
    -Igraphics \
    -ffreestanding \
    -fno-pie \
    -fno-stack-protector \
    -mno-red-zone \
    -mgeneral-regs-only \
    -nostdlib \
    -c "$KERNEL64_IRQ_C" \
    -o "$KERNEL64_IRQ_OBJ"

  msg "Compiling 64-bit keyboard input queue"
  "$GCC" \
    -m64 \
    -I. \
    -Iinclude \
    -Ikernel64 \
    -Igraphics \
    -ffreestanding \
    -fno-pie \
    -fno-stack-protector \
    -mno-red-zone \
    -mgeneral-regs-only \
    -nostdlib \
    -c "$KERNEL64_INPUT_C" \
    -o "$KERNEL64_INPUT_OBJ"

  msg "Compiling 64-bit power controls"
  "$GCC" \
    -m64 \
    -I. \
    -Iinclude \
    -Ikernel64 \
    -Igraphics \
    -ffreestanding \
    -fno-pie \
    -fno-stack-protector \
    -mno-red-zone \
    -mgeneral-regs-only \
    -nostdlib \
    -c "$KERNEL64_POWER_C" \
    -o "$KERNEL64_POWER_OBJ"

  msg "Compiling 64-bit timer"
  "$GCC" \
    -m64 \
    -I. \
    -Iinclude \
    -Ikernel64 \
    -Igraphics \
    -ffreestanding \
    -fno-pie \
    -fno-stack-protector \
    -mno-red-zone \
    -mgeneral-regs-only \
    -nostdlib \
    -c "$KERNEL64_TIMER_C" \
    -o "$KERNEL64_TIMER_OBJ"

  msg "Compiling 64-bit shell"
  "$GCC" \
    -m64 \
    -I. \
    -Iinclude \
    -Ikernel64 \
    -Igraphics \
    -ffreestanding \
    -fno-pie \
    -fno-stack-protector \
    -mno-red-zone \
    -mgeneral-regs-only \
    -nostdlib \
    -c "$KERNEL64_SHELL_C" \
    -o "$KERNEL64_SHELL_OBJ"

  msg "Compiling 64-bit block storage"
  "$GCC" \
    -m64 \
    -I. \
    -Iinclude \
    -Ikernel64 \
    -Igraphics \
    -ffreestanding \
    -fno-pie \
    -fno-stack-protector \
    -mno-red-zone \
    -mgeneral-regs-only \
    -nostdlib \
    -c "$KERNEL64_BLOCK_C" \
    -o "$KERNEL64_BLOCK_OBJ"

  msg "Compiling 64-bit FAT32 read-only filesystem"
  "$GCC" \
    -m64 \
    -I. \
    -Iinclude \
    -Ikernel64 \
    -Igraphics \
    -ffreestanding \
    -fno-pie \
    -fno-stack-protector \
    -mno-red-zone \
    -mgeneral-regs-only \
    -nostdlib \
    -c "$KERNEL64_FAT32_C" \
    -o "$KERNEL64_FAT32_OBJ"

  msg "Compiling 64-bit VFS"
  "$GCC" \
    -m64 \
    -I. \
    -Iinclude \
    -Ikernel64 \
    -Igraphics \
    -ffreestanding \
    -fno-pie \
    -fno-stack-protector \
    -mno-red-zone \
    -mgeneral-regs-only \
    -nostdlib \
    -c "$KERNEL64_VFS_C" \
    -o "$KERNEL64_VFS_OBJ"

  msg "Linking 64-bit kernel ELF"
  "$LD" \
    -m elf_x86_64 \
    -T "$KERNEL64_LD" \
    -o "$KERNEL64_ELF" \
    "$KERNEL64_ENTRY_OBJ" \
    "$KERNEL64_OBJ" \
    "$KERNEL64_FONT8X8_OBJ" \
    "$KERNEL64_ISR_OBJ" \
    "$KERNEL64_IDT_OBJ" \
    "$KERNEL64_MEMMAP_OBJ" \
    "$KERNEL64_PMM_OBJ" \
    "$KERNEL64_PAGING_OBJ" \
    "$KERNEL64_KMEM_OBJ" \
    "$KERNEL64_GDT_ASM_OBJ" \
    "$KERNEL64_GDT_OBJ" \
    "$KERNEL64_CONSOLE_OBJ" \
    "$KERNEL64_IRQ_OBJ" \
    "$KERNEL64_INPUT_OBJ" \
    "$KERNEL64_POWER_OBJ" \
    "$KERNEL64_TIMER_OBJ" \
    "$KERNEL64_SHELL_OBJ" \
    "$KERNEL64_BLOCK_OBJ" \
    "$KERNEL64_FAT32_OBJ" \
    "$KERNEL64_VFS_OBJ"

  msg "Converting 64-bit kernel ELF to flat binary"
  "$OBJCOPY" -O binary "$KERNEL64_ELF" "$KERNEL64_BIN"

  local kernel64_size
  kernel64_size=$(stat -c%s "$KERNEL64_BIN")
  echo "Kernel64 binary size: $kernel64_size bytes"
}

build_apps() {
  msg "Building app entry assembly"
  "$NASM" -f elf32 "$APP_ENTRY_ASM" -o "$APP_ENTRY_OBJ"

  local found_any=0

  for app_src in "$APP_DIR"/*.c; do
    if [[ ! -e "$app_src" ]]; then
      continue
    fi

    found_any=1

    local app_base
    app_base="$(basename "$app_src" .c)"

    local app_name_upper
    app_name_upper="$(echo "$app_base" | tr '[:lower:]' '[:upper:]')"

    local app_obj="$BUILD_DIR/${app_base}.o"
    local app_elf="$BUILD_DIR/${app_base}.elf"
    # local app_naq="$BUILD_DIR/${app_name_upper}.NAQ"
    local app_elf_upper="$BUILD_DIR/${app_name_upper}.ELF"

    msg "Compiling app: $app_base"
    "$GCC" "${GCC_FLAGS[@]}" "$app_src" -o "$app_obj"

    msg "Linking app: $app_base"
    "$LD" -m elf_i386 -z noexecstack --emit-relocs -T "$APP_LD" -o "$app_elf" \
      "$APP_ENTRY_OBJ" \
      "$app_obj"

    cp "$app_elf" "$app_elf_upper"
    echo "$app_name_upper.ELF size: $(stat -c%s "$app_elf_upper") bytes"

    #msg "Converting app to executable image: $app_name_upper.NAQ"
    #python3 tools/make_naq.py "$app_elf" "$app_naq"

    #echo "$app_name_upper.NAQ size: $(stat -c%s "$app_naq") bytes"
  done

  if [[ "$found_any" -eq 0 ]]; then
    msg "No app source files found in $APP_DIR"
  fi
}

build_filesystem() {
  msg "Creating FAT32 filesystem image (${FS_IMG_MB} MB)"
  rm -f "$FS_IMG"
  dd if=/dev/zero of="$FS_IMG" bs=1M count="$FS_IMG_MB" status=none

  "$MKFS_FAT" -F 32 "$FS_IMG" >/dev/null

  msg "Creating test README.TXT"
  cat > "$README_TXT" <<'EOF'
hello from FAT32
this file was loaded by the kernel shell
EOF

  msg "Copying KERNEL.BIN into FAT32 image"
  "$MCOPY" -i "$FS_IMG" "$KERNEL_BIN" ::KERNEL.BIN

  msg "Creating FAT32 directories"
  mmd -i "$FS_IMG" ::/apps || true
  mmd -i "$FS_IMG" ::/docs || true
  mmd -i "$FS_IMG" ::/boot || true

  if [[ -f "$SPLASH_PNG" ]]; then
    msg "Converting splash image"
    "$PYTHON3" tools/make_nimg.py "$SPLASH_PNG" "$SPLASH_NIMG" 1024 768

    msg "Copying splash image into FAT32 /boot directory"
    "$MCOPY" -i "$FS_IMG" "$SPLASH_NIMG" ::/boot/splash.ni
  else
    echo "Warning: $SPLASH_PNG not found, splash image will use fallback text."
  fi

#  for app_naq in "$BUILD_DIR"/*.NAQ; do
#    if [[ ! -e "$app_naq" ]]; then
#      continue
#    fi

#    app_name="$(basename "$app_naq")"
#    "$MCOPY" -i "$FS_IMG" "$app_naq" "::${app_name}"
#  done

  msg "Copying app executables into FAT32 /apps directory"
  for app_elf in "$BUILD_DIR"/*.ELF; do
    if [[ ! -e "$app_elf" ]]; then
      continue
    fi

    app_name="$(basename "$app_elf")"
    app_lower="$(echo "$app_name" | tr '[:upper:]' '[:lower:]')"

    "$MCOPY" -i "$FS_IMG" "$app_elf" "::/apps/${app_lower}"
  done

  msg "Copying README.TXT into FAT32 /docs directory"
  "$MCOPY" -i "$FS_IMG" "$README_TXT" ::/docs/readme.txt

  "$MDIR" -i "$FS_IMG" ::/ || true
  "$MDIR" -i "$FS_IMG" ::/apps || true
  "$MDIR" -i "$FS_IMG" ::/docs || true
}

find_first_existing() {
  for path in "$@"; do
    if [[ -e "$path" ]]; then
      echo "$path"
      return 0
    fi
  done

  return 1
}

build_disk_image() {
  msg "Creating final disk image (${OS_IMG_MB} MB)"
  rm -f "$OS_IMG"
  dd if=/dev/zero of="$OS_IMG" bs=1M count="$OS_IMG_MB" status=none

  msg "Writing stage 1 to LBA 0"
  dd if="$BOOT_BIN" of="$OS_IMG" conv=notrunc bs=512 seek=0 status=none

  msg "Writing stage 2 to LBA 1"
  dd if="$STAGE2_BIN" of="$OS_IMG" conv=notrunc bs=512 seek=1 status=none

  msg "Writing FAT32 filesystem to LBA ${FS_START_LBA}"
  dd if="$FS_IMG" of="$OS_IMG" conv=notrunc bs=512 seek="$FS_START_LBA" status=none
}

add_uefi_test_files() {
  msg "Adding UEFI test files"

  local docs_dir="$BUILD_DIR/test_docs"
  local readme_file="$docs_dir/readme.txt"

  mkdir -p "$docs_dir"

  cat > "$readme_file" <<'EOF'
Hello from NAQ OS kernel64!

If you can read this with:

  cat /docs/readme.txt

then block64 + FAT32 read-only + shell filesystem commands are working.
EOF

  "$MMD" -i "$UEFI_IMG" ::/docs 2>/dev/null || true
  "$MCOPY" -o -i "$UEFI_IMG" "$readme_file" ::/docs/readme.txt
}

build_uefi() {
  msg "Building UEFI BOOTX64.EFI"

  if [[ ! -f "$UEFI_C" ]]; then
    echo "Error: missing $UEFI_C" >&2
    exit 1
  fi

  local gnuefi_crt
  local gnuefi_lds
  local gnuefi_lib_dir

  gnuefi_crt="$(find_first_existing \
    /usr/lib/crt0-efi-x86_64.o \
    /usr/lib/x86_64-linux-gnu/gnuefi/crt0-efi-x86_64.o \
    /usr/lib64/gnuefi/crt0-efi-x86_64.o \
  )" || {
    echo "Error: could not find crt0-efi-x86_64.o" >&2
    echo "Install GNU-EFI with: sudo apt install gnu-efi" >&2
    exit 1
  }

  gnuefi_lds="$(find_first_existing \
    /usr/lib/elf_x86_64_efi.lds \
    /usr/lib/x86_64-linux-gnu/gnuefi/elf_x86_64_efi.lds \
    /usr/lib64/gnuefi/elf_x86_64_efi.lds \
  )" || {
    echo "Error: could not find elf_x86_64_efi.lds" >&2
    echo "Install GNU-EFI with: sudo apt install gnu-efi" >&2
    exit 1
  }

  gnuefi_lib_dir="$(dirname "$gnuefi_crt")"

  "$GCC" \
    -Iinclude \
    -I/usr/include/efi \
    -I/usr/include/efi/x86_64 \
    -fpic \
    -ffreestanding \
    -fno-stack-protector \
    -fno-stack-check \
    -fshort-wchar \
    -mno-red-zone \
    -maccumulate-outgoing-args \
    -c "$UEFI_C" \
    -o "$UEFI_OBJ"

  "$LD" \
    -nostdlib \
    -znocombreloc \
    -T "$gnuefi_lds" \
    -shared \
    -Bsymbolic \
    "$gnuefi_crt" \
    "$UEFI_OBJ" \
    -L "$gnuefi_lib_dir" \
    -lefi \
    -lgnuefi \
    -o "$UEFI_SO"

  "$OBJCOPY" \
    -j .text \
    -j .sdata \
    -j .data \
    -j .dynamic \
    -j .dynsym \
    -j .rel \
    -j .rela \
    -j .reloc \
    --target=efi-app-x86_64 \
    "$UEFI_SO" \
    "$UEFI_EFI"

  echo "UEFI app size: $(stat -c%s "$UEFI_EFI") bytes"
}

build_uefi_image() {
  msg "Creating UEFI FAT image (${UEFI_IMG_MB} MB)"

  rm -f "$UEFI_IMG"

  dd if=/dev/zero of="$UEFI_IMG" bs=1M count="$UEFI_IMG_MB" status=none
  "$MKFS_FAT" -F 32 "$UEFI_IMG" >/dev/null

  "$MMD" -i "$UEFI_IMG" ::/EFI || true
  "$MMD" -i "$UEFI_IMG" ::/EFI/BOOT || true

  "$MCOPY" -i "$UEFI_IMG" "$UEFI_EFI" ::/EFI/BOOT/BOOTX64.EFI

  add_uefi_test_files

  if [[ -f "$KERNEL64_BIN" ]]; then
    msg "Copying KERNEL64.BIN into UEFI image"
    "$MCOPY" -i "$UEFI_IMG" "$KERNEL64_BIN" ::KERNEL64.BIN
  else
    echo "Warning: $KERNEL64_BIN not found; UEFI loader will not find KERNEL64.BIN."
  fi

  "$MDIR" -i "$UEFI_IMG" ::/ || true
  "$MDIR" -i "$UEFI_IMG" ::/EFI/BOOT || true

  echo "Created: $UEFI_IMG"
}

show_summary() {
  msg "Build summary"
  echo "boot.bin      : $(stat -c%s "$BOOT_BIN") bytes"
  echo "stage2.bin    : $(stat -c%s "$STAGE2_BIN") bytes"
  echo "kernel.bin    : $(stat -c%s "$KERNEL_BIN") bytes"
  echo "fs.img        : $(stat -c%s "$FS_IMG") bytes"
  echo "os.img        : $(stat -c%s "$OS_IMG") bytes"
  echo
  echo "Disk layout:"
  echo "  LBA 0        -> stage1 boot sector"
  echo "  LBA 1-$STAGE2_SECTORS     -> stage2 loader"
  echo "  LBA ${FS_START_LBA}+      -> FAT32 filesystem containing KERNEL.BIN"
}

run_qemu() {
  local display_backend="${QEMU_DISPLAY:-gtk}"

  "$QEMU" \
    -vga std \
    -display "$display_backend" \
    -monitor none \
    -serial none \
    -drive format=raw,file="$OS_IMG"
}

run_qemu_uefi() {
  msg "Running UEFI image in QEMU"

  if ! command -v "$QEMU64" >/dev/null 2>&1; then
    echo "Error: $QEMU64 not found." >&2
    echo "Install it with:" >&2
    echo "  sudo apt install qemu-system-x86" >&2
    exit 1
  fi

  if [[ ! -f "$UEFI_IMG" ]]; then
    echo "Error: $UEFI_IMG does not exist." >&2
    echo "Build it first with:" >&2
    echo "  ./build.sh uefi" >&2
    exit 1
  fi

  if [[ ! -f "$OVMF_CODE" ]]; then
    local detected_code
    detected_code="$(find_ovmf_file CODE)" || {
      echo "Error: could not find OVMF_CODE fd file." >&2
      echo "Install OVMF with:" >&2
      echo "  sudo apt install ovmf" >&2
      exit 1
    }

    OVMF_CODE="$detected_code"
  fi

  if [[ ! -f "$OVMF_VARS_TEMPLATE" ]]; then
    local detected_vars
    detected_vars="$(find_ovmf_file VARS)" || {
      echo "Error: could not find OVMF_VARS fd file." >&2
      echo "Install OVMF with:" >&2
      echo "  sudo apt install ovmf" >&2
      exit 1
    }

    OVMF_VARS_TEMPLATE="$detected_vars"
  fi

  mkdir -p "$BUILD_DIR"
  cp "$OVMF_VARS_TEMPLATE" "$OVMF_VARS"

  echo "OVMF_CODE: $OVMF_CODE"
  echo "OVMF_VARS: $OVMF_VARS"
  echo "UEFI_IMG : $UEFI_IMG"

  "$QEMU64" \
    -machine pc \
    -m 256M \
    -cpu qemu64 \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$OVMF_VARS" \
    -drive if=ide,format=raw,file="$UEFI_IMG" \
    -vga std \
    -monitor none \
    -serial none \
    -net none
}

build_vbox_disk() {
  msg "Converting raw OS image to VirtualBox VDI"

  if ! command -v "$VBOXMANAGE" >/dev/null 2>&1; then
    echo "Error: VBoxManage not found." >&2
    echo "If you're using WSL, you may need to run VBoxManage from Windows PowerShell." >&2
    exit 1
  fi

  rm -f "$VBOX_VDI"

  "$VBOXMANAGE" convertfromraw "$OS_IMG" "$VBOX_VDI" --format VDI

  echo "Created: $VBOX_VDI"
}

create_or_update_vbox_vm() {
  msg "Creating/updating VirtualBox VM"

  if ! command -v "$VBOXMANAGE" >/dev/null 2>&1; then
    echo "Error: VBoxManage not found." >&2
    exit 1
  fi

  if ! "$VBOXMANAGE" showvminfo "$VBOX_VM_NAME" >/dev/null 2>&1; then
    "$VBOXMANAGE" createvm \
      --name "$VBOX_VM_NAME" \
      --ostype Other \
      --register

    "$VBOXMANAGE" modifyvm "$VBOX_VM_NAME" \
      --memory 256 \
      --vram 32 \
      --firmware bios \
      --graphicscontroller vboxvga \
      --boot1 disk \
      --boot2 none \
      --boot3 none \
      --boot4 none \
      --audio none \
      --usb off \
      --nic1 none

    "$VBOXMANAGE" storagectl "$VBOX_VM_NAME" \
      --name "IDE Controller" \
      --add ide \
      --controller PIIX4
  fi

  "$VBOXMANAGE" storageattach "$VBOX_VM_NAME" \
    --storagectl "IDE Controller" \
    --port 0 \
    --device 0 \
    --medium none >/dev/null 2>&1 || true

  "$VBOXMANAGE" storageattach "$VBOX_VM_NAME" \
    --storagectl "IDE Controller" \
    --port 0 \
    --device 0 \
    --type hdd \
    --medium "$VBOX_VDI"
}

run_vbox() {
  build_vbox_disk
  create_or_update_vbox_vm

  msg "Starting VirtualBox VM"
  "$VBOXMANAGE" startvm "$VBOX_VM_NAME"
}


usage() {
  cat <<EOF
Usage: ./build.sh [command]

Commands:
  build     Clean and build everything
  run       Build everything and boot in QEMU
  clean     Remove build directory
  fs        Build only filesystem image (after kernel build)
  kernel    Build only kernel
  boot      Build only stage1 + stage2
  image     Build final disk image (after boot + kernel + fs)
  vbox-win  Build and export a Windows VirtualBox VDI to C:\naqos
  uefi      Build UEFI BOOTX64.EFI and uefi.img
  qemu      Run in QEMU
  uefi-qemu Build UEFI image and run it in QEMU/OVMF
  help      Show this help

Environment:
  QEMU_DISPLAY=gtk|sdl|curses|none  Choose QEMU display backend (default: gtk)

Default:
  build
EOF
}

export_vbox_windows() {
  msg "Exporting VirtualBox image for Windows"

  if [[ ! -f "$OS_IMG" ]]; then
    echo "Error: $OS_IMG does not exist. Build the disk image first." >&2
    exit 1
  fi

  mkdir -p "$WIN_EXPORT_DIR"

  rm -f "$WIN_EXPORT_IMG"
  rm -f "$WIN_EXPORT_VDI"

  msg "Copying raw image to Windows-accessible folder"
  cp "$OS_IMG" "$WIN_EXPORT_IMG"

  if [[ -x "$WIN_VBOXMANAGE" ]]; then
    msg "Converting raw image to VDI using Windows VBoxManage"

    "$WIN_VBOXMANAGE" convertfromraw \
      "C:\\naqos\\os.img" \
      "C:\\naqos\\naqos.vdi" \
      --format VDI

    echo
    echo "Created:"
    echo "  C:\\naqos\\naqos.vdi"
    echo
    echo "Attach this VDI in VirtualBox with:"
    echo "  Firmware: BIOS"
    echo "  Graphics Controller: VBoxVGA"
    echo "  Video Memory: 32 MB"
    echo "  RAM: 256 MB"
  else
    echo
    echo "Copied raw image to:"
    echo "  $WIN_EXPORT_IMG"
    echo
    echo "Could not find executable VBoxManage at:"
    echo "  $WIN_VBOXMANAGE"
    echo
    echo "Run this from Windows PowerShell:"
    echo
    echo '  & "C:\Program Files\Oracle\VirtualBox\VBoxManage.exe" convertfromraw "C:\naqos\os.img" "C:\naqos\naqos.vdi" --format VDI'
    echo
  fi
}

# ============================================================
# Main
# ============================================================

cmd="${1:-build}"

case "$cmd" in
  build)
    check_tools
    clean
    build_bootloader
    build_kernel
    build_apps
    build_filesystem
    build_disk_image
    show_summary
    ;;
  run)
    check_tools
    clean
    build_bootloader
    build_kernel
    build_apps
    build_filesystem
    build_disk_image
    show_summary
    run_qemu
    ;;
  clean)
    clean
    ;;
  boot)
    check_tools
    mkdir -p "$BUILD_DIR"
    build_bootloader
    ;;
  qemu)
    run_qemu
    ;;
  uefi-qemu)
    check_tools
    clean
    build_kernel64
    build_uefi
    build_uefi_image
    run_qemu_uefi
    ;;
  vdi)
    check_tools
    clean
    build_bootloader
    build_kernel
    build_apps
    build_filesystem
    build_disk_image
    show_summary
    build_vbox_disk
    ;;
  uefi)
    check_tools
    clean
    build_kernel64
    build_uefi
    build_uefi_image
    ;;
  vbox)
    check_tools
    clean
    build_bootloader
    build_kernel
    build_apps
    build_filesystem
    build_disk_image
    show_summary
    run_vbox
    ;;
  vbox-win)
    check_tools
    clean
    build_bootloader
    build_kernel
    build_apps
    build_filesystem
    build_disk_image
    show_summary
    export_vbox_windows
    ;;
  kernel)
    check_tools
    mkdir -p "$BUILD_DIR"
    build_kernel
    ;;
  fs)
    check_tools
    mkdir -p "$BUILD_DIR"
    build_filesystem
    ;;
  apps)
    check_tools
    mkdir -p "$BUILD_DIR"
    build_apps
    ;;
  image)
    check_tools
    mkdir -p "$BUILD_DIR"
    build_disk_image
    show_summary
    ;;
  help|-h|--help)
    usage
    ;;
  *)
    echo "Unknown command: $cmd" >&2
    usage
    exit 1
    ;;
esac
