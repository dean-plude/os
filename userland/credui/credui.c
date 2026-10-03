/*
 * credui.dll — the Windows credential prompts.  NovaOS has one local user
 * with no password and no credential dialog yet, so every prompt reports
 * that the user cancelled it; callers treat that as "not confirmed".
 */
#include <windows.h>

#define CREDUIAPI __declspec(dllexport)
#ifndef ERROR_CANCELLED
#define ERROR_CANCELLED 1223
#endif

CREDUIAPI DWORD WINAPI CredUIPromptForWindowsCredentialsW(void *info, DWORD err, ULONG *package, LPCVOID in, ULONG nin,
                                                          LPVOID *out, ULONG *nout, BOOL *save, DWORD flags)
{
    (void)info; (void)err; (void)package; (void)in; (void)nin; (void)flags;
    if (out) *out = NULL;
    if (nout) *nout = 0;
    if (save) *save = FALSE;
    return ERROR_CANCELLED;
}
CREDUIAPI DWORD WINAPI CredUIPromptForWindowsCredentialsA(void *info, DWORD err, ULONG *package, LPCVOID in, ULONG nin,
                                                          LPVOID *out, ULONG *nout, BOOL *save, DWORD flags)
{
    return CredUIPromptForWindowsCredentialsW(info, err, package, in, nin, out, nout, save, flags);
}
CREDUIAPI DWORD WINAPI CredUIPromptForCredentialsW(void *info, LPCWSTR target, void *ctx, DWORD err, LPWSTR user,
                                                   ULONG nuser, LPWSTR pass, ULONG npass, BOOL *save, DWORD flags)
{
    (void)info; (void)target; (void)ctx; (void)err; (void)user; (void)nuser; (void)pass; (void)npass; (void)flags;
    if (save) *save = FALSE;
    return ERROR_CANCELLED;
}
CREDUIAPI DWORD WINAPI CredUICmdLinePromptForCredentialsW(LPCWSTR target, void *ctx, DWORD err, LPWSTR user, ULONG nuser,
                                                          LPWSTR pass, ULONG npass, BOOL *save, DWORD flags)
{
    return CredUIPromptForCredentialsW(NULL, target, ctx, err, user, nuser, pass, npass, save, flags);
}
CREDUIAPI BOOL WINAPI CredUnPackAuthenticationBufferW(DWORD flags, PVOID buf, DWORD n, LPWSTR user, DWORD *nuser,
                                                      LPWSTR domain, DWORD *ndomain, LPWSTR pass, DWORD *npass)
{
    (void)flags; (void)buf; (void)n; (void)user; (void)nuser; (void)domain; (void)ndomain; (void)pass; (void)npass;
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}
CREDUIAPI BOOL WINAPI CredPackAuthenticationBufferW(DWORD flags, LPWSTR user, LPWSTR pass, PBYTE buf, DWORD *n)
{
    (void)flags; (void)user; (void)pass; (void)buf;
    if (n) *n = 0;
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}
CREDUIAPI DWORD WINAPI CredUIConfirmCredentialsW(LPCWSTR target, BOOL confirm) { (void)target; (void)confirm; return ERROR_SUCCESS; }
