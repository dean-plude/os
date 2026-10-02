/*
 * hda.h — Intel High Definition Audio controller
 *
 * The controller plays one ring of 48 kHz, 16-bit stereo PCM that the
 * mixer (audio.c) keeps filled ahead of the hardware's read position, and
 * records into a second ring the mixer reads behind the hardware.
 */

#pragma once

#include "../include/types.h"

#define HDA_RATE      48000
#define HDA_CHANNELS  2

/* Find the controller and a codec output path, start the DMA ring
 * (silent).  False when there is no HD Audio device. */
bool        HdaInit(void);
/* After waking from S3: set the controller and codecs up again */
void        HdaResume(void);
const char *HdaName(void);
/* The DMA ring: @size bytes of interleaved s16 stereo frames */
INT16      *HdaRing(UINT32 *size);
/* The hardware's read offset in the ring (bytes) */
UINT32      HdaPosition(void);

/* Recording: an input jack routed to an ADC.  The capture ring fills with
 * 48 kHz s16 stereo frames while HdaCapture(true) runs it. */
bool        HdaCanRecord(void);
const char *HdaInputName(void);
INT16      *HdaCaptureRing(UINT32 *size);
void        HdaCapture(bool run);
UINT32      HdaCapturePosition(void);
