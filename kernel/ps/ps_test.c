/*
 * ps_test.c — boot-time self-test for PsGetCurrentThread
 *
 * Most threads are created by the scheduler directly (sched_create_thread)
 * and are bare Threads, not the start of an ETHREAD.  This test runs on
 * such threads and checks that PsGetCurrentThread gives them an ETHREAD
 * of their own (a real Thread object) rather than treating the Thread as
 * one, which wrote past the end of it.
 */

#include "ps.h"
#include "../ob/ob.h"
#include "../ke/printf.h"
#include "../ke/scheduler.h"

static int g_pass, g_fail;

#define CHECK(cond, what) do {                                   \
    if (cond) g_pass++;                                          \
    else { g_fail++; kprintf("[PSTEST] FAIL: %s\n", (what)); }   \
} while (0)

static volatile PETHREAD g_child_et;

/* A second bare thread that ends through PsTerminateSystemThread */
static void child(void *arg)
{
    (void)arg;
    PETHREAD et = PsGetCurrentThread();
    if (et) ObReferenceObject(et);        /* the parent drops it */
    g_child_et = et;
    PsTerminateSystemThread(0x1234);
}

static void ps_test_thread(void *arg)
{
    (void)arg;
    Thread *t = sched_current();
    PETHREAD et = PsGetCurrentThread();

    CHECK(et != NULL, "bare kernel thread has an ETHREAD");
    if (!et) goto done;
    CHECK((uintptr_t)et + sizeof(ETHREAD) <= (uintptr_t)t ||
          (uintptr_t)et >= (uintptr_t)t + sizeof(Thread),
          "ETHREAD does not overlay the scheduler's Thread");
    CHECK(OBJECT_TYPE_OF(et) == ObpThreadType, "ETHREAD is a Thread object");
    CHECK(et->Kthread == t && t->ethread == et, "Thread and ETHREAD point at each other");
    CHECK(PsGetCurrentThread() == et, "second call gives the same ETHREAD");
    CHECK(PsGetCurrentProcess() == PsInitialSystemProcess, "runs in the System process");
    CHECK(et->Cid.UniqueThread == et->UniqueThread && et->UniqueThread >= 8,
          "has a TID of its own");

    void *obj = NULL;
    NTSTATUS s = ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)-2, THREAD_ALL_ACCESS,
                                           ObpThreadType, NULL, &obj, NULL);
    CHECK(NT_SUCCESS(s) && obj == et, "NtCurrentThread() resolves to it");
    if (NT_SUCCESS(s) && obj) ObDereferenceObject(obj);

    PETHREAD found = NULL;
    s = PsLookupThreadByThreadId(et->UniqueThread, &found);
    CHECK(NT_SUCCESS(s) && found == et, "PsLookupThreadByThreadId finds it");
    if (NT_SUCCESS(s) && found) ObDereferenceObject(found);

    /* Writes through the ETHREAD land in the ETHREAD */
    et->ExitStatus = (NTSTATUS)0x103;
    CHECK(t->ethread == et && t->state == THREAD_RUNNING, "Thread intact after writes");

    Thread *c = sched_create_thread("pstest-child", child, NULL, 8);
    CHECK(c != NULL, "child thread created");
    for (int i = 0; c && i < 500 && !(g_child_et && sched_thread_gone(c)); i++)
        sched_sleep_tick();
    PETHREAD cet = g_child_et;
    CHECK(cet && cet != et && cet->Kthread == c, "child has its own ETHREAD");
    CHECK(cet && cet->HasExited && cet->ExitStatus == 0x1234,
          "PsTerminateSystemThread records the exit in the child's ETHREAD");
    CHECK(c && c->state == THREAD_DEAD && c->ethread == cet, "child Thread intact");
    if (cet) ObDereferenceObject(cet);

done:
    kprintf("[PSTEST] PsGetCurrentThread self-test: %d passed, %d failed\n",
            g_pass, g_fail);
}

void PsSelfTest(void)
{
    sched_create_thread("pstest", ps_test_thread, NULL, 8);
}
