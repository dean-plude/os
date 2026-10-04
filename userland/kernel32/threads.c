/*
 * threads.c — kernel32 threads, synchronization, TLS, dynamic loading and
 * the exception helpers, on top of ntdll's native API.
 */

#define NOVA_BUILD_KERNEL32
#include <winternl.h>
#include <winnt.h>

void *memset(void *d, int c, size_t n);
size_t strlen(const char *s);

static BYTE *teb(void) { return NtCurrentTebBytes(); }
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
    ULONG64 info[3];
    NTSTATUS s = NtNovaProcessInfo(p, info);
    if (!NT_SUCCESS(s)) { set_error(s); return FALSE; }
    if (code) *code = (DWORD)info[1];
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
/* An alertable wait runs this thread's queued APCs (WAIT_IO_COMPLETION):
 * those already queued, and those queued while it waits (looked for
 * between slices of the wait).  A slice also ends when one of the
 * thread's waitable timers is due to run its completion routine, so that
 * routine runs on time.  @n 0: SleepEx. */
BOOL k32_run_apcs(void);
ULONGLONG k32_now_100ns(void);
ULONGLONG k32_timer_apc_slice(ULONGLONG slice);
DWORD k32_alertable_wait(DWORD n, const HANDLE *h, BOOL all, DWORD ms)
{
    ULONGLONG until = ms == INFINITE ? ~0ULL : k32_now_100ns() + (ULONGLONG)ms * 10000;
    for (;;) {
        if (k32_run_apcs()) return WAIT_IO_COMPLETION;
        ULONGLONG now = k32_now_100ns();
        ULONGLONG slice = until <= now ? 0 : until - now > 500000 ? 500000 : until - now;    /* (50 ms) */
        slice = k32_timer_apc_slice(slice);
        LARGE_INTEGER li;
        li.QuadPart = -(LONGLONG)slice;
        NTSTATUS s = n == 0 ? NtDelayExecution(TRUE, &li)
                   : n == 1 ? NtWaitForSingleObject(h[0], TRUE, &li)
                            : NtWaitForMultipleObjects(n, h, all ? WaitAll : WaitAny, TRUE, &li);
        DWORD r = n ? wait_status(s) : WAIT_TIMEOUT;
        if (r != WAIT_TIMEOUT) return r;
        if (until != ~0ULL && k32_now_100ns() >= until) return k32_run_apcs() ? WAIT_IO_COMPLETION : WAIT_TIMEOUT;
    }
}
#define alertable_wait k32_alertable_wait

DWORD WINAPI WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable)
{
    LARGE_INTEGER li;
    if (alertable) return alertable_wait(1, &h, FALSE, ms);
    return wait_status(NtWaitForSingleObject(h, alertable, ms_timeout(&li, ms)));
}
DWORD WINAPI WaitForSingleObject(HANDLE h, DWORD ms) { return WaitForSingleObjectEx(h, ms, FALSE); }

DWORD WINAPI WaitForMultipleObjectsEx(DWORD n, const HANDLE *h, BOOL all, DWORD ms, BOOL alertable)
{
    LARGE_INTEGER li;
    if (alertable) return alertable_wait(n, h, all, ms);
    return wait_status(NtWaitForMultipleObjects(n, h, all ? WaitAll : WaitAny, alertable, ms_timeout(&li, ms)));
}
DWORD WINAPI WaitForMultipleObjects(DWORD n, const HANDLE *h, BOOL all, DWORD ms)
{
    return WaitForMultipleObjectsEx(n, h, all, ms, FALSE);
}

/* -----------------------------------------------------------------------
 * Events, mutexes, semaphores.  Named ones are shared between processes
 * (the kernel's object namespace); creating an existing name opens it and
 * reports ERROR_ALREADY_EXISTS.
 * ----------------------------------------------------------------------- */
typedef struct { UNICODE_STRING us; OBJECT_ATTRIBUTES oa; WCHAR buf[260]; } ObName;

static POBJECT_ATTRIBUTES ob_name_w(ObName *n, LPCWSTR name)
{
    if (!name || !name[0]) return 0;
    int k = 0;
    for (; name[k] && k < 259; k++) n->buf[k] = name[k];
    n->buf[k] = 0;
    RtlInitUnicodeString(&n->us, n->buf);
    memset(&n->oa, 0, sizeof(n->oa));
    n->oa.Length = sizeof(n->oa);
    n->oa.ObjectName = &n->us;
    return &n->oa;
}

static POBJECT_ATTRIBUTES ob_name_a(ObName *n, LPCSTR name)
{
    if (!name || !name[0]) return 0;
    WCHAR w[260];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, w, 260);
    w[259] = 0;
    return ob_name_w(n, w);
}

/* @oa with OBJ_INHERIT added when @inherit (an unnamed object gets one) */
static POBJECT_ATTRIBUTES ob_inherit(ObName *n, POBJECT_ATTRIBUTES oa, BOOL inherit)
{
    if (!inherit) return oa;
    if (!oa) {
        memset(&n->oa, 0, sizeof(n->oa));
        n->oa.Length = sizeof(n->oa);
        oa = &n->oa;
    }
    oa->Attributes |= OBJ_INHERIT;
    return oa;
}
#define SA_INHERIT(sa) ((sa) && (sa)->bInheritHandle)

/* @oa for a new object: inheritable and with the descriptor @sa gives */
static POBJECT_ATTRIBUTES ob_attrs(ObName *n, POBJECT_ATTRIBUTES oa, LPSECURITY_ATTRIBUTES sa)
{
    oa = ob_inherit(n, oa, SA_INHERIT(sa));
    if (!sa || !sa->lpSecurityDescriptor) return oa;
    if (!oa) {
        memset(&n->oa, 0, sizeof(n->oa));
        n->oa.Length = sizeof(n->oa);
        oa = &n->oa;
    }
    oa->SecurityDescriptor = sa->lpSecurityDescriptor;
    return oa;
}

static HANDLE created(NTSTATUS s, HANDLE h)
{
    if (!NT_SUCCESS(s)) { set_error(s); return 0; }
    SetLastError(s == 0x40000000 /* STATUS_OBJECT_NAME_EXISTS */ ? ERROR_ALREADY_EXISTS : 0);
    return h;
}

static HANDLE opened(NTSTATUS s, HANDLE h)
{
    if (!NT_SUCCESS(s)) {
        if (s == (NTSTATUS)0xC0000034) SetLastError(ERROR_FILE_NOT_FOUND);
        else set_error(s);
        return 0;
    }
    return h;
}

HANDLE WINAPI CreateEventW(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCWSTR name)
{
    ObName n;
    HANDLE h = 0;
    NTSTATUS s = NtCreateEvent(&h, EVENT_ALL_ACCESS, ob_attrs(&n, ob_name_w(&n, name), sa), manual ? NotificationEvent : SynchronizationEvent, initial);
    return created(s, h);
}
HANDLE WINAPI CreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCSTR name)
{
    ObName n;
    HANDLE h = 0;
    NTSTATUS s = NtCreateEvent(&h, EVENT_ALL_ACCESS, ob_attrs(&n, ob_name_a(&n, name), sa), manual ? NotificationEvent : SynchronizationEvent, initial);
    return created(s, h);
}
__declspec(dllexport) HANDLE WINAPI CreateEventExW(LPSECURITY_ATTRIBUTES sa, LPCWSTR name, DWORD flags, DWORD access)
{ (void)access; return CreateEventW(sa, (flags & 1) != 0, (flags & 2) != 0, name); }
__declspec(dllexport) HANDLE WINAPI CreateEventExA(LPSECURITY_ATTRIBUTES sa, LPCSTR name, DWORD flags, DWORD access)
{ (void)access; return CreateEventA(sa, (flags & 1) != 0, (flags & 2) != 0, name); }
__declspec(dllexport) HANDLE WINAPI OpenEventW(DWORD access, BOOL inherit, LPCWSTR name)
{
    ObName n;
    HANDLE h = 0;
    POBJECT_ATTRIBUTES oa = ob_name_w(&n, name);
    if (!oa) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    oa = ob_inherit(&n, oa, inherit);
    NTSTATUS st = NtOpenEvent(&h, access, oa);
    return opened(st, h);
}
__declspec(dllexport) HANDLE WINAPI OpenEventA(DWORD access, BOOL inherit, LPCSTR name)
{
    ObName n;
    HANDLE h = 0;
    POBJECT_ATTRIBUTES oa = ob_name_a(&n, name);
    if (!oa) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    oa = ob_inherit(&n, oa, inherit);
    NTSTATUS st = NtOpenEvent(&h, access, oa);
    return opened(st, h);
}
BOOL WINAPI SetEvent(HANDLE h)   { NTSTATUS s = NtSetEvent(h, 0);   return NT_SUCCESS(s) ? TRUE : (set_error(s), FALSE); }
BOOL WINAPI ResetEvent(HANDLE h) { NTSTATUS s = NtResetEvent(h, 0); return NT_SUCCESS(s) ? TRUE : (set_error(s), FALSE); }

HANDLE WINAPI CreateMutexW(LPSECURITY_ATTRIBUTES sa, BOOL owner, LPCWSTR name)
{
    ObName n;
    HANDLE h = 0;
    NTSTATUS st = NtCreateMutant(&h, MUTEX_ALL_ACCESS, ob_attrs(&n, ob_name_w(&n, name), sa), owner);
    return created(st, h);
}
HANDLE WINAPI CreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL owner, LPCSTR name)
{
    ObName n;
    HANDLE h = 0;
    NTSTATUS st = NtCreateMutant(&h, MUTEX_ALL_ACCESS, ob_attrs(&n, ob_name_a(&n, name), sa), owner);
    return created(st, h);
}
__declspec(dllexport) HANDLE WINAPI CreateMutexExW(LPSECURITY_ATTRIBUTES sa, LPCWSTR name, DWORD flags, DWORD access)
{ (void)access; return CreateMutexW(sa, (flags & 1) != 0, name); }
__declspec(dllexport) HANDLE WINAPI CreateMutexExA(LPSECURITY_ATTRIBUTES sa, LPCSTR name, DWORD flags, DWORD access)
{ (void)access; return CreateMutexA(sa, (flags & 1) != 0, name); }
__declspec(dllexport) HANDLE WINAPI OpenMutexW(DWORD access, BOOL inherit, LPCWSTR name)
{
    ObName n;
    HANDLE h = 0;
    POBJECT_ATTRIBUTES oa = ob_name_w(&n, name);
    if (!oa) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    oa = ob_inherit(&n, oa, inherit);
    NTSTATUS st = NtOpenMutant(&h, access, oa);
    return opened(st, h);
}
__declspec(dllexport) HANDLE WINAPI OpenMutexA(DWORD access, BOOL inherit, LPCSTR name)
{
    ObName n;
    HANDLE h = 0;
    POBJECT_ATTRIBUTES oa = ob_name_a(&n, name);
    if (!oa) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    oa = ob_inherit(&n, oa, inherit);
    NTSTATUS st = NtOpenMutant(&h, access, oa);
    return opened(st, h);
}
/* Waitable timers: kernel timer objects, which end the waits on them when
 * they are due (to the TSC, not the 10 ms tick).  SetWaitableTimer and the
 * completion routines are in extra.c. */
__declspec(dllexport) HANDLE WINAPI CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES sa, LPCWSTR name, DWORD flags, DWORD access)
{
    ObName n;
    HANDLE h = 0;
    NTSTATUS s = NtCreateTimer(&h, access ? access : 0x1F0003 /* TIMER_ALL_ACCESS */, ob_attrs(&n, ob_name_w(&n, name), sa),
                               flags & 1 /* CREATE_WAITABLE_TIMER_MANUAL_RESET */ ? 0 /* NotificationTimer */ : 1);
    return created(s, h);
}
__declspec(dllexport) HANDLE WINAPI CreateWaitableTimerW(LPSECURITY_ATTRIBUTES sa, BOOL manual, LPCWSTR name)
{
    return CreateWaitableTimerExW(sa, name, manual ? 1 : 0, 0);
}
__declspec(dllexport) HANDLE WINAPI CreateWaitableTimerA(LPSECURITY_ATTRIBUTES sa, BOOL manual, LPCSTR name)
{
    ObName n;
    HANDLE h = 0;
    NTSTATUS s = NtCreateTimer(&h, 0x1F0003, ob_attrs(&n, ob_name_a(&n, name), sa), manual ? 0 : 1);
    return created(s, h);
}
__declspec(dllexport) HANDLE WINAPI OpenWaitableTimerW(DWORD access, BOOL inherit, LPCWSTR name)
{
    ObName n;
    HANDLE h = 0;
    POBJECT_ATTRIBUTES oa = ob_name_w(&n, name);
    if (!oa) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    oa = ob_inherit(&n, oa, inherit);
    return opened(NtOpenTimer(&h, access, oa), h);
}
__declspec(dllexport) HANDLE WINAPI OpenWaitableTimerA(DWORD access, BOOL inherit, LPCSTR name)
{
    ObName n;
    HANDLE h = 0;
    POBJECT_ATTRIBUTES oa = ob_name_a(&n, name);
    if (!oa) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    oa = ob_inherit(&n, oa, inherit);
    return opened(NtOpenTimer(&h, access, oa), h);
}

BOOL WINAPI ReleaseMutex(HANDLE h) { NTSTATUS s = NtReleaseMutant(h, 0); return NT_SUCCESS(s) ? TRUE : (set_error(s), FALSE); }

HANDLE WINAPI CreateSemaphoreW(LPSECURITY_ATTRIBUTES sa, LONG init, LONG max, LPCWSTR name)
{
    ObName n;
    HANDLE h = 0;
    NTSTATUS st = NtCreateSemaphore(&h, SEMAPHORE_ALL_ACCESS, ob_attrs(&n, ob_name_w(&n, name), sa), init, max);
    return created(st, h);
}
HANDLE WINAPI CreateSemaphoreA(LPSECURITY_ATTRIBUTES sa, LONG init, LONG max, LPCSTR name)
{
    ObName n;
    HANDLE h = 0;
    NTSTATUS st = NtCreateSemaphore(&h, SEMAPHORE_ALL_ACCESS, ob_attrs(&n, ob_name_a(&n, name), sa), init, max);
    return created(st, h);
}
__declspec(dllexport) HANDLE WINAPI CreateSemaphoreExW(LPSECURITY_ATTRIBUTES sa, LONG init, LONG max, LPCWSTR name, DWORD flags, DWORD access)
{ (void)flags; (void)access; return CreateSemaphoreW(sa, init, max, name); }
__declspec(dllexport) HANDLE WINAPI OpenSemaphoreW(DWORD access, BOOL inherit, LPCWSTR name)
{
    ObName n;
    HANDLE h = 0;
    POBJECT_ATTRIBUTES oa = ob_name_w(&n, name);
    if (!oa) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    oa = ob_inherit(&n, oa, inherit);
    NTSTATUS st = NtOpenSemaphore(&h, access, oa);
    return opened(st, h);
}
__declspec(dllexport) HANDLE WINAPI OpenSemaphoreA(DWORD access, BOOL inherit, LPCSTR name)
{
    ObName n;
    HANDLE h = 0;
    POBJECT_ATTRIBUTES oa = ob_name_a(&n, name);
    if (!oa) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    oa = ob_inherit(&n, oa, inherit);
    NTSTATUS st = NtOpenSemaphore(&h, access, oa);
    return opened(st, h);
}
BOOL WINAPI ReleaseSemaphore(HANDLE h, LONG count, LPLONG prev)
{ NTSTATUS s = NtReleaseSemaphore(h, count, prev); return NT_SUCCESS(s) ? TRUE : (set_error(s), FALSE); }

BOOL WINAPI DuplicateHandle(HANDLE sp, HANDLE src, HANDLE tp, LPHANDLE dst, DWORD access, BOOL inherit, DWORD options)
{
    NTSTATUS s = NtDuplicateObject(sp, src, tp, dst, access, inherit ? OBJ_INHERIT : 0, options);
    return NT_SUCCESS(s) ? TRUE : (set_error(s), FALSE);
}

BOOL WINAPI CompareObjectHandles(HANDLE first, HANDLE second)
{
    NTSTATUS s = NtCompareObjects(first, second);
    return NT_SUCCESS(s) ? TRUE : (set_error(s), FALSE);
}

/* -----------------------------------------------------------------------
 * Critical sections, SRW locks, condition variables (thin wrappers)
 * ----------------------------------------------------------------------- */
VOID WINAPI InitializeCriticalSection(LPCRITICAL_SECTION cs) { RtlInitializeCriticalSection(cs); }
BOOL WINAPI InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION cs, DWORD spin)
{ RtlInitializeCriticalSectionAndSpinCount(cs, spin); return TRUE; }
/* The spin count before a waiter sleeps (NovaOS's critical sections keep
 * it but don't spin on it): returns the old one */
WINBASEAPI DWORD WINAPI SetCriticalSectionSpinCount(LPCRITICAL_SECTION cs, DWORD spin)
{
    DWORD old = (DWORD)cs->SpinCount;
    cs->SpinCount = spin;
    return old;
}
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
    if (s == 0x102) { SetLastError(1460 /* ERROR_TIMEOUT */); return FALSE; }   /* STATUS_TIMEOUT */
    return NT_SUCCESS(s) ? TRUE : (set_error(s), FALSE);
}
BOOL WINAPI SleepConditionVariableSRW(PCONDITION_VARIABLE cv, PSRWLOCK l, DWORD ms, ULONG flags)
{
    LARGE_INTEGER li;
    NTSTATUS s = RtlSleepConditionVariableSRW(cv, l, ms_timeout(&li, ms), flags);
    if (s == 0x102) { SetLastError(1460 /* ERROR_TIMEOUT */); return FALSE; }   /* STATUS_TIMEOUT */
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
 * Thread-local storage: 64 TEB slots and 1024 expansion slots (ntdll_tls.c)
 * ----------------------------------------------------------------------- */
DWORD WINAPI TlsAlloc(void)
{
    ULONG i;
    NTSTATUS s = RtlTlsAlloc(&i);
    if (!NT_SUCCESS(s)) { set_error(s); return TLS_OUT_OF_INDEXES; }
    return i;
}

BOOL WINAPI TlsFree(DWORD i)
{
    NTSTATUS s = RtlTlsFree(i);
    return NT_SUCCESS(s) ? TRUE : (set_error(s), FALSE);
}

LPVOID WINAPI TlsGetValue(DWORD i)
{
    if (i >= TLS_MINIMUM_AVAILABLE + TLS_EXPANSION_SLOTS) { set_error(STATUS_INVALID_PARAMETER); return 0; }
    set_error(STATUS_SUCCESS);
    if (i < TLS_MINIMUM_AVAILABLE) return ((void **)(teb() + TEB_TLS_SLOTS))[i];
    void **a = *(void ***)(teb() + TEB_TLS_EXPANSION);
    return a ? a[i - TLS_MINIMUM_AVAILABLE] : 0;
}

BOOL WINAPI TlsSetValue(DWORD i, LPVOID v)
{
    NTSTATUS s = RtlTlsSetValue(i, v);
    return NT_SUCCESS(s) ? TRUE : (set_error(s), FALSE);
}

/* Fiber-local storage: ntdll's own slots and callbacks (ntdll_fls.c) */
DWORD WINAPI FlsAlloc(PFLS_CALLBACK_FUNCTION cb)
{
    ULONG i;
    NTSTATUS s = RtlFlsAlloc(cb, &i);
    if (!NT_SUCCESS(s)) { set_error(s); return FLS_OUT_OF_INDEXES; }
    return i;
}
BOOL WINAPI FlsFree(DWORD i)
{
    NTSTATUS s = RtlFlsFree(i);
    return NT_SUCCESS(s) ? TRUE : (set_error(s), FALSE);
}
PVOID WINAPI FlsGetValue(DWORD i)
{
    PVOID v = 0;
    NTSTATUS s = RtlFlsGetValue(i, &v);
    set_error(s);
    return v;
}
BOOL WINAPI FlsSetValue(DWORD i, PVOID v)
{
    NTSTATUS s = RtlFlsSetValue(i, v);
    return NT_SUCCESS(s) ? TRUE : (set_error(s), FALSE);
}

/* -----------------------------------------------------------------------
 * Dynamic loading
 * ----------------------------------------------------------------------- */
/* LoadLibraryEx's flags that change how a module is mapped (as data); the
 * search-path ones need nothing: the program's folder, then the system's */
#define LL_DATA_FLAGS 0x62                  /* AS_DATAFILE, AS_IMAGE_RESOURCE, AS_DATAFILE_EXCLUSIVE */
HMODULE WINAPI LoadLibraryExA(LPCSTR name, HANDLE f, DWORD flags)
{
    (void)f;
    if (!name || !*name) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    PVOID base = 0;
    NTSTATUS s = LdrNovaLoadDllExA(name, flags & LL_DATA_FLAGS, &base);
    if (!NT_SUCCESS(s)) { set_error(s); return 0; }
    return (HMODULE)base;
}
HMODULE WINAPI LoadLibraryA(LPCSTR name) { return LoadLibraryExA(name, 0, 0); }
HMODULE WINAPI LoadLibraryW(LPCWSTR name)
{
    char n[MAX_PATH * 3];
    if (!name) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    int k = WideCharToMultiByte(CP_UTF8, 0, name, -1, n, sizeof(n), 0, 0);
    if (!k) { SetLastError(ERROR_FILENAME_EXCED_RANGE); return 0; }
    return LoadLibraryExA(n, 0, 0);
}

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
    if ((code == 0xC06D007E || code == 0xC06D007F) && nargs >= 1 && args[0]) {
        /* the delay-load helper's "module/procedure not found": its
         * DelayLoadInfo names what was missing, which the serial log shows */
        struct { DWORD cb; const void *pidd; void *ppfn; const char *dll; BOOL by_name; const char *proc; } *dli = (void *)args[0];
        char msg[200];
        int k = 0;
        const char *parts[4] = { "delay load failed: ", dli->dll ? dli->dll : "?", "!",
                                 code == 0xC06D007E ? "(module not found)" : dli->by_name && dli->proc ? dli->proc : "(an ordinal)" };
        for (int i = 0; i < 4; i++) for (const char *c = parts[i]; *c && k < 190; c++) msg[k++] = *c;
        if (code == 0xC06D007F && !dli->by_name && k < 180) {
            DWORD o = (DWORD)(ULONG_PTR)dli->proc;
            char d[12];
            int n = 0;
            do d[n++] = (char)('0' + o % 10); while ((o /= 10) && n < 11);
            msg[k++] = ' ';
            while (n) msg[k++] = d[--n];
        }
        msg[k++] = '\n';
        NtNovaDebugPrint(msg, (ULONG)k);
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

VOID WINAPI GetNativeSystemInfo(LPSYSTEM_INFO si)
{
    GetSystemInfo(si);
    si->wProcessorArchitecture = 9;                          /* the machine: AMD64 */
    si->dwProcessorType = 8664;
}
