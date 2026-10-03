/*
 * tablet.h — pen tablets: what wintab32.dll reads
 *
 * Pens (USB HID digitizer pens, and the synthetic pens programs make with
 * CreateSyntheticPointerDevice) post INPUT_PEN events beside the pointer
 * motion they cause; the desktop loop hands each to TabletPacketIn, which
 * keeps the last TABLET_RING packets, numbered, for every program to read
 * (NtNovaGuiCtl op 30, kernel/um/um_gui.c).  Pens that report tilt
 * (X and Y, from upright) or barrel rotation say so when they come
 * (TabletDevice's caps), and each packet carries them with TABLET_TILT /
 * TABLET_TWIST set; a pen that can't report one leaves it 0.
 */
#pragma once

#include "../include/types.h"
#include "input.h"

#define TABLET_RING      256
#define TABLET_PRESSURE  1023        /* pressure runs 0..TABLET_PRESSURE */

/* TabletPacket.flags */
#define TABLET_INRANGE   1           /* the pen is over the tablet */
#define TABLET_ERASER    2           /* ... with its eraser end */
#define TABLET_TILT      4           /* tilt_x, tilt_y are the pen's */
#define TABLET_TWIST     8           /* twist is the pen's */

/* TabletDevice's caps, TabletCaps */
#define TABLET_CAP_TILT  1
#define TABLET_CAP_TWIST 2

typedef struct {
    UINT32 serial;                   /* 1, 2, 3... */
    UINT32 time;                     /* ms since boot */
    INT32  x, y;                     /* 0-65535 across the desktop, y down */
    UINT16 pressure;                 /* 0..TABLET_PRESSURE */
    UINT8  buttons;                  /* bit 0 the tip, 1 the barrel button, 2 the second one */
    UINT8  flags;                    /* TABLET_* */
    INT16  tilt_x, tilt_y;           /* tenths of a degree, -900..900 (InputEvent's) */
    UINT16 twist;                    /* tenths of a degree clockwise, 0..3599 */
    UINT16 reserved;
} TabletPacket;

/* A pen device came (+1) or went (-1); @owner is the process of a
 * synthetic pen (NULL for hardware; synthetic pens have every cap), @caps
 * the TABLET_CAP_* it reports */
void TabletDevice(void *owner, int delta, int caps);
/* The process went: its synthetic pens go too */
void TabletOwnerGone(void *owner);
/* Has @owner a synthetic pen? */
bool TabletOwns(void *owner);
/* Pen devices present now */
int  TabletDevices(void);
/* What the pens present can report: TABLET_CAP_* (any of them) */
int  TabletCaps(void);
/* An INPUT_PEN event (desktop loop); returns its packet's number */
UINT32 TabletPacketIn(const InputEvent *ev);
/* Packets numbered after @after (the oldest still kept first), up to @max;
 * waits up to @wait_ticks for one if there are none (and max isn't 0).
 * Returns how many; *newest (may be NULL) gets the newest packet's number. */
int  TabletRead(UINT32 after, TabletPacket *out, int max, UINT64 wait_ticks, UINT32 *newest);
