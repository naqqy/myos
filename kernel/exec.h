#ifndef EXEC_H
#define EXEC_H

typedef unsigned int u32;

#define EXEC_FORMAT_NONE 0
#define EXEC_FORMAT_ELF  1

#define EXEC_MAX_IMAGE_PAGES 512

typedef struct exec_image {
    void* load_base;
    u32 load_size;

    u32 entry_addr;
    u32 preferred_base;

    u32 image_start;
    u32 image_end;

    u32 page_count;
    u32 pages[EXEC_MAX_IMAGE_PAGES];

    u32* page_directory;

    int format;
    int in_use;
} exec_image_t;

int exec_load_into_directory(const char* filename, exec_image_t* image, u32* page_directory);
void exec_unload(exec_image_t* image);

#endif