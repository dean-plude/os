/*
 * proctest.exe — CreateProcess flags and what a child shares with its parent
 *
 *   proctest            run the tests
 *   proctest touch F    (child) create file F
 *   proctest console    (child) exit with the number of processes on its console
 *   proctest write H S  (child) write S to handle H (decimal), at its current position
 *
 * CREATE_SUSPENDED: the child does nothing until ResumeThread.
 * CREATE_NEW_CONSOLE: the child is alone on a console of its own (a
 * Terminal window that closes when it ends); without it, it shares ours.
 * File positions: an inherited or duplicated handle shares the open file's
 * position, so parent and child writes follow each other.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass, g_fail;

static void check(int ok, const char *what)
{
    if (ok) g_pass++;
    else { g_fail++; printf("FAIL: %s (error %lu)\n", what, GetLastError()); }
}

static char g_self[MAX_PATH];

/* Start "proctest ARGS" with @flags; 0 if it did not start */
static BOOL start(const char *args, DWORD flags, BOOL inherit, STARTUPINFOA *si_in, PROCESS_INFORMATION *pi)
{
    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (si_in) si = *si_in;
    char cl[MAX_PATH * 2];
    snprintf(cl, sizeof(cl), "\"%s\" %s", g_self, args);
    return CreateProcessA(g_self, cl, 0, 0, inherit, flags, 0, 0, &si, pi);
}

/* Wait for the child; its exit code (-1: it did not end in 20 s) */
static int finish(PROCESS_INFORMATION *pi)
{
    DWORD code = (DWORD)-1;
    if (WaitForSingleObject(pi->hProcess, 20000) == WAIT_OBJECT_0) GetExitCodeProcess(pi->hProcess, &code);
    else TerminateProcess(pi->hProcess, 1);
    CloseHandle(pi->hThread);
    CloseHandle(pi->hProcess);
    return (int)code;
}

static BOOL exists(const char *f) { return GetFileAttributesA(f) != INVALID_FILE_ATTRIBUTES; }

static void suspended(void)
{
    const char *f = "proctest-suspended.tmp";
    DeleteFileA(f);
    PROCESS_INFORMATION pi;
    char args[64];
    snprintf(args, sizeof(args), "touch %s", f);
    if (!start(args, CREATE_SUSPENDED, FALSE, 0, &pi)) { check(0, "CreateProcess(CREATE_SUSPENDED)"); return; }
    check(1, "CreateProcess(CREATE_SUSPENDED)");
    check(WaitForSingleObject(pi.hProcess, 500) == WAIT_TIMEOUT, "a suspended child does not run");
    check(!exists(f), "a suspended child has not created its file");
    check(ResumeThread(pi.hThread) == 1, "ResumeThread: suspend count was 1");
    check(finish(&pi) == 0, "the resumed child exits with 0");
    check(exists(f), "the resumed child created its file");
    DeleteFileA(f);

    /* a suspended child can be ended without ever running */
    if (start(args, CREATE_SUSPENDED, FALSE, 0, &pi)) {
        check(TerminateProcess(pi.hProcess, 7), "TerminateProcess on a suspended child");
        check(finish(&pi) == 7, "the suspended child ended with code 7");
        check(!exists(f), "the terminated child never ran");
    } else check(0, "CreateProcess(CREATE_SUSPENDED) again");
}

static void new_console(void)
{
    PROCESS_INFORMATION pi;
    DWORD ids[8];
    DWORD here = GetConsoleProcessList(ids, 8);
    check(here >= 1, "GetConsoleProcessList: we are on a console");
    int k = 0;
    for (DWORD i = 0; i < here && i < 8; i++) if (ids[i] == GetCurrentProcessId()) k = 1;
    check(k, "GetConsoleProcessList lists us");

    check(start("console", 0, FALSE, 0, &pi), "CreateProcess (our console)");
    check(finish(&pi) == (int)here + 1, "a child shares our console (one process more on it)");

    check(start("console", CREATE_NEW_CONSOLE, FALSE, 0, &pi), "CreateProcess(CREATE_NEW_CONSOLE)");
    check(finish(&pi) == 1, "a CREATE_NEW_CONSOLE child is alone on its console");
    check(GetConsoleProcessList(ids, 8) == here, "our console is as it was");
}

static HANDLE open_new(const char *f, BOOL inherit)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), 0, inherit };
    return CreateFileA(f, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
}

static int contents(const char *f, char *buf, int cap)
{
    HANDLE h = CreateFileA(f, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
    DWORD n = 0;
    if (h == INVALID_HANDLE_VALUE) { buf[0] = 0; return 0; }
    ReadFile(h, buf, (DWORD)cap - 1, &n, 0);
    buf[n] = 0;
    CloseHandle(h);
    return (int)n;
}

static DWORD pos(HANDLE h) { return SetFilePointer(h, 0, 0, FILE_CURRENT); }

static void positions(void)
{
    const char *f = "proctest-position.tmp";
    char buf[64], args[64];
    DWORD n;
    HANDLE h = open_new(f, TRUE);
    if (h == INVALID_HANDLE_VALUE) { check(0, "create the test file"); return; }
    check(WriteFile(h, "AB", 2, &n, 0) && n == 2, "parent writes AB");

    /* an inherited handle: the child writes at our position and moves it */
    PROCESS_INFORMATION pi;
    snprintf(args, sizeof(args), "write %llu CD", (unsigned long long)(ULONG_PTR)h);
    check(start(args, 0, TRUE, 0, &pi), "CreateProcess (inheriting the file)");
    check(finish(&pi) == 0, "the child wrote to the inherited handle");
    check(pos(h) == 4, "the child's write moved our position to 4");
    check(WriteFile(h, "EF", 2, &n, 0) && n == 2, "parent writes EF");

    /* the file as a child's standard output */
    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = h;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    check(start("write 1 GH", 0, TRUE, &si, &pi), "CreateProcess (the file as standard output)");
    check(finish(&pi) == 0, "the child wrote to its standard output");
    check(pos(h) == 8, "its write moved our position to 8");

    /* a duplicate shares it; a second open has its own */
    HANDLE d = 0;
    check(DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &d, 0, FALSE, DUPLICATE_SAME_ACCESS), "DuplicateHandle");
    check(SetFilePointer(d, 1, 0, FILE_BEGIN) == 1, "move the duplicate to 1");
    check(pos(h) == 1, "the original moved with it");
    HANDLE o = CreateFileA(f, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
    check(o != INVALID_HANDLE_VALUE && pos(o) == 0, "a second open starts at 0");
    if (o != INVALID_HANDLE_VALUE) CloseHandle(o);
    CloseHandle(d);
    SetFilePointer(h, 0, 0, FILE_END);
    check(WriteFile(h, "IJ", 2, &n, 0) && n == 2, "parent writes IJ after closing the duplicate");
    CloseHandle(h);
    contents(f, buf, sizeof(buf));
    check(!strcmp(buf, "ABCDEFGHIJ"), "the file holds ABCDEFGHIJ");
    if (strcmp(buf, "ABCDEFGHIJ")) printf("  the file holds \"%s\"\n", buf);
    DeleteFileA(f);
}

int main(int argc, char **argv)
{
    GetModuleFileNameA(0, g_self, sizeof(g_self));
    if (argc >= 3 && !strcmp(argv[1], "touch")) {
        HANDLE h = CreateFileA(argv[2], GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
        if (h == INVALID_HANDLE_VALUE) return 1;
        CloseHandle(h);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "console")) {
        DWORD ids[16];
        printf("proctest: %s\n", "a console process");
        return (int)GetConsoleProcessList(ids, 16);
    }
    if (argc >= 4 && !strcmp(argv[1], "write")) {
        unsigned long long v = strtoull(argv[2], 0, 10);
        HANDLE h = v == 1 ? GetStdHandle(STD_OUTPUT_HANDLE) : (HANDLE)(ULONG_PTR)v;
        DWORD n;
        return WriteFile(h, argv[3], (DWORD)strlen(argv[3]), &n, 0) && n == strlen(argv[3]) ? 0 : 1;
    }

    suspended();
    new_console();
    positions();
    printf("proctest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
