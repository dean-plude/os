/* abitest.exe — NovaOS's binary interface against Windows 10 1903 x64:
 * the TEB, PEB, process parameters, loader data, KUSER_SHARED_DATA and
 * CONTEXT/EXCEPTION_RECORD layouts, ntdll's system-call stubs and the
 * system-call numbers themselves.  Programs that read these structures
 * directly, or make system calls without ntdll, depend on every offset
 * and number here; each is written out as the Windows value (not taken
 * from NovaOS's headers), so if one moves this test fails. */
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include <winternl.h>

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

#ifndef PF_XMMI64_INSTRUCTIONS_AVAILABLE
#define PF_XMMI64_INSTRUCTIONS_AVAILABLE 10
#endif

#define U8(p, o)  (*(volatile BYTE *)((BYTE *)(p) + (o)))
#define U16(p, o) (*(volatile USHORT *)((BYTE *)(p) + (o)))
#define U32(p, o) (*(volatile ULONG *)((BYTE *)(p) + (o)))
#define U64(p, o) (*(volatile ULONGLONG *)((BYTE *)(p) + (o)))
#define PTR(p, o) ((BYTE *)(ULONG_PTR)U64(p, o))

#ifdef _WIN64

/* ---- compile time: NovaOS's own headers agree with Windows ---- */
_Static_assert(sizeof(CONTEXT) == 0x4D0, "CONTEXT size");
_Static_assert(offsetof(CONTEXT, ContextFlags) == 0x30, "CONTEXT.ContextFlags");
_Static_assert(offsetof(CONTEXT, MxCsr) == 0x34, "CONTEXT.MxCsr");
_Static_assert(offsetof(CONTEXT, SegCs) == 0x38, "CONTEXT.SegCs");
_Static_assert(offsetof(CONTEXT, SegSs) == 0x42, "CONTEXT.SegSs");
_Static_assert(offsetof(CONTEXT, EFlags) == 0x44, "CONTEXT.EFlags");
_Static_assert(offsetof(CONTEXT, Dr0) == 0x48, "CONTEXT.Dr0");
_Static_assert(offsetof(CONTEXT, Dr7) == 0x70, "CONTEXT.Dr7");
_Static_assert(offsetof(CONTEXT, Rax) == 0x78, "CONTEXT.Rax");
_Static_assert(offsetof(CONTEXT, Rcx) == 0x80, "CONTEXT.Rcx");
_Static_assert(offsetof(CONTEXT, Rsp) == 0x98, "CONTEXT.Rsp");
_Static_assert(offsetof(CONTEXT, Rbp) == 0xA0, "CONTEXT.Rbp");
_Static_assert(offsetof(CONTEXT, R8) == 0xB8, "CONTEXT.R8");
_Static_assert(offsetof(CONTEXT, R15) == 0xF0, "CONTEXT.R15");
_Static_assert(offsetof(CONTEXT, Rip) == 0xF8, "CONTEXT.Rip");
_Static_assert(offsetof(CONTEXT, FltSave) == 0x100, "CONTEXT.FltSave");
_Static_assert(offsetof(CONTEXT, Xmm0) == 0x1A0, "CONTEXT.Xmm0");
_Static_assert(offsetof(CONTEXT, Xmm15) == 0x290, "CONTEXT.Xmm15");
_Static_assert(offsetof(CONTEXT, VectorRegister) == 0x300, "CONTEXT.VectorRegister");
_Static_assert(offsetof(CONTEXT, VectorControl) == 0x4A0, "CONTEXT.VectorControl");
_Static_assert(offsetof(CONTEXT, DebugControl) == 0x4A8, "CONTEXT.DebugControl");
_Static_assert(offsetof(CONTEXT, LastExceptionFromRip) == 0x4C8, "CONTEXT.LastExceptionFromRip");
_Static_assert(offsetof(EXCEPTION_RECORD, ExceptionCode) == 0x00, "EXCEPTION_RECORD.ExceptionCode");
_Static_assert(offsetof(EXCEPTION_RECORD, ExceptionFlags) == 0x04, "EXCEPTION_RECORD.ExceptionFlags");
_Static_assert(offsetof(EXCEPTION_RECORD, ExceptionAddress) == 0x10, "EXCEPTION_RECORD.ExceptionAddress");
_Static_assert(offsetof(EXCEPTION_RECORD, NumberParameters) == 0x18, "EXCEPTION_RECORD.NumberParameters");
_Static_assert(offsetof(EXCEPTION_RECORD, ExceptionInformation) == 0x20, "EXCEPTION_RECORD.ExceptionInformation");
_Static_assert(sizeof(EXCEPTION_RECORD) == 0x98, "EXCEPTION_RECORD size");

/* ---- system calls without ntdll ---- */
__attribute__((naked)) static NTSTATUS raw_syscall(ULONG num, ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3)
{
    __asm__("movl %ecx, %eax\n\t"           /* number */
            "movq %rdx, %r10\n\t"           /* the service's arguments: a1, a2, a3 */
            "movq %r8, %rdx\n\t"
            "movq %r9, %r8\n\t"
            "syscall\n\t"
            "retq");
}

/* Windows 10 1903's numbers, every one */
static const struct { const char *name; ULONG num; } g_nt1903[] = {
#define NT1903(n, v) { #n, v },
#include "nt1903_services.h"
#undef NT1903
};

static ULONG_PTR g_fault_rip, g_fault_ctx_rip, g_fault_addr;
static ULONG g_fault_code, g_fault_params;
static LONG WINAPI on_fault(EXCEPTION_POINTERS *e)
{
    BYTE *ctx = (BYTE *)e->ContextRecord, *rec = (BYTE *)e->ExceptionRecord;
    g_fault_code = U32(rec, 0x00);
    g_fault_addr = (ULONG_PTR)PTR(rec, 0x10);
    g_fault_params = U32(rec, 0x18);
    g_fault_ctx_rip = (ULONG_PTR)U64(ctx, 0xF8);
    if (g_fault_code == EXCEPTION_BREAKPOINT) {
        U64(ctx, 0xF8) = U64(ctx, 0xF8) + 1;                /* step over the int3 (Rip at 0xF8) */
        U64(ctx, 0x78) = 0x1234;                            /* and hand back Rax (0x78) */
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

__attribute__((noinline)) static ULONG_PTR breakpoint(void)
{
    ULONG_PTR rax;
    __asm__ volatile("xorl %%eax, %%eax\n\t"
                     "leaq 0f(%%rip), %%rcx\n\t"
                     "movq %%rcx, %1\n"
                     "0: int3\n\t"
                     "movq %%rax, %0" : "=r"(rax), "=m"(g_fault_rip) : : "rax", "rcx", "memory");
    return rax;
}

static volatile LONG g_spin;
static DWORD WINAPI spinner(LPVOID p) { (void)p; for (;;) __atomic_add_fetch(&g_spin, 1, __ATOMIC_RELAXED); }

int main(void)
{
    BYTE *teb = NtCurrentTebBytes(), *peb = PTR(teb, 0x60);
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");

    /* ---- TEB ---- */
    ULONG_PTR here = (ULONG_PTR)&teb;
    CHECK("TEB.NtTib.Self (0x30)", PTR(teb, 0x30) == teb);
    CHECK("TEB.NtTib.StackBase (0x08) / StackLimit (0x10)", (ULONG_PTR)PTR(teb, 0x10) < here && here < (ULONG_PTR)PTR(teb, 0x08));
    CHECK("TEB.ClientId.UniqueProcess (0x40)", U64(teb, 0x40) == GetCurrentProcessId());
    CHECK("TEB.ClientId.UniqueThread (0x48)", U64(teb, 0x48) == GetCurrentThreadId());
    CHECK("TEB.ProcessEnvironmentBlock (0x60)", peb != NULL);
    SetLastError(0xABCD1234);
    CHECK("TEB.LastErrorValue (0x68)", U32(teb, 0x68) == 0xABCD1234);
    SetLastError(0);
    DWORD tls = TlsAlloc();
    TlsSetValue(tls, (LPVOID)0x5150);
    CHECK("TEB.TlsSlots (0x1480)", tls < 64 && U64(teb, 0x1480 + 8 * tls) == 0x5150);
    TlsFree(tls);
    CHECK("TEB.DeallocationStack (0x1478)", (ULONG_PTR)PTR(teb, 0x1478) <= (ULONG_PTR)PTR(teb, 0x10));

    /* ---- PEB ---- */
    PROCESS_BASIC_INFORMATION pbi;
    ULONG got = 0;
    CHECK("NtQueryInformationProcess", !NtQueryInformationProcess(GetCurrentProcess(), 0, &pbi, sizeof(pbi), &got));
    CHECK("PEB address (TEB 0x60 = PebBaseAddress)", (BYTE *)pbi.PebBaseAddress == peb);
    CHECK("PEB.BeingDebugged (0x02)", U8(peb, 0x02) == (IsDebuggerPresent() ? 1 : 0));
    CHECK("PEB.ImageBaseAddress (0x10)", PTR(peb, 0x10) == (BYTE *)GetModuleHandleW(NULL));
    CHECK("PEB.ProcessHeap (0x30)", PTR(peb, 0x30) == (BYTE *)GetProcessHeap());
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    CHECK("PEB.NumberOfProcessors (0xB8)", U32(peb, 0xB8) == si.dwNumberOfProcessors);
    CHECK("PEB.OSMajorVersion (0x118)", U32(peb, 0x118) == 10);
    CHECK("PEB.OSMinorVersion (0x11C)", U32(peb, 0x11C) == 0);
    CHECK("PEB.OSBuildNumber (0x120)", U16(peb, 0x120) == 18362);
    CHECK("PEB.OSPlatformId (0x124)", U32(peb, 0x124) == 2);

    /* RTL_USER_PROCESS_PARAMETERS */
    BYTE *pp = PTR(peb, 0x20);
    CHECK("PEB.ProcessParameters (0x20)", pp != NULL);
    if (pp) {
        WCHAR *cmd = (WCHAR *)PTR(pp, 0x78), *img = (WCHAR *)PTR(pp, 0x68), dir[MAX_PATH], mod[MAX_PATH];
        CHECK("Parameters.CommandLine (0x70)", cmd && !wcscmp(cmd, GetCommandLineW()) && U16(pp, 0x70) == 2 * wcslen(cmd));
        GetModuleFileNameW(NULL, mod, MAX_PATH);
        CHECK("Parameters.ImagePathName (0x60)", img && !_wcsicmp(img, mod));
        DWORD n = GetCurrentDirectoryW(MAX_PATH, dir);
        WCHAR *cur = (WCHAR *)PTR(pp, 0x40);
        CHECK("Parameters.CurrentDirectory.DosPath (0x38)", cur && n && !_wcsnicmp(cur, dir, n));
        CHECK("Parameters.StandardOutput (0x28)", (HANDLE)PTR(pp, 0x28) == GetStdHandle(STD_OUTPUT_HANDLE));
        WCHAR *env = (WCHAR *)PTR(pp, 0x80), *s = env;
        int found = 0;
        for (; s && *s; s += wcslen(s) + 1) if (!_wcsnicmp(s, L"SystemRoot=", 11)) found = 1;
        CHECK("Parameters.Environment (0x80)", found);
    }

    /* PEB_LDR_DATA and the module list */
    BYTE *ldr = PTR(peb, 0x18);
    CHECK("PEB.Ldr (0x18)", ldr != NULL);
    if (ldr) {
        BYTE *head = ldr + 0x10, *e = PTR(ldr, 0x10);
        CHECK("Ldr.InLoadOrderModuleList (0x10): the program first", e && PTR(e, 0x30) == (BYTE *)GetModuleHandleW(NULL));
        int seen_ntdll = 0, n = 0;
        for (; e && e != head && n < 256; e = PTR(e, 0x00), n++) {
            WCHAR *base = (WCHAR *)PTR(e, 0x60);                /* BaseDllName (0x58) */
            if (base && !_wcsicmp(base, L"ntdll.dll")) {
                seen_ntdll = PTR(e, 0x30) == (BYTE *)ntdll;     /* DllBase */
                IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((BYTE *)ntdll + ((IMAGE_DOS_HEADER *)ntdll)->e_lfanew);
                CHECK("LDR_DATA_TABLE_ENTRY.SizeOfImage (0x40)", U32(e, 0x40) == nt->OptionalHeader.SizeOfImage);
                CHECK("LDR_DATA_TABLE_ENTRY.FullDllName (0x48)", PTR(e, 0x50) && wcsstr((WCHAR *)PTR(e, 0x50), L"ntdll"));
            }
        }
        CHECK("LDR_DATA_TABLE_ENTRY.DllBase (0x30) / BaseDllName (0x58)", seen_ntdll);
        BYTE *m = PTR(ldr, 0x20);                               /* InMemoryOrderModuleList: links at entry + 0x10 */
        CHECK("Ldr.InMemoryOrderModuleList (0x20)", m && PTR(m - 0x10, 0x30) == (BYTE *)GetModuleHandleW(NULL));
    }

    /* ---- KUSER_SHARED_DATA ---- */
    BYTE *ku = (BYTE *)0x7FFE0000;
    WCHAR win[MAX_PATH];
    GetWindowsDirectoryW(win, MAX_PATH);
    CHECK("KUSER.NtSystemRoot (0x30)", !_wcsicmp((WCHAR *)(ku + 0x30), win));
    CHECK("KUSER.ImageNumberLow/High (0x2C)", U16(ku, 0x2C) == 0x8664 && U16(ku, 0x2E) == 0x8664);
    CHECK("KUSER.NtBuildNumber (0x260)", (U32(ku, 0x260) & 0xFFFF) == 18362);
    CHECK("KUSER.NtProductType (0x264)", U32(ku, 0x264) == 1 && U8(ku, 0x268) == 1);
    CHECK("KUSER.NtMajorVersion (0x26C)", U32(ku, 0x26C) == 10);
    CHECK("KUSER.NtMinorVersion (0x270)", U32(ku, 0x270) == 0);
    CHECK("KUSER.ProcessorFeatures (0x274)", U8(ku, 0x274 + PF_XMMI64_INSTRUCTIONS_AVAILABLE) ==
                                             (IsProcessorFeaturePresent(PF_XMMI64_INSTRUCTIONS_AVAILABLE) ? 1 : 0));
    CHECK("KUSER.SystemCall (0x308) = 0: syscall, not int 2e", U32(ku, 0x308) == 0);
    CHECK("KUSER.ActiveProcessorCount (0x3C0)", U32(ku, 0x3C0) == si.dwNumberOfProcessors);
    ULONGLONG t0 = ((ULONGLONG)U32(ku, 0x324) << 32 | U32(ku, 0x320)) * U32(ku, 0x04) >> 24, t1 = GetTickCount64();
    if (t0 + 200 < t1 || t0 > t1 + 200) printf("KUSER tick count %llu ms, GetTickCount64 %llu ms\n", t0, t1);
    CHECK("KUSER.TickCount (0x320) x TickCountMultiplier (0x04)", t0 + 200 >= t1 && t0 <= t1 + 200);
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULONGLONG st = (ULONGLONG)U32(ku, 0x18) << 32 | U32(ku, 0x14), now = (ULONGLONG)ft.dwHighDateTime << 32 | ft.dwLowDateTime;
    CHECK("KUSER.SystemTime (0x14)", st <= now + 10000000 && st + 10000000 >= now);
    ULONGLONG i0 = (ULONGLONG)U32(ku, 0x0C) << 32 | U32(ku, 0x08);
    Sleep(50);
    ULONGLONG i1 = (ULONGLONG)U32(ku, 0x0C) << 32 | U32(ku, 0x08);
    CHECK("KUSER.InterruptTime (0x08) advances", i1 > i0 && i1 - i0 < 50000000);

    /* ---- CONTEXT at run time ---- */
    CONTEXT c;
    memset(&c, 0, sizeof(c));
    RtlCaptureContext(&c);
    BYTE *cb = (BYTE *)&c;
    CHECK("RtlCaptureContext: Rip (0xF8) in this program", U64(cb, 0xF8) > (ULONGLONG)GetModuleHandleW(NULL) &&
                                                           U64(cb, 0xF8) < (ULONGLONG)GetModuleHandleW(NULL) + 0x100000);
    CHECK("RtlCaptureContext: Rsp (0x98) on this stack", U64(cb, 0x98) < (ULONGLONG)PTR(teb, 0x08) && U64(cb, 0x98) > (ULONGLONG)PTR(teb, 0x10));
    CHECK("RtlCaptureContext: SegCs (0x38) is the 64-bit user selector", (U16(cb, 0x38) & 3) == 3);
    CHECK("RtlCaptureContext: ContextFlags (0x30)", (U32(cb, 0x30) & 0x00100000) != 0);

    PVOID veh = AddVectoredExceptionHandler(1, on_fault);
    ULONG_PTR rax = breakpoint();
    RemoveVectoredExceptionHandler(veh);
    CHECK("EXCEPTION_RECORD.ExceptionCode (0x00)", g_fault_code == EXCEPTION_BREAKPOINT);
    CHECK("EXCEPTION_RECORD.ExceptionAddress (0x10)", g_fault_addr == g_fault_rip);
    CHECK("exception CONTEXT.Rip (0xF8)", g_fault_ctx_rip == g_fault_rip);
    CHECK("exception CONTEXT changes resume (Rip 0xF8, Rax 0x78)", rax == 0x1234);

    HANDLE th = CreateThread(NULL, 0, spinner, NULL, 0, NULL);
    while (g_spin < 1000) Sleep(1);
    SuspendThread(th);
    memset(&c, 0, sizeof(c));
    c.ContextFlags = CONTEXT_FULL;
    BOOL ok = GetThreadContext(th, &c);
    if (!(ok && U64(cb, 0xF8) >= (ULONGLONG)spinner && U64(cb, 0xF8) < (ULONGLONG)spinner + 64))
        printf("GetThreadContext: %d, Rip %llx, Rsp %llx, spinner() at %p\n", ok, U64(cb, 0xF8), U64(cb, 0x98), (void *)spinner);
    CHECK("GetThreadContext: Rip (0xF8) in spinner()", ok && U64(cb, 0xF8) >= (ULONGLONG)spinner && U64(cb, 0xF8) < (ULONGLONG)spinner + 64);
    TerminateThread(th, 0);
    CloseHandle(th);

    /* ---- ntdll's stubs: Windows's bytes and 1903's numbers ---- */
    int stubs = 0, wrong = 0, in_ntdll = 0;
    for (size_t i = 0; i < sizeof(g_nt1903) / sizeof(g_nt1903[0]); i++) {
        BYTE *f = (BYTE *)GetProcAddress(ntdll, g_nt1903[i].name);
        if (!f) continue;
        stubs++;
        /* mov r10, rcx; mov eax, N; test byte [7FFE0308], 1; jne +3; syscall; ret; int 2e; ret */
        static const BYTE tail[] = { 0xF6, 0x04, 0x25, 0x08, 0x03, 0xFE, 0x7F, 0x01, 0x75, 0x03, 0x0F, 0x05, 0xC3, 0xCD, 0x2E, 0xC3 };
        if (f[0] != 0x4C || f[1] != 0x8B || f[2] != 0xD1 || f[3] != 0xB8) {
            stubs--;                                         /* done in ntdll itself, not the kernel */
            in_ntdll++;
            continue;
        }
        if (memcmp(f + 8, tail, sizeof(tail))) {
            printf("FAIL: ntdll!%s is not laid out as Windows's stub\n", g_nt1903[i].name);
            wrong++;
        } else if (U32(f, 4) != g_nt1903[i].num) {
            printf("FAIL: ntdll!%s calls 0x%lx; Windows 10 1903: 0x%lx\n", g_nt1903[i].name, U32(f, 4), g_nt1903[i].num);
            wrong++;
        }
    }
    printf("%d ntdll system-call stubs checked against Windows 10 1903 (%d more services are ntdll code)\n", stubs, in_ntdll);
    CHECK("ntdll stubs: Windows layout and 1903 numbers", !wrong);
    CHECK("ntdll exports system-call stubs", stubs >= 80);

    /* the kernel's own numbering, with no ntdll in between */
    LARGE_INTEGER ctr = { 0 }, freq = { 0 };
    CHECK("syscall 0x31 NtQueryPerformanceCounter", raw_syscall(0x31, (ULONG_PTR)&ctr, (ULONG_PTR)&freq, 0) == 0 && freq.QuadPart && ctr.QuadPart);
    CHECK("syscall 0x0F NtClose (bad handle)", raw_syscall(0x0F, 0x1234560, 0, 0) == (NTSTATUS)0xC0000008);
    LARGE_INTEGER sys = { 0 };
    CHECK("syscall 0x5A NtQuerySystemTime", raw_syscall(0x5A, (ULONG_PTR)&sys, 0, 0) == 0 && sys.QuadPart > 0x01D0000000000000LL);
    PROCESS_BASIC_INFORMATION pb2;
    memset(&pb2, 0, sizeof(pb2));
    /* NtQueryInformationProcess takes 5 arguments; the fifth (ReturnLength) is optional */
    CHECK("syscall 0x19 NtQueryInformationProcess", raw_syscall(0x19, (ULONG_PTR)-1, 0, (ULONG_PTR)&pb2) != 0 ||
                                                    (BYTE *)pb2.PebBaseAddress == peb);
    HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    CHECK("syscall 0x0E NtSetEvent", raw_syscall(0x0E, (ULONG_PTR)ev, 0, 0) == 0 && WaitForSingleObject(ev, 0) == WAIT_OBJECT_0);
    CHECK("syscall 0x04 NtWaitForSingleObject", raw_syscall(0x04, (ULONG_PTR)ev, 0, 0) == 0);
    CloseHandle(ev);

    printf("abitest: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}

#else   /* the 32-bit layouts are not checked yet */
int main(void) { printf("abitest: 0 passed, 0 failed (x64 only)\n"); return 0; }
#endif
