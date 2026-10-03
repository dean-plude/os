/*
 * syscursor.h — the system pointers (IDC_* / OCR_* shapes), drawn from
 * vector outlines so they are sharp at every display scale
 */
#pragma once

#include "../include/types.h"

/* Windows' OCR_* numbers, the same as IDC_* */
#define OCR_NORMAL      32512
#define OCR_IBEAM       32513
#define OCR_WAIT        32514
#define OCR_CROSS       32515
#define OCR_UP          32516
#define OCR_SIZE        32640       /* obsolete: SIZEALL */
#define OCR_ICON        32641       /* obsolete: the arrow */
#define OCR_SIZENWSE    32642
#define OCR_SIZENESW    32643
#define OCR_SIZEWE      32644
#define OCR_SIZENS      32645
#define OCR_SIZEALL     32646
#define OCR_NO          32648
#define OCR_HAND        32649
#define OCR_APPSTARTING 32650
#define OCR_HELP        32651
#define OCR_PIN         32671
#define OCR_PERSON      32672

/* Every shape fits a box of SYSCUR_BOX logical pixels a side */
#define SYSCUR_BOX      32
/* Animated shapes (the busy ring) have this many phases per turn */
#define SYSCUR_PHASES   64

/* The shape drawn for @id (aliases resolved: OCR_SIZE is OCR_SIZEALL),
 * or 0 if @id names no system pointer */
int  SysCursorCanon(int id);
bool SysCursorAnimated(int id);

/* Renders pointer @id into @argb: SYSCUR_BOX * @scale pixels a side,
 * 0xAARRGGBB, not premultiplied.  @phase (0..SYSCUR_PHASES-1) turns the
 * busy ring; @shadow adds the soft drop shadow the desktop draws under
 * the pointer.  The hot spot, in pixels of @argb, goes to *hx, *hy.
 * @scratch: SYSCUR_SCRATCH bytes of the caller's (so no lock is needed:
 * a program's user32 asks for images while it starts, and the desktop
 * may be waiting on that start with its lock held). */
#define SYSCUR_SCRATCH  12288
void SysCursorRender(int id, int scale, int phase, bool shadow, UINT32 *argb, int *hx, int *hy, void *scratch);
