#ifndef GDT64_H
#define GDT64_H

#include "naq_boot_info.h"

#define GDT64_ACCESS_KERNEL_CODE 0x9A
#define GDT64_ACCESS_KERNEL_DATA 0x92
#define GDT64_ACCESS_USER_DATA   0xF2
#define GDT64_ACCESS_USER_CODE   0xFA
#define GDT64_ACCESS_TSS         0x89

#define GDT64_FLAG_LONG_MODE     0x20

/*
 * GDT selector layout:
 *
 *   0x00 null
 *   0x08 kernel code
 *   0x10 kernel data
 *   0x18 user data
 *   0x20 user code
 *   0x28 TSS
 *
 * Ring 3 selectors need RPL=3:
 *
 *   user data = 0x18 | 3 = 0x1B
 *   user code = 0x20 | 3 = 0x23
 */
#define GDT64_KERNEL_CODE     0x08
#define GDT64_KERNEL_DATA     0x10
#define GDT64_USER_DATA       0x18
#define GDT64_USER_CODE       0x20
#define GDT64_TSS             0x28

#define GDT64_USER_DATA_SEL   (GDT64_USER_DATA | 3)
#define GDT64_USER_CODE_SEL   (GDT64_USER_CODE | 3)

#define GDT64_IST_STACK_SIZE  16384u

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