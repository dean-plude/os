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

/* Read the headphone jacks (twice a second, from the mixer thread): the
 * speakers are off while headphones are plugged in */
void        HdaPollJacks(void);

/* Whether a PCI function is an HD Audio controller: HDA_MATCH_CLASS
 * (class 04.03), HDA_MATCH_DSP (an Intel controller with its audio DSP
 * on, class 04.01, known by ID), HDA_MATCH_PROBE (another Intel class
 * 04.01 function: HD Audio only if its registers say so), or none */
#define HDA_MATCH_NONE  0
#define HDA_MATCH_CLASS 1
#define HDA_MATCH_DSP   2
#define HDA_MATCH_PROBE 3
int         HdaPciMatch(UINT16 vendor, UINT16 device, UINT8 cls, UINT8 sub);

/* hwcheck: the controller matching on real machines' IDs, and the codec
 * setup and jack handling against a modelled Realtek ALC257 (the T14 Gen
 * 4's codec); one line per check through @say, returns the failures */
int         HdaSelfCheck(void (*say)(void *ctx, const char *line), void *ctx);
