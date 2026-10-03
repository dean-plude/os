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

/* One line on what happened: "firmware 2.12.0.1 running, 2 digital
 * microphones", or why the DSP was left off */
const char *SofStatus(void);

/* hwcheck: the NHLT reader on a modelled table, the firmware manifest
 * reader and the whole boot (core power, ROM, code loader DMA, IPC4) on a
 * modelled DSP; one line per check through @say, returns the failures */
int         SofSelfCheck(void (*say)(void *ctx, const char *line), void *ctx);
