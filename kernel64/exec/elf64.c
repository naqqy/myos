#include "elf64.h"

#include "../fs/vfs64.h"
#include "../mm/kmem64.h"
#include "../mm/paging64.h"
#include "../mm/usercopy64.h"
#include "../arch/x86_64/usermode64.h"
#include "../k64_console.h"

#include "../arch/x86_64/gdt64.h"
#include "../proc/process64.h"

#define EI_NIDENT 16

#define EI_MAG0       0
#define EI_MAG1       1
#define EI_MAG2       2
#define EI_MAG3       3
#define EI_CLASS      4
#define EI_DATA       5
#define EI_VERSION    6

#define ELFMAG0       0x7F
#define ELFMAG1       'E'
#define ELFMAG2       'L'
#define ELFMAG3       'F'

#define ELFCLASS64    2
#define ELFDATA2LSB   1
#define EV_CURRENT    1

#define ET_EXEC       2
#define EM_X86_64     62

#define PT_LOAD       1

#define PF_X          1
#define PF_W          2
#define PF_R          4

/*
 * POC user app area.
 *
 * Because paging is still identity-mapped 2 MiB pages, choose a simple
 * fixed virtual/physical region that is not near the kernel.
 *
 * Later, this should become a real per-process user address space.
 */
#define ELF64_USER_MIN_ADDR 0x02000000ull
#define ELF64_USER_MAX_ADDR 0x04000000ull
#define ELF64_USER_STACK_TOP ELF64_USER_MAX_ADDR
#define ELF64_USER_STACK_BASE (ELF64_USER_STACK_TOP - ELF64_USER_STACK_SIZE)

typedef struct elf64_ehdr {
    nq_u8  e_ident[EI_NIDENT];
    nq_u16 e_type;
    nq_u16 e_machine;
    nq_u32 e_version;
    nq_u64 e_entry;
    nq_u64 e_phoff;
    nq_u64 e_shoff;
    nq_u32 e_flags;
    nq_u16 e_ehsize;
    nq_u16 e_phentsize;
    nq_u16 e_phnum;
    nq_u16 e_shentsize;
    nq_u16 e_shnum;
    nq_u16 e_shstrndx;
} __attribute__((packed)) elf64_ehdr_t;

typedef struct elf64_phdr {
    nq_u32 p_type;
    nq_u32 p_flags;
    nq_u64 p_offset;
    nq_u64 p_vaddr;
    nq_u64 p_paddr;
    nq_u64 p_filesz;
    nq_u64 p_memsz;
    nq_u64 p_align;
} __attribute__((packed)) elf64_phdr_t;

static nq_u8 g_elf64_file_buf[ELF64_MAX_IMAGE_SIZE];

#define ELF64_MAX_PATH 128

typedef struct elf64_spawn_arg {
    char path[ELF64_MAX_PATH];
} elf64_spawn_arg_t;

static void str_copy64(char* dst, const char* src, nq_u64 dst_size) {
    nq_u64 i = 0;

    if (!dst || dst_size == 0) {
        return;
    }

    if (!src) {
        dst[0] = 0;
        return;
    }

    while (src[i] && i + 1 < dst_size) {
        dst[i] = src[i];
        i++;
    }

    dst[i] = 0;
}

static void mem_zero64(void* ptr, nq_u64 size) {
    nq_u8* p = (nq_u8*)ptr;

    if (!p) {
        return;
    }

    for (nq_u64 i = 0; i < size; i++) {
        p[i] = 0;
    }
}

static void mem_copy64(void* dst, const void* src, nq_u64 size) {
    nq_u8* d = (nq_u8*)dst;
    const nq_u8* s = (const nq_u8*)src;

    if (!d || !s) {
        return;
    }

    for (nq_u64 i = 0; i < size; i++) {
        d[i] = s[i];
    }
}

static int elf64_validate_header(const elf64_ehdr_t* eh, nq_u64 file_size) {
    if (!eh) {
        return 0;
    }

    if (file_size < sizeof(elf64_ehdr_t)) {
        return 0;
    }

    if (eh->e_ident[EI_MAG0] != ELFMAG0 ||
        eh->e_ident[EI_MAG1] != ELFMAG1 ||
        eh->e_ident[EI_MAG2] != ELFMAG2 ||
        eh->e_ident[EI_MAG3] != ELFMAG3) {
        k64_print("elf64: bad magic\n");
        return 0;
    }

    if (eh->e_ident[EI_CLASS] != ELFCLASS64) {
        k64_print("elf64: not ELF64\n");
        return 0;
    }

    if (eh->e_ident[EI_DATA] != ELFDATA2LSB) {
        k64_print("elf64: not little-endian\n");
        return 0;
    }

    if (eh->e_ident[EI_VERSION] != EV_CURRENT ||
        eh->e_version != EV_CURRENT) {
        k64_print("elf64: unsupported version\n");
        return 0;
    }

    if (eh->e_type != ET_EXEC) {
        k64_print("elf64: only ET_EXEC supported for now\n");
        return 0;
    }

    if (eh->e_machine != EM_X86_64) {
        k64_print("elf64: not x86_64\n");
        return 0;
    }

    if (eh->e_phentsize != sizeof(elf64_phdr_t)) {
        k64_print("elf64: bad program header size\n");
        return 0;
    }

    if (eh->e_phnum == 0) {
        k64_print("elf64: no program headers\n");
        return 0;
    }

    if (eh->e_phoff >= file_size) {
        k64_print("elf64: bad program header offset\n");
        return 0;
    }

    nq_u64 ph_end =
        eh->e_phoff + ((nq_u64)eh->e_phnum * sizeof(elf64_phdr_t));

    if (ph_end < eh->e_phoff || ph_end > file_size) {
        k64_print("elf64: program headers exceed file\n");
        return 0;
    }

    return 1;
}

static int elf64_validate_load_segment(
    const elf64_phdr_t* ph,
    nq_u64 file_size
) {
    if (!ph) {
        return 0;
    }

    if (ph->p_type != PT_LOAD) {
        return 1;
    }

    if (ph->p_memsz < ph->p_filesz) {
        k64_print("elf64: segment memsz < filesz\n");
        return 0;
    }

    if (ph->p_filesz > 0) {
        if (ph->p_offset >= file_size) {
            k64_print("elf64: bad segment offset\n");
            return 0;
        }

        nq_u64 file_end = ph->p_offset + ph->p_filesz;

        if (file_end < ph->p_offset || file_end > file_size) {
            k64_print("elf64: segment exceeds file\n");
            return 0;
        }
    }

    if (ph->p_vaddr < ELF64_USER_MIN_ADDR) {
        k64_print("elf64: segment below user min\n");
        return 0;
    }

    nq_u64 mem_end = ph->p_vaddr + ph->p_memsz;

    if (mem_end < ph->p_vaddr || mem_end > ELF64_USER_STACK_BASE) {
        k64_print("elf64: segment outside POC user range\n");
        return 0;
    }

    return 1;
}

static int elf64_load_segments(
    const elf64_ehdr_t* eh,
    const nq_u8* file,
    nq_u64 file_size
) {
    const elf64_phdr_t* ph =
        (const elf64_phdr_t*)(file + eh->e_phoff);

    for (nq_u32 i = 0; i < eh->e_phnum; i++) {
        if (!elf64_validate_load_segment(&ph[i], file_size)) {
            return 0;
        }

        if (ph[i].p_type != PT_LOAD) {
            continue;
        }

        void* dst = (void*)ph[i].p_vaddr;
        const void* src = file + ph[i].p_offset;

        k64_print("elf64: loading segment vaddr=");
        k64_print_hex64(ph[i].p_vaddr);
        k64_print(" filesz=");
        k64_print_dec64(ph[i].p_filesz);
        k64_print(" memsz=");
        k64_print_dec64(ph[i].p_memsz);
        k64_print("\n");

        /*
         * POC:
         * The destination virtual address is identity-mapped to physical memory.
         * Clear BSS first, then copy file bytes.
         */
        mem_zero64(dst, ph[i].p_memsz);

        if (ph[i].p_filesz > 0) {
            mem_copy64(dst, src, ph[i].p_filesz);
        }

        /*
         * Mark loaded segment as user-accessible.
         * Current paging64 marks at 2 MiB granularity.
         */
        nq_u64 seg_start = ph[i].p_vaddr & ~0xFFFull;
        nq_u64 seg_end = ph[i].p_vaddr + ph[i].p_memsz;

        if (seg_end < ph[i].p_vaddr) {
            k64_print("elf64: segment overflow\n");
            return 0;
        }

        seg_end = (seg_end + 0xFFFu) & ~0xFFFull;

        if (!paging64_mark_user_range(seg_start, seg_end - seg_start)) {
            k64_print("elf64: failed to mark segment user-accessible\n");
            k64_print("range=");
            k64_print_hex64(seg_start);
            k64_print("..");
            k64_print_hex64(seg_end);
            k64_print("\n");
            return 0;
        }
    }

    return 1;
}

static int elf64_entry_is_valid(const elf64_ehdr_t* eh) {
    if (!eh) {
        return 0;
    }

    if (eh->e_entry < ELF64_USER_MIN_ADDR ||
        eh->e_entry >= ELF64_USER_MAX_ADDR) {
        return 0;
    }

    return 1;
}

int elf64_exec_file(const char* path) {
    nq_u64 file_size = 0;

    if (!path || !path[0]) {
        k64_print("elf64: missing path\n");
        return 0;
    }

    k64_print("elf64: loading ");
    k64_print(path);
    k64_print("\n");

    if (!vfs64_read_file(
            path,
            g_elf64_file_buf,
            ELF64_MAX_IMAGE_SIZE,
            &file_size)) {
        k64_print("elf64: read failed or file too large\n");
        return 0;
    }

    const elf64_ehdr_t* eh = (const elf64_ehdr_t*)g_elf64_file_buf;

    if (!elf64_validate_header(eh, file_size)) {
        return 0;
    }

    if (!elf64_entry_is_valid(eh)) {
        k64_print("elf64: invalid entry point\n");
        return 0;
    }

    if (!elf64_load_segments(eh, g_elf64_file_buf, file_size)) {
        return 0;
    }

    nq_u64 user_stack_base = ELF64_USER_STACK_BASE;
    nq_u64 user_stack_top = ELF64_USER_STACK_TOP;

    user_stack_top &= ~0xFull;

    mem_zero64((void*)user_stack_base, ELF64_USER_STACK_SIZE);

    if (!paging64_mark_user_range(user_stack_base, ELF64_USER_STACK_SIZE)) {
        k64_print("elf64: failed to mark user stack\n");
        return 0;
    }

    k64_print("elf64: entering user app\n");
    k64_print("entry=");
    k64_print_hex64(eh->e_entry);
    k64_print(" stack=");
    k64_print_hex64(user_stack_top);
    k64_print("\n");

    process64_task_t* current = process64_current();

    if (current && current->stack_base && current->stack_size) {
        nq_u64 kernel_stack_top = current->stack_base + current->stack_size;

        gdt64_set_rsp0(kernel_stack_top);

        k64_print("elf64: TSS RSP0=");
        k64_print_hex64(kernel_stack_top);
        k64_print("\n");
    }

    usermode64_enter(eh->e_entry, user_stack_top);

    return 1;
}

static void elf64_process_entry(void* arg) {
    elf64_spawn_arg_t* spawn_arg = (elf64_spawn_arg_t*)arg;
    char path[ELF64_MAX_PATH];

    if (!spawn_arg || !spawn_arg->path[0]) {
        k64_print("elf64 task: missing path\n");
        process64_exit();
    }

    str_copy64(path, spawn_arg->path, sizeof(path));
    kfree64(spawn_arg);

    /*
     * This enters ring 3 and normally does not return until:
     *   - app faults, or
     *   - app calls SYS_EXIT and the syscall path terminates this task.
     */
    if (!elf64_exec_file(path)) {
        k64_print("elf64 task: exec failed\n");
        process64_exit();
    }

    /*
     * If elf64_exec_file ever returns, end the task.
     */
    process64_exit();
}

int elf64_spawn_file(const char* path) {
    if (!path || !path[0]) {
        return -1;
    }

    elf64_spawn_arg_t* arg =
        (elf64_spawn_arg_t*)kmalloc64(sizeof(elf64_spawn_arg_t));

    if (!arg) {
        k64_print("elf64: failed to allocate spawn arg\n");
        return -1;
    }

    mem_zero64(arg, sizeof(*arg));
    str_copy64(arg->path, path, sizeof(arg->path));

    int pid = process64_create_kernel_task(
        "elf64-app",
        elf64_process_entry,
        arg
    );

    if (pid < 0) {
        k64_print("elf64: failed to create app task\n");
        kfree64(arg);
        return -1;
    }

    k64_print("elf64: spawned ");
    k64_print(path);
    k64_print(" pid=");
    k64_print_dec64((nq_u64)pid);
    k64_print("\n");

    return pid;
}
