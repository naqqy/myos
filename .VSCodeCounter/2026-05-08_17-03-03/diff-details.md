# Diff Details

Date : 2026-05-08 17:03:03

Directory /home/naqqydev/myos

Total : 131 files,  4646 codes, 220 comments, 1386 blanks, all 6252 lines

[Summary](results.md) / [Details](details.md) / [Diff Summary](diff.md) / Diff Details

## Files
| filename | language | code | comment | blank | total |
| :--- | :--- | ---: | ---: | ---: | ---: |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/apps/2dgame.c](///wsl.localhost/Ubuntu/home/naqqydev/myos/apps/2dgame.c) | C | -191 | 0 | -51 | -242 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/apps/app\_entry.asm](///wsl.localhost/Ubuntu/home/naqqydev/myos/apps/app_entry.asm) | ARM | -13 | 0 | -4 | -17 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/apps/badptr.c](///wsl.localhost/Ubuntu/home/naqqydev/myos/apps/badptr.c) | C | -6 | 0 | -3 | -9 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/apps/demo.c](///wsl.localhost/Ubuntu/home/naqqydev/myos/apps/demo.c) | C | -6 | 0 | -1 | -7 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/apps/keytest.c](///wsl.localhost/Ubuntu/home/naqqydev/myos/apps/keytest.c) | C | -17 | 0 | -6 | -23 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/apps/spin.c](///wsl.localhost/Ubuntu/home/naqqydev/myos/apps/spin.c) | C | -8 | 0 | -3 | -11 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/apps/syscall.h](///wsl.localhost/Ubuntu/home/naqqydev/myos/apps/syscall.h) | C++ | -123 | -47 | -38 | -208 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/apps/sysinfo.c](///wsl.localhost/Ubuntu/home/naqqydev/myos/apps/sysinfo.c) | C | -29 | 0 | -6 | -35 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/apps/systest.c](///wsl.localhost/Ubuntu/home/naqqydev/myos/apps/systest.c) | C | -6 | 0 | -2 | -8 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/apps/uhello.c](///wsl.localhost/Ubuntu/home/naqqydev/myos/apps/uhello.c) | C | -10 | 0 | -2 | -12 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/apps/wolf3d.c](///wsl.localhost/Ubuntu/home/naqqydev/myos/apps/wolf3d.c) | C | -262 | -32 | -70 | -364 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/apps/yt.c](///wsl.localhost/Ubuntu/home/naqqydev/myos/apps/yt.c) | C | -8 | 0 | -2 | -10 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/boot/boot.asm](///wsl.localhost/Ubuntu/home/naqqydev/myos/boot/boot.asm) | ARM | -59 | 0 | -14 | -73 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/boot/stage2.asm](///wsl.localhost/Ubuntu/home/naqqydev/myos/boot/stage2.asm) | ARM | -442 | 0 | -131 | -573 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/build.sh](///wsl.localhost/Ubuntu/home/naqqydev/myos/build.sh) | Shell Script | -369 | -25 | -97 | -491 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/filesforlater/com.c](///wsl.localhost/Ubuntu/home/naqqydev/myos/filesforlater/com.c) | C | -167 | 0 | -25 | -192 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/context\_switch.asm](///wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/context_switch.asm) | ARM | -17 | 0 | -5 | -22 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/entry.asm](///wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/entry.asm) | ARM | -17 | 0 | -7 | -24 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/exec.c](///wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/exec.c) | C | -353 | -4 | -90 | -447 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/exec.h](///wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/exec.h) | C++ | -22 | 0 | -11 | -33 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/fat16.c](///wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/fat16.c) | C | -691 | -8 | -195 | -894 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/fat16.h](///wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/fat16.h) | C++ | -13 | -11 | -7 | -31 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/gdt.c](///wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/gdt.c) | C | -92 | -32 | -23 | -147 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/gdt.h](///wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/gdt.h) | C++ | -13 | 0 | -4 | -17 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/gdt\_flush.asm](///wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/gdt_flush.asm) | ARM | -20 | 0 | -6 | -26 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/isr.asm](///wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/isr.asm) | ARM | -146 | 0 | -30 | -176 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/kernel.c](///wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/kernel.c) | C | -765 | -11 | -202 | -978 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/kmem.c](///wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/kmem.c) | C | -211 | -12 | -56 | -279 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/kmem.h](///wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/kmem.h) | C++ | -9 | 0 | -4 | -13 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/memmap.c](///wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/memmap.c) | C | -174 | -1 | -56 | -231 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/memmap.h](///wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/memmap.h) | C++ | -23 | 0 | -7 | -30 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/paging.c](///wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/paging.c) | C | -244 | -39 | -83 | -366 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/paging.h](///wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/paging.h) | C++ | -22 | -8 | -9 | -39 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/pmm.c](///wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/pmm.c) | C | -121 | 0 | -32 | -153 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/pmm.h](///wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/pmm.h) | C++ | -13 | 0 | -6 | -19 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/power.c](///wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/power.c) | C | -39 | -1 | -12 | -52 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/power.h](///wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/power.h) | C++ | -5 | 0 | -2 | -7 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/process.c](///wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/process.c) | C | -610 | -58 | -197 | -865 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/process.h](///wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/process.h) | C++ | -72 | 0 | -29 | -101 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/shell.c](///wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/shell.c) | C | -618 | -4 | -135 | -757 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/shell.h](///wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/shell.h) | C++ | -10 | 0 | -2 | -12 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/syscall.c](///wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/syscall.c) | C | -83 | 0 | -27 | -110 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/syscall.h](///wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/syscall.h) | C++ | -18 | 0 | -4 | -22 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/syscall\_stub.asm](///wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/syscall_stub.asm) | ARM | -61 | 0 | -14 | -75 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/terminal.h](///wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/terminal.h) | C++ | -22 | 0 | -6 | -28 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/timer.c](///wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/timer.c) | C | -47 | 0 | -13 | -60 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/timer.h](///wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/timer.h) | C++ | -10 | 0 | -3 | -13 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/usercopy.c](///wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/usercopy.c) | C | -90 | 0 | -31 | -121 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/usercopy.h](///wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/usercopy.h) | C++ | -7 | 0 | -3 | -10 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/usermode.asm](///wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/usermode.asm) | ARM | -33 | 0 | -10 | -43 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/usermode.h](///wsl.localhost/Ubuntu/home/naqqydev/myos/kernel/usermode.h) | C++ | -5 | 0 | -3 | -8 |
| [//wsl.localhost/Ubuntu/home/naqqydev/myos/tools/make\_naq.py](///wsl.localhost/Ubuntu/home/naqqydev/myos/tools/make_naq.py) | Python | -30 | -4 | -9 | -43 |
| [apps/2dgame.c](/apps/2dgame.c) | C | 191 | 0 | 51 | 242 |
| [apps/badptr.c](/apps/badptr.c) | C | 6 | 0 | 3 | 9 |
| [apps/demo.c](/apps/demo.c) | C | 6 | 0 | 1 | 7 |
| [apps/fstest.c](/apps/fstest.c) | C | 25 | 0 | 7 | 32 |
| [apps/keytest.c](/apps/keytest.c) | C | 17 | 0 | 6 | 23 |
| [apps/spin.c](/apps/spin.c) | C | 8 | 0 | 3 | 11 |
| [apps/syscall.h](/apps/syscall.h) | C++ | 139 | 43 | 42 | 224 |
| [apps/sysinfo.c](/apps/sysinfo.c) | C | 29 | 0 | 6 | 35 |
| [apps/systest.c](/apps/systest.c) | C | 6 | 0 | 2 | 8 |
| [apps/uhello.c](/apps/uhello.c) | C | 10 | 0 | 2 | 12 |
| [apps/wolf3d.c](/apps/wolf3d.c) | C | 262 | 32 | 70 | 364 |
| [apps/yt.c](/apps/yt.c) | C | 8 | 0 | 2 | 10 |
| [build.sh](/build.sh) | Shell Script | 1,038 | 25 | 213 | 1,276 |
| [filesforlater/com.c](/filesforlater/com.c) | C | 167 | 0 | 25 | 192 |
| [graphics/font8x8.c](/graphics/font8x8.c) | C | 108 | 0 | 11 | 119 |
| [graphics/font8x8.h](/graphics/font8x8.h) | C++ | 5 | 0 | 3 | 8 |
| [graphics/framebuffer.c](/graphics/framebuffer.c) | C | 278 | 0 | 89 | 367 |
| [graphics/framebuffer.h](/graphics/framebuffer.h) | C++ | 33 | 0 | 10 | 43 |
| [graphics/image.c](/graphics/image.c) | C | 101 | 0 | 36 | 137 |
| [graphics/image.h](/graphics/image.h) | C++ | 16 | 0 | 5 | 21 |
| [include/naq\_boot\_info.h](/include/naq_boot_info.h) | C++ | 50 | 4 | 15 | 69 |
| [kernel/exec.c](/kernel/exec.c) | C | 319 | 0 | 79 | 398 |
| [kernel/exec.h](/kernel/exec.h) | C++ | 22 | 0 | 11 | 33 |
| [kernel/fat32.c](/kernel/fat32.c) | C | 1,586 | 27 | 459 | 2,072 |
| [kernel/fat32.h](/kernel/fat32.h) | C++ | 16 | 0 | 7 | 23 |
| [kernel/gdt.c](/kernel/gdt.c) | C | 92 | 32 | 23 | 147 |
| [kernel/gdt.h](/kernel/gdt.h) | C++ | 13 | 0 | 4 | 17 |
| [kernel/kernel.c](/kernel/kernel.c) | C | 1,136 | 49 | 353 | 1,538 |
| [kernel/kmem.c](/kernel/kmem.c) | C | 211 | 12 | 56 | 279 |
| [kernel/kmem.h](/kernel/kmem.h) | C++ | 9 | 0 | 4 | 13 |
| [kernel/memmap.c](/kernel/memmap.c) | C | 211 | 1 | 69 | 281 |
| [kernel/memmap.h](/kernel/memmap.h) | C++ | 23 | 0 | 7 | 30 |
| [kernel/paging.c](/kernel/paging.c) | C | 285 | 35 | 97 | 417 |
| [kernel/paging.h](/kernel/paging.h) | C++ | 22 | 8 | 9 | 39 |
| [kernel/pmm.c](/kernel/pmm.c) | C | 158 | 0 | 47 | 205 |
| [kernel/pmm.h](/kernel/pmm.h) | C++ | 14 | 0 | 7 | 21 |
| [kernel/power.c](/kernel/power.c) | C | 39 | 1 | 12 | 52 |
| [kernel/power.h](/kernel/power.h) | C++ | 5 | 0 | 2 | 7 |
| [kernel/process.c](/kernel/process.c) | C | 610 | 58 | 197 | 865 |
| [kernel/process.h](/kernel/process.h) | C++ | 72 | 0 | 29 | 101 |
| [kernel/shell.c](/kernel/shell.c) | C | 657 | 12 | 140 | 809 |
| [kernel/shell.h](/kernel/shell.h) | C++ | 10 | 0 | 2 | 12 |
| [kernel/syscall.c](/kernel/syscall.c) | C | 145 | 0 | 52 | 197 |
| [kernel/syscall.h](/kernel/syscall.h) | C++ | 22 | 0 | 5 | 27 |
| [kernel/terminal.h](/kernel/terminal.h) | C++ | 22 | 0 | 6 | 28 |
| [kernel/timer.c](/kernel/timer.c) | C | 47 | 0 | 13 | 60 |
| [kernel/timer.h](/kernel/timer.h) | C++ | 10 | 0 | 3 | 13 |
| [kernel/usercopy.c](/kernel/usercopy.c) | C | 117 | 0 | 41 | 158 |
| [kernel/usercopy.h](/kernel/usercopy.h) | C++ | 8 | 0 | 3 | 11 |
| [kernel/usermode.h](/kernel/usermode.h) | C++ | 5 | 0 | 3 | 8 |
| [kernel64/arch/x86\_64/gdt64.c](/kernel64/arch/x86_64/gdt64.c) | C | 112 | 3 | 33 | 148 |
| [kernel64/arch/x86\_64/gdt64.h](/kernel64/arch/x86_64/gdt64.h) | C++ | 12 | 0 | 5 | 17 |
| [kernel64/arch/x86\_64/idt64.c](/kernel64/arch/x86_64/idt64.c) | C | 164 | 0 | 43 | 207 |
| [kernel64/arch/x86\_64/idt64.h](/kernel64/arch/x86_64/idt64.h) | C++ | 50 | 0 | 12 | 62 |
| [kernel64/arch/x86\_64/io64.h](/kernel64/arch/x86_64/io64.h) | C++ | 18 | 0 | 6 | 24 |
| [kernel64/arch/x86\_64/irq64.c](/kernel64/arch/x86_64/irq64.c) | C | 133 | 7 | 38 | 178 |
| [kernel64/arch/x86\_64/irq64.h](/kernel64/arch/x86_64/irq64.h) | C++ | 7 | 0 | 3 | 10 |
| [kernel64/arch/x86\_64/power64.c](/kernel64/arch/x86_64/power64.c) | C | 24 | 6 | 8 | 38 |
| [kernel64/arch/x86\_64/power64.h](/kernel64/arch/x86_64/power64.h) | C++ | 5 | 0 | 2 | 7 |
| [kernel64/arch/x86\_64/timer64.c](/kernel64/arch/x86_64/timer64.c) | C | 52 | 6 | 19 | 77 |
| [kernel64/arch/x86\_64/timer64.h](/kernel64/arch/x86_64/timer64.h) | C++ | 10 | 0 | 5 | 15 |
| [kernel64/console/fb\_console.c](/kernel64/console/fb_console.c) | C | 204 | 13 | 59 | 276 |
| [kernel64/input/kinput64.c](/kernel64/input/kinput64.c) | C | 60 | 3 | 21 | 84 |
| [kernel64/input/kinput64.h](/kernel64/input/kinput64.h) | C++ | 8 | 0 | 4 | 12 |
| [kernel64/k64\_console.h](/kernel64/k64_console.h) | C++ | 11 | 0 | 5 | 16 |
| [kernel64/main.c](/kernel64/main.c) | C | 90 | 5 | 35 | 130 |
| [kernel64/mm/kmem64.c](/kernel64/mm/kmem64.c) | C | 195 | 4 | 68 | 267 |
| [kernel64/mm/kmem64.h](/kernel64/mm/kmem64.h) | C++ | 11 | 0 | 5 | 16 |
| [kernel64/mm/memmap64.c](/kernel64/mm/memmap64.c) | C | 208 | 9 | 43 | 260 |
| [kernel64/mm/memmap64.h](/kernel64/mm/memmap64.h) | C++ | 24 | 0 | 9 | 33 |
| [kernel64/mm/paging64.c](/kernel64/mm/paging64.c) | C | 223 | 31 | 78 | 332 |
| [kernel64/mm/paging64.h](/kernel64/mm/paging64.h) | C++ | 16 | 0 | 8 | 24 |
| [kernel64/mm/pmm64.c](/kernel64/mm/pmm64.c) | C | 349 | 35 | 120 | 504 |
| [kernel64/mm/pmm64.h](/kernel64/mm/pmm64.h) | C++ | 21 | 5 | 10 | 36 |
| [kernel64/shell/shell64.c](/kernel64/shell/shell64.c) | C | 178 | 0 | 44 | 222 |
| [kernel64/shell/shell64.h](/kernel64/shell/shell64.h) | C++ | 6 | 0 | 2 | 8 |
| [tools/make\_naq.py](/tools/make_naq.py) | Python | 30 | 4 | 9 | 43 |
| [tools/make\_nimg.py](/tools/make_nimg.py) | Python | 34 | 2 | 14 | 50 |
| [uefi/bootx64.c](/uefi/bootx64.c) | C | 444 | 45 | 124 | 613 |

[Summary](results.md) / [Details](details.md) / [Diff Summary](diff.md) / Diff Details