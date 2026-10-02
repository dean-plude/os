/*
 * xhci.h — USB 3 (xHCI) host controller driver
 *
 * Brings up the xHCI controller, enumerates the devices on its root ports
 * and behind hubs, and offers their interfaces to the class drivers
 * (usb.h): hubs, HID keyboards, mice, tablets and touch screens (which
 * feed the same input queue as the PS/2 driver) and mass storage.  Like
 * the other drivers it is polled: device interrupts stay off and
 * XhciPoll(), called from the timer tick, drains the event ring.  Devices
 * plugged in later are enumerated by a small kernel thread.
 */

#pragma once

#include "../include/types.h"

/* Probe every controller and attach what is plugged in; returns the number
 * of HID devices bound.  Needs InputInit() first. */
int  XhciInit(void);
/* Drain the event rings (timer tick, any CPU). */
void XhciPoll(void);
/* After waking from S3: restart the controller and enumerate again */
void XhciResume(void);
