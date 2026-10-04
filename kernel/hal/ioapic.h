/*
 * ioapic.h — I/O APICs: routing a device interrupt to a CPU
 *
 * NovaOS polls most of its devices, so the I/O APICs only carry the ACPI
 * SCI, the GPIO controllers' interrupts (a touchpad's pin, gpio.c) and an
 * I2C-HID device's own interrupt line.  The MADT says where each I/O APIC is and how
 * ISA interrupts map to its inputs (interrupt source overrides).  Every
 * input is masked at start-up; a routed one goes to the boot CPU.
 */

#pragma once

#include "../include/types.h"

/* Find the I/O APICs (MADT) and mask every input; false if there are none */
bool IoApicInit(void);
bool IoApicPresent(void);

/* Route ISA IRQ @irq (or a GSI, if no override names it) to @vector on the
 * boot CPU, level- or edge-triggered and active low or high as the MADT's
 * override says, else as @level / @low say.  False if no I/O APIC has it. */
bool IoApicRouteIrq(UINT32 irq, bool level, bool low, UINT8 vector, UINT32 *gsi);

/* Mask or unmask a routed input */
void IoApicMask(UINT32 gsi, bool mask);

/* After waking from S3: mask everything, then route what was routed again */
void IoApicResume(void);

/* Interrupt handlers for the vectors IoApicRouteIrq is given: called with
 * the kernel lock held, before the EOI */
typedef void (*IrqHandler)(void *ctx);
bool IrqInstall(UINT8 vector, IrqHandler fn, void *ctx);
bool IrqDispatch(UINT8 vector);          /* idt.c: false if nothing is installed */
