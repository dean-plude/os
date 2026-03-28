/*
 * boot_protocol.h — NovaOS boot handoff protocol
 *
 * This header is shared between the UEFI bootloader and the kernel.
 * The bootloader fills a BootInfo structure, passes it to the kernel
 * entry point, and the kernel uses it to finish initialization.
 *
 * Design rationale: We define our own boot protocol (rather than using
 * something like Multiboot2) so we can carry NT-specific data (ACPI
 * pointers, future secure-boot measurements) and evolve it freely.
 *
 * The structure is plain C, 8-byte aligned throughout, and must remain
 * ABI-stable between bootloader and kernel builds.
 */

#pragma once

#include <stdint.h>

/* Increment BOOT_PROTOCOL_VERSION when the struct layout changes. */
#define BOOT_MAGIC            UINT64_C(0x4E4F564100424F4F)   /* "NOVA\0BOO" */
#define BOOT_PROTOCOL_VERSION 2

/* -----------------------------------------------------------------------
 * Memory map — mirrors UEFI EFI_MEMORY_DESCRIPTOR, but uses our own
 * type enum so the kernel doesn't need UEFI headers at runtime.
 * ----------------------------------------------------------------------- */
typedef enum {
    BOOT_MEM_RESERVED        = 0,   /* Must not be used */
    BOOT_MEM_LOADER_CODE     = 1,   /* Bootloader text — reclaimable after init */
    BOOT_MEM_LOADER_DATA     = 2,   /* Bootloader data — reclaimable after init */
    BOOT_MEM_BOOT_SERVICES   = 3,   /* UEFI boot services — reclaimable */
    BOOT_MEM_RUNTIME         = 4,   /* UEFI runtime services — must keep mapped */
    BOOT_MEM_CONVENTIONAL    = 5,   /* Free RAM */
    BOOT_MEM_UNUSABLE        = 6,   /* Bad / ECC scrub failure */
    BOOT_MEM_ACPI_RECLAIM    = 7,   /* ACPI tables — reclaimable after parsing */
    BOOT_MEM_ACPI_NVS        = 8,   /* ACPI NVS — firmware-owned, do not touch */
    BOOT_MEM_MMIO            = 9,   /* Device MMIO range */
    BOOT_MEM_KERNEL          = 10,  /* Kernel image (marked by bootloader) */
    BOOT_MEM_INITRD          = 11,  /* Reserved for future initrd use */
} BootMemType;

/* Flags stored in BootMemDescriptor.flags */
#define BOOT_MEM_FLAG_WRITABLE    (1u << 0)
#define BOOT_MEM_FLAG_EXECUTABLE  (1u << 1)
#define BOOT_MEM_FLAG_WC          (1u << 2)   /* Write-combining */

typedef struct {
    uint64_t     physical_base;  /* First byte of this region (page-aligned) */
    uint64_t     num_pages;      /* Size in 4KiB pages */
    uint32_t     type;           /* BootMemType */
    uint32_t     flags;          /* BOOT_MEM_FLAG_* */
} __attribute__((packed)) BootMemDescriptor;

/* -----------------------------------------------------------------------
 * Framebuffer descriptor
 * pixel_format: 0 = BGR_8888, 1 = RGB_8888, 2 = BITMASK (not supported)
 * ----------------------------------------------------------------------- */
typedef struct {
    uint64_t  base;                /* Physical base address */
    uint64_t  size;                /* Total byte size */
    uint32_t  width;               /* Horizontal resolution (pixels) */
    uint32_t  height;              /* Vertical resolution (pixels) */
    uint32_t  pixels_per_scanline; /* May be > width due to alignment padding */
    uint32_t  pixel_format;        /* 0=BGR, 1=RGB */
} BootFramebuffer;

/* -----------------------------------------------------------------------
 * Top-level boot information block
 *
 * The bootloader allocates this in EfiLoaderData so the kernel owns it.
 * The pointer passed to the kernel entry is the PHYSICAL address; the
 * kernel must account for this before virtual memory is live.
 * ----------------------------------------------------------------------- */
typedef struct {
    uint64_t         magic;          /* Must equal BOOT_MAGIC */
    uint32_t         version;        /* Must equal BOOT_PROTOCOL_VERSION */
    uint32_t         _pad0;

    /* Memory map (array of BootMemDescriptor, allocated in EfiLoaderData) */
    uint64_t         mem_map;        /* Physical address of descriptor array */
    uint32_t         mem_map_count;  /* Number of entries */
    uint32_t         _pad1;

    /* Framebuffer (may be zeroed if no display available) */
    BootFramebuffer  framebuffer;

    /* ACPI — physical address of RSDP (ACPI 2.0+) */
    uint64_t         rsdp_physical;

    /* Kernel placement in memory */
    uint64_t         kernel_physical_base; /* Lowest physical address used */
    uint64_t         kernel_virtual_base;  /* Lowest virtual address (linked at) */
    uint64_t         kernel_size;          /* Byte size of all kernel pages */

    /* Initial stack the bootloader set up for the kernel entry call */
    uint64_t         boot_stack_top;       /* Virtual address (stack grows down) */

    /* Initial ramdisk (CPIO newc archive, optional)
     * Set to 0/0 if no initrd was loaded. */
    uint64_t         initrd_base;          /* Physical base address */
    uint64_t         initrd_size;          /* Size in bytes */
} BootInfo;

/* Sanity check: kernel entry function signature */
typedef void (*KernelEntryFn)(const BootInfo *info);
