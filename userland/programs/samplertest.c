/*
 * samplertest.exe — what a sampling profiler needs from GetThreadContext
 * (Chromium's GPU process runs one from its start: --start-stack-profiler)
 *
 * The profiler suspends a thread, reads its registers with
 * GetThreadContext and unwinds its stack with RtlLookupFunctionEntry and
 * RtlVirtualUnwind.  A thread is suspended most often while it waits, in
 * a system call, and its context must then hold every nonvolatile
 * register as the thread left them, Rbp above all: a function that uses
 * Rbp as its frame pointer (one with alloca, as the Vulkan loader's) is
 * unwound from it.  A zero Rbp sent the unwinder to address -0x10, and the
 * access violation in Steam's browser's GPU process ended it.
 *
 * Here a thread sets r12-r15 to known values and waits in
 * NtWaitForSingleObject from inside a function with an Rbp frame; the
 * main thread suspends it, reads its context and unwinds it back to the
 * function that called the frame function, as the profiler does.
 */
#include <windows.h>
#include <stdio.h>
#include <malloc.h>
#include <string.h>

static int g_pass, g_fail;

static void check(int ok, const char *what)
{
    if (ok) g_pass++;
    else { g_fail++; printf("FAIL: %s\n", what); }
}

#define MAGIC(n) (0x5A3D000000000000ULL | (n))

typedef LONG (WINAPI *WaitFn)(HANDLE, BOOLEAN, PLARGE_INTEGER);
typedef PRUNTIME_FUNCTION (WINAPI *LookupFn)(DWORD64, PDWORD64, PVOID);
typedef PVOID (WINAPI *UnwindFn)(ULONG, DWORD64, DWORD64, PRUNTIME_FUNCTION, PCONTEXT, PVOID *, PDWORD64, PVOID);
static WaitFn g_wait;
static HANDLE g_event;
static volatile LONG g_parked;
static volatile ULONG_PTR g_local;          /* an address on the waiting thread's stack */
static volatile int g_sink;

/* Wait in the system call with r12-r15 holding MAGIC(12..15), from the
 * caller's own frame (its unwind data knows nothing of the 40 bytes
 * taken here, so the caller must be one unwound from Rbp) */
static __forceinline void park(void)
{
    __asm__ volatile (
        "movabsq %[m12], %%r12\n\t"
        "movabsq %[m13], %%r13\n\t"
        "movabsq %[m14], %%r14\n\t"
        "movabsq %[m15], %%r15\n\t"
        "subq $40, %%rsp\n\t"
        "movq %[ev], %%rcx\n\t"
        "xorl %%edx, %%edx\n\t"
        "xorl %%r8d, %%r8d\n\t"
        "callq *%[fn]\n\t"
        "addq $40, %%rsp\n\t"
        :
        : [m12] "i"(MAGIC(12)), [m13] "i"(MAGIC(13)), [m14] "i"(MAGIC(14)), [m15] "i"(MAGIC(15)),
          [ev] "m"(g_event), [fn] "m"(g_wait)
        : "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "memory", "cc");
}

/* A function whose frame is found from Rbp (alloca makes it set one) */
static __declspec(noinline) void frame_fn(int n)
{
    volatile char *buf = _alloca(n);
    buf[0] = 1;
    g_local = (ULONG_PTR)buf;
    InterlockedExchange(&g_parked, 1);
    park();
    g_sink = buf[0];
}

static __declspec(noinline) DWORD WINAPI waiter(LPVOID arg)
{
    frame_fn((int)(ULONG_PTR)arg);
    g_sink++;
    return 0;
}

int main(void)
{
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    g_wait = (WaitFn)GetProcAddress(nt, "NtWaitForSingleObject");
    LookupFn lookup = (LookupFn)GetProcAddress(nt, "RtlLookupFunctionEntry");
    UnwindFn unwind = (UnwindFn)GetProcAddress(nt, "RtlVirtualUnwind");
    g_event = CreateEventA(0, TRUE, FALSE, 0);
    check(g_wait && lookup && unwind && g_event, "ntdll's NtWaitForSingleObject, RtlLookupFunctionEntry, RtlVirtualUnwind; an event");
    if (!g_wait || !lookup || !unwind || !g_event) { printf("samplertest: %d passed, %d failed\n", g_pass, g_fail); return 1; }

    HANDLE th = CreateThread(0, 0, waiter, (LPVOID)(ULONG_PTR)200, 0, 0);
    while (!g_parked) Sleep(10);
    Sleep(200);                                     /* in the system call by now */
    check(SuspendThread(th) == 0, "SuspendThread");

    CONTEXT c;
    memset(&c, 0, sizeof(c));
    c.ContextFlags = CONTEXT_FULL;
    check(GetThreadContext(th, &c), "GetThreadContext of a thread waiting in a system call");
    check(c.R12 == MAGIC(12) && c.R13 == MAGIC(13) && c.R14 == MAGIC(14) && c.R15 == MAGIC(15),
          "its r12-r15 are the values it waits with");
    check(c.Rbp && c.Rbp > c.Rsp && c.Rbp - g_local < 0x1000, "its Rbp is its frame function's frame pointer");
    check(c.Rsp && c.Rsp < g_local, "its Rsp is below the frame function's locals");

    /* unwind it as the profiler does: up to waiter(), which called frame_fn() */
    int reached = 0, frames = 0;
    __try {
        for (; frames < 16 && c.Rip && !reached; frames++) {
            DWORD64 base = 0;
            PRUNTIME_FUNCTION f = lookup(c.Rip, &base, 0);
            if (!f) {                               /* a leaf (the system call stub) */
                c.Rip = *(DWORD64 *)c.Rsp;
                c.Rsp += 8;
            } else {
                PVOID hd;
                DWORD64 est;
                unwind(0, base, c.Rip, f, &c, &hd, &est, 0);
            }
            DWORD64 wb = 0;
            PRUNTIME_FUNCTION wf = lookup((DWORD64)(ULONG_PTR)waiter, &wb, 0);
            if (wf && c.Rip >= wb + wf->BeginAddress && c.Rip < wb + wf->EndAddress) reached = 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        printf("samplertest: unwinding faulted (exception %08lx)\n", GetExceptionCode());
    }
    check(reached, "the unwind from its context reaches the caller of the frame function");

    check(ResumeThread(th) == 1, "ResumeThread");
    SetEvent(g_event);
    check(WaitForSingleObject(th, 5000) == WAIT_OBJECT_0, "the thread goes on and ends");
    printf("samplertest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
