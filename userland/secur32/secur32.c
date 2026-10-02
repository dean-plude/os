/*
 * secur32.dll — logon sessions (Lsa*Logon*) and name translation.
 *
 * NovaOS has one user, already logged on: no process can log on as
 * someone else, so registering a logon process is refused, and there is
 * no directory service to translate names with.
 */
#include <windows.h>

#define SECUR32 __declspec(dllexport)
typedef LONG NTSTATUS;
#define STATUS_ACCESS_DENIED_   ((NTSTATUS)0xC0000022L)
#define STATUS_NO_SUCH_PACKAGE_ ((NTSTATUS)0xC00000FEL)
#define STATUS_NOT_SUPPORTED_   ((NTSTATUS)0xC00000BBL)
#define ERROR_NO_SUCH_DOMAIN_   1355

SECUR32 NTSTATUS WINAPI LsaConnectUntrusted(PHANDLE h) { *h = (HANDLE)(ULONG_PTR)0x4C534131; return 0; }
SECUR32 NTSTATUS WINAPI LsaRegisterLogonProcess(PVOID name, PHANDLE h, PVOID mode) { (void)name; (void)mode; *h = 0; return STATUS_ACCESS_DENIED_; }
SECUR32 NTSTATUS WINAPI LsaDeregisterLogonProcess(HANDLE h) { (void)h; return 0; }
SECUR32 NTSTATUS WINAPI LsaLookupAuthenticationPackage(HANDLE h, PVOID name, PULONG pkg) { (void)h; (void)name; *pkg = 0; return STATUS_NO_SUCH_PACKAGE_; }
SECUR32 NTSTATUS WINAPI LsaLogonUser(HANDLE h, PVOID origin, int type, ULONG pkg, PVOID info, ULONG len, PVOID groups,
                                     PVOID source, PVOID *profile, PULONG plen, PLUID id, PHANDLE token, PVOID quotas,
                                     NTSTATUS *sub)
{
    (void)h; (void)origin; (void)type; (void)pkg; (void)info; (void)len; (void)groups; (void)source; (void)id; (void)quotas;
    if (profile) *profile = 0;
    if (plen) *plen = 0;
    if (token) *token = 0;
    if (sub) *sub = 0;
    return STATUS_NOT_SUPPORTED_;
}
SECUR32 NTSTATUS WINAPI LsaFreeReturnBuffer(PVOID p) { if (p) HeapFree(GetProcessHeap(), 0, p); return 0; }
SECUR32 BOOLEAN WINAPI TranslateNameW(LPCWSTR name, int from, int to, LPWSTR out, PULONG n)
{ (void)name; (void)from; (void)to; (void)out; (void)n; SetLastError(ERROR_NO_SUCH_DOMAIN_); return FALSE; }
SECUR32 BOOLEAN WINAPI GetUserNameExW(int fmt, LPWSTR out, PULONG n)
{
    WCHAR me[128];
    DWORD k = 128;
    if (fmt != 2 /* NameSamCompatible */ && fmt != 3 /* NameDisplay */) { SetLastError(1332 /* ERROR_NONE_MAPPED */); return FALSE; }
    if (!GetUserNameW(me, &k)) return FALSE;
    WCHAR full[160];
    int o = 0;
    if (fmt == 2) { const WCHAR *d = L"NOVAOS\\"; while (*d) full[o++] = *d++; }
    for (DWORD i = 0; me[i]; i++) full[o++] = me[i];
    full[o] = 0;
    if (*n <= (ULONG)o) { *n = (ULONG)o + 1; SetLastError(ERROR_MORE_DATA); return FALSE; }
    for (int i = 0; i <= o; i++) out[i] = full[i];
    *n = (ULONG)o;
    return TRUE;
}

/* SSPI's dispatch table: no security packages (NTLM, Kerberos,
 * Negotiate) are installed, so there is none to hand out */
SECUR32 PVOID WINAPI InitSecurityInterfaceW(void) { return 0; }
SECUR32 PVOID WINAPI InitSecurityInterfaceA(void) { return 0; }
