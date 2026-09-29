/*
 * smp.h — multiprocessor support: the other CPUs and the big kernel lock
 *
 * NovaOS runs threads on every CPU the firmware reports (ACPI MADT).  The
 * kernel itself is serialized by one lock, the big kernel lock (BKL):
 *
 *   - a CPU holds the BKL whenever it runs kernel code;
 *   - it takes it on every way in from user mode (SYSCALL, interrupts and
 *     exceptions from ring 3) and drops it on every way back;
 *   - a context switch hands it from one thread to the next on the same CPU;
 *   - cpu_idle_wait() drops it while the CPU halts.
 *
 * So kernel data structures see one CPU at a time, as before, while
 * programs' own code runs on all CPUs at once.  Code that runs without the
 * BKL: the IPI handlers below and the short entry/exit paths around it.
 */

#pragma once

#include "../include/types.h"
#include "../../include/boot_protocol.h"

/* IPI vectors */
#define IPI_WAKE  0xF0   /* leave a halt: a thread became ready, or the kernel lock is free */
#define IPI_TLB   0xF1   /* flush the TLB (see smp_tlb_flush) */

/* Big kernel lock.  Call bkl_acquire with interrupts off; it may enable
 * them while it waits (see smp.c), and returns with them off. */
void bkl_acquire(void);
void bkl_acquire_boot(void);   /* the same, never enabling interrupts */
void bkl_release(void);
bool bkl_held(void);           /* by this CPU */
/* If another CPU wants the lock, let it have it, then take it back.  Only
 * where other threads could run anyway (the scheduler's yield points). */
void bkl_relax(void);

/* Halt until the next interrupt without holding the BKL (the caller holds
 * it before and after).  For idle loops. */
void cpu_idle_wait(void);

/* Early boot (after the PMM): note the RSDP and check the low pages the
 * start-up code needs. */
void smp_early(const BootInfo *info);
/* Start the other CPUs (after the scheduler, syscalls and SSE are set up). */
void smp_start(void);

/* A thread became ready: wake one halted CPU to run it. */
void smp_kick(void);
/* Page table entries changed: other CPUs running with page table @cr3 flush
 * their TLB before this returns.  cr3 = 0: every CPU, global pages too. */
void smp_tlb_flush(uint64_t cr3);

/* Handle IPI_WAKE / IPI_TLB (interrupt context, without the BKL). */
void smp_ipi(uint64_t vector);
