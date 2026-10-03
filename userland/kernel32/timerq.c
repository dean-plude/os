/*
 * timerq.c — timer queues (CreateTimerQueue, CreateTimerQueueTimer, ...):
 * one thread per queue runs the callbacks in due order.  A timer stays
 * until deleted; a one-shot (period 0) just stops firing.  The default
 * queue (NULL) is created on first use.
 */

#define NOVA_BUILD_KERNEL32
#include <winternl.h>
#include "k32.h"

#define K32 __declspec(dllexport)

void *memset(void *d, int c, size_t n);

typedef VOID (CALLBACK *TimerFn)(PVOID, BOOLEAN);

typedef struct TqTimer {
    struct TqTimer *next;
    struct TqQueue *q;
    TimerFn   fn;
    PVOID     ctx;
    ULONGLONG due;                      /* GetTickCount64 of the next run; 0 = stopped */
    DWORD     period;
    DWORD     magic;                    /* 0x54514D52 "TQMR" */
    volatile LONG running;              /* its callback is executing */
} TqTimer;

typedef struct TqQueue {
    SRWLOCK   lock;
    HANDLE    thread, wake;             /* wake: the timers changed */
    TqTimer  *timers;
    DWORD     magic;                    /* 0x54515545 "TQUE" */
    volatile LONG quit;
} TqQueue;

K32 HANDLE WINAPI CreateTimerQueue(void);

static TqQueue *g_default;
static SRWLOCK  g_dlock;

static void *zalloc(SIZE_T n) { return RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, n); }
static void  zfree(void *p)   { if (p) RtlFreeHeap(RtlGetProcessHeap(), 0, p); }

static DWORD WINAPI tq_thread(LPVOID p)
{
    TqQueue *q = p;
    while (!q->quit) {
        AcquireSRWLockExclusive(&q->lock);
        ULONGLONG now = GetTickCount64(), next = ~0ull;
        TqTimer *fire = 0;
        for (TqTimer *t = q->timers; t; t = t->next) {
            if (!t->due) continue;
            if (t->due <= now) { fire = t; break; }
            if (t->due < next) next = t->due;
        }
        if (fire) {
            fire->due = fire->period ? now + fire->period : 0;
            fire->running = 1;
            TimerFn fn = fire->fn; PVOID ctx = fire->ctx;
            ReleaseSRWLockExclusive(&q->lock);
            fn(ctx, TRUE);
            __atomic_store_n(&fire->running, 0, __ATOMIC_RELEASE);
            continue;
        }
        ReleaseSRWLockExclusive(&q->lock);
        DWORD ms = next == ~0ull ? INFINITE : next > now ? (DWORD)(next - now) : 0;
        WaitForSingleObject(q->wake, ms);
    }
    return 0;
}

static TqQueue *tq_get(HANDLE h)
{
    if (h) return ((TqQueue *)h)->magic == 0x54515545 ? (TqQueue *)h : 0;
    AcquireSRWLockExclusive(&g_dlock);
    if (!g_default) g_default = (TqQueue *)CreateTimerQueue();
    TqQueue *q = g_default;
    ReleaseSRWLockExclusive(&g_dlock);
    return q;
}

K32 HANDLE WINAPI CreateTimerQueue(void)
{
    TqQueue *q = zalloc(sizeof(*q));
    if (!q) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    q->magic = 0x54515545;
    q->wake = CreateEventW(0, FALSE, FALSE, 0);
    q->thread = CreateThread(0, 0, tq_thread, q, 0, 0);
    if (!q->wake || !q->thread) { if (q->wake) CloseHandle(q->wake); zfree(q); return 0; }
    return (HANDLE)q;
}

K32 BOOL WINAPI CreateTimerQueueTimer(PHANDLE out, HANDLE queue, TimerFn fn, PVOID ctx, DWORD due, DWORD period, ULONG flags)
{
    (void)flags;
    TqQueue *q = tq_get(queue);
    if (!q || !out || !fn) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    TqTimer *t = zalloc(sizeof(*t));
    if (!t) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    t->q = q; t->fn = fn; t->ctx = ctx; t->period = period; t->magic = 0x54514D52;
    t->due = GetTickCount64() + due;
    if (!t->due) t->due = 1;
    AcquireSRWLockExclusive(&q->lock);
    t->next = q->timers; q->timers = t;
    ReleaseSRWLockExclusive(&q->lock);
    SetEvent(q->wake);
    *out = (HANDLE)t;
    return TRUE;
}

K32 BOOL WINAPI ChangeTimerQueueTimer(HANDLE queue, HANDLE timer, ULONG due, ULONG period)
{
    TqTimer *t = (TqTimer *)timer;
    (void)queue;
    if (!t || t->magic != 0x54514D52) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    TqQueue *q = t->q;
    AcquireSRWLockExclusive(&q->lock);
    t->period = period;
    t->due = GetTickCount64() + due;
    if (!t->due) t->due = 1;
    ReleaseSRWLockExclusive(&q->lock);
    SetEvent(q->wake);
    return TRUE;
}

/* Unlink @t; with @done (not INVALID_HANDLE_VALUE) the caller is told when
 * a callback still running has finished. */
static BOOL tq_delete(TqQueue *q, TqTimer *t, HANDLE done)
{
    AcquireSRWLockExclusive(&q->lock);
    for (TqTimer **pp = &q->timers; *pp; pp = &(*pp)->next)
        if (*pp == t) { *pp = t->next; break; }
    t->due = 0;
    ReleaseSRWLockExclusive(&q->lock);
    BOOL ok = TRUE;
    if (done == INVALID_HANDLE_VALUE) {
        if (GetCurrentThreadId() != GetThreadId(q->thread))
            while (__atomic_load_n(&t->running, __ATOMIC_ACQUIRE)) Sleep(1);
    } else {
        if (__atomic_load_n(&t->running, __ATOMIC_ACQUIRE)) { SetLastError(ERROR_IO_PENDING); ok = FALSE; }
        if (done) SetEvent(done);
    }
    t->magic = 0;
    zfree(t);
    return ok;
}

K32 BOOL WINAPI DeleteTimerQueueTimer(HANDLE queue, HANDLE timer, HANDLE done)
{
    TqTimer *t = (TqTimer *)timer;
    (void)queue;
    if (!t || t->magic != 0x54514D52) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    return tq_delete(t->q, t, done);
}

K32 BOOL WINAPI DeleteTimerQueueEx(HANDLE queue, HANDLE done)
{
    TqQueue *q = (TqQueue *)queue;
    if (!q || q->magic != 0x54515545) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    AcquireSRWLockExclusive(&q->lock);
    while (q->timers) { TqTimer *t = q->timers; q->timers = t->next; t->magic = 0; zfree(t); }
    ReleaseSRWLockExclusive(&q->lock);
    q->quit = 1;
    SetEvent(q->wake);
    if (done == INVALID_HANDLE_VALUE) {
        if (GetCurrentThreadId() != GetThreadId(q->thread)) WaitForSingleObject(q->thread, INFINITE);
    } else if (done) {
        WaitForSingleObject(q->thread, INFINITE);
        SetEvent(done);
    }
    CloseHandle(q->thread);
    CloseHandle(q->wake);
    q->magic = 0;
    zfree(q);
    return TRUE;
}

K32 BOOL WINAPI DeleteTimerQueue(HANDLE queue) { return DeleteTimerQueueEx(queue, 0); }
