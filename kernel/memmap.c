#include "memmap.h"

typedef unsigned char u8;
typedef unsigned int  u32;

#define E820_MAP_ADDR    ((u8*)0x5000)
#define E820_MAX_ENTRIES 32
#define PAGE_SIZE        4096

extern void term_print(const char* s);
extern void term_put_char(char c);

static memmap_entry_t g_memmap[MEMMAP_MAX_ENTRIES];
static u32 g_memmap_count = 0;
static u32 g_total_usable_low = 0;
static u32 g_highest_usable_low = 0;

static u32 align_up(u32 value, u32 align) {
    u32 mask = align - 1;

    if ((value & mask) == 0) {
        return value;
    }

    if (value > 0xFFFFFFFFu - mask) {
        return 0xFFFFFFFFu & ~mask;
    }

    return (value + mask) & ~mask;
}

static u32 align_down(u32 value, u32 align) {
    return value & ~(align - 1);
}

static int add_overflows_u32(u32 a, u32 b, u32* out) {
    u32 result = a + b;

    if (result < a) {
        return 1;
    }

    if (out) {
        *out = result;
    }

    return 0;
}

static void print_hex32(u32 value) {
    const char* hex = "0123456789ABCDEF";
    for (int i = 7; i >= 0; i--) {
        term_put_char(hex[(value >> (i * 4)) & 0xF]);
    }
}

static void print_dec32(u32 value) {
    char buf[16];
    int i = 0;

    if (value == 0) {
        term_put_char('0');
        return;
    }

    while (value > 0) {
        buf[i++] = '0' + (value % 10);
        value /= 10;
    }

    while (i > 0) {
        term_put_char(buf[--i]);
    }
}

void memmap_init(void) {
    u32 raw_count = *(u32*)E820_MAP_ADDR;
    if (raw_count > E820_MAX_ENTRIES) {
        raw_count = E820_MAX_ENTRIES;
    }

    memmap_entry_t* raw = (memmap_entry_t*)(E820_MAP_ADDR + 4);

    g_memmap_count = 0;
    g_total_usable_low = 0;
    g_highest_usable_low = 0;

    for (u32 i = 0; i < raw_count; i++) {
        memmap_entry_t entry = raw[i];
        u32 region_end;

        // Basic validation
        if (entry.length_low == 0 && entry.length_high == 0) {
            continue; // Skip zero-length entries
        }

        if (entry.type == 0) {
            continue; // Invalid type
        }

        if (g_memmap_count >= MEMMAP_MAX_ENTRIES) {
            break;
        }

        g_memmap[g_memmap_count++] = entry;

        if (entry.type != MEMMAP_TYPE_USABLE) {
            continue;
        }

        if (entry.base_high != 0 || entry.length_high != 0) {
            continue;
        }

        if (add_overflows_u32(entry.base_low, entry.length_low, &region_end)) {
            continue;
        }

        u32 start = align_up(entry.base_low, PAGE_SIZE);
        u32 end   = align_down(region_end, PAGE_SIZE);

        if (end > start) {
            g_total_usable_low += (end - start);
            if (end > g_highest_usable_low) {
                g_highest_usable_low = end;
            }
        }
    }
}

u32 memmap_count(void) {
    return g_memmap_count;
}

const memmap_entry_t* memmap_entry(u32 index) {
    if (index >= g_memmap_count) {
        return 0;
    }
    return &g_memmap[index];
}

u32 memmap_total_usable_low(void) {
    return g_total_usable_low;
}

u32 memmap_highest_usable_low(void) {
    return g_highest_usable_low;
}

int memmap_find_largest_usable_low(u32* out_base, u32* out_length) {
    u32 best_base = 0;
    u32 best_len = 0;

    for (u32 i = 0; i < g_memmap_count; i++) {
        const memmap_entry_t* e = &g_memmap[i];

        if (e->type != MEMMAP_TYPE_USABLE) continue;
        if (e->base_high != 0 || e->length_high != 0) continue;

        u32 region_end;
        if (add_overflows_u32(e->base_low, e->length_low, &region_end)) {
            continue;
        }

        u32 start = align_up(e->base_low, PAGE_SIZE);
        u32 end   = align_down(region_end, PAGE_SIZE);

        if (end > start) {
            u32 len = end - start;
            if (len > best_len) {
                best_base = start;
                best_len = len;
            }
        }
    }

    if (best_len == 0) {
        return 0;
    }

    if (out_base) *out_base = best_base;
    if (out_length) *out_length = best_len;
    return 1;
}

int memmap_find_first_usable_above(u32 min_addr, u32* out_base, u32* out_length) {
    for (u32 i = 0; i < g_memmap_count; i++) {
        const memmap_entry_t* e = &g_memmap[i];

        if (e->type != MEMMAP_TYPE_USABLE) continue;
        if (e->base_high != 0 || e->length_high != 0) continue;

        u32 start = e->base_low;
        u32 end;

        if (add_overflows_u32(e->base_low, e->length_low, &end)) {
            continue;
        }

        if (end <= min_addr) continue;
        if (start < min_addr) start = min_addr;

        start = align_up(start, PAGE_SIZE);
        end   = align_down(end, PAGE_SIZE);

        if (end > start) {
            if (out_base) *out_base = start;
            if (out_length) *out_length = end - start;
            return 1;
        }
    }

    return 0;
}

int memmap_is_usable_range_low(u32 base, u32 length) {
    if (length == 0) return 1;

    u32 end;
    if (add_overflows_u32(base, length, &end)) {
        return 0;
    }

    for (u32 i = 0; i < g_memmap_count; i++) {
        const memmap_entry_t* e = &g_memmap[i];

        if (e->type != MEMMAP_TYPE_USABLE) continue;
        if (e->base_high != 0 || e->length_high != 0) continue;

        u32 e_start = e->base_low;
        u32 e_end;

        if (add_overflows_u32(e->base_low, e->length_low, &e_end)) {
            continue;
        }

        if (base >= e_start && end <= e_end) {
            return 1;
        }
    }

    return 0;
}

void memmap_print(void) {
    term_print("E820 memory map entries: ");
    print_dec32(g_memmap_count);
    term_print("\n");

    for (u32 i = 0; i < g_memmap_count; i++) {
        const memmap_entry_t* e = &g_memmap[i];

        term_print("  [");
        print_dec32(i);
        term_print("] base=");
        print_hex32(e->base_high);
        print_hex32(e->base_low);

        term_print(" len=");
        print_hex32(e->length_high);
        print_hex32(e->length_low);

        term_print(" type=");
        print_dec32(e->type);

        if (e->type == MEMMAP_TYPE_USABLE) {
            term_print(" usable");
        }

        term_print("\n");
    }

    term_print("Total usable low RAM: ");
    print_dec32(g_total_usable_low / 1024);
    term_print(" KB\n");

    term_print("Highest usable low address: ");
    print_hex32(g_highest_usable_low);
    term_print("\n");
}
