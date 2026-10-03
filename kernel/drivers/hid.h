/*
 * hid.h — HID devices on buses other than USB
 *
 * usbhid.c's report-descriptor parser and report decoding (keys, mice,
 * touchpads in mouse mode, touch screens, pens) serve I2C-HID devices
 * (i2chid.c) too.
 */

#pragma once

#include "../include/types.h"
#include "../wm/input.h"

/* Read report descriptor @desc (@len bytes); NULL when the device has
 * nothing NovaOS uses.  @kind says what it is ("touchpad (mouse mode)",
 * "keyboard", ...).  Nothing reaches the desktop until HidStart(). */
void *HidAttach(const UINT8 *desc, int len, const char **kind);
void  HidStart(void *hid);
/* One input report (starting with its report ID if the device uses IDs) */
void  HidInput(void *hid, const UINT8 *report, int len);

/* Checks: @hid's events are collected instead of posted, until
 * HidCaptureEnd() hands them over (at most @max) */
void  HidCaptureBegin(void *hid);
int   HidCaptureEnd(InputEvent *out, int max);
