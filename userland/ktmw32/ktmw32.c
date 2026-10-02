/*
 * ktmw32.dll — the Kernel Transaction Manager.  NovaOS has no transacted
 * file system or registry: the *Transacted calls make their change at
 * once, so a transaction is a handle that commits trivially and cannot
 * be rolled back.
 */
#include <windows.h>

#define KTM __declspec(dllexport)

KTM HANDLE WINAPI CreateTransaction(LPSECURITY_ATTRIBUTES sa, LPGUID uow, DWORD options, DWORD isolation,
                                    DWORD isolation_flags, DWORD timeout, LPWSTR description)
{
    (void)uow; (void)options; (void)isolation; (void)isolation_flags; (void)timeout; (void)description;
    return CreateEventW(sa, TRUE, FALSE, 0);
}
KTM BOOL WINAPI CommitTransaction(HANDLE t) { if (!t || t == INVALID_HANDLE_VALUE) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; } return TRUE; }
KTM BOOL WINAPI CommitTransactionAsync(HANDLE t) { return CommitTransaction(t); }
KTM BOOL WINAPI RollbackTransaction(HANDLE t)
{
    (void)t;
    SetLastError(ERROR_NOT_SUPPORTED);              /* the changes were already made */
    return FALSE;
}
KTM BOOL WINAPI RollbackTransactionAsync(HANDLE t) { return RollbackTransaction(t); }
KTM BOOL WINAPI GetTransactionInformation(HANDLE t, LPDWORD outcome, LPDWORD isolation, LPDWORD isolation_flags,
                                          LPDWORD timeout, DWORD n, LPWSTR description)
{
    (void)t;
    if (outcome) *outcome = 1;                      /* TransactionOutcomeUndetermined */
    if (isolation) *isolation = 0;
    if (isolation_flags) *isolation_flags = 0;
    if (timeout) *timeout = 0;
    if (description && n) description[0] = 0;
    return TRUE;
}
