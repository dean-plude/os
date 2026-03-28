/*
 * ldr.h — NT Loader (PE32+ Image Loader)
 *
 * The loader maps PE32+ executables into a process's address space and
 * prepares them for execution.  This is the kernel-side equivalent of
 * ntdll.dll's LdrLoadDll / LdrpLoadDll.
 *
 * PE32+ format overview:
 *   IMAGE_DOS_HEADER (64 bytes)
 *     e_magic    = 0x5A4D ("MZ")
 *     e_lfanew   = offset to IMAGE_NT_HEADERS
 *   IMAGE_NT_HEADERS64 = "PE\0\0" + IMAGE_FILE_HEADER + IMAGE_OPTIONAL_HEADER64
 *   IMAGE_SECTION_HEADER[NumberOfSections]
 *
 * Image loading steps (LdrLoadImage):
 *   1. Validate DOS+NT headers, check machine type == IMAGE_FILE_MACHINE_AMD64.
 *   2. Create an image SECTION_OBJECT (MmCreateImageSection).
 *   3. Map the image into the target process VA space (MmMapImageView) at
 *      PreferredBase or a relocated address.
 *   4. Process base relocations if load_base != PreferredBase.
 *   5. Resolve import directory (load each imported DLL, fix up IAT).
 *   6. Set up PEB.Ldr (LDR_DATA_TABLE_ENTRY chain).
 *   7. Return the entry point VA.
 *
 * Relocations (IMAGE_BASE_RELOCATION):
 *   Each block covers 4 KB; each 16-bit entry is (type<<12 | offset).
 *   For AMD64: type 10 = IMAGE_REL_BASED_DIR64 — add (delta) to UINT64 at
 *              the given offset within the block's VirtualAddress page.
 *   delta = load_base - PreferredBase.
 *
 * Imports (IMAGE_IMPORT_DESCRIPTOR):
 *   For each DLL import descriptor:
 *     - Read DLL name string.
 *     - Load that DLL (LdrLoadImage recursively).
 *     - Walk the Import Lookup Table (ILT): for each thunk, either
 *       IMAGE_IMPORT_BY_NAME (name lookup) or ordinal import.
 *     - Write resolved VA into the Import Address Table (IAT).
 *
 * Phase 3 scope:
 *   - Only AMD64 PE32+ images (machine 0x8664).
 *   - Built-in DLL stubs: ntdll, kernel32, user32, msvcrt — each provides a
 *     fixed export table of stub functions.
 *   - Actual DLL file loading from disk: Phase 4 (needs file system).
 *   - No TLS, SEH/CFG processing, or manifest parsing.
 *   - No ASLR (load at preferred base when possible).
 */

#pragma once

#include "../include/types.h"
#include "../ps/ps.h"
#include "../mm/section.h"
#include "../mm/vma.h"

/* -----------------------------------------------------------------------
 * PE32+ constants
 * ----------------------------------------------------------------------- */
#define IMAGE_DOS_SIGNATURE         0x5A4D      /* MZ */
#define IMAGE_NT_SIGNATURE          0x00004550  /* PE\0\0 */
#define IMAGE_FILE_MACHINE_AMD64    0x8664
#define IMAGE_NT_OPTIONAL_HDR64_MAGIC 0x020B    /* PE32+ */

/* DataDirectory indices */
#define IMAGE_DIRECTORY_ENTRY_EXPORT     0
#define IMAGE_DIRECTORY_ENTRY_IMPORT     1
#define IMAGE_DIRECTORY_ENTRY_RESOURCE   2
#define IMAGE_DIRECTORY_ENTRY_EXCEPTION  3
#define IMAGE_DIRECTORY_ENTRY_SECURITY   4
#define IMAGE_DIRECTORY_ENTRY_BASERELOC  5
#define IMAGE_DIRECTORY_ENTRY_DEBUG      6
#define IMAGE_DIRECTORY_ENTRY_TLS        9
#define IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG 10
#define IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT 11
#define IMAGE_DIRECTORY_ENTRY_IAT        12
#define IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT 13
#define IMAGE_NUMBEROF_DIRECTORY_ENTRIES 16

/* Section characteristics */
#define IMAGE_SCN_CNT_CODE              0x00000020
#define IMAGE_SCN_CNT_INITIALIZED_DATA  0x00000040
#define IMAGE_SCN_CNT_UNINITIALIZED_DATA 0x00000080
#define IMAGE_SCN_MEM_EXECUTE           0x20000000
#define IMAGE_SCN_MEM_READ              0x40000000
#define IMAGE_SCN_MEM_WRITE             0x80000000

/* Relocation types */
#define IMAGE_REL_BASED_ABSOLUTE    0   /* Skip (padding) */
#define IMAGE_REL_BASED_DIR64       10  /* 64-bit VA fixup (AMD64) */

/* -----------------------------------------------------------------------
 * PE structures (clean-room, matching PE specification)
 * The field names and offsets match Windows PE/COFF specification.
 * ----------------------------------------------------------------------- */

typedef struct _IMAGE_DOS_HEADER {
    UINT16 e_magic;
    UINT16 e_cblp, e_cp, e_crlc, e_cparhdr, e_minalloc, e_maxalloc;
    UINT16 e_ss, e_sp, e_csum, e_ip, e_cs, e_lfarlc, e_ovno;
    UINT16 e_res[4];
    UINT16 e_oemid, e_oeminfo;
    UINT16 e_res2[10];
    INT32  e_lfanew;    /* Offset to NT headers */
} IMAGE_DOS_HEADER, *PIMAGE_DOS_HEADER;

typedef struct _IMAGE_FILE_HEADER {
    UINT16 Machine;
    UINT16 NumberOfSections;
    UINT32 TimeDateStamp;
    UINT32 PointerToSymbolTable;
    UINT32 NumberOfSymbols;
    UINT16 SizeOfOptionalHeader;
    UINT16 Characteristics;
} IMAGE_FILE_HEADER, *PIMAGE_FILE_HEADER;

typedef struct _IMAGE_DATA_DIRECTORY {
    UINT32 VirtualAddress;
    UINT32 Size;
} IMAGE_DATA_DIRECTORY;

typedef struct _IMAGE_OPTIONAL_HEADER64 {
    UINT16 Magic;                    /* 0x020B */
    UINT8  MajorLinkerVersion;
    UINT8  MinorLinkerVersion;
    UINT32 SizeOfCode;
    UINT32 SizeOfInitializedData;
    UINT32 SizeOfUninitializedData;
    UINT32 AddressOfEntryPoint;      /* RVA of entry point */
    UINT32 BaseOfCode;
    UINT64 ImageBase;                /* Preferred load address */
    UINT32 SectionAlignment;
    UINT32 FileAlignment;
    UINT16 MajorOperatingSystemVersion;
    UINT16 MinorOperatingSystemVersion;
    UINT16 MajorImageVersion;
    UINT16 MinorImageVersion;
    UINT16 MajorSubsystemVersion;
    UINT16 MinorSubsystemVersion;
    UINT32 Win32VersionValue;
    UINT32 SizeOfImage;
    UINT32 SizeOfHeaders;
    UINT32 CheckSum;
    UINT16 Subsystem;
    UINT16 DllCharacteristics;
    UINT64 SizeOfStackReserve;
    UINT64 SizeOfStackCommit;
    UINT64 SizeOfHeapReserve;
    UINT64 SizeOfHeapCommit;
    UINT32 LoaderFlags;
    UINT32 NumberOfRvaAndSizes;
    IMAGE_DATA_DIRECTORY DataDirectory[IMAGE_NUMBEROF_DIRECTORY_ENTRIES];
} IMAGE_OPTIONAL_HEADER64, *PIMAGE_OPTIONAL_HEADER64;

typedef struct _IMAGE_NT_HEADERS64 {
    UINT32                  Signature;
    IMAGE_FILE_HEADER       FileHeader;
    IMAGE_OPTIONAL_HEADER64 OptionalHeader;
} IMAGE_NT_HEADERS64, *PIMAGE_NT_HEADERS64;

/* Pointer to first section header: immediately follows the optional header */
#define IMAGE_FIRST_SECTION(nth) \
    ((PIMAGE_SECTION_HEADER)((UINT8 *)(&(nth)->OptionalHeader) + \
     (nth)->FileHeader.SizeOfOptionalHeader))

typedef struct _IMAGE_SECTION_HEADER {
    UINT8  Name[8];
    UINT32 VirtualSize;         /* Misc.VirtualSize */
    UINT32 VirtualAddress;
    UINT32 SizeOfRawData;
    UINT32 PointerToRawData;
    UINT32 PointerToRelocations;
    UINT32 PointerToLinenumbers;
    UINT16 NumberOfRelocations;
    UINT16 NumberOfLinenumbers;
    UINT32 Characteristics;
} IMAGE_SECTION_HEADER, *PIMAGE_SECTION_HEADER;

typedef struct _IMAGE_BASE_RELOCATION {
    UINT32 VirtualAddress;
    UINT32 SizeOfBlock;
    /* UINT16 TypeOffset[1] follows */
} IMAGE_BASE_RELOCATION, *PIMAGE_BASE_RELOCATION;

typedef struct _IMAGE_IMPORT_DESCRIPTOR {
    union {
        UINT32 Characteristics;
        UINT32 OriginalFirstThunk;  /* RVA of ILT (Import Lookup Table) */
    };
    UINT32 TimeDateStamp;
    UINT32 ForwarderChain;
    UINT32 Name;                    /* RVA of DLL name string */
    UINT32 FirstThunk;              /* RVA of IAT (Import Address Table) */
} IMAGE_IMPORT_DESCRIPTOR, *PIMAGE_IMPORT_DESCRIPTOR;

typedef struct _IMAGE_EXPORT_DIRECTORY {
    UINT32 Characteristics;
    UINT32 TimeDateStamp;
    UINT16 MajorVersion;
    UINT16 MinorVersion;
    UINT32 Name;                    /* RVA of module name */
    UINT32 Base;                    /* Ordinal base */
    UINT32 NumberOfFunctions;
    UINT32 NumberOfNames;
    UINT32 AddressOfFunctions;      /* RVA of EAT (export address table) */
    UINT32 AddressOfNames;          /* RVA of name pointer table */
    UINT32 AddressOfNameOrdinals;   /* RVA of ordinal table */
} IMAGE_EXPORT_DIRECTORY, *PIMAGE_EXPORT_DIRECTORY;

typedef struct _IMAGE_IMPORT_BY_NAME {
    UINT16 Hint;
    CHAR   Name[1];
} IMAGE_IMPORT_BY_NAME, *PIMAGE_IMPORT_BY_NAME;

/* -----------------------------------------------------------------------
 * LDR_MODULE — kernel-side per-loaded-module descriptor
 * Placed in the target process's address space as LDR_DATA_TABLE_ENTRY
 * and also tracked in the kernel via an LDR_MODULE chain.
 * ----------------------------------------------------------------------- */
typedef struct _LDR_MODULE {
    struct _LDR_MODULE *Next;
    UINT64  ImageBase;          /* Actual load base */
    UINT64  ImageSize;
    UINT64  EntryPoint;         /* Absolute VA of EP */
    char    BaseName[64];       /* ASCII module name (e.g. "ntdll.dll") */
    /* Pointer into process PEB.Ldr for the LDR_DATA_TABLE_ENTRY */
    void   *LdrEntry;           /* Points into process user VA space */
} LDR_MODULE, *PLDR_MODULE;

/* -----------------------------------------------------------------------
 * LOAD_IMAGE_RESULT — returned by LdrLoadImage
 * ----------------------------------------------------------------------- */
typedef struct _LOAD_IMAGE_RESULT {
    UINT64  ImageBase;
    UINT64  EntryPoint;         /* Absolute VA */
    UINT64  StackBase;          /* Allocated user-mode stack base */
    UINT64  StackSize;          /* Allocated user-mode stack size */
} LOAD_IMAGE_RESULT, *PLOAD_IMAGE_RESULT;

/* -----------------------------------------------------------------------
 * Built-in stub DLL export table entry
 * ----------------------------------------------------------------------- */
typedef struct _STUB_EXPORT {
    const char *Name;
    UINT64      StubAddress;    /* Absolute kernel VA of the stub */
} STUB_EXPORT;

typedef struct _STUB_DLL {
    const char         *Name;    /* DLL name (e.g. "ntdll.dll"), case-insensitive */
    const STUB_EXPORT  *Exports;
    UINT32              ExportCount;
    UINT64              ImageBase;  /* Assigned when stubs are mapped */
} STUB_DLL;

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

/* Initialize the loader subsystem (map stub DLLs into the kernel VA) */
void LdrInitialize(void);

/*
 * Load a PE32+ image into a process.
 *
 * @PeBuffer:   Pointer to raw PE image in kernel memory (flat file copy).
 * @PeSize:     Size of the PE image buffer in bytes.
 * @Process:    Target EPROCESS to load into.
 * @Result:     Receives load results (ImageBase, EntryPoint, stack info).
 *
 * On success, the image is mapped, imports resolved, relocations applied,
 * and a user-mode stack is allocated.  The caller then creates a user
 * thread at EntryPoint.
 */
NTSTATUS LdrLoadImage(void *PeBuffer, UINT64 PeSize,
                       PEPROCESS Process, PLOAD_IMAGE_RESULT Result);

/*
 * Resolve an export by name from a loaded module list.
 * @ModuleList:  Head of the kernel-side LDR_MODULE chain.
 * @DllName:     Name of the DLL (case-insensitive).
 * @FuncName:    Name of the export function.
 * Returns the absolute VA of the export, or 0 if not found.
 */
UINT64 LdrGetProcAddress(PLDR_MODULE ModuleList,
                          const char *DllName, const char *FuncName);

/*
 * Apply base relocations to an already-mapped image.
 * @ImageBase:    Actual load base VA.
 * @PrefBase:     Preferred base from PE header.
 * @RelocRVA:     RVA of the relocation directory.
 * @RelocSize:    Size of the relocation directory.
 */
NTSTATUS LdrApplyRelocations(UINT64 ImageBase, UINT64 PrefBase,
                               UINT32 RelocRVA, UINT32 RelocSize);
