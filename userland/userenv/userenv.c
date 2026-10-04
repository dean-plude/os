/* userenv.dll — the user's profile directory and environment block */
#include <windows.h>

#define UEAPI __declspec(dllexport)

static BOOL put(const WCHAR *s, int n, LPWSTR out, LPDWORD cap)
{
    if (!out || *cap <= (DWORD)n) { *cap = (DWORD)n + 1; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    for (int i = 0; i <= n; i++) out[i] = s[i];
    *cap = (DWORD)n + 1;
    return TRUE;
}

static int env_or(const char *var, const char *dflt, WCHAR *w)
{
    char a[MAX_PATH];
    DWORD n = GetEnvironmentVariableA(var, a, MAX_PATH);
    const char *s = n && n < MAX_PATH ? a : dflt;
    int k = MultiByteToWideChar(CP_UTF8, 0, s, -1, w, MAX_PATH);
    return k > 0 ? k - 1 : 0;
}

UEAPI BOOL WINAPI GetUserProfileDirectoryW(HANDLE token, LPWSTR dir, LPDWORD n)
{
    (void)token;
    WCHAR w[MAX_PATH];
    int k = env_or("USERPROFILE", "C:\\", w);
    return put(w, k, dir, n);
}

UEAPI BOOL WINAPI GetUserProfileDirectoryA(HANDLE token, LPSTR dir, LPDWORD n)
{
    (void)token;
    char a[MAX_PATH];
    DWORD k = GetEnvironmentVariableA("USERPROFILE", a, MAX_PATH);
    if (!k || k >= MAX_PATH) { a[0] = 'C'; a[1] = ':'; a[2] = '\\'; a[3] = 0; k = 3; }
    if (!dir || *n <= k) { *n = k + 1; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    for (DWORD i = 0; i <= k; i++) dir[i] = a[i];
    *n = k + 1;
    return TRUE;
}

UEAPI BOOL WINAPI GetProfilesDirectoryW(LPWSTR dir, LPDWORD n) { static const WCHAR root[] = { 'C', ':', '\\', 0 }; return put(root, 3, dir, n); }
UEAPI BOOL WINAPI GetDefaultUserProfileDirectoryW(LPWSTR dir, LPDWORD n) { WCHAR w[MAX_PATH]; return put(w, env_or("USERPROFILE", "C:\\", w), dir, n); }
UEAPI BOOL WINAPI GetAllUsersProfileDirectoryW(LPWSTR dir, LPDWORD n) { WCHAR w[MAX_PATH]; return put(w, env_or("ALLUSERSPROFILE", "C:\\ProgramData", w), dir, n); }

/* A copy of this process's environment block (the same variables for everyone) */
UEAPI BOOL WINAPI CreateEnvironmentBlock(LPVOID *env, HANDLE token, BOOL inherit)
{
    (void)token; (void)inherit;
    LPWSTR src = GetEnvironmentStringsW();
    SIZE_T n = 0;
    while (src[n] || src[n + 1]) n++;
    n += 2;
    WCHAR *copy = LocalAlloc(0, 2 * n);
    if (!copy) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    for (SIZE_T i = 0; i < n; i++) copy[i] = src[i];
    *env = copy;
    return TRUE;
}

UEAPI BOOL WINAPI DestroyEnvironmentBlock(LPVOID env) { LocalFree(env); return TRUE; }

UEAPI BOOL WINAPI ExpandEnvironmentStringsForUserW(HANDLE token, LPCWSTR s, LPWSTR out, DWORD n)
{
    (void)token;
    DWORD r = ExpandEnvironmentStringsW(s, out, n);
    return r && r <= n;
}

UEAPI BOOL WINAPI LoadUserProfileW(HANDLE token, LPVOID info) { (void)token; (void)info; return TRUE; }
UEAPI BOOL WINAPI UnloadUserProfile(HANDLE token, HANDLE profile) { (void)token; (void)profile; return TRUE; }

/* AppContainers: a container's SID is S-1-15-2- and seven numbers hashed
 * from its name (SHA-256 on Windows; here a simple hash: the SID only
 * has to be stable per name, and NovaOS enforces no containers) */
UEAPI HRESULT WINAPI DeriveAppContainerSidFromAppContainerName(LPCWSTR name, PSID *sid)
{
    if (!name || !sid) return E_INVALIDARG;
    DWORD h[7] = { 0x811C9DC5u, 0x01000193u, 0x2545F491u, 0x9E3779B9u, 0x85EBCA6Bu, 0xC2B2AE35u, 0x27D4EB2Fu };
    for (const WCHAR *c = name; *c; c++) {
        WCHAR ch = *c >= 'A' && *c <= 'Z' ? (WCHAR)(*c + 32) : *c;
        for (int i = 0; i < 7; i++) h[i] = (h[i] ^ ch) * 0x01000193u + (DWORD)i;
    }
    SID_IDENTIFIER_AUTHORITY app = { { 0, 0, 0, 0, 0, 15 } };
    return AllocateAndInitializeSid(&app, 8, 2, h[0], h[1], h[2], h[3], h[4], h[5], h[6], sid) ? S_OK : E_OUTOFMEMORY;
}

/* Group Policy: NovaOS applies none, so nothing ever holds the policy
 * section for writing; the "section" handed out is a handle that only has
 * to close */
UEAPI HANDLE WINAPI EnterCriticalPolicySection(BOOL machine)
{
    (void)machine;
    HANDLE h = CreateEventW(0, TRUE, TRUE, 0);
    return h ? h : 0;
}
UEAPI BOOL WINAPI LeaveCriticalPolicySection(HANDLE section)
{
    if (!section) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    return CloseHandle(section);
}

/* every NovaOS profile is a local one: 0 is a local profile (no PT_TEMPORARY,
 * PT_ROAMING or PT_MANDATORY bit) */
UEAPI BOOL WINAPI GetProfileType(DWORD *flags)
{
    if (!flags) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    *flags = 0;
    return TRUE;
}
