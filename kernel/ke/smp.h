/*
 * smp.h — multiprocessor support: the other CPUs and the big kernel lock
 *
 * NovaOS runs threads on every CPU the firmware reports (ACPI MADT).
 *
 * Kernel code that has no lock of its own runs under the big kernel lock
 * (BKL).  It belongs to threads: bkl_acquire/bkl_release nest (per-thread
 * depth), and the scheduler drops a holder's lock when it switches the
 * thread out and takes it back when it resumes.  System calls and
 * interrupts from user mode take it on the way in and drop it on the way
 * out — except those that run under finer-grained locks (smp.c has the
 * list of what does): the scheduler, the timer and IPIs, and the services
 * um_syscall.c marks lock-free (waits and events, memory, sockets, the GUI,
 * time, files, the registry, the console, starting processes and threads),
 * plus the desktop's drawing.
 *
 * Locking order: the BKL and the sleeping locks (UmLock and UmRwLock:
 * DesktopLock, the file-system lock, process locks, the registry's,
 * net_lock) may be taken in either order, because waiting for one yields,
 * and yielding hands over the BKL.  Among the sleeping locks: the desktop,
 * then the file system (FsLock: exclusive to change the tree, shared to
 * read and write open files), then a file's own lock, then a process's
 * (exclusive; looking up, opening and closing a handle take it shared
 * plus the handle's slot lock); the registry's g_reg, then a key's lock,
 * then its watch lock.  Saving drive C: (fs/persist.c) takes the file
 * system, then its save lock, and writes the disk holding only the latter;
 * the disk drivers (AHCI, NVMe, USB storage) take a lock per disk, so
 * their callers need no BKL.  Spinlocks (spinlock.h) come last: nothing
 * that sleeps or takes the BKL while one is held.
 *
 * The terminal's "profile" command samples where the CPUs spend their time
 * (prof.c): kernel functions and their callers, waits for the BKL by who
 * waits, programs' code, and the system calls made.
 */

#pragma once

#include "../include/types.h"
#include "../../include/boot_protocol.h"

/* IPI vectors */
#define IPI_WAKE  0xF0   /* leave a halt: a thread became ready, or the kernel lock is free; or switch to a woken thread */
#define IPI_TLB   0xF1   /* flush the TLB (see smp_tlb_flush) */

struct Thread;

/* Big kernel lock (the calling thread's; nests) */
void bkl_acquire(void);
void bkl_release(void);
void bkl_acquire_boot(void);   /* a starting CPU's first: never enables interrupts */
void bkl_leave_kernel(void);   /* on the way to user mode: drop it however deep */
bool bkl_held(void);
/* Long work that needs no big lock (the program loader's copying): let go
 * of it however deep, then take it back to that depth. */
uint32_t bkl_drop(void);
void     bkl_restore(uint32_t depth);           /* by the calling thread */
/* The scheduler (interrupts off): @t is switched out / back in */
void bkl_switch_out(struct Thread *t);
void bkl_switch_in(struct Thread *t);
/* If another CPU wants the lock, let it have it, then take it back.  Only
 * where other threads could run anyway (the scheduler's yield points). */
void bkl_relax(void);

/* Halt until the next interrupt, without the BKL if the caller holds it.
 * For idle loops. */
void cpu_idle_wait(void);

/* Early boot (after the PMM): note the RSDP and check the low pages the
 * start-up code needs. */
void smp_early(const BootInfo *info);
/* Start the other CPUs (after the scheduler, syscalls and SSE are set up). */
uint32_t smp_start(void);                 /* the number of CPUs running (or about to) */

/* Point the start-up page at @entry (with RSP = @stack_top, RDI = @arg)
 * and return its physical address, or 0 if the page isn't available */
uint32_t smp_trampoline(void (*entry)(uint64_t), void *stack_top, uint64_t arg);
/* The start-up page's 32-bit protected-mode entry (an S3 waking vector) */
uint32_t smp_trampoline_wake32(void);
/* INIT and STARTUP IPIs to @apic; true once the CPU sets *@started */
bool smp_start_cpu(uint8_t apic, volatile uint32_t *started);

/* A thread became ready in CPU @prefer's queue: wake that CPU if it is
 * halted, else any halted CPU (it will take the thread from the queue).
 * False when no CPU was halted. */
bool smp_kick(uint32_t prefer);
/* Page table entries changed: other CPUs running with page table @cr3 flush
 * their TLB before this returns.  cr3 = 0: every CPU, global pages too. */
void smp_tlb_flush(uint64_t cr3);

/* Handle IPI_WAKE / IPI_TLB (interrupt context, without the BKL). */
void smp_ipi(uint64_t vector);
