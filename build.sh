#!/usr/bin/env bash
set -euo pipefail

# ============================================================
# Config
# ============================================================

BUILD_DIR="${BUILD_DIR:-build}"

# ------------------------------------------------------------
# Legacy BIOS / 32-bit paths
# ------------------------------------------------------------

BOOT_ASM="boot/boot.asm"
STAGE2_ASM="boot/stage2.asm"

BOOT_BIN="$BUILD_DIR/boot.bin"
STAGE2_BIN="$BUILD_DIR/stage2.bin"

KERNEL_C="kernel/kernel.c"
SHELL_C="kernel/shell.c"
ENTRY_ASM="kernel/entry.asm"
ISR_ASM="kernel/isr.asm"
CONTEXT_ASM="kernel/context_switch.asm"
LINKER_LD="kernel/linker.ld"

ENTRY_OBJ="$BUILD_DIR/entry.o"
ISR_OBJ="$BUILD_DIR/isr.o"
CONTEXT_OBJ="$BUILD_DIR/context_switch.o"
KERNEL_OBJ="$BUILD_DIR/kernel.o"
SHELL_OBJ="$BUILD_DIR/shell.o"
KERNEL_ELF="$BUILD_DIR/kernel.elf"
KERNEL_BIN="$BUILD_DIR/kernel.bin"

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

FAT32_C="kernel/fat32.c"
FAT32_H="kernel/fat32.h"
FAT32_OBJ="$BUILD_DIR/fat32.o"

KMEM_C="kernel/kmem.c"
KMEM_H="kernel/kmem.h"
KMEM_OBJ="$BUILD_DIR/kmem.o"

FB_C="graphics/framebuffer.c"
FB_H="graphics/framebuffer.h"
FB_OBJ="$BUILD_DIR/framebuffer.o"

FONT8X8_C="graphics/font8x8.c"
FONT8X8_H="graphics/font8x8.h"
FONT8X8_OBJ="$BUILD_DIR/font8x8.o"

IMAGE_C="graphics/image.c"
IMAGE_H="graphics/image.h"
IMAGE_OBJ="$BUILD_DIR/image.o"

APP_DIR="apps"
APP_LD="$APP_DIR/app.ld"
APP_ENTRY_ASM="$APP_DIR/app_entry.asm"
APP_ENTRY_OBJ="$BUILD_DIR/app_entry.o"

README_TXT="$BUILD_DIR/README.TXT"

FS_IMG="$BUILD_DIR/fs.img"
OS_IMG="$BUILD_DIR/os.img"

STAGE2_SECTORS=64
FS_START_LBA=65

FS_IMG_MB=64
OS_IMG_MB=68

# ------------------------------------------------------------
# UEFI loader paths
# ------------------------------------------------------------

UEFI_DIR="uefi"
UEFI_C="$UEFI_DIR/bootx64.c"
UEFI_OBJ="$BUILD_DIR/bootx64.o"
UEFI_SO="$BUILD_DIR/bootx64.so"
UEFI_EFI="$BUILD_DIR/BOOTX64.EFI"
UEFI_IMG="$BUILD_DIR/uefi.img"
UEFI_IMG_MB="${UEFI_IMG_MB:-64}"

# ------------------------------------------------------------
# 64-bit kernel paths
# ------------------------------------------------------------

KERNEL64_DIR="kernel64"

KERNEL64_ENTRY_ASM="$KERNEL64_DIR/entry64.asm"
KERNEL64_C="$KERNEL64_DIR/main.c"
KERNEL64_LD="$KERNEL64_DIR/linker.ld"

KERNEL64_ENTRY_OBJ="$BUILD_DIR/entry64.o"
KERNEL64_OBJ="$BUILD_DIR/kernel64.o"
KERNEL64_ELF="$BUILD_DIR/kernel64.elf"
KERNEL64_BIN="$BUILD_DIR/KERNEL64.BIN"

KERNEL64_FONT8X8_OBJ="$BUILD_DIR/font8x8_64.o"

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

KERNEL64_VMEM_C="$KERNEL64_DIR/mm/vmem64.c"
KERNEL64_VMEM_OBJ="$BUILD_DIR/vmem64.o"

KERNEL64_USERCOPY_C="$KERNEL64_DIR/mm/usercopy64.c"
KERNEL64_USERCOPY_OBJ="$BUILD_DIR/usercopy64.o"

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

KERNEL64_SYSCALL_C="$KERNEL64_DIR/arch/x86_64/syscall64.c"
KERNEL64_SYSCALL_ASM="$KERNEL64_DIR/arch/x86_64/syscall64.asm"
KERNEL64_SYSCALL_OBJ="$BUILD_DIR/syscall64.o"
KERNEL64_SYSCALL_ASM_OBJ="$BUILD_DIR/syscall64_asm.o"

KERNEL64_USERMODE_C="$KERNEL64_DIR/arch/x86_64/usermode64.c"
KERNEL64_USERMODE_ASM="$KERNEL64_DIR/arch/x86_64/usermode64.asm"
KERNEL64_USERMODE_OBJ="$BUILD_DIR/usermode64.o"
KERNEL64_USERMODE_ASM_OBJ="$BUILD_DIR/usermode64_asm.o"

KERNEL64_CONTEXT_SWITCH_ASM="$KERNEL64_DIR/arch/x86_64/context_switch64.asm"
KERNEL64_CONTEXT_SWITCH_OBJ="$BUILD_DIR/context_switch64.o"

KERNEL64_SHELL_C="$KERNEL64_DIR/shell/shell64.c"
KERNEL64_SHELL_OBJ="$BUILD_DIR/shell64.o"

KERNEL64_BLOCK_C="$KERNEL64_DIR/storage/block64.c"
KERNEL64_BLOCK_OBJ="$BUILD_DIR/block64.o"

KERNEL64_FAT32_C="$KERNEL64_DIR/fs/fat32_64.c"
KERNEL64_FAT32_OBJ="$BUILD_DIR/fat32_64.o"

KERNEL64_VFS_C="$KERNEL64_DIR/fs/vfs64.c"
KERNEL64_VFS_OBJ="$BUILD_DIR/vfs64.o"

KERNEL64_INITRD_C="$KERNEL64_DIR/fs/initrd64.c"
KERNEL64_INITRD_OBJ="$BUILD_DIR/initrd64.o"

KERNEL64_FB64_C="$KERNEL64_DIR/graphics/framebuffer64.c"
KERNEL64_FB64_OBJ="$BUILD_DIR/framebuffer64.o"

KERNEL64_IMAGE_C="$KERNEL64_DIR/graphics/image64.c"
KERNEL64_IMAGE_OBJ="$BUILD_DIR/image64.o"

KERNEL64_PROCESS_C="$KERNEL64_DIR/proc/process64.c"
KERNEL64_PROCESS_OBJ="$BUILD_DIR/process64.o"

KERNEL64_ELF_C="$KERNEL64_DIR/exec/elf64.c"
KERNEL64_ELF_OBJ="$BUILD_DIR/elf64.o"

KERNEL64_SAFE_MODE_C="$KERNEL64_DIR/safety/safe_mode64.c"
KERNEL64_SAFE_MODE_OBJ="$BUILD_DIR/safe_mode64.o"

KERNEL64_BOOTLOG_C="$KERNEL64_DIR/debug/bootlog64.c"
KERNEL64_BOOTLOG_OBJ="$BUILD_DIR/bootlog64.o"

KERNEL64_BOOTDIAG_C="$KERNEL64_DIR/debug/boot_diag64.c"
KERNEL64_BOOTDIAG_OBJ="$BUILD_DIR/boot_diag64.o"

# ------------------------------------------------------------
# Assets / initrd / user apps
# ------------------------------------------------------------

ASSETS_FS_DIR="${ASSETS_FS_DIR:-assets/fs}"
ASSETS_BUILD_DIR="$BUILD_DIR/assets_fs"
NIMG_TOOL="${NIMG_TOOL:-tools/make_nimg.py}"

COPY_ASSETS_TO_UEFI="${COPY_ASSETS_TO_UEFI:-0}"

SPLASH_NIMG_WIDTH="${SPLASH_NIMG_WIDTH:-512}"
SPLASH_NIMG_HEIGHT="${SPLASH_NIMG_HEIGHT:-384}"

USERAPPS_DIR="${USERAPPS_DIR:-userapps}"
USERAPPS_BUILD_DIR="$BUILD_DIR/userapps"
USERAPP_LINKER="$USERAPPS_DIR/userapp.ld"

INITRD_TOOL="${INITRD_TOOL:-tools/make_initrd.py}"
INITRD_IMG="$BUILD_DIR/INITRD.NAQ"
INITRD_FS_DIR="$BUILD_DIR/initrd_fs"

# ------------------------------------------------------------
# QEMU / OVMF / VirtualBox
# ------------------------------------------------------------

QEMU="${QEMU:-qemu-system-i386}"
QEMU64="${QEMU64:-qemu-system-x86_64}"

OVMF_CODE="${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}"
OVMF_VARS_TEMPLATE="${OVMF_VARS_TEMPLATE:-/usr/share/OVMF/OVMF_VARS_4M.fd}"
OVMF_VARS="$BUILD_DIR/OVMF_VARS.fd"

VBOXMANAGE="${VBOXMANAGE:-VBoxManage}"
VBOX_VM_NAME="${VBOX_VM_NAME:-NAQ OS}"
VBOX_VDI="$BUILD_DIR/naqos.vdi"

WIN_EXPORT_DIR="${WIN_EXPORT_DIR:-/mnt/c/naqos}"
WIN_EXPORT_IMG="$WIN_EXPORT_DIR/os.img"
WIN_EXPORT_VDI="$WIN_EXPORT_DIR/naqos.vdi"
WIN_VBOXMANAGE="${WIN_VBOXMANAGE:-/mnt/c/Program Files/Oracle/VirtualBox/VBoxManage.exe}"

# ------------------------------------------------------------
# Hardware-safe release
# ------------------------------------------------------------

NAQ_SAFE_HW_DEFAULT="${NAQ_SAFE_HW_DEFAULT:-1}"
NAQ_BOOT_VERBOSE="${NAQ_BOOT_VERBOSE:-0}"

USB_DRIVE="${USB_DRIVE:-}"
HARDWARE_SAFE_DIR="$BUILD_DIR/hardware-safe"

# ------------------------------------------------------------
# Tools
# ------------------------------------------------------------

NASM="${NASM:-nasm}"
GCC="${GCC:-gcc}"
LD="${LD:-ld}"
OBJCOPY="${OBJCOPY:-objcopy}"
PYTHON3="${PYTHON3:-python3}"
MKFS_FAT="${MKFS_FAT:-mkfs.fat}"
MCOPY="${MCOPY:-mcopy}"
MDIR="${MDIR:-mdir}"
MMD="${MMD:-mmd}"
BOCHS="${BOCHS:-bochs}"

BOCHS_CFG="$BUILD_DIR/bochsrc.txt"
BOCHS_RC="$BUILD_DIR/bochs.rc"

# ------------------------------------------------------------
# Flags
# ------------------------------------------------------------

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

GCC64_FLAGS=(
  -m64
  -I.
  -Iinclude
  -Ikernel64
  -Igraphics
  -ffreestanding
  -fno-pie
  -fno-stack-protector
  -mno-red-zone
  -mgeneral-regs-only
  -nostdlib
  -c
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

require_file() {
  local path="$1"
  local description="$2"

  if [[ ! -f "$path" ]]; then
    echo "Error: missing $description: $path" >&2
    exit 1
  fi
}

copy_to_usb_windows() {
  local src="$1"
  local dst="$2"

  local src_win
  src_win="$(wslpath -w "$src")"

  powershell.exe -NoProfile -Command "Copy-Item -LiteralPath '$src_win' -Destination '$dst' -Force"
}

compile64_c() {
  local label="$1"
  local src="$2"
  local obj="$3"
  shift 3

  msg "$label"
  "$GCC" "${GCC64_FLAGS[@]}" "$@" "$src" -o "$obj"
}

assemble64() {
  local label="$1"
  local src="$2"
  local obj="$3"

  msg "$label"
  "$NASM" -f elf64 "$src" -o "$obj"
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

check_tools_common() {
  msg "Checking required tools"

  need_cmd "$NASM"
  need_cmd "$GCC"
  need_cmd "$LD"
  need_cmd "$OBJCOPY"
  need_cmd "$PYTHON3"
  need_cmd "$MKFS_FAT"
  need_cmd "$MCOPY"
  need_cmd "$MDIR"
  need_cmd "$MMD"
}

check_tools() {
  check_tools_common
}

check_tools_qemu() {
  check_tools_common
  need_cmd "$QEMU"
}

check_tools_uefi() {
  check_tools_common
}

check_tools_uefi_qemu() {
  check_tools_common
  need_cmd "$QEMU64"
}

# ============================================================
# Legacy BIOS / 32-bit build
# ============================================================

build_bootloader() {
  msg "Building stage 1 boot sector"
  "$NASM" -f bin "$BOOT_ASM" -o "$BOOT_BIN"

  local boot_size
  boot_size="$(stat -c%s "$BOOT_BIN")"

  if [[ "$boot_size" -ne 512 ]]; then
    echo "Error: boot.bin must be exactly 512 bytes, got $boot_size" >&2
    exit 1
  fi

  msg "Building stage 2 loader"
  "$NASM" -f bin "$STAGE2_ASM" -o "$STAGE2_BIN"

  local stage2_target_size=$((STAGE2_SECTORS * 512))
  local stage2_real_size
  stage2_real_size="$(stat -c%s "$STAGE2_BIN")"

  if [[ "$stage2_real_size" -gt "$stage2_target_size" ]]; then
    echo "Error: stage2.bin is too large." >&2
    echo "  stage2.bin size      : $stage2_real_size bytes" >&2
    echo "  configured capacity  : $stage2_target_size bytes" >&2
    echo "  STAGE2_SECTORS       : $STAGE2_SECTORS" >&2
    exit 1
  fi

  truncate -s "$stage2_target_size" "$STAGE2_BIN"
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

  echo "Kernel binary size: $(stat -c%s "$KERNEL_BIN") bytes"
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
    local app_elf_upper="$BUILD_DIR/${app_name_upper}.ELF"

    msg "Compiling app: $app_base"
    "$GCC" "${GCC_FLAGS[@]}" "$app_src" -o "$app_obj"

    msg "Linking app: $app_base"
    "$LD" -m elf_i386 -z noexecstack --emit-relocs -T "$APP_LD" -o "$app_elf" \
      "$APP_ENTRY_OBJ" \
      "$app_obj"

    cp "$app_elf" "$app_elf_upper"

    echo "$app_name_upper.ELF size: $(stat -c%s "$app_elf_upper") bytes"
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
  "$MMD" -i "$FS_IMG" ::/apps 2>/dev/null || true
  "$MMD" -i "$FS_IMG" ::/docs 2>/dev/null || true
  "$MMD" -i "$FS_IMG" ::/boot 2>/dev/null || true

  msg "Copying app executables into FAT32 /apps directory"
  for app_elf in "$BUILD_DIR"/*.ELF; do
    if [[ ! -e "$app_elf" ]]; then
      continue
    fi

    local app_name
    app_name="$(basename "$app_elf")"

    local app_lower
    app_lower="$(echo "$app_name" | tr '[:upper:]' '[:lower:]')"

    "$MCOPY" -i "$FS_IMG" "$app_elf" "::/apps/${app_lower}"
  done

  msg "Copying README.TXT into FAT32 /docs directory"
  "$MCOPY" -i "$FS_IMG" "$README_TXT" ::/docs/readme.txt

  "$MDIR" -i "$FS_IMG" ::/ || true
  "$MDIR" -i "$FS_IMG" ::/apps || true
  "$MDIR" -i "$FS_IMG" ::/docs || true
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

show_summary() {
  msg "Build summary"

  echo "boot.bin      : $(stat -c%s "$BOOT_BIN") bytes"
  echo "stage2.bin    : $(stat -c%s "$STAGE2_BIN") bytes"
  echo "kernel.bin    : $(stat -c%s "$KERNEL_BIN") bytes"
  echo "fs.img        : $(stat -c%s "$FS_IMG") bytes"
  echo "os.img        : $(stat -c%s "$OS_IMG") bytes"
  echo
  echo "Disk layout:"
  echo "  LBA 0              -> stage1 boot sector"
  echo "  LBA 1-$STAGE2_SECTORS       -> stage2 loader"
  echo "  LBA ${FS_START_LBA}+            -> FAT32 filesystem containing KERNEL.BIN"
}

# ============================================================
# 64-bit kernel build
# ============================================================

build_kernel64() {
  assemble64 "Building 64-bit kernel entry assembly" "$KERNEL64_ENTRY_ASM" "$KERNEL64_ENTRY_OBJ"

  compile64_c "Compiling 64-bit kernel" "$KERNEL64_C" "$KERNEL64_OBJ"
  compile64_c "Compiling 64-bit font8x8" "$FONT8X8_C" "$KERNEL64_FONT8X8_OBJ"

  assemble64 "Building 64-bit ISR assembly" "$KERNEL64_ISR_ASM" "$KERNEL64_ISR_OBJ"

  compile64_c "Compiling 64-bit memory map parser" "$KERNEL64_MEMMAP_C" "$KERNEL64_MEMMAP_OBJ"
  compile64_c "Compiling 64-bit IDT" "$KERNEL64_IDT_C" "$KERNEL64_IDT_OBJ"
  compile64_c "Compiling 64-bit PMM" "$KERNEL64_PMM_C" "$KERNEL64_PMM_OBJ"
  compile64_c "Compiling 64-bit paging" "$KERNEL64_PAGING_C" "$KERNEL64_PAGING_OBJ"
  compile64_c "Compiling 64-bit kernel heap" "$KERNEL64_KMEM_C" "$KERNEL64_KMEM_OBJ"
  compile64_c "Compiling 64-bit virtual memory layout" "$KERNEL64_VMEM_C" "$KERNEL64_VMEM_OBJ"

  assemble64 "Building 64-bit GDT assembly" "$KERNEL64_GDT_ASM" "$KERNEL64_GDT_ASM_OBJ"
  compile64_c "Compiling 64-bit GDT/TSS" "$KERNEL64_GDT_C" "$KERNEL64_GDT_OBJ"

  compile64_c "Compiling 64-bit framebuffer console" "$KERNEL64_CONSOLE_C" "$KERNEL64_CONSOLE_OBJ"
  compile64_c "Compiling 64-bit IRQ/PIC/PIT/keyboard" "$KERNEL64_IRQ_C" "$KERNEL64_IRQ_OBJ"
  compile64_c "Compiling 64-bit keyboard input queue" "$KERNEL64_INPUT_C" "$KERNEL64_INPUT_OBJ"
  compile64_c "Compiling 64-bit power controls" "$KERNEL64_POWER_C" "$KERNEL64_POWER_OBJ"
  compile64_c "Compiling 64-bit timer" "$KERNEL64_TIMER_C" "$KERNEL64_TIMER_OBJ"

  compile64_c "Compiling 64-bit safe hardware mode" "$KERNEL64_SAFE_MODE_C" "$KERNEL64_SAFE_MODE_OBJ" \
    -D "NAQ_SAFE_HW_DEFAULT=${NAQ_SAFE_HW_DEFAULT:-1}"

  compile64_c "Compiling 64-bit boot logging" "$KERNEL64_BOOTLOG_C" "$KERNEL64_BOOTLOG_OBJ" \
    -D "NAQ_BOOT_VERBOSE=${NAQ_BOOT_VERBOSE:-0}"

  compile64_c "Compiling 64-bit boot diagnostics" "$KERNEL64_BOOTDIAG_C" "$KERNEL64_BOOTDIAG_OBJ"

  compile64_c "Compiling 64-bit block storage" "$KERNEL64_BLOCK_C" "$KERNEL64_BLOCK_OBJ"
  compile64_c "Compiling 64-bit FAT32 filesystem" "$KERNEL64_FAT32_C" "$KERNEL64_FAT32_OBJ"
  compile64_c "Compiling 64-bit VFS" "$KERNEL64_VFS_C" "$KERNEL64_VFS_OBJ"
  compile64_c "Compiling 64-bit initrd filesystem" "$KERNEL64_INITRD_C" "$KERNEL64_INITRD_OBJ"

  compile64_c "Compiling 64-bit framebuffer graphics" "$KERNEL64_FB64_C" "$KERNEL64_FB64_OBJ"
  compile64_c "Compiling 64-bit image/NIMG loader" "$KERNEL64_IMAGE_C" "$KERNEL64_IMAGE_OBJ"

  assemble64 "Assembling 64-bit context switch" "$KERNEL64_CONTEXT_SWITCH_ASM" "$KERNEL64_CONTEXT_SWITCH_OBJ"
  compile64_c "Compiling 64-bit kernel process system" "$KERNEL64_PROCESS_C" "$KERNEL64_PROCESS_OBJ"

  compile64_c "Compiling 64-bit usermode support" "$KERNEL64_USERMODE_C" "$KERNEL64_USERMODE_OBJ"
  assemble64 "Assembling 64-bit usermode entry" "$KERNEL64_USERMODE_ASM" "$KERNEL64_USERMODE_ASM_OBJ"

  compile64_c "Compiling 64-bit syscall support" "$KERNEL64_SYSCALL_C" "$KERNEL64_SYSCALL_OBJ"
  assemble64 "Assembling 64-bit syscall stub" "$KERNEL64_SYSCALL_ASM" "$KERNEL64_SYSCALL_ASM_OBJ"

  compile64_c "Compiling 64-bit usercopy" "$KERNEL64_USERCOPY_C" "$KERNEL64_USERCOPY_OBJ"
  compile64_c "Compiling 64-bit ELF loader" "$KERNEL64_ELF_C" "$KERNEL64_ELF_OBJ"

  compile64_c "Compiling 64-bit shell" "$KERNEL64_SHELL_C" "$KERNEL64_SHELL_OBJ"

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
    "$KERNEL64_VMEM_OBJ" \
    "$KERNEL64_GDT_ASM_OBJ" \
    "$KERNEL64_GDT_OBJ" \
    "$KERNEL64_CONSOLE_OBJ" \
    "$KERNEL64_IRQ_OBJ" \
    "$KERNEL64_INPUT_OBJ" \
    "$KERNEL64_POWER_OBJ" \
    "$KERNEL64_TIMER_OBJ" \
    "$KERNEL64_SAFE_MODE_OBJ" \
    "$KERNEL64_BOOTLOG_OBJ" \
    "$KERNEL64_BOOTDIAG_OBJ" \
    "$KERNEL64_BLOCK_OBJ" \
    "$KERNEL64_FAT32_OBJ" \
    "$KERNEL64_VFS_OBJ" \
    "$KERNEL64_INITRD_OBJ" \
    "$KERNEL64_FB64_OBJ" \
    "$KERNEL64_IMAGE_OBJ" \
    "$KERNEL64_CONTEXT_SWITCH_OBJ" \
    "$KERNEL64_PROCESS_OBJ" \
    "$KERNEL64_USERMODE_OBJ" \
    "$KERNEL64_USERMODE_ASM_OBJ" \
    "$KERNEL64_SYSCALL_OBJ" \
    "$KERNEL64_SYSCALL_ASM_OBJ" \
    "$KERNEL64_USERCOPY_OBJ" \
    "$KERNEL64_ELF_OBJ" \
    "$KERNEL64_SHELL_OBJ"

  msg "Converting 64-bit kernel ELF to flat binary"
  "$OBJCOPY" -O binary "$KERNEL64_ELF" "$KERNEL64_BIN"

  echo "Kernel64 binary size: $(stat -c%s "$KERNEL64_BIN") bytes"
}

# ============================================================
# User apps / assets / INITRD
# ============================================================

build_userapps() {
  msg "Building 64-bit ELF user apps"

  mkdir -p "$USERAPPS_BUILD_DIR"

  if [[ ! -d "$USERAPPS_DIR" ]]; then
    echo "No userapps directory found at $USERAPPS_DIR; skipping."
    return 0
  fi

  if [[ ! -f "$USERAPP_LINKER" ]]; then
    echo "Missing user app linker script: $USERAPP_LINKER" >&2
    return 1
  fi

  local found=0

  while IFS= read -r -d '' src; do
    found=1

    local base
    local name_upper
    local out

    base="$(basename "$src" .S)"
    name_upper="$(printf '%s' "$base" | tr 'a-z' 'A-Z')"
    out="$USERAPPS_BUILD_DIR/$name_upper.ELF"

    msg "Building user app $name_upper.ELF"

    "$GCC" \
      -m64 \
      -I"$USERAPPS_DIR" \
      -I"$USERAPPS_DIR/include" \
      -ffreestanding \
      -fno-pie \
      -fno-stack-protector \
      -mno-red-zone \
      -nostdlib \
      -static \
      -Wl,-T,"$USERAPP_LINKER" \
      "$src" \
      -o "$out"

    "$OBJCOPY" --strip-debug "$out" "$out"
  done < <(find "$USERAPPS_DIR" -maxdepth 1 -type f -name '*.S' -print0)

  if [[ "$found" == "0" ]]; then
    echo "No user app .S files found in $USERAPPS_DIR."
  fi
}

mtools_mkdir_p() {
  local img="$1"
  local path="$2"

  path="${path#/}"
  path="${path%/}"

  if [[ -z "$path" || "$path" == "." ]]; then
    return 0
  fi

  local current=""
  IFS='/' read -r -a parts <<< "$path"

  for part in "${parts[@]}"; do
    [[ -z "$part" ]] && continue

    current="$current/$part"
    "$MMD" -i "$img" "::$current" 2>/dev/null || true
  done
}

copy_asset_to_uefi_img() {
  local src="$1"
  local rel="$2"

  local dest_dir
  local dest_path

  dest_dir="$(dirname "$rel")"
  dest_path="/$rel"

  if [[ "$dest_dir" != "." ]]; then
    mtools_mkdir_p "$UEFI_IMG" "$dest_dir"
  fi

  "$MCOPY" -o -i "$UEFI_IMG" "$src" "::$dest_path"
}

add_uefi_assets() {
  msg "Adding loose assets to UEFI filesystem"

  if [[ ! -d "$ASSETS_FS_DIR" ]]; then
    echo "No assets directory found at $ASSETS_FS_DIR; skipping assets."
    return 0
  fi

  if [[ ! -f "$NIMG_TOOL" ]]; then
    echo "Warning: $NIMG_TOOL not found; PNG assets cannot be converted." >&2
  fi

  mkdir -p "$ASSETS_BUILD_DIR"

  while IFS= read -r -d '' src; do
    local rel="${src#$ASSETS_FS_DIR/}"
    local ext="${rel##*.}"
    local lower_ext
    lower_ext="$(printf '%s' "$ext" | tr 'A-Z' 'a-z')"

    if [[ "$lower_ext" == "png" ]]; then
      if [[ ! -f "$NIMG_TOOL" ]]; then
        echo "Skipping PNG asset because converter is missing: $rel" >&2
        continue
      fi

      local rel_no_ext="${rel%.*}"
      local converted="$ASSETS_BUILD_DIR/$rel_no_ext.ni"
      local converted_rel="$rel_no_ext.ni"

      mkdir -p "$(dirname "$converted")"

      if [[ "$rel" == "boot/splash.png" ]]; then
        "$PYTHON3" "$NIMG_TOOL" "$src" "$converted" "$SPLASH_NIMG_WIDTH" "$SPLASH_NIMG_HEIGHT"
      else
        "$PYTHON3" "$NIMG_TOOL" "$src" "$converted"
      fi

      copy_asset_to_uefi_img "$converted" "$converted_rel"
    else
      copy_asset_to_uefi_img "$src" "$rel"
    fi
  done < <(find "$ASSETS_FS_DIR" -type f -print0)
}

build_initrd() {
  msg "Building INITRD.NAQ"

  rm -rf "$INITRD_FS_DIR"
  mkdir -p "$INITRD_FS_DIR"

  if [[ -d "$ASSETS_FS_DIR" ]]; then
    cp -a "$ASSETS_FS_DIR"/. "$INITRD_FS_DIR"/
  fi

  mkdir -p "$INITRD_FS_DIR/apps"

  if [[ -d "$USERAPPS_BUILD_DIR" ]]; then
    while IFS= read -r -d '' app; do
      cp "$app" "$INITRD_FS_DIR/apps/$(basename "$app")"
    done < <(find "$USERAPPS_BUILD_DIR" -maxdepth 1 -type f -name '*.ELF' -print0)
  fi

  if [[ -f "$NIMG_TOOL" ]]; then
    while IFS= read -r -d '' png; do
      local rel="${png#$INITRD_FS_DIR/}"
      local rel_no_ext="${rel%.*}"
      local out="$INITRD_FS_DIR/$rel_no_ext.ni"

      mkdir -p "$(dirname "$out")"

      if [[ "$rel" == "boot/splash.png" ]]; then
        "$PYTHON3" "$NIMG_TOOL" "$png" "$out" "$SPLASH_NIMG_WIDTH" "$SPLASH_NIMG_HEIGHT"
      else
        "$PYTHON3" "$NIMG_TOOL" "$png" "$out"
      fi

      rm -f "$png"
    done < <(find "$INITRD_FS_DIR" -type f -iname '*.png' -print0)
  fi

  require_file "$INITRD_TOOL" "INITRD tool"
  "$PYTHON3" "$INITRD_TOOL" "$INITRD_FS_DIR" "$INITRD_IMG"
}

# ============================================================
# UEFI build
# ============================================================

build_uefi() {
  msg "Building UEFI BOOTX64.EFI"

  require_file "$UEFI_C" "UEFI loader source"

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

  require_file "$UEFI_EFI" "UEFI loader"
  require_file "$KERNEL64_BIN" "64-bit kernel"

  rm -f "$UEFI_IMG"

  dd if=/dev/zero of="$UEFI_IMG" bs=1M count="$UEFI_IMG_MB" status=none
  "$MKFS_FAT" -F 32 "$UEFI_IMG" >/dev/null

  "$MMD" -i "$UEFI_IMG" ::/EFI 2>/dev/null || true
  "$MMD" -i "$UEFI_IMG" ::/EFI/BOOT 2>/dev/null || true

  build_userapps
  build_initrd

  require_file "$INITRD_IMG" "INITRD image"

  msg "Copying UEFI loader, kernel, and INITRD"
  "$MCOPY" -o -i "$UEFI_IMG" "$UEFI_EFI" ::/EFI/BOOT/BOOTX64.EFI
  "$MCOPY" -o -i "$UEFI_IMG" "$KERNEL64_BIN" ::/KERNEL64.BIN
  "$MCOPY" -o -i "$UEFI_IMG" "$INITRD_IMG" ::/INITRD.NAQ

  "$MMD" -i "$UEFI_IMG" ::/apps 2>/dev/null || true

  if [[ -d "$USERAPPS_BUILD_DIR" ]]; then
    while IFS= read -r -d '' app; do
      "$MCOPY" -o -i "$UEFI_IMG" "$app" "::/apps/$(basename "$app")"
    done < <(find "$USERAPPS_BUILD_DIR" -maxdepth 1 -type f -name '*.ELF' -print0)
  fi

  if [[ "${COPY_ASSETS_TO_UEFI:-0}" == "1" ]]; then
    add_uefi_assets
  else
    msg "Skipping loose UEFI assets; assets are packed into INITRD.NAQ"
  fi

  "$MDIR" -i "$UEFI_IMG" ::/ || true
  "$MDIR" -i "$UEFI_IMG" ::/EFI/BOOT || true
  "$MDIR" -i "$UEFI_IMG" ::/apps || true

  echo "Created: $UEFI_IMG"
}

# ============================================================
# Hardware-safe packaging
# ============================================================

package_hardware_safe() {
  msg "Packaging hardware-safe release"

  require_file "$UEFI_EFI" "UEFI loader"
  require_file "$KERNEL64_BIN" "64-bit kernel"
  require_file "$INITRD_IMG" "INITRD image"

  rm -rf "$HARDWARE_SAFE_DIR"
  mkdir -p "$HARDWARE_SAFE_DIR/EFI/BOOT"

  cp "$UEFI_EFI" "$HARDWARE_SAFE_DIR/EFI/BOOT/BOOTX64.EFI"
  cp "$KERNEL64_BIN" "$HARDWARE_SAFE_DIR/KERNEL64.BIN"
  cp "$INITRD_IMG" "$HARDWARE_SAFE_DIR/INITRD.NAQ"

  msg "Hardware-safe release created"

  echo
  echo "Output:"
  echo "  $HARDWARE_SAFE_DIR/EFI/BOOT/BOOTX64.EFI"
  echo "  $HARDWARE_SAFE_DIR/KERNEL64.BIN"
  echo "  $HARDWARE_SAFE_DIR/INITRD.NAQ"
  echo
  echo "USB layout:"
  echo "  EFI/BOOT/BOOTX64.EFI"
  echo "  KERNEL64.BIN"
  echo "  INITRD.NAQ"
  echo

  if [[ -n "${USB_DRIVE:-}" ]]; then
    msg "Copying hardware-safe files to USB drive $USB_DRIVE"

    powershell.exe -NoProfile -Command "New-Item -ItemType Directory -Force '${USB_DRIVE}:\EFI\BOOT' | Out-Null"

    copy_to_usb_windows "$HARDWARE_SAFE_DIR/EFI/BOOT/BOOTX64.EFI" "${USB_DRIVE}:\EFI\BOOT\BOOTX64.EFI"
    copy_to_usb_windows "$HARDWARE_SAFE_DIR/KERNEL64.BIN" "${USB_DRIVE}:\KERNEL64.BIN"
    copy_to_usb_windows "$HARDWARE_SAFE_DIR/INITRD.NAQ" "${USB_DRIVE}:\INITRD.NAQ"

    echo
    echo "Copied to USB drive $USB_DRIVE:"
    powershell.exe -NoProfile -Command "Get-ChildItem '${USB_DRIVE}:\' -Recurse"
  else
    echo "To copy to USB manually:"
    echo "  cp -r $HARDWARE_SAFE_DIR/* /path/to/usb/"
    echo
    echo "Or copy directly to Windows USB drive F:"
    echo "  USB_DRIVE=F ./build.sh hardware-safe"
  fi
}

build_hardware_safe() {
  msg "Building hardware-safe UEFI release"

  export NAQ_SAFE_HW_DEFAULT=1
  export NAQ_BOOT_VERBOSE=0
  export COPY_ASSETS_TO_UEFI=0

  check_tools_uefi
  build_kernel64
  build_uefi
  build_uefi_image
  package_hardware_safe
}

# ============================================================
# Run / export
# ============================================================

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

  if [[ ! -f "$UEFI_IMG" ]]; then
    echo "Error: $UEFI_IMG does not exist." >&2
    echo "Build it first with: ./build.sh uefi" >&2
    exit 1
  fi

  if [[ ! -f "$OVMF_CODE" ]]; then
    local detected_code
    detected_code="$(find_ovmf_file CODE)" || {
      echo "Error: could not find OVMF_CODE fd file." >&2
      echo "Install OVMF with: sudo apt install ovmf" >&2
      exit 1
    }

    OVMF_CODE="$detected_code"
  fi

  if [[ ! -f "$OVMF_VARS_TEMPLATE" ]]; then
    local detected_vars
    detected_vars="$(find_ovmf_file VARS)" || {
      echo "Error: could not find OVMF_VARS fd file." >&2
      echo "Install OVMF with: sudo apt install ovmf" >&2
      exit 1
    }

    OVMF_VARS_TEMPLATE="$detected_vars"
  fi

  mkdir -p "$BUILD_DIR"
  cp "$OVMF_VARS_TEMPLATE" "$OVMF_VARS"

  echo "OVMF_CODE: $OVMF_CODE"
  echo "OVMF_VARS: $OVMF_VARS"
  echo "UEFI_IMG : $UEFI_IMG"

  local qemu_debug_args=()

  if [[ "${QEMU_DEBUG:-0}" == "1" ]]; then
    qemu_debug_args=(
      -no-reboot
      -no-shutdown
      -d int,cpu_reset,guest_errors
      -D "$BUILD_DIR/qemu-uefi.log"
    )
  fi

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
    -net none \
    "${qemu_debug_args[@]}"
}

build_vbox_disk() {
  msg "Converting raw OS image to VirtualBox VDI"

  if ! command -v "$VBOXMANAGE" >/dev/null 2>&1; then
    echo "Error: VBoxManage not found." >&2
    exit 1
  fi

  rm -f "$VBOX_VDI"

  "$VBOXMANAGE" convertfromraw "$OS_IMG" "$VBOX_VDI" --format VDI

  echo "Created: $VBOX_VDI"
}

create_or_update_vbox_vm() {
  msg "Creating/updating VirtualBox VM"

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
  fi
}

# ============================================================
# Usage
# ============================================================

usage() {
  cat <<EOF
Usage: ./build.sh [command]

Commands:
  build          Clean and build legacy BIOS image
  run            Build legacy BIOS image and boot in QEMU
  clean          Remove build directory
  boot           Build only legacy stage1 + stage2
  kernel         Build only legacy 32-bit kernel
  apps           Build only legacy 32-bit apps
  fs             Build only legacy FAT32 filesystem image
  image          Build only legacy final disk image

  uefi           Build 64-bit UEFI image
  uefi-qemu      Build 64-bit UEFI image and run it in QEMU/OVMF
  hardware-safe  Build USB-safe real-hardware release package

  qemu           Run legacy BIOS image in QEMU
  vdi            Build legacy VirtualBox VDI
  vbox           Build and start VirtualBox VM
  vbox-win       Export legacy image for Windows VirtualBox

  help           Show this help

Useful environment:
  NAQ_SAFE_HW_DEFAULT=0 ./build.sh uefi-qemu
      Build UEFI/QEMU in dev mode with FAT32/block64 enabled.

  USB_DRIVE=F ./build.sh hardware-safe
      Build safe real-hardware package and copy to Windows USB drive F:.

  COPY_ASSETS_TO_UEFI=1 ./build.sh uefi
      Also copy loose assets into the UEFI FAT image.

  NAQ_BOOT_VERBOSE=1 ./build.sh uefi-qemu
      Enable verbose boot logs.

Default:
  build
EOF
}

# ============================================================
# Main
# ============================================================

cmd="${1:-build}"

case "$cmd" in
  build)
    check_tools_qemu
    clean
    build_bootloader
    build_kernel
    build_apps
    build_filesystem
    build_disk_image
    show_summary
    ;;

  run)
    check_tools_qemu
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
    check_tools_common
    mkdir -p "$BUILD_DIR"
    build_bootloader
    ;;

  kernel)
    check_tools_common
    mkdir -p "$BUILD_DIR"
    build_kernel
    ;;

  apps)
    check_tools_common
    mkdir -p "$BUILD_DIR"
    build_apps
    ;;

  fs)
    check_tools_common
    mkdir -p "$BUILD_DIR"
    build_filesystem
    ;;

  image)
    check_tools_common
    mkdir -p "$BUILD_DIR"
    build_disk_image
    show_summary
    ;;

  qemu)
    need_cmd "$QEMU"
    run_qemu
    ;;

  uefi)
    check_tools_uefi
    clean
    build_kernel64
    build_uefi
    build_uefi_image
    ;;

  uefi-qemu)
    check_tools_uefi_qemu
    clean
    build_kernel64
    build_uefi
    build_uefi_image
    run_qemu_uefi
    ;;

  hardware-safe)
    clean
    build_hardware_safe
    ;;

  vdi)
    check_tools_common
    clean
    build_bootloader
    build_kernel
    build_apps
    build_filesystem
    build_disk_image
    show_summary
    build_vbox_disk
    ;;

  vbox)
    check_tools_common
    need_cmd "$VBOXMANAGE"
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
    check_tools_common
    clean
    build_bootloader
    build_kernel
    build_apps
    build_filesystem
    build_disk_image
    show_summary
    export_vbox_windows
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