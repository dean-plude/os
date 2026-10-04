/*
 * ntdll.dll — NovaOS native API: system-call stubs, heap, runtime helpers
 *
 * Each Nt* export is Windows 10's x64 stub, byte for byte: mov r10, rcx;
 * mov eax, N; test byte [7FFE0308h], 1; jne +3; syscall; ret; int 2Eh; ret
 * (programs and sandboxes that read or copy the stubs expect exactly this;
 * KUSER_SHARED_DATA.SystemCall is 0, so syscall is the path taken).  There
 * is one for every Windows 10 1903 service, at 1903's number
 * (nt1903_services.h), and one for each of NovaOS's own (ke/syscall.h);
 * abitest and syscalltest check them.
 *
 * The heap (Rtl*Heap) serves one process-wide heap: small blocks come
 * from segregated free lists carved out of 1 MiB arenas committed from a
 * 1 GiB reservation; blocks larger than 256 KiB get their own
 * NtAllocateVirtualMemory region.  Programs are single-threaded, so the
 * heap takes no lock.
 */

#define NOVA_BUILD_NTDLL
#include <winternl.h>
#include "syscall_numbers.h"

/* -----------------------------------------------------------------------
 * System-call stubs, laid out as Windows lays out its own: one 32-byte stub
 * for every Windows 10 1903 service, in service-number order, each with an
 * entry in the exception directory (.pdata), then NovaOS's own services.
 * Programs that make system calls themselves (anti-cheat and sandbox code
 * such as Roblox's Hyperion) find a service's number from this layout
 * rather than by reading the stub: by ranking the Zw exports by address,
 * or by counting them in .pdata order.  A service the kernel lacks still
 * has its stub, so every later number ranks right; calling it returns
 * STATUS_INVALID_SYSTEM_SERVICE.
 * ----------------------------------------------------------------------- */
#ifdef _WIN64                 /* 32-bit programs: ntdll_wow.c */
#define STUB(name, num)                                                     \
    __asm__(".section .text$nt,\"xr\"\n"                                    \
            ".globl " #name "\n"                                            \
            ".p2align 4\n"                                                  \
            ".def " #name "; .scl 2; .type 32; .endef\n"                    \
            #name ":\n\t"                                                   \
            ".seh_proc " #name "\n\t"                                       \
            ".seh_endprologue\n\t"                                          \
            ".byte 0x4C, 0x8B, 0xD1\n\t"          /* mov r10, rcx */         \
            ".byte 0xB8\n\t.long " #num "\n\t"    /* mov eax, num */         \
            ".byte 0xF6, 0x04, 0x25, 0x08, 0x03, 0xFE, 0x7F, 0x01\n\t"       \
            ".byte 0x75, 0x03\n\t"                /* jne +3: int 2Eh */      \
            ".byte 0x0F, 0x05, 0xC3\n\t"          /* syscall; ret */         \
            ".byte 0xCD, 0x2E, 0xC3\n\t"          /* int 2Eh; ret */         \
            ".seh_endproc\n\t"                                              \
            ".byte 0x0F, 0x1F, 0x84, 0, 0, 0, 0, 0\n\t"   /* nop: 32 bytes */ \
            ".section .drectve,\"yn\"\n\t"                                  \
            ".ascii \" /EXPORT:" #name "\"\n\t"                             \
            ".text\n");

#define XSTUB(name, num) STUB(name, num)
#define NT1903(name, num) STUB(name, num)
#include "nt1903_services.h"
#undef NT1903
/* NovaOS's own services (0x200 on), in number order */
XSTUB(NtNovaLoadDll,        SYS_NtNovaLoadDll)
XSTUB(NtNovaDebugPrint,     SYS_NtNovaDebugPrint)
XSTUB(NtNovaGetRandom,      SYS_NtNovaGetRandom)
XSTUB(NtNovaCreateProcess,  SYS_NtNovaCreateProcess)
XSTUB(NtNovaProcessInfo,    SYS_NtNovaProcessInfo)
XSTUB(NtNovaProcessList,    SYS_NtNovaProcessList)
XSTUB(NtNovaWatchDirectory, SYS_NtNovaWatchDirectory)
XSTUB(NtNovaSocket,         SYS_NtNovaSocket)
XSTUB(NtNovaSockConnect,    SYS_NtNovaSockConnect)
XSTUB(NtNovaSockSend,       SYS_NtNovaSockSend)
XSTUB(NtNovaSockRecv,       SYS_NtNovaSockRecv)
XSTUB(NtNovaSockBind,       SYS_NtNovaSockBind)
XSTUB(NtNovaSockListen,     SYS_NtNovaSockListen)
XSTUB(NtNovaSockAccept,     SYS_NtNovaSockAccept)
XSTUB(NtNovaSockCtl,        SYS_NtNovaSockCtl)
XSTUB(NtNovaSockSendTo,     SYS_NtNovaSockSendTo)
XSTUB(NtNovaSockRecvFrom,   SYS_NtNovaSockRecvFrom)
XSTUB(NtNovaResolve,        SYS_NtNovaResolve)
XSTUB(NtNovaFlushView,      SYS_NtNovaFlushView)
XSTUB(NtNovaBugCheck,       SYS_NtNovaBugCheck)
XSTUB(NtNovaConsole,        SYS_NtNovaConsole)
XSTUB(NtNovaGuiCreate,      SYS_NtNovaGuiCreate)
XSTUB(NtNovaGuiGetMessage,  SYS_NtNovaGuiGetMessage)
XSTUB(NtNovaGuiInvalidate,  SYS_NtNovaGuiInvalidate)
XSTUB(NtNovaGuiSetText,     SYS_NtNovaGuiSetText)
XSTUB(NtNovaGuiShow,        SYS_NtNovaGuiShow)
XSTUB(NtNovaGuiDestroy,     SYS_NtNovaGuiDestroy)
XSTUB(NtNovaGuiSetTimer,    SYS_NtNovaGuiSetTimer)
XSTUB(NtNovaGuiKillTimer,   SYS_NtNovaGuiKillTimer)
XSTUB(NtNovaGuiMessageBox,  SYS_NtNovaGuiMessageBox)
XSTUB(NtNovaGuiScreenSize,  SYS_NtNovaGuiScreenSize)
XSTUB(NtNovaGuiPostMessage, SYS_NtNovaGuiPostMessage)
XSTUB(NtNovaGuiCtl,         SYS_NtNovaGuiCtl)
XSTUB(NtNovaClipboard,      SYS_NtNovaClipboard)
XSTUB(NtNovaAudioOpen,      SYS_NtNovaAudioOpen)
XSTUB(NtNovaAudioWrite,     SYS_NtNovaAudioWrite)
XSTUB(NtNovaAudioCtl,       SYS_NtNovaAudioCtl)
XSTUB(NtNovaGpuCtl,         SYS_NtNovaGpuCtl)
XSTUB(NtNovaFirmwareTable,  SYS_NtNovaFirmwareTable)
#endif

/* -----------------------------------------------------------------------
 * Memory/string primitives (real ntdll exports these too)
 * ----------------------------------------------------------------------- */
/* rep movsb/stosb: fast, and the compiler can't turn them back into calls */
__declspec(dllexport) void *memcpy(void *d, const void *s, size_t n)
{
    void *r = d;
    __asm__ volatile ("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return r;
}

__declspec(dllexport) void *memmove(void *d, const void *s, size_t n)
{
    void *r = d;
    if ((char *)d <= (const char *)s || (char *)d >= (const char *)s + n) {
        __asm__ volatile ("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    } else {
        d = (char *)d + n - 1; s = (const char *)s + n - 1;
        __asm__ volatile ("std; rep movsb; cld" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    }
    return r;
}

__declspec(dllexport) void *memset(void *d, int c, size_t n)
{
    void *r = d;
    __asm__ volatile ("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return r;
}

__declspec(dllexport) int memcmp(const void *x, const void *y, size_t n)
{
    const unsigned char *a = x, *b = y;
    for (; n; n--, a++, b++) if (*a != *b) return *a - *b;
    return 0;
}

__declspec(dllexport) size_t strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

__declspec(dllexport) int _strnicmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        int x = lc((unsigned char)*a), y = lc((unsigned char)*b);
        if (x != y || !x) return x - y;
    }
    return 0;
}

__declspec(dllexport) int _stricmp(const char *a, const char *b) { return _strnicmp(a, b, (size_t)-1); }

__declspec(dllexport) void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    for (; n; n--, p++) if (*p == (unsigned char)c) return (void *)p;
    return 0;
}

__declspec(dllexport) char *strcpy(char *d, const char *s)
{
    char *r = d;
    while ((*d++ = *s++)) {}
    return r;
}

__declspec(dllexport) char *strrchr(const char *s, int c)
{
    const char *r = 0;
    do { if (*s == (char)c) r = s; } while (*s++);
    return (char *)r;
}

__declspec(dllexport) size_t wcslen(const WCHAR *s)
{
    const WCHAR *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

__declspec(dllexport) WCHAR *wcschr(const WCHAR *s, WCHAR c)
{
    do { if (*s == c) return (WCHAR *)s; } while (*s++);
    return 0;
}

__declspec(dllexport) WCHAR *wcscpy(WCHAR *d, const WCHAR *s)
{
    WCHAR *r = d;
    while ((*d++ = *s++)) {}
    return r;
}

__declspec(dllexport) int wcscpy_s(WCHAR *d, size_t n, const WCHAR *s)
{
    if (!d || !n) return 22;                       /* EINVAL */
    if (!s) { d[0] = 0; return 22; }
    size_t len = wcslen(s);
    if (len >= n) { d[0] = 0; return 34; }         /* ERANGE */
    for (size_t i = 0; i <= len; i++) d[i] = s[i];
    return 0;
}

__declspec(dllexport) int wcsncmp(const WCHAR *a, const WCHAR *b, size_t n)
{
    for (; n; n--, a++, b++) if (*a != *b || !*a) return (int)*a - (int)*b;
    return 0;
}

__declspec(dllexport) WCHAR *wcspbrk(const WCHAR *s, const WCHAR *set)
{
    for (; *s; s++) for (const WCHAR *t = set; *t; t++) if (*s == *t) return (WCHAR *)s;
    return 0;
}

__declspec(dllexport) WCHAR *wcstok_s(WCHAR *s, const WCHAR *delim, WCHAR **ctx)
{
    if (!s) s = *ctx;
    while (*s && wcschr(delim, *s)) s++;
    if (!*s) { *ctx = s; return 0; }
    WCHAR *tok = s;
    while (*s && !wcschr(delim, *s)) s++;
    if (*s) *s++ = 0;
    *ctx = s;
    return tok;
}

__declspec(dllexport) unsigned long wcstoul(const WCHAR *s, WCHAR **end, int base)
{
    const WCHAR *p = s;
    unsigned long v = 0;
    int neg = 0;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p == '-' || *p == '+') neg = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { p += 2; base = 16; }
    else if (base == 0) base = p[0] == '0' ? 8 : 10;
    const WCHAR *start = p;
    for (;; p++) {
        int d = *p >= '0' && *p <= '9' ? *p - '0' : lc(*p) >= 'a' && lc(*p) <= 'z' ? lc(*p) - 'a' + 10 : 99;
        if (d >= base) break;
        v = v * (unsigned long)base + (unsigned long)d;
    }
    if (end) *end = (WCHAR *)(p == start ? s : p);
    return neg ? (unsigned long)-(long)v : v;
}

/* -----------------------------------------------------------------------
 * PEB, strings, errors, exit
 * ----------------------------------------------------------------------- */
NTSYSAPI PPEB NTAPI RtlGetCurrentPeb(void)
{
    PPEB peb;
#ifdef _WIN64
    __asm__("movq %%gs:0x60, %0" : "=r"(peb));
#else
    __asm__("movl %%fs:0x30, %0" : "=r"(peb));
#endif
    return peb;
}

/* Windows 10 22H2 (build 19045), the version NovaOS reports everywhere */
NTSYSAPI VOID NTAPI RtlGetNtVersionNumbers(ULONG *major, ULONG *minor, ULONG *build)
{
    if (major) *major = 10;
    if (minor) *minor = 0;
    if (build) *build = 0xF0000000u | 19045;       /* high nibble: a free (retail) build */
}

/* RtlGetDeviceFamilyInfoEnum(ULONGLONG *UAPInfo, DWORD *DeviceFamily, DWORD *DeviceForm):
 * the same version packed (major.minor.build.revision, 16 bits each; the
 * revision is the registry's UBR), on a desktop PC (Chromium's setup
 * checks the family before it installs) */
NTSYSAPI VOID NTAPI RtlGetDeviceFamilyInfoEnum(ULONGLONG *uap, ULONG *family, ULONG *form)
{
    if (uap) *uap = (ULONGLONG)10 << 48 | (ULONGLONG)0 << 32 | (ULONGLONG)19045 << 16 | 1;
    if (family) *family = 3;                       /* DEVICEFAMILYINFOENUM_DESKTOP */
    if (form) *form = 0;                           /* DEVICEFAMILYDEVICEFORM_UNKNOWN, as most PCs report */
}

NTSYSAPI NTSTATUS NTAPI RtlGetVersion(PVOID info)
{
    ULONG *v = info;                               /* RTL_OSVERSIONINFOW(EX) */
    ULONG size = v[0];
    if (size < 276) return 0xC000000D;             /* STATUS_INVALID_PARAMETER */
    for (ULONG i = 1; i < size / 4; i++) v[i] = 0;
    v[1] = 10;                                     /* major */
    v[2] = 0;                                      /* minor */
    v[3] = 19045;                                  /* build */
    v[4] = 2;                                      /* VER_PLATFORM_WIN32_NT */
    if (size >= 284) ((UCHAR *)v)[282] = 1;        /* wProductType: VER_NT_WORKSTATION */
    return 0;
}

/* One field of RtlVerifyVersionInfo: @cur against @want under VER_EQUAL (1),
 * VER_GREATER (2), VER_GREATER_EQUAL (3), VER_LESS (4) or VER_LESS_EQUAL (5) */
static BOOLEAN ver_test(ULONG cur, ULONG want, ULONG op)
{
    switch (op) {
    case 1: return cur == want;
    case 2: return cur > want;
    case 3: return cur >= want;
    case 4: return cur < want;
    case 5: return cur <= want;
    }
    return FALSE;
}

/* Does this system's version (RtlGetVersion's) meet @info's fields named in
 * @mask (VER_MINORVERSION 1, VER_MAJORVERSION 2, VER_BUILDNUMBER 4,
 * VER_PLATFORMID 8, VER_SERVICEPACKMINOR 0x10, VER_SERVICEPACKMAJOR 0x20,
 * VER_SUITENAME 0x40, VER_PRODUCT_TYPE 0x80) under @cond's operators (three
 * bits per field, VerSetConditionMask's)?  Major version, minor version and
 * service pack compare as one number, under the operator of the first of
 * them in @mask, as on Windows; the suite under VER_AND (6, all of its
 * bits) or VER_OR (7, any).  STATUS_SUCCESS, or STATUS_REVISION_MISMATCH. */
NTSYSAPI NTSTATUS NTAPI RtlVerifyVersionInfo(PVOID info, ULONG mask, ULONGLONG cond)
{
    const ULONG *want = info;                      /* RTL_OSVERSIONINFOEXW */
    if (!info || !mask || !cond) return 0xC000000D;   /* STATUS_INVALID_PARAMETER */
    ULONG me[71];                                  /* (284 bytes) */
    me[0] = sizeof(me);
    RtlGetVersion(me);
    #define OP(bit) ((ULONG)(cond >> (3 * (bit))) & 7)
    #define W16(v, off) (((const USHORT *)(v))[(off) / 2])
    #define B8(v, off) (((const UCHAR *)(v))[off])
    if ((mask & 0x80) && !ver_test(B8(me, 282), B8(want, 282), OP(7))) goto no;
    if ((mask & 0x40)) {
        USHORT have = W16(me, 280), need = W16(want, 280);
        if (OP(6) == 6 ? (have & need) != need : OP(6) == 7 ? !(have & need) && need : FALSE) goto no;
    }
    if ((mask & 0x08) && !ver_test(me[4], want[4], OP(3))) goto no;
    if ((mask & 0x04) && !ver_test(me[3], want[3], OP(2))) goto no;
    /* major, minor, service pack major, minor: as one number */
    static const struct { ULONG bit, shift; } part[4] = { { 0x02, 1 }, { 0x01, 0 }, { 0x20, 5 }, { 0x10, 4 } };
    ULONG op = 0;
    for (int i = 0; i < 4; i++) {
        if (!(mask & part[i].bit)) continue;
        if (!op) op = OP(part[i].shift);
        ULONG c = i == 0 ? me[1] : i == 1 ? me[2] : i == 2 ? W16(me, 276) : W16(me, 278);
        ULONG w = i == 0 ? want[1] : i == 1 ? want[2] : i == 2 ? W16(want, 276) : W16(want, 278);
        if (c != w) {
            if (!ver_test(c, w, op)) goto no;
            op = 0xFF;                             /* decided */
            break;
        }
    }
    if (op && op != 0xFF && !ver_test(0, 0, op)) goto no;   /* all equal: only =, >= and <= hold */
    return 0;
no:
    return 0xC0000059;                             /* STATUS_REVISION_MISMATCH */
    #undef OP
    #undef W16
    #undef B8
}

NTSYSAPI VOID NTAPI RtlInitUnicodeString(PUNICODE_STRING us, const WCHAR *s)
{
    USHORT n = 0;
    if (s) while (s[n]) n++;
    us->Length = (USHORT)(n * 2);
    us->MaximumLength = (USHORT)(s ? n * 2 + 2 : 0);
    us->Buffer = (WCHAR *)s;
}

#ifndef _WIN64
/* Extended attributes: the file system has none */
NTSYSAPI NTSTATUS NTAPI NtSetEaFile(HANDLE h, PIO_STATUS_BLOCK io, PVOID buf, ULONG len)
{
    (void)h; (void)buf; (void)len;
    if (io) { io->Status = (NTSTATUS)0xC000004F; io->Information = 0; }
    return (NTSTATUS)0xC000004F;                      /* STATUS_EAS_NOT_SUPPORTED */
}
NTSYSAPI NTSTATUS NTAPI NtQueryEaFile(HANDLE h, PIO_STATUS_BLOCK io, PVOID buf, ULONG len, BOOLEAN single, PVOID list,
                                      ULONG list_len, PULONG index, BOOLEAN restart)
{
    (void)h; (void)buf; (void)len; (void)single; (void)list; (void)list_len; (void)index; (void)restart;
    if (io) { io->Status = (NTSTATUS)0xC000004F; io->Information = 0; }
    return (NTSTATUS)0xC000004F;
}
#endif  /* x64: system calls (the stubs in ntdll.c) */

NTSYSAPI ULONG NTAPI RtlNtStatusToDosError(NTSTATUS s)
{
    switch ((ULONG)s) {
    case 0x00000000: return ERROR_SUCCESS;
    case 0x00000103: return 997;                          /* ERROR_IO_PENDING */
    case 0x80000005: return ERROR_MORE_DATA;
    case 0xC0000010: return 1;                            /* ERROR_INVALID_FUNCTION */
    case 0xC0000023: return ERROR_INSUFFICIENT_BUFFER;
    case 0xC00000AB: case 0xC00000AC: case 0xC00000AE: return 231;   /* ERROR_PIPE_BUSY */
    case 0xC00000AD: return 230;                          /* ERROR_BAD_PIPE */
    case 0xC00000B0: return 233;                          /* ERROR_PIPE_NOT_CONNECTED */
    case 0xC00000B1: case 0xC00000D9: return 232;         /* ERROR_NO_DATA */
    case 0xC00000B2: return 535;                          /* ERROR_PIPE_CONNECTED */
    case 0xC00000B3: return 536;                          /* ERROR_PIPE_LISTENING */
    case 0xC00000B5: return 121;                          /* ERROR_SEM_TIMEOUT */
    case 0xC000014B: return 109;                          /* ERROR_BROKEN_PIPE */
    case 0xC0000120: return 995;                          /* ERROR_OPERATION_ABORTED */
    case 0xC0000236: return 1225;                         /* STATUS_CONNECTION_REFUSED: ERROR_CONNECTION_REFUSED */
    case 0xC000020D: case 0xC000013B: return 64;          /* connection reset, local disconnect: ERROR_NETNAME_DELETED */
    case 0xC0000241: return 1236;                         /* ERROR_CONNECTION_ABORTED */
    case 0xC000023C: return 1231;                         /* ERROR_NETWORK_UNREACHABLE */
    case 0xC000023D: return 1232;                         /* ERROR_HOST_UNREACHABLE */
    case 0xC00000B6: return 1236;                         /* STATUS_FILE_FORCED_CLOSED */
    case 0xC0000225: return 1168;                         /* ERROR_NOT_FOUND */
    case 0xC000004F: return 282;                          /* ERROR_EAS_NOT_SUPPORTED */
    case 0xC0000135: return 126;                          /* ERROR_MOD_NOT_FOUND */
    case 0xC000007B: return 193;                          /* ERROR_BAD_EXE_FORMAT */
    case 0x80000006: return ERROR_NO_MORE_FILES;
    case 0xC0000002: return ERROR_CALL_NOT_IMPLEMENTED;
    case 0xC0000003: case 0xC000000D: return ERROR_INVALID_PARAMETER;
    case 0xC0000004: return ERROR_INSUFFICIENT_BUFFER;
    case 0xC0000008: return ERROR_INVALID_HANDLE;
    case 0xC0000011: return ERROR_HANDLE_EOF;
    case 0xC0000017: case 0xC000009A: return ERROR_NOT_ENOUGH_MEMORY;
    case 0xC0000040: return ERROR_NOT_ENOUGH_MEMORY;      /* STATUS_SECTION_TOO_BIG */
    case 0xC000011E: return 1006;                         /* STATUS_MAPPED_FILE_SIZE_ZERO: ERROR_FILE_INVALID */
    case 0xC000012D: return 1455;                         /* STATUS_COMMITMENT_LIMIT: ERROR_COMMITMENT_LIMIT */
    case 0xC0000018: case 0xC00000A0: return ERROR_INVALID_ADDRESS;
    case 0xC0000022: return ERROR_ACCESS_DENIED;
    case 0xC0000033: return ERROR_INVALID_NAME;
    case 0xC0000034: return ERROR_FILE_NOT_FOUND;
    case 0xC0000035: return ERROR_ALREADY_EXISTS;
    case 0xC000003A: return ERROR_PATH_NOT_FOUND;
    case 0xC000007F: return ERROR_DISK_FULL;
    case 0xC00000A2: return 19;                           /* ERROR_WRITE_PROTECT: a read-only volume */
    case 0xC0000032: return 1392;                         /* ERROR_FILE_CORRUPT */
    case 0xC00000BA: return ERROR_ACCESS_DENIED;          /* file is a directory */
    case 0xC0000101: return ERROR_DIR_NOT_EMPTY;
    case 0xC0000103: return ERROR_INVALID_NAME;           /* not a directory */
    case 0xC0000106: return 206;                          /* ERROR_FILENAME_EXCED_RANGE: STATUS_NAME_TOO_LONG */
    case 0xC000011F: return ERROR_TOO_MANY_OPEN_FILES;
    case 0xC0000121: return ERROR_ACCESS_DENIED;
    case 0xC0000056: return ERROR_ACCESS_DENIED;          /* STATUS_DELETE_PENDING */
    case 0xC0000024: return 6;                            /* OBJECT_TYPE_MISMATCH: ERROR_INVALID_HANDLE */
    case 0xC000005C: return 1309;                         /* ERROR_NO_IMPERSONATION_TOKEN */
    case 0xC0000078: return 1337;                         /* ERROR_INVALID_SID */
    case 0xC0000079: return 1338;                         /* ERROR_INVALID_SECURITY_DESCR */
    case 0xC000007C: return 1008;                         /* ERROR_NO_TOKEN */
    case 0xC00000A8: return 1349;                         /* ERROR_BAD_TOKEN_TYPE */
    }
    return ERROR_INVALID_FUNCTION;
}

NTSYSAPI VOID NTAPI RtlExitUserProcess(NTSTATUS status)
{
    extern void nova_run_process_detach(void);
    nova_run_process_detach();
    NtTerminateProcess(NtCurrentProcess(), status);
    for (;;) NtYieldExecution();
}

NTSYSAPI VOID NTAPI RtlExitUserThread(NTSTATUS status)
{
    extern void nova_run_thread_detach(void);
    nova_run_thread_detach();
    NtTerminateThread(NtCurrentThread(), status);
    for (;;) NtYieldExecution();
}

__declspec(dllexport) int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/* -----------------------------------------------------------------------
 * Heap
 * ----------------------------------------------------------------------- */
typedef struct Block {
    SIZE_T        size;        /* usable bytes (class size, or the large size) */
    SIZE_T        tag;         /* HEAP_MAGIC with the arena and class in its low bytes, or HEAP_LARGE */
#ifndef _WIN64
    SIZE_T        pad[2];      /* (32-bit: SSE code and JIT compilers such as
                                * Mesa's expect 16-byte-aligned blocks too) */
#endif
} Block;                       /* 16 bytes: user data stays 16-byte aligned */

#define HEAP_MAGIC   ((SIZE_T)0x4E4F564148454150ULL)   /* "NOVAHEAP" (its low half in 32-bit programs; low 2 bytes: arena, class) */
#define HEAP_LARGE   ((SIZE_T)0x4E4F56414C415247ULL)   /* "NOVALARG" */
#define NCLASSES     48
#define LARGE_MIN    (256 * 1024)
#ifdef _WIN64
#define RESERVE_SIZE ((SIZE_T)1024 * 1024 * 1024)   /* 1 GiB of address space */
#else
#define RESERVE_SIZE ((SIZE_T)128 * 1024 * 1024)    /* 32-bit programs: 128 MiB at a time */
#endif
#define ARENA_SIZE   (1024 * 1024)

/* Small blocks come from a few arenas, each with its own lock and free
 * lists; a thread uses the one its ID picks (threads side by side mostly
 * get different ones) and a block goes back to the arena it came from
 * (its number is in the tag).  Fresh memory is carved under a lock of its
 * own. */
#define NARENAS      8
typedef struct {
    volatile long lock;
    void         *free_list[NCLASSES];
    char          pad[64];                  /* (no line shared with the next arena's lock) */
} Arena;

static SIZE_T  class_size[NCLASSES];
static Arena   arenas[NARENAS];
static char   *arena_base, *arena_cur, *arena_end, *arena_reserved_end;
static int     heap_ready;
static volatile long carve_lock;

static void spin_lock(volatile long *l)
{
    for (int spins = 0; __atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE); )
        while (__atomic_load_n(l, __ATOMIC_RELAXED))
            if (++spins < 1000) __builtin_ia32_pause();
            else { NtYieldExecution(); spins = 0; }     /* (its holder was switched out) */
}
static void spin_unlock(volatile long *l) { __atomic_store_n(l, 0, __ATOMIC_RELEASE); }

static int my_arena(void)
{
#ifdef _WIN64
    SIZE_T tid = *(SIZE_T *)(NtCurrentTebBytes() + 0x48);  /* ClientId.UniqueThread */
#else
    SIZE_T tid = *(SIZE_T *)(NtCurrentTebBytes() + 0x24);
#endif
    return (int)(tid / 4 % NARENAS);
}

static void heap_init(void)
{
    /* 16..1024 in 16-byte steps (64 would be too many): 16, 32, 48 ... then x1.25 */
    SIZE_T s = 16;
    for (int i = 0; i < NCLASSES; i++) {
        class_size[i] = s;
        s = s < 256 ? s + 16 : (s + s / 4 + 15) & ~(SIZE_T)15;
        if (s > LARGE_MIN) s = LARGE_MIN;
    }
    PVOID base = 0;
    SIZE_T size = RESERVE_SIZE;
    if (NT_SUCCESS(NtAllocateVirtualMemory(NtCurrentProcess(), &base, 0, &size, MEM_RESERVE, PAGE_READWRITE))) {
        arena_base = arena_cur = arena_end = base;
        arena_reserved_end = (char *)base + size;
    }
    heap_ready = 1;
    RtlGetCurrentPeb()->ProcessHeap = (PVOID)&heap_ready;
}

static int class_of(SIZE_T n)
{
    for (int i = 0; i < NCLASSES; i++) if (class_size[i] >= n) return i;
    return -1;
}

static void *carve(SIZE_T bytes)
{
    if (arena_cur + bytes > arena_end) {
        SIZE_T grow = bytes > ARENA_SIZE ? (bytes + ARENA_SIZE - 1) & ~(SIZE_T)(ARENA_SIZE - 1) : ARENA_SIZE;
        if (!arena_base || arena_end + grow > arena_reserved_end) {
            /* this reservation is used up: start another */
            PVOID base = 0;
            SIZE_T size = grow > RESERVE_SIZE ? grow : RESERVE_SIZE;
            if (!NT_SUCCESS(NtAllocateVirtualMemory(NtCurrentProcess(), &base, 0, &size, MEM_RESERVE, PAGE_READWRITE)))
                return 0;
            arena_base = arena_cur = arena_end = base;
            arena_reserved_end = (char *)base + size;
        }
        PVOID at = arena_end;
        SIZE_T sz = grow;
        if (!NT_SUCCESS(NtAllocateVirtualMemory(NtCurrentProcess(), &at, 0, &sz, MEM_COMMIT, PAGE_READWRITE)))
            return 0;
        arena_end += grow;
    }
    void *p = arena_cur;
    arena_cur += bytes;
    return p;
}

NTSYSAPI PVOID NTAPI RtlGetProcessHeap(void)
{
    if (!heap_ready) heap_init();
    return (PVOID)&heap_ready;
}

NTSYSAPI PVOID NTAPI RtlAllocateHeap(PVOID heap, ULONG flags, SIZE_T n)
{
    (void)heap;
    if (!heap_ready) heap_init();
    if (!n) n = 1;
    Block *b;
    if (n >= LARGE_MIN - sizeof(Block)) {
        PVOID base = 0;
        SIZE_T size = n + sizeof(Block);
        if (!NT_SUCCESS(NtAllocateVirtualMemory(NtCurrentProcess(), &base, 0, &size,
                                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)))
            return 0;
        b = base;
        b->size = n;
        b->tag = HEAP_LARGE;
        return b + 1;                               /* fresh pages are zeroed */
    }
    int c = class_of(n), a = my_arena();
    Arena *ar = &arenas[a];
    spin_lock(&ar->lock);
    if (ar->free_list[c]) {
        b = (Block *)ar->free_list[c] - 1;
        ar->free_list[c] = *(void **)ar->free_list[c];
        spin_unlock(&ar->lock);
    } else {
        spin_unlock(&ar->lock);
        spin_lock(&carve_lock);
        b = carve(class_size[c] + sizeof(Block));
        spin_unlock(&carve_lock);
    }
    if (!b) return 0;
    b->size = class_size[c];
    b->tag = (HEAP_MAGIC & ~(SIZE_T)0xFFFF) | (SIZE_T)a << 8 | (SIZE_T)c;  /* (the magic's own low bytes would hide them) */
    if (flags & HEAP_ZERO_MEMORY) memset(b + 1, 0, class_size[c]);
    return b + 1;
}

NTSYSAPI BOOLEAN NTAPI RtlFreeHeap(PVOID heap, ULONG flags, PVOID p)
{
    (void)heap; (void)flags;
    if (!p) return TRUE;
    Block *b = (Block *)p - 1;
    if (b->tag == HEAP_LARGE) {
        PVOID base = b;
        SIZE_T size = 0;
        return NT_SUCCESS(NtFreeVirtualMemory(NtCurrentProcess(), &base, &size, MEM_RELEASE));
    }
    if ((b->tag & ~(SIZE_T)0xFFFF) != (HEAP_MAGIC & ~(SIZE_T)0xFFFF)) return FALSE;   /* not ours */
    int c = (int)(b->tag & 0xFF), a = (int)(b->tag >> 8 & 0xFF);
    if (c >= NCLASSES || a >= NARENAS) return FALSE;
    Arena *ar = &arenas[a];
    spin_lock(&ar->lock);
    *(void **)p = ar->free_list[c];
    ar->free_list[c] = p;
    b->tag = 0;                                     /* catches double frees */
    spin_unlock(&ar->lock);
    return TRUE;
}

NTSYSAPI SIZE_T NTAPI RtlSizeHeap(PVOID heap, ULONG flags, const VOID *p)
{
    (void)heap; (void)flags;
    return p ? ((const Block *)p - 1)->size : (SIZE_T)-1;
}

/* Private heaps share the process heap (HeapCreate does the same) */
NTSYSAPI PVOID NTAPI RtlCreateHeap(ULONG flags, PVOID base, SIZE_T reserve, SIZE_T commit, PVOID lock, PVOID params)
{
    (void)flags; (void)base; (void)reserve; (void)commit; (void)lock; (void)params;
    return RtlGetProcessHeap();
}

NTSYSAPI PVOID NTAPI RtlDestroyHeap(PVOID heap) { (void)heap; return 0; }

/* The heap Windows shares with csrss.exe. Nothing allocates from it, but
 * Chromium's sandbox finds it by its header (the segment and heap
 * signatures, and heap class 8 in Flags) and destroys it before cutting a
 * content process off from csrss, and gives up if it isn't there. */
static __attribute__((aligned(16))) UCHAR csr_port_heap[0x100];

static int csr_ready;

static PVOID csr_heap(void)
{
    UCHAR *h = csr_port_heap;
    if (!csr_ready) {
        csr_ready = 1;
#ifdef _WIN64
        *(ULONG *)(h + 0x10) = 0xFFEEFFEE;          /* SegmentSignature */
        *(PVOID *)(h + 0x28) = h;                   /* Heap */
        *(ULONG *)(h + 0x70) = 0x8000 | 0x2;        /* Flags: HEAP_CLASS_8 | HEAP_GROWABLE */
        *(ULONG *)(h + 0x98) = 0xEEFFEEFF;          /* Signature */
#else
        *(ULONG *)(h + 0x08) = 0xFFEEFFEE;
        *(PVOID *)(h + 0x18) = h;
        *(ULONG *)(h + 0x40) = 0x8000 | 0x2;
        *(ULONG *)(h + 0x60) = 0xEEFFEEFF;
#endif
    }
    return h;
}

NTSYSAPI ULONG NTAPI RtlGetProcessHeaps(ULONG n, PVOID *heaps)
{
    if (heaps && n >= 1) heaps[0] = RtlGetProcessHeap();
    if (heaps && n >= 2) heaps[1] = csr_heap();
    return 2;
}

NTSYSAPI PVOID NTAPI RtlReAllocateHeap(PVOID heap, ULONG flags, PVOID p, SIZE_T n)
{
    if (!p) return RtlAllocateHeap(heap, flags, n);
    SIZE_T old = RtlSizeHeap(heap, 0, p);
    if (n <= old && ((Block *)p - 1)->tag != HEAP_LARGE) return p;
    PVOID q = RtlAllocateHeap(heap, flags, n);
    if (!q) return 0;
    memcpy(q, p, old < n ? old : n);
    RtlFreeHeap(heap, 0, p);
    return q;
}

/* Signal one object, then wait on another (the sandbox IPC client uses it) */
NTSTATUS NTAPI NtSetEvent(HANDLE, PLONG);
NTSTATUS NTAPI NtReleaseMutant(HANDLE, PLONG);
NTSTATUS NTAPI NtReleaseSemaphore(HANDLE, LONG, PLONG);
#ifndef _WIN64
NTSYSAPI NTSTATUS NTAPI NtSignalAndWaitForSingleObject(HANDLE sig, HANDLE wait, BOOLEAN alertable, PLARGE_INTEGER timeout)
{
    if (!NT_SUCCESS(NtSetEvent(sig, 0)) && !NT_SUCCESS(NtReleaseMutant(sig, 0))) {
        NTSTATUS s = NtReleaseSemaphore(sig, 1, 0);
        if (!NT_SUCCESS(s)) return s;
    }
    return NtWaitForSingleObject(wait, alertable, timeout);
}
#endif  /* x64: system calls (the stubs in ntdll.c) */

/* AppContainer capability SIDs: NovaOS has no AppContainers */
NTSYSAPI NTSTATUS NTAPI RtlDeriveCapabilitySidsFromName(PVOID name, PVOID group_sid, PVOID sid)
{
    (void)name; (void)group_sid; (void)sid;
    return 0xC00000BB;                             /* STATUS_NOT_SUPPORTED */
}
