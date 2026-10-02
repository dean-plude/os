/*
 * taskkill.exe — end processes
 *
 *   taskkill /IM image.exe [/IM ...] [/PID n ...] [/F] [/T]
 *
 * Without /F the process's windows are asked to close (WM_CLOSE); a process
 * with no window can only be ended with /F, as on Windows.  /T is accepted.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    DWORD dwSize, cntUsage, th32ProcessID; ULONG_PTR th32DefaultHeapID; DWORD th32ModuleID, cntThreads,
    th32ParentProcessID; LONG pcPriClassBase; DWORD dwFlags; CHAR szExeFile[MAX_PATH];
} PE32;
HANDLE WINAPI CreateToolhelp32Snapshot(DWORD flags, DWORD pid);
BOOL WINAPI Process32First(HANDLE h, LPVOID pe);
BOOL WINAPI Process32Next(HANDLE h, LPVOID pe);

static int g_closed;

static BOOL CALLBACK close_window(HWND w, LPARAM pid)
{
    DWORD p = 0;
    GetWindowThreadProcessId(w, &p);
    if (p == (DWORD)pid && IsWindowVisible(w)) { PostMessageA(w, WM_CLOSE, 0, 0); g_closed++; }
    return TRUE;
}

/* 0 when ended (or asked to close) */
static int end_process(DWORD pid, const char *name, int force)
{
    if (!force) {
        g_closed = 0;
        EnumWindows(close_window, (LPARAM)pid);
        if (g_closed) {
            printf("SUCCESS: Sent termination signal to the process \"%s\" with PID %lu.\n", name, pid);
            return 0;
        }
        printf("ERROR: The process \"%s\" with PID %lu could not be terminated.\n"
               "Reason: This process can only be terminated forcefully (with /F option).\n", name, pid);
        return 1;
    }
    HANDLE h = OpenProcess(0x0001 /* PROCESS_TERMINATE */, FALSE, pid);
    if (!h || !TerminateProcess(h, 1)) {
        printf("ERROR: The process \"%s\" with PID %lu could not be terminated.\n"
               "Reason: Access is denied.\n", name, pid);
        if (h) CloseHandle(h);
        return 1;
    }
    CloseHandle(h);
    printf("SUCCESS: The process \"%s\" with PID %lu has been terminated.\n", name, pid);
    return 0;
}

int main(int argc, char **argv)
{
    const char *images[32]; DWORD pids[32];
    int ni = 0, np = 0, force = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if ((a[0] == '/' || a[0] == '-') && !_stricmp(a + 1, "F")) force = 1;
        else if ((a[0] == '/' || a[0] == '-') && !_stricmp(a + 1, "T")) ;
        else if ((a[0] == '/' || a[0] == '-') && !_stricmp(a + 1, "IM") && i + 1 < argc && ni < 32) images[ni++] = argv[++i];
        else if ((a[0] == '/' || a[0] == '-') && !_stricmp(a + 1, "PID") && i + 1 < argc && np < 32) pids[np++] = strtoul(argv[++i], 0, 10);
        else if (!_stricmp(a, "/?")) {
            printf("TASKKILL [/F] [/T] { [/PID processid | /IM imagename] }\n");
            return 0;
        } else {
            fprintf(stderr, "ERROR: Invalid argument/option - '%s'.\nType \"TASKKILL /?\" for usage.\n", a);
            return 1;
        }
    }
    if (!ni && !np) {
        fprintf(stderr, "ERROR: Invalid syntax. Neither /FI nor /PID nor /IM were specified.\n"
                        "Type \"TASKKILL /?\" for usage.\n");
        return 1;
    }
    int failed = 0;
    int found_i[32] = {0}, found_p[32] = {0};
    HANDLE snap = CreateToolhelp32Snapshot(2 /* TH32CS_SNAPPROCESS */, 0);
    PE32 pe;
    pe.dwSize = sizeof(pe);
    DWORD self = GetCurrentProcessId();
    if (snap != INVALID_HANDLE_VALUE && Process32First(snap, &pe)) {
        do {
            if (pe.th32ProcessID == self) continue;
            int hit = 0;
            for (int i = 0; i < ni; i++) if (!_stricmp(pe.szExeFile, images[i])) found_i[i] = hit = 1;
            for (int i = 0; i < np; i++) if (pe.th32ProcessID == pids[i]) found_p[i] = hit = 1;
            if (hit && end_process(pe.th32ProcessID, pe.szExeFile, force)) failed = 1;
        } while (Process32Next(snap, &pe));
    }
    if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
    int missing = 0;
    for (int i = 0; i < ni; i++)
        if (!found_i[i]) { printf("ERROR: The process \"%s\" not found.\n", images[i]); missing = 1; }
    for (int i = 0; i < np; i++)
        if (!found_p[i]) { printf("ERROR: The process \"%lu\" not found.\n", pids[i]); missing = 1; }
    return missing ? 128 : failed ? 1 : 0;
}
