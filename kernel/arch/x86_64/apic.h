/*
 * apic.h — Local APIC (Advanced Programmable Interrupt Controller)
 *
 * The Local APIC handles:
 *   - Sending EOI (End-of-Interrupt) after IRQ processing
 *   - Generating the periodic timer interrupt for the scheduler
 *   - IPI (Inter-Processor Interrupts) for SMP (Phase 6+)
 *   - Spurious interrupt vector
 *
 * We use the memory-mapped APIC interface (MMIO registers at
 * 0xFEE00000 physical, mapped in the physmap).  x2APIC MSR mode
 * is detected and used if available for better SMP scalability.
 *
 * The legacy 8259A PIC is disabled entirely — all IRQ routing goes
 * through the I/O APIC (which we'll initialize in Phase 3 when we
 * add device drivers).  For Phase 1 we only need the local APIC timer.
 */

#pragma once

#include "../../include/types.h"

/* Local APIC MMIO base (physical) — may be different if firmware moved it */
#define LAPIC_DEFAULT_PHYS   UINT64_C(0xFEE00000)

/* Local APIC register offsets from the base */
#define LAPIC_ID             0x020   /* Local APIC ID register */
#define LAPIC_VERSION        0x030   /* Local APIC Version register */
#define LAPIC_TPR            0x080   /* Task Priority Register */
#define LAPIC_APR            0x090   /* Arbitration Priority Register */
#define LAPIC_PPR            0x0A0   /* Processor Priority Register */
#define LAPIC_EOI            0x0B0   /* End-of-Interrupt register (write 0) */
#define LAPIC_RRD            0x0C0   /* Remote Read Register */
#define LAPIC_LDR            0x0D0   /* Logical Destination Register */
#define LAPIC_DFR            0x0E0   /* Destination Format Register */
#define LAPIC_SPURIOUS       0x0F0   /* Spurious Interrupt Vector Register */
#define LAPIC_ISR_BASE       0x100   /* In-Service Register (256 bits, 8 DWORDs) */
#define LAPIC_TMR_BASE       0x180   /* Trigger Mode Register */
#define LAPIC_IRR_BASE       0x200   /* Interrupt Request Register */
#define LAPIC_ESR            0x280   /* Error Status Register */
#define LAPIC_LVT_CMCI       0x2F0   /* LVT CMCI Register */
#define LAPIC_ICR_LO         0x300   /* Interrupt Command Register (bits 0-31) */
#define LAPIC_ICR_HI         0x310   /* Interrupt Command Register (bits 32-63) */
#define LAPIC_LVT_TIMER      0x320   /* LVT Timer Register */
#define LAPIC_LVT_THERMAL    0x330   /* LVT Thermal Sensor Register */
#define LAPIC_LVT_PERFMON    0x340   /* LVT Performance Monitor Counters Register */
#define LAPIC_LVT_LINT0      0x350   /* LVT LINT0 Register */
#define LAPIC_LVT_LINT1      0x360   /* LVT LINT1 Register */
#define LAPIC_LVT_ERROR      0x370   /* LVT Error Register */
#define LAPIC_TIMER_INIT     0x380   /* Initial Count Register (timer) */
#define LAPIC_TIMER_CURR     0x390   /* Current Count Register (timer) */
#define LAPIC_TIMER_DIV      0x3E0   /* Divide Configuration Register (timer) */

/* Spurious Interrupt Vector Register bits */
#define LAPIC_SPURIOUS_ENABLE  (1u << 8)   /* APIC Software Enable bit */

/* LVT Timer modes (bits 17:18) */
#define LAPIC_TIMER_ONESHOT   (0u << 17)
#define LAPIC_TIMER_PERIODIC  (1u << 17)
#define LAPIC_TIMER_TSC_DL    (2u << 17)   /* TSC-Deadline mode */

/* LVT entry: masked bit */
#define LAPIC_LVT_MASKED      (1u << 16)

/* Timer divide values (LAPIC_TIMER_DIV register) */
#define LAPIC_TIMER_DIV_2     0
#define LAPIC_TIMER_DIV_4     1
#define LAPIC_TIMER_DIV_8     2
#define LAPIC_TIMER_DIV_16    3
#define LAPIC_TIMER_DIV_32    8
#define LAPIC_TIMER_DIV_64    9
#define LAPIC_TIMER_DIV_128   10
#define LAPIC_TIMER_DIV_1     11

/*
 * Initialize the Local APIC:
 *   1. Disable the legacy 8259A PIC
 *   2. Map LAPIC MMIO into kernel virtual address space
 *   3. Enable the APIC via the spurious vector register
 *   4. Set up the APIC timer in periodic mode
 */
void apic_init(void);

/*
 * Send End-of-Interrupt to the APIC.
 * Must be called at the end of every hardware IRQ handler.
 * MUST NOT be called for spurious interrupts (vector 0xFF).
 */
void apic_eoi(void);

/*
 * Read the current APIC timer count.
 * Useful for measuring elapsed time before the scheduler is up.
 */
uint32_t apic_timer_current(void);

/*
 * Return this CPU's APIC ID.
 */
uint8_t apic_id(void);

/* Enable the calling (non-boot) CPU's LAPIC and start its 100 Hz timer. */
void apic_init_ap(void);

/* Send an inter-processor interrupt: @command is the ICR low word
 * (vector | delivery mode | level ...). */
void apic_send_ipi(uint32_t dest_apic_id, uint32_t command);
#define APIC_IPI_FIXED   0x4000u                /* fixed delivery, level assert */
#define APIC_IPI_INIT    0x4500u
#define APIC_IPI_SIPI    0x4600u                /* | start page number */

/* TSC ticks per 10 ms (measured against the PIT at boot) */
extern uint64_t g_tsc_per_tick;
void udelay(uint64_t us);
