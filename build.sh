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
  echo "  LBA 0        -> stage1 boot sector"
  echo "  LBA 1-$STAGE2_SECTORS     -> stage2 loader"
  echo "  LBA ${FS_START_LBA}+      -> FAT32 filesystem containing KERNEL.BIN"
}

run_qemu() {
  "$QEMU" \
    -vga std \
    -display sdl \
    -monitor none \
    -serial none \
    -drive format=raw,file="$OS_IMG"
}

write_bochsrc() {
  mkdir -p "$BUILD_DIR"

  local bochs_bios="/usr/share/bochs/BIOS-bochs-latest"
  local bochs_vgabios=""

  if [[ -f "/usr/share/bochs/VGABIOS-lgpl-latest" ]]; then
    bochs_vgabios="/usr/share/bochs/VGABIOS-lgpl-latest"
  elif [[ -f "/usr/share/vgabios/vgabios.bin" ]]; then
    bochs_vgabios="/usr/share/vgabios/vgabios.bin"
  else
    echo "Error: could not find Bochs/VBE VGA BIOS ROM" >&2
    echo "Tried:" >&2
    echo "  /usr/share/bochs/VGABIOS-lgpl-latest" >&2
    echo "  /usr/share/vgabios/vgabios.bin" >&2
    exit 1
  fi

  cat > "$BOCHS_CFG" <<EOF
megs: 128

romimage: file=$bochs_bios
vgaromimage: file=$bochs_vgabios

vga: extension=vbe, update_freq=15

ata0-master: type=disk, mode=flat, path="$OS_IMG", cylinders=272, heads=16, spt=32
boot: disk

log: $BUILD_DIR/bochs.log
panic: action=ask
error: action=ask
info: action=report
debug: action=ignore

clock: sync=realtime
cpu: ips=50000000

display_library: sdl2
EOF

  echo "Bochs BIOS    : $bochs_bios"
  echo "Bochs VGA BIOS: $bochs_vgabios"
}

run_bochs() {
  if ! command -v "$BOCHS" >/dev/null 2>&1; then
    echo "Error: bochs not found. Install it with:" >&2
    echo "  sudo apt install bochs bochs-sdl bochsbios vgabios" >&2
    exit 1
  fi

  write_bochsrc

  "$BOCHS" -q -f "$BOCHS_CFG"
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
  qemu      Run in QEMU
  bochs     Run in Bochs
  help      Show this help

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
  bochs)
    check_tools
    clean
    build_bootloader
    build_kernel
    build_apps
    build_filesystem
    build_disk_image
    show_summary
    run_bochs
    ;;
  bochs-only)
    run_bochs
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