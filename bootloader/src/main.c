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
static CHAR16 LOADER_PATH[] = { '\\','E','F','I','\\','B','O','O','T',
                                 '\\','B','O','O','T','X','6','4','.','E','F','I', 0 };
/* The boot log file: only the ISO's EFI System Partition has it (the
 * installer copies the kernel and the loader, not this), so it also marks
 * the installation media when the ISO was written to a USB stick */
static CHAR16 BOOTLOG_PATH[] = { '\\','E','F','I','\\','N','O','V','A',
                                 '\\','b','o','o','t','l','o','g','.','t','x','t', 0 };

/* The device we booted from, kept for detecting installation media */
static EFI_HANDLE g_boot_device;

/* Device path nodes: Type, SubType, Length (LE16), then node data */
#define EFI_DEVICE_PATH_PROTOCOL_GUID \
    { 0x09576e91, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }
#define DP_TYPE_MESSAGING 0x03
#define DP_SUB_USB        0x05
#define DP_SUB_USB_CLASS  0x0F
#define DP_SUB_USB_WWID   0x10
#define DP_TYPE_MEDIA    0x04
#define DP_SUB_CDROM     0x02
#define DP_TYPE_END      0x7F

/* Whether the boot device's path has a node of @type with one of the
 * subtypes @sub[0..n-1] */
static BOOLEAN boot_path_has(UINT8 type, const UINT8 *sub, int n)
{
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    UINT8 *dp = NULL;
    if (!g_boot_device ||
        EFI_ERROR(g_bs->OpenProtocol(g_boot_device, &dp_guid, (VOID **)&dp, g_image_handle, NULL,
                                     EFI_OPEN_PROTOCOL_GET_PROTOCOL)) || !dp)
        return FALSE;
    for (int k = 0; k < 64; k++) {
        UINT16 len = (UINT16)(dp[2] | dp[3] << 8);
        if (dp[0] == DP_TYPE_END || len < 4) break;
        for (int i = 0; i < n; i++)
            if (dp[0] == type && dp[1] == sub[i]) return TRUE;
        dp += len;
    }
    return FALSE;
}

/* A CD-ROM media node: an El Torito boot image on a CD/DVD */
static BOOLEAN booted_from_cd(void)
{
    static const UINT8 cd[] = { DP_SUB_CDROM };
    return boot_path_has(DP_TYPE_MEDIA, cd, 1);
}

/* A USB node: a USB stick (or a USB CD drive) */
static BOOLEAN booted_from_usb(void)
{
    static const UINT8 usb[] = { DP_SUB_USB, DP_SUB_USB_CLASS, DP_SUB_USB_WWID };
    return boot_path_has(DP_TYPE_MESSAGING, usb, 3);
}

/* -----------------------------------------------------------------------
 * The firmware's boot entry for an installed disk.  The installer only
 * writes the removable-media path \EFI\BOOT\BOOTX64.EFI, which firmware
 * starts from a disk it has no entry for (or from its boot menu); the
 * first boot from the disk then adds a "NovaOS" Boot#### variable for
 * itself and puts it first in BootOrder, so the machine keeps starting
 * NovaOS after the USB stick comes out (UEFI 2.10, 3.1 Boot Manager).
 * ----------------------------------------------------------------------- */
typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_GET_VARIABLE)(CHAR16 *name, EFI_GUID *vendor, UINT32 *attrs,
                                                              UINTN *size, VOID *data);
typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_SET_VARIABLE)(CHAR16 *name, EFI_GUID *vendor, UINT32 attrs,
                                                              UINTN size, VOID *data);
typedef struct {
    UINT64 Signature;
    UINT32 Revision, HeaderSize, CRC32, Reserved;
    VOID  *GetTime, *SetTime, *GetWakeupTime, *SetWakeupTime, *SetVirtualAddressMap, *ConvertPointer;
    EFI_GET_VARIABLE GetVariable;
    VOID  *GetNextVariableName;
    EFI_SET_VARIABLE SetVariable;
} EFI_RUNTIME_SERVICES_VARS;

#define EFI_GLOBAL_VARIABLE_GUID \
    { 0x8be4df61, 0x93ca, 0x11d2, { 0xaa, 0x0d, 0x00, 0xe0, 0x98, 0x03, 0x2b, 0x8c } }
#define VAR_NV_BS_RT 7u                    /* non-volatile, boot service and runtime access */
#define LOAD_OPTION_ACTIVE 1u

static const CHAR16 NOVA_DESC[] = { 'N','o','v','a','O','S', 0 };

static UINTN dp_size(const UINT8 *dp)       /* up to and including the end node */
{
    UINTN n = 0;
    for (int k = 0; k < 64; k++) {
        UINT16 len = (UINT16)(dp[n + 2] | dp[n + 3] << 8);
        if (len < 4) return 0;
        n += len;
        if (dp[n - len] == DP_TYPE_END && dp[n - len + 1] == 0xFF) return n;
    }
    return 0;
}

static void boot_var_name(CHAR16 *out, UINT16 num)
{
    static const char hex[] = "0123456789ABCDEF";
    const char *b = "Boot";
    for (int i = 0; i < 4; i++) out[i] = (CHAR16)b[i];
    for (int i = 0; i < 4; i++) out[4 + i] = (CHAR16)hex[(num >> (12 - 4 * i)) & 15];
    out[8] = 0;
}

static BOOLEAN bytes_equal(const UINT8 *a, const UINT8 *b, UINTN n)
{
    for (UINTN i = 0; i < n; i++) if (a[i] != b[i]) return FALSE;
    return TRUE;
}

/* BOOT_FLAG_BOOT_ENTRY (| BOOT_FLAG_ENTRY_ADDED), or 0 if there is none */
static UINT64 ensure_boot_entry(void)
{
    EFI_RUNTIME_SERVICES_VARS *rt = (EFI_RUNTIME_SERVICES_VARS *)g_st->RuntimeServices;
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID, dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_GUID gv = EFI_GLOBAL_VARIABLE_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *li;
    UINT8 *dev = NULL;
    if (!rt || EFI_ERROR(g_bs->OpenProtocol(g_image_handle, &li_guid, (VOID **)&li, g_image_handle, NULL,
                                            EFI_OPEN_PROTOCOL_GET_PROTOCOL)) || !li->FilePath ||
        EFI_ERROR(g_bs->OpenProtocol(li->DeviceHandle, &dp_guid, (VOID **)&dev, g_image_handle, NULL,
                                     EFI_OPEN_PROTOCOL_GET_PROTOCOL)) || !dev)
        return 0;
    UINTN dsz = dp_size(dev), fsz = dp_size((const UINT8 *)li->FilePath);
    if (dsz < 4 || fsz < 4) return 0;

    /* The load option: attributes, the path's length, the description,
     * then the device's path (without its end node) and the file's */
    static UINT8 opt[1024], cur[1024];
    UINTN path_len = dsz - 4 + fsz, desc_len = sizeof(NOVA_DESC);
    UINTN opt_len = 6 + desc_len + path_len;
    if (opt_len > sizeof(opt)) return 0;
    UINT32 attrs = LOAD_OPTION_ACTIVE;
    mem_copy(opt, &attrs, 4);
    opt[4] = (UINT8)path_len;
    opt[5] = (UINT8)(path_len >> 8);
    mem_copy(opt + 6, NOVA_DESC, desc_len);
    mem_copy(opt + 6 + desc_len, dev, dsz - 4);
    mem_copy(opt + 6 + desc_len + dsz - 4, li->FilePath, fsz);

    /* An entry that already starts this file on this disk? */
    CHAR16 name[9];
    UINT32 va;
    int free_num = -1;
    for (int num = 0; num < 0x100; num++) {
        boot_var_name(name, (UINT16)num);
        UINTN sz = sizeof(cur);
        EFI_STATUS st = rt->GetVariable(name, &gv, &va, &sz, cur);
        if (st == EFI_NOT_FOUND) { if (free_num < 0) free_num = num; continue; }
        if (!EFI_ERROR(st) && sz == opt_len && bytes_equal(cur + 4, opt + 4, opt_len - 4)) return BOOT_FLAG_BOOT_ENTRY;
    }
    if (free_num < 0) return 0;

    boot_var_name(name, (UINT16)free_num);
    if (EFI_ERROR(rt->SetVariable(name, &gv, VAR_NV_BS_RT, opt_len, opt))) {
        console_printf("Could not add a firmware boot entry for NovaOS\r\n");
        return 0;
    }
    /* First in BootOrder */
    static UINT16 order[256];
    UINTN osz = sizeof(order) - sizeof(UINT16);
    CHAR16 bo[] = { 'B','o','o','t','O','r','d','e','r', 0 };
    if (EFI_ERROR(rt->GetVariable(bo, &gv, &va, &osz, order + 1))) osz = 0;
    order[0] = (UINT16)free_num;
    rt->SetVariable(bo, &gv, VAR_NV_BS_RT, osz + sizeof(UINT16), order);
    console_printf("Added the firmware boot entry Boot%x \"NovaOS\" for this disk\r\n", (UINT64)free_num);
    return BOOT_FLAG_BOOT_ENTRY | BOOT_FLAG_ENTRY_ADDED;
}

/* Whether @path exists on the boot volume */
static BOOLEAN boot_file_exists(CHAR16 *path)
{
    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_FILE_PROTOCOL *root, *file;
    if (EFI_ERROR(g_bs->OpenProtocol(g_boot_device, &fs_guid, (VOID **)&fs, g_image_handle, NULL,
                                     EFI_OPEN_PROTOCOL_GET_PROTOCOL)) ||
        EFI_ERROR(fs->OpenVolume(fs, &root)))
        return FALSE;
    EFI_STATUS status = root->Open(root, &file, path, EFI_FILE_MODE_READ, 0);
    root->Close(root);
    if (EFI_ERROR(status)) return FALSE;
    file->Close(file);
    return TRUE;
}

/* Read a whole file from the boot volume into EfiLoaderData pages (the
 * kernel never reuses those); physical address and size out */
static EFI_STATUS read_boot_file(CHAR16 *path, UINT64 *phys_out, UINT64 *size_out)
{
    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_FILE_PROTOCOL *root, *file;
    EFI_STATUS status = g_bs->OpenProtocol(g_boot_device, &fs_guid, (VOID **)&fs,
                                           g_image_handle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    if (EFI_ERROR(status)) return status;
    status = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(status)) return status;
    status = root->Open(root, &file, path, EFI_FILE_MODE_READ, 0);
    root->Close(root);
    if (EFI_ERROR(status)) return status;
    EFI_GUID info_guid = EFI_FILE_INFO_ID;
    UINT8 fi_storage[sizeof(EFI_FILE_INFO) + 256 * sizeof(CHAR16)];
    EFI_FILE_INFO *fi = (EFI_FILE_INFO *)fi_storage;
    UINTN fi_size = sizeof(fi_storage);
    status = file->GetInfo(file, &info_guid, &fi_size, fi);
    if (EFI_ERROR(status)) { file->Close(file); return status; }
    UINT64 size = fi->FileSize, phys = 0;
    status = g_bs->AllocatePages(AllocateAnyPages, EfiLoaderData, (size + 4095) / 4096 + 1, &phys);
    if (EFI_ERROR(status)) { file->Close(file); return status; }
    UINTN got = (UINTN)size;
    status = file->Read(file, &got, (VOID *)(UINTN)phys);
    file->Close(file);
    if (EFI_ERROR(status) || got != size) return EFI_ERROR(status) ? status : EFI_LOAD_ERROR;
    *phys_out = phys;
    *size_out = size;
    return EFI_SUCCESS;
}

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
    g_boot_device = loaded_image->DeviceHandle;

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

    /* Get file size via GetInfo.  EFI_FILE_INFO ends with a variable-length
     * FileName[], so the buffer must leave room for the name — otherwise the
     * firmware returns EFI_BUFFER_TOO_SMALL.  A fixed oversized buffer avoids
     * a second allocation round-trip. */
    EFI_GUID       info_guid = EFI_FILE_INFO_ID;
    UINT8          fi_storage[sizeof(EFI_FILE_INFO) + 256 * sizeof(CHAR16)];
    EFI_FILE_INFO *fi = (EFI_FILE_INFO *)fi_storage;
    UINTN          fi_size = sizeof(fi_storage);
    status = file->GetInfo(file, &info_guid, &fi_size, fi);
    CHECK(status, "GetInfo(kernel)");

    console_printf("Kernel file size: %u bytes\r\n", fi->FileSize);

    *file_out = file;
    *size_out = (UINTN)fi->FileSize;
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
 * Copy the SMBIOS tables (SMBIOS 3.x entry point first, else 2.x) into
 * EfiLoaderData pages as Windows' RawSMBIOSData, for the kernel's
 * GetSystemFirmwareTable('RSMB').  The kernel then needs no mapping of
 * wherever the firmware keeps them.
 * ----------------------------------------------------------------------- */
static void copy_smbios(UINT64 *base_out, UINT64 *size_out)
{
    EFI_GUID g3 = SMBIOS3_TABLE_GUID, g2 = SMBIOS_TABLE_GUID;
    const UINT8 *ep3 = NULL, *ep2 = NULL;
    for (UINTN i = 0; i < g_st->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *ct = &g_st->ConfigurationTable[i];
        if (EFI_GUID_EQ(ct->VendorGuid, g3)) ep3 = (const UINT8 *)ct->VendorTable;
        else if (EFI_GUID_EQ(ct->VendorGuid, g2)) ep2 = (const UINT8 *)ct->VendorTable;
    }
    UINT8 major, minor, dmi;
    UINT64 table;
    UINT32 len;
    if (ep3 && ep3[0] == '_' && ep3[1] == 'S' && ep3[2] == 'M' && ep3[3] == '3' && ep3[4] == '_') {
        major = ep3[7]; minor = ep3[8]; dmi = ep3[9];
        len   = *(const UINT32 *)(ep3 + 0x0C);           /* maximum size */
        table = *(const UINT64 *)(ep3 + 0x10);
    } else if (ep2 && ep2[0] == '_' && ep2[1] == 'S' && ep2[2] == 'M' && ep2[3] == '_') {
        major = ep2[6]; minor = ep2[7]; dmi = ep2[0x1E] ? ep2[0x1E] : (UINT8)((ep2[6] << 4) | ep2[7]);
        len   = *(const UINT16 *)(ep2 + 0x16);
        table = *(const UINT32 *)(ep2 + 0x18);
    } else {
        return;
    }
    if (!table || !len || len > 0x100000) return;
    /* An SMBIOS 3 length is an upper bound: stop after the end-of-table
     * structure (type 127) */
    const UINT8 *t = (const UINT8 *)(UINTN)table;
    UINT32 off = 0;
    while (off + 4 <= len) {
        UINT8 type = t[off], hl = t[off + 1];
        if (hl < 4) break;
        UINT32 k = off + hl;
        while (k + 1 < len && (t[k] || t[k + 1])) k++;  /* the strings, ended by two zeros */
        k += 2;
        if (k > len) break;
        off = k;
        if (type == 127) break;
    }
    if (!off) return;
    UINT64 phys = 0;
    if (EFI_ERROR(g_bs->AllocatePages(AllocateAnyPages, EfiLoaderData, (8 + off + 4095) / 4096, &phys)))
        return;
    UINT8 *d = (UINT8 *)(UINTN)phys;
    d[0] = 0; d[1] = major; d[2] = minor; d[3] = dmi;
    *(UINT32 *)(d + 4) = off;
    mem_copy(d + 8, t, off);
    *base_out = phys;
    *size_out = 8 + off;
    console_printf("SMBIOS %u.%u: %u bytes\r\n", major, minor, off);
}

/* -----------------------------------------------------------------------
 * Find the best GOP mode (prefer native/largest resolution)
 * ----------------------------------------------------------------------- */
static BOOLEAN gop_linear(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop)
{
    for (UINT32 m = 0; m < gop->Mode->MaxMode; m++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
        UINTN info_size;
        if (EFI_ERROR(gop->QueryMode(gop, m, &info_size, &info))) continue;
        if (info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor ||
            info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor) return 1;
    }
    return 0;
}

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
    /* A GOP with no framebuffer to draw on (OVMF's virtio-gpu-pci one: Blt
     * only) when another adapter has one: that other one */
    if (!gop_linear(gop)) {
        EFI_LOCATE_HANDLE_BUFFER locate = (EFI_LOCATE_HANDLE_BUFFER)g_bs->LocateHandleBuffer2;
        EFI_HANDLE *handles = NULL;
        UINTN n = 0;
        if (!EFI_ERROR(locate(ByProtocol, &gop_guid, NULL, &n, &handles))) {
            for (UINTN i = 0; i < n; i++) {
                EFI_GRAPHICS_OUTPUT_PROTOCOL *g;
                if (!EFI_ERROR(g_bs->HandleProtocol(handles[i], &gop_guid, (VOID **)&g)) && gop_linear(g)) {
                    gop = g;
                    break;
                }
            }
            g_bs->FreePool(handles);
        }
    }

    /* The largest mode with 32-bit pixels (the kernel draws nothing else:
     * Cirrus's 1024x768 is a 24-bit PixelBitMask mode, for one). */
    UINT32 best_mode = gop->Mode->Mode;
    UINT32 best_w    = gop->Mode->Info->HorizontalResolution;
    UINT32 best_h    = gop->Mode->Info->VerticalResolution;
    if (gop->Mode->Info->PixelFormat != PixelBlueGreenRedReserved8BitPerColor &&
        gop->Mode->Info->PixelFormat != PixelRedGreenBlueReserved8BitPerColor)
        best_w = best_h = 0;

    for (UINT32 m = 0; m < gop->Mode->MaxMode; m++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
        UINTN info_size;
        status = gop->QueryMode(gop, m, &info_size, &info);
        if (EFI_ERROR(status)) continue;

        if (info->PixelFormat != PixelBlueGreenRedReserved8BitPerColor &&
            info->PixelFormat != PixelRedGreenBlueReserved8BitPerColor) continue;
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

    /* elf_load() already returns the kernel's VIRTUAL entry point (e_entry
     * is a VMA, adjusted for any relocation).  Adding kernel_virt again
     * would double-count the high-half base and overflow, so use it as-is. */
    UINT64 kernel_virt_entry = kernel_entry;
    console_printf("Kernel: phys=0x%x virt=0x%x entry_virt=0x%x size=0x%x\r\n",
                   kernel_phys, kernel_virt, kernel_virt_entry, kernel_size);

    /* 2. Set up framebuffer ------------------------------------------ */
    BootFramebuffer fb;
    mem_zero(&fb, sizeof(fb));
    init_framebuffer(&fb);

    /* 2b. Installation media (the ISO, on a CD or written to a USB
     *     stick): hand the boot files to the kernel so its installer can
     *     copy them to a disk ------------------------------------------- */
    UINT64 boot_flags = 0, media_kernel = 0, media_kernel_size = 0, media_loader = 0, media_loader_size = 0;
    if (booted_from_cd() || boot_file_exists(BOOTLOG_PATH)) {
        boot_flags |= BOOT_FLAG_LIVE_MEDIA;
        if (booted_from_usb()) {
            boot_flags |= BOOT_FLAG_LIVE_USB;
            console_printf("Booted from a USB stick\r\n");
        }
        if (EFI_ERROR(read_boot_file(KERNEL_PATH, &media_kernel, &media_kernel_size)) ||
            EFI_ERROR(read_boot_file(LOADER_PATH, &media_loader, &media_loader_size))) {
            console_printf("WARNING: could not read the installation files\r\n");
            media_kernel = media_kernel_size = media_loader = media_loader_size = 0;
        } else {
            console_printf("Installation media: kernel %u bytes, loader %u bytes\r\n",
                           media_kernel_size, media_loader_size);
        }
    }
    else {
        boot_flags |= ensure_boot_entry();   /* an installed disk */
    }

    /* 3. Find ACPI RSDP ----------------------------------------------- */
    UINT64 rsdp = find_rsdp();
    console_printf("RSDP physical: 0x%x\r\n", rsdp);
    UINT64 smbios = 0, smbios_size = 0;
    copy_smbios(&smbios, &smbios_size);

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
    bi->boot_flags           = boot_flags;
    bi->media_kernel_base    = media_kernel;
    bi->media_kernel_size    = media_kernel_size;
    bi->media_loader_base    = media_loader;
    bi->media_loader_size    = media_loader_size;
    bi->smbios_base          = smbios;
    bi->smbios_size          = smbios_size;

    /* 9. The kernel entry expects the PHYSICAL address of BootInfo and
     *    derefs it through the physmap itself (PHYSMAP_BASE + phys).  All
     *    pointers stored inside BootInfo (mem_map, framebuffer base, …) are
     *    likewise physical, so pass the physical address here — adding
     *    PHYSMAP_BASE now would make the kernel double-offset into a
     *    non-canonical address and #GP. */
    /* 10. Switch to our page tables and jump to the kernel ------------ */
    jump_to_kernel(new_cr3, stack_top, kernel_virt_entry, boot_info_phys);
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
