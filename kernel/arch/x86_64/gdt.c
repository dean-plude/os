/*
 * gdt.c — GDT and TSS initialization
 *
 * We allocate a static CpuGdt for the boot CPU.  When SMP is added
 * each CPU will get its own GDT (the TSS is per-CPU).
 */

#include "gdt.h"
#include "cpu.h"

/* Dedicated double-fault and NMI stacks (16 KiB each).
 * These are used via the IST mechanism so a stack overflow or corrupted
 * RSP can't prevent the handler from running. */
#define EXCEPTION_STACK_SIZE 0x4000   /* 16 KiB */

static uint8_t __aligned(16) double_fault_stack[EXCEPTION_STACK_SIZE];
static uint8_t __aligned(16) nmi_stack[EXCEPTION_STACK_SIZE];
static uint8_t __aligned(16) machine_check_stack[EXCEPTION_STACK_SIZE];
static uint8_t __aligned(16) debug_stack[EXCEPTION_STACK_SIZE];

/* The boot CPU's GDT + TSS */
static CpuGdt __aligned(16) boot_gdt;

/* -----------------------------------------------------------------------
 * Descriptor constructors
 * ----------------------------------------------------------------------- */

/* Build a regular 64-bit code/data descriptor.
 *
 * In 64-bit mode, base and limit are mostly ignored (the CPU treats them
 * as 0 and 0xFFFFFFFF respectively in 64-bit CS), but some fields still
 * need to be set correctly:
 *
 *   access byte:
 *     bit 7: Present
 *     bits 6-5: DPL
 *     bit 4: S=1 (code/data, not system)
 *     bits 3-0: Type
 *       0xA = Code, readable, non-conforming
 *       0x2 = Data, writable, expand-up
 *
 *   granularity byte:
 *     bit 7: G (granularity — doesn't matter in 64-bit)
 *     bit 6: DB (default operation size — must be 0 for 64-bit CS)
 *     bit 5: L  (long mode — must be 1 for 64-bit CS)
 *     bit 4: AVL
 *     bits 3-0: Limit high (doesn't matter in 64-bit)
 */
static GdtEntry64 make_code64(uint8_t dpl)
{
    return (GdtEntry64){
        .limit_low   = 0xFFFF,
        .base_low    = 0,
        .base_mid    = 0,
        .access      = (uint8_t)(0x9A | (dpl << 5)),  /* P=1, S=1, Type=0xA (exec+read) */
        .granularity = 0x20,   /* L=1 (64-bit), G=0, DB=0 */
        .base_high   = 0,
    };
}

static GdtEntry64 make_data64(uint8_t dpl)
{
    return (GdtEntry64){
        .limit_low   = 0xFFFF,
        .base_low    = 0,
        .base_mid    = 0,
        .access      = (uint8_t)(0x92 | (dpl << 5)),  /* P=1, S=1, Type=0x2 (data+write) */
        .granularity = 0x00,   /* L=0 for data (DB=0 is fine) */
        .base_high   = 0,
    };
}

static GdtEntry64 make_null(void)
{
    return (GdtEntry64){0};
}

/* Build the 16-byte TSS system descriptor.
 * The TSS descriptor is unusual: it's 16 bytes (two consecutive 8-byte
 * slots in the GDT) and encodes the full 64-bit TSS base address. */
static TssDescriptor make_tss_descriptor(uintptr_t tss_addr, uint16_t tss_limit)
{
    return (TssDescriptor){
        .limit_low   = tss_limit,
        .base_low    = (uint16_t)(tss_addr & 0xFFFF),
        .base_mid    = (uint8_t)((tss_addr >> 16) & 0xFF),
        .access      = 0x89,   /* Present, DPL=0, Type=9 (64-bit TSS Available) */
        .flags       = 0x00,
        .base_high   = (uint8_t)((tss_addr >> 24) & 0xFF),
        .base_upper  = (uint32_t)(tss_addr >> 32),
        .reserved    = 0,
    };
}

/* -----------------------------------------------------------------------
 * gdt_init
 * ----------------------------------------------------------------------- */
void gdt_init(void)
{
    CpuGdt *g = &boot_gdt;

    /* Segment descriptors */
    g->entries[0] = make_null();                /* 0x00 — Null */
    g->entries[1] = make_code64(0);             /* 0x08 — Kernel code, DPL=0 */
    g->entries[2] = make_data64(0);             /* 0x10 — Kernel data, DPL=0 */
    g->entries[3] = make_code64(3);             /* 0x18 — User code,   DPL=3 */
    g->entries[4] = make_data64(3);             /* 0x20 — User data,   DPL=3 */
    /* Entries 5 and 6 (0x28 and 0x30) are occupied by the 16-byte TSS
     * descriptor — written separately via g->tss_descriptor below. */
    g->entries[5] = make_null();                /* placeholder — TSS low  */
    g->entries[6] = make_null();                /* placeholder — TSS high */

    /* Initialize the TSS */
    Tss64 *tss = &g->tss;
    __builtin_memset(tss, 0, sizeof(*tss));

    /* RSP0: the kernel stack used when entering ring 0 from ring 3.
     * For now point it to the top of the double-fault stack — the real
     * per-thread RSP0 will be set in the scheduler when switching tasks. */
    tss->rsp[0] = (uint64_t)(double_fault_stack + EXCEPTION_STACK_SIZE);

    /* IST entries — each points to the top (high address) of its stack */
    tss->ist[IST_DOUBLE_FAULT - 1] = (uint64_t)(double_fault_stack + EXCEPTION_STACK_SIZE);
    tss->ist[IST_NMI          - 1] = (uint64_t)(nmi_stack          + EXCEPTION_STACK_SIZE);
    tss->ist[IST_MACHINE_CHECK - 1] = (uint64_t)(machine_check_stack + EXCEPTION_STACK_SIZE);
    tss->ist[IST_DEBUG        - 1] = (uint64_t)(debug_stack         + EXCEPTION_STACK_SIZE);

    /* I/O permission bitmap — set offset past the TSS end so all I/O
     * from ring 3 causes a GPF (we'll add selective I/O permission later). */
    tss->iopb_offset = sizeof(Tss64);

    /* Write the TSS descriptor into the GDT.
     * The GDT layout has entries[5] at offset 0x28 and entries[6] at 0x30.
     * The TssDescriptor is 16 bytes and overlaps both slots. */
    g->tss_descriptor = make_tss_descriptor((uintptr_t)tss, sizeof(Tss64) - 1);

    /* Copy the 16-byte TSS descriptor over entries[5..6].
     * This is legal because both are the same size (2 × 8 bytes = 16 bytes). */
    __builtin_memcpy(&g->entries[5], &g->tss_descriptor, sizeof(TssDescriptor));

    /* Load the GDTR */
    Gdtr gdtr = {
        .limit = sizeof(g->entries) - 1,
        .base  = (uint64_t)(uintptr_t)g->entries,
    };
    __asm__ volatile ("lgdt %0" : : "m"(gdtr) : "memory");

    /* Load TSS selector (LTR instruction) */
    __asm__ volatile ("ltr %0" : : "r"((uint16_t)GDT_TSS) : "memory");

    gdt_reload_segments();
}

/* -----------------------------------------------------------------------
 * gdt_reload_segments
 *
 * After LGDT we must reload all segment registers to point at the new
 * descriptors.  CS cannot be loaded with MOV — we use a far return trick.
 * ----------------------------------------------------------------------- */
void gdt_reload_segments(void)
{
    __asm__ volatile (
        /* Push new CS and the address of the 'lretq' target onto the stack,
         * then far-return to atomically reload CS. */
        "pushq %0\n\t"                  /* CS = GDT_KERNEL_CODE */
        "lea 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        /* Reload all data segment registers */
        "mov %1, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%ss\n\t"
        /* FS and GS are zeroed; they'll be set per-thread for TEB/PEB access */
        "xor %%eax, %%eax\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        :
        : "i"(GDT_KERNEL_CODE), "i"(GDT_KERNEL_DATA)
        : "rax", "memory"
    );
}

/* -----------------------------------------------------------------------
 * gdt_set_rsp0
 *
 * Called by the scheduler on every context switch to update the kernel
 * stack pointer in the TSS.  When the CPU delivers an interrupt while
 * executing ring-3 code, it automatically switches to RSP0.
 * ----------------------------------------------------------------------- */
void gdt_set_rsp0(uintptr_t rsp0)
{
    boot_gdt.tss.rsp[0] = rsp0;
}
