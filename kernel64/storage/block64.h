#ifndef BLOCK64_H
#define BLOCK64_H

#include "naq_boot_info.h"

#define BLOCK64_SECTOR_SIZE 512u
#define BLOCK64_MAX_DEVICES 8

typedef enum block64_device_type {
    BLOCK64_DEVICE_NONE = 0,
    BLOCK64_DEVICE_ATA_PIO,
    BLOCK64_DEVICE_AHCI,
    BLOCK64_DEVICE_USB_MSC,
    BLOCK64_DEVICE_NVME,
    BLOCK64_DEVICE_RAMDISK
} block64_device_type_t;

typedef struct block64_device {
    nq_u32 id;
    const char* name;
    block64_device_type_t type;

    nq_u64 sector_count;
    nq_u32 sector_size;

    int present;
    int read_only;
    int writes_unlocked;
} block64_device_t;

int block64_init(void);

int block64_ready(void);

void block64_print_summary(void);
int block64_available(void);

const block64_device_t* block64_boot_device(void);
const block64_device_t* block64_get_device(nq_u32 id);
nq_u32 block64_device_count(void);

const char* block64_device_type_name(block64_device_type_t type);

void block64_print_devices(void);

int block64_unlock_writes(nq_u32 device_id);
int block64_lock_writes(nq_u32 device_id);

int block64_read(nq_u64 lba, void* buffer, nq_u32 sector_count);
int block64_write(nq_u64 lba, const void* buffer, nq_u32 sector_count);

int block64_read_device(
    nq_u32 device_id,
    nq_u64 lba,
    void* buffer,
    nq_u32 sector_count
);

int block64_write_device(
    nq_u32 device_id,
    nq_u64 lba,
    const void* buffer,
    nq_u32 sector_count
);

#endif