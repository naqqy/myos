#ifndef PAGING_H
#define PAGING_H

typedef unsigned int u32;

#define PAGE_PRESENT  0x001
#define PAGE_WRITABLE 0x002
#define PAGE_USER     0x004

void paging_init(void);

void paging_map_page(u32 virt_addr, u32 phys_addr, u32 flags);
void paging_unmap_page(u32 virt_addr);
int  paging_is_mapped(u32 virt_addr);
u32  paging_get_physical(u32 virt_addr);
void paging_identity_map_range(u32 start, u32 length, u32 flags);

/*
 * Address-space support.
 *
 * Stage 1:
 * process directories clone the kernel directory entries.
 * This does not isolate memory yet, but it gives each process
 * its own CR3/page-directory object.
 */
u32* paging_kernel_directory(void);
u32* paging_current_directory(void);

u32* paging_create_process_directory(void);
void paging_destroy_process_directory(u32* dir);

void paging_switch_directory(u32* dir);
void paging_switch_kernel_directory(void);

int paging_map_page_in_directory(u32* dir, u32 virt_addr, u32 phys_addr, u32 flags);
int paging_unmap_page_in_directory(u32* dir, u32 virt_addr);
u32 paging_get_physical_in_directory(u32* dir, u32 virt_addr);

#endif