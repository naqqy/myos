#include <efi.h>
#include <efilib.h>
#include "naq_boot_info.h"

#define UEFI_KERNEL64_LOAD_ADDR       0x00100000ULL
#define UEFI_KERNEL64_PATH            L"KERNEL64.BIN"

/*
 * 16 pages = 64 KiB kernel stack.
 */
#define UEFI_KERNEL_STACK_PAGES       16u

/*
 * Early kernel reservation.
 * Covers flat binary + BSS/static storage until we have exact ELF/kernel-end metadata.
 */
#define UEFI_KERNEL64_ALLOC_PAGES 1024u  /* 4 MiB */
#define UEFI_KERNEL64_PMM_RESERVE_PAGES  4096u  /* 16 MiB */

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

    /*
     * First call asks UEFI how large the EFI_FILE_INFO buffer must be.
     */
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

static naq_boot_info_t g_boot_info;

static void zero_memory(void* ptr, UINTN size) {
    UINT8* p = (UINT8*)ptr;

    if (!p) {
        return;
    }

    for (UINTN i = 0; i < size; i++) {
        p[i] = 0;
    }
}

static EFI_STATUS load_kernel64_to_fixed_address(
    EFI_HANDLE image_handle,
    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop,
    EFI_PHYSICAL_ADDRESS load_addr,
    UINTN* out_kernel_size
) {
    EFI_STATUS status;
    VOID* file_buffer = NULL;
    UINTN file_size = 0;

    UINTN file_pages = 0;
    UINTN alloc_pages = 0;
    UINTN alloc_bytes = 0;

    EFI_PHYSICAL_ADDRESS requested_addr = load_addr;

    if (!out_kernel_size) {
        return EFI_INVALID_PARAMETER;
    }

    *out_kernel_size = 0;

    status = load_file_from_boot_volume(
        image_handle,
        UEFI_KERNEL64_PATH,
        &file_buffer,
        &file_size
    );

    if (EFI_ERROR(status)) {
        return status;
    }

    if (file_size == 0) {
        uefi_call_wrapper(BS->FreePool, 1, file_buffer);
        return EFI_LOAD_ERROR;
    }

    file_pages = (file_size + 0xFFFu) / 0x1000u;

    alloc_pages = file_pages;
    if (alloc_pages < UEFI_KERNEL64_ALLOC_PAGES) {
        alloc_pages = UEFI_KERNEL64_ALLOC_PAGES;
    }

    alloc_bytes = alloc_pages * 4096u;

    UINTN reserve_pages = UEFI_KERNEL64_PMM_RESERVE_PAGES;
    UINTN reserve_bytes = reserve_pages * 4096u;

    status = uefi_call_wrapper(
        BS->AllocatePages,
        4,
        AllocateAddress,
        EfiLoaderCode,
        alloc_pages,
        &requested_addr
    );

    if (EFI_ERROR(status)) {
        uefi_call_wrapper(BS->FreePool, 1, file_buffer);
        return status;
    }

    /*
     * Clear the entire reserved kernel region.
     * This protects early .bss/static storage when loading a flat binary.
     */
    zero_memory((VOID*)(UINTN)load_addr, alloc_bytes);

    /*
     * Copy the flat binary into the start of the reserved region.
     */
    CopyMem((VOID*)(UINTN)load_addr, file_buffer, file_size);

    uefi_call_wrapper(BS->FreePool, 1, file_buffer);

    g_boot_info.magic = NAQ_BOOT_INFO_MAGIC;
    g_boot_info.version = NAQ_BOOT_INFO_VERSION;

    if (gop && gop->Mode && gop->Mode->Info) {
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

    g_boot_info.kernel_load_addr = (nq_u64)load_addr;

    /*
     * Important:
     * This is the reserved kernel memory size, not only the file size.
     * PMM64 uses this to avoid handing kernel-owned memory to kmem64.
     */
    g_boot_info.kernel_size = (nq_u64)reserve_bytes;

    /*
     * Return the file size for display only.
     */
    *out_kernel_size = file_size;

    return EFI_SUCCESS;
}

static EFI_STATUS allocate_kernel_stack(naq_boot_info_t* boot) {
    EFI_STATUS status;
    EFI_PHYSICAL_ADDRESS stack_base = 0;
    UINTN stack_pages = UEFI_KERNEL_STACK_PAGES;
    UINTN stack_size = stack_pages * 4096u;

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

    boot->stack_base = (nq_u64)stack_base;
    boot->stack_size = (nq_u64)stack_size;
    boot->stack_top  = (nq_u64)(stack_base + stack_size);

    return EFI_SUCCESS;
}

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

    /*
     * First call discovers required memory map size.
     */
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

    /*
     * Add extra room because AllocatePool itself may change the memory map.
     */
    memory_map_buffer_size = memory_map_size + (descriptor_size * 16);

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

    /*
     * ExitBootServices can fail if the map changes between GetMemoryMap()
     * and ExitBootServices(). Retry a few times using the same buffer.
     */
    for (UINTN attempt = 0; attempt < 5; attempt++) {
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

EFI_STATUS
EFIAPI
efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE* SystemTable) {
    EFI_STATUS status;
    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = NULL;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* info = NULL;
    UINTN kernel64_size = 0;

    InitializeLib(ImageHandle, SystemTable);

    zero_memory(&g_boot_info, sizeof(g_boot_info));

    uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);

    Print(L"NAQ OS UEFI Loader\r\n");
    Print(L"------------------\r\n\r\n");

    status = locate_gop(&gop);

    if (EFI_ERROR(status) || !gop || !gop->Mode || !gop->Mode->Info) {
        Print(L"GOP: not found\r\n");
        Print(L"Status: %r\r\n\r\n", status);
        Print(L"Press any key to return to firmware...\r\n");
        wait_for_key();
        return EFI_SUCCESS;
    }

    info = gop->Mode->Info;

    Print(L"GOP: found\r\n");
    Print(L"Resolution: %ux%u\r\n",
          info->HorizontalResolution,
          info->VerticalResolution);
    Print(L"PixelsPerScanLine: %u\r\n", info->PixelsPerScanLine);
    Print(L"PixelFormat: %s\r\n", pixel_format_name(info->PixelFormat));
    Print(L"FramebufferBase: 0x%lx\r\n", gop->Mode->FrameBufferBase);
    Print(L"FramebufferSize: %lu bytes\r\n\r\n", gop->Mode->FrameBufferSize);

    Print(L"Loading KERNEL64.BIN to 0x00100000...\r\n");

    status = load_kernel64_to_fixed_address(
        ImageHandle,
        gop,
        (EFI_PHYSICAL_ADDRESS)UEFI_KERNEL64_LOAD_ADDR,
        &kernel64_size
    );

    if (EFI_ERROR(status)) {
        Print(L"KERNEL64.BIN load failed: %r\r\n", status);
        Print(L"Press any key to return to firmware...\r\n");
        wait_for_key();
        return EFI_SUCCESS;
    }

    Print(L"KERNEL64.BIN loaded successfully.\r\n");
    Print(L"Kernel64 file size: %lu bytes\r\n", kernel64_size);
    Print(L"Kernel64 reserved size: %lu bytes\r\n", g_boot_info.kernel_size);

    Print(L"Allocating kernel64 stack...\r\n");

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

    Print(L"Press any key to ExitBootServices and jump to kernel64...\r\n");
    wait_for_key();

    /*
     * After this succeeds, no more UEFI calls are allowed.
     */
    status = exit_boot_services_with_memory_map(ImageHandle, &g_boot_info);

    if (EFI_ERROR(status)) {
        /*
         * ExitBootServices failed, so Boot Services are still available.
         * It is safe to print this failure.
         */
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