/*
 * csrss.c — Client/Server Runtime SubSystem bootstrap (Phase 6)
 *
 * Provides a minimal kernel-side CSRSS shim so that user-mode processes can
 * complete ntdll!LdrpInitializeProcess without crashing on CSRSS port lookups.
 *
 * Phase 6 scope:
 *   - CsrInitialize: create a CSRSS kernel thread (logs registrations)
 *   - CsrRegisterProcess: record process in a small table
 *   - CsrClientCallServer: no-op stub; returns STATUS_SUCCESS
 *
 * Phase 8 will replace this with proper LPC port objects and a real CSRSS.
 */

#include "csrss.h"
#include "ps.h"
#include "../ke/printf.h"
#include "../ke/scheduler.h"
#include "../mm/vmm.h"
#include "../include/types.h"

/* -----------------------------------------------------------------------
 * Process registry — tracks up to 64 registered processes
 * ----------------------------------------------------------------------- */
#define CSRSS_MAX_PROCS  64

typedef struct {
    UINT64    Pid;
    UINT64    Tid;         /* initial thread */
    bool      Valid;
} CSRSS_PROC_ENTRY;

static CSRSS_PROC_ENTRY g_csr_procs[CSRSS_MAX_PROCS];
static volatile UINT32  g_csr_lock_next, g_csr_lock_owner;

static void csr_lock(void)
{
    UINT32 t = __atomic_fetch_add(&g_csr_lock_next, 1, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&g_csr_lock_owner, __ATOMIC_ACQUIRE) != t)
        __asm__ volatile("pause");
}

static void csr_unlock(void)
{
    __atomic_fetch_add(&g_csr_lock_owner, 1, __ATOMIC_RELEASE);
}

/* -----------------------------------------------------------------------
 * CSRSS server thread — minimal event loop
 * ----------------------------------------------------------------------- */
static void csrss_server_thread(void *arg)
{
    (void)arg;
    kprintf("[CSRSS] Server thread started (PID=server)\n");
    for (;;) {
        /* Phase 6: nothing to do — real work happens in CsrRegisterProcess */
        sched_yield();
    }
}

/* -----------------------------------------------------------------------
 * CsrInitialize
 * ----------------------------------------------------------------------- */
void CsrInitialize(void)
{
    __builtin_memset(g_csr_procs, 0, sizeof(g_csr_procs));
    g_csr_lock_next = g_csr_lock_owner = 0;

    /* Create the CSRSS server kernel thread */
    sched_create_thread("csrss", csrss_server_thread, NULL, 4 /* low priority */);

    kprintf("[CSRSS] Initialized (Phase 6 stub: LPC ports deferred to Phase 8)\n");
}

/* -----------------------------------------------------------------------
 * CsrRegisterProcess
 * ----------------------------------------------------------------------- */
NTSTATUS CsrRegisterProcess(PEPROCESS Process, PETHREAD Thread)
{
    if (!Process) return STATUS_INVALID_PARAMETER;

    csr_lock();
    for (int i = 0; i < CSRSS_MAX_PROCS; i++) {
        if (!g_csr_procs[i].Valid) {
            g_csr_procs[i].Pid   = Process->UniqueProcessId;
            g_csr_procs[i].Tid   = Thread ? Thread->UniqueThread : 0;
            g_csr_procs[i].Valid = true;
            csr_unlock();
            kprintf("[CSRSS] Registered PID=%lu TID=%lu ('%s')\n",
                    Process->UniqueProcessId,
                    Thread ? Thread->UniqueThread : 0ULL,
                    Process->ImageFileName);
            return STATUS_SUCCESS;
        }
    }
    csr_unlock();
    kprintf("[CSRSS] WARNING: process table full (max %d)\n", CSRSS_MAX_PROCS);
    return STATUS_UNSUCCESSFUL;
}

/* -----------------------------------------------------------------------
 * CsrClientCallServer — stub
 * ----------------------------------------------------------------------- */
NTSTATUS CsrClientCallServer(PCSR_API_MSG ApiMessage,
                              void        *CaptureBuffer,
                              UINT32       ApiNumber,
                              UINT32       ArgLength)
{
    (void)CaptureBuffer; (void)ArgLength;

    if (ApiMessage) ApiMessage->ReturnValue = STATUS_SUCCESS;

    /* Log high-level calls in debug builds */
    kprintf("[CSRSS] CsrClientCallServer API=0x%x (stub)\n",
            ApiNumber);

    return STATUS_SUCCESS;
}
