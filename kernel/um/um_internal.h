/*
 * um_internal.h — structures shared by um.c, um_console.c, um_syscall.c
 */

#pragma once

#include "um.h"
#include "../ke/scheduler.h"

#define UM_MAX_PROCS     32
#define UM_MAX_HANDLES   64
#define UM_MAX_REGIONS   128
#define UM_MAX_MODULES   16

/* Fixed user addresses for the per-process system areas */
#define UM_PEB_VA        UINT64_C(0x00007FFDF0000000)
#define UM_TEB_VA        (UM_PEB_VA + 0x1000)
#define UM_PARAMS_VA     (UM_PEB_VA + 0x2000)      /* 4 pages: params + strings */
#define UM_PARAMS_PAGES  4
#define UM_STACK_TOP     UINT64_C(0x00007FFDE0000000)
#define UM_STACK_SIZE    (1024 * 1024)
#define UM_ALLOC_MIN     UINT64_C(0x0000000010000000)   /* NtAllocateVirtualMemory */
#define UM_ALLOC_MAX     UINT64_C(0x00007FFD00000000)

/* NTSTATUS values used here */
#define UM_STATUS_CONTROL_C_EXIT   0xC000013Au
#define UM_STATUS_ACCESS_VIOLATION 0xC0000005u

typedef enum { H_FREE = 0, H_FILE, H_CON_IN, H_CON_OUT, H_DIR } UmHandleKind;

typedef struct {
    UmHandleKind kind;
    RamNode     *node;          /* H_FILE, H_DIR */
    UINT64       pos;           /* H_FILE: current byte offset; H_DIR: next entry */
    bool         read, write, append, delete_on_close;
} UmHandle;

typedef struct {
    UINT64 base, size;          /* reserved range (page aligned) */
    UINT32 protect;             /* PAGE_* of committed pages */
    bool   image;               /* part of a loaded module */
} UmRegion;

typedef struct {
    char   name[32];            /* lower case, e.g. "kernel32.dll" */
    UINT64 base, size;
} UmModule;

struct UmProcess {
    UINT32      pid;
    char        name[32];
    UINT64      pml4;           /* physical address of the page table */
    Thread     *thread;
    RamNode    *cwd;
    UmConsole  *con;

    UmHandle    handles[UM_MAX_HANDLES];
    UmRegion    regions[UM_MAX_REGIONS];
    int         nregions;
    UmModule    modules[UM_MAX_MODULES];
    int         nmodules;
    UINT32      pages;          /* committed user pages */

    volatile bool   exited;     /* thread has stopped for good */
    volatile bool   kill_pending;
    UINT32          kill_status;
    UINT32          exit_status;
    char            why[96];    /* crash/kill description */
    bool            released;   /* spawner is done with it */
    bool            reclaimed;  /* memory and handles freed */
};

/* um.c */
UmProcess *UmCurrent(void);                    /* NULL for kernel threads */
void       UmExitCurrent(UINT32 status) __attribute__((noreturn));
/* Address-space services (process may be the current one) */
bool       um_is_free(UmProcess *p, UINT64 base, UINT64 size);
UINT64     um_find_free(UmProcess *p, UINT64 size, UINT64 lo, UINT64 hi);
UmRegion  *um_region_add(UmProcess *p, UINT64 base, UINT64 size, UINT32 protect, bool image);
UmRegion  *um_region_find(UmProcess *p, UINT64 va);
void       um_region_remove(UmProcess *p, UmRegion *r);
bool       um_commit(UmProcess *p, UINT64 va, UINT64 size, UINT32 protect);
void       um_decommit(UmProcess *p, UINT64 va, UINT64 size);
bool       um_is_committed(UmProcess *p, UINT64 va);
/* Copy into/out of user memory through the page tables (any process). */
bool       um_write(UmProcess *p, UINT64 va, const void *src, UINT64 n);
bool       um_read(UmProcess *p, UINT64 va, void *dst, UINT64 n);

/* um_console.c */
UmConsole *um_console_ref(UmConsole *c);
int        um_console_write(UmConsole *c, const char *data, int len);   /* program output */
/* Program reads keyboard input: bytes, 0 at EOF, -1 if killed while waiting */
int        um_console_read(UmConsole *c, char *buf, int cap, UmProcess *p);

/* um_syscall.c */
void       um_syscall_init(void);
void       um_close_all_handles(UmProcess *p);
