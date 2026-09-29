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
