/* prof.h — a sampling profiler on the timer tick (prof.c) */
#pragma once
#include "../include/types.h"

/* Start sampling every CPU's timer tick (forgetting earlier samples),
 * from @delay ticks on for @len ticks (0: until ProfReport) */
void ProfStart(UINT64 delay, UINT64 len);
/* Stop, and print where the time went on the serial log and through @out */
void ProfReport(void (*out)(void *ctx, const char *line), void *ctx);
/* The timer tick: note where @rip (kernel or user, @rbp its frame) was */
void ProfSample(UINT64 rip, UINT64 rbp, bool user, bool lock_wait, bool idle);
/* A program's system call @num (counted while profiling) */
void ProfSyscall(UINT64 num);
/* An interrupt or exception @vector taken under the big kernel lock */
void ProfInterrupt(UINT64 vector);
