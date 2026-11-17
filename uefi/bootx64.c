#include <efi.h>
#include <efilib.h>
#include "naq_boot_info.h"

/* ============================================================
 * NAQ OS UEFI Loader
 * ------------------------------------------------------------
 * Responsibilities:
 *   1. Find GOP framebuffer info.
 *   2. Load KERNEL64.BIN to a fixed physical address.
 *   3. Load optional INITRD.NAQ into RAM.
 *   4. Allocate a kernel64 stack.
 *   5. Get the UEFI memory map.
 *   6. ExitBootServices().
 *   7. Jump to kernel64.
 * ============================================================ */

#define UEFI_PAGE_SIZE 4096u

#define UEFI_KERNEL64_LOAD_ADDR 0x00100000ULL

/*
 * Amount UEFI actually allocates at the fixed kernel load address.
 * Keep this at 4 MiB because OVMF may fail larger fixed allocations.
 */
#define UEFI_KERNEL64_ALLOC_PAGES 1024u  /* 4 MiB */

/*
 * Amount kernel64 PMM should reserve around the loaded kernel.
 * This does not need to be fully allocated by UEFI.
 */
#define UEFI_KERNEL64_PMM_RESERVE_PAGES 4096u  /* 16 MiB */

/*
 * 16 pages = 64 KiB kernel stack.
 */
#define UEFI_KERNEL_STACK_PAGES 16u

static naq_boot_info_t g_boot_info;

/* ============================================================
 * Small utilities
 * ============================================================ */

static void zero_memory(void* ptr, UINTN size) {
    UINT8* p = (UINT8*)ptr;

    if (!p) {
        return;
    }

    for (UINTN i = 0; i < size; i++) {
        p[i] = 0;
    }
}

static const CHAR16* pixel_format_name(EFI_GRAPHICS_PIXEL_FORMAT fmt) {
    switch (fmt) {
        case PixelRedGreenBlueReserved8BitPerColor:
            return L"RGBx 8:8:8:8";

        case PixelBlueGreenRedReserved8BitPerColor:
            return L"BGRx 8:8:8:8";

        case PixelBitMask:
            return L"BitMask";

        case PixelBltOnly:
            return L"BltOnly";

        default:
            return L"Unknown";
    }
}

static void wait_for_key(void) {
    EFI_INPUT_KEY key;
    UINTN index = 0;

    uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);

    uefi_call_wrapper(
        BS->WaitForEvent,
        3,
        1,
        &ST->ConIn->WaitForKey,
        &index
    );

    uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);
}

/* ============================================================
 * GOP / framebuffer discovery
 * ============================================================ */

static EFI_STATUS locate_gop(EFI_GRAPHICS_OUTPUT_PROTOCOL** out_gop) {
    EFI_STATUS status;
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;

    if (!out_gop) {
        return EFI_INVALID_PARAMETER;
    }

    *out_gop = NULL;

    status = uefi_call_wrapper(
        BS->LocateProtocol,
        3,
        &gop_guid,
        NULL,
        (VOID**)out_gop
    );

    return status;
}

static void fill_framebuffer_boot_info(EFI_GRAPHICS_OUTPUT_PROTOCOL* gop) {
    if (!gop || !gop->Mode || !gop->Mode->Info) {
        return;
    }

    g_boot_info.framebuffer_base =
        (nq_u64)gop->Mode->FrameBufferBase;

    g_boot_info.framebuffer_size =
        (nq_u64)gop->Mode->FrameBufferSize;

    g_boot_info.framebuffer_width =
        (nq_u32)gop->Mode->Info->HorizontalResolution;

    g_boot_info.framebuffer_height =
        (nq_u32)gop->Mode->Info->VerticalResolution;

    g_boot_info.framebuffer_pixels_per_scanline =
        (nq_u32)gop->Mode->Info->PixelsPerScanLine;

    g_boot_info.framebuffer_pixel_format =
        (nq_u32)gop->Mode->Info->PixelFormat;
}

/*
 * Debug/compat GOP mode picker.
 *
 * NAQ_TARGET_GOP_WIDTH/HEIGHT let us reproduce hardware framebuffer sizes
 * inside QEMU. QEMU must expose enough VGA RAM too, for example:
 *
 *   QEMU_RAM=2G QEMU_VGAMEM_MB=64 QEMU_DISPLAY=gtk ./build.sh uefi-qemu
 *
 * If the exact target is unavailable, we print every GOP mode and keep the
 * firmware default instead of guessing.
 */
#ifndef NAQ_TARGET_GOP_WIDTH
#define NAQ_TARGET_GOP_WIDTH  2560u
#endif

#ifndef NAQ_TARGET_GOP_HEIGHT
#define NAQ_TARGET_GOP_HEIGHT 1080u
#endif

#ifndef NAQ_FORCE_GOP_MODE
#define NAQ_FORCE_GOP_MODE 1u
#endif

static int gop_pixel_format_supported(EFI_GRAPHICS_PIXEL_FORMAT fmt) {
    return fmt == PixelRedGreenBlueReserved8BitPerColor ||
           fmt == PixelBlueGreenRedReserved8BitPerColor;
}

/* ============================================================
 * File loading from the boot volume
 * ============================================================ */

static EFI_STATUS load_file_from_boot_volume(
    EFI_HANDLE image_handle,
    CHAR16* path,
    VOID** out_buffer,
    UINTN* out_size
) {
    EFI_STATUS status;

    EFI_GUID loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID simple_fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_GUID file_info_guid = EFI_FILE_INFO_ID;

    EFI_LOADED_IMAGE_PROTOCOL* loaded_image = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs = NULL;
    EFI_FILE_PROTOCOL* root = NULL;
    EFI_FILE_PROTOCOL* file = NULL;

    EFI_FILE_INFO* file_info = NULL;
    UINTN file_info_size = 0;

    VOID* buffer = NULL;
    UINTN file_size = 0;
    UINTN read_size = 0;

    if (!path || !out_buffer || !out_size) {
        return EFI_INVALID_PARAMETER;
    }

    *out_buffer = NULL;
    *out_size = 0;

    status = uefi_call_wrapper(
        BS->HandleProtocol,
        3,
        image_handle,
        &loaded_image_guid,
        (VOID**)&loaded_image
    );

    if (EFI_ERROR(status)) {
        return status;
    }

    status = uefi_call_wrapper(
        BS->HandleProtocol,
        3,
        loaded_image->DeviceHandle,
        &simple_fs_guid,
        (VOID**)&fs
    );

    if (EFI_ERROR(status)) {
        return status;
    }

    status = uefi_call_wrapper(
        fs->OpenVolume,
        2,
        fs,
        &root
    );

    if (EFI_ERROR(status)) {
        return status;
    }

    status = uefi_call_wrapper(
        root->Open,
        5,
        root,
        &file,
        path,
        EFI_FILE_MODE_READ,
        0
    );

    if (EFI_ERROR(status)) {
        uefi_call_wrapper(root->Close, 1, root);
        return status;
    }

    status = uefi_call_wrapper(
        file->GetInfo,
        4,
        file,
        &file_info_guid,
        &file_info_size,
        NULL
    );

    if (status != EFI_BUFFER_TOO_SMALL) {
        uefi_call_wrapper(file->Close, 1, file);
        uefi_call_wrapper(root->Close, 1, root);
        return status;
    }

    status = uefi_call_wrapper(
        BS->AllocatePool,
        3,
        EfiLoaderData,
        file_info_size,
        (VOID**)&file_info
    );

    if (EFI_ERROR(status)) {
        uefi_call_wrapper(file->Close, 1, file);
        uefi_call_wrapper(root->Close, 1, root);
        return status;
    }

    status = uefi_call_wrapper(
        file->GetInfo,
        4,
        file,
        &file_info_guid,
        &file_info_size,
        file_info
    );

    if (EFI_ERROR(status)) {
        uefi_call_wrapper(BS->FreePool, 1, file_info);
        uefi_call_wrapper(file->Close, 1, file);
        uefi_call_wrapper(root->Close, 1, root);
        return status;
    }

    file_size = (UINTN)file_info->FileSize;

    uefi_call_wrapper(BS->FreePool, 1, file_info);
    file_info = NULL;

    if (file_size == 0) {
        uefi_call_wrapper(file->Close, 1, file);
        uefi_call_wrapper(root->Close, 1, root);
        return EFI_LOAD_ERROR;
    }

    status = uefi_call_wrapper(
        BS->AllocatePool,
        3,
        EfiLoaderData,
        file_size,
        &buffer
    );

    if (EFI_ERROR(status)) {
        uefi_call_wrapper(file->Close, 1, file);
        uefi_call_wrapper(root->Close, 1, root);
        return status;
    }

    read_size = file_size;

    status = uefi_call_wrapper(
        file->Read,
        3,
        file,
        &read_size,
        buffer
    );

    uefi_call_wrapper(file->Close, 1, file);
    uefi_call_wrapper(root->Close, 1, root);

    if (EFI_ERROR(status)) {
        uefi_call_wrapper(BS->FreePool, 1, buffer);
        return status;
    }

    if (read_size != file_size) {
        uefi_call_wrapper(BS->FreePool, 1, buffer);
        return EFI_LOAD_ERROR;
    }

    *out_buffer = buffer;
    *out_size = file_size;

    return EFI_SUCCESS;
}

/* ============================================================
 * Kernel loading
 * ============================================================ */

static EFI_STATUS load_kernel_file_with_fallbacks(
    EFI_HANDLE image_handle,
    VOID** out_buffer,
    UINTN* out_size
) {
    EFI_STATUS status = EFI_NOT_FOUND;

    CHAR16* kernel_paths[] = {
        L"\\KERNEL64.BIN",
        L"KERNEL64.BIN",
        L"\\EFI\\BOOT\\KERNEL64.BIN",
        L"EFI\\BOOT\\KERNEL64.BIN",
        NULL
    };

    if (!out_buffer || !out_size) {
        return EFI_INVALID_PARAMETER;
    }

    *out_buffer = NULL;
    *out_size = 0;

    for (UINTN i = 0; kernel_paths[i] != NULL; i++) {
        Print(L"Trying kernel path: %s\r\n", kernel_paths[i]);

        status = load_file_from_boot_volume(
            image_handle,
            kernel_paths[i],
            out_buffer,
            out_size
        );

        if (!EFI_ERROR(status)) {
            Print(L"Loaded kernel from: %s\r\n", kernel_paths[i]);
            return EFI_SUCCESS;
        }
    }

    return status;
}

static EFI_STATUS load_kernel64_to_fixed_address(
    EFI_HANDLE image_handle,
    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop,
    EFI_PHYSICAL_ADDRESS load_addr,
    UINTN* out_kernel_file_size
) {
    EFI_STATUS status;
    VOID* file_buffer = NULL;
    UINTN file_size = 0;

    UINTN file_pages = 0;
    UINTN alloc_pages = 0;
    UINTN alloc_bytes = 0;

    UINTN reserve_pages = UEFI_KERNEL64_PMM_RESERVE_PAGES;
    UINTN reserve_bytes = reserve_pages * UEFI_PAGE_SIZE;

    EFI_PHYSICAL_ADDRESS requested_addr = load_addr;

    if (!out_kernel_file_size) {
        return EFI_INVALID_PARAMETER;
    }

    *out_kernel_file_size = 0;

    status = load_kernel_file_with_fallbacks(
        image_handle,
        &file_buffer,
        &file_size
    );

    if (EFI_ERROR(status)) {
        return status;
    }

    file_pages = (file_size + UEFI_PAGE_SIZE - 1u) / UEFI_PAGE_SIZE;
    alloc_pages = file_pages;

    if (alloc_pages < UEFI_KERNEL64_ALLOC_PAGES) {
        alloc_pages = UEFI_KERNEL64_ALLOC_PAGES;
    }

    alloc_bytes = alloc_pages * UEFI_PAGE_SIZE;

    Print(L"Kernel64 file size: %lu bytes\r\n", file_size);
    Print(L"Kernel64 UEFI allocated size: %lu bytes\r\n", alloc_bytes);
    Print(L"Kernel64 PMM reserved size: %lu bytes\r\n", reserve_bytes);
    Print(L"Reserving kernel region at 0x%lx...\r\n", load_addr);

    status = uefi_call_wrapper(
        BS->AllocatePages,
        4,
        AllocateAddress,
        EfiLoaderCode,
        alloc_pages,
        &requested_addr
    );

    if (EFI_ERROR(status)) {
        Print(L"Kernel region allocation failed: %r\r\n", status);
        uefi_call_wrapper(BS->FreePool, 1, file_buffer);
        return status;
    }

    zero_memory((VOID*)(UINTN)load_addr, alloc_bytes);
    CopyMem((VOID*)(UINTN)load_addr, file_buffer, file_size);

    uefi_call_wrapper(BS->FreePool, 1, file_buffer);

    g_boot_info.magic = NAQ_BOOT_INFO_MAGIC;
    g_boot_info.version = NAQ_BOOT_INFO_VERSION;

    fill_framebuffer_boot_info(gop);

    g_boot_info.kernel_load_addr = (nq_u64)load_addr;

    /*
     * Intentionally larger than the actual UEFI allocation.
     * PMM uses this to avoid allocating low memory near the flat kernel.
     */
    g_boot_info.kernel_size = (nq_u64)reserve_bytes;

    *out_kernel_file_size = file_size;

    return EFI_SUCCESS;
}

/* ============================================================
 * Optional INITRD loading
 * ============================================================ */

static EFI_STATUS load_initrd(
    EFI_HANDLE image_handle,
    naq_boot_info_t* boot
) {
    EFI_STATUS status;
    VOID* file_buffer = NULL;
    UINTN file_size = 0;
    UINTN pages = 0;
    UINTN alloc_bytes = 0;
    EFI_PHYSICAL_ADDRESS addr = 0;

    if (!boot) {
        return EFI_INVALID_PARAMETER;
    }

    boot->initrd_base = 0;
    boot->initrd_size = 0;

    status = load_file_from_boot_volume(
        image_handle,
        L"\\INITRD.NAQ",
        &file_buffer,
        &file_size
    );

    if (EFI_ERROR(status)) {
        Print(L"INITRD.NAQ not found; continuing without initrd.\r\n");
        return EFI_SUCCESS;
    }

    pages = (file_size + UEFI_PAGE_SIZE - 1u) / UEFI_PAGE_SIZE;
    alloc_bytes = pages * UEFI_PAGE_SIZE;

    status = uefi_call_wrapper(
        BS->AllocatePages,
        4,
        AllocateAnyPages,
        EfiLoaderData,
        pages,
        &addr
    );

    if (EFI_ERROR(status)) {
        Print(L"INITRD allocation failed: %r\r\n", status);
        uefi_call_wrapper(BS->FreePool, 1, file_buffer);
        return status;
    }

    zero_memory((VOID*)(UINTN)addr, alloc_bytes);
    CopyMem((VOID*)(UINTN)addr, file_buffer, file_size);

    uefi_call_wrapper(BS->FreePool, 1, file_buffer);

    boot->initrd_base = (nq_u64)addr;
    boot->initrd_size = (nq_u64)file_size;

    Print(L"INITRD.NAQ loaded.\r\n");
    Print(L"INITRD base: 0x%lx\r\n", addr);
    Print(L"INITRD size: %lu bytes\r\n", file_size);

    return EFI_SUCCESS;
}

/* ============================================================
 * Kernel stack allocation
 * ============================================================ */

static EFI_STATUS allocate_kernel_stack(naq_boot_info_t* boot) {
    EFI_STATUS status;
    EFI_PHYSICAL_ADDRESS stack_base = 0;
    UINTN stack_pages = UEFI_KERNEL_STACK_PAGES;
    UINTN stack_size = stack_pages * UEFI_PAGE_SIZE;

    if (!boot) {
        return EFI_INVALID_PARAMETER;
    }

    status = uefi_call_wrapper(
        BS->AllocatePages,
        4,
        AllocateAnyPages,
        EfiLoaderData,
        stack_pages,
        &stack_base
    );

    if (EFI_ERROR(status)) {
        return status;
    }

    zero_memory((VOID*)(UINTN)stack_base, stack_size);

    boot->stack_base = (nq_u64)stack_base;
    boot->stack_size = (nq_u64)stack_size;
    boot->stack_top  = (nq_u64)(stack_base + stack_size);

    return EFI_SUCCESS;
}

/* ============================================================
 * Boot volume identity
 * ============================================================ */

/*
 * Reads the serial number out of the boot volume's FAT boot sector and
 * stores it in the boot info. Failing is harmless: the kernel then simply
 * does not know which disk it came from and keeps every disk locked.
 */
static void read_boot_volume_serial(EFI_HANDLE image_handle) {
    EFI_GUID loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID block_io_guid = EFI_BLOCK_IO_PROTOCOL_GUID;

    EFI_LOADED_IMAGE_PROTOCOL* loaded_image = NULL;
    EFI_BLOCK_IO_PROTOCOL* block_io = NULL;
    UINT8* sector = NULL;

    g_boot_info.boot_volume = 0;

    EFI_STATUS status = uefi_call_wrapper(
        BS->HandleProtocol,
        3,
        image_handle,
        &loaded_image_guid,
        (VOID**)&loaded_image
    );

    if (EFI_ERROR(status) || !loaded_image) {
        return;
    }

    status = uefi_call_wrapper(
        BS->HandleProtocol,
        3,
        loaded_image->DeviceHandle,
        &block_io_guid,
        (VOID**)&block_io
    );

    if (EFI_ERROR(status) || !block_io || !block_io->Media) {
        return;
    }

    UINTN block_size = block_io->Media->BlockSize;

    if (block_size < 512u || block_size > 4096u) {
        return;
    }

    status = uefi_call_wrapper(
        BS->AllocatePool,
        3,
        EfiLoaderData,
        block_size,
        (VOID**)&sector
    );

    if (EFI_ERROR(status) || !sector) {
        return;
    }

    status = uefi_call_wrapper(
        block_io->ReadBlocks,
        5,
        block_io,
        block_io->Media->MediaId,
        (EFI_LBA)0,
        block_size,
        sector
    );

    /*
     * FAT32 keeps "FAT32" at offset 82 and the serial at offset 67.
     */
    if (!EFI_ERROR(status) &&
        sector[510] == 0x55 && sector[511] == 0xAA &&
        sector[82] == 'F' && sector[83] == 'A' && sector[84] == 'T' &&
        sector[85] == '3' && sector[86] == '2') {
        UINT32 serial =
            (UINT32)sector[67] |
            ((UINT32)sector[68] << 8) |
            ((UINT32)sector[69] << 16) |
            ((UINT32)sector[70] << 24);

        g_boot_info.boot_volume = (nq_u64)serial | (1ull << 32);
    }

    uefi_call_wrapper(BS->FreePool, 1, sector);
}

/* ============================================================
 * Display selection
 * ============================================================
 *
 * The kernel has no graphics-card driver: it draws on whatever screen and
 * resolution the firmware hands over, and cannot change either once it is
 * running. So the choice is made here, before the kernel starts.
 *
 * The firmware may offer one "graphics output" per monitor, each with its
 * own list of modes. Pressing D at boot opens a small menu to pick an
 * output and a mode; the choice is saved in \NAQDISP.CFG on the boot
 * volume and applied on every later boot.
 *
 * With nothing saved: use the output that offers the preferred mode
 * (NAQ_TARGET_GOP_WIDTH x HEIGHT) if there is one, otherwise leave the
 * firmware's own choice alone.
 */

#define NAQ_DISPLAY_CFG_PATH  L"\\NAQDISP.CFG"
#define NAQ_DISPLAY_CFG_MAGIC 0x5053444Eu
#define NAQ_MAX_OUTPUTS       8u
#define NAQ_DISPLAY_PROMPT_MS 3000u

/*
 * Set by pressing P at the boot prompt: stop before starting the kernel
 * so the loader's messages can be read.
 */
static int g_pause_before_kernel = 0;

typedef struct naq_display_cfg {
    UINT32 magic;
    UINT32 output;
    UINT32 width;
    UINT32 height;
} naq_display_cfg_t;

static EFI_GRAPHICS_OUTPUT_PROTOCOL* g_outputs[NAQ_MAX_OUTPUTS];
static UINTN g_output_count = 0;

static void collect_outputs(void) {
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_HANDLE* handles = NULL;
    UINTN count = 0;

    g_output_count = 0;

    EFI_STATUS status = uefi_call_wrapper(
        BS->LocateHandleBuffer,
        5,
        ByProtocol,
        &gop_guid,
        NULL,
        &count,
        &handles
    );

    if (!EFI_ERROR(status) && handles) {
        for (UINTN i = 0; i < count && g_output_count < NAQ_MAX_OUTPUTS; i++) {
            EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = NULL;

            status = uefi_call_wrapper(
                BS->HandleProtocol,
                3,
                handles[i],
                &gop_guid,
                (VOID**)&gop
            );

            if (EFI_ERROR(status) || !gop || !gop->Mode || !gop->Mode->Info) {
                continue;
            }

            /*
             * Some firmwares list the same output under two handles.
             */
            int duplicate = 0;

            for (UINTN n = 0; n < g_output_count; n++) {
                if (g_outputs[n] == gop) {
                    duplicate = 1;
                }
            }

            if (!duplicate) {
                g_outputs[g_output_count++] = gop;
            }
        }

        uefi_call_wrapper(BS->FreePool, 1, handles);
    }

    if (g_output_count == 0) {
        EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = NULL;

        if (!EFI_ERROR(locate_gop(&gop)) && gop && gop->Mode && gop->Mode->Info) {
            g_outputs[g_output_count++] = gop;
        }
    }
}

/*
 * Reads one mode's size. Returns 0 for modes the kernel cannot draw on.
 */
static int mode_size(EFI_GRAPHICS_OUTPUT_PROTOCOL* gop, UINT32 mode, UINT32* w, UINT32* h) {
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* info = NULL;
    UINTN info_size = 0;

    EFI_STATUS status = uefi_call_wrapper(
        gop->QueryMode,
        4,
        gop,
        mode,
        &info_size,
        &info
    );

    if (EFI_ERROR(status) || !info) {
        return 0;
    }

    if (!gop_pixel_format_supported(info->PixelFormat)) {
        return 0;
    }

    *w = info->HorizontalResolution;
    *h = info->VerticalResolution;

    return 1;
}

static int find_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL* gop, UINT32 width, UINT32 height, UINT32* out_mode) {
    for (UINT32 mode = 0; mode < gop->Mode->MaxMode; mode++) {
        UINT32 w = 0;
        UINT32 h = 0;

        if (mode_size(gop, mode, &w, &h) && w == width && h == height) {
            *out_mode = mode;
            return 1;
        }
    }

    return 0;
}

static int display_cfg_load(EFI_HANDLE image_handle, naq_display_cfg_t* cfg) {
    VOID* buffer = NULL;
    UINTN size = 0;

    EFI_STATUS status = load_file_from_boot_volume(
        image_handle,
        NAQ_DISPLAY_CFG_PATH,
        &buffer,
        &size
    );

    if (EFI_ERROR(status) || !buffer) {
        return 0;
    }

    int ok = 0;

    if (size >= sizeof(*cfg)) {
        const naq_display_cfg_t* stored = (const naq_display_cfg_t*)buffer;

        if (stored->magic == NAQ_DISPLAY_CFG_MAGIC) {
            cfg->magic = stored->magic;
            cfg->output = stored->output;
            cfg->width = stored->width;
            cfg->height = stored->height;
            ok = 1;
        }
    }

    uefi_call_wrapper(BS->FreePool, 1, buffer);

    return ok;
}

static EFI_STATUS display_cfg_save(EFI_HANDLE image_handle, naq_display_cfg_t* cfg) {
    EFI_GUID loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID simple_fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;

    EFI_LOADED_IMAGE_PROTOCOL* loaded_image = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs = NULL;
    EFI_FILE_PROTOCOL* root = NULL;
    EFI_FILE_PROTOCOL* file = NULL;

    EFI_STATUS status = uefi_call_wrapper(
        BS->HandleProtocol,
        3,
        image_handle,
        &loaded_image_guid,
        (VOID**)&loaded_image
    );

    if (EFI_ERROR(status)) {
        return status;
    }

    status = uefi_call_wrapper(
        BS->HandleProtocol,
        3,
        loaded_image->DeviceHandle,
        &simple_fs_guid,
        (VOID**)&fs
    );

    if (EFI_ERROR(status)) {
        return status;
    }

    status = uefi_call_wrapper(fs->OpenVolume, 2, fs, &root);

    if (EFI_ERROR(status)) {
        return status;
    }

    status = uefi_call_wrapper(
        root->Open,
        5,
        root,
        &file,
        NAQ_DISPLAY_CFG_PATH,
        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
        0
    );

    if (EFI_ERROR(status)) {
        uefi_call_wrapper(root->Close, 1, root);
        return status;
    }

    UINTN size = sizeof(*cfg);

    status = uefi_call_wrapper(file->Write, 3, file, &size, cfg);

    uefi_call_wrapper(file->Close, 1, file);
    uefi_call_wrapper(root->Close, 1, root);

    return status;
}

/*
 * Waits up to `ms` for a key. Returns 1 and the key, or 0 on timeout.
 */
static int read_key_timeout(UINTN ms, EFI_INPUT_KEY* key) {
    for (UINTN waited = 0; waited <= ms; waited += 10u) {
        EFI_STATUS status = uefi_call_wrapper(
            ST->ConIn->ReadKeyStroke,
            2,
            ST->ConIn,
            key
        );

        if (!EFI_ERROR(status)) {
            return 1;
        }

        uefi_call_wrapper(BS->Stall, 1, 10000u);
    }

    return 0;
}

/*
 * Reads a decimal number typed on the keyboard, ended with Enter.
 * Returns -1 if nothing was typed or Esc was pressed.
 */
static INTN read_number(void) {
    INTN value = -1;

    for (;;) {
        EFI_INPUT_KEY key;
        UINTN index = 0;

        uefi_call_wrapper(BS->WaitForEvent, 3, 1, &ST->ConIn->WaitForKey, &index);

        if (EFI_ERROR(uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key))) {
            continue;
        }

        if (key.ScanCode == 0x17) {
            Print(L"\r\n");
            return -1;
        }

        if (key.UnicodeChar == L'\r' || key.UnicodeChar == L'\n') {
            Print(L"\r\n");
            return value;
        }

        if (key.UnicodeChar >= L'0' && key.UnicodeChar <= L'9' && value < 100000) {
            value = (value < 0 ? 0 : value * 10) + (INTN)(key.UnicodeChar - L'0');
            Print(L"%c", key.UnicodeChar);
        }
    }
}

/*
 * The display settings menu. Returns 1 if cfg was filled in.
 */
static int display_menu(naq_display_cfg_t* cfg) {
    UINTN output = 0;

    Print(L"\r\nDisplay settings\r\n");
    Print(L"----------------\r\n");

    for (UINTN i = 0; i < g_output_count; i++) {
        Print(
            L"  output %u: currently %ux%u, %u modes\r\n",
            (UINT32)i,
            g_outputs[i]->Mode->Info->HorizontalResolution,
            g_outputs[i]->Mode->Info->VerticalResolution,
            g_outputs[i]->Mode->MaxMode
        );
    }

    if (g_output_count > 1) {
        Print(L"\r\nOutput number, then Enter (Esc to cancel): ");

        INTN picked = read_number();

        if (picked < 0 || (UINTN)picked >= g_output_count) {
            Print(L"Cancelled.\r\n");
            return 0;
        }

        output = (UINTN)picked;
    } else {
        Print(L"\r\nThe firmware offers a single output, so the monitor cannot be\r\n");
        Print(L"changed here; pick it in the firmware setup or swap the cables.\r\n");
    }

    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = g_outputs[output];
    UINT32 column = 0;

    Print(L"\r\nModes of output %u:\r\n", (UINT32)output);

    for (UINT32 mode = 0; mode < gop->Mode->MaxMode; mode++) {
        UINT32 w = 0;
        UINT32 h = 0;

        if (!mode_size(gop, mode, &w, &h)) {
            continue;
        }

        Print(L"  %2u: %4ux%-4u", mode, w, h);

        if (++column == 4u) {
            column = 0;
            Print(L"\r\n");
        }
    }

    if (column) {
        Print(L"\r\n");
    }

    Print(L"\r\nMode number, then Enter (Esc to cancel): ");

    INTN picked = read_number();
    UINT32 w = 0;
    UINT32 h = 0;

    if (picked < 0 || (UINT32)picked >= gop->Mode->MaxMode ||
        !mode_size(gop, (UINT32)picked, &w, &h)) {
        Print(L"Cancelled.\r\n");
        return 0;
    }

    cfg->magic = NAQ_DISPLAY_CFG_MAGIC;
    cfg->output = (UINT32)output;
    cfg->width = w;
    cfg->height = h;

    return 1;
}

static EFI_STATUS choose_display(EFI_HANDLE image_handle, EFI_GRAPHICS_OUTPUT_PROTOCOL** out_gop) {
    naq_display_cfg_t cfg;
    EFI_INPUT_KEY key;

    *out_gop = NULL;

    collect_outputs();

    if (g_output_count == 0) {
        return EFI_NOT_FOUND;
    }

    int have_cfg = display_cfg_load(image_handle, &cfg);

    Print(L"Graphics outputs: %u\r\n", (UINT32)g_output_count);

    if (have_cfg) {
        Print(
            L"Saved display: output %u at %ux%u\r\n",
            cfg.output,
            cfg.width,
            cfg.height
        );
    }

    Print(L"Starting in 3 seconds. D = display settings, P = pause before starting,\r\n");
    Print(L"any other key = start now.\r\n");

    uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);

    int got_key = read_key_timeout(NAQ_DISPLAY_PROMPT_MS, &key);

    if (got_key && (key.UnicodeChar == L'p' || key.UnicodeChar == L'P')) {
        g_pause_before_kernel = 1;
    }

    if (got_key && (key.UnicodeChar == L'd' || key.UnicodeChar == L'D')) {
        naq_display_cfg_t picked;

        if (display_menu(&picked)) {
            cfg = picked;
            have_cfg = 1;

            EFI_STATUS saved = display_cfg_save(image_handle, &cfg);

            if (EFI_ERROR(saved)) {
                Print(L"Could not save the choice (%r); it applies to this boot only.\r\n", saved);
            } else {
                Print(L"Saved.\r\n");
            }
        }
    }

    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = NULL;
    UINT32 mode = 0;
    int set_mode = 0;

    if (have_cfg && cfg.output < g_output_count &&
        find_mode(g_outputs[cfg.output], cfg.width, cfg.height, &mode)) {
        gop = g_outputs[cfg.output];
        set_mode = 1;
    } else {
        if (have_cfg) {
            Print(L"The saved display is not available; using the default.\r\n");
        }

        for (UINTN i = 0; i < g_output_count && !gop; i++) {
            if (find_mode(g_outputs[i], NAQ_TARGET_GOP_WIDTH, NAQ_TARGET_GOP_HEIGHT, &mode)) {
                gop = g_outputs[i];
                set_mode = 1;
            }
        }

        if (!gop) {
            gop = g_outputs[0];
        }
    }

    if (set_mode) {
        EFI_STATUS status = uefi_call_wrapper(gop->SetMode, 2, gop, mode);

        if (EFI_ERROR(status)) {
            /*
             * Not fatal: the firmware keeps its current mode.
             */
            Print(L"SetMode failed: %r; keeping the firmware's mode.\r\n", status);
        }
    }

    *out_gop = gop;

    return EFI_SUCCESS;
}

/* ============================================================
 * ExitBootServices with memory map handoff
 * ============================================================ */

static EFI_STATUS exit_boot_services_with_memory_map(
    EFI_HANDLE image_handle,
    naq_boot_info_t* boot
) {
    EFI_STATUS status;

    EFI_MEMORY_DESCRIPTOR* memory_map = NULL;
    UINTN memory_map_size = 0;
    UINTN memory_map_buffer_size = 0;
    UINTN map_key = 0;
    UINTN descriptor_size = 0;
    UINT32 descriptor_version = 0;

    if (!boot) {
        return EFI_INVALID_PARAMETER;
    }

    status = uefi_call_wrapper(
        BS->GetMemoryMap,
        5,
        &memory_map_size,
        memory_map,
        &map_key,
        &descriptor_size,
        &descriptor_version
    );

    if (status != EFI_BUFFER_TOO_SMALL) {
        return status;
    }

    memory_map_buffer_size = memory_map_size + (descriptor_size * 32);

    status = uefi_call_wrapper(
        BS->AllocatePool,
        3,
        EfiLoaderData,
        memory_map_buffer_size,
        (VOID**)&memory_map
    );

    if (EFI_ERROR(status)) {
        return status;
    }

    for (UINTN attempt = 0; attempt < 8; attempt++) {
        memory_map_size = memory_map_buffer_size;

        status = uefi_call_wrapper(
            BS->GetMemoryMap,
            5,
            &memory_map_size,
            memory_map,
            &map_key,
            &descriptor_size,
            &descriptor_version
        );

        if (EFI_ERROR(status)) {
            return status;
        }

        boot->memory_map = (nq_u64)(UINTN)memory_map;
        boot->memory_map_size = (nq_u64)memory_map_size;
        boot->memory_map_desc_size = (nq_u64)descriptor_size;
        boot->memory_map_desc_version = (nq_u32)descriptor_version;

        status = uefi_call_wrapper(
            BS->ExitBootServices,
            2,
            image_handle,
            map_key
        );

        if (!EFI_ERROR(status)) {
            return EFI_SUCCESS;
        }
    }

    return status;
}

/* ============================================================
 * UEFI entry point
 * ============================================================ */

/*
 * No EFIAPI here: gnu-efi's startup code (crt0 / _entry) calls efi_main()
 * with the SysV calling convention, even when the rest of the library is
 * built with GNU_EFI_USE_MS_ABI. Marking it EFIAPI makes ImageHandle and
 * SystemTable arrive in the wrong registers.
 */
EFI_STATUS
efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE* SystemTable) {
    EFI_STATUS status;
    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = NULL;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* info = NULL;
    UINTN kernel64_file_size = 0;

    InitializeLib(ImageHandle, SystemTable);

    zero_memory(&g_boot_info, sizeof(g_boot_info));

    uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);

    Print(L"NAQ OS UEFI Loader\r\n");
    Print(L"------------------\r\n\r\n");

    read_boot_volume_serial(ImageHandle);

    status = choose_display(ImageHandle, &gop);

    if (EFI_ERROR(status) || !gop || !gop->Mode || !gop->Mode->Info) {
        Print(L"GOP: not found\r\n");
        Print(L"Status: %r\r\n\r\n", status);
        Print(L"Press any key to return to firmware...\r\n");
        wait_for_key();
        return EFI_SUCCESS;
    }

    info = gop->Mode->Info;

    Print(L"Selected GOP mode:\r\n");
    Print(
        L"Resolution: %ux%u\r\n",
        info->HorizontalResolution,
        info->VerticalResolution
    );
    Print(L"PixelsPerScanLine: %u\r\n", info->PixelsPerScanLine);
    Print(L"PixelFormat: %s\r\n", pixel_format_name(info->PixelFormat));
    Print(L"FramebufferBase: 0x%lx\r\n", gop->Mode->FrameBufferBase);
    Print(L"FramebufferSize: %lu bytes\r\n\r\n", gop->Mode->FrameBufferSize);

    Print(L"Loading KERNEL64.BIN to 0x00100000...\r\n");

    status = load_kernel64_to_fixed_address(
        ImageHandle,
        gop,
        (EFI_PHYSICAL_ADDRESS)UEFI_KERNEL64_LOAD_ADDR,
        &kernel64_file_size
    );

    if (EFI_ERROR(status)) {
        Print(L"KERNEL64.BIN load failed: %r\r\n", status);
        Print(L"Press any key to return to firmware...\r\n");
        wait_for_key();
        return EFI_SUCCESS;
    }

    Print(L"KERNEL64.BIN loaded successfully.\r\n\r\n");

    Print(L"Loading optional INITRD.NAQ...\r\n");

    status = load_initrd(ImageHandle, &g_boot_info);

    if (EFI_ERROR(status)) {
        Print(L"INITRD.NAQ load failed: %r\r\n", status);
        Print(L"Press any key to return to firmware...\r\n");
        wait_for_key();
        return EFI_SUCCESS;
    }

    Print(L"\r\nAllocating kernel64 stack...\r\n");

    status = allocate_kernel_stack(&g_boot_info);

    if (EFI_ERROR(status)) {
        Print(L"Kernel stack allocation failed: %r\r\n", status);
        Print(L"Press any key to return to firmware...\r\n");
        wait_for_key();
        return EFI_SUCCESS;
    }

    Print(L"Stack base: 0x%lx\r\n", g_boot_info.stack_base);
    Print(L"Stack size: %lu bytes\r\n", g_boot_info.stack_size);
    Print(L"Stack top : 0x%lx\r\n\r\n", g_boot_info.stack_top);

    Print(L"Kernel summary:\r\n");
    Print(L"  file size:   %lu bytes\r\n", kernel64_file_size);
    Print(L"  load addr:   0x%lx\r\n", g_boot_info.kernel_load_addr);
    Print(L"  PMM reserve: %lu bytes\r\n", g_boot_info.kernel_size);
    Print(L"  initrd base: 0x%lx\r\n", g_boot_info.initrd_base);
    Print(L"  initrd size: %lu bytes\r\n\r\n", g_boot_info.initrd_size);

    if (g_pause_before_kernel) {
        Print(L"Paused. Press any key to start the kernel...\r\n");
        wait_for_key();
    } else {
        Print(L"Starting the kernel...\r\n");
    }

    status = exit_boot_services_with_memory_map(ImageHandle, &g_boot_info);

    if (EFI_ERROR(status)) {
        Print(L"ExitBootServices failed: %r\r\n", status);
        Print(L"Press any key to return to firmware...\r\n");
        wait_for_key();
        return EFI_SUCCESS;
    }

    /*
     * No UEFI calls after this point.
     */
    typedef void (*kernel64_entry_t)(naq_boot_info_t* boot_info);

    kernel64_entry_t entry =
        (kernel64_entry_t)(UINTN)UEFI_KERNEL64_LOAD_ADDR;

    entry(&g_boot_info);

    for (;;) {
        __asm__ volatile ("cli; hlt");
    }

    return EFI_SUCCESS;
}
