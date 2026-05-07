#ifndef MEMMAP_H
#define MEMMAP_H

typedef unsigned int u32;

#define MEMMAP_MAX_ENTRIES 32
#define MEMMAP_TYPE_USABLE 1

typedef struct {
    u32 base_low;
    u32 base_high;
    u32 length_low;
    u32 length_high;
    u32 type;
    u32 acpi_ext;
} memmap_entry_t;

void memmap_init(void);
u32  memmap_count(void);
const memmap_entry_t* memmap_entry(u32 index);

u32  memmap_total_usable_low(void);
u32  memmap_highest_usable_low(void);
int  memmap_find_largest_usable_low(u32* out_base, u32* out_length);
int  memmap_find_first_usable_above(u32 min_addr, u32* out_base, u32* out_length);

int  memmap_is_usable_range_low(u32 base, u32 length);
void memmap_print(void);

#endif