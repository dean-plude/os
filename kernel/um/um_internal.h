/*
 * um_internal.h — structures shared by the kernel/um sources
 */

#pragma once

#include "um.h"
#include "../ke/scheduler.h"
#include "../ke/syscall.h"

#define UM_MAX_PROCS     32
#define UM_MAX_HANDLES   4096
#define UM_MAX_REGIONS   8192     /* (runtimes such as CoreCLR reserve thousands of ranges) */
#define UM_MAX_MODULES   1024     /* (Audacity loads about 150, VLC every plugin: about 410) */
#define UM_MAX_DLL_DIRS  16       /* AddDllDirectory's folders */
#define UM_MAX_THREADS   256      /* (a browser's main process runs well over 64) */
#define UM_THREAD_MMCSS  0x4E4D   /* NtSetInformationThread class: avrt.dll's MMCSS (um_thread.c) */
#define UM32_MAX_THREADS 96       /* WoW: the TEB area must stay below KUSER_SHARED_DATA */

/* Fixed user addresses for the per-process system areas:
 *   PEB (1 page) | loader info (47 pages) | process parameters (4 pages) |
 *   stubs for unimplemented imports (4 pages) | TEBs (2 pages each, one
 *   slot per thread) */
#define UM_PEB_VA        UINT64_C(0x00007FFDF0000000)
#define UM_LDR_INFO_VA   (UM_PEB_VA + 0x1000)
#define UM_LDR_INFO_SIZE 0x2F000                                /* 8 + 184 * UM_MAX_MODULES, rounded up */
#define UM_PARAMS_OFF    0x30000
#define UM_PARAMS_VA     (UM_PEB_VA + UM_PARAMS_OFF)
#define UM_PARAMS_PAGES  4
#define UM_CMDLINE_MAX   32766    /* UTF-16 units in a command line (32,767 with its NUL, as Windows) */
#define UM_STUBS_OFF     0x34000
#define UM_STUBS_VA      (UM_PEB_VA + UM_STUBS_OFF)
#define UM_STUBS_PAGES   4
#define UM_STUB_SIZE     16
#define UM_MAX_STUBS     (UM_STUBS_PAGES * 0x1000 / UM_STUB_SIZE)
#define UM_TEB_OFF       0x38000
#define UM_TEB_AREA      (UM_PEB_VA + UM_TEB_OFF)
#define UM_TEB_SIZE      0x2000
#define UM_SYS_SIZE(n)   (UM_TEB_OFF + (UINT64)(n) * UM_TEB_SIZE)
#define UM_STACK_TOP     UINT64_C(0x00007FFDE0000000)        /* first thread */
#define UM_STACK_SIZE    (1024 * 1024)
#define UM_THREAD_STACK  (256 * 1024)                         /* default for new threads */
#define UM_ALLOC_MIN     UINT64_C(0x0000000010000000)   /* NtAllocateVirtualMemory */
#define UM_ALLOC_MAX     UINT64_C(0x00007FFD00000000)
#define UM_DLL_MIN       UINT64_C(0x0000000180000000)   /* relocated DLLs */
#define UM_DLL_MAX       UINT64_C(0x00007FF000000000)

/* 32-bit programs (WoW): the same areas, all below 2 GiB (0x7FFE0000 is
 * KUSER_SHARED_DATA in both) */
#define UM32_PEB_VA      UINT64_C(0x7FEE0000)     /* its system areas end below KUSER_SHARED_DATA (0x7FFE0000) */
#define UM32_STACK_TOP   UINT64_C(0x7FE00000)
#define UM32_ALLOC_MIN   UINT64_C(0x00110000)
#define UM32_ALLOC_MAX   UINT64_C(0x7FD00000)
#define UM32_DLL_MIN     UINT64_C(0x10000000)
#define UM32_DLL_MAX     UINT64_C(0x7F000000)

/* A process's address-space layout (64-bit, or 32-bit for WoW) */
typedef struct {
    UINT64 peb, ldr_info, params, stubs, teb_area;
    UINT64 stack_top, alloc_min, alloc_max, dll_min, dll_max;
    int max_threads;
} UmLayout;

/* NTSTATUS values used here */
#define UM_STATUS_CONTROL_C_EXIT   0xC000013Au
#define UM_STATUS_ACCESS_VIOLATION 0xC0000005u
#define UM_STATUS_NOT_SAME_OBJECT  0xC00001ACu

/* A recursive lock that waits by yielding (never held across user code) */
typedef struct {
    volatile int v;
    Thread      *owner;
    int          depth;
} UmLock;
void um_lock(UmLock *l);
void um_unlock(UmLock *l);

/* A reader/writer lock on the same terms: one writer (recursive, and may
 * take it shared too) or many readers, who must not take it twice. */
typedef struct {
    UmLock       w;
    struct { volatile int n; } __attribute__((aligned(64))) readers[16];   /* by CPU (MAX_CPUS): each on a line of its own */
} __attribute__((aligned(64))) UmRwLock;
void um_lock_shared(UmRwLock *l);
void um_unlock_shared(UmRwLock *l);
void um_lock_excl(UmRwLock *l);
void um_unlock_excl(UmRwLock *l);

/* -----------------------------------------------------------------------
 * Kernel objects reachable through handles
 * ----------------------------------------------------------------------- */
typedef enum { UO_EVENT = 1, UO_MUTANT, UO_SEMAPHORE, UO_THREAD, UO_SOCKET, UO_WINDOW, UO_PROCESS, UO_KEY, UO_SECTION, UO_PIPE,
               UO_DIRECTORY, UO_SYMLINK, UO_TIMER, UO_AUDIO, UO_CONSOLE, UO_TOKEN, UO_GPU } UmObType;

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
    bool            free_unlocked;  /* @destroy needs no big kernel lock */
    int             sock;           /* UO_SOCKET: kernel socket index */
    int             audio;          /* UO_AUDIO: mixer stream (drivers/audio.c) */
    UmProcess      *proc;           /* UO_PROCESS: signaled when it has exited */
    void           *ptr;            /* UO_KEY: the registry key; UO_DIRECTORY: its name;
                                       UO_SYMLINK: its target (UmLinkTarget) */
    UINT64          due;            /* UO_TIMER: the TSC it fires at (0: not set) */
    UINT64          period;         /* UO_TIMER: TSC cycles between firings (0: once) */
    void          (*destroy)(struct UmObject *o);   /* extra cleanup (sockets, windows) */
    void           *sd;             /* its security descriptor (um_security.c), or NULL: open to all */
} UmObject;

struct UmThread {
    UmObject        ob;             /* signaled when the thread has ended */
    UmProcess      *proc;
    UmObject       *imp;            /* the token it impersonates (referenced), or NULL */
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
    UINT32          oa_attrs;       /* OBJECT_ATTRIBUTES.Attributes of its latest path (um_syscall.c) */
    UINT64          oa_sd;          /* and its SecurityDescriptor (a user pointer) */
    UINT64          last_a1;        /* and its first argument */
    /* Waiting (um_thread.c, under g_um_oblock): the objects, the waiter
     * list link, and the flag a signaler sets to wake it */
    UmObject      **wait_objs;
    int             wait_n;
    UmThread       *wait_next;
    volatile UINT32 wake;
    volatile UINT32 alerted;        /* NtAlertThreadByThreadId, taken by NtWaitForAlertByThreadId */
    /* Its base priority relative to its process's class (SetThreadPriority:
     * -2..2, -7..6 in a real-time process, or +-16: saturated at the top
     * or bottom of the class's range, TIME_CRITICAL and IDLE) */
    INT8            prio_incr;
    bool            no_boost;       /* SetThreadPriorityBoost(TRUE) */
    UINT8           mm_priority;    /* registered with MMCSS (avrt.dll): its real-time priority, else 0 */
    bool            auto_align;     /* ThreadEnableAlignmentFaultFixup (um_gpfault.c) */
    bool            hide_debug;     /* ThreadHideFromDebugger: set, and asked back (no debugger events either way) */
};

UmObject *um_ob_ref(UmObject *o);
void      um_ob_unref(UmObject *o);
/* @o became signaled (or acquirable): wake the threads waiting on it.
 * Called with g_um_oblock held. */
void      um_ob_wake(UmObject *o);
/* The same with the wake-up boost @boost (BOOST_*, scheduler.h) for the
 * threads woken; um_ob_wake gives an event's, a semaphore's or a mutex's
 * (+1) */
void      um_ob_wake_boost(UmObject *o, int boost);

typedef enum { H_FREE = 0, H_FILE, H_CON_IN, H_CON_OUT, H_DIR, H_OBJECT, H_NULL } UmHandleKind;

/* An open file's byte offset.  Like a Windows file object's
 * CurrentByteOffset it belongs to the open, not the handle: duplicates and
 * inherited copies of a handle share it (refs), so a child process writing
 * to an inherited log file appends after its parent.  Changed with the
 * desktop lock held. */
typedef struct {
    volatile INT32 refs;
    UINT64         pos;
} UmFilePos;

UmFilePos *um_fpos_new(void);
void       um_fpos_ref(UmFilePos *f);
void       um_fpos_unref(UmFilePos *f);

typedef struct {
    UmHandleKind kind;
    RamNode     *node;          /* H_FILE, H_DIR */
    UmObject    *obj;           /* H_OBJECT */
    UINT64       pos;           /* H_DIR: next entry (H_FILE without fp: its byte offset) */
    UmFilePos   *fp;            /* H_FILE: the shared byte offset (referenced) */
    bool         read, write, append, delete_on_close;
    bool         inherit;       /* passed to child processes (bInheritHandles) */
    bool         async;         /* H_FILE: opened for overlapped I/O */
    bool         npfs;          /* H_NULL: the \Device\NamedPipe\ directory (a RootDirectory for pipe names) */
} UmHandle;

typedef struct {
    UINT64 base, size;          /* reserved range (page aligned) */
    UINT32 protect;             /* PAGE_* of committed pages */
    bool   image;               /* part of a loaded module */
    struct UmObject *section;   /* a view of this section (referenced), or NULL */
} UmRegion;

typedef struct {
    char   name[64];            /* lower case, e.g. "kernel32.dll" */
    char   path[96];            /* full path on drive C: */
    UINT64 base, size;
    UINT32 entry;               /* entry point RVA (0: none) */
    bool   dll;
} UmModule;

struct UmProcess {
    UINT32      pid;
    UINT32      parent_pid;     /* the process that started it (0: the system) */
    UINT64      create_time;    /* 100 ns units since 1601 */
    bool        wow;            /* a 32-bit (x86) program: compatibility mode, SysWOW64 DLLs */
    UmLayout    lay;            /* where its system areas and allocations go */
    char        name[32];
    UINT64      pml4;           /* physical address of the page table */
    RamNode    *cwd;
    RamNode    *exe_dir;        /* searched for DLLs before System32 */
    char        dll_dirs[UM_MAX_DLL_DIRS][RAMFS_PATH_MAX];  /* AddDllDirectory's ("": a free slot) */
    char        dll_dir[RAMFS_PATH_MAX];                    /* SetDllDirectory's ("": none) */
    UmConsole  *con;
    char        log_line[256];  /* a detached process's unfinished console line (um_console_write) */
    int         log_n;
    UmRwLock    lock;           /* handles, regions, modules, threads */
    UmLock      ldr_lock;       /* one runtime DLL load at a time (taken before the desktop lock) */
    UINT64      image_base, image_entry;   /* the program's, between um_spawn_image and _finish */

    UmHandle    handles[UM_MAX_HANDLES];
    volatile UINT8 hbusy[UM_MAX_HANDLES];   /* a slot's own lock, for object handles (see um_syscall.c) */
    UmRegion   *regions;        /* [UM_MAX_REGIONS], allocated with the process */
    int         nregions;
    UmModule    modules[UM_MAX_MODULES];
    int         nmodules;
    RamNode    *images[UM_MAX_MODULES];   /* its program and DLL files, held (in use: not deleted or replaced) */
    int         nimages;
    UINT16      init_order[UM_MAX_MODULES];   /* dependencies first */
    int         ninit;
    volatile UINT32 pages;      /* resident user pages (backed by memory) */
    UINT32      commit;         /* committed user pages (resident or backed on first touch) */

    UmThread   *threads[UM_MAX_THREADS];
    int         live_threads;
    UINT64      thread_start;   /* ntdll!RtlUserThreadStart */
    UINT64      exc_dispatcher; /* ntdll!KiUserExceptionDispatcher */
    UINT32      stack_reserve;  /* from the image header */

    void       *gui;            /* per-process window-system state (um_gui.c) */
    struct GdiCursorShape *cursor;  /* its SetCursor pointer (NULL: the arrow; um_gui.c) */
    char      (*stub_names)[64]; /* "dll!function" per stub at UM_STUBS_VA */
    int         nstubs;

    volatile bool   exited;     /* every thread has stopped for good */
    volatile bool   kill_pending;
    UINT32          kill_status;
    UINT32          exit_status;
    char            why[96];    /* crash/kill description */
    char            crash_report[UM_CRASH_PATH];   /* where its crash report goes (um_crash.c), or "" */
    bool            released;   /* spawner is done with it */
    UmObject       *exit_ob;    /* UO_PROCESS object of a program-created process (not referenced) */
    UmObject       *token;      /* its primary token (referenced) */
    bool            reclaimed;  /* memory and handles freed */
    /* Priority (um_thread.c): PROCESS_PRIORITY_CLASS_* (1 IDLE, 2 NORMAL,
     * 3 HIGH, 4 REALTIME, 5 BELOW_NORMAL, 6 ABOVE_NORMAL; 0 is NORMAL),
     * and SetProcessPriorityBoost's flag, which new threads inherit */
    UINT8           prio_class;
    bool            no_boost;
    bool            auto_align; /* ProcessEnableAlignmentFaultFixup, SEM_NOALIGNMENTFAULTEXCEPT (um_gpfault.c) */
};

/* um.c */
UmProcess *UmCurrent(void);                    /* NULL for kernel threads */
void       um_set_layout(UmProcess *p, bool wow);   /* 64-bit or 32-bit (WoW) address layout */
void       um_wow_path(UmProcess *p, char *path);  /* System32 -> SysWOW64 for 32-bit programs */
UINT16     um_pe_machine(const RamNode *f);     /* 0x8664, 0x014C, or 0 if not a PE file */
UINT16     um_pe_subsystem(RamNode *f);   /* IMAGE_SUBSYSTEM_* (2 GUI, 3 console), 0 if not an image */
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
UINT64     um_find_free_aligned(UmProcess *p, UINT64 size, UINT64 lo, UINT64 hi, UINT64 align);
/* MEM_EXTENDED_PARAMETERs (user @ptr, @n of them): the address range and
 * alignment a MemExtendedParameterAddressRequirements asks for (left as
 * they are if none).  False if unreadable. */
bool       um_addr_requirements(UINT64 ptr, UINT32 n, UINT64 *lo, UINT64 *hi, UINT64 *align);
UmRegion  *um_region_add(UmProcess *p, UINT64 base, UINT64 size, UINT32 protect, bool image);
UmRegion  *um_region_find(UmProcess *p, UINT64 va);
void       um_region_remove(UmProcess *p, UmRegion *r);
bool       um_commit(UmProcess *p, UINT64 va, UINT64 size, UINT32 protect);
bool       um_map_image_page(UmProcess *p, UINT64 va, const void *content, UINT32 protect);
bool       um_is_guard(UmProcess *p, UINT64 va);            /* a PAGE_GUARD page */
void       um_decommit(UmProcess *p, UINT64 va, UINT64 size);
bool       um_is_committed(UmProcess *p, UINT64 va);
UINT32     um_page_protect(UmProcess *p, UINT64 va);       /* PAGE_* now, 0 if not committed */
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
/* Priorities (um_thread.c): @p's class base priority (4, 6, 8, 10, 13 or
 * 24), and a thread's base for an increment (UmThread.prio_incr) in it */
UINT8      um_class_base(const UmProcess *p);
UINT8      um_thread_base(const UmProcess *p, int incr);
/* Create a thread in @p starting at @start(@arg) via ntdll.  NULL on failure. */
UmThread  *um_create_thread(UmProcess *p, UINT64 start, UINT64 arg, UINT64 stack_size,
                            bool suspended, UINT32 *status);
/* Load a DLL (and what it imports) into the running process: *base gets
 * its address; new modules are appended to the loader info page. */
UINT32     um_load_dll(UmProcess *p, const char *name, UINT64 *base, UINT32 flags);
UINT32     um_dll_directory(UmProcess *p, UINT32 op, const char *path, UINT64 *cookie);
const UmModule *um_module_at(UmProcess *p, UINT64 va);
/* um_gpfault.c: a #GP at @rip in the current program, looked into as
 * Windows does: 1 if it was fixed up (run the instruction again), else 0
 * with *@code / *@nparams changed when the cause has a status of its own */
int UmGpFault(UINT64 rip, UINT32 *code, UINT64 *nparams);

/* um_console.c */
UmConsole *um_console_ref(UmConsole *c);
int        um_console_write(UmConsole *c, const char *data, int len);   /* program output */
void       um_console_flush_log(UmProcess *p);                          /* a detached process's unfinished line */
/* Program reads keyboard input: bytes, 0 at EOF, -1 if killed while waiting */
int        um_console_read(UmConsole *c, char *buf, int cap, UmProcess *p);
/* The console's waitable object (referenced), for input handles */
UmObject  *um_console_object(UmConsole *c);
/* Input records: copy up to @max (taking them with @remove; @wait for
 * one); -1 if the thread is being ended while waiting */
int        um_console_records(UmConsole *c, UmConInput *out, int max, bool remove, bool wait);
int        um_console_count(UmConsole *c);
void       um_console_flush(UmConsole *c);
void       um_console_set_mode(UmConsole *c, bool input, UINT32 mode);
void       um_console_size(UmConsole *c, int *cols, int *rows);
int        um_console_pids(UmConsole *c, UINT32 *out, int max);   /* running processes on @c (count; up to @max ids) */

/* um_syscall.c */
void       um_syscall_init(void);
void       um_close_all_handles(UmProcess *p);
void       um_install(UINT32 num, SYSCALL_HANDLER h);
SYSCALL_HANDLER um_service(UINT32 num);          /* a program's service (installed: not NULL) */
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
UINT64     um_boot_time_100ns(void);            /* the system time NovaOS started at */
UINT64     um_handle_new_object(UmProcess *p, UmObject *o);   /* takes a reference; 0 if full */
UmObject  *um_handle_object(UmProcess *p, UINT64 h, UmObType type);   /* referenced; NULL if bad */
/* The process a handle names (-1: @self); @ob holds a reference to drop
 * (um_ob_unref) when it is another process.  NULL if bad or gone. */
UmProcess *um_proc_of(UmProcess *self, UINT64 h, UmObject **ob);
bool       um_handle_object_exists(UmProcess *p, UINT64 h);           /* any open handle */
UmObject  *um_open_process(UINT32 pid);          /* OpenProcess: referenced, or NULL */
UmObject  *um_open_thread(UINT32 tid);           /* OpenThread: referenced, or NULL */
void       um_pipe_end_name(UmObject *o, char *buf, int cap); /* um_pipe.c: a pipe end's pipe name */
void       um_object_name(UmObject *o, char *buf, int cap);   /* um_thread.c: a named object's name */
UINT64     um_close_handle(UINT64 h);           /* NtClose for the current process */
void       um_log_stack(UmProcess *p, UINT64 sp);   /* return addresses from @sp to the serial log */
void       um_crash_report(UmProcess *p, const char *what, UINT32 status, UINT64 rip, UINT64 addr, UINT64 sp);   /* um_crash.c */
/* How a new process starts: standard handles (kind H_FREE = the console),
 * handles it inherits (at the same values; NULL: none) and its
 * environment (UTF-8 "NAME=value" strings, then an empty one; NULL: the
 * default).  File nodes must be referenced by the caller's locks. */
typedef struct {
    const UmHandle *std;                /* [3], or NULL */
    UINT64          std_value[3];       /* nonzero: that inherited handle is the standard one */
    const UmHandle *inherit;            /* [UM_MAX_HANDLES], or NULL */
    const char     *env;
    UINT32          env_len;
    const UINT8    *runtime;            /* STARTUPINFO's lpReserved2 bytes (the C runtime's), or NULL */
    UINT32          runtime_len;
    bool            suspended;          /* CREATE_SUSPENDED: the first thread waits for NtResumeThread */
} UmSpawnOpts;
UmProcess *um_spawn_ex(RamNode *exe, const char *cmdline, RamNode *cwd, UmConsole *con,
                       const UmSpawnOpts *o, char *err, int err_cap);
/* um_spawn_ex in two steps: map the images (with @yield, the desktop lock
 * is let go meanwhile; see um.c), then finish under the caller's locks */
UmProcess *um_spawn_image(RamNode *exe, RamNode *cwd, UmConsole *con, bool yield, char *err, int err_cap);
UmProcess *um_spawn_finish(UmProcess *p, RamNode *exe, const char *cmdline, const UmSpawnOpts *o,
                           char *err, int err_cap);
/* A path from OBJECT_ATTRIBUTES (UTF-8, NT prefix removed); *attrs gets
 * its Attributes (OBJ_INHERIT...) */
UINT32     um_get_path(UmProcess *p, UINT64 oa_ptr, char *out, int cap, UINT32 *attrs);

/* um_pipe.c: named and anonymous pipes */
const char *um_pipe_name(const char *path);   /* "\Device\NamedPipe\X" -> "X", else NULL */
UINT32     um_pipe_create(const char *path, UINT32 access, UINT32 disposition, UINT32 options,
                          UINT32 type, UINT32 read_mode, UINT32 completion, UINT32 max_inst,
                          UINT32 in_quota, UINT32 out_quota, UmObject **out, bool *rd, bool *wr);
UINT32     um_pipe_open(const char *path, UINT32 access, UINT32 options, UmObject **out, bool *rd, bool *wr);
bool       um_pipe_anonymous(UmObject **rd_end, UmObject **wr_end);
UINT32     um_pipe_read(UmObject *o, UINT64 event, UINT64 iosb, UINT64 buf, UINT32 len, UINT64 *info);
UINT32     um_pipe_write(UmObject *o, UINT64 event, UINT64 iosb, UINT64 buf, UINT32 len, UINT64 *info);
UINT32     um_pipe_fsctl(UmObject *o, UINT64 event, UINT64 iosb, UINT32 code,
                         UINT64 in, UINT32 in_len, UINT64 out, UINT32 out_len, UINT64 *info);
UINT32     um_pipe_query(UmObject *o, UINT32 cls, UINT8 *buf, UINT32 cap, UINT32 *len);
UINT32     um_pipe_set_mode(UmObject *o, UINT32 read_mode, UINT32 completion);
UINT32     um_pipe_cancel(UmObject *o, UINT64 iosb);
bool       um_pipe_is_async(UmObject *o);
UINT32     um_pipe_client_pid(UmObject *o);
void       um_pipe_process_gone(UmProcess *p);

/* um_thread.c: threads, synchronization objects, waits */
void       um_thread_syscalls_init(void);
void       um_socket_syscalls_init(void);
void       um_audio_syscalls_init(void);
void       um_gpu_syscalls_init(void);
UINT64     um_section_foreign(UmProcess *p, UINT64 pa, UINT64 size, void (*release)(void *), void *ctx);
UINT64     um_section_frames(UmProcess *p, const PADDR *frames, UINT64 n, UINT64 size,
                             void (*release)(void *), void *ctx);
void       um_gui_syscalls_init(void);
void       um_gui_process_gone(UmProcess *p);   /* destroy the process's windows */
/* Wait until @o is signaled (acquiring it), @timeout_100ns passes (-1:
 * forever) or the process is being killed. */
UINT32     um_wait_one(UmObject *o, INT64 timeout_100ns);
void       um_abandon_mutants(UmProcess *p, UmThread *t);

/* um_security.c: tokens, security descriptors, access checks */
void       um_security_syscalls_init(void);
void       um_services_init(void);              /* um_services.c */
UmObject  *um_token_for_process(UmProcess *creator);   /* a new process's primary token (referenced) */
bool       um_token_elevated(UmObject *token);         /* the elevated (full administrator) token? */
bool       um_elevate_process(UmProcess *p);           /* give @p the elevated token */
UINT32     um_set_process_token(UmProcess *p, UINT64 buf, UINT64 len);   /* ProcessAccessToken */
bool       um_pe_wants_admin(RamNode *f);              /* its manifest asks to run as administrator */
void       um_thread_drop_token(UmThread *t);          /* stop impersonating (the thread ended) */
UINT32     um_set_thread_token(UmThread *t, UINT64 buf, UINT32 len);   /* ThreadImpersonationToken */
bool       um_privilege_held(UINT32 luid);    /* the caller's token holds privilege @luid */
UINT32     um_check_object(UmObject *o, UINT32 want);  /* opening @o for @want: STATUS_SUCCESS or ACCESS_DENIED */
UINT32     um_oa_security(UINT64 oa, void **sd);       /* OBJECT_ATTRIBUTES' descriptor, captured (NULL: none) */
void       um_sd_free(void *sd);
/* The file system's checks: @want (mapped by @map) against self-relative
 * descriptor @sd (NULL: none, open to all) as the calling thread */
UINT32     um_access_check_sd(const UINT8 *sd, UINT32 len, UINT32 want, const UINT32 map[4], UINT32 *granted);
/* NtQuery/SetSecurityObject for H_FILE/H_DIR handles (um_syscall.c, on kernel/fs/fsec.c) */
UINT64     um_file_query_security(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4);   /* (um_syscall.c) */
UINT64     um_file_set_security(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4);
int        um_handle_kind(UmProcess *p, UINT64 h);        /* its UmHandleKind; -1: no such handle */
void       um_handle_set_inherit(UmProcess *p, UINT64 h, bool inherit);

/* um_exception.c: SEH delivery */
void       um_exception_syscalls_init(void);
/* um_registry.c */
void       um_registry_init(void);
void       um_registry_syscalls_init(void);
void       um_registry_poll(void);   /* save the hive after changes (desktop thread) */
void       um_registry_flush(void);  /* save the hive now if it changed */
void       um_registry_pending_renames(void);   /* MoveFileEx(DELAY_UNTIL_REBOOT) operations, at boot */
void       um_registry_process_gone(UmProcess *p);   /* drop the process's change watches */
void       um_registry_environment(void (*cb)(void *ctx, const char *name, const char *value, bool user, bool expand),
                                   void *ctx);
