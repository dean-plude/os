/*
 * spinlock.h — spinlocks for short critical sections on any CPU
 *
 * A ticket lock, held with interrupts off on this CPU (so a holder is never
 * preempted, and an interrupt handler on this CPU never spins on a lock its
 * own CPU holds).  While it waits, a CPU still answers TLB shootdown
 * requests (the holder may be waiting for one to be answered).
 *
 * Rules: hold it briefly; never sleep, yield, take the big kernel lock or a
 * sleeping lock (UmLock) while holding it.
 */

#pragma once

#include "../include/types.h"
#include "../arch/x86_64/cpu.h"

typedef struct {
    volatile uint32_t next;
    volatile uint32_t owner;
} KSpinLock;

#define KSPINLOCK_INIT { 0, 0 }

void smp_poll_tlb(void);   /* smp.c: answer a pending TLB flush request */

/* With interrupts already off */
static inline void spin_lock(KSpinLock *l)
{
    uint32_t t = __atomic_fetch_add(&l->next, 1, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&l->owner, __ATOMIC_ACQUIRE) != t) {
        smp_poll_tlb();
        pause_cpu();
    }
}

static inline void spin_unlock(KSpinLock *l)
{
    __atomic_fetch_add(&l->owner, 1, __ATOMIC_RELEASE);
}

static inline bool spin_trylock(KSpinLock *l)
{
    uint32_t o = __atomic_load_n(&l->owner, __ATOMIC_ACQUIRE);
    uint32_t n = o;
    return __atomic_compare_exchange_n(&l->next, &n, o + 1, false,
                                       __ATOMIC_SEQ_CST, __ATOMIC_RELAXED);
}

static inline IrqState spin_lock_irqsave(KSpinLock *l)
{
    IrqState s = irq_save();
    spin_lock(l);
    return s;
}

static inline void spin_unlock_irqrestore(KSpinLock *l, IrqState s)
{
    spin_unlock(l);
    irq_restore(s);
}
