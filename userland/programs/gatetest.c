/* gatetest.exe — the x64 segment layout and the self-inspection calls
 * Windows gives a 64-bit program, the way anti-cheat code (Roblox's
 * Hyperion) leans on them:
 *
 *   - the selectors are Windows' own: CS 0x33, SS/DS/ES/GS 0x2B, FS 0x53;
 *   - a far jump to 0x23 runs 32-bit code (compatibility mode) inside a
 *     64-bit program, and a far jump to 0x33 comes back ("heaven's gate"
 *     in reverse: Hyperion runs CPUID at 0xFFFFFFFE this way, so EIP wraps
 *     to 0 and faults);
 *   - a fault in that 32-bit code reaches the program's handlers with
 *     SegCs 0x23 in its CONTEXT, and NtContinue with SegCs 0x33 resumes in
 *     64-bit code;
 *   - GetThreadContext on the calling thread works (the registers its
 *     system call left with, debug registers clear) and SetThreadContext
 *     on it resumes at the CONTEXT;
 *   - __fastfail (int 0x29) ends the program at once with
 *     STATUS_STACK_BUFFER_OVERRUN; no handler of its own runs.
 *
 * All faithful to Windows, nothing bypassed.
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

/* (the userland headers are thin) */
WINBASEAPI VOID WINAPI RtlCaptureContext(PCONTEXT);

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

#ifdef _WIN64

static unsigned short sel_cs(void) { unsigned short v; __asm__ volatile ("mov %%cs, %0" : "=r"(v)); return v; }
static unsigned short sel_ss(void) { unsigned short v; __asm__ volatile ("mov %%ss, %0" : "=r"(v)); return v; }
static unsigned short sel_ds(void) { unsigned short v; __asm__ volatile ("mov %%ds, %0" : "=r"(v)); return v; }
static unsigned short sel_es(void) { unsigned short v; __asm__ volatile ("mov %%es, %0" : "=r"(v)); return v; }
static unsigned short sel_fs(void) { unsigned short v; __asm__ volatile ("mov %%fs, %0" : "=r"(v)); return v; }
static unsigned short sel_gs(void) { unsigned short v; __asm__ volatile ("mov %%gs, %0" : "=r"(v)); return v; }

/* The gate, built in memory below 4 GiB (32-bit code can only run there):
 *
 *   +0x00  push rbx; mov rbx, rsp           64-bit: keep the stack
 *   +0x04  lea rsp, [rip + STACK]           a stack below 4 GiB
 *   +0x0B  jmp far dword [rip + FARPTR]     to 0x23:target
 *   +0x11  mov rsp, rbx; pop rbx; ret       BACK: 64-bit again
 *   +0x40  mov eax, 0x12345678; dec eax     32-bit code ("dec eax" is a
 *          jmp far 0x33:BACK                REX prefix in 64-bit code)
 *   +0x80  FARPTR: offset (4 bytes), selector (2 bytes)
 */
#define G_BACK   0x11
#define G_CODE32 0x40
#define G_FARPTR 0x80
#define G_SIZE   0x8000
static BYTE *g_gate;

static void put32(BYTE *p, DWORD v) { memcpy(p, &v, 4); }

static int build_gate(void)
{
    for (ULONG_PTR a = 0x20000000; a < 0x80000000 && !g_gate; a += 0x1000000)
        g_gate = VirtualAlloc((void *)a, G_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!g_gate) return 0;
    BYTE *g = g_gate;
    static const BYTE head[] = { 0x53, 0x48, 0x89, 0xE3,             /* push rbx; mov rbx, rsp */
                                 0x48, 0x8D, 0x25, 0, 0, 0, 0,       /* lea rsp, [rip + x] */
                                 0xFF, 0x2D, 0, 0, 0, 0,             /* jmp far dword [rip + x] */
                                 0x48, 0x89, 0xDC, 0x5B, 0xC3 };     /* mov rsp, rbx; pop rbx; ret */
    memcpy(g, head, sizeof(head));
    put32(g + 0x07, (DWORD)(G_SIZE - 0x100 - 0x0B));                 /* stack: near the end */
    put32(g + 0x0D, (DWORD)(G_FARPTR - 0x11));
    BYTE *c = g + G_CODE32;
    c[0] = 0xB8; put32(c + 1, 0x12345678);                           /* mov eax, 0x12345678 */
    c[5] = 0x48;                                                     /* dec eax */
    c[6] = 0xEA; put32(c + 7, (DWORD)(ULONG_PTR)(g + G_BACK));       /* jmp far 0x33:BACK */
    c[11] = 0x33; c[12] = 0x00;
    return 1;
}

static void aim_gate(DWORD offset, WORD selector)
{
    put32(g_gate + G_FARPTR, offset);
    memcpy(g_gate + G_FARPTR + 4, &selector, 2);
}

/* A fault in the 32-bit code: send it back through BACK in 64-bit mode */
static volatile LONG g_seen_cs, g_seen_kind, g_seen_hits;
static volatile ULONG_PTR g_seen_addr;
static LONG CALLBACK gate_veh(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode != (DWORD)EXCEPTION_ACCESS_VIOLATION || !g_gate)
        return EXCEPTION_CONTINUE_SEARCH;
    g_seen_hits++;
    g_seen_addr = (ULONG_PTR)ep->ExceptionRecord->ExceptionAddress;
    g_seen_kind = (LONG)ep->ExceptionRecord->ExceptionInformation[0];
    g_seen_cs = ep->ContextRecord->SegCs;
    ep->ContextRecord->Rip = (DWORD64)(ULONG_PTR)(g_gate + G_BACK);
    ep->ContextRecord->SegCs = 0x33;
    return EXCEPTION_CONTINUE_EXECUTION;
}

/* NtGetContextThread on the calling thread with RBX and R12 set: they come
 * back as the system call left them */
typedef LONG (WINAPI *GetCtxFn)(HANDLE, PCONTEXT);
static GetCtxFn g_getctx;
static DWORD64 g_rbx_seen, g_r12_seen;
__attribute__((naked)) static LONG getctx_with_regs(PCONTEXT c)
{
    __asm__("push %rbx\n\t"
            "push %r12\n\t"
            "sub $0x28, %rsp\n\t"
            "mov %rcx, %rdx\n\t"
            "mov $-2, %rcx\n\t"
            "movabs $0x1111222233334444, %rbx\n\t"
            "movabs $0x5555666677778888, %r12\n\t"
            "call *g_getctx(%rip)\n\t"
            "add $0x28, %rsp\n\t"
            "pop %r12\n\t"
            "pop %rbx\n\t"
            "ret");
}

static void fastfail_child(void)
{
    AddVectoredExceptionHandler(1, gate_veh);                        /* must not run */
    __asm__ volatile ("mov $7, %ecx\n\tint $0x29");
    ExitProcess(1);                                                  /* never reached */
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "fastfail")) { fastfail_child(); return 1; }

    /* Windows' selectors */
    CHECK("CS is 0x33", sel_cs() == 0x33);
    CHECK("SS is 0x2B", sel_ss() == 0x2B);
    CHECK("DS and ES are 0x2B", sel_ds() == 0x2B && sel_es() == 0x2B);
    CHECK("FS is 0x53", sel_fs() == 0x53);
    CHECK("GS is 0x2B", sel_gs() == 0x2B);
    CONTEXT cc;
    memset(&cc, 0, sizeof(cc));
    RtlCaptureContext(&cc);
    CHECK("RtlCaptureContext: SegCs 0x33, SegSs 0x2B", cc.SegCs == 0x33 && cc.SegSs == 0x2B);

    /* 32-bit code through a far jump to 0x23, back through 0x33 */
    CHECK("gate memory below 4 GiB", build_gate());
    if (g_gate) {
        aim_gate((DWORD)(ULONG_PTR)(g_gate + G_CODE32), 0x23);
        DWORD r = ((DWORD (*)(void))g_gate)();
        CHECK("far jump to 0x23 ran 32-bit code and came back", r == 0x12345677);

        /* EIP wraps at 4 GiB in 32-bit code: two NOPs at 0xFFFFFFFE, then
         * the fetch at 0 faults; the handler sees SegCs 0x23 and resumes in
         * 64-bit code with NtContinue */
        BYTE *top = VirtualAlloc((void *)0xFFFF0000, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        CHECK("memory at the top of 4 GiB", top == (BYTE *)0xFFFF0000);
        if (top) {
            top[0xFFFE] = 0x90; top[0xFFFF] = 0x90;
            PVOID veh = AddVectoredExceptionHandler(1, gate_veh);
            aim_gate(0xFFFFFFFE, 0x23);
            ((DWORD (*)(void))g_gate)();
            RemoveVectoredExceptionHandler(veh);
            CHECK("EIP wrapped to 0 and faulted once", g_seen_hits == 1 && g_seen_addr == 0);
            CHECK("the fault was an instruction fetch", g_seen_kind == 8);
            CHECK("the CONTEXT says 32-bit code (SegCs 0x23)", g_seen_cs == 0x23);
            CHECK("back in 64-bit code after it", sel_cs() == 0x33);
            VirtualFree(top, 0, MEM_RELEASE);
        }
    }

    /* GetThreadContext on the calling thread */
    g_getctx = (GetCtxFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtGetContextThread");
    static CONTEXT gc;
    memset(&gc, 0, sizeof(gc));
    gc.ContextFlags = CONTEXT_FULL | CONTEXT_DEBUG_REGISTERS;
    LONG s = g_getctx ? getctx_with_regs(&gc) : -1;
    CHECK("NtGetContextThread on itself succeeds", s == 0);
    CHECK("its RBX and R12 as the call left them",
          gc.Rbx == 0x1111222233334444ull && gc.R12 == 0x5555666677778888ull);
    CHECK("its RIP in ntdll's NtGetContextThread",
          gc.Rip >= (DWORD64)(ULONG_PTR)g_getctx && gc.Rip < (DWORD64)(ULONG_PTR)g_getctx + 32);
    CHECK("its RSP on this stack", gc.Rsp < (DWORD64)(ULONG_PTR)&s && gc.Rsp + 0x1000 > (DWORD64)(ULONG_PTR)&s);
    CHECK("SegCs 0x33, no hardware breakpoints", gc.SegCs == 0x33 && gc.Dr7 == 0 && gc.Dr0 == 0);
    CHECK("ContextFlags kept", gc.ContextFlags == (CONTEXT_FULL | CONTEXT_DEBUG_REGISTERS));
    CONTEXT k32;
    memset(&k32, 0, sizeof(k32));
    k32.ContextFlags = CONTEXT_CONTROL;
    CHECK("kernel32 GetThreadContext(GetCurrentThread())", GetThreadContext(GetCurrentThread(), &k32) && k32.Rip);

    /* SetThreadContext on the calling thread resumes at the CONTEXT */
    static CONTEXT jc;
    static volatile int rounds;
    memset(&jc, 0, sizeof(jc));
    RtlCaptureContext(&jc);
    rounds++;
    if (rounds == 1) {
        jc.ContextFlags = CONTEXT_FULL;
        SetThreadContext(GetCurrentThread(), &jc);
        CHECK("SetThreadContext on itself did not return", 0);
    }
    CHECK("SetThreadContext on itself resumed at the CONTEXT", rounds == 2);

    /* __fastfail ends the program with STATUS_STACK_BUFFER_OVERRUN */
    char exe[MAX_PATH], cmd[MAX_PATH + 16];
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    snprintf(cmd, sizeof(cmd), "\"%s\" fastfail", exe);
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    DWORD code = 0;
    if (CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 30000);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    CHECK("__fastfail ends the program with 0xC0000409", code == 0xC0000409u);

    printf("gatetest: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}

#else
int main(void) { printf("gatetest: 0 passed, 0 failed (x64 only)\n"); return 0; }
#endif
