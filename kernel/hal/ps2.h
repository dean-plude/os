/*
 * ps2.h — i8042 PS/2 controller: keyboard + mouse (polled)
 *
 * Phase 8.  Initializes the 8042 controller, the keyboard (scancode set 1
 * via the controller's translation), and the auxiliary mouse (streaming,
 * 3-byte packets).  Input is polled from the WM event loop and pushed into
 * the input queue; a later phase can switch this to IRQ1/IRQ12 once the
 * IOAPIC redirection entries are wired.
 */

#pragma once

#include "../include/types.h"

/* Probe + initialize the controller, keyboard and mouse. Returns true if a
 * usable controller was found. */
bool ps2_init(void);

/* Drain all currently-available bytes from the controller, decoding them
 * into InputEvents posted to the input queue. Call frequently. */
void ps2_poll(void);

/* A few common set-1 scancodes the shell cares about. */
#define SC_ESC   0x01
#define SC_ENTER 0x1C
