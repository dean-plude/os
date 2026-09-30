/*
 * um_internal.h — structures shared by the kernel/um sources
 */

#pragma once

#include "um.h"
#include "../ke/scheduler.h"
#include "../ke/syscall.h"

#define UM_MAX_PROCS     32
#define UM_MAX_HANDLES   256
#define UM_MAX_REGIONS   256
#define UM_MAX_MODULES   64
#define UM_MAX_THREADS   64

/* Fixed user addresses for the per-process system areas:
 *   PEB (1 page) | loader info (3 pages) | process parameters (4 pages) |
 *   stubs for unimplemented imports (1 page) | ... |
 *   TEBs (2 pages each, one slot per thread) */
#define UM_PEB_VA        UINT64_C(0x00007FFDF0000000)
#define UM_LDR_INFO_VA   (UM_PEB_VA + 0x1000)
#define UM_LDR_INFO_SIZE 0x3000
#define UM_PARAMS_VA     (UM_PEB_VA + 0x4000)
#define UM_PARAMS_PAGES  4
#define UM_STUBS_VA      (UM_PEB_VA + 0x8000)
#define UM_STUB_SIZE     16
#define UM_MAX_STUBS     (0x1000 / UM_STUB_SIZE)
#define UM_TEB_AREA      (UM_PEB_VA + 0x10000)
#define UM_TEB_SIZE      0x2000
#define UM_SYS_SIZE      (0x10000 + UM_MAX_THREADS * UM_TEB_SIZE)
#define UM_STACK_TOP     UINT64_C(0x00007FFDE0000000)        /* first thread */
#define UM_STACK_SIZE    (1024 * 1024)
#define UM_THREAD_STACK  (256 * 1024)                         /* default for new threads */
#define UM_ALLOC_MIN     UINT64_C(0x0000000010000000)   /* NtAllocateVirtualMemory */
#define UM_ALLOC_MAX     UINT64_C(0x00007FFD00000000)
#define UM_DLL_MIN       UINT64_C(0x0000000180000000)   /* relocated DLLs */
#define UM_DLL_MAX       UINT64_C(0x00007FF000000000)

/* 32-bit programs (WoW): the same areas, all below 2 GiB (0x7FFE0000 is
 * KUSER_SHARED_DATA in both) */
#define UM32_PEB_VA      UINT64_C(0x7FF00000)
#define UM32_STACK_TOP   UINT64_C(0x7FE00000)
#define UM32_ALLOC_MIN   UINT64_C(0x00110000)
#define UM32_ALLOC_MAX   UINT64_C(0x7FD00000)
#define UM32_DLL_MIN     UINT64_C(0x10000000)
#define UM32_DLL_MAX     UINT64_C(0x7F000000)

/* A process's address-space layout (64-bit, or 32-bit for WoW) */
typedef struct {
    UINT64 peb, ldr_info, params, stubs, teb_area;
    UINT64 stack_top, alloc_min, alloc_max, dll_min, dll_max;
} UmLayout;

/* NTSTATUS values used here */
#define UM_STATUS_CONTROL_C_EXIT   0xC000013Au
#define UM_STATUS_ACCESS_VIOLATION 0xC0000005u

/* A recursive lock that waits by yielding (never held across user code) */
typedef struct {
    volatile int v;
    Thread      *owner;
    int          depth;
} UmLock;
void um_lock(UmLock *l);
void um_unlock(UmLock *l);

/* -----------------------------------------------------------------------
 * Kernel objects reachable through handles
 * ----------------------------------------------------------------------- */
typedef enum { UO_EVENT = 1, UO_MUTANT, UO_SEMAPHORE, UO_THREAD, UO_SOCKET, UO_WINDOW, UO_PROCESS, UO_KEY, UO_SECTION } UmObType;

typedef struct UmThread UmThread;

typedef struct UmObject {
    UmObType        type;
    volatile int    refs;
    bool            signaled;       /* event state; thread: has ended */
    bool            manual;         /* event: manual reset */
    INT32           count, max;     /* semaphore */
    UmThread       *owner;          /* mutant */
    UINT32          recursion;
    bool            abandoned;
    bool            named;          /* in the object namespace (um_thread.c) */
    int             sock;           /* UO_SOCKET: kernel socket index */
    UmProcess      *proc;           /* UO_PROCESS: signaled when it has exited */
    void           *ptr;            /* UO_KEY: the registry key */
    void          (*destroy)(struct UmObject *o);   /* extra cleanup (sockets, windows) */
} UmObject;

struct UmThread {
    UmObject        ob;             /* signaled when the thread has ended */
    UmProcess      *proc;
    Thread         *kt;             /* scheduler thread; NULL once reclaimed */
    UINT32          tid;
    int             slot;           /* TEB slot */
    UINT64          teb, stack_lo, stack_size;
    UINT64          start, arg;     /* where it begins (via ntdll) */
    volatile INT32  suspend;        /* suspend count */
    volatile bool   terminate;      /* NtTerminateThread from another thread */
    UINT32          term_status;
    volatile bool   exited;
    UINT32          exit_code;
    bool            in_exception;   /* delivering an exception (nested fault = fatal) */
    /* Where the thread's user registers are while it is in the kernel, for
     * NtGetContextThread: 0 unknown, 1 in a system call (user_rsp),
     * 2 at an interrupt (uframe) */
    volatile UINT8  park;
    void           *uframe;
    UINT16          last_sys;       /* the latest system call (diagnostics) */
    /* Waiting (um_thread.c, under g_um_oblock): the objects, the waiter
     * list link, and the flag a signaler sets to wake it */
    UmObject      **wait_objs;
    int             wait_n;
    UmThread       *wait_next;
    volatile UINT32 wake;
};

UmObject *um_ob_ref(UmObject *o);
void      um_ob_unref(UmObject *o);
/* @o became signaled (or acquirable): wake the threads waiting on it.
 * Called with g_um_oblock held. */
void      um_ob_wake(UmObject *o);

typedef enum { H_FREE = 0, H_FILE, H_CON_IN, H_CON_OUT, H_DIR, H_OBJECT } UmHandleKind;

typedef struct {
    UmHandleKind kind;
    RamNode     *node;          /* H_FILE, H_DIR */
    UmObject    *obj;           /* H_OBJECT */
    UINT64       pos;           /* H_FILE: current byte offset; H_DIR: next entry */
    bool         read, write, append, delete_on_close;
} UmHandle;

typedef struct {
    UINT64 base, size;          /* reserved range (page aligned) */
    UINT32 protect;             /* PAGE_* of committed pages */
    bool   image;               /* part of a loaded module */
    struct UmObject *section;   /* a view of this section (referenced), or NULL */
} UmRegion;

typedef struct {
    char   name[32];            /* lower case, e.g. "kernel32.dll" */
    char   path[96];            /* full path on drive C: */
    UINT64 base, size;
    UINT32 entry;               /* entry point RVA (0: none) */
    bool   dll;
} UmModule;

struct UmProcess {
    UINT32      pid;
    bool        wow;            /* a 32-bit (x86) program: compatibility mode, SysWOW64 DLLs */
    UmLayout    lay;            /* where its system areas and allocations go */
    char        name[32];
    UINT64      pml4;           /* physical address of the page table */
    RamNode    *cwd;
    RamNode    *exe_dir;        /* searched for DLLs before System32 */
    UmConsole  *con;
    UmLock      lock;           /* handles, regions, modules, threads */

    UmHandle    handles[UM_MAX_HANDLES];
    UmRegion    regions[UM_MAX_REGIONS];
    int         nregions;
    UmModule    modules[UM_MAX_MODULES];
    int         nmodules;
    UINT8       init_order[UM_MAX_MODULES];   /* dependencies first */
    int         ninit;
    volatile UINT32 pages;      /* resident user pages (backed by memory) */
    UINT32      commit;         /* committed user pages (resident or backed on first touch) */

    UmThread   *threads[UM_MAX_THREADS];
    int         live_threads;
    UINT64      thread_start;   /* ntdll!RtlUserThreadStart */
    UINT64      exc_dispatcher; /* ntdll!KiUserExceptionDispatcher */
    UINT32      stack_reserve;  /* from the image header */

    void       *gui;            /* per-process window-system state (um_gui.c) */
    char      (*stub_names)[64]; /* "dll!function" per stub at UM_STUBS_VA */
    int         nstubs;

    volatile bool   exited;     /* every thread has stopped for good */
    volatile bool   kill_pending;
    UINT32          kill_status;
    UINT32          exit_status;
    char            why[96];    /* crash/kill description */
    bool            released;   /* spawner is done with it */
    UmObject       *exit_ob;    /* UO_PROCESS object of a program-created process (not referenced) */
    bool            reclaimed;  /* memory and handles freed */
};

/* um.c */
UmProcess *UmCurrent(void);                    /* NULL for kernel threads */
void       um_set_layout(UmProcess *p, bool wow);   /* 64-bit or 32-bit (WoW) address layout */
void       um_wow_path(UmProcess *p, char *path);  /* System32 -> SysWOW64 for 32-bit programs */
UINT16     um_pe_machine(const RamNode *f);     /* 0x8664, 0x014C, or 0 if not a PE file */
UmThread  *UmCurrentThread(void);
UINT32     um_new_id(void);
/* The calling thread should stop (its process or itself is being ended) */
bool       um_stopping(void);
/* End the calling thread; the process ends with it if it was the last. */
void       um_exit_thread(UINT32 status) __attribute__((noreturn));
/* End the whole process (every thread) with @status. */
void       um_exit_process(UINT32 status) __attribute__((noreturn));
/* Address-space services (process may be the current one) */
bool       um_is_free(UmProcess *p, UINT64 base, UINT64 size);
UINT64     um_find_free(UmProcess *p, UINT64 size, UINT64 lo, UINT64 hi);
UmRegion  *um_region_add(UmProcess *p, UINT64 base, UINT64 size, UINT32 protect, bool image);
UmRegion  *um_region_find(UmProcess *p, UINT64 va);
void       um_region_remove(UmProcess *p, UmRegion *r);
bool       um_commit(UmProcess *p, UINT64 va, UINT64 size, UINT32 protect);
void       um_decommit(UmProcess *p, UINT64 va, UINT64 size);
bool       um_is_committed(UmProcess *p, UINT64 va);
/* Shared sections: frames the section owns, mapped into processes */
PADDR     *um_alloc_frames(UINT64 n);           /* n zeroed frames; NULL if memory is short */
void       um_free_frames(PADDR *f, UINT64 n);
bool       um_map_frames(UmProcess *p, UINT64 va, const PADDR *f, UINT64 n, UINT32 protect);
void       um_unmap_frames(UmProcess *p, UINT64 va, UINT64 n);
void       um_release_views(UmProcess *p);      /* drop the sections of every view (process teardown) */
void      *um_frame_ptr(PADDR f);               /* a frame's kernel address */
RamNode   *um_handle_file(UmProcess *p, UINT64 h);   /* the file behind a handle (under the locks), or NULL */
void       um_flush_view_at(UmProcess *p, UINT64 va);  /* a file-backed view: write its section back */

/* Copy into/out of user memory through the page tables (any process). */
bool       um_write(UmProcess *p, UINT64 va, const void *src, UINT64 n);
bool       um_read(UmProcess *p, UINT64 va, void *dst, UINT64 n);
/* Create a thread in @p starting at @start(@arg) via ntdll.  NULL on failure. */
UmThread  *um_create_thread(UmProcess *p, UINT64 start, UINT64 arg, UINT64 stack_size,
                            bool suspended, UINT32 *status);
/* Load a DLL (and what it imports) into the running process: *base gets
 * its address; new modules are appended to the loader info page. */
UINT32     um_load_dll(UmProcess *p, const char *name, UINT64 *base);
const UmModule *um_module_at(UmProcess *p, UINT64 va);

/* um_console.c */
UmConsole *um_console_ref(UmConsole *c);
int        um_console_write(UmConsole *c, const char *data, int len);   /* program output */
/* Program reads keyboard input: bytes, 0 at EOF, -1 if killed while waiting */
int        um_console_read(UmConsole *c, char *buf, int cap, UmProcess *p);

/* um_syscall.c */
void       um_syscall_init(void);
void       um_close_all_handles(UmProcess *p);
void       um_install(UINT32 num, SYSCALL_HANDLER h);
void       um_lock_free(UINT32 num);

/* Synchronization objects' state (signaled, owner, count...) and thread
 * and process exit flags: short sections under one spinlock, so a wait's
 * check-and-take is atomic on any CPU */
#include "../ke/spinlock.h"
extern KSpinLock g_um_oblock;
static inline IrqState ob_lock(void)          { return spin_lock_irqsave(&g_um_oblock); }
static inline void     ob_unlock(IrqState s)  { spin_unlock_irqrestore(&g_um_oblock, s); }     /* mark a service as running without the big kernel lock */
UINT64     um_stack_arg(int n);                 /* syscall argument n >= 5 */
UINT64     um_now_100ns(void);                  /* system time (100 ns since 1601) */
UINT64     um_handle_new_object(UmProcess *p, UmObject *o);   /* takes a reference; 0 if full */
UmObject  *um_handle_object(UmProcess *p, UINT64 h, UmObType type);   /* referenced; NULL if bad */
UINT64     um_close_handle(UINT64 h);           /* NtClose for the current process */
/* UmSpawn, with standard handles taken from the creating process (@std:
 * three entries, kind H_FREE = the console default) */
UmProcess *um_spawn_ex(RamNode *exe, const char *cmdline, RamNode *cwd, UmConsole *con,
                       const UmHandle *std, char *err, int err_cap);

/* um_thread.c: threads, synchronization objects, waits */
void       um_thread_syscalls_init(void);
void       um_socket_syscalls_init(void);
void       um_gui_syscalls_init(void);
void       um_gui_process_gone(UmProcess *p);   /* destroy the process's windows */
/* Wait until @o is signaled (acquiring it), @timeout_100ns passes (-1:
 * forever) or the process is being killed. */
UINT32     um_wait_one(UmObject *o, INT64 timeout_100ns);
void       um_abandon_mutants(UmProcess *p, UmThread *t);

/* um_exception.c: SEH delivery */
void       um_exception_syscalls_init(void);
/* um_registry.c */
void       um_registry_init(void);
void       um_registry_syscalls_init(void);
void       um_registry_poll(void);   /* save the hive after changes (desktop thread) */
void       um_registry_flush(void);  /* save the hive now if it changed */
