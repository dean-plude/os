/*
 * main.c — NovaOS UEFI bootloader entry point
 *
 * Flow:
 *   1. Locate the kernel ELF on the EFI System Partition
 *   2. Load it into memory at its requested physical addresses
 *   3. Enumerate GOP framebuffer
 *   4. Find ACPI RSDP in EFI configuration tables
 *   5. Build initial page tables (identity + physmap + kernel high)
 *   6. Allocate the boot-time kernel stack
 *   7. Get the UEFI memory map and call ExitBootServices
 *   8. Switch CR3 to our new page tables
 *   9. Jump to kernel entry
 *
 * After step 7 we must not call any UEFI services (console, allocation, etc).
 * Any failure before that point prints an error and halts.
 *
 * The kernel binary must be at \EFI\NOVA\kernel.elf on the ESP.
 */

#include "../include/efi.h"
#include "../../include/boot_protocol.h"
#include "elf_loader.h"
#include "paging.h"

/* Forward declarations for helpers implemented below */
static void   halt(void);
static void   mem_zero(void *ptr, UINTN n);
static void   mem_copy(void *dst, const void *src, UINTN n);
static UINTN  str16len(const CHAR16 *s);

/* console_* functions are in console.c and linked in */
void console_init(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *conout);
void console_puts(const char *s);
void console_printf(const char *fmt, ...);

/* -----------------------------------------------------------------------
 * Panic — print message and triple-fault / spin
 * ----------------------------------------------------------------------- */
#define PANIC(fmt, ...) do {                               \
    console_printf("\r\n*** BOOTLOADER PANIC ***\r\n");    \
    console_printf(fmt, ##__VA_ARGS__);                    \
    console_printf("\r\nSystem halted.\r\n");              \
    halt();                                                \
} while (0)

#define CHECK(s, msg, ...) do {                            \
    if (EFI_ERROR(s)) PANIC(msg " (status=%x)", ##__VA_ARGS__, (UINT64)(s)); \
} while (0)

/* -----------------------------------------------------------------------
 * UEFI globals
 * ----------------------------------------------------------------------- */
static EFI_HANDLE       g_image_handle;
static EFI_SYSTEM_TABLE *g_st;
static EFI_BOOT_SERVICES *g_bs;

/* -----------------------------------------------------------------------
 * Kernel path on the ESP
 * ----------------------------------------------------------------------- */
static CHAR16 KERNEL_PATH[] = { '\\','E','F','I','\\','N','O','V','A',
                                 '\\','k','e','r','n','e','l','.','e','l','f', 0 };

/* -----------------------------------------------------------------------
 * Open the kernel file from the ESP
 * ----------------------------------------------------------------------- */
static EFI_STATUS open_kernel_file(EFI_FILE_PROTOCOL **file_out,
                                    UINTN              *size_out)
{
    EFI_STATUS                       status;
    EFI_LOADED_IMAGE_PROTOCOL       *loaded_image;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_FILE_PROTOCOL               *root, *file;

    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;

    /* Get our own loaded-image protocol to find the device we booted from. */
    status = g_bs->OpenProtocol(
        g_image_handle, &li_guid,
        (VOID **)&loaded_image,
        g_image_handle, NULL,
        EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    CHECK(status, "OpenProtocol(LoadedImage)");

    /* Get the simple filesystem on that device. */
    status = g_bs->OpenProtocol(
        loaded_image->DeviceHandle, &fs_guid,
        (VOID **)&fs,
        g_image_handle, NULL,
        EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    CHECK(status, "OpenProtocol(SimpleFS)");

    /* Open root directory. */
    status = fs->OpenVolume(fs, &root);
    CHECK(status, "OpenVolume");

    /* Open kernel file. */
    status = root->Open(root, &file, KERNEL_PATH, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) {
        console_printf("Cannot open kernel at \\EFI\\NOVA\\kernel.elf: %x\r\n",
                       (UINT64)status);
        return status;
    }

    /* Get file size via GetInfo. */
    EFI_GUID       info_guid = EFI_FILE_INFO_ID;
    EFI_FILE_INFO  fi_buf;
    UINTN          fi_size = sizeof(fi_buf);
    status = file->GetInfo(file, &info_guid, &fi_size, &fi_buf);
    CHECK(status, "GetInfo(kernel)");

    console_printf("Kernel file size: %u bytes\r\n", fi_buf.FileSize);

    *file_out = file;
    *size_out = (UINTN)fi_buf.FileSize;
    root->Close(root);
    return EFI_SUCCESS;
}

/* -----------------------------------------------------------------------
 * Locate the ACPI 2.0 RSDP in the EFI configuration table
 * ----------------------------------------------------------------------- */
static UINT64 find_rsdp(void)
{
    EFI_GUID acpi_guid = EFI_ACPI_20_TABLE_GUID;
    for (UINTN i = 0; i < g_st->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *ct = &g_st->ConfigurationTable[i];
        if (EFI_GUID_EQ(ct->VendorGuid, acpi_guid)) {
            return (UINT64)(UINTN)ct->VendorTable;
        }
    }
    console_printf("WARNING: ACPI 2.0 RSDP not found in EFI configuration\r\n");
    return 0;
}

/* -----------------------------------------------------------------------
 * Find the best GOP mode (prefer native/largest resolution)
 * ----------------------------------------------------------------------- */
static void init_framebuffer(BootFramebuffer *fb)
{
    EFI_STATUS                    status;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    EFI_GUID                      gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;

    status = g_bs->LocateProtocol(&gop_guid, NULL, (VOID **)&gop);
    if (EFI_ERROR(status)) {
        console_printf("WARNING: No GOP found, framebuffer unavailable\r\n");
        return;
    }

    /* Try to find a mode with at least 1024x768. */
    UINT32 best_mode = gop->Mode->Mode;
    UINT32 best_w    = gop->Mode->Info->HorizontalResolution;
    UINT32 best_h    = gop->Mode->Info->VerticalResolution;

    for (UINT32 m = 0; m < gop->Mode->MaxMode; m++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
        UINTN info_size;
        status = gop->QueryMode(gop, m, &info_size, &info);
        if (EFI_ERROR(status)) continue;

        if (info->PixelFormat == PixelBltOnly) continue;
        if (info->HorizontalResolution > best_w ||
            (info->HorizontalResolution == best_w && info->VerticalResolution > best_h)) {
            best_mode = m;
            best_w    = info->HorizontalResolution;
            best_h    = info->VerticalResolution;
        }
    }

    if (best_mode != gop->Mode->Mode) {
        status = gop->SetMode(gop, best_mode);
        if (EFI_ERROR(status)) {
            console_printf("WARNING: GOP SetMode failed: %x\r\n", (UINT64)status);
        }
    }

    fb->base                = gop->Mode->FrameBufferBase;
    fb->size                = gop->Mode->FrameBufferSize;
    fb->width               = gop->Mode->Info->HorizontalResolution;
    fb->height              = gop->Mode->Info->VerticalResolution;
    fb->pixels_per_scanline = gop->Mode->Info->PixelsPerScanLine;
    fb->pixel_format        = (gop->Mode->Info->PixelFormat ==
                                PixelRedGreenBlueReserved8BitPerColor) ? 1 : 0;

    console_printf("Framebuffer: %ux%u stride=%u base=0x%x (%u KiB)\r\n",
                   (UINT64)fb->width, (UINT64)fb->height,
                   (UINT64)fb->pixels_per_scanline,
                   fb->base, fb->size / 1024);
}

/* -----------------------------------------------------------------------
 * Collect memory map and exit boot services
 *
 * This is the trickiest part of the bootloader: we must call GetMemoryMap,
 * then ExitBootServices with the key returned. If any allocation happens
 * between the two calls, the key becomes invalid and we must retry.
 * ----------------------------------------------------------------------- */
static EFI_STATUS exit_boot_services_and_get_map(
    BootMemDescriptor **map_out,
    UINT32             *count_out)
{
    EFI_STATUS status;
    UINTN      map_size = 0, map_key = 0, desc_size = 0;
    UINT32     desc_version = 0;
    UINT8     *map_buf = NULL;

    /* First call to get required buffer size. */
    status = g_bs->GetMemoryMap(&map_size, NULL, &map_key, &desc_size, &desc_version);
    /* Expected EFI_BUFFER_TOO_SMALL, or success with size=0 */
    map_size += desc_size * 16;   /* Add slack for the allocation below */

    /* Allocate the buffer — this changes the map key! */
    UINT64 map_phys = 0;
    UINTN  map_pages = (map_size + 4095) / 4096;
    status = g_bs->AllocatePages(AllocateAnyPages, EfiLoaderData, map_pages, &map_phys);
    if (EFI_ERROR(status)) return status;
    map_buf = (UINT8 *)(UINTN)map_phys;

    /* Retry loop: GetMemoryMap then ExitBootServices must use the same key. */
    for (int attempt = 0; attempt < 3; attempt++) {
        UINTN cur_map_size = map_pages * 4096;
        status = g_bs->GetMemoryMap(&cur_map_size, (EFI_MEMORY_DESCRIPTOR *)map_buf,
                                    &map_key, &desc_size, &desc_version);
        if (EFI_ERROR(status)) return status;

        status = g_bs->ExitBootServices(g_image_handle, map_key);
        if (!EFI_ERROR(status)) {
            /* Convert EFI memory map to our BootMemDescriptor format.
             * We do this in place (BootMemDescriptor is smaller than
             * EFI_MEMORY_DESCRIPTOR so we work front-to-back). */
            UINTN n = cur_map_size / desc_size;
            BootMemDescriptor *out = (BootMemDescriptor *)(UINTN)map_phys;

            for (UINTN i = 0; i < n; i++) {
                EFI_MEMORY_DESCRIPTOR *efi_desc =
                    (EFI_MEMORY_DESCRIPTOR *)(map_buf + i * desc_size);
                BootMemDescriptor bmd;

                bmd.physical_base = efi_desc->physical_start;
                bmd.num_pages     = efi_desc->num_pages;

                switch ((UINT32)efi_desc->type) {
                case EfiConventionalMemory:
                    bmd.type = BOOT_MEM_CONVENTIONAL; break;
                case EfiLoaderCode:
                    bmd.type = BOOT_MEM_LOADER_CODE; break;
                case EfiLoaderData:
                    bmd.type = BOOT_MEM_LOADER_DATA; break;
                case EfiBootServicesCode:
                case EfiBootServicesData:
                    bmd.type = BOOT_MEM_BOOT_SERVICES; break;
                case EfiRuntimeServicesCode:
                case EfiRuntimeServicesData:
                    bmd.type = BOOT_MEM_RUNTIME; break;
                case EfiACPIReclaimMemory:
                    bmd.type = BOOT_MEM_ACPI_RECLAIM; break;
                case EfiACPIMemoryNVS:
                    bmd.type = BOOT_MEM_ACPI_NVS; break;
                case EfiMemoryMappedIO:
                case EfiMemoryMappedIOPortSpace:
                    bmd.type = BOOT_MEM_MMIO; break;
                default:
                    bmd.type = BOOT_MEM_RESERVED; break;
                }

                bmd.flags = 0;
                if (efi_desc->attribute & EFI_MEMORY_WB)
                    bmd.flags |= BOOT_MEM_FLAG_WRITABLE;
                if (!(efi_desc->attribute & EFI_MEMORY_XP))
                    bmd.flags |= BOOT_MEM_FLAG_EXECUTABLE;

                out[i] = bmd;
            }

            *map_out   = out;
            *count_out = (UINT32)n;
            return EFI_SUCCESS;
        }
        /* ExitBootServices failed — map key stale, retry GetMemoryMap */
    }
    return EFI_LOAD_ERROR;
}

/* -----------------------------------------------------------------------
 * Assembly trampoline: load new CR3, switch to kernel stack, jump to entry
 *
 * Called after ExitBootServices — NO UEFI calls after this point.
 * Parameters (System V AMD64 ABI since we're in our own kernel code):
 *   cr3       — new PML4 physical address
 *   stack_top — virtual address for the initial kernel stack
 *   entry     — virtual address of kernel entry
 *   boot_info — virtual address of BootInfo struct (in physmap)
 * ----------------------------------------------------------------------- */
static __attribute__((noreturn)) void
jump_to_kernel(UINT64 cr3, UINT64 stack_top, UINT64 entry, UINT64 boot_info)
{
    __asm__ volatile (
        /* Load new page tables */
        "mov %0, %%cr3\n\t"
        /* Switch to kernel stack */
        "mov %1, %%rsp\n\t"
        "xor %%rbp, %%rbp\n\t"
        /* Call kernel entry(boot_info) */
        "mov %3, %%rdi\n\t"
        "jmpq *%2\n\t"
        :
        : "r"(cr3), "r"(stack_top), "r"(entry), "r"(boot_info)
        : "memory"
    );
    __builtin_unreachable();
}

/* -----------------------------------------------------------------------
 * efi_main — UEFI entry point
 * ----------------------------------------------------------------------- */
EFI_STATUS __attribute__((ms_abi)) efi_main(
    EFI_HANDLE       ImageHandle,
    EFI_SYSTEM_TABLE *SystemTable)
{
    g_image_handle = ImageHandle;
    g_st           = SystemTable;
    g_bs           = SystemTable->BootServices;

    console_init(SystemTable->ConOut);
    console_printf("\r\nNovaOS Bootloader v0.1\r\n");
    console_printf("UEFI firmware: %S rev %u.%u\r\n",
                   SystemTable->FirmwareVendor,
                   (UINT64)(SystemTable->FirmwareRevision >> 16),
                   (UINT64)(SystemTable->FirmwareRevision & 0xFFFF));

    /* 1. Load the kernel ELF ----------------------------------------- */
    EFI_FILE_PROTOCOL *kernel_file;
    UINTN              kernel_file_size;
    EFI_STATUS status = open_kernel_file(&kernel_file, &kernel_file_size);
    CHECK(status, "open_kernel_file");

    UINT64 kernel_entry, kernel_phys, kernel_virt, kernel_size;
    status = elf_load(kernel_file, kernel_file_size,
                      &kernel_entry, &kernel_phys, &kernel_virt, &kernel_size,
                      g_bs);
    CHECK(status, "elf_load");
    kernel_file->Close(kernel_file);

    /* The ELF entry point is the physical address of the entry function
     * (since the ELF LMA == physical).  The virtual entry is:
     *   virt_entry = kernel_virt + (kernel_entry - kernel_phys)
     * We'll compute this after paging setup. */
    UINT64 kernel_virt_entry = kernel_virt + (kernel_entry - kernel_phys);
    console_printf("Kernel: phys=0x%x virt=0x%x entry_virt=0x%x size=0x%x\r\n",
                   kernel_phys, kernel_virt, kernel_virt_entry, kernel_size);

    /* 2. Set up framebuffer ------------------------------------------ */
    BootFramebuffer fb;
    mem_zero(&fb, sizeof(fb));
    init_framebuffer(&fb);

    /* 3. Find ACPI RSDP ----------------------------------------------- */
    UINT64 rsdp = find_rsdp();
    console_printf("RSDP physical: 0x%x\r\n", rsdp);

    /* 4. Build page tables -------------------------------------------- */
    UINT64 new_cr3 = 0;
    status = paging_build(kernel_phys, kernel_virt, kernel_size, &new_cr3, g_bs);
    CHECK(status, "paging_build");

    /* 5. Allocate boot stack (16 KiB) --------------------------------- */
    UINT64 stack_pages   = 4;  /* 16 KiB */
    UINT64 stack_phys    = 0;
    status = g_bs->AllocatePages(AllocateAnyPages, EfiLoaderData,
                                 stack_pages, &stack_phys);
    CHECK(status, "AllocatePages(stack)");
    /* Stack top = physmap_base + stack_phys + size (stack grows down) */
    UINT64 stack_top = PHYSMAP_BASE + stack_phys + stack_pages * 4096;

    /* 6. Allocate BootInfo struct ------------------------------------- */
    UINT64 boot_info_phys = 0;
    status = g_bs->AllocatePages(AllocateAnyPages, EfiLoaderData, 1, &boot_info_phys);
    CHECK(status, "AllocatePages(BootInfo)");
    BootInfo *bi = (BootInfo *)(UINTN)boot_info_phys;
    mem_zero(bi, sizeof(BootInfo));

    /* 7. Exit boot services and get memory map ----------------------- */
    BootMemDescriptor *mem_map   = NULL;
    UINT32             map_count = 0;
    status = exit_boot_services_and_get_map(&mem_map, &map_count);
    /* After this point: NO UEFI calls! */

    /* 8. Fill BootInfo ------------------------------------------------ */
    bi->magic               = BOOT_MAGIC;
    bi->version             = BOOT_PROTOCOL_VERSION;
    bi->mem_map             = (UINT64)(UINTN)mem_map;
    bi->mem_map_count       = map_count;
    bi->framebuffer         = fb;
    bi->rsdp_physical       = rsdp;
    bi->kernel_physical_base = kernel_phys;
    bi->kernel_virtual_base  = kernel_virt;
    bi->kernel_size          = kernel_size;
    bi->boot_stack_top       = stack_top;

    /* 9. Compute virtual address of BootInfo (it's in the physmap) ---- */
    UINT64 bi_virt = PHYSMAP_BASE + boot_info_phys;

    /* 10. Switch to our page tables and jump to the kernel ------------ */
    jump_to_kernel(new_cr3, stack_top, kernel_virt_entry, bi_virt);
}

/* -----------------------------------------------------------------------
 * Helper implementations
 * ----------------------------------------------------------------------- */
static void halt(void)
{
    for (;;) __asm__ volatile ("cli; hlt");
}

static void mem_zero(void *ptr, UINTN n)
{
    UINT8 *p = ptr;
    while (n--) *p++ = 0;
}

static void mem_copy(void *dst, const void *src, UINTN n)
{
    UINT8 *d = dst;
    const UINT8 *s = src;
    while (n--) *d++ = *s++;
}

static UINTN __attribute__((unused)) str16len(const CHAR16 *s)
{
    UINTN n = 0;
    while (s[n]) n++;
    return n;
}
