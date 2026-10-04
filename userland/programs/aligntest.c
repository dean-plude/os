/* aligntest.exe — the general-protection faults Windows looks into before
 * it reports them, the way anti-cheat code (Roblox's Hyperion) leans on:
 *
 *   - with alignment-fault fixup on (SetErrorMode SEM_NOALIGNMENTFAULTEXCEPT,
 *     or NtSetInformationThread ThreadEnableAlignmentFaultFixup), a
 *     16-byte SSE access to unaligned memory does not raise: the kernel
 *     rewrites MOVDQA to MOVDQU (and MOVAPS to MOVUPS) in the program's
 *     own code and runs it again;
 *   - a misaligned move with fixup off is a normal access violation,
 *     seen here through a vectored handler that turns fixup on so the
 *     retried instruction goes through (no __try: clang's -fasync-exceptions
 *     __try around inline asm hangs the compiler, as fpstate.c notes);
 *   - ThreadHideFromDebugger accepts no data and reads back the flag;
 *   - ntdll's extended-context functions describe the processor's state.
 *
 * All faithful to Windows, nothing bypassed.
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <winternl.h>

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

#ifdef _WIN64

/* A 16-byte store with MOVDQA to [rcx]; faults with #GP if rcx is not
 * 16-byte aligned.  Written so the 0x66 prefix the kernel's fixup
 * rewrites is findable. */
__attribute__((naked)) static void movdqa_store(void *dst)
{
    __asm__("pxor %xmm0, %xmm0\n\t"
            "movdqa %xmm0, (%rcx)\n\t"
            "retq");
}

/* MOVDQA load from [rcx] into xmm0, then its low dword to eax */
__attribute__((naked)) static unsigned movdqa_load(const void *src)
{
    __asm__("movdqa (%rcx), %xmm0\n\t"
            "movd %xmm0, %eax\n\t"
            "retq");
}

/* A second MOVDQA load, at its own code address, for the per-thread test
 * (the first was rewritten in place by the process-wide fixup) */
__attribute__((naked)) static unsigned movdqa_load2(const void *src)
{
    __asm__("movdqa (%rcx), %xmm1\n\t"
            "movd %xmm1, %eax\n\t"
            "retq");
}

/* Turn alignment-fault fixup on when a misaligned SSE move raises, then
 * let the instruction run again (Windows' kernel would have fixed it up
 * had fixup been on; this proves it was an access violation without it) */
static volatile LONG g_av;
static LONG CALLBACK av_veh(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode != (DWORD)EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    g_av = 1;
    SetErrorMode(0x0004 /* SEM_NOALIGNMENTFAULTEXCEPT */);
    return EXCEPTION_CONTINUE_EXECUTION;                   /* retry; now the kernel fixes it up */
}

int main(void)
{
    /* 16 bytes, deliberately 8-aligned but not 16 */
    static __declspec(align(16)) BYTE buf[48];
    BYTE *unaligned = buf + 8;
    for (int i = 0; i < 16; i++) unaligned[i] = (BYTE)(0x40 + i);

    /* Fixup off: a misaligned MOVDQA raises an access violation.  The
     * handler records it, turns fixup on, and the retried instruction
     * reads the bytes. */
    SetErrorMode(0);
    PVOID veh = AddVectoredExceptionHandler(1, av_veh);
    unsigned v = movdqa_load(unaligned);
    RemoveVectoredExceptionHandler(veh);
    CHECK("misaligned MOVDQA without fixup raised an access violation", g_av == 1);
    CHECK("misaligned MOVDQA load fixed up once fixup is on", v == 0x43424140u);

    /* A store, with fixup on: it must land in memory. */
    for (int i = 0; i < 16; i++) unaligned[i] = 0xAA;
    movdqa_store(unaligned);
    int zeroed = 1;
    for (int i = 0; i < 16; i++) if (unaligned[i]) zeroed = 0;
    CHECK("misaligned MOVDQA store fixed up", zeroed);

    /* The fixup rewrote the 0x66 prefix to 0xF3 (MOVDQU) in the code, so a
     * second call does not fault even though nothing re-decodes it. */
    for (int i = 0; i < 16; i++) unaligned[i] = (BYTE)i;
    CHECK("MOVDQA rewritten to MOVDQU in place", movdqa_load(unaligned) == 0x03020100u);

    /* Per-thread fixup, through NtSetInformationThread, on a fresh
     * instruction (movdqa_load2, not yet patched). */
    SetErrorMode(0);
    BOOLEAN on = TRUE;
    NtSetInformationThread(NtCurrentThread(), 7 /* ThreadEnableAlignmentFaultFixup */, &on, sizeof(on));
    for (int i = 0; i < 16; i++) unaligned[i] = (BYTE)(0x10 + i);
    CHECK("per-thread fixup reads unaligned", movdqa_load2(unaligned) == 0x13121110u);

    /* ThreadHideFromDebugger: a non-zero length is STATUS_INFO_LENGTH_MISMATCH;
     * zero length sets it; a BOOLEAN query reads it back. */
    BYTE one = 1;
    CHECK("ThreadHideFromDebugger with data -> length mismatch",
          NtSetInformationThread(NtCurrentThread(), 17, &one, 1) == (NTSTATUS)0xC0000004);
    CHECK("ThreadHideFromDebugger set",
          NtSetInformationThread(NtCurrentThread(), 17, NULL, 0) == 0);
    BYTE hidden = 0;
    ULONG rl = 0;
    NTSTATUS qs = NtQueryInformationThread(NtCurrentThread(), 17, &hidden, 1, &rl);
    CHECK("ThreadHideFromDebugger read back", qs == 0 && hidden == 1 && rl == 1);

    /* ntdll's extended-context functions (kernel32's InitializeContext and
     * Hyperion use them): a CONTEXT_XSTATE buffer initialises and its
     * legacy CONTEXT is reachable. */
    typedef NTSTATUS (NTAPI *LenFn)(ULONG, PULONG);
    typedef NTSTATUS (NTAPI *InitFn)(PVOID, ULONG, PVOID *);
    typedef PCONTEXT (NTAPI *LegFn)(PVOID, PULONG);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    LenFn getlen = (LenFn)GetProcAddress(nt, "RtlGetExtendedContextLength");
    InitFn init = (InitFn)GetProcAddress(nt, "RtlInitializeExtendedContext");
    LegFn legacy = (LegFn)GetProcAddress(nt, "RtlLocateLegacyContext");
    CHECK("ntdll extended-context functions present", getlen && init && legacy);
    if (getlen && init && legacy) {
        ULONG len = 0;
        ULONG flags = 0x100000 | 0x1 | 0x40;               /* CONTEXT_AMD64 | CONTROL | XSTATE */
        NTSTATUS s = getlen(flags, &len);
        CHECK("RtlGetExtendedContextLength", s == 0 && len >= sizeof(CONTEXT));
        void *mem = HeapAlloc(GetProcessHeap(), 0, len);
        PVOID ex = NULL;
        s = init(mem, flags, &ex);
        CHECK("RtlInitializeExtendedContext", s == 0 && ex);
        if (ex) {
            PCONTEXT c = legacy(ex, NULL);
            CHECK("RtlLocateLegacyContext", c && (c->ContextFlags & 0x100000));
        }
        HeapFree(GetProcessHeap(), 0, mem);
    }

    printf("aligntest: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}

#else
int main(void) { printf("aligntest: 0 passed, 0 failed (x64 only)\n"); return 0; }
#endif
