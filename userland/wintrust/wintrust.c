/*
 * wintrust.dll — Authenticode.  NovaOS has no code-signing trust store, so
 * no file has a signature it can verify: WinVerifyTrust reports
 * TRUST_E_NOSIGNATURE, the answer for an unsigned file.
 */
#include <windows.h>

#define WTAPI __declspec(dllexport)
#define TRUST_E_NOSIGNATURE_ ((LONG)0x800B0100L)

WTAPI LONG WINAPI WinVerifyTrust(HWND w, GUID *action, LPVOID data) { (void)w; (void)action; (void)data; return TRUST_E_NOSIGNATURE_; }
WTAPI HRESULT WINAPI WinVerifyTrustEx(HWND w, GUID *action, LPVOID data) { (void)w; (void)action; (void)data; return TRUST_E_NOSIGNATURE_; }
WTAPI BOOL WINAPI CryptCATAdminAcquireContext(HANDLE *h, const GUID *sub, DWORD flags)
{ (void)sub; (void)flags; if (h) *h = 0; SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
WTAPI BOOL WINAPI CryptCATAdminAcquireContext2(HANDLE *h, const GUID *sub, LPCWSTR alg, const void *pol, DWORD flags)
{ (void)alg; (void)pol; return CryptCATAdminAcquireContext(h, sub, flags); }
WTAPI BOOL WINAPI CryptCATAdminReleaseContext(HANDLE h, DWORD flags) { (void)h; (void)flags; return TRUE; }
WTAPI BOOL WINAPI CryptCATAdminCalcHashFromFileHandle(HANDLE f, DWORD *n, BYTE *hash, DWORD flags)
{ (void)f; (void)n; (void)hash; (void)flags; SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
WTAPI HANDLE WINAPI CryptCATAdminEnumCatalogFromHash(HANDLE h, BYTE *hash, DWORD n, DWORD flags, HANDLE *prev)
{ (void)h; (void)hash; (void)n; (void)flags; (void)prev; SetLastError(ERROR_NOT_FOUND); return 0; }
WTAPI BOOL WINAPI CryptCATAdminReleaseCatalogContext(HANDLE h, HANDLE c, DWORD flags) { (void)h; (void)c; (void)flags; return TRUE; }
WTAPI PVOID WINAPI WTHelperProvDataFromStateData(HANDLE h) { (void)h; return 0; }
