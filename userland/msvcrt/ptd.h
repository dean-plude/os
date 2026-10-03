/* ptd.h — the C runtime's per-thread data (ptd.c) */
#pragma once
#include <time.h>
#include <wchar.h>

typedef struct NovaPtd {
    int            err;             /* errno */
    unsigned long  doserr;          /* _doserrno: the Win32 error behind errno */
    int            fpecode;         /* _fpecode */
    unsigned long  rand;            /* rand's seed: 1 on a new thread */
    char          *tok;             /* strtok's position */
    wchar_t       *wtok;            /* _wcstok's position */
    struct tm      tm;              /* gmtime / localtime result */
    char           asc[32];         /* asctime / ctime result */
    wchar_t        wcserr[128];     /* _wcserror result */
    char           tmpnam[260 * 3]; /* tmpnam(NULL) result */
    wchar_t        wtmpnam[260 + 24];
} NovaPtd;

NovaPtd *__nova_ptd(void);          /* never NULL */
