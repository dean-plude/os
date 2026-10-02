/* prof.h — a sampling profiler on the timer tick (prof.c) */
#pragma once
#include "../include/types.h"

/* Start sampling every CPU's timer tick (forgetting earlier samples) */
void ProfStart(void);
/* Stop, and print where the time went on the serial log and through @out */
void ProfReport(void (*out)(void *ctx, const char *line), void *ctx);
/* The timer tick: note where @rip (kernel or user, @rbp its frame) was */
void ProfSample(UINT64 rip, UINT64 rbp, bool user, bool lock_wait, bool idle);
