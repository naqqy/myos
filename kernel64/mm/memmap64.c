#include "memmap64.h"
#include "../k64_console.h"

/*
 * UEFI memory types.
 * These values match the UEFI spec enum ordering.
 */
#define EFI_RESERVED_MEMORY_TYPE       0
#define EFI_LOADER_CODE                1
#define EFI_LOADER_DATA                2
#define EFI_BOOT_SERVICES_CODE         3
#define EFI_BOOT_SERVICES_DATA         4
#define EFI_RUNTIME_SERVICES_CODE      5
#define EFI_RUNTIME_SERVICES_DATA      6
#define EFI_CONVENTIONAL_MEMORY        7
#define EFI_UNUSABLE_MEMORY            8
#define EFI_ACPI_RECLAIM_MEMORY        9
#define EFI_ACPI_MEMORY_NVS            10
#define EFI_MEMORY_MAPPED_IO           11
#define EFI_MEMORY_MAPPED_IO_PORT      12
#define EFI_PAL_CODE                   13
#define EFI_PERSISTENT_MEMORY          14

typedef struct efi_memory_descriptor64 {
    nq_u32 type;
    nq_u32 pad;
    nq_u64 physical_start;
    nq_u64 virtual_start;
    nq_u64 number_of_pages;
    nq_u64 attribute;
} efi_memory_descriptor64_t;

static k64_mem_region_t g_regions[K64_MEMMAP_MAX_REGIONS];
static nq_u64 g_region_count = 0;
static nq_u64 g_total_usable = 0;
static nq_u64 g_highest_usable = 0;

static void mem_zero(void* ptr, nq_u64 size) {
    nq_u8* p = (nq_u8*)ptr;

    for (nq_u64 i = 0; i < size; i++) {
        p[i] = 0;
    }
}

static const char* uefi_type_name(nq_u32 type) {
    switch (type) {
        case EFI_RESERVED_MEMORY_TYPE:
            return "Reserved";
        case EFI_LOADER_CODE:
            return "LoaderCode";
        case EFI_LOADER_DATA:
            return "LoaderData";
        case EFI_BOOT_SERVICES_CODE:
            return "BootServicesCode";
        case EFI_BOOT_SERVICES_DATA:
            return "BootServicesData";
        case EFI_RUNTIME_SERVICES_CODE:
            return "RuntimeServicesCode";
        case EFI_RUNTIME_SERVICES_DATA:
            return "RuntimeServicesData";
        case EFI_CONVENTIONAL_MEMORY:
            return "Conventional";
        case EFI_UNUSABLE_MEMORY:
            return "Unusable";
        case EFI_ACPI_RECLAIM_MEMORY:
            return "ACPIReclaim";
        case EFI_ACPI_MEMORY_NVS:
            return "ACPINVS";
        case EFI_MEMORY_MAPPED_IO:
            return "MMIO";
        case EFI_MEMORY_MAPPED_IO_PORT:
            return "MMIOPort";
        case EFI_PAL_CODE:
            return "PALCode";
        case EFI_PERSISTENT_MEMORY:
            return "Persistent";
        default:
            return "Unknown";
    }
}

static const char* k64_region_type_name(nq_u32 type) {
    switch (type) {
        case K64_MEM_REGION_USABLE:
            return "usable";
        case K64_MEM_REGION_ACPI:
            return "acpi";
        case K64_MEM_REGION_MMIO:
            return "mmio";
        case K64_MEM_REGION_LOADER:
            return "loader";
        case K64_MEM_REGION_RESERVED:
        default:
            return "reserved";
    }
}

static nq_u32 normalize_uefi_type(nq_u32 uefi_type) {
    /*
     * After ExitBootServices, BootServices memory can usually be reclaimed.
     * For now we mark it as usable. The PMM will reserve kernel/stack/map
     * regions explicitly later.
     */
    if (uefi_type == EFI_CONVENTIONAL_MEMORY ||
        uefi_type == EFI_BOOT_SERVICES_CODE ||
        uefi_type == EFI_BOOT_SERVICES_DATA) {
        return K64_MEM_REGION_USABLE;
    }

    if (uefi_type == EFI_LOADER_CODE ||
        uefi_type == EFI_LOADER_DATA) {
        return K64_MEM_REGION_LOADER;
    }

    if (uefi_type == EFI_ACPI_RECLAIM_MEMORY ||
        uefi_type == EFI_ACPI_MEMORY_NVS) {
        return K64_MEM_REGION_ACPI;
    }

    if (uefi_type == EFI_MEMORY_MAPPED_IO ||
        uefi_type == EFI_MEMORY_MAPPED_IO_PORT) {
        return K64_MEM_REGION_MMIO;
    }

    return K64_MEM_REGION_RESERVED;
}

static void add_region(nq_u64 base, nq_u64 length, nq_u32 type, nq_u32 uefi_type, nq_u64 attributes) {
    nq_u64 end;

    if (length == 0) {
        return;
    }

    if (g_region_count >= K64_MEMMAP_MAX_REGIONS) {
        return;
    }

    end = base + length;
    if (end < base) {
        return;
    }

    g_regions[g_region_count].base = base;
    g_regions[g_region_count].length = length;
    g_regions[g_region_count].type = type;
    g_regions[g_region_count].uefi_type = uefi_type;
    g_regions[g_region_count].attributes = attributes;
    g_region_count++;

    if (type == K64_MEM_REGION_USABLE) {
        g_total_usable += length;

        if (end > g_highest_usable) {
            g_highest_usable = end;
        }
    }
}

void memmap64_init(const naq_boot_info_t* boot) {
    g_region_count = 0;
    g_total_usable = 0;
    g_highest_usable = 0;

    mem_zero(g_regions, sizeof(g_regions));

    if (!boot ||
        boot->magic != NAQ_BOOT_INFO_MAGIC ||
        boot->version != NAQ_BOOT_INFO_VERSION) {
        return;
    }

    if (boot->memory_map == 0 ||
        boot->memory_map_size == 0 ||
        boot->memory_map_desc_size < sizeof(efi_memory_descriptor64_t)) {
        return;
    }

    nq_u8* map = (nq_u8*)boot->memory_map;
    nq_u64 offset = 0;

    while (offset + boot->memory_map_desc_size <= boot->memory_map_size) {
        efi_memory_descriptor64_t* desc =
            (efi_memory_descriptor64_t*)(map + offset);

        nq_u64 base = desc->physical_start;
        nq_u64 length = desc->number_of_pages * 4096ull;
        nq_u32 normalized = normalize_uefi_type(desc->type);

        add_region(
            base,
            length,
            normalized,
            desc->type,
            desc->attribute
        );

        offset += boot->memory_map_desc_size;
    }
}

nq_u64 memmap64_region_count(void) {
    return g_region_count;
}

const k64_mem_region_t* memmap64_region(nq_u64 index) {
    if (index >= g_region_count) {
        return 0;
    }

    return &g_regions[index];
}

nq_u64 memmap64_total_usable_bytes(void) {
    return g_total_usable;
}

nq_u64 memmap64_highest_usable_address(void) {
    return g_highest_usable;
}

void memmap64_print_summary(void) {
    k64_print("Memory regions: ");
    k64_print_dec64(g_region_count);
    k64_print("\n");

    k64_print("Total usable memory: ");
    k64_print_dec64(g_total_usable / 1024ull);
    k64_print(" KiB\n");

    k64_print("Highest usable address: ");
    k64_print_hex64(g_highest_usable);
    k64_print("\n");
}

void memmap64_print_regions(void) {
    for (nq_u64 i = 0; i < g_region_count; i++) {
        const k64_mem_region_t* r = &g_regions[i];

        k64_print("[");
        k64_print_dec64(i);
        k64_print("] ");

        k64_print(k64_region_type_name(r->type));
        k64_print(" / ");
        k64_print(uefi_type_name(r->uefi_type));

        k64_print(" base=");
        k64_print_hex64(r->base);

        k64_print(" len=");
        k64_print_hex64(r->length);

        k64_print(" end=");
        k64_print_hex64(r->base + r->length);

        k64_print("\n");
    }
}