/* wvstarttest.exe — what the WebView2 runtime's browser process
 * (msedgewebview2.exe, Chromium) and its loader need to start:
 *
 *   - COM's apartment published in the TEB (ReservedForOle), which
 *     Chromium reads directly: STA and MTA flags, gone after the last
 *     CoUninitialize;
 *   - TerminateProcess on the process itself ends it without DLL detach
 *     or fiber-local storage callbacks (Chromium's DllMain deliberately
 *     crashes on a detach it did not expect): a child run of this program
 *     with an FLS callback that would leave a file behind;
 *   - more than 128 FLS slots (statically linked C runtimes each take
 *     some);
 *   - ws2_32's GetAddrInfoExW, ntdll's RtlIpv4/6StringToAddressEx and
 *     LdrLockLoaderLock, crypt32's CryptFindOIDInfo, advapi32's
 *     performance counter provider API, kernel32's GetDllDirectoryW,
 *     GetPhysicallyInstalledSystemMemory and the packaged-app queries
 *     (none: not a packaged app).
 *
 * `wvstarttest child PATH` is the child run. */
#include <stdio.h>
#include <string.h>
#include <winsock2.h>
#include <windows.h>
#include <winternl.h>
#include <objbase.h>

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d, error %lu)\n", what, __LINE__, GetLastError()); } } while (0)

__declspec(dllimport) NTSTATUS NTAPI RtlIpv4StringToAddressExW(PCWSTR s, BOOLEAN strict, void *addr, USHORT *port);
__declspec(dllimport) NTSTATUS NTAPI RtlIpv6StringToAddressExW(PCWSTR s, void *addr, ULONG *scope, USHORT *port);
__declspec(dllimport) NTSTATUS NTAPI LdrLockLoaderLock(ULONG flags, ULONG *state, ULONG_PTR *cookie);
__declspec(dllimport) NTSTATUS NTAPI LdrUnlockLoaderLock(ULONG flags, ULONG_PTR cookie);
__declspec(dllimport) ULONG WINAPI PerfStartProviderEx(GUID *guid, void *context, HANDLE *out);
__declspec(dllimport) ULONG WINAPI PerfStopProvider(HANDLE h);
__declspec(dllimport) void *WINAPI PerfCreateInstance(HANDLE h, const GUID *set, PCWSTR name, ULONG id);
__declspec(dllimport) ULONG WINAPI PerfDeleteInstance(HANDLE h, void *instance);
__declspec(dllimport) ULONG WINAPI PerfSetULongCounterValue(HANDLE h, void *instance, ULONG counter, ULONG v);
__declspec(dllimport) DWORD WINAPI GetDllDirectoryW(DWORD n, WCHAR *buf);
__declspec(dllimport) BOOL WINAPI GetPhysicallyInstalledSystemMemory(ULONGLONG *kb);
__declspec(dllimport) LONG WINAPI FindPackagesByPackageFamily(PCWSTR family, UINT32 flags, UINT32 *count,
                                                              PWSTR *names, UINT32 *len, WCHAR *buf, UINT32 *props);
__declspec(dllimport) LPWSTR *WINAPI CommandLineToArgvW(LPCWSTR cmd, int *argc);
__declspec(dllimport) LONG WINAPI AppPolicyGetThreadInitializationType(HANDLE token, int *policy);

typedef struct { DWORD cbSize; LPCSTR pszOID; LPCWSTR pwszName; DWORD dwGroupId; DWORD dwValue;
                 struct { DWORD cbData; BYTE *pbData; } ExtraInfo; LPCWSTR pwszCNGAlgid; LPCWSTR pwszCNGExtraAlgid; } OIDINFO;
__declspec(dllimport) const OIDINFO *WINAPI CryptFindOIDInfo(DWORD keytype, void *key, DWORD group);

#define OLE_TLS (sizeof(void *) == 8 ? 0x1758 : 0xF80)
#define OLE_FLAGS (sizeof(void *) == 8 ? 0x14 : 0x0C)

static DWORD ole_flags(void)
{
    BYTE *tls = *(BYTE **)(NtCurrentTebBytes() + OLE_TLS);
    return tls ? *(DWORD *)(tls + OLE_FLAGS) : 0;
}

static DWORD WINAPI mta_thread(void *arg)
{
    (void)arg;
    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) return 0;
    DWORD f = ole_flags();
    CoUninitialize();
    return f;
}

static void apartment(void)
{
    CHECK("no apartment before CoInitializeEx", !(ole_flags() & 0x180));
    CHECK("CoInitializeEx STA", SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)));
    CHECK("the TEB says STA", (ole_flags() & 0x80) && !(ole_flags() & 0x100));
    CoUninitialize();
    CHECK("gone after CoUninitialize", !(ole_flags() & 0x180));
    HANDLE t = CreateThread(NULL, 0, mta_thread, NULL, 0, NULL);
    DWORD f = 0;
    WaitForSingleObject(t, INFINITE);
    GetExitCodeThread(t, &f);
    CloseHandle(t);
    CHECK("the TEB says MTA on a multithreaded thread", (f & 0x140) == 0x140 && !(f & 0x80));
}

/* the child: an FLS callback that would leave a file, then TerminateProcess */
static WCHAR g_mark[MAX_PATH];
static void WINAPI fls_callback(void *v)
{
    (void)v;
    HANDLE h = CreateFileW(g_mark, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

static int child(const WCHAR *mark)
{
    lstrcpynW(g_mark, mark, MAX_PATH);
    DWORD i = FlsAlloc(fls_callback);
    FlsSetValue(i, (void *)1);
    TerminateProcess(GetCurrentProcess(), 7);
    return 1;                                   /* (not reached) */
}

static void terminate_self(void)
{
    WCHAR exe[MAX_PATH], mark[MAX_PATH], cmd[3 * MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    GetTempPathW(MAX_PATH, mark);
    lstrcatW(mark, L"wvstarttest.fls");
    DeleteFileW(mark);
    wsprintfW(cmd, L"\"%s\" child \"%s\"", exe, mark);
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    CHECK("start the child", ok);
    if (!ok) return;
    DWORD code = 0;
    WaitForSingleObject(pi.hProcess, 60000);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CHECK("TerminateProcess(self) ends with its code", code == 7);
    CHECK("... running no FLS callback (nor DLL detach)", GetFileAttributesW(mark) == INVALID_FILE_ATTRIBUTES);
    DeleteFileW(mark);
}

static void fls_many(void)
{
    static DWORD idx[300];
    int n = 0;
    while (n < 300 && (idx[n] = FlsAlloc(NULL)) != FLS_OUT_OF_INDEXES) n++;
    CHECK("300 FLS slots", n == 300);
    int ok = 1;
    for (int i = 0; i < n; i++) ok &= FlsSetValue(idx[i], (void *)(ULONG_PTR)(i + 1));
    for (int i = 0; i < n; i++) ok &= FlsGetValue(idx[i]) == (void *)(ULONG_PTR)(i + 1);
    CHECK("each keeps its value", ok);
    for (int i = 0; i < n; i++) FlsFree(idx[i]);
}

static void names(void)
{
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    ADDRINFOEXW hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    int r = GetAddrInfoExW(L"127.0.0.1", L"80", NS_DNS, NULL, &hints, &res, NULL, NULL, NULL, NULL);
    CHECK("GetAddrInfoExW", r == 0 && res);
    if (r == 0 && res) {
        struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
        CHECK("... the address and port", sa->sin_family == AF_INET && sa->sin_addr.s_addr == htonl(0x7F000001) && sa->sin_port == htons(80));
        FreeAddrInfoExW(res);
    }

    BYTE a4[4], a6[16];
    USHORT port = 1;
    ULONG scope = 9;
    CHECK("RtlIpv4StringToAddressExW", RtlIpv4StringToAddressExW(L"10.1.2.3:8080", TRUE, a4, &port) == 0 &&
          a4[0] == 10 && a4[3] == 3 && port == htons(8080));
    CHECK("... refuses junk", RtlIpv4StringToAddressExW(L"10.1.2", TRUE, a4, &port) != 0);
    CHECK("RtlIpv6StringToAddressExW", RtlIpv6StringToAddressExW(L"[fe80::1%3]:443", a6, &scope, &port) == 0 &&
          a6[0] == 0xfe && a6[1] == 0x80 && a6[15] == 1 && scope == 3 && port == htons(443));
    CHECK("... plain ::1", RtlIpv6StringToAddressExW(L"::1", a6, &scope, &port) == 0 && a6[15] == 1 && !scope && !port);
}

static void misc(void)
{
    ULONG state = 0;
    ULONG_PTR cookie = 0;
    CHECK("LdrLockLoaderLock", LdrLockLoaderLock(0, &state, &cookie) == 0 && cookie);
    CHECK("LdrUnlockLoaderLock", LdrUnlockLoaderLock(0, cookie) == 0);
    CHECK("... try-only takes it", LdrLockLoaderLock(2, &state, &cookie) == 0 && state == 1);
    LdrUnlockLoaderLock(0, cookie);

    const OIDINFO *o = CryptFindOIDInfo(1, (void *)"1.2.840.113549.1.1.11", 0);
    CHECK("CryptFindOIDInfo by OID", o && !lstrcmpW(o->pwszName, L"sha256RSA") && !lstrcmpW(o->pwszCNGAlgid, L"SHA256"));
    CHECK("... by name", CryptFindOIDInfo(2, (void *)L"sha1", 1) != NULL);
    CHECK("... unknown", CryptFindOIDInfo(1, (void *)"1.2.3.4.5", 0) == NULL);

    GUID g = { 0x12345678, 1, 2, { 3, 4, 5, 6, 7, 8, 9, 10 } };
    HANDLE p = NULL;
    CHECK("PerfStartProviderEx", PerfStartProviderEx(&g, NULL, &p) == 0 && p);
    void *in = PerfCreateInstance(p, &g, L"wvstarttest", 1);
    CHECK("PerfCreateInstance", in != NULL);
    CHECK("PerfSetULongCounterValue", PerfSetULongCounterValue(p, in, 1, 5) == 0);
    CHECK("PerfDeleteInstance and PerfStopProvider", PerfDeleteInstance(p, in) == 0 && PerfStopProvider(p) == 0);

    WCHAR dir[MAX_PATH];
    SetDllDirectoryW(L"C:\\Windows");
    CHECK("GetDllDirectoryW", GetDllDirectoryW(MAX_PATH, dir) == 10 && !lstrcmpiW(dir, L"C:\\Windows"));
    SetDllDirectoryW(NULL);
    CHECK("... none after SetDllDirectory(NULL)", GetDllDirectoryW(MAX_PATH, dir) == 0);

    ULONGLONG kb = 0;
    CHECK("GetPhysicallyInstalledSystemMemory", GetPhysicallyInstalledSystemMemory(&kb) && kb > 1024);
    UINT32 count = 5, len = 0;
    LONG r = FindPackagesByPackageFamily(L"Microsoft.WebView2Runtime_8wekyb3d8bbwe", 0x10, &count, NULL, &len, NULL, NULL);
    CHECK("FindPackagesByPackageFamily: no packages", r == 0 && count == 0);
    int policy = 5;
    CHECK("AppPolicyGetThreadInitializationType", AppPolicyGetThreadInitializationType((HANDLE)-6, &policy) == 0 && policy == 0);
}

int main(void)
{
    int argc = 0;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc >= 3 && !lstrcmpW(argv[1], L"child")) return child(argv[2]);
    apartment();
    terminate_self();
    fls_many();
    names();
    misc();
    printf("wvstarttest: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
