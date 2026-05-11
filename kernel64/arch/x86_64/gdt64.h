#ifndef GDT64_H
#define GDT64_H

#include "naq_boot_info.h"

#define GDT64_KERNEL_CODE 0x08
#define GDT64_KERNEL_DATA 0x10
#define GDT64_USER_DATA   0x1B
#define GDT64_USER_CODE   0x23
#define GDT64_TSS         0x28

#define GDT64_IST_DOUBLE_FAULT 1
#define GDT64_IST_PAGE_FAULT   2

void gdt64_init(const naq_boot_info_t* boot);

void gdt64_set_rsp0(nq_u64 rsp0);
nq_u64 gdt64_get_rsp0(void);

nq_u16 gdt64_kernel_code_selector(void);
nq_u16 gdt64_kernel_data_selector(void);
nq_u16 gdt64_user_code_selector(void);
nq_u16 gdt64_user_data_selector(void);
nq_u16 gdt64_tss_selector(void);

#endif