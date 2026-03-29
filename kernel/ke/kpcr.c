/*
 * kpcr.c — Kernel Processor Control Region
 *
 * Sets up the per-CPU KPCR and programs MSR_KERNEL_GS_BASE so that
 * SWAPGS in syscall_entry.asm makes GS point to the KPCR.
 *
 * After KiInitializeKpcr():
 *   MSR_KERNEL_GS_BASE = &g_kpcr (kernel virtual address)
 *   MSR_GS_BASE        = 0 (user GS, will be set to TEB on user-mode entry)
 *
 * The KPCR is a single static allocation for the boot CPU (Phase 5).
 * SMP support will need one KPCR per logical CPU.
 */

#include "kpcr.h"
#include "printf.h"
#include "../include/types.h"
#include "../arch/x86_64/cpu.h"
#include "../mm/vmm.h"

/* -----------------------------------------------------------------------
 * Boot CPU KPCR
 * Static allocation — no heap needed for Phase 5 single-CPU path.
 * ----------------------------------------------------------------------- */
static KPCR g_boot_kpcr;

/* -----------------------------------------------------------------------
 * KiInitializeKpcr
 * ----------------------------------------------------------------------- */
void KiInitializeKpcr(void)
{
    PKPCR kpcr = &g_boot_kpcr;
    __builtin_memset(kpcr, 0, sizeof(KPCR));

    kpcr->Self      = kpcr;
    kpcr->CpuNumber = 0;

    /* MSR_KERNEL_GS_BASE = kernel virtual address of KPCR.
     * SWAPGS swaps MSR_GS_BASE ↔ MSR_KERNEL_GS_BASE.
     * On syscall entry: SWAPGS makes GS point to KPCR.
     * On sysret exit:   SWAPGS restores user GS (TEB).
     */
    wrmsr(MSR_KERNEL_GS_BASE, (UINT64)(uintptr_t)kpcr);

    /* MSR_GS_BASE = 0 for now (user GS is 0 until first user thread runs) */
    wrmsr(MSR_GS_BASE, 0);

    kprintf("[KPCR] Boot CPU KPCR at %p, MSR_KERNEL_GS_BASE=0x%llx\n",
            (void *)kpcr, (unsigned long long)(uintptr_t)kpcr);
}

/* -----------------------------------------------------------------------
 * KiGetCurrentKpcr — read directly from MSR_KERNEL_GS_BASE
 * (valid before the first SWAPGS; after SWAPGS use GS segment)
 * ----------------------------------------------------------------------- */
PKPCR KiGetCurrentKpcr(void)
{
    return (PKPCR)(uintptr_t)rdmsr(MSR_KERNEL_GS_BASE);
}

/* -----------------------------------------------------------------------
 * KiGetCurrentThread
 * ----------------------------------------------------------------------- */
void *KiGetCurrentThread(void)
{
    PKPCR kpcr = KiGetCurrentKpcr();
    return kpcr ? kpcr->CurrentThread : NULL;
}

/* -----------------------------------------------------------------------
 * KiSetCurrentThread
 * ----------------------------------------------------------------------- */
void KiSetCurrentThread(void *Thread)
{
    PKPCR kpcr = KiGetCurrentKpcr();
    if (kpcr) kpcr->CurrentThread = Thread;
}
