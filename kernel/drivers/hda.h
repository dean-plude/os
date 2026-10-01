/*
 * hda.h — Intel High Definition Audio controller (output)
 *
 * The controller plays one ring of 48 kHz, 16-bit stereo PCM that the
 * mixer (audio.c) keeps filled ahead of the hardware's read position.
 */

#pragma once

#include "../include/types.h"

#define HDA_RATE      48000
#define HDA_CHANNELS  2

/* Find the controller and a codec output path, start the DMA ring
 * (silent).  False when there is no HD Audio device. */
bool        HdaInit(void);
const char *HdaName(void);
/* The DMA ring: @size bytes of interleaved s16 stereo frames */
INT16      *HdaRing(UINT32 *size);
/* The hardware's read offset in the ring (bytes) */
UINT32      HdaPosition(void);
