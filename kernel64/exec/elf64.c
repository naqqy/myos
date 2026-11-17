#include "elf64.h"

#include "../fs/vfs64.h"
#include "../mm/kmem64.h"
#include "../mm/paging64.h"
#include "../mm/vmem64.h"
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
 * User app virtual region (see VMEM64_USER_BASE). Apps are linked at the
 * base; the user stack sits at the top. Mappings are per-process.
 */
#define ELF64_USER_MIN_ADDR VMEM64_USER_BASE
#define ELF64_USER_MAX_ADDR VMEM64_USER_LIMIT
#define ELF64_USER_STACK_TOP ELF64_USER_MAX_ADDR
#define ELF64_USER_STACK_BASE (ELF64_USER_STACK_TOP - ELF64_USER_STACK_SIZE)

#ifndef ELF64_VERBOSE
#define ELF64_VERBOSE 0
#endif

static void elf64_debug(const char* s) {
#if ELF64_VERBOSE
    k64_print(s);
#else
    (void)s;
#endif
}

static void elf64_debug_hex(const char* label, nq_u64 value) {
#if ELF64_VERBOSE
    k64_print(label);
    k64_print_hex64(value);
    k64_print("\n");
#else
    (void)label;
    (void)value;
#endif
}

static void elf64_debug_dec(const char* label, nq_u64 value) {
#if ELF64_VERBOSE
    k64_print(label);
    k64_print_dec64(value);
    k64_print("\n");
#else
    (void)label;
    (void)value;
#endif
}

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
    nq_u64 file_size,
    nq_u64* out_image_base,
    nq_u64* out_image_size
) {
    const elf64_phdr_t* ph =
        (const elf64_phdr_t*)(file + eh->e_phoff);

    nq_u64 image_min = 0xFFFFFFFFFFFFFFFFull;
    nq_u64 image_max = 0;

    process64_task_t* current = process64_current();

    if (!current) {
        return 0;
    }

    nq_u64 pml4_phys = process64_current_pml4();

    if (!pml4_phys) {
        return 0;
    }

    for (nq_u32 i = 0; i < eh->e_phnum; i++) {
        if (!elf64_validate_load_segment(&ph[i], file_size)) {
            return 0;
        }

        if (ph[i].p_type != PT_LOAD) {
            continue;
        }

        void* dst = (void*)ph[i].p_vaddr;
        const void* src = file + ph[i].p_offset;

        elf64_debug("elf64: loading segment vaddr=");
        elf64_debug_hex(" vaddr=", ph[i].p_vaddr);
        elf64_debug(" filesz=");
        elf64_debug_dec(" filesz=", ph[i].p_filesz);
        elf64_debug(" memsz=");
        elf64_debug_dec(" memsz=", ph[i].p_memsz);
        elf64_debug("\n");

        nq_u64 seg_start = ph[i].p_vaddr & ~0xFFFull;
        nq_u64 seg_end = ph[i].p_vaddr + ph[i].p_memsz;

        if (seg_end < ph[i].p_vaddr) {
            k64_print("elf64: segment overflow\n");
            return 0;
        }

        seg_end = (seg_end + 0xFFFu) & ~0xFFFull;

        if (!paging64_alloc_map_user_range(
                pml4_phys,
                seg_start,
                seg_end - seg_start,
                PAGE64_USER | PAGE64_WRITABLE)) {
            k64_print("elf64: failed to allocate/map segment\n");
            k64_print("range=");
            k64_print_hex64(seg_start);
            k64_print("..");
            k64_print_hex64(seg_end);
            k64_print("\n");
            return 0;
        }

        if (!paging64_user_range_accessible(seg_start, seg_end - seg_start)) {
            k64_print("elf64: mapped segment is NOT accessible after map\n");
            k64_print("range=");
            k64_print_hex64(seg_start);
            k64_print("..");
            k64_print_hex64(seg_end);
            k64_print("\n");
            return 0;
        }

        /*
         * The task is already running on its own CR3. After mapping the
         * segment, copying to p_vaddr writes into this process's private
         * physical pages.
         */
        mem_zero64(dst, ph[i].p_memsz);

        if (ph[i].p_filesz > 0) {
            mem_copy64(dst, src, ph[i].p_filesz);
        }

        if (seg_start < image_min) {
            image_min = seg_start;
        }

        if (seg_end > image_max) {
            image_max = seg_end;
        }
    }

    if (image_min == 0xFFFFFFFFFFFFFFFFull || image_max <= image_min) {
        k64_print("elf64: no loadable image range\n");
        return 0;
    }

    if (out_image_base) {
        *out_image_base = image_min;
    }

    if (out_image_size) {
        *out_image_size = image_max - image_min;
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

#define ELF64_PATH_CANDIDATES 4u

static int elf64_has_elf_extension(const char* path) {
    nq_u64 len = 0;

    while (path[len]) {
        len++;
    }

    if (len < 4) {
        return 0;
    }

    const char* ext = path + len - 4;

    return ext[0] == '.' &&
           (ext[1] == 'e' || ext[1] == 'E') &&
           (ext[2] == 'l' || ext[2] == 'L') &&
           (ext[3] == 'f' || ext[3] == 'F');
}

/*
 * Builds the index-th path to try for an app name:
 *   0: <path>
 *   1: <path>.ELF          (only if it has no .elf extension)
 *   2: /apps/<path>        (only if it is not absolute)
 *   3: /apps/<path>.ELF    (both of the above)
 * Returns 0 when that variant does not apply or does not fit.
 */
static int elf64_build_candidate(
    const char* path,
    nq_u32 index,
    char* out,
    nq_u64 out_size
) {
    int add_prefix = (index & 2u) != 0;
    int add_ext = (index & 1u) != 0;
    nq_u64 n = 0;

    if (add_prefix && path[0] == '/') {
        return 0;
    }

    if (add_ext && elf64_has_elf_extension(path)) {
        return 0;
    }

    const char* parts[3];

    parts[0] = add_prefix ? "/apps/" : "";
    parts[1] = path;
    parts[2] = add_ext ? ".ELF" : "";

    for (nq_u32 p = 0; p < 3; p++) {
        for (nq_u64 i = 0; parts[p][i]; i++) {
            if (n + 1 >= out_size) {
                return 0;
            }

            out[n++] = parts[p][i];
        }
    }

    out[n] = 0;

    return 1;
}

/*
 * Reads the ELF file and builds the current task's user address space.
 *
 * Runs with the preemption guard held by the caller: the VFS, PMM and page
 * table code used here are shared kernel state, and two apps spawned back to
 * back would otherwise load at the same time.
 */
static int elf64_load_image(
    const nq_u8* file_buf,
    nq_u64 file_size,
    nq_u64* out_entry,
    nq_u64* out_stack_top
) {
    nq_u64 image_base = 0;
    nq_u64 image_size = 0;

    const elf64_ehdr_t* eh = (const elf64_ehdr_t*)file_buf;

    if (!elf64_validate_header(eh, file_size)) {
        return 0;
    }

    if (!elf64_entry_is_valid(eh)) {
        k64_print("elf64: invalid entry point\n");
        return 0;
    }

    /*
     * Create and switch to this task's private user address space before
     * mapping/copying ELF segments. From this point on, writes to the fixed
     * ELF virtual addresses go into this process's private physical pages.
     */
    if (!process64_current_create_user_address_space()) {
        k64_print("elf64: failed to create user address space\n");
        return 0;
    }

    if (!elf64_load_segments(
            eh,
            file_buf,
            file_size,
            &image_base,
            &image_size)) {
        return 0;
    }

    nq_u64 user_stack_base = ELF64_USER_STACK_BASE;
    nq_u64 user_stack_top = ELF64_USER_STACK_TOP;

    user_stack_top &= ~0xFull;

    if (!paging64_alloc_map_user_range(
            process64_current_pml4(),
            user_stack_base,
            ELF64_USER_STACK_SIZE,
            PAGE64_USER | PAGE64_WRITABLE)) {
        k64_print("elf64: failed to allocate/map user stack\n");
        return 0;
    }

    mem_zero64((void*)user_stack_base, ELF64_USER_STACK_SIZE);

    /*
     * Track metadata for ps/debug output. Old fixed-address save/restore
     * swapping is disabled now that each user app has its own address space.
     */
    if (!process64_user_memory_setup(
            image_base,
            image_size,
            user_stack_base,
            ELF64_USER_STACK_SIZE)) {
        k64_print("elf64: failed to setup user memory swap state\n");
        return 0;
    }

    *out_entry = eh->e_entry;
    *out_stack_top = user_stack_top;

    return 1;
}

int elf64_exec_file(const char* path) {
    nq_u64 entry = 0;
    nq_u64 user_stack_top = 0;

    if (!path || !path[0]) {
        k64_print("elf64: missing path\n");
        return 0;
    }

    elf64_debug("elf64: loading ");
    elf64_debug(path);
    elf64_debug("\n");

    /*
     * The file is read into a kernel heap buffer sized to the file (direct
     * map), not into BSS: a big static buffer made the kernel image larger
     * than the memory the UEFI loader reserves for it.
     */
    int file_read = 0;
    int loaded = 0;
    char candidate[ELF64_MAX_PATH];

    /*
     * Accept "run cube" as well as "run /apps/CUBE.ELF": try the path as
     * typed, then with .ELF added, then the same two under /apps/.
     */
    for (nq_u32 c = 0; c < ELF64_PATH_CANDIDATES && !file_read; c++) {
        void* file_buf = 0;
        nq_u64 file_size = 0;

        if (!elf64_build_candidate(path, c, candidate, sizeof(candidate))) {
            continue;
        }

        process64_preempt_disable();

        file_read = vfs64_read_file_alloc(
            candidate,
            ELF64_MAX_IMAGE_SIZE,
            &file_buf,
            &file_size
        );

        if (file_read) {
            loaded = elf64_load_image(
                (const nq_u8*)file_buf,
                file_size,
                &entry,
                &user_stack_top
            );
        }

        process64_preempt_enable();

        if (file_buf) {
            kfree64(file_buf);
        }
    }

    if (!file_read) {
        k64_print("elf64: cannot read ");
        k64_print(path);
        k64_print(" (not found, also tried /apps/ and .ELF; or larger than ");
        k64_print_dec64(ELF64_MAX_IMAGE_SIZE);
        k64_print(" bytes)\n");
        return 0;
    }

    if (!loaded) {
        return 0;
    }

    elf64_debug("elf64: entering user app\n");
    elf64_debug("entry=");
    elf64_debug_hex(" entry=", entry);
    elf64_debug(" stack=");
    elf64_debug_hex(" stack=", user_stack_top);
    elf64_debug("\n");

    process64_task_t* current = process64_current();

    if (current && current->stack_base && current->stack_size) {
        nq_u64 kernel_stack_top = current->stack_base + current->stack_size;

        gdt64_set_rsp0(kernel_stack_top);

        elf64_debug("elf64: TSS RSP0=");
        elf64_debug_hex(" RSP0=", kernel_stack_top);
        elf64_debug("\n");
    }

    usermode64_enter(entry, user_stack_top);

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

int elf64_spawn_file_ex(const char* path, nq_u32 flags) {
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

    int pid = process64_create_kernel_task_ex(
        "elf64-app",
        elf64_process_entry,
        arg,
        flags
    );

    if (pid < 0) {
        k64_print("elf64: failed to create app task\n");
        kfree64(arg);
        return -1;
    }

    elf64_debug("elf64: spawned ");
    elf64_debug(path);
    elf64_debug(" pid=");
    elf64_debug_dec(" pid=", (nq_u64)pid);
    elf64_debug("\n");

    return pid;
}

int elf64_spawn_file(const char* path) {
    /*
     * Backward-compatible behavior for old callers:
     * spawn into a private terminal/session.
     */
    return elf64_spawn_file_ex(
        path,
        PROCESS64_SPAWN_PRIVATE_TERMINAL
    );
}
