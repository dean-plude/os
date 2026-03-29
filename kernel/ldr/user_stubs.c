/*
 * user_stubs.c — User-mode SYSCALL thunk page generator (Phase 6)
 *
 * Each of the four stub DLL pages contains 16-byte SYSCALL thunks:
 *
 *   Offset 0x000  Function[0]:  4C 8B D1  B8 xx xx xx xx  0F 05  C3  90×5
 *   Offset 0x010  Function[1]:  ...
 *   ...
 *
 * The machine code encoding:
 *   4C 8B D1           = mov r10, rcx   (Windows ABI: first arg → r10)
 *   B8 lo hi 00 00     = mov eax, N     (syscall number, 32-bit zero-extended)
 *   0F 05              = syscall
 *   C3                 = ret
 *   90 90 90 90 90     = nop × 5 (pad to 16 bytes)
 *
 * The pages are mapped MAP_USER (read+exec, no write) into every new process
 * via LdrMapUserStubPages(), called from PsCreateUserProcess().
 */

#include "user_stubs.h"
#include "../mm/pmm.h"
#include "../arch/x86_64/paging.h"
#include "../ke/printf.h"
#include "../ke/syscall.h"
#include "../include/types.h"

/* -----------------------------------------------------------------------
 * Stub function descriptor: (name → syscall_number)
 * ----------------------------------------------------------------------- */
typedef struct {
    const char *Name;
    UINT32      SyscallNum;
} STUB_FUNC_DESC;

/* -----------------------------------------------------------------------
 * ntdll.dll stub table
 * ----------------------------------------------------------------------- */
static const STUB_FUNC_DESC s_ntdll[] = {
    /* NT syscalls — Windows 10 1903 numbers */
    { "NtClose",                    SYSCALL_NtClose                   },
    { "ZwClose",                    SYSCALL_NtClose                   },
    { "NtAllocateVirtualMemory",    SYSCALL_NtAllocateVirtualMemory   },
    { "ZwAllocateVirtualMemory",    SYSCALL_NtAllocateVirtualMemory   },
    { "NtFreeVirtualMemory",        SYSCALL_NtFreeVirtualMemory       },
    { "ZwFreeVirtualMemory",        SYSCALL_NtFreeVirtualMemory       },
    { "NtQueryVirtualMemory",       SYSCALL_NtQueryVirtualMemory      },
    { "NtProtectVirtualMemory",     SYSCALL_NtProtectVirtualMemory    },
    { "NtQuerySystemInformation",   SYSCALL_NtQuerySystemInformation  },
    { "NtQueryInformationProcess",  SYSCALL_NtQueryInformationProcess },
    { "NtQueryInformationThread",   SYSCALL_NtQueryInformationThread  },
    { "NtSetInformationThread",     SYSCALL_NtSetInformationThread    },
    { "NtTerminateProcess",         SYSCALL_NtTerminateProcess        },
    { "ZwTerminateProcess",         SYSCALL_NtTerminateProcess        },
    { "NtYieldExecution",           SYSCALL_NtYieldExecution          },
    { "NtDelayExecution",           SYSCALL_NtDelayExecution          },
    { "NtCreateFile",               SYSCALL_NtCreateFile              },
    { "ZwCreateFile",               SYSCALL_NtCreateFile              },
    { "NtReadFile",                 SYSCALL_NtReadFile                },
    { "NtWriteFile",                SYSCALL_NtWriteFile               },
    { "NtQueryInformationFile",     SYSCALL_NtQueryInformationFile    },
    { "NtOpenKey",                  SYSCALL_NtOpenKey                 },
    { "ZwOpenKey",                  SYSCALL_NtOpenKey                 },
    { "NtQueryValueKey",            SYSCALL_NtQueryValueKey           },
    { "NtSetValueKey",              SYSCALL_NtSetValueKey             },
    { "NtCreateKey",                SYSCALL_NtCreateKey               },
    { "ZwCreateKey",                SYSCALL_NtCreateKey               },
    { "NtQuerySystemTime",          SYSCALL_NtQuerySystemTime         },
    { "NtQueryPerformanceCounter",  SYSCALL_NtQueryPerformanceCounter },
    { "NtCreateSection",            SYSCALL_NtCreateSection           },
    { "NtMapViewOfSection",         SYSCALL_NtMapViewOfSection        },
    { "NtUnmapViewOfSection",       SYSCALL_NtUnmapViewOfSection      },
    { "NtOpenProcess",              SYSCALL_NtOpenProcess             },
    { "NtCreateProcessEx",          SYSCALL_NtCreateProcessEx         },
    { "NtCreateThread",             SYSCALL_NtCreateThread            },
    { "NtFlushInstructionCache",    SYSCALL_NtFlushInstructionCache   },
    { "NtSetInformationProcess",    SYSCALL_NtYieldExecution          },
    /* RTL helpers routed through kernel-helper numbers */
    { "RtlAllocateHeap",            KH_RtlAllocateHeap                },
    { "RtlFreeHeap",                KH_RtlFreeHeap                    },
    { "RtlReAllocateHeap",          KH_RtlReAllocateHeap              },
    { "RtlZeroMemory",              KH_RtlZeroMemory                  },
    { "RtlFillMemory",              KH_RtlZeroMemory                  },
    { "RtlMoveMemory",              KH_RtlMoveMemory                  },
    { "RtlCopyMemory",              KH_RtlMoveMemory                  },
    { "RtlInitUnicodeString",       KH_RtlInitUnicodeString           },
    { "RtlAnsiStringToUnicodeString", SYSCALL_NtYieldExecution        },
    { "RtlFreeUnicodeString",       SYSCALL_NtYieldExecution          },
    { "RtlUnicodeStringToAnsiString", SYSCALL_NtYieldExecution        },
    { "DbgPrint",                   KH_DbgPrint                       },
    { "DbgPrintEx",                 KH_DbgPrint                       },
    { "LdrLoadDll",                 SYSCALL_NtYieldExecution          },
    { "LdrGetProcedureAddress",     SYSCALL_NtYieldExecution          },
    { "RtlGetVersion",              SYSCALL_NtYieldExecution          },
    { "RtlCreateHeap",              KH_GetProcessHeap                 },
    /* Terminator / not-implemented fallback (must be last) */
    { "_NtNovaNotImpl",             SYSCALL_NtYieldExecution          },
};

/* -----------------------------------------------------------------------
 * kernel32.dll stub table
 * ----------------------------------------------------------------------- */
static const STUB_FUNC_DESC s_kernel32[] = {
    { "CloseHandle",                    SYSCALL_NtClose                   },
    { "DuplicateHandle",                SYSCALL_NtYieldExecution          },
    { "VirtualAlloc",                   SYSCALL_NtAllocateVirtualMemory   },
    { "VirtualAllocEx",                 SYSCALL_NtAllocateVirtualMemory   },
    { "VirtualFree",                    SYSCALL_NtFreeVirtualMemory       },
    { "VirtualFreeEx",                  SYSCALL_NtFreeVirtualMemory       },
    { "VirtualProtect",                 SYSCALL_NtProtectVirtualMemory    },
    { "VirtualProtectEx",               SYSCALL_NtProtectVirtualMemory    },
    { "VirtualQuery",                   SYSCALL_NtQueryVirtualMemory      },
    { "VirtualQueryEx",                 SYSCALL_NtQueryVirtualMemory      },
    { "GetProcessHeap",                 KH_GetProcessHeap                 },
    { "HeapCreate",                     KH_GetProcessHeap                 },
    { "HeapAlloc",                      KH_RtlAllocateHeap                },
    { "HeapFree",                       KH_RtlFreeHeap                    },
    { "HeapReAlloc",                    KH_RtlReAllocateHeap              },
    { "HeapSize",                       SYSCALL_NtYieldExecution          },
    { "HeapDestroy",                    SYSCALL_NtYieldExecution          },
    { "LocalAlloc",                     KH_RtlAllocateHeap                },
    { "LocalFree",                      KH_RtlFreeHeap                    },
    { "GlobalAlloc",                    KH_RtlAllocateHeap                },
    { "GlobalFree",                     KH_RtlFreeHeap                    },
    { "ExitProcess",                    SYSCALL_NtTerminateProcess        },
    { "TerminateProcess",               SYSCALL_NtTerminateProcess        },
    { "ExitThread",                     SYSCALL_NtTerminateProcess        },
    { "Sleep",                          SYSCALL_NtDelayExecution          },
    { "SleepEx",                        SYSCALL_NtDelayExecution          },
    { "GetLastError",                   KH_GetLastError                   },
    { "SetLastError",                   KH_SetLastError                   },
    { "GetCurrentProcess",              KH_GetCurrentProcess              },
    { "GetCurrentThread",               KH_GetCurrentThread               },
    { "GetCurrentProcessId",            KH_GetCurrentProcessId            },
    { "GetCurrentThreadId",             KH_GetCurrentThreadId             },
    { "IsDebuggerPresent",              KH_IsDebuggerPresent              },
    { "CheckRemoteDebuggerPresent",     KH_IsDebuggerPresent              },
    { "OutputDebugStringA",             KH_DbgPrint                       },
    { "OutputDebugStringW",             KH_DbgPrint                       },
    { "GetStdHandle",                   KH_GetStdHandle                   },
    { "CreateFileA",                    SYSCALL_NtCreateFile              },
    { "CreateFileW",                    SYSCALL_NtCreateFile              },
    { "ReadFile",                       SYSCALL_NtReadFile                },
    { "ReadFileEx",                     SYSCALL_NtReadFile                },
    { "WriteFile",                      SYSCALL_NtWriteFile               },
    { "WriteFileEx",                    SYSCALL_NtWriteFile               },
    { "FlushInstructionCache",          SYSCALL_NtFlushInstructionCache   },
    { "LoadLibraryA",                   SYSCALL_NtYieldExecution          },
    { "LoadLibraryW",                   SYSCALL_NtYieldExecution          },
    { "LoadLibraryExA",                 SYSCALL_NtYieldExecution          },
    { "LoadLibraryExW",                 SYSCALL_NtYieldExecution          },
    { "GetProcAddress",                 SYSCALL_NtYieldExecution          },
    { "FreeLibrary",                    SYSCALL_NtYieldExecution          },
    { "RtlMoveMemory",                  KH_RtlMoveMemory                  },
    { "RtlCopyMemory",                  KH_RtlMoveMemory                  },
    { "RtlZeroMemory",                  KH_RtlZeroMemory                  },
    { "ZeroMemory",                     KH_RtlZeroMemory                  },
    { "FillMemory",                     KH_RtlZeroMemory                  },
    { "RegOpenKeyExA",                  SYSCALL_NtOpenKey                 },
    { "RegOpenKeyExW",                  SYSCALL_NtOpenKey                 },
    { "RegQueryValueExA",               SYSCALL_NtQueryValueKey           },
    { "RegQueryValueExW",               SYSCALL_NtQueryValueKey           },
    { "RegSetValueExA",                 SYSCALL_NtSetValueKey             },
    { "RegSetValueExW",                 SYSCALL_NtSetValueKey             },
    { "RegCreateKeyExA",                SYSCALL_NtCreateKey               },
    { "RegCreateKeyExW",                SYSCALL_NtCreateKey               },
    { "RegCloseKey",                    SYSCALL_NtClose                   },
    { "QueryPerformanceCounter",        SYSCALL_NtQueryPerformanceCounter },
    { "QueryPerformanceFrequency",      SYSCALL_NtQueryPerformanceCounter },
    { "GetSystemTimeAsFileTime",        SYSCALL_NtQuerySystemTime         },
    { "GetTickCount",                   SYSCALL_NtQuerySystemTime         },
    { "GetTickCount64",                 SYSCALL_NtQuerySystemTime         },
    { "MultiByteToWideChar",            SYSCALL_NtYieldExecution          },
    { "WideCharToMultiByte",            SYSCALL_NtYieldExecution          },
    { "GetSystemInfo",                  SYSCALL_NtQuerySystemInformation  },
    { "SetUnhandledExceptionFilter",    SYSCALL_NtYieldExecution          },
    { "UnhandledExceptionFilter",       SYSCALL_NtYieldExecution          },
    { "RaiseException",                 SYSCALL_NtTerminateProcess        },
    { "InitializeCriticalSection",      SYSCALL_NtYieldExecution          },
    { "DeleteCriticalSection",          SYSCALL_NtYieldExecution          },
    { "EnterCriticalSection",           SYSCALL_NtYieldExecution          },
    { "LeaveCriticalSection",           SYSCALL_NtYieldExecution          },
    { "TlsAlloc",                       SYSCALL_NtYieldExecution          },
    { "TlsFree",                        SYSCALL_NtYieldExecution          },
    { "TlsGetValue",                    SYSCALL_NtYieldExecution          },
    { "TlsSetValue",                    SYSCALL_NtYieldExecution          },
    { "_NtNovaNotImpl",                 SYSCALL_NtYieldExecution          },
};

/* -----------------------------------------------------------------------
 * msvcrt.dll stub table
 * ----------------------------------------------------------------------- */
static const STUB_FUNC_DESC s_msvcrt[] = {
    { "malloc",         KH_RtlAllocateHeap      },
    { "calloc",         KH_RtlAllocateHeap      },
    { "realloc",        KH_RtlReAllocateHeap    },
    { "free",           KH_RtlFreeHeap          },
    { "_malloc_base",   KH_RtlAllocateHeap      },
    { "_free_base",     KH_RtlFreeHeap          },
    { "memset",         KH_RtlZeroMemory        },
    { "memcpy",         KH_RtlMoveMemory        },
    { "memmove",        KH_RtlMoveMemory        },
    { "memcmp",         SYSCALL_NtYieldExecution },
    { "strlen",         SYSCALL_NtYieldExecution },
    { "strcmp",         SYSCALL_NtYieldExecution },
    { "strcpy",         SYSCALL_NtYieldExecution },
    { "strcat",         SYSCALL_NtYieldExecution },
    { "printf",         KH_DbgPrint             },
    { "fprintf",        KH_DbgPrint             },
    { "sprintf",        KH_DbgPrint             },
    { "exit",           SYSCALL_NtTerminateProcess },
    { "_exit",          SYSCALL_NtTerminateProcess },
    { "abort",          SYSCALL_NtTerminateProcess },
    { "_cexit",         SYSCALL_NtYieldExecution },
    { "__acrt_iob_func",SYSCALL_NtYieldExecution },
    { "__stdio_common_vfprintf", KH_DbgPrint    },
    { "__p___argc",     SYSCALL_NtYieldExecution },
    { "__p___argv",     SYSCALL_NtYieldExecution },
    { "_NtNovaNotImpl", SYSCALL_NtYieldExecution },
};

/* -----------------------------------------------------------------------
 * user32.dll stub table
 * ----------------------------------------------------------------------- */
static const STUB_FUNC_DESC s_user32[] = {
    { "MessageBoxA",            KH_DbgPrint             },
    { "MessageBoxW",            KH_DbgPrint             },
    { "GetSystemMetrics",       SYSCALL_NtYieldExecution },
    { "PostQuitMessage",        SYSCALL_NtTerminateProcess },
    { "DefWindowProcA",         SYSCALL_NtYieldExecution },
    { "DefWindowProcW",         SYSCALL_NtYieldExecution },
    { "RegisterClassExA",       SYSCALL_NtYieldExecution },
    { "RegisterClassExW",       SYSCALL_NtYieldExecution },
    { "CreateWindowExA",        SYSCALL_NtYieldExecution },
    { "CreateWindowExW",        SYSCALL_NtYieldExecution },
    { "ShowWindow",             SYSCALL_NtYieldExecution },
    { "UpdateWindow",           SYSCALL_NtYieldExecution },
    { "GetMessageA",            SYSCALL_NtDelayExecution },
    { "GetMessageW",            SYSCALL_NtDelayExecution },
    { "TranslateMessage",       SYSCALL_NtYieldExecution },
    { "DispatchMessageA",       SYSCALL_NtYieldExecution },
    { "DispatchMessageW",       SYSCALL_NtYieldExecution },
    { "LoadCursorA",            SYSCALL_NtYieldExecution },
    { "LoadCursorW",            SYSCALL_NtYieldExecution },
    { "_NtNovaNotImpl",         SYSCALL_NtYieldExecution },
};

/* -----------------------------------------------------------------------
 * Stub DLL table (canonical entries; aliases handled in LdrGetStubVA)
 * ----------------------------------------------------------------------- */
typedef struct {
    const char            *Name;
    UINT64                 UserVA;
    const STUB_FUNC_DESC  *Funcs;
    UINT32                 FuncCount;
    uintptr_t              PhysPage;   /* set during LdrInitUserStubs */
} USER_STUB_DLL;

#define ARRAY_SZ(a) ((UINT32)(sizeof(a)/sizeof((a)[0])))

static USER_STUB_DLL g_stub_dlls[] = {
    { "ntdll.dll",    USER_NTDLL_STUB_VA,    s_ntdll,   ARRAY_SZ(s_ntdll),   0 },
    { "kernel32.dll", USER_KERNEL32_STUB_VA, s_kernel32,ARRAY_SZ(s_kernel32),0 },
    { "msvcrt.dll",   USER_MSVCRT_STUB_VA,   s_msvcrt,  ARRAY_SZ(s_msvcrt),  0 },
    { "user32.dll",   USER_USER32_STUB_VA,   s_user32,  ARRAY_SZ(s_user32),  0 },
};
#define STUB_DLL_COUNT  ARRAY_SZ(g_stub_dlls)

/* -----------------------------------------------------------------------
 * emit_thunk — write one 16-byte SYSCALL thunk into a slot
 * ----------------------------------------------------------------------- */
static void emit_thunk(UINT8 *slot, UINT32 num)
{
    /* mov r10, rcx */
    slot[0] = 0x4C; slot[1] = 0x8B; slot[2] = 0xD1;
    /* mov eax, imm32 */
    slot[3] = 0xB8;
    slot[4] = (UINT8)( num        & 0xFF);
    slot[5] = (UINT8)((num >>  8) & 0xFF);
    slot[6] = (UINT8)((num >> 16) & 0xFF);
    slot[7] = (UINT8)((num >> 24) & 0xFF);
    /* syscall */
    slot[8] = 0x0F; slot[9] = 0x05;
    /* ret */
    slot[10] = 0xC3;
    /* nop × 5 (padding) */
    slot[11] = 0x90; slot[12] = 0x90; slot[13] = 0x90;
    slot[14] = 0x90; slot[15] = 0x90;
}

/* -----------------------------------------------------------------------
 * Case-insensitive ASCII compare
 * ----------------------------------------------------------------------- */
static int stub_icmp(const char *a, const char *b)
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
 * LdrInitUserStubs — allocate pages and emit thunks (called once at boot)
 * ----------------------------------------------------------------------- */
void LdrInitUserStubs(void)
{
    for (UINT32 d = 0; d < STUB_DLL_COUNT; d++) {
        USER_STUB_DLL *dll = &g_stub_dlls[d];

        if (dll->FuncCount > USER_STUB_MAX_SLOTS) {
            kprintf("[LDR] WARNING: %s has %u stubs, max %u — truncating\n",
                    dll->Name, dll->FuncCount, USER_STUB_MAX_SLOTS);
        }

        uintptr_t phys = pmm_alloc_page();
        if (!phys) {
            kprintf("[LDR] FATAL: no physical page for %s user stubs\n", dll->Name);
            continue;
        }
        dll->PhysPage = phys;

        /* Fill page with NOPs, then emit thunks via physmap window */
        UINT8 *page = (UINT8 *)(PHYSMAP_BASE + phys);
        __builtin_memset(page, 0x90, PAGE_SIZE);

        UINT32 count = dll->FuncCount < USER_STUB_MAX_SLOTS
                       ? dll->FuncCount : USER_STUB_MAX_SLOTS;
        for (UINT32 i = 0; i < count; i++)
            emit_thunk(page + i * USER_STUB_SLOT_SIZE, dll->Funcs[i].SyscallNum);

        kprintf("[LDR] User stubs: %-16s VA=0x%llx phys=0x%llx (%u stubs)\n",
                dll->Name,
                (unsigned long long)dll->UserVA,
                (unsigned long long)phys,
                count);
    }
}

/* -----------------------------------------------------------------------
 * LdrMapUserStubPages — map stub pages into a process page table
 * ----------------------------------------------------------------------- */
void LdrMapUserStubPages(uintptr_t pt_phys)
{
    if (!pt_phys) return;
    for (UINT32 d = 0; d < STUB_DLL_COUNT; d++) {
        USER_STUB_DLL *dll = &g_stub_dlls[d];
        if (!dll->PhysPage) continue;
        /* MAP_USER: user accessible, read-only, executable (no MAP_NO_EXEC) */
        NTSTATUS s = paging_map_in_pt(pt_phys, (uintptr_t)dll->UserVA,
                                       dll->PhysPage, MAP_USER);
        if (!NT_SUCCESS(s))
            kprintf("[LDR] WARNING: failed to map %s stubs: 0x%x\n",
                    dll->Name, (UINT32)s);
    }
}

/* -----------------------------------------------------------------------
 * dll_name_to_index — resolve DLL name (including common aliases) to table index
 * ----------------------------------------------------------------------- */
static UINT32 dll_name_to_index(const char *dll_name)
{
    /* Direct match */
    for (UINT32 d = 0; d < STUB_DLL_COUNT; d++) {
        if (stub_icmp(g_stub_dlls[d].Name, dll_name) == 0)
            return d;
    }

    /* Alias table: maps alternate names to canonical DLL index */
    static const struct { const char *alias; UINT32 idx; } aliases[] = {
        { "ntdll",            0 },
        { "ntdll.dll",        0 },
        { "kernel32",         1 },
        { "kernelbase.dll",   1 },
        { "kernelbase",       1 },
        { "advapi32.dll",     1 },
        { "advapi32",         1 },
        { "msvcrt",           2 },
        { "vcruntime140.dll", 2 },
        { "vcruntime140",     2 },
        { "ucrtbase.dll",     2 },
        { "ucrtbase",         2 },
        { "api-ms-win-crt-runtime-l1-1-0.dll", 2 },
        { "user32",           3 },
        { "gdi32.dll",        3 },
        { "gdi32",            3 },
    };
    for (UINT32 i = 0; i < ARRAY_SZ(aliases); i++) {
        if (stub_icmp(aliases[i].alias, dll_name) == 0)
            return aliases[i].idx;
    }
    return 0xFFFFFFFF;
}

/* -----------------------------------------------------------------------
 * LdrGetStubVA
 * ----------------------------------------------------------------------- */
UINT64 LdrGetStubVA(const char *dll_name, const char *func_name)
{
    UINT32 idx = dll_name_to_index(dll_name);
    if (idx == 0xFFFFFFFF) {
        kprintf("[LDR] WARNING: unknown DLL '%s' (stub)\n", dll_name);
        return LdrGetFallbackStubVA();
    }

    USER_STUB_DLL *dll = &g_stub_dlls[idx];
    if (!dll->PhysPage) return LdrGetFallbackStubVA();

    for (UINT32 i = 0; i < dll->FuncCount; i++) {
        if (stub_icmp(dll->Funcs[i].Name, func_name) == 0)
            return dll->UserVA + (UINT64)(i * USER_STUB_SLOT_SIZE);
    }

    kprintf("[LDR] WARNING: %s!%s not in stubs — using fallback\n",
            dll_name, func_name);
    return LdrGetFallbackStubVA();
}

/* -----------------------------------------------------------------------
 * LdrGetFallbackStubVA — NtYieldExecution in ntdll (harmless no-op)
 * ----------------------------------------------------------------------- */
UINT64 LdrGetFallbackStubVA(void)
{
    /* Slot for NtYieldExecution in ntdll (find it in the table) */
    USER_STUB_DLL *ntdll = &g_stub_dlls[0];
    if (!ntdll->PhysPage) return 0;
    for (UINT32 i = 0; i < ntdll->FuncCount; i++) {
        if (stub_icmp(ntdll->Funcs[i].Name, "NtYieldExecution") == 0)
            return ntdll->UserVA + (UINT64)(i * USER_STUB_SLOT_SIZE);
    }
    return ntdll->UserVA;  /* slot 0 as last resort */
}
