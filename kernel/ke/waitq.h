/*
 * waitq.h — wait queues: sleep until something changes, on any CPU
 *
 *     UINT32 gen = waitq_gen(&q);     read the generation first,
 *     if (condition) ...;             then look,
 *     waitq_wait(&q, gen, ticks);     and sleep until waitq_wake(&q) (or
 *                                     the ticks pass) if it wasn't so.
 *
 * A wake-up between the look and the sleep moves the generation on, so
 * the sleep returns at once: nothing is missed.  waitq_wake may be called
 * from any thread, under sleeping locks or spinlocks that aren't the
 * queue's (not from interrupt handlers).
 */
#pragma once

#include "../include/types.h"
#include "spinlock.h"

typedef struct WaitqEntry WaitqEntry;

typedef struct {
    KSpinLock          lock;
    volatile UINT32    gen;
    volatile UINT32    sleepers;
    WaitqEntry        *list;
} WaitQueue;

#define WAITQ_INIT { KSPINLOCK_INIT, 0, 0, NULL }

static inline UINT32 waitq_gen(WaitQueue *q) { return __atomic_load_n(&q->gen, __ATOMIC_ACQUIRE); }
void waitq_wait(WaitQueue *q, UINT32 gen, UINT64 max_ticks);
void waitq_wake(WaitQueue *q);
