/*
 * cpu.h — x86_64 CPU intrinsics and register access
 *
 * Inline wrappers for privileged instructions: MSRs, CR registers,
 * I/O ports, CPUID, and memory barriers. These are the low-level
 * building blocks used throughout the kernel.
 *
 * All functions are static inline so they expand at the call site
 * with no function call overhead.
 */

#pragma once

#include "../../include/types.h"

/* -----------------------------------------------------------------------
 * I/O port access
 * ----------------------------------------------------------------------- */

static __always_inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port) : "memory");
}

static __always_inline void outw(uint16_t port, uint16_t val)
{
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port) : "memory");
}

static __always_inline void outl(uint16_t port, uint32_t val)
{
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port) : "memory");
}

static __always_inline uint8_t inb(uint16_t port)
{
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port) : "memory");
    return val;
}

static __always_inline uint16_t inw(uint16_t port)
{
    uint16_t val;
    __asm__ volatile ("inw %1, %0" : "=a"(val) : "Nd"(port) : "memory");
    return val;
}

static __always_inline uint32_t inl(uint16_t port)
{
    uint32_t val;
    __asm__ volatile ("inl %1, %0" : "=a"(val) : "Nd"(port) : "memory");
    return val;
}

/* Short I/O delay via dummy port write (historical technique). */
static __always_inline void io_wait(void)
{
    outb(0x80, 0);
}

/* -----------------------------------------------------------------------
 * MSR (Model-Specific Register) access
 * ----------------------------------------------------------------------- */

/* Well-known MSR addresses */
#define MSR_IA32_APIC_BASE      0x0000001B
#define MSR_IA32_EFER           0xC0000080
#define MSR_IA32_STAR           0xC0000081
#define MSR_IA32_LSTAR          0xC0000082
#define MSR_IA32_CSTAR          0xC0000083
#define MSR_IA32_FMASK          0xC0000084
#define MSR_IA32_FSBASE         0xC0000100
#define MSR_IA32_GSBASE         0xC0000101
#define MSR_IA32_KERNEL_GSBASE  0xC0000102
#define MSR_IA32_TSC            0x00000010
#define MSR_IA32_TSC_DEADLINE   0x000006E0
#define MSR_IA32_PAT            0x00000277

/* EFER bits */
#define EFER_SCE   BIT(0)   /* SYSCALL enable */
#define EFER_LME   BIT(8)   /* Long mode enable */
#define EFER_LMA   BIT(10)  /* Long mode active (read-only) */
#define EFER_NXE   BIT(11)  /* No-execute enable */

static __always_inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr) : "memory");
    return ((uint64_t)hi << 32) | lo;
}

static __always_inline void wrmsr(uint32_t msr, uint64_t val)
{
    uint32_t lo = (uint32_t)val;
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"(lo), "d"(hi) : "memory");
}

/* -----------------------------------------------------------------------
 * Control registers
 * ----------------------------------------------------------------------- */

/* CR0 bits */
#define CR0_PE   BIT(0)   /* Protected mode */
#define CR0_MP   BIT(1)   /* Monitor co-processor */
#define CR0_EM   BIT(2)   /* Emulation (FPU) */
#define CR0_TS   BIT(3)   /* Task switched */
#define CR0_ET   BIT(4)   /* Extension type */
#define CR0_NE   BIT(5)   /* Numeric error */
#define CR0_WP   BIT(16)  /* Write protect */
#define CR0_AM   BIT(18)  /* Alignment mask */
#define CR0_NW   BIT(29)  /* Not write-through */
#define CR0_CD   BIT(30)  /* Cache disable */
#define CR0_PG   BIT(31)  /* Paging enable */

/* CR4 bits */
#define CR4_VME         BIT(0)
#define CR4_PVI         BIT(1)
#define CR4_TSD         BIT(2)
#define CR4_DE          BIT(3)
#define CR4_PSE         BIT(4)   /* Page size extension (4MB pages in legacy) */
#define CR4_PAE         BIT(5)   /* Physical address extension */
#define CR4_MCE         BIT(6)
#define CR4_PGE         BIT(7)   /* Page global enable */
#define CR4_PCE         BIT(8)
#define CR4_OSFXSR      BIT(9)   /* OS FXSave/FXRestore */
#define CR4_OSXMMEXCPT  BIT(10)  /* OS unmasked SSE exceptions */
#define CR4_UMIP        BIT(11)
#define CR4_LA57        BIT(12)  /* 5-level paging */
#define CR4_VMXE        BIT(13)
#define CR4_SMXE        BIT(14)
#define CR4_FSGSBASE    BIT(16)  /* RDFSBASE/WRFSBASE etc. */
#define CR4_PCIDE       BIT(17)  /* PCID enable */
#define CR4_OSXSAVE     BIT(18)
#define CR4_SMEP        BIT(20)  /* Supervisor mode exec protection */
#define CR4_SMAP        BIT(21)  /* Supervisor mode access protection */

static __always_inline uint64_t read_cr0(void)
{
    uint64_t v;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(v));
    return v;
}

static __always_inline void write_cr0(uint64_t v)
{
    __asm__ volatile ("mov %0, %%cr0" : : "r"(v) : "memory");
}

static __always_inline uint64_t read_cr2(void)
{
    uint64_t v;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(v));
    return v;
}

static __always_inline uint64_t read_cr3(void)
{
    uint64_t v;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(v));
    return v;
}

static __always_inline void write_cr3(uint64_t v)
{
    __asm__ volatile ("mov %0, %%cr3" : : "r"(v) : "memory");
}

static __always_inline uint64_t read_cr4(void)
{
    uint64_t v;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(v));
    return v;
}

static __always_inline void write_cr4(uint64_t v)
{
    __asm__ volatile ("mov %0, %%cr4" : : "r"(v) : "memory");
}

/* -----------------------------------------------------------------------
 * CPUID
 * ----------------------------------------------------------------------- */

typedef struct {
    uint32_t eax, ebx, ecx, edx;
} CpuidResult;

static __always_inline CpuidResult cpuid(uint32_t leaf, uint32_t subleaf)
{
    CpuidResult r;
    __asm__ volatile (
        "cpuid"
        : "=a"(r.eax), "=b"(r.ebx), "=c"(r.ecx), "=d"(r.edx)
        : "a"(leaf), "c"(subleaf)
    );
    return r;
}

/* -----------------------------------------------------------------------
 * Interrupt control
 * ----------------------------------------------------------------------- */

static __always_inline void sti(void) { __asm__ volatile ("sti" ::: "memory"); }
static __always_inline void cli(void) { __asm__ volatile ("cli" ::: "memory"); }
static __always_inline void hlt(void) { __asm__ volatile ("hlt"); }
static __always_inline void pause_cpu(void) { __asm__ volatile ("pause"); }

static __always_inline uint64_t read_rflags(void)
{
    uint64_t f;
    __asm__ volatile ("pushfq; popq %0" : "=r"(f) :: "memory");
    return f;
}

#define RFLAGS_IF BIT(9)

static __always_inline bool interrupts_enabled(void)
{
    return !!(read_rflags() & RFLAGS_IF);
}

/* Save/restore interrupt state (for spinlocks, critical sections). */
typedef uint64_t IrqState;

static __always_inline IrqState irq_save(void)
{
    IrqState s = read_rflags();
    cli();
    return s;
}

static __always_inline void irq_restore(IrqState s)
{
    if (s & RFLAGS_IF) sti();
}

/* -----------------------------------------------------------------------
 * Cache and memory barriers
 * ----------------------------------------------------------------------- */

static __always_inline void mfence(void)
{
    __asm__ volatile ("mfence" ::: "memory");
}

static __always_inline void sfence(void)
{
    __asm__ volatile ("sfence" ::: "memory");
}

static __always_inline void lfence(void)
{
    __asm__ volatile ("lfence" ::: "memory");
}

static __always_inline void invlpg(uintptr_t va)
{
    __asm__ volatile ("invlpg (%0)" : : "r"(va) : "memory");
}

/* Compiler barrier only (no CPU fence) */
#define barrier() __asm__ volatile ("" ::: "memory")

/* -----------------------------------------------------------------------
 * TSC
 * ----------------------------------------------------------------------- */

static __always_inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* -----------------------------------------------------------------------
 * Misc
 * ----------------------------------------------------------------------- */

static __always_inline void wbinvd(void)
{
    __asm__ volatile ("wbinvd" ::: "memory");
}

/* Halt the CPU forever — for unrecoverable errors */
static __noreturn __always_inline void cpu_halt_forever(void)
{
    for (;;) {
        cli();
        hlt();
    }
}

/* Read the current stack pointer */
static __always_inline uintptr_t read_rsp(void)
{
    uintptr_t v;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(v));
    return v;
}
