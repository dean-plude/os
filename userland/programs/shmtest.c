/* shmtest.exe — named shared memory between two processes: the parent
 * creates a mapping, the child it starts opens it by name, reads the
 * parent's message and writes an answer back.  Then (64-bit) a view in
 * another process: a reserved section mapped into a suspended child
 * through its process handle, as Firefox's launcher places its hooks'
 * trampolines in the browser process */
#include <stdio.h>
#include <string.h>
#include <windows.h>

#define NAME "Local\\NovaShmTest"
#define FNAME "Local\\NovaShmFile"
#define FPATH "C:\\Temp\\shmtest.bin"

static int child(void)
{
    HANDLE m = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, NAME);
    if (!m) { printf("child: OpenFileMapping failed (%lu)\n", GetLastError()); return 2; }
    char *p = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!p) { printf("child: MapViewOfFile failed (%lu)\n", GetLastError()); return 3; }
    printf("child: read \"%s\"\n", p);
    strcpy(p + 2048, "pong from the child");
    p[8191] = 'Z';                                  /* the second page too */
    UnmapViewOfFile(p);
    CloseHandle(m);
    /* the file-backed mapping: change the file through it */
    HANDLE fm = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, FNAME);
    char *q = fm ? MapViewOfFile(fm, FILE_MAP_ALL_ACCESS, 0, 0, 0) : NULL;
    if (!q) { printf("child: file mapping open failed (%lu)\n", GetLastError()); return 4; }
    printf("child: file view says \"%s\"\n", q);
    memcpy(q, "child wrote", 11);
    UnmapViewOfFile(q);
    CloseHandle(fm);
    return 0;
}

#ifdef _WIN64
#define RESERVE 0x4000000                                   /* SEC_RESERVE */
typedef struct { PVOID lowest, highest; SIZE_T align; } AddrReq;      /* MEM_ADDRESS_REQUIREMENTS */
typedef struct { ULONGLONG type; PVOID ptr; } ExtParam;              /* MEM_EXTENDED_PARAMETER */
typedef PVOID (WINAPI *MapViewOfFile3Fn)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, ExtParam *, ULONG);
typedef LONG (WINAPI *NtMapViewOfSectionFn)(HANDLE, HANDLE, PVOID *, ULONG_PTR, SIZE_T, LARGE_INTEGER *, SIZE_T *, DWORD, ULONG, ULONG);
typedef LONG (WINAPI *NtUnmapViewOfSectionFn)(HANDLE, PVOID);
typedef BOOL (WINAPI *ReadProcessMemoryFn)(HANDLE, LPCVOID, LPVOID, SIZE_T, SIZE_T *);

/* A SEC_RESERVE section: committed through the local view, mapped into a
 * suspended child (MapViewOfFile3 within 2 GB of ntdll, and
 * NtMapViewOfSection), read back with ReadProcessMemory */
static int remote_views(const char *self)
{
    MapViewOfFile3Fn map3 = (MapViewOfFile3Fn)GetProcAddress(GetModuleHandleA("kernel32.dll"), "MapViewOfFile3");
    NtMapViewOfSectionFn ntmap = (NtMapViewOfSectionFn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtMapViewOfSection");
    NtUnmapViewOfSectionFn ntunmap = (NtUnmapViewOfSectionFn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtUnmapViewOfSection");
    ReadProcessMemoryFn ReadProcessMemory = (ReadProcessMemoryFn)GetProcAddress(GetModuleHandleA("kernel32.dll"), "ReadProcessMemory");
    if (!map3 || !ntmap || !ntunmap || !ReadProcessMemory) { printf("remote views: missing exports\n"); return 0; }
    HANDLE m = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_EXECUTE_READWRITE | RESERVE, 0, 65536, NULL);
    char *local = m ? MapViewOfFile(m, FILE_MAP_WRITE, 0, 0, 0) : NULL;
    if (!local) { printf("remote views: reserved section failed (%lu)\n", GetLastError()); return 0; }
    if (!VirtualAlloc(local, 4096, MEM_COMMIT, PAGE_READWRITE)) {
        printf("remote views: committing the local view failed (%lu)\n", GetLastError());
        return 0;
    }
    strcpy(local, "trampoline");

    char cmd[MAX_PATH + 16];
    snprintf(cmd, sizeof(cmd), "%s child-idle", self);
    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
        printf("remote views: CreateProcess failed (%lu)\n", GetLastError());
        return 0;
    }
    int ok = 1;
    char *ntdll = (char *)GetModuleHandleA("ntdll.dll");    /* at the same address in the child */
    AddrReq req = { ntdll - 0x70000000, ntdll + 0x70000000, 0 };
    ExtParam par = { 1, &req };                             /* MemExtendedParameterAddressRequirements */
    char *remote = map3(m, pi.hProcess, NULL, 0, 65536, 0, PAGE_EXECUTE_READ, &par, 1);
    char got[16] = { 0 };
    SIZE_T n = 0;
    if (!remote) { printf("remote views: MapViewOfFile3 into the child failed (%lu)\n", GetLastError()); ok = 0; }
    else {
        int close_by = remote >= (char *)req.lowest && remote < (char *)req.highest;
        ReadProcessMemory(pi.hProcess, remote, got, 11, &n);
        printf("remote view near ntdll: %s, child reads \"%s\"\n", close_by ? "yes" : "NO", got);
        if (!close_by || strcmp(got, "trampoline")) ok = 0;
    }
    PVOID base = NULL;
    SIZE_T size = 0;
    LARGE_INTEGER off;
    off.QuadPart = 0;
    LONG st = ntmap(m, pi.hProcess, &base, 0, 0, &off, &size, 2, 0, PAGE_EXECUTE_READ);
    memset(got, 0, sizeof(got));
    if (st < 0) { printf("remote views: NtMapViewOfSection into the child failed (%08lx)\n", (unsigned long)st); ok = 0; }
    else {
        strcpy(local + 16, "second");
        ReadProcessMemory(pi.hProcess, (char *)base + 16, got, 7, &n);
        printf("second remote view reads \"%s\"; unmapped: %s\n", got, ntunmap(pi.hProcess, base) >= 0 ? "yes" : "NO");
        if (strcmp(got, "second") || ReadProcessMemory(pi.hProcess, base, got, 1, &n)) ok = 0;
    }
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    UnmapViewOfFile(local);
    CloseHandle(m);
    return ok;
}
#endif

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "child")) return child();
    if (argc > 1 && !strcmp(argv[1], "child-idle")) return 0;

    HANDLE m = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 8192, NAME);
    if (!m) { printf("CreateFileMapping failed (%lu)\n", GetLastError()); return 1; }
    char *p = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!p) { printf("MapViewOfFile failed (%lu)\n", GetLastError()); return 1; }
    int zero = 1;
    for (int i = 0; i < 8192; i++) if (p[i]) zero = 0;
    printf("new mapping is zeroed: %s\n", zero ? "yes" : "NO");

    /* a second create of the same name opens it */
    HANDLE again = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 8192, NAME);
    printf("second create: %s\n", again && GetLastError() == ERROR_ALREADY_EXISTS ? "already exists (ok)" : "WRONG");
    char *q = MapViewOfFile(again, FILE_MAP_READ, 0, 0, 0);
    strcpy(p, "ping from the parent");
    printf("second view sees: \"%s\"\n", q ? q : "(no view)");

    /* a file-backed mapping shared by name */
    CreateDirectoryA("C:\\Temp", NULL);
    HANDLE f = CreateFileA(FPATH, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD wr;
    WriteFile(f, "file content here", 17, &wr, NULL);
    HANDLE fm = CreateFileMappingA(f, NULL, PAGE_READWRITE, 0, 0, FNAME);
    char *fv = fm ? MapViewOfFile(fm, FILE_MAP_ALL_ACCESS, 0, 0, 0) : NULL;
    printf("file view: \"%s\"\n", fv ? fv : "(none)");

    char cmd[MAX_PATH + 16];
    GetModuleFileNameA(NULL, cmd, MAX_PATH);
    strcat(cmd, " child");
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { printf("CreateProcess failed (%lu)\n", GetLastError()); return 1; }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 99;
    GetExitCodeProcess(pi.hProcess, &code);
    printf("child exited with %lu\n", code);
    printf("parent reads: \"%s\", last byte '%c'\n", p + 2048, p[8191]);
    int ok = code == 0 && !strcmp(p + 2048, "pong from the child") && p[8191] == 'Z';
    printf("file view after child: \"%s\"\n", fv ? fv : "(none)");
    if (!fv || memcmp(fv, "child wrote", 11)) ok = 0;
    UnmapViewOfFile(fv);
    CloseHandle(fm);
    char rb[32] = { 0 };
    SetFilePointer(f, 0, NULL, FILE_BEGIN);
    ReadFile(f, rb, 17, &wr, NULL);
    CloseHandle(f);
    DeleteFileA(FPATH);
    printf("file on disk: \"%s\"\n", rb);
    if (memcmp(rb, "child wrote", 11)) ok = 0;

    UnmapViewOfFile(q);
    UnmapViewOfFile(p);
    CloseHandle(again);
    CloseHandle(m);
    /* the name is gone with its last handle */
    HANDLE gone = OpenFileMappingA(FILE_MAP_READ, FALSE, NAME);
    printf("after closing: %s\n", gone ? "STILL OPEN" : "gone (ok)");
    if (gone) ok = 0;
#ifdef _WIN64
    char self[MAX_PATH];
    GetModuleFileNameA(NULL, self, MAX_PATH);
    if (!remote_views(self)) ok = 0;
#endif
    printf("shmtest: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
