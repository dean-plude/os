"""Compile actual source fragments against deterministic host lifecycle fixtures."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
CC = shutil.which('cc') or shutil.which('gcc')


@unittest.skipUnless(CC, 'host C compiler unavailable')
class ThreadChurnTests(unittest.TestCase):
    def execute(self, source, pthread=False):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'test.c'
            path.write_text(source)
            binary = Path(d) / 'test'
            subprocess.run([CC, '-std=c11', '-Wall', '-Wextra', '-Werror',
                            *(['-pthread'] if pthread else []), str(path), '-o', str(binary)],
                           check=True, capture_output=True, text=True)
            return subprocess.run([str(binary)], check=True, timeout=15,
                                  capture_output=True, text=True).stdout

    def test_reaper_frees_only_exited_off_cpu_threads_and_reuses_slots(self):
        source = (ROOT / 'kernel/um/um.c').read_text()
        reaper = source[source.index('static int reap_threads(UmProcess *p)\n{'):source.index('/* Whether a thread of @p has ended')]
        creation = source[source.index('UmThread *um_create_thread('):]
        selection = creation[creation.index('    um_lock_excl(&p->lock);'):creation.index('    if (slot < 0 || p->kill_pending)')]
        old_selection = selection.replace('    reap_threads(p);\n', '')
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#define UM_MAX_THREADS 256
typedef struct { bool gone; void *fpu; } Thread;
typedef struct { int refs; } Object;
typedef struct { bool exited; Thread *kt; Object ob; } UmThread;
typedef struct { int lock; struct { int max_threads; } lay; UmThread *threads[UM_MAX_THREADS]; } UmProcess;
static int freed, pages;
static void um_lock_excl(int *lock) { (void)lock; }
static bool sched_thread_gone(Thread *t) { return t->gone; }
static void kernel_free_pages(void *p, int n) { (void)p; pages += n; }
static void sched_free_thread(Thread *t) { assert(t->gone); freed++; }
static void um_ob_unref(Object *o) { o->refs--; }
'''
        harness += reaper + '\nstatic int old_slot(UmProcess *p) {\n' + old_selection + '\nreturn slot; }\n'
        harness += '\nstatic int new_slot(UmProcess *p) {\n' + selection + '\nreturn slot; }\n'
        harness += r'''
int main(void) {
    UmProcess p = { .lay.max_threads = 96 };
    UmThread threads[96]; Thread kernel[96];
    for (int i = 0; i < 96; i++) {
        kernel[i] = (Thread){ .gone = true, .fpu = (void *)1 };
        threads[i] = (UmThread){ .exited = true, .kt = &kernel[i], .ob.refs = 2 };
        p.threads[i] = &threads[i];
    }
    threads[0].exited = false; kernel[0].gone = false;
    kernel[3].gone = false; /* exited but still on CPU: must survive */
    assert(old_slot(&p) == -1); /* reproduces false TOO_MANY_THREADS */
    assert(new_slot(&p) == 1);
    assert(p.threads[0] == &threads[0] && p.threads[3] == &threads[3]);
    assert(threads[0].ob.refs == 2 && threads[3].ob.refs == 2);
    assert(freed == 94 && pages == 94 && threads[1].ob.refs == 1);
    /* All truly live slots remain occupied, regardless of the reaper. */
    for (int i = 0; i < 96; i++) {
        kernel[i].gone = false;
        threads[i] = (UmThread){ .exited = false, .kt = &kernel[i], .ob.refs = 2 };
        p.threads[i] = &threads[i];
    }
    assert(new_slot(&p) == -1);
    /* 5120 creations without any background UmPoll call. */
    for (int i = 1; i < 96; i++) { threads[i].exited = true; kernel[i].gone = true; }
    for (int i = 0; i < 5120; i++) {
        int slot = new_slot(&p); assert(slot > 0);
        threads[slot].kt = &kernel[slot]; threads[slot].ob.refs = 2;
        threads[slot].exited = true; kernel[slot].gone = true;
        p.threads[slot] = &threads[slot];
    }
    puts("old: no slot; fixed: 5120 creations; live/off-CPU/reference guards pass");
}
'''
        self.assertIn('fixed: 5120 creations', self.execute(harness))

    def test_callback_batch_joins_teardown_and_closes_handles_before_baseline(self):
        source = (ROOT / 'userland/programs/tpsimpletest.c').read_text()
        join = source[source.index('static BOOL join_batch(void)'):source.index('int main(void)')]
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define BATCH 32
#define TRUE 1
#define FALSE 0
#define WAIT_OBJECT_0 0
typedef int BOOL;
typedef uint32_t DWORD;
typedef void *HANDLE;
static HANDLE g_threads[BATCH];
static int waited, closed, fail_wait = -1, fail_close = -1;
static DWORD now;
static DWORD GetTickCount(void) { return now; }
static DWORD WaitForSingleObject(HANDLE h, DWORD timeout) {
    assert(h == (HANDLE)(uintptr_t)(waited + 1));
    assert(timeout == (now < 10000 ? 10000 - now : 0));
    now += 500;
    return waited++ == fail_wait ? 258 : WAIT_OBJECT_0;
}
static BOOL CloseHandle(HANDLE h) {
    assert(h == (HANDLE)(uintptr_t)(closed + 1));
    return closed++ != fail_close;
}
static void reset(void) {
    waited = closed = 0; now = 0;
    for (int i = 0; i < BATCH; i++) g_threads[i] = (HANDLE)(uintptr_t)(i + 1);
}
'''
        harness += join + r'''
int main(void) {
    reset(); assert(join_batch()); assert(waited == BATCH && closed == BATCH);
    for (int i = 0; i < BATCH; i++) assert(!g_threads[i]);
    reset(); fail_wait = 5; assert(!join_batch()); assert(closed == BATCH);
    reset(); fail_wait = -1; fail_close = 5; assert(!join_batch()); assert(closed == BATCH);
    reset(); fail_close = -1; g_threads[0] = 0; waited = closed = 1;
    assert(!join_batch()); assert(closed == BATCH);
    puts("joined teardown; bounded waits; wait/close/missing-handle failures fail closed");
}
'''
        self.assertIn('joined teardown', self.execute(harness))
        main = source[source.index('int main(void)'):]
        self.assertLess(main.index('if (!join_batch())'), main.index('after > before'))
        self.assertIn('#define ROUNDS 160', source)
        self.assertIn('#define BATCH 32', source)

    def test_native_warmup_initializes_before_baseline_and_fails_closed(self):
        source = (ROOT / 'userland/programs/tpsimpletest.c').read_text()
        warmup = source[source.index('static DWORD WINAPI warm_thread'):source.index('#define BATCH')]
        harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define WINAPI
#define FALSE 0
#define TRUE 1
#define WAIT_OBJECT_0 0
#define TEB_PEB 0
typedef void *PVOID, *HANDLE;
typedef uint32_t DWORD, ULONG;
typedef uintptr_t ULONG_PTR;
typedef int BOOL, NTSTATUS;
typedef struct { HANDLE LockSemaphore; } Lock, *PRTL_CRITICAL_SECTION;
typedef struct { Lock *LoaderLock; } PEB;
static Lock loader; static PEB peb = { &loader };
static PEB *teb_peb = &peb;
static unsigned char *NtCurrentTebBytes(void) { return (void *)&teb_peb; }
static int held, handles, permit_event = 1, permit_thread = 1, joins;
static DWORD ticks;
static DWORD GetTickCount(void) { return ticks; }
static void Sleep(DWORD ms) {
    ticks += ms;
    if (permit_event && !loader.LockSemaphore) { loader.LockSemaphore = (void *)1; handles++; }
}
static NTSTATUS LdrLockLoaderLock(ULONG flags, ULONG *state, ULONG_PTR *cookie) {
    (void)flags; assert(!held); held = 1; *state = 1; *cookie = 123; return 0;
}
static NTSTATUS LdrUnlockLoaderLock(ULONG flags, ULONG_PTR cookie) {
    (void)flags; assert(held && cookie == 123); held = 0; return 0;
}
static HANDLE CreateThread(void *sa, uintptr_t stack, DWORD (*fn)(PVOID), PVOID ctx, DWORD flags, DWORD *tid) {
    (void)sa; (void)stack; (void)flags; (void)tid;
    assert(held); assert(fn(ctx) == 0);
    if (!permit_thread) return NULL;
    handles++; return (void *)2;
}
static DWORD WaitForSingleObject(HANDLE h, DWORD timeout) {
    assert(h == (void *)2 && !held && timeout == 10000); joins++; return WAIT_OBJECT_0;
}
static void CloseHandle(HANDLE h) { assert(h == (void *)2); handles--; }
'''
        harness += warmup + r'''
int main(void) {
    assert(warm_loader_lock() && !held && handles == 1 && joins == 1);
    int baseline = handles;
    assert(warm_loader_lock() && handles == baseline); /* exact stable baseline */
    loader.LockSemaphore = NULL; handles = 0; permit_event = 0; ticks = 0;
    assert(!warm_loader_lock() && !held && handles == 0 && ticks == 5000);
    permit_thread = 0;
    assert(!warm_loader_lock() && !held && handles == 0);
    puts("warmup initializes once, joins, preserves exact baseline; failures are bounded");
}
'''
        self.assertIn('preserves exact baseline', self.execute(harness))

    def test_actual_loader_critical_section_retains_one_lazy_event_not_a_leak(self):
        source = (ROOT / 'userland/ntdll/ntdll_sync.c').read_text()
        routines = source[source.index('NTSTATUS NTAPI RtlInitializeCriticalSectionAndSpinCount'):source.index('/* -----------------------------------------------------------------------\n * Waiting on an address')]
        harness = r'''
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define NTAPI
#define STATUS_SUCCESS 0
#define NT_SUCCESS(s) ((s) == 0)
#define SynchronizationEvent 0
#define FALSE 0
#define TRUE 1
#define YieldProcessor() sched_yield()
typedef int NTSTATUS;
typedef uint32_t ULONG;
typedef uintptr_t ULONG_PTR;
typedef void *HANDLE;
typedef bool BOOLEAN;
typedef struct { void *DebugInfo; int LockCount, RecursionCount; HANDLE OwningThread, LockSemaphore; ULONG_PTR SpinCount; } RTL_CRITICAL_SECTION, *PRTL_CRITICAL_SECTION;
static _Thread_local ULONG tid = 1;
static ULONG cur_tid(void) { return tid; }
static volatile int handles;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static int signaled;
static NTSTATUS NtCreateEvent(HANDLE *out, int a, void *b, int c, int d) {
    (void)a; (void)b; (void)c; (void)d;
    *out = (void *)1; __atomic_add_fetch(&handles, 1, __ATOMIC_SEQ_CST); return 0;
}
static void NtClose(HANDLE h) { assert(h); __atomic_sub_fetch(&handles, 1, __ATOMIC_SEQ_CST); }
static void *InterlockedCompareExchangePointer(void **p, void *value, void *expected) {
    __atomic_compare_exchange_n(p, &expected, value, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return expected;
}
static int InterlockedIncrement(int *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static int InterlockedDecrement(int *p) { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
static int InterlockedCompareExchange(int *p, int v, int e) {
    __atomic_compare_exchange_n(p, &e, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return e;
}
static void NtWaitForSingleObject(HANDLE h, int alert, void *timeout) {
    assert(h); (void)alert; (void)timeout; pthread_mutex_lock(&mutex);
    while (!signaled) pthread_cond_wait(&condition, &mutex);
    signaled = 0; pthread_mutex_unlock(&mutex);
}
static void NtSetEvent(HANDLE h, void *old) {
    assert(h); (void)old; pthread_mutex_lock(&mutex); signaled = 1;
    pthread_cond_signal(&condition); pthread_mutex_unlock(&mutex);
}
'''
        harness += routines
        harness += r'''
static RTL_CRITICAL_SECTION loader;
static void *worker(void *unused) {
    (void)unused; tid = 2;
    RtlEnterCriticalSection(&loader); RtlLeaveCriticalSection(&loader); return NULL;
}
int main(void) {
    RtlInitializeCriticalSection(&loader);
    assert(handles == 0); /* cold baseline */
    for (int i = 0; i < 10; i++) {
        RtlEnterCriticalSection(&loader);
        pthread_t thread; assert(pthread_create(&thread, NULL, worker, NULL) == 0);
        while (__atomic_load_n(&loader.LockCount, __ATOMIC_SEQ_CST) != 1 || !__atomic_load_n(&handles, __ATOMIC_SEQ_CST)) sched_yield();
        RtlLeaveCriticalSection(&loader);
        assert(pthread_join(thread, NULL) == 0);
        assert(handles == 1); /* retained lock event: no per-callback growth */
    }
    RtlDeleteCriticalSection(&loader); assert(handles == 0);
    puts("cold baseline gains one lazy event; warmed baseline remains exact");
}
'''
        self.assertIn('warmed baseline remains exact', self.execute(harness, pthread=True))


if __name__ == '__main__':
    unittest.main()
