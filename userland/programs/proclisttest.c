/* proclisttest.exe — finding another running program by its name, owner and
 * command line, as Microsoft Edge Update does before it uninstalls itself
 * (it must not while one of its install workers runs): EnumProcesses, the
 * Tool Help snapshot, OpenProcess, GetProcessImageFileName and
 * QueryDosDevice, QueryFullProcessImageName, GetModuleFileNameEx, the
 * process token's user, ProcessIdToSessionId, IsWow64Process, and the
 * other program's command line read from its PEB with ReadProcessMemory
 * and through NtQueryInformationProcess(ProcessCommandLineInformation).
 *
 * `proclisttest` starts a copy of itself (`proclisttest child /handoff
 * ...`, which waits) and looks it up; the 64-bit build also starts the
 * 32-bit one and looks that up. */
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include <winternl.h>

/* Tool Help and the process status API (kernel32's K32 names), which the
 * headers here do not declare */
typedef struct {
    DWORD dwSize, cntUsage, th32ProcessID; ULONG_PTR th32DefaultHeapID; DWORD th32ModuleID, cntThreads,
    th32ParentProcessID; LONG pcPriClassBase; DWORD dwFlags; WCHAR szExeFile[MAX_PATH];
} PROCESSENTRY32W;
#define TH32CS_SNAPPROCESS 2
HANDLE WINAPI CreateToolhelp32Snapshot(DWORD flags, DWORD pid);
BOOL WINAPI Process32FirstW(HANDLE h, PROCESSENTRY32W *pe);
BOOL WINAPI Process32NextW(HANDLE h, PROCESSENTRY32W *pe);
BOOL WINAPI K32EnumProcesses(DWORD *pids, DWORD cb, DWORD *needed);
DWORD WINAPI K32GetProcessImageFileNameW(HANDLE p, LPWSTR buf, DWORD n);
DWORD WINAPI K32GetModuleFileNameExW(HANDLE p, HMODULE m, LPWSTR buf, DWORD n);
DWORD WINAPI K32GetModuleBaseNameW(HANDLE p, HMODULE m, LPWSTR buf, DWORD n);
BOOL WINAPI QueryFullProcessImageNameW(HANDLE p, DWORD flags, LPWSTR buf, PDWORD n);
BOOL WINAPI IsWow64Process2(HANDLE p, USHORT *machine, USHORT *native);
BOOL WINAPI ProcessIdToSessionId(DWORD pid, DWORD *session);
BOOL WINAPI IsWow64Process(HANDLE p, PBOOL wow);
BOOL WINAPI ReadProcessMemory(HANDLE p, LPCVOID base, LPVOID buf, SIZE_T n, SIZE_T *done);
DWORD WINAPI QueryDosDeviceW(LPCWSTR name, LPWSTR buf, DWORD n);
#define PROCESS_VM_READ                   0x0010
#define PROCESS_QUERY_INFORMATION         0x0400
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#define PROCESS_NAME_NATIVE               1
#define MACHINE_I386                      0x014C
#define MACHINE_AMD64                     0x8664

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

#define CHILD_ARGS L" child /handoff \"appguid={F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}&needsadmin=false\" /silent"

typedef NTSTATUS (NTAPI *NtQIP)(HANDLE, ULONG, PVOID, ULONG, PULONG);
static NtQIP qip;

/* "\Device\HarddiskVolume1\x" to "C:\x" through the drives' device names,
 * as Edge Update converts GetProcessImageFileName's answer */
static BOOL dos_path(const WCHAR *dev, WCHAR *out, DWORD cap)
{
    WCHAR drives[128], target[MAX_PATH];
    if (!GetLogicalDriveStringsW(128, drives)) return FALSE;
    for (WCHAR *d = drives; *d; d += wcslen(d) + 1) {
        WCHAR name[3] = { d[0], ':', 0 };
        if (!QueryDosDeviceW(name, target, MAX_PATH)) continue;
        size_t n = wcslen(target);
        if (!_wcsnicmp(dev, target, n) && dev[n] == '\\') {
            _snwprintf(out, cap, L"%s%s", name, dev + n);
            return TRUE;
        }
    }
    return FALSE;
}

/* The command line in the PEB of process @h (same bitness as this one),
 * read with ReadProcessMemory as Edge Update does */
static BOOL peb_command_line(HANDLE h, WCHAR *out, DWORD cap)
{
    PROCESS_BASIC_INFORMATION pbi;
    ULONG got = 0;
    if (qip(h, 0 /* ProcessBasicInformation */, &pbi, sizeof(pbi), &got) < 0) return FALSE;
    BYTE *params = 0;
    SIZE_T n = 0;
    if (!ReadProcessMemory(h, (BYTE *)pbi.PebBaseAddress + (sizeof(void *) == 4 ? 0x10 : 0x20), &params, sizeof(params), &n))
        return FALSE;
    BYTE upp[0x400];                                   /* (Edge Update reads 0x290 bytes of it) */
    if (!ReadProcessMemory(h, params, upp, sizeof(void *) == 4 ? 0x290 : 0x400, &n)) return FALSE;
    UNICODE_STRING *cl = (UNICODE_STRING *)(upp + (sizeof(void *) == 4 ? 0x40 : 0x70));
    if (cl->MaximumLength < cl->Length + 2 || cl->MaximumLength > 2 * cap) return FALSE;
    return ReadProcessMemory(h, cl->Buffer, out, cl->MaximumLength, &n) && !out[cl->Length / 2];
}

/* A UNICODE_STRING class of NtQueryInformationProcess as a string */
static BOOL info_string(HANDLE h, ULONG cls, WCHAR *out, DWORD cap)
{
    BYTE b[4096];
    ULONG got = 0;
    if (qip(h, cls, b, sizeof(b), &got) < 0) return FALSE;
    UNICODE_STRING *u = (UNICODE_STRING *)b;
    if (u->Length / 2 >= cap || got != sizeof(*u) + u->MaximumLength) return FALSE;
    memcpy(out, u->Buffer, u->Length);
    out[u->Length / 2] = 0;
    return TRUE;
}

static BOOL same_user(HANDLE h)
{
    HANDLE t1 = 0, t2 = 0;
    BYTE u1[256], u2[256];
    DWORD n;
    BOOL ok = OpenProcessToken(h, TOKEN_QUERY, &t1) && OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t2) &&
              GetTokenInformation(t1, TokenUser, u1, sizeof(u1), &n) && GetTokenInformation(t2, TokenUser, u2, sizeof(u2), &n) &&
              EqualSid(((TOKEN_USER *)u1)->User.Sid, ((TOKEN_USER *)u2)->User.Sid);
    if (t1) CloseHandle(t1);
    if (t2) CloseHandle(t2);
    return ok;
}

/* Edge Update's lookup of its install workers: programs called @exe_name,
 * not this one, run by this user in this session, whose image path is
 * @exe_name's and whose command line holds @arg and @dir; the first PID or 0 */
static DWORD find_worker(const WCHAR *exe_name, const WCHAR *arg, const WCHAR *dir)
{
    DWORD pids[1024], n = 0, session = 0, mine = 0;
    if (!K32EnumProcesses(pids, sizeof(pids), &n)) return 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &mine);
    for (DWORD i = 0; i < n / 4; i++) {
        if (pids[i] == GetCurrentProcessId()) continue;
        if (!ProcessIdToSessionId(pids[i], &session) || session != mine) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pids[i]);
        if (!h) continue;
        WCHAR dev[MAX_PATH], path[MAX_PATH];
        BOOL named = K32GetProcessImageFileNameW(h, dev, MAX_PATH) && dos_path(dev, path, MAX_PATH);
        CloseHandle(h);
        const WCHAR *base = named ? wcsrchr(path, '\\') : 0;
        if (!base || _wcsicmp(base + 1, exe_name)) continue;
        h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pids[i]);
        if (!h) continue;
        WCHAR cl[1024];
        BOOL match = same_user(h) && peb_command_line(h, cl, 1024);
        CloseHandle(h);
        if (!match) continue;
        _wcslwr(cl);
        if (wcsstr(cl, arg) && wcsstr(cl, dir)) return pids[i];
    }
    return 0;
}

static void look_up(const WCHAR *exe, BOOL wow, const WCHAR *tag)
{
    WCHAR cmd[1024], expect[1024];
    _snwprintf(cmd, 1024, L"\"%s\"" CHILD_ARGS, exe);
    wcscpy(expect, cmd);
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe, cmd, 0, 0, FALSE, 0, 0, 0, &si, &pi)) {
        fail++;
        printf("FAIL: CreateProcess %ls (%lu)\n", exe, GetLastError());
        return;
    }
    char what[64];
    snprintf(what, sizeof(what), "%ls: ", tag);
#define C(name, cond) do { char m[160]; snprintf(m, sizeof(m), "%s%s", what, name); CHECK(m, cond); } while (0)

    DWORD pids[1024], n = 0;
    BOOL listed = FALSE;
    C("EnumProcesses", K32EnumProcesses(pids, sizeof(pids), &n));
    for (DWORD i = 0; i < n / 4; i++) listed |= pids[i] == pi.dwProcessId;
    C("EnumProcesses lists the child", listed);

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe = { sizeof(pe) };
    BOOL named = FALSE;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (pe.th32ProcessID == pi.dwProcessId) named = !_wcsicmp(pe.szExeFile, L"proclisttest.exe");
    CloseHandle(snap);
    C("Tool Help names the child", named);

    WCHAR dev[MAX_PATH], path[MAX_PATH], buf[MAX_PATH];
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pi.dwProcessId);
    C("OpenProcess(QUERY_LIMITED_INFORMATION)", h != 0);
    DWORD k = K32GetProcessImageFileNameW(h, dev, MAX_PATH);
    C("GetProcessImageFileNameW is a device path", k && !wcsncmp(dev, L"\\Device\\HarddiskVolume", 22) && k == wcslen(dev));
    C("QueryDosDevice maps it to the program", dos_path(dev, path, MAX_PATH) && !_wcsicmp(path, exe));
    DWORD sz = MAX_PATH;
    C("QueryFullProcessImageNameW", QueryFullProcessImageNameW(h, 0, buf, &sz) && !_wcsicmp(buf, exe) && sz == wcslen(buf));
    sz = MAX_PATH;
    C("QueryFullProcessImageNameW(PROCESS_NAME_NATIVE)", QueryFullProcessImageNameW(h, PROCESS_NAME_NATIVE, buf, &sz) && !wcscmp(buf, dev));
    sz = 5;
    C("QueryFullProcessImageNameW with too small a buffer", !QueryFullProcessImageNameW(h, 0, buf, &sz) && GetLastError() == ERROR_INSUFFICIENT_BUFFER);
    C("GetModuleFileNameExW(NULL)", K32GetModuleFileNameExW(h, 0, buf, MAX_PATH) && !_wcsicmp(buf, exe));
    C("GetModuleBaseNameW(NULL)", K32GetModuleBaseNameW(h, 0, buf, MAX_PATH) && !_wcsicmp(buf, L"proclisttest.exe"));
    C("NtQueryInformationProcess(ProcessImageFileName)", info_string(h, 27, buf, MAX_PATH) && !wcscmp(buf, dev));
    C("NtQueryInformationProcess(ProcessImageFileNameWin32)", info_string(h, 43, buf, MAX_PATH) && !_wcsicmp(buf, exe));
    BOOL w = 2;
    C("IsWow64Process", IsWow64Process(h, &w) && w == wow);
    USHORT machine = 1, native = 0;
    C("IsWow64Process2", IsWow64Process2(h, &machine, &native) && machine == (wow ? MACHINE_I386 : 0) &&
                         native == MACHINE_AMD64);
    DWORD s1 = 7, s2 = 8;
    C("ProcessIdToSessionId", ProcessIdToSessionId(pi.dwProcessId, &s1) && ProcessIdToSessionId(GetCurrentProcessId(), &s2) && s1 == s2);
    CloseHandle(h);

    h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pi.dwProcessId);
    C("OpenProcess(QUERY_INFORMATION | VM_READ)", h != 0);
    C("the token's user is this user", same_user(h));
    WCHAR cl[1024];
    if (wow == (sizeof(void *) == 4))                  /* the PEB's layout is this program's */
        C("the command line from its PEB (ReadProcessMemory)", peb_command_line(h, cl, 1024) && !wcscmp(cl, expect));
    C("NtQueryInformationProcess(ProcessCommandLineInformation)", info_string(h, 60, cl, 1024) && !wcscmp(cl, expect));
    BYTE small[24];
    ULONG got = 0;
    NTSTATUS st = qip(h, 60, small, sizeof(small), &got);
    C("ProcessCommandLineInformation with too small a buffer", st == (NTSTATUS)0xC0000004 && got == sizeof(UNICODE_STRING) + 2 * wcslen(expect) + 2);
    CloseHandle(h);

    if (wow == (sizeof(void *) == 4)) {
        WCHAR dir[MAX_PATH];
        wcscpy(dir, exe);
        *wcsrchr(dir, '\\') = 0;
        _wcslwr(dir);
        C("Edge Update's lookup finds the running worker", find_worker(L"proclisttest.exe", L"/handoff", dir) == pi.dwProcessId);
        C("and not with another argument", find_worker(L"proclisttest.exe", L"/install", dir) == 0);
    }

    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 10000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
#undef C
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "child")) {        /* the "install worker": waits to be found */
        Sleep(120000);
        return 0;
    }
    qip = (NtQIP)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
    CHECK("NtQueryInformationProcess", qip != 0);
    if (!qip) return 1;
    WCHAR self[MAX_PATH], dev[MAX_PATH], path[MAX_PATH];
    GetModuleFileNameW(0, self, MAX_PATH);
    CHECK("GetProcessImageFileNameW of this process", K32GetProcessImageFileNameW(GetCurrentProcess(), dev, MAX_PATH) &&
                                                      dos_path(dev, path, MAX_PATH) && !_wcsicmp(path, self));
    BOOL w = 2;
    CHECK("IsWow64Process of this process", IsWow64Process(GetCurrentProcess(), &w) && w == (sizeof(void *) == 4));
    look_up(self, sizeof(void *) == 4, sizeof(void *) == 4 ? L"x86 child" : L"x64 child");
    if (sizeof(void *) == 8) look_up(L"C:\\Programs\\x86\\proclisttest.exe", TRUE, L"x86 child of x64");
    printf("proclisttest: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
