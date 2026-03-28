/*
 * main.c — KiSystemStartup: the C-level kernel entry point
 *
 * This is called from the assembly stub in entry.asm after the BSS has
 * been zeroed and a temporary stack has been set up.
 *
 * Initialization order is critical:
 *
 *   1. Serial port  — must be first; gives us debug output from the start
 *   2. Framebuffer  — early visual output
 *   3. PMM          — physical memory manager (reads the memory map)
 *   4. Paging       — takes ownership of page tables from bootloader;
 *                     enables NX; PHYSMAP must be live before this
 *   5. VMM          — kernel heap (depends on PMM + paging)
 *   6. GDT          — reload segments; set up TSS/IST stacks
 *   7. IDT          — exception/interrupt handlers
 *   8. APIC         — disable legacy PIC; enable APIC timer
 *   9. Scheduler    — create idle thread; ready for preemption
 *  10. Enable IRQs  — sti()
 *  11. Test threads — demonstrate the scheduler working
 *  12. Idle loop    — kernel main loop (yields to scheduler)
 *
 * After step 10, the APIC timer fires at 100 Hz and the scheduler
 * preempts threads.  The kernel is "alive".
 *
 * Phase 2 will add the NT Object Manager and process/thread creation here.
 */

#include "../include/types.h"
#include "../../include/boot_protocol.h"
#include "../hal/serial.h"
#include "../hal/framebuffer.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../arch/x86_64/gdt.h"
#include "../arch/x86_64/idt.h"
#include "../arch/x86_64/apic.h"
#include "../arch/x86_64/paging.h"
#include "../arch/x86_64/cpu.h"
#include "printf.h"
#include "scheduler.h"
#include "syscall.h"
#include "../ob/ob.h"
#include "../ps/ps.h"
#include "../cm/cm.h"
#include "../se/se.h"
#include "../io/io.h"
#include "../mm/vma.h"
#include "../mm/section.h"
#include "../ldr/ldr.h"

/* -----------------------------------------------------------------------
 * Banner
 * ----------------------------------------------------------------------- */
static void print_banner(void)
{
    fb_set_colors(FB_YELLOW, FB_BOOT_BG);
    kprintf("\n");
    kprintf("  ███╗   ██╗ ██████╗ ██╗   ██╗ █████╗  ██████╗ ███████╗\n");
    kprintf("  ████╗  ██║██╔═══██╗██║   ██║██╔══██╗██╔═══██╗██╔════╝\n");
    kprintf("  ██╔██╗ ██║██║   ██║██║   ██║███████║██║   ██║███████╗\n");
    kprintf("  ██║╚██╗██║██║   ██║╚██╗ ██╔╝██╔══██║██║   ██║╚════██║\n");
    kprintf("  ██║ ╚████║╚██████╔╝ ╚████╔╝ ██║  ██║╚██████╔╝███████║\n");
    kprintf("  ╚═╝  ╚═══╝ ╚═════╝   ╚═══╝  ╚═╝  ╚═╝ ╚═════╝ ╚══════╝\n");
    kprintf("\n");
    fb_set_colors(FB_BOOT_FG, FB_BOOT_BG);
    kprintf("  Windows-compatible OS kernel  [Phase 3 — PE Loader & I/O Manager]\n");
    kprintf("  Built: " __DATE__ " " __TIME__ "\n");
    kprintf("\n");
}

/* -----------------------------------------------------------------------
 * Demo threads — Phase 1 smoke test
 * ----------------------------------------------------------------------- */
static void thread_a(void *arg)
{
    (void)arg;
    for (int i = 0; i < 5; i++) {
        kprintf("[Thread A] iteration %d (TID=%lu)\n",
                i, sched_current()->tid);
        sched_yield();
    }
    kprintf("[Thread A] done\n");
}

static void thread_b(void *arg)
{
    (void)arg;
    for (int i = 0; i < 5; i++) {
        kprintf("[Thread B] iteration %d (TID=%lu)\n",
                i, sched_current()->tid);
        sched_yield();
    }
    kprintf("[Thread B] done\n");
}

static void memory_test_thread(void *arg)
{
    (void)arg;
    kprintf("[MemTest] Testing kernel heap...\n");

    /* Allocate and free various sizes */
    void *ptrs[16];
    static const size_t sizes[] = { 8, 16, 32, 64, 128, 256, 512, 1024,
                                    2048, 4096, 8192, 16384,
                                    32, 64, 128, 256 };
    for (int i = 0; i < 16; i++) {
        ptrs[i] = kmalloc(sizes[i]);
        if (ptrs[i]) {
            /* Write and verify a pattern */
            memset(ptrs[i], (int)(0xAA + i), sizes[i]);
        } else {
            kprintf("[MemTest] FAIL: kmalloc(%zu) returned NULL\n", sizes[i]);
        }
    }

    /* Verify patterns */
    bool ok = true;
    for (int i = 0; i < 16; i++) {
        if (!ptrs[i]) continue;
        uint8_t *p = ptrs[i];
        for (size_t j = 0; j < sizes[i]; j++) {
            if (p[j] != (uint8_t)(0xAA + i)) {
                kprintf("[MemTest] FAIL: corruption at ptrs[%d][%zu]\n", i, j);
                ok = false;
                break;
            }
        }
    }

    /* Free all */
    for (int i = 0; i < 16; i++) {
        if (ptrs[i]) kfree(ptrs[i]);
    }

    if (ok) {
        kprintf("[MemTest] All allocations OK\n");
    }

    /* PMM stats */
    uint64_t total, free, used;
    pmm_stats(&total, &free, &used);
    kprintf("[MemTest] PMM: %lu MiB total, %lu MiB free, %lu MiB used\n",
            (total * PAGE_SIZE) >> 20,
            (free  * PAGE_SIZE) >> 20,
            (used  * PAGE_SIZE) >> 20);
}

/* -----------------------------------------------------------------------
 * KiSystemStartup — C kernel entry
 *
 * @info: physical address of BootInfo struct
 *        (mapped via physmap: PHYSMAP_BASE + phys_addr)
 * ----------------------------------------------------------------------- */
void __attribute__((noreturn)) KiSystemStartup(const BootInfo *info_phys)
{
    /* ----------------------------------------------------------------
     * STEP 1: Serial port — first thing, no heap, no paging needed
     * ---------------------------------------------------------------- */
    serial_init(SERIAL_COM1_BASE);
    early_printf("\r\n[NovaOS] Serial console active\r\n");
    early_printf("[NovaOS] Kernel entry at %p\r\n", (void *)KiSystemStartup);

    /* The BootInfo pointer passed in is a physical address (the bootloader
     * jumped to us with the physmap already active, so we use the physmap). */
    const BootInfo *info = (const BootInfo *)(PHYSMAP_BASE + (uintptr_t)info_phys);

    /* Validate boot magic */
    if (info->magic != BOOT_MAGIC) {
        early_printf("[NovaOS] FATAL: Bad boot magic 0x%016llx (expected 0x%016llx)\r\n",
                     (unsigned long long)info->magic,
                     (unsigned long long)BOOT_MAGIC);
        for (;;) { cli(); hlt(); }
    }

    early_printf("[NovaOS] Boot protocol version %u\r\n", info->version);
    early_printf("[NovaOS] Kernel: phys=0x%llx virt=0x%llx size=0x%llx\r\n",
                 (unsigned long long)info->kernel_physical_base,
                 (unsigned long long)info->kernel_virtual_base,
                 (unsigned long long)info->kernel_size);

    /* ----------------------------------------------------------------
     * STEP 2: Framebuffer
     * ---------------------------------------------------------------- */
    fb_init(&info->framebuffer);
    if (fb_available()) {
        early_printf("[NovaOS] Framebuffer %ux%u initialized\r\n",
                     info->framebuffer.width, info->framebuffer.height);
    }

    print_banner();

    /* ----------------------------------------------------------------
     * STEP 3: Physical Memory Manager
     * ---------------------------------------------------------------- */
    kprintf("=== Phase 1: Memory Manager ===\n");
    pmm_init(info);

    /* ----------------------------------------------------------------
     * STEP 4: Paging (take ownership, enable NX)
     * ---------------------------------------------------------------- */
    kprintf("=== Phase 1: Paging ===\n");
    paging_init();

    /* ----------------------------------------------------------------
     * STEP 5: Kernel Virtual Memory Manager (heap)
     * ---------------------------------------------------------------- */
    kprintf("=== Phase 1: VMM / Heap ===\n");
    vmm_init();

    /* ----------------------------------------------------------------
     * STEP 6: GDT (reload segments + TSS)
     * ---------------------------------------------------------------- */
    kprintf("=== Phase 1: GDT ===\n");
    gdt_init();
    kprintf("[GDT] GDT/TSS initialized, segments reloaded\n");

    /* ----------------------------------------------------------------
     * STEP 7: IDT (exception and interrupt handlers)
     * ---------------------------------------------------------------- */
    kprintf("=== Phase 1: IDT ===\n");
    idt_init();

    /* ----------------------------------------------------------------
     * STEP 8: APIC (disable 8259A PIC, enable LAPIC timer at 100 Hz)
     * ---------------------------------------------------------------- */
    kprintf("=== Phase 1: APIC ===\n");
    apic_init();

    /* ----------------------------------------------------------------
     * STEP 9: Scheduler
     * ---------------------------------------------------------------- */
    kprintf("=== Phase 1: Scheduler ===\n");
    sched_init();

    /* ----------------------------------------------------------------
     * STEP 10: Wire APIC timer to scheduler
     * ---------------------------------------------------------------- */
    kprintf("=== Phase 1: Hardware IRQs enabled ===\n");

    /* ----------------------------------------------------------------
     * STEP 11 (Phase 2): Object Manager
     * ---------------------------------------------------------------- */
    kprintf("=== Phase 2: Object Manager ===\n");
    ObInitialize();

    /* ----------------------------------------------------------------
     * STEP 12 (Phase 2): Security Reference Monitor
     * Must be before Process Manager (system token assigned to System process)
     * ---------------------------------------------------------------- */
    kprintf("=== Phase 2: Security Reference Monitor ===\n");
    SeInitialize();

    /* ----------------------------------------------------------------
     * STEP 13 (Phase 2): Process Manager
     * Creates System process (PID 4) and wraps the boot thread as TID 4
     * ---------------------------------------------------------------- */
    kprintf("=== Phase 2: Process Manager ===\n");
    PsInitialize();

    /* Assign the System token to the System process */
    {
        PTOKEN system_token = NULL;
        if (NT_SUCCESS(SeCreateSystemToken(&system_token))) {
            PsInitialSystemProcess->Token = system_token;
            kprintf("[PS] System token assigned to System process\n");
        }
    }

    /* ----------------------------------------------------------------
     * STEP 14 (Phase 2): Configuration Manager (Registry)
     * ---------------------------------------------------------------- */
    kprintf("=== Phase 2: Configuration Manager (Registry) ===\n");
    CmInitialize();

    /* ----------------------------------------------------------------
     * STEP 15 (Phase 2): Syscall dispatcher (INT 0x2E + SYSCALL MSR)
     * ---------------------------------------------------------------- */
    kprintf("=== Phase 2: Syscall Dispatcher ===\n");
    SyscallInitialize();

    /* ----------------------------------------------------------------
     * STEP 16 (Phase 3): I/O Manager + built-in devices
     * ---------------------------------------------------------------- */
    kprintf("=== Phase 3: I/O Manager ===\n");
    IoInitialize();

    /* ----------------------------------------------------------------
     * STEP 17 (Phase 3): Section object subsystem
     * ---------------------------------------------------------------- */
    kprintf("=== Phase 3: Section Objects ===\n");
    MmInitializeSections();

    /* ----------------------------------------------------------------
     * STEP 18 (Phase 3): PE32+ Loader
     * ---------------------------------------------------------------- */
    kprintf("=== Phase 3: PE Loader ===\n");
    LdrInitialize();

    /* ----------------------------------------------------------------
     * STEP 16: Create demo threads to validate the scheduler
     * ---------------------------------------------------------------- */
    kprintf("=== Phase 1/2: Creating test threads ===\n");
    sched_create_thread("thread_a",  thread_a,            NULL, 8);
    sched_create_thread("thread_b",  thread_b,            NULL, 8);
    sched_create_thread("mem_test",  memory_test_thread,  NULL, 6);

    sched_dump();

    /* ----------------------------------------------------------------
     * STEP 17: Enable interrupts — the scheduler is now live
     * ---------------------------------------------------------------- */
    kprintf("\n[NovaOS] Phase 2 initialized. Enabling interrupts...\n");
    sti();

    /* ----------------------------------------------------------------
     * Main kernel loop — yield CPU to other threads
     * In a real NT system, this would become the System process.
     * ---------------------------------------------------------------- */
    kprintf("[NovaOS] Entering kernel main loop (System thread)\n");

    uint64_t last_dump = 0;
    for (;;) {
        /* Periodically dump scheduler state */
        /* (We don't have a reliable tick counter visible here yet;
         *  use a simple busy counter as a very rough timer) */
        volatile uint64_t spin;
        for (spin = 0; spin < 50000000ULL; spin++)
            pause_cpu();

        if (last_dump < 5) {
            last_dump++;
            sched_dump();
        }

        sched_yield();
        hlt();
    }
}
