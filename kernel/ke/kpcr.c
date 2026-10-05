/*
 * kpcr.c — Kernel Processor Control Region
 *
 * Each CPU has a KPCR in g_kpcr[], found through GS in kernel mode (see
 * kpcr.h for the GS convention).
 */

#include "kpcr.h"
#include "printf.h"
#include "../include/types.h"
#include "../arch/x86_64/cpu.h"

KPCR g_kpcr[MAX_CPUS];
volatile UINT32 g_cpu_count = 1;

void KiInitializeKpcr(PKPCR kpcr, UINT32 cpu)
{
    __builtin_memset(kpcr, 0, sizeof(KPCR));
    kpcr->Self      = kpcr;
    kpcr->CpuNumber = cpu;

    wrmsr(MSR_GS_BASE, (UINT64)(uintptr_t)kpcr);      /* kernel GS: this KPCR */
    wrmsr(MSR_KERNEL_GS_BASE, 0);                     /* user GS until a thread sets it */
}

void *KiGetCurrentThread(void)
{
    /* A preempted caller can resume on another CPU. Reading the KPCR
     * self-pointer and then dereferencing it could return the old CPU's
     * new thread (including its idle thread). Read the current thread
     * from the live GS base in one instruction instead. */
    void *thread;
    __asm__ volatile ("mov %%gs:%c1, %0" : "=r"(thread) : "i"(KPCR_CURRENT_THREAD) : "memory");
    return thread;
}

void KiSetCurrentThread(void *Thread)
{
    KiGetCurrentKpcr()->CurrentThread = Thread;
}
