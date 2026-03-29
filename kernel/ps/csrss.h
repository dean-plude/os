/*
 * csrss.h — Client/Server Runtime SubSystem bootstrap (Phase 6)
 *
 * On Windows, every Win32 process must connect to CSRSS.EXE during startup
 * to register its process/thread handles, allocate a console, and enable
 * Win32 subsystem services.  CSRSS uses LPC (Local Procedure Call) ports.
 *
 * Phase 6 provides a minimal kernel-side CSRSS shim:
 *   - CsrInitialize()         — start the CSRSS server thread
 *   - CsrRegisterProcess()    — register a new user process with CSRSS
 *   - CsrClientCallServer()   — stub for LPC port message dispatch
 *
 * Real LPC port objects are Phase 8+.  For now, CsrClientCallServer is a
 * no-op that returns STATUS_SUCCESS so that ntdll's process-init sequence
 * does not fail.
 */

#pragma once
#include "../include/types.h"
#include "../ps/ps.h"

/* -----------------------------------------------------------------------
 * CSRSS API message classes (matches Windows CSR_APINUMBER subset)
 * ----------------------------------------------------------------------- */
typedef enum _CSR_API_NUMBER {
    CsrNullApiNumber          = 0,
    BasepCreateProcess        = 0x10000,
    BasepCreateThread         = 0x10001,
    BasepExitProcess          = 0x10002,
    BasepGetTempFile          = 0x10003,
    BasepAllocConsole         = 0x10020,
    BasepFreeConsole          = 0x10021,
    BasepGetConsoleTitle      = 0x10022,
    BasepSetConsoleTitle      = 0x10023,
} CSR_API_NUMBER;

/* Minimal CSR_API_MSG (enough for ntdll startup to reference) */
typedef struct _CSR_API_MSG {
    UINT32          ApiNumber;
    UINT32          ReturnValue;
    UINT64          CaptureBuffer;  /* NULL in Phase 6 */
    UINT8           Data[64];       /* per-API payload (stub ignores) */
} CSR_API_MSG, *PCSR_API_MSG;

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

/*
 * Initialize the CSRSS subsystem.  Creates a kernel CSRSS server thread
 * that accepts process-registration events.  Called once at boot after
 * PsInitialize().
 */
void CsrInitialize(void);

/*
 * Register a newly created user-mode process with CSRSS.
 * Equivalent to the CsrCreateProcess() call inside ntdll!LdrpInitializeProcess.
 * @Process:  The EPROCESS to register.
 * @Thread:   The initial ETHREAD.
 */
NTSTATUS CsrRegisterProcess(PEPROCESS Process, PETHREAD Thread);

/*
 * Stub implementation of CsrClientCallServer.
 * User-mode processes call this via ntdll!CsrClientCallServer to communicate
 * with the CSRSS server.  Phase 6: always returns STATUS_SUCCESS.
 */
NTSTATUS CsrClientCallServer(PCSR_API_MSG ApiMessage,
                              void        *CaptureBuffer,
                              UINT32       ApiNumber,
                              UINT32       ArgLength);
