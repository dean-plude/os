/* fpstate.exe — the floating-point state a thread runs with: the x87
 * control word (0x27F) and MXCSR (0x1F80, every exception masked) that a
 * new thread starts with on Windows, kept across context switches, put
 * back by RtlRestoreContext/NtContinue (MXCSR from CONTEXT.MxCsr, the x87
 * state from FltSave, as RtlCaptureContext saved them) and after an SEH
 * unwind; inexact arithmetic never faults with the default masks; and an
 * x87 exception the program unmasked arrives with its own status code.
 * Under TCG an unmasked SSE exception never traps, so a wrong MXCSR only
 * showed under KVM, where Audacity died with STATUS_FLOAT_INEXACT_RESULT
 * after a C++ exception was caught. */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <winternl.h>

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

static unsigned get_mxcsr(void) { unsigned v; __asm__ volatile("stmxcsr %0" : "=m"(v)); return v; }
static void set_mxcsr(unsigned v) { __asm__ volatile("ldmxcsr %0" : : "m"(v)); }
static unsigned get_fcw(void) { unsigned short v; __asm__ volatile("fnstcw %0" : "=m"(v)); return v; }
static void set_fcw(unsigned v) { unsigned short w = (unsigned short)v; __asm__ volatile("fldcw %0" : : "m"(w)); }
static unsigned get_fsw(void) { unsigned short v; __asm__ volatile("fnstsw %0" : "=m"(v)); return v; }

/* x87 division (and an fwait, where a pending exception is delivered) */
static double x87_div(double a, double b)
{
    double r;
    __asm__ volatile("fldl %1\n\tfdivl %2\n\tfstpl %0\n\tfwait" : "=m"(r) : "m"(a), "m"(b));
    return r;
}

static volatile double g_one = 1.0, g_three = 3.0, g_zero = 0.0;

typedef struct { unsigned fcw, mxcsr; } State;

static DWORD WINAPI read_state(void *arg)
{
    State *s = arg;
    s->fcw = get_fcw();
    s->mxcsr = get_mxcsr();
    return 0;
}

static volatile LONG g_stop;
static State g_spin;
static DWORD WINAPI spinner(void *arg)
{
    (void)arg;
    set_mxcsr(0x1F80 | 0x6000);                     /* round toward zero in this thread only */
    set_fcw(0x37F);
    volatile double x = 1.0;
    while (!g_stop) x = x / 3.0 + 1.0;
    g_spin.mxcsr = get_mxcsr();
    g_spin.fcw = get_fcw();
    return (g_spin.mxcsr & ~0x3Fu) != (0x1F80 | 0x6000) || g_spin.fcw != 0x37F;
}

static int seh_filter(DWORD code, DWORD *seen) { *seen = code; return EXCEPTION_EXECUTE_HANDLER; }

int main(void)
{
    /* the state a program starts with */
    CHECK("main thread x87 control word 0x27F", get_fcw() == 0x27F);
    CHECK("main thread MXCSR 0x1F80", (get_mxcsr() & ~0x3Fu) == 0x1F80);

    /* a new thread starts with the defaults, whatever its creator runs with */
    set_mxcsr(0x1F80 | 0x2000);                     /* round down here */
    set_fcw(0x37F);
    State s = { 0, 0 };
    HANDLE t = CreateThread(NULL, 0, read_state, &s, 0, NULL);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    CHECK("new thread x87 control word 0x27F", s.fcw == 0x27F);
    CHECK("new thread MXCSR 0x1F80", (s.mxcsr & ~0x3Fu) == 0x1F80);

    /* context switches keep each thread's own state */
    t = CreateThread(NULL, 0, spinner, NULL, 0, NULL);
    for (int i = 0; i < 20; i++) Sleep(5);
    CHECK("MXCSR kept across switches", (get_mxcsr() & ~0x3Fu) == (0x1F80 | 0x2000));
    CHECK("x87 control word kept across switches", get_fcw() == 0x37F);
    g_stop = 1;
    WaitForSingleObject(t, INFINITE);
    DWORD rc = 1;
    GetExitCodeThread(t, &rc);
    CloseHandle(t);
    CHECK("other thread kept its own state", rc == 0);
    if (rc) printf("fpstate: other thread ended with x87 control word %#x, MXCSR %#x\n", g_spin.fcw, g_spin.mxcsr);
    set_mxcsr(0x1F80);
    set_fcw(0x27F);

#ifdef _WIN64
    /* RtlCaptureContext saves the FPU state and RtlRestoreContext puts it
     * back, into a CONTEXT that started out zeroed (a stack buffer; the
     * 32-bit RtlCaptureContext leaves the FPU out, as on Windows) */
    static CONTEXT c;
    volatile int round = 0;
    memset(&c, 0, sizeof(c));
    RtlCaptureContext(&c);
    if (round == 0) {
        round = 1;
        set_mxcsr(0x1F80 | 0x4000);                 /* round up; the restore undoes it */
        set_fcw(0x7F);
        RtlRestoreContext(&c, NULL);
    }
    CHECK("RtlRestoreContext: MXCSR back to 0x1F80", (get_mxcsr() & ~0x3Fu) == 0x1F80);
    CHECK("RtlRestoreContext: x87 control word back to 0x27F", get_fcw() == 0x27F);
    CHECK("RtlCaptureContext: FltSave.ControlWord", c.FltSave.ControlWord == 0x27F);
    CHECK("RtlCaptureContext: FltSave.MxCsr", (c.FltSave.MxCsr & ~0x3Fu) == 0x1F80);

    /* NtContinue takes MXCSR from CONTEXT.MxCsr (FltSave.MxCsr is ignored) */
    memset(&c, 0, sizeof(c));
    round = 0;
    RtlCaptureContext(&c);
    if (round == 0) {
        round = 1;
        c.MxCsr = 0x1F80 | 0x2000;
        RtlRestoreContext(&c, NULL);
    }
    CHECK("NtContinue: MXCSR from CONTEXT.MxCsr", (get_mxcsr() & ~0x3Fu) == (0x1F80 | 0x2000));
    set_mxcsr(0x1F80);
#endif

    /* an SEH unwind resumes with the state the program had */
    DWORD seen = 0;
    __try {
        RaiseException(0xE0004650, 0, 0, NULL);
    } __except (seh_filter(GetExceptionCode(), &seen)) {
        CHECK("SEH: caught", seen == 0xE0004650);
    }
    CHECK("after SEH: MXCSR 0x1F80", (get_mxcsr() & ~0x3Fu) == 0x1F80);
    CHECK("after SEH: x87 control word 0x27F", get_fcw() == 0x27F);

    /* inexact results with every exception masked: no fault, just the flag */
    set_mxcsr(0x1F80);
    volatile double q = g_one / g_three;
    CHECK("SSE 1/3", q > 0.33 && q < 0.34);
    CHECK("SSE inexact flag set", get_mxcsr() & 0x20);
    __asm__ volatile("fnclex");
    q = x87_div(g_one, g_three);
    CHECK("x87 1/3", q > 0.33 && q < 0.34);
    CHECK("x87 inexact flag set", get_fsw() & 0x20);
    q = g_one / g_zero;
    CHECK("SSE 1/0 masked gives infinity", q > 1e308);

    /* an x87 divide by zero the program unmasked: FLT_DIVIDE_BY_ZERO */
    seen = 0;
    __asm__ volatile("fnclex");
    __try {
#ifdef _WIN64
        /* in line: clang gives x87_div no unwind data, which would hide the __try */
        double a = g_one, b = g_zero, r;
        unsigned short cw = 0x27F & ~0x4;           /* unmask ZE */
        __asm__ volatile("fldcw %3\n\tfldl %1\n\tfdivl %2\n\tfstpl %0\n\tfwait" : "=m"(r) : "m"(a), "m"(b), "m"(cw));
        q = r;
#else
        set_fcw(0x27F & ~0x4);                      /* unmask ZE */
        q = x87_div(g_one, g_zero);                 /* a call: clang's 32-bit __try covers calls */
#endif
    } __except (seh_filter(GetExceptionCode(), &seen)) {
    }
    __asm__ volatile("fnclex");
    set_fcw(0x27F);
    CHECK("x87 divide by zero: EXCEPTION_FLT_DIVIDE_BY_ZERO", seen == EXCEPTION_FLT_DIVIDE_BY_ZERO);

    /* an SSE divide by zero the program unmasked (traps under KVM and on
     * real hardware; QEMU's TCG never raises SSE exceptions) */
    seen = 0;
    __try {
        set_mxcsr(0x1F80 & ~0x200);                 /* unmask ZM */
        q = g_one / g_zero;
    } __except (seh_filter(GetExceptionCode(), &seen)) {
    }
    set_mxcsr(0x1F80);
    CHECK("SSE divide by zero: EXCEPTION_FLT_DIVIDE_BY_ZERO or no trap", seen == 0 || seen == EXCEPTION_FLT_DIVIDE_BY_ZERO);
    printf("fpstate: SSE exceptions %s\n", seen ? "trap" : "do not trap (TCG)");

    printf("fpstate: %d passed, %d failed\n", pass, fail);
    return fail;
}
