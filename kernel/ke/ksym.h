/* ksym.h — kernel function names and backtraces (ksym.c) */
#pragma once
#include "../include/types.h"

/* The function containing @addr, and the offset into it; NULL if none */
const char *KsymLookup(UINT64 addr, UINT64 *offset);
/* Print "Backtrace:" and a symbolized frame per line on the serial log,
 * from @rip and the RBP chain at @rbp, on the stack at @rsp */
void KsymBacktrace(UINT64 rip, UINT64 rbp, UINT64 rsp);
/* The same from the caller of KsymBacktraceHere */
void KsymBacktraceHere(void);
/* A deliberate kernel page fault, three calls deep (NtNovaBugCheck) */
void KeCrashTest(void);
