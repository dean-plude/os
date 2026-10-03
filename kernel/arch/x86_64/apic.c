/*
 * apic.c — Local APIC initialization
 *
 * Architectural notes:
 *
 * 1. 8259A PIC shutdown: The legacy PIC is still powered on after UEFI boot
 *    and will generate spurious IRQs if left in an unknown state.  We remap
 *    its vectors to 0xA0–0xAF (safely above exceptions) and then mask all
 *    lines to prevent any PIC interrupts from reaching the CPU.
 *
 * 2. LAPIC enable: We write to the Spurious Interrupt Vector Register with
 *    the software-enable bit set.  The spurious vector is set to 0xFF.
 *
 * 3. APIC timer calibration: The APIC timer's frequency is derived from
 *    the internal bus clock.  We calibrate it, and the TSC, against the
 *    HPET's main counter (hpet.c).  Where the firmware hides the HPET (and
 *    may gate the 8254's clock too, as recent Intel laptops do), the CPU
 *    reports both clocks in CPUID leaf 0x15 (or 0x16); the PIT (8254
 *    timer) is the last resort, with a time limit.
 *
 * 4. The timer is one-shot: the scheduler arms it for the next 10 ms tick
 *    or the earliest timed sleeper on this CPU, whichever comes first
 *    (apic_timer_arm), so a sleep ends when it is due rather than at the
 *    next tick.  Where the CPU has it (CPUID.1:ECX[24]) the timer runs in
 *    TSC-deadline mode, taking the TSC value to fire at; otherwise it
 *    counts down from a count worked out from the calibration.
 */

#include "apic.h"
#include "cpu.h"
#include "idt.h"
#include "../../hal/serial.h"
#include "../../hal/hpet.h"
#include "../../ke/printf.h"
#include "../../include/types.h"

/* The LAPIC MMIO base after it's been mapped into the physmap (the same
 * physical address on every CPU; each CPU sees its own LAPIC there) */
static volatile uint32_t *lapic_base;

/* Timer count for 10 ms, and TSC ticks per 10 ms, measured on the boot CPU */
static uint32_t g_timer_10ms;
uint64_t g_tsc_per_tick;
static bool g_tsc_deadline;           /* the timer runs in TSC-deadline mode */

/* -----------------------------------------------------------------------
 * LAPIC register access
 * ----------------------------------------------------------------------- */

static uint32_t lapic_read(uint32_t reg)
{
    return *(volatile uint32_t *)((uintptr_t)lapic_base + reg);
}

static void lapic_write(uint32_t reg, uint32_t val)
{
    *(volatile uint32_t *)((uintptr_t)lapic_base + reg) = val;
}

/* -----------------------------------------------------------------------
 * Disable the legacy 8259A PIC
 *
 * We remap it first (so any stray IRQs don't hit exception vectors),
 * then mask all IRQ lines, and finally send OCW1=0xFF to both PICs.
 *
 * This follows the standard Linux/BSD approach.
 * ----------------------------------------------------------------------- */

#define PIC1_CMD    0x20
#define PIC1_DATA   0x21
#define PIC2_CMD    0xA0
#define PIC2_DATA   0xA1
#define PIC_EOI     0x20

static void pic_mask(void)
{
    /* Remap PIC1: ICW1 */
    outb(PIC1_CMD, 0x11);  io_wait();
    /* ICW2: PIC1 vectors start at 0xA0 (safe, well above exceptions) */
    outb(PIC1_DATA, 0xA0); io_wait();
    /* ICW3: slave at IRQ2 */
    outb(PIC1_DATA, 0x04); io_wait();
    /* ICW4: 8086 mode */
    outb(PIC1_DATA, 0x01); io_wait();

    /* Remap PIC2: ICW1 */
    outb(PIC2_CMD, 0x11);  io_wait();
    /* ICW2: PIC2 vectors start at 0xA8 */
    outb(PIC2_DATA, 0xA8); io_wait();
    /* ICW3: cascade identity */
    outb(PIC2_DATA, 0x02); io_wait();
    /* ICW4: 8086 mode */
    outb(PIC2_DATA, 0x01); io_wait();

    /* OCW1: mask all IRQs on both PICs */
    outb(PIC1_DATA, 0xFF); io_wait();
    outb(PIC2_DATA, 0xFF); io_wait();
}

static void pic_disable(void)
{
    pic_mask();
    kprintf("[APIC] Legacy 8259A PIC disabled\n");
}

/* -----------------------------------------------------------------------
 * PIT (8254) — used only for APIC timer calibration
 *
 * We set up the PIT channel 2 (connected to the PC speaker, but we don't
 * need sound) for a one-shot count and use it as a ~10ms reference.
 * This avoids touching PIT channel 0 which may still be generating IRQs.
 * ----------------------------------------------------------------------- */

#define PIT_CHANNEL2   0x42
#define PIT_COMMAND    0x43
#define PIT_GATE2      0x61   /* Port B, bit 0 = gate, bit 1 = speaker enable */
#define PIT_FREQUENCY  1193182   /* Hz */

/* Measure APIC timer ticks in ~10ms using PIT channel 2. */
static uint32_t calibrate_apic_timer(void)
{
    /* Set up channel 2 for one-shot mode (mode 0), counting from 11931 ≈ 10ms */
    uint16_t pit_count = PIT_FREQUENCY / 100;   /* 100 Hz = 10ms per period */

    /* Gate channel 2: bit 0 on, bit 1 off (no speaker) */
    uint8_t gate = inb(PIT_GATE2);
    outb(PIT_GATE2, (gate & 0xFC) | 0x01);

    /* Channel 2, mode 0 (one-shot), binary */
    outb(PIT_COMMAND, 0xB0);
    outb(PIT_CHANNEL2, (uint8_t)(pit_count & 0xFF));
    outb(PIT_CHANNEL2, (uint8_t)(pit_count >> 8));

    /* Start APIC timer counting down from max */
    lapic_write(LAPIC_TIMER_DIV,  LAPIC_TIMER_DIV_16);
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFF);

    /* Disable the gate, then re-enable to start counting */
    outb(PIT_GATE2, inb(PIT_GATE2) & ~0x01);
    outb(PIT_GATE2, inb(PIT_GATE2) | 0x01);
    uint64_t tsc0 = rdtsc();

    /* Wait for PIT channel 2 to expire (bit 5 of Port B goes high); a
     * gated 8254 never does, so give up after 2^34 TSC ticks (seconds at
     * any clock rate) */
    while (!(inb(PIT_GATE2) & 0x20)) {
        if (rdtsc() - tsc0 > (1ull << 34)) {
            lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
            g_tsc_per_tick = 0;
            return 0;
        }
        pause_cpu();
    }
    g_tsc_per_tick = rdtsc() - tsc0;          /* the TSC over the same 10 ms */

    /* Stop APIC timer and read how far it counted in 10ms */
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    uint32_t ticks_in_10ms = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CURR);

    return ticks_in_10ms;
}

/* The same against the HPET: 10 ms of its main counter */
static uint32_t calibrate_apic_timer_hpet(void)
{
    uint64_t n = HpetFrequency() / 100;
    lapic_write(LAPIC_TIMER_DIV,  LAPIC_TIMER_DIV_16);
    uint64_t h0 = HpetCounter();
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFF);
    uint64_t tsc0 = rdtsc();
    while (((HpetCounter() - h0) & 0xFFFFFFFFull) < n) pause_cpu();
    g_tsc_per_tick = rdtsc() - tsc0;
    uint32_t ticks_in_10ms = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CURR);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_TIMER_INIT, 0);
    return ticks_in_10ms;
}

/* Without an HPET: the clocks the CPU reports (Intel, CPUID leaf 0x15:
 * the core crystal's frequency and the TSC/crystal ratio; leaf 0x16's base
 * frequency when the crystal isn't given, as Linux's native_calibrate_tsc
 * does).  The local APIC timer runs from the crystal.  0: not reported. */
static uint32_t calibrate_from_cpuid(uint64_t *crystal_hz)
{
    if (cpuid(0, 0).eax < 0x15) return 0;
    CpuidResult r = cpuid(0x15, 0);
    if (!r.eax || !r.ebx) return 0;
    uint64_t crystal = r.ecx;
    if (!crystal && cpuid(0, 0).eax >= 0x16) {
        uint64_t base_mhz = cpuid(0x16, 0).eax & 0xFFFF;
        crystal = base_mhz * 1000000ull * r.eax / r.ebx;
    }
    if (!crystal) return 0;
    uint64_t tsc_hz = crystal * r.ebx / r.eax;
    g_tsc_per_tick = tsc_hz / 100;
    *crystal_hz = crystal;
    return (uint32_t)(crystal / 100 / 16);         /* the APIC timer at div/16 */
}

/* -----------------------------------------------------------------------
 * The timer: one-shot or TSC-deadline, armed for a TSC value
 * ----------------------------------------------------------------------- */
void apic_timer_arm(uint64_t tsc)
{
    if (g_tsc_deadline) {
        wrmsr(MSR_IA32_TSC_DEADLINE, tsc ? tsc : 1);   /* (0 would disarm it; a past value fires at once) */
        return;
    }
    uint64_t now = rdtsc(), d = tsc > now ? tsc - now : 0;
    if (d > g_tsc_per_tick * 100) d = g_tsc_per_tick * 100;   /* at most a second ahead */
    uint64_t count = g_tsc_per_tick ? d * g_timer_10ms / g_tsc_per_tick : g_timer_10ms;
    if (count < 1) count = 1;
    if (count > 0xFFFFFFFFull) count = 0xFFFFFFFFull;
    lapic_write(LAPIC_TIMER_INIT, (uint32_t)count);
}

/* Set this CPU's timer going: the first tick 10 ms from now */
static void timer_start(void)
{
    lapic_write(LAPIC_TIMER_DIV, LAPIC_TIMER_DIV_16);
    lapic_write(LAPIC_LVT_TIMER, (g_tsc_deadline ? LAPIC_TIMER_TSC_DL : LAPIC_TIMER_ONESHOT) | IRQ_TIMER);
    if (g_tsc_deadline) __asm__ volatile ("mfence" ::: "memory");   /* (the mode before the MSR, SDM 10.5.4.1) */
    apic_timer_arm(rdtsc() + g_tsc_per_tick);
}

bool apic_timer_tsc_deadline(void) { return g_tsc_deadline; }

/* -----------------------------------------------------------------------
 * apic_init
 * ----------------------------------------------------------------------- */
void apic_init(void)
{
    /* 1. Disable the legacy PIC */
    pic_disable();

    /* 2. Find the LAPIC base from the IA32_APIC_BASE MSR */
    uint64_t apic_base_msr = rdmsr(MSR_IA32_APIC_BASE);
    uint64_t lapic_phys    = apic_base_msr & UINT64_C(0xFFFFFFFFFF000);  /* bits 12-51 */

    kprintf("[APIC] IA32_APIC_BASE MSR = 0x%016lx (phys=0x%lx)\n",
            apic_base_msr, lapic_phys);

    /* 3. Map into kernel virtual address space via physmap */
    lapic_base = (volatile uint32_t *)(PHYSMAP_BASE + lapic_phys);

    /* 4. Enable the APIC by writing to the Spurious Interrupt Vector Register
     *    Bit 8 = APIC Software Enable, low byte = spurious vector (0xFF) */
    lapic_write(LAPIC_SPURIOUS, LAPIC_SPURIOUS_ENABLE | IRQ_SPURIOUS);

    /* 5. Set Task Priority Register to 0 to allow all interrupts */
    lapic_write(LAPIC_TPR, 0);

    /* 6. Clear any pending errors */
    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_ESR, 0);

    uint32_t apic_ver = lapic_read(LAPIC_VERSION);
    kprintf("[APIC] Version=0x%x, MaxLVT=%u, APIC ID=%u\n",
            apic_ver & 0xFF,
            ((apic_ver >> 16) & 0xFF) + 1,
            lapic_read(LAPIC_ID) >> 24);

    /* 7. Calibrate the APIC timer and the TSC against the HPET, else take
     *    them from CPUID, else measure them against the PIT */
    uint64_t crystal = 0;
    uint32_t ticks_10ms;
    const char *source;
    if (HpetPresent()) {
        ticks_10ms = calibrate_apic_timer_hpet();
        source = "calibrated against the HPET";
    } else if ((ticks_10ms = calibrate_from_cpuid(&crystal)) != 0) {
        source = "from CPUID 0x15 (no HPET)";
    } else if ((ticks_10ms = calibrate_apic_timer()) != 0) {
        source = "calibrated against the PIT (no HPET)";
    } else {
        /* nothing to measure against: a guess that keeps the machine going */
        g_tsc_per_tick = 20000000;                 /* 2 GHz */
        ticks_10ms = 62500;                        /* a 100 MHz bus at div/16 */
        source = "guessed: no HPET, no CPUID 0x15 and the PIT never counted down";
    }
    uint32_t ticks_per_sec = ticks_10ms * 100;
    kprintf("[APIC] Timer: %u ticks/10ms = ~%u Hz (div/16), %s\n", ticks_10ms, ticks_per_sec, source);
    if (crystal)
        kprintf("[APIC] CPUID: core crystal %llu Hz, TSC %llu Hz\n",
                (unsigned long long)crystal, (unsigned long long)g_tsc_per_tick * 100);

    /* 8. Start the timer: TSC-deadline mode where the CPU has it, else
     *    one-shot; the scheduler re-arms it at each interrupt */
    g_timer_10ms = ticks_10ms;
    g_tsc_deadline = (cpuid(1, 0).ecx >> 24) & 1;
    timer_start();

    kprintf("[APIC] %s timer started (vector 0x%x), TSC %llu per 10 ms\n",
            g_tsc_deadline ? "TSC-deadline" : "One-shot", IRQ_TIMER,
            (unsigned long long)g_tsc_per_tick);
}

/* -----------------------------------------------------------------------
 * apic_init_ap — enable the calling CPU's LAPIC and its timer
 * (the other CPUs reuse the boot CPU's calibration)
 * ----------------------------------------------------------------------- */
/* After S3: the firmware set the legacy PIC up again; mask it (no
 * logging: the waking CPU takes no locks) */
void apic_resume(void)
{
    pic_mask();
}

void apic_init_ap(void)
{
    lapic_write(LAPIC_SPURIOUS, LAPIC_SPURIOUS_ENABLE | IRQ_SPURIOUS);
    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_ESR, 0);
    timer_start();
}

/* -----------------------------------------------------------------------
 * apic_send_ipi — write the ICR (destination APIC ID, command) and wait
 * until the LAPIC has sent it
 * ----------------------------------------------------------------------- */
void apic_send_ipi(uint32_t dest_apic_id, uint32_t command)
{
    lapic_write(LAPIC_ICR_HI, dest_apic_id << 24);
    lapic_write(LAPIC_ICR_LO, command);
    for (int i = 0; i < 1000000 && (lapic_read(LAPIC_ICR_LO) & (1u << 12)); i++)
        pause_cpu();                          /* delivery status: send pending */
}

/* Busy-wait @us microseconds (TSC; before or without the scheduler) */
void udelay(uint64_t us)
{
    uint64_t end = rdtsc() + g_tsc_per_tick * us / 10000;
    while (rdtsc() < end) pause_cpu();
}

/* -----------------------------------------------------------------------
 * apic_eoi — send End-of-Interrupt
 * ----------------------------------------------------------------------- */
void apic_eoi(void)
{
    lapic_write(LAPIC_EOI, 0);
}

/* -----------------------------------------------------------------------
 * apic_timer_current
 * ----------------------------------------------------------------------- */
uint32_t apic_timer_current(void)
{
    return lapic_read(LAPIC_TIMER_CURR);
}

/* -----------------------------------------------------------------------
 * apic_id — return this CPU's APIC ID
 * ----------------------------------------------------------------------- */
uint8_t apic_id(void)
{
    return (uint8_t)(lapic_read(LAPIC_ID) >> 24);
}
