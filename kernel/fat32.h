#ifndef FAT32_H
#define FAT32_H

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

void fat32_init(void);

void fat32_list_root(void);
int fat32_list_path(const char* path);

int fat32_read_file(const char* name83, u8* out_buf, u32 max_bytes, u32* out_size);
int fat32_read_file_path(const char* path, u8* out_buf, u32 max_bytes, u32* out_size);

int fat32_load_file_to_memory(const char* name83, u8* dest, u32 max_bytes, u32* out_size);
int fat32_load_file_to_memory_path(const char* path, u8* dest, u32 max_bytes, u32* out_size);

int fat32_mkdir_path(const char* path);
int fat32_rm_path(const char* path);
int fat32_rmdir_path(const char* path);

#endif