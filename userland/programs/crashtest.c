/* crashtest.exe — a crashed program leaves a report in C:\NovaOS\Crashes.
 *
 * Starts crash.exe (from the folder crashtest.exe is in, so the 32-bit
 * build starts the 32-bit crash.exe), checks that it ended with
 * STATUS_ACCESS_VIOLATION, then waits for the report the kernel writes,
 * C:\NovaOS\Crashes\crash-YYYYMMDD-HHMMSS-PID.txt, and checks what it
 * says: the program and PID, the exception, the place in crash.exe, the
 * address 0 it wrote to, return addresses and the module list. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

/* The report for @pid, read whole (NULL if there is none yet) */
static char *find_report(DWORD pid, char *name, size_t cap)
{
    WIN32_FIND_DATAA fd;
    char want[32];
    snprintf(want, sizeof(want), "-%lu.txt", (unsigned long)pid);
    HANDLE h = FindFirstFileA("C:\\NovaOS\\Crashes\\crash-*.txt", &fd);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    char *text = NULL;
    do {
        size_t n = strlen(fd.cFileName), w = strlen(want);
        if (n <= w || _stricmp(fd.cFileName + n - w, want)) continue;
        snprintf(name, cap, "C:\\NovaOS\\Crashes\\%s", fd.cFileName);
        HANDLE f = CreateFileA(name, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (f == INVALID_HANDLE_VALUE) continue;
        DWORD size = GetFileSize(f, NULL), got = 0;
        text = malloc(size + 1);
        if (text && ReadFile(f, text, size, &got, NULL)) text[got] = 0;
        CloseHandle(f);
        break;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return text;
}

int main(void)
{
    char exe[MAX_PATH], cmd[MAX_PATH + 8];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    char *slash = strrchr(exe, '\\');
    if (slash) strcpy(slash + 1, "crash.exe");
    snprintf(cmd, sizeof(cmd), "\"%s\"", exe);

    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    BOOL started = CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    CHECK("crash.exe starts", started);
    if (!started) { printf("crashtest: %d passed, %d failed\n", pass, fail); return 1; }
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CHECK("crash.exe ends with STATUS_ACCESS_VIOLATION", code == 0xC0000005u);
    DWORD pid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    char name[MAX_PATH], *r = NULL;
    for (int i = 0; i < 100 && !(r = find_report(pid, name, sizeof(name))); i++) Sleep(100);   /* (written a moment later) */
    CHECK("a report in C:\\NovaOS\\Crashes", r != NULL);
    if (r) {
        char line[64];
        printf("crashtest: crash.exe (PID %lu) left %s\n", (unsigned long)pid, name);
        CHECK("it says it is a crash report", !strncmp(r, "NovaOS crash report", 19));
        snprintf(line, sizeof(line), "crash.exe (PID %lu,", (unsigned long)pid);
        CHECK("it names the program and its PID", strstr(r, line) != NULL);
        CHECK("it names the exception", strstr(r, "access violation (0xC0000005)") != NULL);
        CHECK("it says where (crash.exe+offset)", strstr(r, "At:         crash.exe+0x") != NULL);
        CHECK("it gives the address written to", strstr(r, "Address:    0x0 ") != NULL);
        CHECK("it lists return addresses", strstr(r, "Return addresses on the stack") != NULL);
        CHECK("it lists the modules (ntdll.dll)", strstr(r, "Modules:") && strstr(r, "ntdll.dll"));
        CHECK("it ends with the kernel's log", strstr(r, "The end of the kernel's log:") != NULL);
        CHECK("it has the time and version", strstr(r, "Time:       ") && strstr(r, "NovaOS:     "));
        free(r);
    }
    printf("crashtest: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
