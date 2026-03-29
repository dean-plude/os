/*
 * kpcr.h — Kernel Processor Control Region (KPCR)
 *
 * One KPCR per logical CPU.  The current CPU's KPCR is always accessible
 * via the GS segment in kernel mode:
 *
 *   MSR_KERNEL_GS_BASE (0xC0000102) = physical/virtual address of KPCR
 *
 * On SYSCALL entry (KiSystemCall64):
 *   SWAPGS — exchanges MSR_GS_BASE with MSR_KERNEL_GS_BASE
 *            → GS now points at KPCR
 * On SYSRET exit:
 *   SWAPGS — restores user GS (TEB pointer)
 *
 * On hardware interrupt entry (already in kernel):
 *   GS already points at KPCR — no SWAPGS needed.
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
    void         *CurrentThread; /* +0x08 Current ETHREAD* */
    void         *IdleThread;    /* +0x10 Idle ETHREAD* */
    void         *Tss;           /* +0x18 TSS64* */
    UINT64        UserRsp;       /* +0x20 Saved user RSP (set by syscall entry) */
    UINT64        KernelRsp;     /* +0x28 Kernel RSP for syscall path */
    UINT32        CpuNumber;     /* +0x30 Logical CPU index */
    UINT32        _pad;
} KPCR, *PKPCR;

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

/*
 * Initialize the KPCR for the boot CPU.
 * Allocates a KPCR, sets MSR_KERNEL_GS_BASE.
 * Must be called after VMM (heap) is up.
 */
void KiInitializeKpcr(void);

/*
 * Return the current CPU's KPCR.
 * Uses RDGSBASE / reads MSR_KERNEL_GS_BASE (before first SWAPGS).
 */
PKPCR KiGetCurrentKpcr(void);

/*
 * Fast accessor: read the current thread from GS-relative offset.
 * Used by PsGetCurrentThread() after SWAPGS is active.
 * Returns NULL before KiInitializeKpcr() is called.
 */
void *KiGetCurrentThread(void);

/*
 * Set the current thread in the KPCR.
 * Called by the scheduler on every context switch.
 */
void KiSetCurrentThread(void *Thread);
