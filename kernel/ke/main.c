/*
 * main.c — KiSystemStartup: the C-level kernel entry point
 *
 * Initialization order:
 *
 *  Phase 1 — Boot & Foundation
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
 *
 *  Phase 2 — NT Kernel Personality
 *  10. Object Manager  (ObInitialize)
 *  11. Security SRM    (SeInitialize + SeCreateSystemToken)
 *  12. Process Manager (PsInitialize)
 *  13. Registry        (CmInitialize)
 *  14. Syscall dispatcher (SyscallInitialize — INT 0x2E + SYSCALL MSR)
 *
 *  Phase 3 — I/O, Section Objects, PE Loader
 *  15. I/O Manager      (IoInitialize)
 *  16. Section Objects  (MmInitializeSections)
 *  17. PE32+ Loader     (LdrInitialize)
 *
 *  Phase 4 — File System
 *  18. VFS Layer        (VfsInitialize)
 *  19. InitRD driver    (InitrdMount)
 *
 *  Phase 5 — Process Isolation & User-Mode Foundation
 *  20. KPCR             (KiInitializeKpcr — GS base for per-CPU state)
 *
 *  Enable IRQs, create test threads, enter idle loop.
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
#include "../lib/string.h"
#include "syscall.h"
#include "../ob/ob.h"
#include "../ps/ps.h"
#include "../cm/cm.h"
#include "../se/se.h"
#include "../io/io.h"
#include "../mm/vma.h"
#include "../mm/section.h"
#include "../ldr/ldr.h"
#include "../ldr/user_stubs.h"
#include "../fs/vfs.h"
#include "../fs/initrd.h"
#include "kpcr.h"
#include "../ps/csrss.h"
#include "../gdi/gdi.h"
#include "../wm/wm.h"
#include "../wm/desktop.h"
#include "../wm/input.h"
#include "../hal/ps2.h"
#include "phase9_pe.h"

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
    kprintf("  Windows-compatible OS kernel  [Phase 9 — Native PE32+ Execution]\n");
    kprintf("  Built: " __DATE__ " " __TIME__ "\n");
    kprintf("\n");
}

/* -----------------------------------------------------------------------
 * Demo threads — smoke tests
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

    void *ptrs[16];
    static const size_t sizes[] = { 8, 16, 32, 64, 128, 256, 512, 1024,
                                    2048, 4096, 8192, 16384,
                                    32, 64, 128, 256 };
    for (int i = 0; i < 16; i++) {
        ptrs[i] = kmalloc(sizes[i]);
        if (ptrs[i]) {
            __builtin_memset(ptrs[i], (int)(0xAA + i), sizes[i]);
        } else {
            kprintf("[MemTest] FAIL: kmalloc(%zu) returned NULL\n", sizes[i]);
        }
    }

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

    for (int i = 0; i < 16; i++) {
        if (ptrs[i]) kfree(ptrs[i]);
    }

    if (ok) kprintf("[MemTest] All allocations OK\n");

    uint64_t total, free_pages, used;
    pmm_stats(&total, &free_pages, &used);
    kprintf("[MemTest] PMM: %lu MiB total, %lu MiB free, %lu MiB used\n",
            (total * PAGE_SIZE) >> 20,
            (free_pages * PAGE_SIZE) >> 20,
            (used  * PAGE_SIZE) >> 20);
}

static void phase9_thread(void *arg)
{
    (void)arg;
    for (int i = 0; i < 3; i++) sched_yield();
    kprintf("\n[Phase 9] === Starting native PE32+ execution test ===\n");
    PEPROCESS proc = NULL;
    HANDLE thread_handle = 0;
    NTSTATUS s = PsCreateUserProcess(
        (void *)g_phase9_pe, (UINT64)g_phase9_pe_size,
        "hello.exe", &proc, &thread_handle);
    if (NT_SUCCESS(s))
        kprintf("[Phase 9] User process created PID=%lu handle=%p\n",
                proc ? (unsigned long)proc->UniqueProcessId : 0UL,
                (void *)(uintptr_t)thread_handle);
    else
        kprintf("[Phase 9] FAILED to create user process: 0x%08x\n", (UINT32)s);
    if (proc) ObDereferenceObject(proc);
}

/* -----------------------------------------------------------------------
 * KiSystemStartup — C kernel entry
 * @info_phys: physical address of BootInfo (use physmap to access)
 * ----------------------------------------------------------------------- */
void __attribute__((noreturn)) KiSystemStartup(const BootInfo *info_phys)
{
    /* ------------------------------------------------------------------
     * STEP 1: Serial port
     * ------------------------------------------------------------------ */
    serial_init(SERIAL_COM1_BASE);
    early_printf("\r\n[NovaOS] Serial console active\r\n");
    early_printf("[NovaOS] Kernel entry at %p\r\n", (void *)KiSystemStartup);

    const BootInfo *info = (const BootInfo *)(PHYSMAP_BASE + (uintptr_t)info_phys);

    if (info->magic != BOOT_MAGIC) {
        early_printf("[NovaOS] FATAL: Bad boot magic 0x%016llx\r\n",
                     (unsigned long long)info->magic);
        for (;;) { cli(); hlt(); }
    }

    early_printf("[NovaOS] Boot protocol v%u\r\n", info->version);

    /* ------------------------------------------------------------------
     * STEP 2: Framebuffer
     * ------------------------------------------------------------------ */
    fb_init(&info->framebuffer);
    print_banner();

    /* ------------------------------------------------------------------
     * STEP 3-5: Memory (PMM, Paging, VMM)
     * ------------------------------------------------------------------ */
    kprintf("=== Phase 1: Memory Manager ===\n");
    pmm_init(info);

    kprintf("=== Phase 1: Paging ===\n");
    paging_init();

    kprintf("=== Phase 1: VMM / Heap ===\n");
    vmm_init();

    /* ------------------------------------------------------------------
     * STEP 6-8: GDT, IDT, APIC
     * ------------------------------------------------------------------ */
    kprintf("=== Phase 1: GDT ===\n");
    gdt_init();

    kprintf("=== Phase 1: IDT ===\n");
    idt_init();

    kprintf("=== Phase 1: APIC ===\n");
    apic_init();

    /* ------------------------------------------------------------------
     * STEP 9: Scheduler
     * ------------------------------------------------------------------ */
    kprintf("=== Phase 1: Scheduler ===\n");
    sched_init();

    /* ------------------------------------------------------------------
     * STEP 10: Object Manager
     * ------------------------------------------------------------------ */
    kprintf("=== Phase 2: Object Manager ===\n");
    ObInitialize();

    /* ------------------------------------------------------------------
     * STEP 11: Security Reference Monitor
     * ------------------------------------------------------------------ */
    kprintf("=== Phase 2: Security Reference Monitor ===\n");
    SeInitialize();

    /* ------------------------------------------------------------------
     * STEP 12: Process Manager
     * ------------------------------------------------------------------ */
    kprintf("=== Phase 2: Process Manager ===\n");
    PsInitialize();

    {
        PTOKEN system_token = NULL;
        if (NT_SUCCESS(SeCreateSystemToken(&system_token))) {
            PsInitialSystemProcess->Token = system_token;
            kprintf("[PS] System token assigned\n");
        }
    }

    /* ------------------------------------------------------------------
     * STEP 13: Configuration Manager (Registry)
     * ------------------------------------------------------------------ */
    kprintf("=== Phase 2: Configuration Manager ===\n");
    CmInitialize();

    /* ------------------------------------------------------------------
     * STEP 14: Syscall dispatcher (INT 0x2E + SYSCALL MSR)
     * ------------------------------------------------------------------ */
    kprintf("=== Phase 2: Syscall Dispatcher ===\n");
    SyscallInitialize();

    /* ------------------------------------------------------------------
     * STEP 15: I/O Manager
     * ------------------------------------------------------------------ */
    kprintf("=== Phase 3: I/O Manager ===\n");
    IoInitialize();

    /* ------------------------------------------------------------------
     * STEP 16: Section object subsystem
     * ------------------------------------------------------------------ */
    kprintf("=== Phase 3: Section Objects ===\n");
    MmInitializeSections();

    /* ------------------------------------------------------------------
     * STEP 17: PE32+ Loader
     * ------------------------------------------------------------------ */
    kprintf("=== Phase 3: PE Loader ===\n");
    LdrInitialize();

    /* ------------------------------------------------------------------
     * STEP 17b (Phase 6): User-mode SYSCALL stub pages
     * Allocate one physical page per stub DLL and emit x86-64 SYSCALL
     * thunk machine code.  Must run after PMM is up and before any
     * user-mode process is created (so LdrMapUserStubPages has pages).
     * ------------------------------------------------------------------ */
    kprintf("=== Phase 6: User-mode SYSCALL stubs ===\n");
    LdrInitUserStubs();

    /* ------------------------------------------------------------------
     * STEP 18 (Phase 4): Virtual File System
     * ------------------------------------------------------------------ */
    kprintf("=== Phase 4: VFS ===\n");
    VfsInitialize();

    /* ------------------------------------------------------------------
     * STEP 19 (Phase 4): InitRD — mount the in-memory initial ramdisk
     * ------------------------------------------------------------------ */
    kprintf("=== Phase 4: InitRD ===\n");
    if (info->initrd_size > 0) {
        void *initrd_virt = (void *)(PHYSMAP_BASE + info->initrd_base);
        InitrdMount(initrd_virt, info->initrd_size);
    } else {
        kprintf("[INITRD] No initial ramdisk provided\n");
    }

    /* ------------------------------------------------------------------
     * STEP 20 (Phase 5): KPCR — Kernel Processor Control Region
     * Programs MSR_KERNEL_GS_BASE so that SWAPGS in syscall_entry.asm
     * switches GS to the KPCR (saving user GS / TEB pointer).
     * Also initializes KPCR.KernelRsp for the initial boot thread.
     * ------------------------------------------------------------------ */
    kprintf("=== Phase 5: KPCR ===\n");
    KiInitializeKpcr();

    /* ------------------------------------------------------------------
     * STEP 20b (Phase 6): CSRSS — Client/Server Runtime SubSystem
     * ------------------------------------------------------------------ */
    kprintf("=== Phase 6: CSRSS bootstrap ===\n");
    CsrInitialize();

    /* Seed KPCR.KernelRsp with the idle thread's kernel stack top.
     * The scheduler updates this on every context switch. */
    {
        PKPCR kpcr = KiGetCurrentKpcr();
        Thread *idle = sched_current();
        if (kpcr && idle && idle->kernel_stack) {
            kpcr->KernelRsp = (UINT64)((uintptr_t)idle->kernel_stack
                                       + idle->stack_size);
        }
    }

    /* ------------------------------------------------------------------
     * STEP 21: Create test threads, then enable IRQs
     * ------------------------------------------------------------------ */
    kprintf("=== Creating test threads ===\n");
    sched_create_thread("thread_a",  thread_a,           NULL, 8);
    sched_create_thread("thread_b",  thread_b,           NULL, 8);
    sched_create_thread("mem_test",  memory_test_thread, NULL, 6);

    sched_dump();

    /* ------------------------------------------------------------------
     * STEP 22 (Phase 7): GUI — GDI software renderer, window manager,
     * and the desktop shell.  Once the desktop is painted we silence
     * framebuffer logging so kernel/thread output (which still flows to
     * the serial console) cannot overwrite the rendered UI.
     * ------------------------------------------------------------------ */
    kprintf("=== Phase 7: GDI + Window Manager + Desktop Shell ===\n");
    if (GdiInitialize()) {
        WmInitialize();
        DesktopInitialize();
        /* Phase 8: input plumbing + interactive desktop event loop. */
        InputInit();
        ps2_init();
        sched_create_thread("desktop", DesktopRun, NULL, 8);
        kprintf_set_fb_enabled(false);    /* WM owns the screen; logs → serial */
        kprintf("[NovaOS] Desktop event loop started (%dx%d)\n",
                GdiScreenW(), GdiScreenH());
    } else {
        kprintf("[NovaOS] No framebuffer present; running headless\n");
    }

    /* ------------------------------------------------------------------
     * STEP 23 (Phase 9): Launch a native PE32+ in ring-3
     * ------------------------------------------------------------------ */
    kprintf("=== Phase 9: Native PE32+ execution ===\n");
    sched_create_thread("phase9", phase9_thread, NULL, 8);

    kprintf("\n[NovaOS] Phase 9 initialized. Enabling interrupts...\n");
    sti();

    kprintf("[NovaOS] Entering kernel main loop\n");

    for (;;) {
        volatile uint64_t spin;
        for (spin = 0; spin < 50000000ULL; spin++)
            pause_cpu();

        sched_yield();
        hlt();
    }
}
