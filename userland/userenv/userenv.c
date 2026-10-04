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

/* AppContainer profiles, kept where Windows keeps them: the mapping from
 * the container's SID to its name under HKCU\...\AppContainer\Mappings
 * and its folder %LOCALAPPDATA%\Packages\NAME.  NovaOS runs no process
 * inside a container, so a profile is only this record. */
#define AC_MAPPINGS L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppContainer\\Mappings\\"

static int ac_key(PSID sid, WCHAR *key, int cap)
{
    LPWSTR s = 0;
    if (!ConvertSidToStringSidW(sid, &s)) return 0;
    int n = lstrlenW(AC_MAPPINGS), k = lstrlenW(s);
    if (n + k >= cap) { LocalFree(s); return 0; }
    lstrcpyW(key, AC_MAPPINGS);
    lstrcpyW(key + n, s);
    LocalFree(s);
    return 1;
}

static int ac_folder(LPCWSTR name, WCHAR *dir, int cap, int create)
{
    WCHAR base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (!n || n >= MAX_PATH || n + 10 + lstrlenW(name) >= (DWORD)cap) return 0;
    lstrcpyW(dir, base);
    lstrcatW(dir, L"\\Packages");
    if (create) CreateDirectoryW(dir, 0);
    lstrcatW(dir, L"\\");
    lstrcatW(dir, name);
    return 1;
}

static int ac_name_ok(LPCWSTR name)
{
    int n = name ? lstrlenW(name) : 0;
    if (n < 2 || n > 64) return 0;
    for (int i = 0; i < n; i++)
        if (!((name[i] >= 'a' && name[i] <= 'z') || (name[i] >= 'A' && name[i] <= 'Z') ||
              (name[i] >= '0' && name[i] <= '9') || name[i] == '.' || name[i] == '-' || name[i] == '_'))
            return 0;
    return 1;
}

UEAPI HRESULT WINAPI CreateAppContainerProfile(LPCWSTR name, LPCWSTR display, LPCWSTR description, PVOID caps,
                                               DWORD ncaps, PSID *sid)
{
    (void)caps; (void)ncaps;
    if (!sid) return E_INVALIDARG;
    *sid = 0;
    if (!ac_name_ok(name) || !display || !description) return E_INVALIDARG;
    PSID s;
    HRESULT hr = DeriveAppContainerSidFromAppContainerName(name, &s);
    if (FAILED(hr)) return hr;
    WCHAR key[512], dir[MAX_PATH];
    HKEY k;
    DWORD how = 0;
    if (!ac_key(s, key, 512) || RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, 0, 0, KEY_ALL_ACCESS, 0, &k, &how)) {
        FreeSid(s);
        return E_FAIL;
    }
    if (how == REG_OPENED_EXISTING_KEY) {
        RegCloseKey(k);
        FreeSid(s);
        return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    }
    RegSetValueExW(k, L"Moniker", 0, REG_SZ, (const BYTE *)name, (DWORD)(lstrlenW(name) + 1) * 2);
    RegSetValueExW(k, L"DisplayName", 0, REG_SZ, (const BYTE *)display, (DWORD)(lstrlenW(display) + 1) * 2);
    RegSetValueExW(k, L"Description", 0, REG_SZ, (const BYTE *)description, (DWORD)(lstrlenW(description) + 1) * 2);
    RegCloseKey(k);
    if (ac_folder(name, dir, MAX_PATH, 1)) {
        CreateDirectoryW(dir, 0);
        lstrcatW(dir, L"\\AC");
        CreateDirectoryW(dir, 0);
    }
    *sid = s;
    return S_OK;
}

UEAPI HRESULT WINAPI DeleteAppContainerProfile(LPCWSTR name)
{
    if (!ac_name_ok(name)) return E_INVALIDARG;
    PSID s;
    HRESULT hr = DeriveAppContainerSidFromAppContainerName(name, &s);
    if (FAILED(hr)) return hr;
    WCHAR key[512], dir[MAX_PATH];
    int ok = ac_key(s, key, 512);
    FreeSid(s);
    if (ok) RegDeleteKeyW(HKEY_CURRENT_USER, key);          /* (gone already is not an error) */
    if (ac_folder(name, dir, MAX_PATH, 0)) {
        int n = lstrlenW(dir);
        lstrcatW(dir, L"\\AC");
        RemoveDirectoryW(dir);
        dir[n] = 0;
        RemoveDirectoryW(dir);
    }
    return S_OK;
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

/* Group Policy change events: NovaOS applies no policy, so a registered
 * event is never signalled; registering and unregistering only check it */
UEAPI BOOL WINAPI RegisterGPNotification(HANDLE event, BOOL machine)
{
    (void)machine;
    if (!event) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    return TRUE;
}
UEAPI BOOL WINAPI UnregisterGPNotification(HANDLE event)
{
    if (!event) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    return TRUE;
}
