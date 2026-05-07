#include "exec.h"
#include "fat32.h"
#include "pmm.h"
#include "paging.h"

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

extern void kernel_print(const char* s);

#define EXEC_FILE_MAX_SIZE 1048576

#define ELF_MAGIC0 0x7F
#define ELF_MAGIC1 'E'
#define ELF_MAGIC2 'L'
#define ELF_MAGIC3 'F'

#define ET_EXEC 2
#define EM_386  3

#define PT_LOAD 1

#define ELF_CLASS_32 1
#define ELF_DATA_LSB 1

#define PAGE_SIZE 4096

typedef struct __attribute__((packed)) elf32_ehdr {
    u8  e_ident[16];
    u16 e_type;
    u16 e_machine;
    u32 e_version;
    u32 e_entry;
    u32 e_phoff;
    u32 e_shoff;
    u32 e_flags;
    u16 e_ehsize;
    u16 e_phentsize;
    u16 e_phnum;
    u16 e_shentsize;
    u16 e_shnum;
    u16 e_shstrndx;
} elf32_ehdr_t;

typedef struct __attribute__((packed)) elf32_phdr {
    u32 p_type;
    u32 p_offset;
    u32 p_vaddr;
    u32 p_paddr;
    u32 p_filesz;
    u32 p_memsz;
    u32 p_flags;
    u32 p_align;
} elf32_phdr_t;

static void mem_zero(u8* dst, u32 count) {
    for (u32 i = 0; i < count; i++) {
        dst[i] = 0;
    }
}

static u32 align_down(u32 value, u32 align) {
    return value & ~(align - 1);
}

static u32 align_up(u32 value, u32 align) {
    return (value + align - 1) & ~(align - 1);
}

static int range_ok(u32 offset, u32 size, u32 file_size) {
    if (offset > file_size) {
        return 0;
    }

    if (size > file_size - offset) {
        return 0;
    }

    return 1;
}

static void exec_image_reset(exec_image_t* image) {
    image->load_base = 0;
    image->load_size = 0;
    image->entry_addr = 0;
    image->preferred_base = 0;
    image->image_start = 0;
    image->image_end = 0;
    image->page_count = 0;
    image->page_directory = 0;
    image->format = EXEC_FORMAT_NONE;
    image->in_use = 0;

    for (u32 i = 0; i < EXEC_MAX_IMAGE_PAGES; i++) {
        image->pages[i] = 0;
    }
}

static int elf_validate_header(elf32_ehdr_t* ehdr, u32 file_size) {
    if (file_size < sizeof(elf32_ehdr_t)) {
        kernel_print("ELF too small.\n");
        return 0;
    }

    if (ehdr->e_ident[0] != ELF_MAGIC0 ||
        ehdr->e_ident[1] != ELF_MAGIC1 ||
        ehdr->e_ident[2] != ELF_MAGIC2 ||
        ehdr->e_ident[3] != ELF_MAGIC3) {
        kernel_print("Bad ELF magic.\n");
        return 0;
    }

    if (ehdr->e_ident[4] != ELF_CLASS_32) {
        kernel_print("ELF is not 32-bit.\n");
        return 0;
    }

    if (ehdr->e_ident[5] != ELF_DATA_LSB) {
        kernel_print("ELF is not little-endian.\n");
        return 0;
    }

    if (ehdr->e_type != ET_EXEC || ehdr->e_machine != EM_386) {
        kernel_print("Unsupported ELF type/machine.\n");
        return 0;
    }

    if (ehdr->e_phentsize != sizeof(elf32_phdr_t)) {
        kernel_print("Unexpected ELF program header size.\n");
        return 0;
    }

    if (!range_ok(ehdr->e_phoff,
                  (u32)ehdr->e_phentsize * ehdr->e_phnum,
                  file_size)) {
        kernel_print("ELF program headers outside file.\n");
        return 0;
    }

    return 1;
}

static int elf_find_load_range(
    u8* file_buffer,
    u32 file_size,
    elf32_ehdr_t* ehdr,
    u32* out_min_vaddr,
    u32* out_max_vaddr
) {
    elf32_phdr_t* phdrs = (elf32_phdr_t*)(file_buffer + ehdr->e_phoff);

    u32 min_vaddr = 0xFFFFFFFFu;
    u32 max_vaddr = 0;

    for (u32 i = 0; i < ehdr->e_phnum; i++) {
        elf32_phdr_t* ph = &phdrs[i];

        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) {
            continue;
        }

        if (ph->p_filesz > ph->p_memsz) {
            kernel_print("ELF segment filesz > memsz.\n");
            return 0;
        }

        if (!range_ok(ph->p_offset, ph->p_filesz, file_size)) {
            kernel_print("ELF segment outside file.\n");
            return 0;
        }

        u32 seg_start = align_down(ph->p_vaddr, PAGE_SIZE);
        u32 seg_end = align_up(ph->p_vaddr + ph->p_memsz, PAGE_SIZE);

        if (seg_start < min_vaddr) {
            min_vaddr = seg_start;
        }

        if (seg_end > max_vaddr) {
            max_vaddr = seg_end;
        }
    }

    if (min_vaddr == 0xFFFFFFFFu || max_vaddr <= min_vaddr) {
        kernel_print("ELF has no loadable segments.\n");
        return 0;
    }

    if (ehdr->e_entry < min_vaddr || ehdr->e_entry >= max_vaddr) {
        kernel_print("ELF entry outside image range.\n");
        return 0;
    }

    *out_min_vaddr = min_vaddr;
    *out_max_vaddr = max_vaddr;
    return 1;
}

static int exec_alloc_and_map_pages(
    exec_image_t* image,
    u32* dir,
    u32 image_start,
    u32 image_end
) {
    u32 page_count = (image_end - image_start) / PAGE_SIZE;

    if (page_count > EXEC_MAX_IMAGE_PAGES) {
        kernel_print("ELF image too large for page list.\n");
        return 0;
    }

    image->page_count = 0;

    for (u32 i = 0; i < page_count; i++) {
        u32 virt = image_start + i * PAGE_SIZE;
        u32 phys = pmm_alloc_frame();

        if (!phys) {
            kernel_print("exec: pmm_alloc_frame failed.\n");
            return 0;
        }

        mem_zero((u8*)phys, PAGE_SIZE);

        if (!paging_map_page_in_directory(
                dir,
                virt,
                phys,
                PAGE_WRITABLE | PAGE_USER)) {
            pmm_free_frame(phys);
            kernel_print("exec: map page failed.\n");
            return 0;
        }

        image->pages[image->page_count++] = phys;
    }

    return 1;
}

static int exec_write_byte_to_process(
    u32* dir,
    u32 virt_addr,
    u8 value
) {
    u32 phys = paging_get_physical_in_directory(dir, virt_addr);

    if (!phys) {
        return 0;
    }

    *((u8*)phys) = value;
    return 1;
}

static int exec_copy_to_process(
    u32* dir,
    u32 dest_virt,
    const u8* src,
    u32 count
) {
    for (u32 i = 0; i < count; i++) {
        if (!exec_write_byte_to_process(dir, dest_virt + i, src[i])) {
            return 0;
        }
    }

    return 1;
}

static int elf_load_segments_into_directory(
    u8* file_buffer,
    elf32_ehdr_t* ehdr,
    u32* dir
) {
    elf32_phdr_t* phdrs = (elf32_phdr_t*)(file_buffer + ehdr->e_phoff);

    for (u32 i = 0; i < ehdr->e_phnum; i++) {
        elf32_phdr_t* ph = &phdrs[i];

        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) {
            continue;
        }

        if (!exec_copy_to_process(
                dir,
                ph->p_vaddr,
                file_buffer + ph->p_offset,
                ph->p_filesz)) {
            kernel_print("exec: segment copy failed.\n");
            return 0;
        }
    }

    return 1;
}

static int exec_load_elf_into_directory(
    u8* file_buffer,
    u32 file_size,
    exec_image_t* image,
    u32* dir
) {
    elf32_ehdr_t* ehdr = (elf32_ehdr_t*)file_buffer;

    if (!elf_validate_header(ehdr, file_size)) {
        return 0;
    }

    u32 min_vaddr;
    u32 max_vaddr;

    if (!elf_find_load_range(file_buffer, file_size, ehdr, &min_vaddr, &max_vaddr)) {
        return 0;
    }

    image->image_start = min_vaddr;
    image->image_end = max_vaddr;
    image->load_size = max_vaddr - min_vaddr;
    image->preferred_base = min_vaddr;
    image->entry_addr = ehdr->e_entry;
    image->page_directory = dir;
    image->format = EXEC_FORMAT_ELF;

    if (!exec_alloc_and_map_pages(image, dir, min_vaddr, max_vaddr)) {
        exec_unload(image);
        return 0;
    }

    if (!elf_load_segments_into_directory(file_buffer, ehdr, dir)) {
        exec_unload(image);
        return 0;
    }

    image->load_base = (void*)min_vaddr;
    image->in_use = 1;

    kernel_print("ELF executable loaded into process address space.\n");
    return 1;
}

int exec_load_into_directory(const char* filename, exec_image_t* image, u32* page_directory) {
    u32 file_size = 0;
    static u8 file_buffer[EXEC_FILE_MAX_SIZE];

    if (!image) {
        kernel_print("exec_load: null image\n");
        return 0;
    }

    if (!page_directory) {
        kernel_print("exec_load: null page directory\n");
        return 0;
    }

    exec_image_reset(image);

    kernel_print("Loading executable: ");
    kernel_print(filename);
    kernel_print("\n");

    if (!fat32_load_file_to_memory_path(filename, file_buffer, EXEC_FILE_MAX_SIZE, &file_size)) {
        kernel_print("Executable load failed.\n");
        return 0;
    }

    if (file_size >= 4 &&
        file_buffer[0] == ELF_MAGIC0 &&
        file_buffer[1] == ELF_MAGIC1 &&
        file_buffer[2] == ELF_MAGIC2 &&
        file_buffer[3] == ELF_MAGIC3) {
        return exec_load_elf_into_directory(file_buffer, file_size, image, page_directory);
    }

    kernel_print("Unknown executable format. Expected ELF.\n");
    return 0;
}

void exec_unload(exec_image_t* image) {
    if (!image) {
        return;
    }

    if (image->page_directory) {
        for (u32 i = 0; i < image->page_count; i++) {
            u32 virt = image->image_start + i * PAGE_SIZE;

            paging_unmap_page_in_directory(image->page_directory, virt);

            if (image->pages[i]) {
                pmm_free_frame(image->pages[i]);
            }
        }
    }

    exec_image_reset(image);
}