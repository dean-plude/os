/*
 * gpio.h — Intel GPIO controllers (the chipset's pin controller)
 *
 * A laptop touchpad signals "I have a report" on a GPIO pin; the ACPI
 * namespace names the pin in the device's GpioInt resource: the GPIO
 * controller (\_SB.GPI0) and the pin's number in that controller's ACPI
 * numbering.  gpio.c finds the controllers (INTC1055 and its relatives)
 * while the ACPI namespace loads, routes their shared interrupt through
 * the I/O APIC, and hands each connected pin's interrupt to its driver.
 */

#pragma once

#include "../include/types.h"

/* aml.c, while loading the namespace: find the GPIO controllers, mask
 * every pin's interrupt and route the controllers' own */
void GpioProbe(void);

/* After S3: the pads the drivers use and their enables come back */
void GpioResume(void);

typedef struct GpioIrq GpioIrq;

/* Connect @fn to pin @pin of the controller at ACPI path @ctrl (as
 * uACPI prints it, \_SB_.GPI0): the pad is set up as a GPIO input
 * interrupting on @level (else edge) @low (else high; @both: both
 * edges), its interrupt enabled.  @fn runs in interrupt context with the
 * kernel lock held; for a level-triggered pin it runs with the pin masked,
 * and GpioIrqUnmask() enables it again once the device let go of the line.
 * NULL, with the reason in @why, when that can't be done. */
GpioIrq *GpioIrqConnect(const char *ctrl, UINT16 pin, bool level, bool low, bool both,
                        void (*fn)(void *ctx), void *ctx, char *why, int whycap);
void     GpioIrqDisconnect(GpioIrq *g);
void     GpioIrqUnmask(GpioIrq *g);

/* The pad's name and controller ("GPP_C21 on \_SB_.GPI0"), whether the
 * device asserts the line now, and the pad's state: its first
 * configuration register, interrupt enable and status */
const char *GpioIrqName(GpioIrq *g);
bool        GpioIrqAsserted(GpioIrq *g);
void        GpioIrqState(GpioIrq *g, UINT32 *padcfg0, bool *enabled, bool *status);
UINT32      GpioIrqCount(GpioIrq *g);         /* interrupts handed to @fn */

/* The controller is the self-tests' model (_HID NOVA1055, no registers):
 * GpioModelSetLine() is the modelled device driving the pin */
bool GpioIrqIsModel(GpioIrq *g);
bool GpioControllerIsModel(const char *ctrl);
void GpioModelSetLine(GpioIrq *g, bool asserted);

/* Terminal `devices`-style summary of what was found, for hwcheck */
const char *GpioStatus(void);
