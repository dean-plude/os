/*
 * threads.c — kernel32 threads, synchronization, TLS, dynamic loading and
 * the exception helpers, on top of ntdll's native API.
 */

#define NOVA_BUILD_KERNEL32
#include <winternl.h>
#include <winnt.h>

void *memset(void *d, int c, size_t n);
size_t strlen(const char *s);

static BYTE *teb(void) { BYTE *t; __asm__("movq %%gs:0x30, %0" : "=r"(t)); return t; }
static void set_error(NTSTATUS s) { *(DWORD *)(teb() + TEB_LAST_ERROR) = RtlNtStatusToDosError(s); }

/* Relative timeout in 100 ns units (NULL = INFINITE) into a LARGE_INTEGER */
static PLARGE_INTEGER ms_timeout(LARGE_INTEGER *li, DWORD ms)
{
    if (ms == INFINITE) return 0;
    li->QuadPart = -((LONGLONG)ms * 10000);
    return li;
}

static DWORD wait_status(NTSTATUS s)
{
    if (s == STATUS_TIMEOUT) return WAIT_TIMEOUT;
    if ((ULONG)s >= STATUS_ABANDONED_WAIT_0 && (ULONG)s < STATUS_ABANDONED_WAIT_0 + MAXIMUM_WAIT_OBJECTS)
        return WAIT_ABANDONED_0 + ((ULONG)s - STATUS_ABANDONED_WAIT_0);
    if (NT_SUCCESS(s)) return WAIT_OBJECT_0 + (ULONG)s;
    set_error(s);
    return WAIT_FAILED;
}

/* -----------------------------------------------------------------------
 * Threads
 * ----------------------------------------------------------------------- */
HANDLE WINAPI CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE start,
                           LPVOID param, DWORD flags, LPDWORD tid)
{
    (void)sa;
    HANDLE h = 0;
    ULONG id = 0;
    NTSTATUS s = RtlNovaCreateThread((PUSER_THREAD_START_ROUTINE)start, param, stack,
                                     (flags & CREATE_SUSPENDED) != 0, &h, &id);
    if (!NT_SUCCESS(s)) { set_error(s); return 0; }
    if (tid) *tid = id;
    return h;
}

HANDLE WINAPI GetCurrentThread(void)                 { return NtCurrentThread(); }
VOID   WINAPI ExitThread(DWORD code)                 { RtlExitUserThread((NTSTATUS)code); }
BOOL   WINAPI SwitchToThread(void)                   { NtYieldExecution(); return TRUE; }

DWORD WINAPI ResumeThread(HANDLE t)
{
    ULONG prev = 0;
    NTSTATUS s = NtResumeThread(t, &prev);
    if (!NT_SUCCESS(s)) { set_error(s); return (DWORD)-1; }
    return prev;
}

DWORD WINAPI SuspendThread(HANDLE t)
{
    ULONG prev = 0;
    NTSTATUS s = NtSuspendThread(t, &prev);
    if (!NT_SUCCESS(s)) { set_error(s); return (DWORD)-1; }
    return prev;
}

BOOL WINAPI TerminateThread(HANDLE t, DWORD code)
{
    NTSTATUS s = NtTerminateThread(t, (NTSTATUS)code);
    if (!NT_SUCCESS(s)) { set_error(s); return FALSE; }
    return TRUE;
}

BOOL WINAPI GetExitCodeThread(HANDLE t, LPDWORD code)
{
    THREAD_BASIC_INFORMATION tbi;
    NTSTATUS s = NtQueryInformationThread(t, 0, &tbi, sizeof(tbi), 0);
    if (!NT_SUCCESS(s)) { set_error(s); return FALSE; }
    if (code) *code = (DWORD)tbi.ExitStatus;
    return TRUE;
}

BOOL WINAPI GetExitCodeProcess(HANDLE p, LPDWORD code)
{
    (void)p;
    if (code) *code = STILL_ACTIVE;                  /* only the current process exists here */
    return TRUE;
}

DWORD WINAPI GetThreadId(HANDLE t)
{
    THREAD_BASIC_INFORMATION tbi;
    if (!NT_SUCCESS(NtQueryInformationThread(t, 0, &tbi, sizeof(tbi), 0))) return 0;
    return (DWORD)(ULONG_PTR)tbi.ClientId.UniqueThread;
}

/* -----------------------------------------------------------------------
 * Waits
 * ----------------------------------------------------------------------- */
DWORD WINAPI WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable)
{
    LARGE_INTEGER li;
    return wait_status(NtWaitForSingleObject(h, alertable, ms_timeout(&li, ms)));
}
DWORD WINAPI WaitForSingleObject(HANDLE h, DWORD ms) { return WaitForSingleObjectEx(h, ms, FALSE); }

DWORD WINAPI WaitForMultipleObjectsEx(DWORD n, const HANDLE *h, BOOL all, DWORD ms, BOOL alertable)
{
    LARGE_INTEGER li;
    return wait_status(NtWaitForMultipleObjects(n, h, all ? WaitAll : WaitAny, alertable, ms_timeout(&li, ms)));
}
DWORD WINAPI WaitForMultipleObjects(DWORD n, const HANDLE *h, BOOL all, DWORD ms)
{
    return WaitForMultipleObjectsEx(n, h, all, ms, FALSE);
}

/* -----------------------------------------------------------------------
 * Events, mutexes, semaphores (named objects are not supported: name ignored)
 * ----------------------------------------------------------------------- */
HANDLE WINAPI CreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCSTR name)
{
    (void)sa; (void)name;
    HANDLE h = 0;
    NTSTATUS s = NtCreateEvent(&h, EVENT_ALL_ACCESS, 0, manual ? NotificationEvent : SynchronizationEvent, initial);
    if (!NT_SUCCESS(s)) { set_error(s); return 0; }
    return h;
}
HANDLE WINAPI CreateEventW(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCWSTR name)
{ (void)name; return CreateEventA(sa, manual, initial, 0); }
BOOL WINAPI SetEvent(HANDLE h)   { NTSTATUS s = NtSetEvent(h, 0);   return NT_SUCCESS(s) ? TRUE : (set_error(s), FALSE); }
BOOL WINAPI ResetEvent(HANDLE h) { NTSTATUS s = NtResetEvent(h, 0); return NT_SUCCESS(s) ? TRUE : (set_error(s), FALSE); }

HANDLE WINAPI CreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL owner, LPCSTR name)
{
    (void)sa; (void)name;
    HANDLE h = 0;
    NTSTATUS s = NtCreateMutant(&h, MUTEX_ALL_ACCESS, 0, owner);
    if (!NT_SUCCESS(s)) { set_error(s); return 0; }
    return h;
}
HANDLE WINAPI CreateMutexW(LPSECURITY_ATTRIBUTES sa, BOOL owner, LPCWSTR name)
{ (void)name; return CreateMutexA(sa, owner, 0); }
BOOL WINAPI ReleaseMutex(HANDLE h) { NTSTATUS s = NtReleaseMutant(h, 0); return NT_SUCCESS(s) ? TRUE : (set_error(s), FALSE); }

HANDLE WINAPI CreateSemaphoreA(LPSECURITY_ATTRIBUTES sa, LONG init, LONG max, LPCSTR name)
{
    (void)sa; (void)name;
    HANDLE h = 0;
    NTSTATUS s = NtCreateSemaphore(&h, SEMAPHORE_ALL_ACCESS, 0, init, max);
    if (!NT_SUCCESS(s)) { set_error(s); return 0; }
    return h;
}
HANDLE WINAPI CreateSemaphoreW(LPSECURITY_ATTRIBUTES sa, LONG init, LONG max, LPCWSTR name)
{ (void)name; return CreateSemaphoreA(sa, init, max, 0); }
BOOL WINAPI ReleaseSemaphore(HANDLE h, LONG count, LPLONG prev)
{ NTSTATUS s = NtReleaseSemaphore(h, count, prev); return NT_SUCCESS(s) ? TRUE : (set_error(s), FALSE); }

BOOL WINAPI DuplicateHandle(HANDLE sp, HANDLE src, HANDLE tp, LPHANDLE dst, DWORD access, BOOL inherit, DWORD options)
{
    (void)inherit;
    NTSTATUS s = NtDuplicateObject(sp, src, tp, dst, access, 0, options);
    return NT_SUCCESS(s) ? TRUE : (set_error(s), FALSE);
}

/* -----------------------------------------------------------------------
 * Critical sections, SRW locks, condition variables (thin wrappers)
 * ----------------------------------------------------------------------- */
VOID WINAPI InitializeCriticalSection(LPCRITICAL_SECTION cs) { RtlInitializeCriticalSection(cs); }
BOOL WINAPI InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION cs, DWORD spin)
{ RtlInitializeCriticalSectionAndSpinCount(cs, spin); return TRUE; }
VOID WINAPI DeleteCriticalSection(LPCRITICAL_SECTION cs) { RtlDeleteCriticalSection(cs); }
VOID WINAPI EnterCriticalSection(LPCRITICAL_SECTION cs)  { RtlEnterCriticalSection(cs); }
VOID WINAPI LeaveCriticalSection(LPCRITICAL_SECTION cs)  { RtlLeaveCriticalSection(cs); }
BOOL WINAPI TryEnterCriticalSection(LPCRITICAL_SECTION cs) { return RtlTryEnterCriticalSection(cs); }

VOID WINAPI InitializeSRWLock(PSRWLOCK l)            { RtlInitializeSRWLock(l); }
VOID WINAPI AcquireSRWLockExclusive(PSRWLOCK l)      { RtlAcquireSRWLockExclusive(l); }
VOID WINAPI ReleaseSRWLockExclusive(PSRWLOCK l)      { RtlReleaseSRWLockExclusive(l); }
VOID WINAPI AcquireSRWLockShared(PSRWLOCK l)         { RtlAcquireSRWLockShared(l); }
VOID WINAPI ReleaseSRWLockShared(PSRWLOCK l)         { RtlReleaseSRWLockShared(l); }
BOOLEAN WINAPI TryAcquireSRWLockExclusive(PSRWLOCK l) { return RtlTryAcquireSRWLockExclusive(l); }
BOOLEAN WINAPI TryAcquireSRWLockShared(PSRWLOCK l)    { return RtlTryAcquireSRWLockShared(l); }

VOID WINAPI InitializeConditionVariable(PCONDITION_VARIABLE cv) { RtlInitializeConditionVariable(cv); }
VOID WINAPI WakeConditionVariable(PCONDITION_VARIABLE cv)       { RtlWakeConditionVariable(cv); }
VOID WINAPI WakeAllConditionVariable(PCONDITION_VARIABLE cv)    { RtlWakeAllConditionVariable(cv); }
BOOL WINAPI SleepConditionVariableCS(PCONDITION_VARIABLE cv, PCRITICAL_SECTION cs, DWORD ms)
{
    LARGE_INTEGER li;
    NTSTATUS s = RtlSleepConditionVariableCS(cv, cs, ms_timeout(&li, ms));
    return NT_SUCCESS(s) ? TRUE : (set_error(s), FALSE);
}
BOOL WINAPI SleepConditionVariableSRW(PCONDITION_VARIABLE cv, PSRWLOCK l, DWORD ms, ULONG flags)
{
    LARGE_INTEGER li;
    NTSTATUS s = RtlSleepConditionVariableSRW(cv, l, ms_timeout(&li, ms), flags);
    return NT_SUCCESS(s) ? TRUE : (set_error(s), FALSE);
}

VOID WINAPI InitOnceInitialize(PINIT_ONCE once) { RtlRunOnceInitialize(once); }
BOOL WINAPI InitOnceExecuteOnce(PINIT_ONCE once, PINIT_ONCE_FN fn, PVOID param, LPVOID *ctx)
{
    PVOID c = 0;
    NTSTATUS s = RtlRunOnceBeginInitialize(once, 0, &c);
    if (s == STATUS_SUCCESS) { if (ctx) *ctx = c; return TRUE; }
    if (s != STATUS_PENDING) { set_error(s); return FALSE; }
    BOOL ok = fn(once, param, ctx);
    RtlRunOnceComplete(once, ok ? 0 : 4, ctx ? *ctx : 0);
    return ok;
}

/* -----------------------------------------------------------------------
 * Thread-local storage (TEB slots) and fiber-local storage (mapped to TLS)
 * ----------------------------------------------------------------------- */
static volatile long g_tls_bitmap[2];               /* 64 slots */
static PFLS_CALLBACK_FUNCTION g_fls_cb[64];

DWORD WINAPI TlsAlloc(void)
{
    for (int i = 0; i < 64; i++) {
        long word = g_tls_bitmap[i / 32];
        if (!(word & (1L << (i % 32)))) {
            if ((InterlockedOr(&g_tls_bitmap[i / 32], 1L << (i % 32)) & (1L << (i % 32))) == 0) {
                *(void **)(teb() + TEB_TLS_SLOTS + (SIZE_T)i * 8) = 0;
                return (DWORD)i;
            }
        }
    }
    set_error(STATUS_NO_MEMORY);
    return TLS_OUT_OF_INDEXES;
}

BOOL WINAPI TlsFree(DWORD i)
{
    if (i >= 64) { set_error(STATUS_INVALID_PARAMETER); return FALSE; }
    InterlockedAnd(&g_tls_bitmap[i / 32], ~(1L << (i % 32)));
    return TRUE;
}

LPVOID WINAPI TlsGetValue(DWORD i)
{
    if (i >= 64) { set_error(STATUS_INVALID_PARAMETER); return 0; }
    set_error(STATUS_SUCCESS);
    return *(void **)(teb() + TEB_TLS_SLOTS + (SIZE_T)i * 8);
}

BOOL WINAPI TlsSetValue(DWORD i, LPVOID v)
{
    if (i >= 64) { set_error(STATUS_INVALID_PARAMETER); return FALSE; }
    *(void **)(teb() + TEB_TLS_SLOTS + (SIZE_T)i * 8) = v;
    return TRUE;
}

DWORD WINAPI FlsAlloc(PFLS_CALLBACK_FUNCTION cb)
{
    DWORD i = TlsAlloc();
    if (i != TLS_OUT_OF_INDEXES && i < 64) g_fls_cb[i] = cb;
    return i;
}
BOOL  WINAPI FlsFree(DWORD i)          { if (i < 64) g_fls_cb[i] = 0; return TlsFree(i); }
PVOID WINAPI FlsGetValue(DWORD i)      { return TlsGetValue(i); }
BOOL  WINAPI FlsSetValue(DWORD i, PVOID v) { return TlsSetValue(i, v); }

/* -----------------------------------------------------------------------
 * Dynamic loading
 * ----------------------------------------------------------------------- */
HMODULE WINAPI LoadLibraryA(LPCSTR name)
{
    PVOID base = 0;
    NTSTATUS s = LdrNovaLoadDllA(name, &base);
    if (!NT_SUCCESS(s)) { set_error(s); return 0; }
    return (HMODULE)base;
}
HMODULE WINAPI LoadLibraryW(LPCWSTR name)
{
    char n[128];
    int i = 0;
    if (name) for (; i < 127 && name[i]; i++) n[i] = (char)name[i];
    n[i] = 0;
    return LoadLibraryA(n);
}
HMODULE WINAPI LoadLibraryExA(LPCSTR name, HANDLE f, DWORD flags) { (void)f; (void)flags; return LoadLibraryA(name); }

BOOL WINAPI GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE *out)
{
    if (!out) return FALSE;
    if (flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) {
        PLDR_DATA_TABLE_ENTRY e = LdrNovaFindEntry((PVOID)name);
        *out = e ? e->DllBase : 0;
    } else {
        *out = name ? (HMODULE)LdrNovaGetModuleA(name) : (HMODULE)RtlGetCurrentPeb()->ImageBaseAddress;
    }
    if (!*out) { set_error(STATUS_DLL_NOT_FOUND); return FALSE; }
    return TRUE;
}

/* -----------------------------------------------------------------------
 * Exceptions
 * ----------------------------------------------------------------------- */
VOID WINAPI RaiseException(DWORD code, DWORD flags, DWORD nargs, const ULONG_PTR *args)
{
    EXCEPTION_RECORD rec;
    memset(&rec, 0, sizeof(rec));
    rec.ExceptionCode = code;
    rec.ExceptionFlags = flags;
    if (args && nargs) {
        if (nargs > EXCEPTION_MAXIMUM_PARAMETERS) nargs = EXCEPTION_MAXIMUM_PARAMETERS;
        rec.NumberParameters = nargs;
        for (DWORD i = 0; i < nargs; i++) rec.ExceptionInformation[i] = args[i];
    }
    RtlRaiseException(&rec);
}

LPTOP_LEVEL_EXCEPTION_FILTER WINAPI SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER f)
{
    RtlSetUnhandledExceptionFilter(f);
    return 0;
}
LONG WINAPI UnhandledExceptionFilter(PEXCEPTION_POINTERS info) { (void)info; return EXCEPTION_EXECUTE_HANDLER; }
PVOID WINAPI AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER h) { return RtlAddVectoredExceptionHandler(first, h); }
ULONG WINAPI RemoveVectoredExceptionHandler(PVOID h) { return RtlRemoveVectoredExceptionHandler(h); }

VOID WINAPI GetNativeSystemInfo(LPSYSTEM_INFO si) { GetSystemInfo(si); }
