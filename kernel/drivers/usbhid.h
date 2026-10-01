/*
 * usbhid.h — HID boot-protocol keyboard and mouse reports
 *
 * Turns the 8-byte boot keyboard report and the 3/4-byte boot mouse report
 * into InputEvents: keys become set-1 scancodes (the codes a PS/2 keyboard
 * sends), so everything above the drivers sees one kind of keyboard.
 * USB keyboards do not repeat keys themselves; UsbHidTick() does it.
 */

#pragma once

#include "../include/types.h"

typedef struct {
    UINT8  prev[8];          /* last report: modifiers, reserved, 6 usages */
    UINT8  repeat;           /* usage being auto-repeated, 0 = none */
    UINT64 repeat_at;        /* tick of its next repeat */
} UsbHidKbd;

void UsbHidKeyboardReport(UsbHidKbd *k, const UINT8 *r, int len, UINT64 now);
void UsbHidMouseReport(const UINT8 *r, int len);
/* Release every key the keyboard still holds (it was unplugged). */
void UsbHidKeyboardGone(UsbHidKbd *k);
/* Typematic repeat of the held key: call every tick. */
void UsbHidTick(UsbHidKbd *k, UINT64 now);
