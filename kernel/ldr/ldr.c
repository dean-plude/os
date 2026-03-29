/*
 * ldr.c — NT PE32+ Image Loader (Phase 3)
 *
 * Loads PE32+ executables into a process address space:
 *   1. Validates and parses PE headers.
 *   2. Creates an image section (MmCreateImageSection).
 *   3. Maps all PE sections into process VA space (MmMapImageView).
 *   4. Applies base relocations (if load base != preferred base).
 *   5. Resolves imports against built-in stub DLLs.
 *   6. Allocates a user-mode stack.
 *   7. Sets up PEB.Ldr (LDR_DATA_TABLE_ENTRY list in user VA).
 *
 * Built-in stub DLLs:
 *   The stub DLL mechanism provides a minimal export table for the
 *   standard Windows DLLs.  Each stub function is a kernel-mode trampoline
 *   that calls the matching NT syscall handler.  Since Phase 3 code runs
 *   in ring 0, the stubs are just direct function calls.
 *
 *   When user mode is fully implemented (Phase 5), the stubs will live
 *   in user-mode pages mapped into every process (like ntdll.dll on Windows).
 *
 * String comparison: all DLL/function name lookups are case-insensitive
 * ASCII (PE format only uses ASCII for import names).
 */

#include "ldr.h"
#include "../mm/section.h"
#include "../mm/vma.h"
#include "../mm/vmm.h"
#include "../mm/pmm.h"
#include "../ps/ps.h"
#include "../ke/printf.h"
#include "../ke/syscall.h"
#include "../lib/string.h"
#include "../include/types.h"
#include "../arch/x86_64/paging.h"

/* -----------------------------------------------------------------------
 * Case-insensitive ASCII string comparison
 * ----------------------------------------------------------------------- */
static int ascii_icmp(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
        a++; b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/* -----------------------------------------------------------------------
 * Stub DLL export tables
 *
 * These provide minimal implementations matching the Windows API surface.
 * Each stub either:
 *   (a) Calls the corresponding NT syscall handler directly (kernel mode),
 *   (b) Returns a default value (STATUS_SUCCESS or NULL), or
 *   (c) Is a no-op with a sensible return value.
 *
 * Format: function pointer cast to UINT64 (the VA of the stub in kernel space).
 * ----------------------------------------------------------------------- */

/* Forward declaration of stub functions */
static UINT64 stub_NtClose(UINT64 h, UINT64 a2, UINT64 a3, UINT64 a4);
static UINT64 stub_NtAllocateVirtualMemory(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4);
static UINT64 stub_NtFreeVirtualMemory(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4);
static UINT64 stub_NtQuerySystemInformation(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4);
static UINT64 stub_NtQueryInformationProcess(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4);
static UINT64 stub_NtTerminateProcess(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4);
static UINT64 stub_NtYieldExecution(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4);
static UINT64 stub_NtDelayExecution(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4);
static UINT64 stub_NtCreateFile(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4);
static UINT64 stub_NtReadFile(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4);
static UINT64 stub_NtWriteFile(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4);
static UINT64 stub_RtlInitUnicodeString(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4);
static UINT64 stub_RtlAnsiStringToUnicodeString(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4);
static UINT64 stub_RtlFreeUnicodeString(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4);
static UINT64 stub_RtlAllocateHeap(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4);
static UINT64 stub_RtlFreeHeap(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4);
static UINT64 stub_RtlReAllocateHeap(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4);
static UINT64 stub_RtlZeroMemory(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4);
static UINT64 stub_RtlMoveMemory(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4);
static UINT64 stub_LdrLoadDll(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4);
static UINT64 stub_LdrGetProcedureAddress(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4);
static UINT64 stub_DbgPrint(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4);
static UINT64 stub_NtOpenKey(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4);
static UINT64 stub_NtQueryValueKey(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4);
static UINT64 stub_NtSetValueKey(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4);
static UINT64 stub_NtCreateKey(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4);

/* ---- stub implementations ---- */
static UINT64 stub_NtClose(UINT64 h, UINT64 a2, UINT64 a3, UINT64 a4) {
    return (UINT64)KiSystemCallDispatch(SYSCALL_NtClose, h, a2, a3, a4);
}
static UINT64 stub_NtAllocateVirtualMemory(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4) {
    return (UINT64)KiSystemCallDispatch(SYSCALL_NtAllocateVirtualMemory, a1, a2, a3, a4);
}
static UINT64 stub_NtFreeVirtualMemory(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4) {
    return (UINT64)KiSystemCallDispatch(SYSCALL_NtFreeVirtualMemory, a1, a2, a3, a4);
}
static UINT64 stub_NtQuerySystemInformation(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4) {
    return (UINT64)KiSystemCallDispatch(SYSCALL_NtQuerySystemInformation, a1, a2, a3, a4);
}
static UINT64 stub_NtQueryInformationProcess(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4) {
    return (UINT64)KiSystemCallDispatch(SYSCALL_NtQueryInformationProcess, a1, a2, a3, a4);
}
static UINT64 stub_NtTerminateProcess(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4) {
    return (UINT64)KiSystemCallDispatch(SYSCALL_NtTerminateProcess, a1, a2, a3, a4);
}
static UINT64 stub_NtYieldExecution(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4) {
    return (UINT64)KiSystemCallDispatch(SYSCALL_NtYieldExecution, a1, a2, a3, a4);
}
static UINT64 stub_NtDelayExecution(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4) {
    return (UINT64)KiSystemCallDispatch(SYSCALL_NtDelayExecution, a1, a2, a3, a4);
}
static UINT64 stub_NtCreateFile(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4) {
    return (UINT64)KiSystemCallDispatch(0x0055 /* NtCreateFile */, a1, a2, a3, a4);
}
static UINT64 stub_NtReadFile(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4) {
    return (UINT64)KiSystemCallDispatch(SYSCALL_NtReadFile, a1, a2, a3, a4);
}
static UINT64 stub_NtWriteFile(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4) {
    return (UINT64)KiSystemCallDispatch(SYSCALL_NtWriteFile, a1, a2, a3, a4);
}
static UINT64 stub_NtOpenKey(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4) {
    return (UINT64)KiSystemCallDispatch(SYSCALL_NtOpenKey, a1, a2, a3, a4);
}
static UINT64 stub_NtQueryValueKey(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4) {
    return (UINT64)KiSystemCallDispatch(SYSCALL_NtQueryValueKey, a1, a2, a3, a4);
}
static UINT64 stub_NtSetValueKey(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4) {
    return (UINT64)KiSystemCallDispatch(SYSCALL_NtSetValueKey, a1, a2, a3, a4);
}
static UINT64 stub_NtCreateKey(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4) {
    return (UINT64)KiSystemCallDispatch(SYSCALL_NtCreateKey, a1, a2, a3, a4);
}

/* RTL stubs — provided by kernel directly */
static UINT64 stub_RtlInitUnicodeString(UINT64 dest, UINT64 src,
                                         UINT64 a3, UINT64 a4) {
    (void)a3; (void)a4;
    UNICODE_STRING *us = (UNICODE_STRING *)(uintptr_t)dest;
    const WCHAR *s     = (const WCHAR *)(uintptr_t)src;
    if (!us) return 0;
    if (!s) { us->Length = us->MaximumLength = 0; us->Buffer = NULL; return 0; }
    USHORT len = 0;
    while (s[len]) len++;
    us->Buffer        = (WCHAR *)(uintptr_t)src;
    us->Length        = (USHORT)(len * sizeof(WCHAR));
    us->MaximumLength = (USHORT)((len + 1) * sizeof(WCHAR));
    return 0;
}

static UINT64 stub_RtlAllocateHeap(UINT64 heap, UINT64 flags, UINT64 size,
                                    UINT64 a4) {
    (void)heap; (void)flags; (void)a4;
    return (UINT64)(uintptr_t)kmalloc((size_t)size);
}
static UINT64 stub_RtlFreeHeap(UINT64 heap, UINT64 flags, UINT64 ptr,
                                 UINT64 a4) {
    (void)heap; (void)flags; (void)a4;
    kfree((void *)(uintptr_t)ptr);
    return 1;
}
static UINT64 stub_RtlReAllocateHeap(UINT64 heap, UINT64 flags, UINT64 ptr,
                                      UINT64 size) {
    (void)heap; (void)flags;
    /* Simple: alloc new + copy + free old */
    void *old = (void *)(uintptr_t)ptr;
    void *nw  = kmalloc((size_t)size);
    if (nw && old) {
        __builtin_memcpy(nw, old, (size_t)size);
        kfree(old);
    }
    return (UINT64)(uintptr_t)nw;
}
static UINT64 stub_RtlZeroMemory(UINT64 dst, UINT64 len, UINT64 a3, UINT64 a4) {
    (void)a3; (void)a4;
    if (dst && len) __builtin_memset((void *)(uintptr_t)dst, 0, (size_t)len);
    return 0;
}
static UINT64 stub_RtlMoveMemory(UINT64 dst, UINT64 src, UINT64 len, UINT64 a4) {
    (void)a4;
    if (dst && src && len)
        __builtin_memmove((void *)(uintptr_t)dst, (void *)(uintptr_t)src, (size_t)len);
    return 0;
}
static UINT64 stub_RtlAnsiStringToUnicodeString(UINT64 a1, UINT64 a2,
                                                  UINT64 a3, UINT64 a4) {
    (void)a1; (void)a2; (void)a3; (void)a4;
    return 0; /* STATUS_SUCCESS */
}
static UINT64 stub_RtlFreeUnicodeString(UINT64 a1, UINT64 a2,
                                         UINT64 a3, UINT64 a4) {
    (void)a1; (void)a2; (void)a3; (void)a4;
    return 0;
}
static UINT64 stub_LdrLoadDll(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4) {
    (void)a1; (void)a2; (void)a3; (void)a4;
    return (UINT64)STATUS_NOT_IMPLEMENTED;
}
static UINT64 stub_LdrGetProcedureAddress(UINT64 a1, UINT64 a2,
                                           UINT64 a3, UINT64 a4) {
    (void)a1; (void)a2; (void)a3; (void)a4;
    return 0;
}
static UINT64 stub_DbgPrint(UINT64 fmt, UINT64 a2, UINT64 a3, UINT64 a4) {
    (void)a2; (void)a3; (void)a4;
    if (fmt) kprintf("[DbgPrint] %s", (const char *)(uintptr_t)fmt);
    return 0;
}

/* -----------------------------------------------------------------------
 * Built-in stub DLL export tables
 * ----------------------------------------------------------------------- */
#define EXPORT(name) { #name, (UINT64)(uintptr_t)stub_##name }

static const STUB_EXPORT ntdll_exports[] = {
    EXPORT(NtClose),
    EXPORT(NtAllocateVirtualMemory),
    EXPORT(NtFreeVirtualMemory),
    EXPORT(NtQuerySystemInformation),
    EXPORT(NtQueryInformationProcess),
    EXPORT(NtTerminateProcess),
    EXPORT(NtYieldExecution),
    EXPORT(NtDelayExecution),
    EXPORT(NtCreateFile),
    EXPORT(NtReadFile),
    EXPORT(NtWriteFile),
    EXPORT(NtOpenKey),
    EXPORT(NtQueryValueKey),
    EXPORT(NtSetValueKey),
    EXPORT(NtCreateKey),
    EXPORT(RtlInitUnicodeString),
    EXPORT(RtlAnsiStringToUnicodeString),
    EXPORT(RtlFreeUnicodeString),
    EXPORT(RtlAllocateHeap),
    EXPORT(RtlFreeHeap),
    EXPORT(RtlReAllocateHeap),
    EXPORT(RtlZeroMemory),
    EXPORT(RtlMoveMemory),
    EXPORT(LdrLoadDll),
    EXPORT(LdrGetProcedureAddress),
    EXPORT(DbgPrint),
};
#define NTDLL_EXPORT_COUNT  (sizeof(ntdll_exports) / sizeof(ntdll_exports[0]))

/* kernel32.dll — Win32 API wrappers.  All forward to ntdll equivalents. */
static const STUB_EXPORT kernel32_exports[] = {
    { "CloseHandle",          (UINT64)(uintptr_t)stub_NtClose                 },
    { "VirtualAlloc",         (UINT64)(uintptr_t)stub_NtAllocateVirtualMemory },
    { "VirtualFree",          (UINT64)(uintptr_t)stub_NtFreeVirtualMemory     },
    { "GetProcessHeap",       (UINT64)(uintptr_t)stub_RtlAllocateHeap         },
    { "HeapAlloc",            (UINT64)(uintptr_t)stub_RtlAllocateHeap         },
    { "HeapFree",             (UINT64)(uintptr_t)stub_RtlFreeHeap             },
    { "HeapReAlloc",          (UINT64)(uintptr_t)stub_RtlReAllocateHeap       },
    { "ExitProcess",          (UINT64)(uintptr_t)stub_NtTerminateProcess      },
    { "TerminateProcess",     (UINT64)(uintptr_t)stub_NtTerminateProcess      },
    { "SleepEx",              (UINT64)(uintptr_t)stub_NtDelayExecution        },
    { "Sleep",                (UINT64)(uintptr_t)stub_NtDelayExecution        },
    { "OutputDebugStringA",   (UINT64)(uintptr_t)stub_DbgPrint                },
    { "GetLastError",         (UINT64)(uintptr_t)stub_NtYieldExecution        },
    { "SetLastError",         (UINT64)(uintptr_t)stub_NtYieldExecution        },
    { "LoadLibraryA",         (UINT64)(uintptr_t)stub_LdrLoadDll              },
    { "GetProcAddress",       (UINT64)(uintptr_t)stub_LdrGetProcedureAddress  },
    { "RtlMoveMemory",        (UINT64)(uintptr_t)stub_RtlMoveMemory           },
    { "ZeroMemory",           (UINT64)(uintptr_t)stub_RtlZeroMemory           },
    { "RtlZeroMemory",        (UINT64)(uintptr_t)stub_RtlZeroMemory           },
    { "CreateFileA",          (UINT64)(uintptr_t)stub_NtCreateFile            },
    { "ReadFile",             (UINT64)(uintptr_t)stub_NtReadFile              },
    { "WriteFile",            (UINT64)(uintptr_t)stub_NtWriteFile             },
    { "RegOpenKeyExA",        (UINT64)(uintptr_t)stub_NtOpenKey               },
    { "RegQueryValueExA",     (UINT64)(uintptr_t)stub_NtQueryValueKey         },
    { "RegSetValueExA",       (UINT64)(uintptr_t)stub_NtSetValueKey           },
    { "RegCreateKeyExA",      (UINT64)(uintptr_t)stub_NtCreateKey             },
};
#define KERNEL32_EXPORT_COUNT (sizeof(kernel32_exports)/sizeof(kernel32_exports[0]))

/* msvcrt.dll — C runtime stubs */
static const STUB_EXPORT msvcrt_exports[] = {
    { "malloc",   (UINT64)(uintptr_t)stub_RtlAllocateHeap },
    { "free",     (UINT64)(uintptr_t)stub_RtlFreeHeap     },
    { "realloc",  (UINT64)(uintptr_t)stub_RtlReAllocateHeap },
    { "memset",   (UINT64)(uintptr_t)stub_RtlZeroMemory   },
    { "memcpy",   (UINT64)(uintptr_t)stub_RtlMoveMemory   },
    { "memmove",  (UINT64)(uintptr_t)stub_RtlMoveMemory   },
    { "printf",   (UINT64)(uintptr_t)stub_DbgPrint        },
    { "fprintf",  (UINT64)(uintptr_t)stub_DbgPrint        },
    { "exit",     (UINT64)(uintptr_t)stub_NtTerminateProcess },
    { "_exit",    (UINT64)(uintptr_t)stub_NtTerminateProcess },
};
#define MSVCRT_EXPORT_COUNT (sizeof(msvcrt_exports)/sizeof(msvcrt_exports[0]))

/* user32.dll — minimal GUI stubs */
static const STUB_EXPORT user32_exports[] = {
    { "MessageBoxA",         (UINT64)(uintptr_t)stub_DbgPrint },
    { "GetSystemMetrics",    (UINT64)(uintptr_t)stub_NtYieldExecution },
};
#define USER32_EXPORT_COUNT (sizeof(user32_exports)/sizeof(user32_exports[0]))

/* Global stub DLL registry */
static STUB_DLL stub_dlls[] = {
    { "ntdll.dll",   ntdll_exports,   NTDLL_EXPORT_COUNT,   0 },
    { "ntdll",       ntdll_exports,   NTDLL_EXPORT_COUNT,   0 },
    { "kernel32.dll",kernel32_exports,KERNEL32_EXPORT_COUNT, 0 },
    { "kernel32",    kernel32_exports,KERNEL32_EXPORT_COUNT, 0 },
    { "kernelbase.dll",kernel32_exports,KERNEL32_EXPORT_COUNT,0 },
    { "msvcrt.dll",  msvcrt_exports,  MSVCRT_EXPORT_COUNT,  0 },
    { "msvcrt",      msvcrt_exports,  MSVCRT_EXPORT_COUNT,  0 },
    { "vcruntime140.dll", msvcrt_exports, MSVCRT_EXPORT_COUNT, 0 },
    { "ucrtbase.dll",    msvcrt_exports, MSVCRT_EXPORT_COUNT, 0 },
    { "user32.dll",  user32_exports,  USER32_EXPORT_COUNT,  0 },
    { "user32",      user32_exports,  USER32_EXPORT_COUNT,  0 },
    { "advapi32.dll",kernel32_exports,KERNEL32_EXPORT_COUNT, 0 },
    { "gdi32.dll",   user32_exports,  USER32_EXPORT_COUNT,  0 },
};
#define STUB_DLL_COUNT (sizeof(stub_dlls)/sizeof(stub_dlls[0]))

/* -----------------------------------------------------------------------
 * LdrInitialize
 * ----------------------------------------------------------------------- */
void LdrInitialize(void)
{
    kprintf("[LDR] PE32+ loader initialized\n");
    kprintf("[LDR] Built-in stubs: ntdll(%u) kernel32(%u) msvcrt(%u) user32(%u)\n",
            (UINT32)NTDLL_EXPORT_COUNT, (UINT32)KERNEL32_EXPORT_COUNT,
            (UINT32)MSVCRT_EXPORT_COUNT, (UINT32)USER32_EXPORT_COUNT);
}

/* -----------------------------------------------------------------------
 * ldr_find_stub_export — look up a function in the stub DLL tables
 * ----------------------------------------------------------------------- */
static UINT64 ldr_find_stub_export(const char *dll_name, const char *func_name)
{
    for (UINT32 d = 0; d < STUB_DLL_COUNT; d++) {
        if (ascii_icmp(stub_dlls[d].Name, dll_name) == 0) {
            for (UINT32 i = 0; i < stub_dlls[d].ExportCount; i++) {
                if (ascii_icmp(stub_dlls[d].Exports[i].Name, func_name) == 0)
                    return stub_dlls[d].Exports[i].StubAddress;
            }
            /* DLL found but function not — return a no-op stub */
            kprintf("[LDR] WARNING: %s!%s not found in stubs\n", dll_name, func_name);
            return (UINT64)(uintptr_t)stub_NtYieldExecution;
        }
    }
    kprintf("[LDR] WARNING: DLL '%s' not in stub table\n", dll_name);
    return (UINT64)(uintptr_t)stub_NtYieldExecution;
}

/* -----------------------------------------------------------------------
 * LdrApplyRelocations
 * ----------------------------------------------------------------------- */
NTSTATUS LdrApplyRelocations(UINT64 ImageBase, UINT64 PrefBase,
                               UINT32 RelocRVA, UINT32 RelocSize)
{
    if (!RelocRVA || !RelocSize) return STATUS_SUCCESS;

    INT64 delta = (INT64)(ImageBase - PrefBase);
    if (!delta) return STATUS_SUCCESS;

    UINT8 *reloc_dir = (UINT8 *)(uintptr_t)(ImageBase + RelocRVA);
    UINT8 *reloc_end = reloc_dir + RelocSize;

    while (reloc_dir < reloc_end) {
        PIMAGE_BASE_RELOCATION block = (PIMAGE_BASE_RELOCATION)(uintptr_t)reloc_dir;
        if (block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION)) break;
        if (block->SizeOfBlock == 0) break;

        UINT32 entry_count = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION))
                             / sizeof(UINT16);
        UINT16 *entries    = (UINT16 *)((UINT8 *)block + sizeof(IMAGE_BASE_RELOCATION));

        for (UINT32 i = 0; i < entry_count; i++) {
            UINT16 e    = entries[i];
            UINT8  type = (UINT8)(e >> 12);
            UINT16 off  = e & 0x0FFF;

            if (type == IMAGE_REL_BASED_ABSOLUTE) continue;
            if (type == IMAGE_REL_BASED_DIR64) {
                UINT64 *target = (UINT64 *)(uintptr_t)
                    (ImageBase + block->VirtualAddress + off);
                *target += (UINT64)delta;
            }
        }

        reloc_dir += block->SizeOfBlock;
    }

    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * ldr_resolve_imports — process the import directory
 * ----------------------------------------------------------------------- */
static NTSTATUS ldr_resolve_imports(UINT64 load_base, UINT64 pref_base,
                                     UINT32 import_rva, UINT32 import_size)
{
    (void)pref_base; (void)import_size;
    if (!import_rva) return STATUS_SUCCESS;

    PIMAGE_IMPORT_DESCRIPTOR desc =
        (PIMAGE_IMPORT_DESCRIPTOR)(uintptr_t)(load_base + import_rva);

    while (desc->Name) {
        const char *dll_name = (const char *)(uintptr_t)(load_base + desc->Name);
        kprintf("[LDR] Resolving imports from %s\n", dll_name);

        /* ILT: OriginalFirstThunk (if non-zero); IAT: FirstThunk */
        UINT64 *ilt_ptr = desc->OriginalFirstThunk
            ? (UINT64 *)(uintptr_t)(load_base + desc->OriginalFirstThunk)
            : (UINT64 *)(uintptr_t)(load_base + desc->FirstThunk);
        UINT64 *iat_ptr = (UINT64 *)(uintptr_t)(load_base + desc->FirstThunk);

        while (*ilt_ptr) {
            UINT64 thunk = *ilt_ptr;
            UINT64 resolved;

            if (thunk & (UINT64_C(1) << 63)) {
                /* Import by ordinal */
                UINT64 ordinal = thunk & 0xFFFF;
                kprintf("[LDR]   ordinal #%llu\n", (unsigned long long)ordinal);
                /* Phase 3: ordinal imports return no-op stub */
                resolved = (UINT64)(uintptr_t)stub_NtYieldExecution;
            } else {
                /* Import by name */
                PIMAGE_IMPORT_BY_NAME ibn =
                    (PIMAGE_IMPORT_BY_NAME)(uintptr_t)(load_base + (UINT32)thunk);
                const char *func_name = (const char *)ibn->Name;
                resolved = ldr_find_stub_export(dll_name, func_name);
            }

            *iat_ptr = resolved;
            ilt_ptr++;
            iat_ptr++;
        }

        desc++;
    }

    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * ldr_section_chars_to_prot — PE section characteristics → PAGE_*
 * ----------------------------------------------------------------------- */
static UINT32 ldr_section_chars_to_prot(UINT32 chars)
{
    bool exec  = (chars & IMAGE_SCN_MEM_EXECUTE)  != 0;
    bool write = (chars & IMAGE_SCN_MEM_WRITE)    != 0;
    bool read  = (chars & IMAGE_SCN_MEM_READ)     != 0;
    (void)read;

    if (exec && write) return PAGE_EXECUTE_READWRITE;
    if (exec)          return PAGE_EXECUTE_READ;
    if (write)         return PAGE_READWRITE;
    return PAGE_READONLY;
}

/* -----------------------------------------------------------------------
 * LdrLoadImage
 * ----------------------------------------------------------------------- */
NTSTATUS LdrLoadImage(void *PeBuffer, UINT64 PeSize,
                       PEPROCESS Process, PLOAD_IMAGE_RESULT Result)
{
    if (!PeBuffer || PeSize < sizeof(IMAGE_DOS_HEADER))
        return STATUS_INVALID_IMAGE_FORMAT;

    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)PeBuffer;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return STATUS_INVALID_IMAGE_FORMAT;

    if ((UINT32)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > PeSize)
        return STATUS_INVALID_IMAGE_FORMAT;

    PIMAGE_NT_HEADERS64 nth = (PIMAGE_NT_HEADERS64)
        ((UINT8 *)PeBuffer + dos->e_lfanew);
    if (nth->Signature != IMAGE_NT_SIGNATURE) return STATUS_INVALID_IMAGE_FORMAT;
    if (nth->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64)
        return STATUS_IMAGE_MACHINE_TYPE_MISMATCH;
    if (nth->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return STATUS_INVALID_IMAGE_FORMAT;

    PIMAGE_OPTIONAL_HEADER64 opt = &nth->OptionalHeader;

    /* Create an image section */
    PSECTION_OBJECT section = NULL;
    NTSTATUS s = MmCreateImageSection(PeBuffer, PeSize, &section);
    if (!NT_SUCCESS(s)) return s;

    /* Map the image into the process's VA space */
    UINT64 load_base = 0;
    s = MmMapImageView(section, &Process->VmaSpace, &load_base);
    if (!NT_SUCCESS(s)) { ObDereferenceObject(section); return s; }

    kprintf("[LDR] Image mapped at 0x%llx (preferred 0x%llx)\n",
            (unsigned long long)load_base,
            (unsigned long long)opt->ImageBase);

    /* Apply base relocations if needed */
    IMAGE_DATA_DIRECTORY *reloc_dir =
        &opt->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (load_base != opt->ImageBase) {
        s = LdrApplyRelocations(load_base, opt->ImageBase,
                                reloc_dir->VirtualAddress, reloc_dir->Size);
        if (!NT_SUCCESS(s)) { ObDereferenceObject(section); return s; }
        kprintf("[LDR] Base relocations applied (delta=0x%llx)\n",
                (unsigned long long)(load_base - opt->ImageBase));
    }

    /* Resolve imports */
    IMAGE_DATA_DIRECTORY *imp_dir =
        &opt->DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    s = ldr_resolve_imports(load_base, opt->ImageBase,
                             imp_dir->VirtualAddress, imp_dir->Size);
    if (!NT_SUCCESS(s)) { ObDereferenceObject(section); return s; }

    /* Apply per-section page protections */
    PIMAGE_SECTION_HEADER shdrs = IMAGE_FIRST_SECTION(nth);
    for (UINT16 i = 0; i < nth->FileHeader.NumberOfSections; i++) {
        UINT64 sec_va   = load_base + shdrs[i].VirtualAddress;
        UINT64 sec_size = shdrs[i].VirtualSize;
        UINT32 prot     = ldr_section_chars_to_prot(shdrs[i].Characteristics);
        UINT32 old_prot = 0;
        if (sec_size) {
            sec_size = (sec_size + PAGE_SIZE - 1) & ~((UINT64)PAGE_SIZE - 1);
            VmaProtect(&Process->VmaSpace, &sec_va, &sec_size, prot, &old_prot);
        }
    }

    /* Allocate user-mode stack */
    UINT64 stack_size    = 2 * 1024 * 1024;  /* 2 MB */
    UINT64 stack_base    = 0;
    UINT64 stack_size_out = stack_size;
    s = VmaAllocate(&Process->VmaSpace, &stack_base, &stack_size_out,
                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!NT_SUCCESS(s)) {
        kprintf("[LDR] Failed to allocate user stack: 0x%x\n", (UINT32)s);
        ObDereferenceObject(section);
        return s;
    }

    kprintf("[LDR] User stack: base=0x%llx size=%llu KB\n",
            (unsigned long long)stack_base,
            (unsigned long long)(stack_size / 1024));

    /* Entry point */
    UINT64 entry_point = load_base + opt->AddressOfEntryPoint;

    /* Fill in result */
    Result->ImageBase  = load_base;
    Result->EntryPoint = entry_point;
    Result->StackBase  = stack_base;
    Result->StackSize  = stack_size_out;

    kprintf("[LDR] Entry point: 0x%llx\n", (unsigned long long)entry_point);

    ObDereferenceObject(section);
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * LdrGetProcAddress — look up an export from the loaded module list
 * ----------------------------------------------------------------------- */
UINT64 LdrGetProcAddress(PLDR_MODULE ModuleList,
                          const char *DllName, const char *FuncName)
{
    /* First try the loaded module list */
    for (PLDR_MODULE m = ModuleList; m; m = m->Next) {
        if (ascii_icmp(m->BaseName, DllName) != 0) continue;

        /* Scan the export directory at m->ImageBase */
        UINT8 *base = (UINT8 *)(uintptr_t)m->ImageBase;
        PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)(uintptr_t)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) break;

        PIMAGE_NT_HEADERS64 nth = (PIMAGE_NT_HEADERS64)(base + dos->e_lfanew);
        if (nth->Signature != IMAGE_NT_SIGNATURE) break;

        IMAGE_DATA_DIRECTORY *exp_dir =
            &nth->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!exp_dir->VirtualAddress) break;

        PIMAGE_EXPORT_DIRECTORY exp =
            (PIMAGE_EXPORT_DIRECTORY)(uintptr_t)(m->ImageBase + exp_dir->VirtualAddress);
        UINT32 *names = (UINT32 *)(uintptr_t)(m->ImageBase + exp->AddressOfNames);
        UINT16 *ords  = (UINT16 *)(uintptr_t)(m->ImageBase + exp->AddressOfNameOrdinals);
        UINT32 *funcs = (UINT32 *)(uintptr_t)(m->ImageBase + exp->AddressOfFunctions);

        for (UINT32 i = 0; i < exp->NumberOfNames; i++) {
            const char *name = (const char *)(uintptr_t)(m->ImageBase + names[i]);
            if (ascii_icmp(name, FuncName) == 0) {
                UINT32 ord = ords[i];
                return m->ImageBase + funcs[ord];
            }
        }
    }

    /* Fall back to stub table */
    return ldr_find_stub_export(DllName, FuncName);
}
