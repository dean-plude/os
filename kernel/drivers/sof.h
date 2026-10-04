/*
 * sof.h — Intel's audio DSP (cAVS 2.5: Tiger Lake, Alder Lake, Raptor
 * Lake) running Sound Open Firmware, the way to a laptop's digital
 * microphones
 */

#pragma once

#include "../include/types.h"

/* After HdaInit(): when the HD Audio controller is one with its DSP on and
 * the ACPI NHLT table lists digital microphones, boot the DSP with the
 * SOF firmware built into the system (a thread: boot does not wait) */
void        SofStart(void);

/* After sleep (from the resume path, after HdaResume; does not wait): the
 * DSP lost its firmware and pipeline with its power, so its thread boots
 * it again from the same file and builds the pipeline again */
void        SofResume(void);

/* One line on what happened: "firmware 2.12.0.1 running, 2 digital
 * microphones (48000 Hz, 2 channels), recording, level 3%" (or "paused
 * while no program records"), or why the DSP or its capture pipeline was
 * left off */
const char *SofStatus(void);

/* The microphones' samples: a ring of @size bytes the DSP's host DMA
 * fills with interleaved 16-bit frames at @rate, @channels to a frame
 * (the microphones' own count: 2 or 4); false while nothing records */
bool        SofCaptureRing(const INT16 **ring, UINT32 *size, UINT32 *rate, UINT32 *channels);
/* How far the DSP has written into that ring (bytes; the stream's LPIB) */
UINT32      SofCapturePosition(void);

/* hwcheck: the NHLT reader on a modelled table, the firmware manifest
 * reader, the whole boot (core power, ROM, code loader DMA, IPC4) and the
 * capture pipeline (copiers, BIND, states, the host ring) on a modelled
 * DSP; one line per check through @say, returns the failures */
int         SofSelfCheck(void (*say)(void *ctx, const char *line), void *ctx);

/* hwcheck mic: a live modelled DSP on a machine without one (QEMU) whose
 * DMA writes a 1 kHz tone, booted as the real one is and attached as the
 * recording device "Microphone Array (DSP model)"; hwcheck mic sleep: it
 * loses its power as in S3 and SofResume() boots it again.  One line
 * each through @say; return the failures. */
int         SofModelMicrophones(void (*say)(void *ctx, const char *line), void *ctx);
int         SofModelSleep(void (*say)(void *ctx, const char *line), void *ctx);
