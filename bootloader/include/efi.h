/*
 * efi.h — Minimal UEFI API headers for the NovaOS bootloader
 *
 * This is a clean-room, MIT-licensed subset of the UEFI 2.9 specification.
 * We include only what we actually use — no bloat from gnu-efi or EDK2.
 *
 * Reference: UEFI Specification 2.9, January 2021
 *            https://uefi.org/specifications
 *
 * Key design decisions:
 *  - All UEFI interfaces use the MS x64 calling convention (__attribute__((ms_abi)))
 *  - Strings are UCS-2 (wchar_t, 2 bytes), not UTF-16LE surrogates
 *  - Pointers are 64-bit; this header targets x86_64 only
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

/* -----------------------------------------------------------------------
 * Primitive types
 * ----------------------------------------------------------------------- */
typedef uint8_t   UINT8;
typedef uint16_t  UINT16;
typedef uint32_t  UINT32;
typedef uint64_t  UINT64;
typedef int8_t    INT8;
typedef int16_t   INT16;
typedef int32_t   INT32;
typedef int64_t   INT64;
typedef uint8_t   BOOLEAN;
typedef uintptr_t UINTN;
typedef intptr_t  INTN;
typedef uint16_t  CHAR16;   /* UCS-2 */
typedef void      VOID;

#define TRUE  1
#define FALSE 0
#define NULL  ((void *)0)

/* -----------------------------------------------------------------------
 * Status codes
 * EFI_STATUS is a UINTN. The high bit set = error; clear = success/warning.
 * ----------------------------------------------------------------------- */
typedef UINTN EFI_STATUS;

#define EFI_ERROR_BIT        ((UINTN)1 << 63)
#define EFI_SUCCESS          ((EFI_STATUS)0)
#define EFI_LOAD_ERROR       (EFI_ERROR_BIT | 1)
#define EFI_INVALID_PARAMETER (EFI_ERROR_BIT | 2)
#define EFI_UNSUPPORTED      (EFI_ERROR_BIT | 3)
#define EFI_BUFFER_TOO_SMALL (EFI_ERROR_BIT | 5)
#define EFI_NOT_FOUND        (EFI_ERROR_BIT | 14)
#define EFI_OUT_OF_RESOURCES (EFI_ERROR_BIT | 9)
#define EFI_NO_MEDIA         (EFI_ERROR_BIT | 12)
#define EFI_END_OF_FILE      (EFI_ERROR_BIT | 31)

#define EFI_ERROR(s)  (((INTN)(s)) < 0)

/* -----------------------------------------------------------------------
 * GUID — 128-bit identifier used for protocols and tables
 * ----------------------------------------------------------------------- */
typedef struct {
    UINT32 data1;
    UINT16 data2;
    UINT16 data3;
    UINT8  data4[8];
} EFI_GUID;

#define EFI_GUID_EQ(a, b) \
    ((a).data1 == (b).data1 && (a).data2 == (b).data2 && \
     (a).data3 == (b).data3 && \
     ((UINT64 *)&(a).data4)[0] == ((UINT64 *)&(b).data4)[0])

/* -----------------------------------------------------------------------
 * Handles and events
 * ----------------------------------------------------------------------- */
typedef void *EFI_HANDLE;
typedef void *EFI_EVENT;
typedef UINT64 EFI_LBA;
typedef UINTN EFI_TPL;

/* -----------------------------------------------------------------------
 * Memory types and allocation
 * ----------------------------------------------------------------------- */
typedef enum {
    EfiReservedMemoryType       = 0,
    EfiLoaderCode               = 1,
    EfiLoaderData               = 2,
    EfiBootServicesCode         = 3,
    EfiBootServicesData         = 4,
    EfiRuntimeServicesCode      = 5,
    EfiRuntimeServicesData      = 6,
    EfiConventionalMemory       = 7,
    EfiUnusableMemory           = 8,
    EfiACPIReclaimMemory        = 9,
    EfiACPIMemoryNVS            = 10,
    EfiMemoryMappedIO           = 11,
    EfiMemoryMappedIOPortSpace  = 12,
    EfiPalCode                  = 13,
    EfiPersistentMemory         = 14,
    EfiMaxMemoryType
} EFI_MEMORY_TYPE;

typedef enum {
    AllocateAnyPages,
    AllocateMaxAddress,
    AllocateAddress,
    MaxAllocateType
} EFI_ALLOCATE_TYPE;

typedef struct {
    UINT32 type;
    UINT32 _pad;
    UINT64 physical_start;
    UINT64 virtual_start;
    UINT64 num_pages;
    UINT64 attribute;
} EFI_MEMORY_DESCRIPTOR;

/* Memory attribute flags */
#define EFI_MEMORY_UC   0x0000000000000001ULL
#define EFI_MEMORY_WC   0x0000000000000002ULL
#define EFI_MEMORY_WT   0x0000000000000004ULL
#define EFI_MEMORY_WB   0x0000000000000008ULL
#define EFI_MEMORY_WP   0x0000000000001000ULL
#define EFI_MEMORY_RP   0x0000000000002000ULL
#define EFI_MEMORY_XP   0x0000000000004000ULL
#define EFI_MEMORY_RUNTIME 0x8000000000000000ULL

/* -----------------------------------------------------------------------
 * Simple Text Output Protocol — for early console output
 * ----------------------------------------------------------------------- */
typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_TEXT_RESET)(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    BOOLEAN                         ExtendedVerification);

typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_TEXT_STRING)(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    CHAR16                          *String);

typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_TEXT_CLEAR_SCREEN)(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This);

struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_TEXT_RESET        Reset;
    EFI_TEXT_STRING       OutputString;
    void                 *TestString;
    void                 *QueryMode;
    void                 *SetMode;
    void                 *SetAttribute;
    EFI_TEXT_CLEAR_SCREEN ClearScreen;
    void                 *SetCursorPosition;
    void                 *EnableCursor;
    void                 *Mode;
};

/* -----------------------------------------------------------------------
 * Simple File System & File Protocols
 * ----------------------------------------------------------------------- */
#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    { 0x964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

typedef struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;
typedef struct _EFI_FILE_PROTOCOL               EFI_FILE_PROTOCOL;

typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_VOLUME_OPEN)(
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
    EFI_FILE_PROTOCOL              **Root);

struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    UINT64           Revision;
    EFI_VOLUME_OPEN  OpenVolume;
};

typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_FILE_OPEN)(
    EFI_FILE_PROTOCOL  *This,
    EFI_FILE_PROTOCOL **NewHandle,
    CHAR16             *FileName,
    UINT64              OpenMode,
    UINT64              Attributes);

typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_FILE_CLOSE)(
    EFI_FILE_PROTOCOL *This);

typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_FILE_READ)(
    EFI_FILE_PROTOCOL *This,
    UINTN             *BufferSize,
    VOID              *Buffer);

typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_FILE_WRITE)(
    EFI_FILE_PROTOCOL *This,
    UINTN             *BufferSize,
    VOID              *Buffer);

typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_FILE_SET_POSITION)(
    EFI_FILE_PROTOCOL *This,
    UINT64             Position);

typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_FILE_GET_POSITION)(
    EFI_FILE_PROTOCOL *This,
    UINT64            *Position);

typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_FILE_GET_INFO)(
    EFI_FILE_PROTOCOL *This,
    EFI_GUID          *InformationType,
    UINTN             *BufferSize,
    VOID              *Buffer);

#define EFI_FILE_INFO_ID \
    { 0x09576e92, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

typedef struct {
    UINT64 Size;         /* Size of this struct including FileName */
    UINT64 FileSize;
    UINT64 PhysicalSize;
    UINT64 CreateTime[2];
    UINT64 LastAccessTime[2];
    UINT64 ModificationTime[2];
    UINT64 Attribute;
    CHAR16 FileName[1]; /* Variable length */
} EFI_FILE_INFO;

#define EFI_FILE_MODE_READ   0x0000000000000001ULL
#define EFI_FILE_MODE_WRITE  0x0000000000000002ULL
#define EFI_FILE_MODE_CREATE 0x8000000000000000ULL
#define EFI_FILE_READ_ONLY   0x0000000000000001ULL

struct _EFI_FILE_PROTOCOL {
    UINT64                Revision;
    EFI_FILE_OPEN         Open;
    EFI_FILE_CLOSE        Close;
    void                 *Delete;
    EFI_FILE_READ         Read;
    EFI_FILE_WRITE        Write;
    EFI_FILE_GET_POSITION GetPosition;
    EFI_FILE_SET_POSITION SetPosition;
    EFI_FILE_GET_INFO     GetInfo;
    void                 *SetInfo;
    void                 *Flush;
};

/* -----------------------------------------------------------------------
 * Loaded Image Protocol — lets us find the device handle for our binary
 * ----------------------------------------------------------------------- */
#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    { 0x5b1b31a1, 0x9562, 0x11d2, { 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

typedef struct {
    UINT32      Revision;
    EFI_HANDLE  ParentHandle;
    void       *SystemTable;
    EFI_HANDLE  DeviceHandle;
    void       *FilePath;
    VOID       *Reserved;
    UINT32      LoadOptionsSize;
    VOID       *LoadOptions;
    VOID       *ImageBase;
    UINT64      ImageSize;
    UINT32      ImageCodeType;  /* EFI_MEMORY_TYPE */
    UINT32      ImageDataType;  /* EFI_MEMORY_TYPE */
    void       *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

/* -----------------------------------------------------------------------
 * Graphics Output Protocol (GOP) — linear framebuffer
 * ----------------------------------------------------------------------- */
#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } }

typedef enum {
    PixelRedGreenBlueReserved8BitPerColor,
    PixelBlueGreenRedReserved8BitPerColor,
    PixelBitMask,
    PixelBltOnly,
    PixelFormatMax
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    UINT32 Version;
    UINT32 HorizontalResolution;
    UINT32 VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT  PixelFormat;
    UINT32 PixelInformation[4]; /* Red/Green/Blue/Reserved masks (PixelBitMask) */
    UINT32 PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    UINT32                              MaxMode;
    UINT32                              Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN                               SizeOfInfo;
    UINT64                              FrameBufferBase;
    UINTN                               FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct _EFI_GRAPHICS_OUTPUT_PROTOCOL EFI_GRAPHICS_OUTPUT_PROTOCOL;

typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE)(
    EFI_GRAPHICS_OUTPUT_PROTOCOL          *This,
    UINT32                                 ModeNumber,
    UINTN                                 *SizeOfInfo,
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);

typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE)(
    EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    UINT32                        ModeNumber);

struct _EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE QueryMode;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE   SetMode;
    void                                   *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE      *Mode;
};

/* -----------------------------------------------------------------------
 * Boot Services Table
 * ----------------------------------------------------------------------- */
typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_ALLOCATE_PAGES)(
    EFI_ALLOCATE_TYPE    Type,
    EFI_MEMORY_TYPE      MemoryType,
    UINTN                Pages,
    UINT64              *Memory);

typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_FREE_PAGES)(
    UINT64 Memory,
    UINTN  Pages);

typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_GET_MEMORY_MAP)(
    UINTN                  *MemoryMapSize,
    EFI_MEMORY_DESCRIPTOR  *MemoryMap,
    UINTN                  *MapKey,
    UINTN                  *DescriptorSize,
    UINT32                 *DescriptorVersion);

typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_ALLOCATE_POOL)(
    EFI_MEMORY_TYPE  PoolType,
    UINTN            Size,
    VOID           **Buffer);

typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_FREE_POOL)(
    VOID *Buffer);

typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_EXIT_BOOT_SERVICES)(
    EFI_HANDLE ImageHandle,
    UINTN      MapKey);

typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_HANDLE_PROTOCOL)(
    EFI_HANDLE  Handle,
    EFI_GUID   *Protocol,
    VOID      **Interface);

typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_LOCATE_PROTOCOL)(
    EFI_GUID  *Protocol,
    VOID      *Registration,
    VOID     **Interface);

typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_LOCATE_HANDLE_BUFFER)(
    UINT32      SearchType,
    EFI_GUID   *Protocol,
    VOID       *SearchKey,
    UINTN      *NoHandles,
    EFI_HANDLE **Buffer);

#define ByProtocol 2

typedef EFI_STATUS (__attribute__((ms_abi)) *EFI_OPEN_PROTOCOL)(
    EFI_HANDLE  Handle,
    EFI_GUID   *Protocol,
    VOID      **Interface,
    EFI_HANDLE  AgentHandle,
    EFI_HANDLE  ControllerHandle,
    UINT32      Attributes);

#define EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL  0x00000001
#define EFI_OPEN_PROTOCOL_GET_PROTOCOL        0x00000002

typedef struct {
    /* Header */
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
    /* Task priority services — MUST be first per the UEFI spec; omitting
     * them shifts every later function pointer (OpenProtocol, AllocatePool,
     * ExitBootServices, …) by 16 bytes and calls the wrong service. */
    void *RaiseTPL;
    void *RestoreTPL;
    /* Memory services */
    EFI_ALLOCATE_PAGES     AllocatePages;
    EFI_FREE_PAGES         FreePages;
    EFI_GET_MEMORY_MAP     GetMemoryMap;
    EFI_ALLOCATE_POOL      AllocatePool;
    EFI_FREE_POOL          FreePool;
    /* Event & timer services */
    void *CreateEvent;
    void *SetTimer;
    void *WaitForEvent;
    void *SignalEvent;
    void *CloseEvent;
    void *CheckEvent;
    /* Protocol handler services */
    void *InstallProtocolInterface;
    void *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    EFI_HANDLE_PROTOCOL    HandleProtocol;
    void                  *Reserved2;
    void *RegisterProtocolNotify;
    EFI_LOCATE_HANDLE_BUFFER LocateHandleBuffer;  /* This is actually LocateHandle */
    void *LocateDevicePath;
    void *InstallConfigurationTable;
    /* Image services */
    void *LoadImage;
    void *StartImage;
    void *Exit;
    void *UnloadImage;
    EFI_EXIT_BOOT_SERVICES ExitBootServices;
    /* Miscellaneous */
    void *GetNextMonotonicCount;
    void *Stall;
    void *SetWatchdogTimer;
    /* Driver support */
    void *ConnectController;
    void *DisconnectController;
    /* Open/close protocol */
    EFI_OPEN_PROTOCOL OpenProtocol;
    void *CloseProtocol;
    void *OpenProtocolInformation;
    /* Library services */
    void *ProtocolsPerHandle;
    void *LocateHandleBuffer2; /* This slot is LocateHandleBuffer in UEFI spec */
    EFI_LOCATE_PROTOCOL LocateProtocol;
    void *InstallMultipleProtocolInterfaces;
    void *UninstallMultipleProtocolInterfaces;
    /* CRC */
    void *CalculateCrc32;
    /* Memory utilities */
    void *CopyMem;
    void *SetMem;
    void *CreateEventEx;
} EFI_BOOT_SERVICES;

/* -----------------------------------------------------------------------
 * Configuration Table (used to find ACPI RSDP)
 * ----------------------------------------------------------------------- */
typedef struct {
    EFI_GUID VendorGuid;
    VOID    *VendorTable;
} EFI_CONFIGURATION_TABLE;

#define EFI_ACPI_20_TABLE_GUID \
    { 0x8868e871, 0xe4f1, 0x11d3, { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } }

/* SMBIOS entry points: the 64-bit "_SM3_" one (SMBIOS 3.x) and the older "_SM_" one */
#define SMBIOS3_TABLE_GUID \
    { 0xf2fd1544, 0x9794, 0x4a2c, { 0x99, 0x2e, 0xe5, 0xbb, 0xcf, 0x20, 0xe3, 0x94 } }
#define SMBIOS_TABLE_GUID \
    { 0xeb9d2d31, 0x2d88, 0x11d3, { 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } }

/* -----------------------------------------------------------------------
 * System Table
 * ----------------------------------------------------------------------- */
typedef struct {
    UINT64                         Signature;
    UINT32                         Revision;
    UINT32                         HeaderSize;
    UINT32                         CRC32;
    UINT32                         Reserved;
    CHAR16                        *FirmwareVendor;
    UINT32                         FirmwareRevision;
    UINT32                         _pad;
    EFI_HANDLE                     ConsoleInHandle;
    void                          *ConIn;
    EFI_HANDLE                     ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE                     StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    void                          *RuntimeServices;
    EFI_BOOT_SERVICES             *BootServices;
    UINTN                          NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE       *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* -----------------------------------------------------------------------
 * Entry point — all UEFI applications start here
 * ----------------------------------------------------------------------- */
typedef EFI_STATUS (__attribute__((ms_abi)) EFI_IMAGE_ENTRY_POINT)(
    EFI_HANDLE      ImageHandle,
    EFI_SYSTEM_TABLE *SystemTable);
