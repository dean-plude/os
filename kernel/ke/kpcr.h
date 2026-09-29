/*
 * kpcr.h — Kernel Processor Control Region (KPCR)
 *
 * One KPCR per logical CPU.  In kernel mode GS always points at the
 * current CPU's KPCR:
 *
 *   in kernel mode:  MSR_GS_BASE = KPCR,  MSR_KERNEL_GS_BASE = user GS (TEB)
 *   in user mode:    MSR_GS_BASE = TEB,   MSR_KERNEL_GS_BASE = KPCR
 *
 * Every way into the kernel from ring 3 (SYSCALL, and interrupts or
 * exceptions whose saved CS has RPL 3) begins with SWAPGS, and every way
 * back ends with one.  Interrupts taken in kernel mode leave GS alone.
 *
 * KPCR layout (offsets must stay stable — assembly references them):
 *   +0x00  Self          (pointer to this KPCR, for validation)
 *   +0x08  CurrentThread (PETHREAD of the running thread)
 *   +0x10  IdleThread    (PETHREAD of this CPU's idle thread)
 *   +0x18  Tss           (pointer to TSS64)
 *   +0x20  UserRsp       (saved user RSP on syscall entry)
 *   +0x28  KernelRsp     (kernel stack pointer for syscall path)
 *
 * Assembly (syscall_entry.asm) references:
 *   KPCR_CURRENT_THREAD  equ 0x08
 *   KPCR_USER_RSP        equ 0x20
 *   KPCR_KERNEL_RSP      equ 0x28
 */

#pragma once

#include "../include/types.h"

/* -----------------------------------------------------------------------
 * KPCR offsets (must match the struct layout exactly)
 * ----------------------------------------------------------------------- */
#define KPCR_SELF           0x00
#define KPCR_CURRENT_THREAD 0x08
#define KPCR_IDLE_THREAD    0x10
#define KPCR_TSS            0x18
#define KPCR_USER_RSP       0x20
#define KPCR_KERNEL_RSP     0x28

/* -----------------------------------------------------------------------
 * MSR addresses
 * ----------------------------------------------------------------------- */
#define MSR_GS_BASE         0xC0000101  /* Current GS base */
#define MSR_KERNEL_GS_BASE  0xC0000102  /* Kernel GS base (used with SWAPGS) */

/* -----------------------------------------------------------------------
 * KPCR structure
 * ----------------------------------------------------------------------- */
typedef struct _KPCR {
    struct _KPCR *Self;          /* +0x00 Self-pointer */
    void         *CurrentThread; /* +0x08 Current Thread* */
    void         *IdleThread;    /* +0x10 This CPU's idle Thread* */
    void         *Tss;           /* +0x18 TSS64* */
    UINT64        UserRsp;       /* +0x20 Saved user RSP (set by syscall entry) */
    UINT64        KernelRsp;     /* +0x28 Kernel RSP for syscall path */
    UINT32        CpuNumber;     /* +0x30 Logical CPU index (0 = boot CPU) */
    UINT32        ApicId;        /* +0x34 Local APIC ID */
    void         *Gdt;           /* +0x38 CpuGdt* */
    volatile UINT32 Idle;        /* +0x40 halted in cpu_idle_wait (wake with an IPI) */
    volatile UINT32 TlbFlush;    /* +0x44 another CPU asked for a TLB flush */
    volatile UINT32 Online;      /* +0x48 running the scheduler */
    volatile UINT32 LockWait;    /* +0x4C halted waiting for the kernel lock */
    void         *PrevThread;    /* +0x50 the thread a switch in progress left */
} KPCR, *PKPCR;

#define MAX_CPUS 16

/* One KPCR per CPU, indexed by CpuNumber; g_cpu_count are in use. */
extern KPCR g_kpcr[MAX_CPUS];
extern volatile UINT32 g_cpu_count;

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

/*
 * Make @kpcr this CPU's KPCR: MSR_GS_BASE = kpcr (the kernel runs with GS
 * on the KPCR), MSR_KERNEL_GS_BASE = 0 (the user GS, swapped in on the way
 * to user mode).  Called early on each CPU, before interrupts.
 */
void KiInitializeKpcr(PKPCR kpcr, UINT32 cpu);

/* The current CPU's KPCR (GS-relative: valid anywhere in kernel mode). */
static inline __attribute__((always_inline)) PKPCR KiGetCurrentKpcr(void)
{
    PKPCR p;
    __asm__ volatile ("mov %%gs:0, %0" : "=r"(p));
    return p;
}

void *KiGetCurrentThread(void);
void KiSetCurrentThread(void *Thread);
