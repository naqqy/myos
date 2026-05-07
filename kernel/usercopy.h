#ifndef USERCOPY_H
#define USERCOPY_H

typedef unsigned int u32;

int user_ptr_readable(u32 user_addr, u32 len);
int user_copy_from(void* kernel_dst, u32 user_src, u32 len);
int user_copy_string(char* kernel_dst, u32 user_src, u32 max_len);
int user_copy_to(u32 user_dst, const void* kernel_src, u32 len);

#endif