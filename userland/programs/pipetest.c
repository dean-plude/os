/*
 * pipetest.exe — pipes, handle inheritance, overlapped I/O and cmd.exe
 *
 *   pipetest          run the tests
 *   pipetest upper    (child) copy stdin to stdout in upper case
 *   pipetest write H  (child) write "inherited" to handle H (decimal)
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <io.h>

static int g_pass, g_fail, g_verbose;

static void check(int ok, const char *what)
{
    if (g_verbose) fprintf(stderr, "  %s: %s\n", ok ? "ok" : "FAIL", what);
    if (ok) g_pass++;
    else { g_fail++; printf("FAIL: %s (error %lu)\n", what, GetLastError()); }
}

static char g_self[MAX_PATH];

/* Run @cmdline with stdin/stdout redirected; its exit code */
static int run_child(const char *cmdline, HANDLE in, HANDLE out, BOOL inherit)
{
    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in ? in : GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = out ? out : GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi;
    char *cl = _strdup(cmdline);
    BOOL ok = CreateProcessA(0, cl, 0, 0, inherit, 0, 0, 0, &si, &pi);
    free(cl);
    if (!ok) return -1;
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return (int)code;
}

/* Everything readable from @h until the writers are gone */
static int read_all(HANDLE h, char *buf, int cap)
{
    int n = 0;
    DWORD got;
    while (n < cap - 1 && ReadFile(h, buf + n, (DWORD)(cap - 1 - n), &got, 0) && got) n += (int)got;
    buf[n] = 0;
    return n;
}

static void anonymous(void)
{
    HANDLE r, w;
    check(CreatePipe(&r, &w, 0, 0), "CreatePipe");
    DWORD n;
    check(GetFileType(r) == FILE_TYPE_PIPE, "GetFileType: pipe");
    check(WriteFile(w, "hello", 5, &n, 0) && n == 5, "write");
    DWORD avail = 0;
    check(PeekNamedPipe(r, 0, 0, 0, &avail, 0) && avail == 5, "PeekNamedPipe: 5 bytes waiting");
    char buf[64];
    check(ReadFile(r, buf, sizeof(buf), &n, 0) && n == 5 && !memcmp(buf, "hello", 5), "read");
    CloseHandle(w);
    check(!ReadFile(r, buf, sizeof(buf), &n, 0) && GetLastError() == ERROR_BROKEN_PIPE, "read after the writer closed: ERROR_BROKEN_PIPE");
    CloseHandle(r);

    check(CreatePipe(&r, &w, 0, 0), "CreatePipe (2)");
    CloseHandle(r);
    check(!WriteFile(w, "x", 1, &n, 0) && GetLastError() == ERROR_NO_DATA, "write after the reader closed: ERROR_NO_DATA");
    CloseHandle(w);

    /* a big write needs the reader: a thread drains it */
    check(CreatePipe(&r, &w, 0, 4096), "CreatePipe (small)");
    static char big[300000];
    for (int i = 0; i < (int)sizeof(big); i++) big[i] = (char)('a' + i % 26);
    HANDLE th;
    DWORD WINAPI drain(LPVOID p);
    th = CreateThread(0, 0, drain, r, 0, 0);
    check(WriteFile(w, big, sizeof(big), &n, 0) && n == sizeof(big), "300000-byte write through a pipe");
    CloseHandle(w);
    WaitForSingleObject(th, INFINITE);
    DWORD total = 0;
    GetExitCodeThread(th, &total);
    check(total == sizeof(big), "the reader got all of it");
    CloseHandle(th);
    CloseHandle(r);
}

DWORD WINAPI drain(LPVOID p)
{
    char buf[7000];
    DWORD n, total = 0;
    int ok = 1;
    while (ReadFile((HANDLE)p, buf, sizeof(buf), &n, 0) && n) {
        for (DWORD i = 0; i < n; i++) if (buf[i] != (char)('a' + (total + i) % 26)) ok = 0;
        total += n;
    }
    return ok ? total : 0;
}

static void children(void)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), 0, TRUE };
    HANDLE r, w;
    char buf[512], cl[MAX_PATH + 64];

    /* the child's output */
    check(CreatePipe(&r, &w, &sa, 0), "CreatePipe (inheritable)");
    SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);
    DWORD flags = 1;
    check(GetHandleInformation(r, &flags) && flags == 0, "GetHandleInformation: not inherited");
    check(GetHandleInformation(w, &flags) && flags == HANDLE_FLAG_INHERIT, "GetHandleInformation: inherited");
    int code = run_child("cmd.exe /c echo hello-from-cmd& exit 3", 0, w, TRUE);
    CloseHandle(w);
    read_all(r, buf, sizeof(buf));
    CloseHandle(r);
    check(code == 3, "cmd /c ... & exit 3: exit code 3");
    check(!strcmp(buf, "hello-from-cmd\r\n"), "the output of cmd /c echo");

    /* the child's input and output: pipetest upper */
    HANDLE ir, iw, orr, ow;
    CreatePipe(&ir, &iw, &sa, 0);
    CreatePipe(&orr, &ow, &sa, 0);
    SetHandleInformation(iw, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(orr, HANDLE_FLAG_INHERIT, 0);
    DWORD n;
    WriteFile(iw, "shout this\n", 11, &n, 0);
    CloseHandle(iw);
    snprintf(cl, sizeof(cl), "\"%s\" upper", g_self);
    code = run_child(cl, ir, ow, TRUE);
    CloseHandle(ir);
    CloseHandle(ow);
    read_all(orr, buf, sizeof(buf));
    CloseHandle(orr);
    check(code == 0 && !strcmp(buf, "SHOUT THIS\n"), "a child reading a pipe and writing another");

    /* an inherited handle that is not a standard one */
    CreatePipe(&r, &w, &sa, 0);
    SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);
    snprintf(cl, sizeof(cl), "\"%s\" write %llu", g_self, (unsigned long long)(ULONG_PTR)w);
    code = run_child(cl, 0, 0, TRUE);
    CloseHandle(w);
    read_all(r, buf, sizeof(buf));
    CloseHandle(r);
    check(code == 0 && !strcmp(buf, "inherited"), "a child writing to an inherited handle by its value");

    /* not inherited when bInheritHandles is FALSE: the pipe ends */
    CreatePipe(&r, &w, &sa, 0);
    SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);
    snprintf(cl, sizeof(cl), "\"%s\" write %llu", g_self, (unsigned long long)(ULONG_PTR)w);
    code = run_child(cl, 0, 0, FALSE);
    CloseHandle(w);
    read_all(r, buf, sizeof(buf));
    CloseHandle(r);
    check(code != 0 && !buf[0], "no handles inherited without bInheritHandles");

    /* the environment goes to children */
    SetEnvironmentVariableA("NOVA_PIPETEST", "from-the-parent");
    CreatePipe(&r, &w, &sa, 0);
    SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);
    run_child("cmd.exe /c echo %NOVA_PIPETEST%", 0, w, TRUE);
    CloseHandle(w);
    read_all(r, buf, sizeof(buf));
    CloseHandle(r);
    check(!strcmp(buf, "from-the-parent\r\n"), "a child sees a variable set at run time");
    char *env = GetEnvironmentStringsA();
    int found = 0;
    for (char *e = env; *e; e += strlen(e) + 1) if (!strcmp(e, "NOVA_PIPETEST=from-the-parent")) found = 1;
    FreeEnvironmentStringsA(env);
    check(found, "GetEnvironmentStrings lists it");

    /* a pipeline in cmd: a program's output into another program */
    CreatePipe(&r, &w, &sa, 0);
    SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);
    snprintf(cl, sizeof(cl), "cmd.exe /c echo piped words| \"%s\" upper", g_self);
    code = run_child(cl, 0, w, TRUE);
    CloseHandle(w);
    read_all(r, buf, sizeof(buf));
    CloseHandle(r);
    check(code == 0 && !strcmp(buf, "PIPED WORDS\r\n"), "cmd: echo ... | pipetest upper");
}

static void crt(void)
{
    FILE *f = _popen("echo popen-works", "r");
    char line[128] = "";
    check(f != 0, "_popen");
    if (f) {
        fgets(line, sizeof(line), f);
        check(!strcmp(line, "popen-works\r\n"), "_popen: the command's output");
        check(_pclose(f) == 0, "_pclose: exit code 0");
    }
    check(system("exit 7") == 7, "system(\"exit 7\") returns 7");
    check(system(0) != 0, "system(NULL): there is a command interpreter");
    int fds[2];
    check(_pipe(fds, 256, 0) == 0, "_pipe");
    check(_write(fds[1], "abc", 3) == 3, "_write to a pipe");
    _close(fds[1]);
    char b[8];
    check(_read(fds[0], b, 8) == 3, "_read from a pipe");
    check(_read(fds[0], b, 8) == 0, "_read at the end of a pipe returns 0");
    _close(fds[0]);
    HANDLE nul = CreateFileA("nul", GENERIC_READ | GENERIC_WRITE, 0, 0, OPEN_EXISTING, 0, 0);
    DWORD n = 0;
    check(nul != INVALID_HANDLE_VALUE && WriteFile(nul, "gone", 4, &n, 0) && n == 4, "writing to NUL");
    check(ReadFile(nul, b, 8, &n, 0) && n == 0, "reading NUL: end of file");
    CloseHandle(nul);
    check(GetFileAttributesA("C:\\Temp\\nul") == INVALID_FILE_ATTRIBUTES, "no file named NUL was made");
}

typedef struct { const char *name; int msg; HANDLE ready; } ServerArg;

static DWORD WINAPI server(LPVOID p)
{
    ServerArg *a = p;
    HANDLE h = CreateNamedPipeA(a->name, PIPE_ACCESS_DUPLEX,
                                a->msg ? PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE : PIPE_TYPE_BYTE,
                                1, 4096, 4096, 0, 0);
    SetEvent(a->ready);                                 /* the pipe exists now */
    if (h == INVALID_HANDLE_VALUE) return 1;
    if (!ConnectNamedPipe(h, 0) && GetLastError() != ERROR_PIPE_CONNECTED) return 2;
    char buf[64];
    DWORD n;
    if (!ReadFile(h, buf, sizeof(buf), &n, 0)) return 3;
    for (DWORD i = 0; i < n; i++) buf[i] = (char)toupper((unsigned char)buf[i]);
    if (!WriteFile(h, buf, n, &n, 0)) return 4;
    if (a->msg) {
        WriteFile(h, "0123456789", 10, &n, 0);           /* a message longer than the reader's buffer */
    }
    FlushFileBuffers(h);
    char x;
    ReadFile(h, &x, 1, &n, 0);                         /* until the client closes */
    DisconnectNamedPipe(h);
    CloseHandle(h);
    return 0;
}

static void named(void)
{
    ServerArg a = { "\\\\.\\pipe\\nova-test", 0, CreateEventA(0, TRUE, FALSE, 0) };
    check(!WaitNamedPipeA(a.name, 100) && GetLastError() == ERROR_FILE_NOT_FOUND, "WaitNamedPipe: no such pipe yet");
    HANDLE th = CreateThread(0, 0, server, &a, 0, 0);
    WaitForSingleObject(a.ready, 5000);
    check(WaitNamedPipeA(a.name, 5000), "WaitNamedPipe");
    HANDLE c = CreateFileA(a.name, GENERIC_READ | GENERIC_WRITE, 0, 0, OPEN_EXISTING, 0, 0);
    check(c != INVALID_HANDLE_VALUE, "CreateFile on \\\\.\\pipe\\nova-test");
    DWORD n;
    char buf[64];
    check(WriteFile(c, "abc", 3, &n, 0), "client write");
    check(ReadFile(c, buf, sizeof(buf), &n, 0) && n == 3 && !memcmp(buf, "ABC", 3), "client reads the server's reply");
    CloseHandle(c);
    WaitForSingleObject(th, INFINITE);
    DWORD code = 9;
    GetExitCodeThread(th, &code);
    check(code == 0, "named pipe server thread");
    CloseHandle(th);
    check(CreateFileA(a.name, GENERIC_READ, 0, 0, OPEN_EXISTING, 0, 0) == INVALID_HANDLE_VALUE &&
          GetLastError() == ERROR_FILE_NOT_FOUND, "no such pipe once the server is gone");

    /* message mode */
    ServerArg m = { "\\\\.\\pipe\\nova-msg", 1, CreateEventA(0, TRUE, FALSE, 0) };
    th = CreateThread(0, 0, server, &m, 0, 0);
    WaitForSingleObject(m.ready, 5000);
    WaitNamedPipeA(m.name, 5000);
    c = CreateFileA(m.name, GENERIC_READ | GENERIC_WRITE, 0, 0, OPEN_EXISTING, 0, 0);
    DWORD mode = PIPE_READMODE_MESSAGE;
    check(SetNamedPipeHandleState(c, &mode, 0, 0), "SetNamedPipeHandleState: message mode");
    check(TransactNamedPipe(c, "hi", 2, buf, sizeof(buf), &n, 0) && n == 2 && !memcmp(buf, "HI", 2), "TransactNamedPipe");
    check(!ReadFile(c, buf, 4, &n, 0) && GetLastError() == ERROR_MORE_DATA && n == 4, "a long message: ERROR_MORE_DATA");
    check(ReadFile(c, buf, sizeof(buf), &n, 0) && n == 6 && !memcmp(buf, "456789", 6), "the rest of the message");
    DWORD flags = 0, maxi = 0;
    check(GetNamedPipeInfo(c, &flags, 0, 0, &maxi) && (flags & PIPE_TYPE_MESSAGE) && maxi == 1, "GetNamedPipeInfo");
    CloseHandle(c);
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
}

static VOID WINAPI done_routine(DWORD err, DWORD bytes, LPOVERLAPPED ov) { ov->hEvent = (HANDLE)(ULONG_PTR)(0x1000 + bytes + err); }

static void overlapped(void)
{
    const char *name = "\\\\.\\pipe\\nova-ov";
    HANDLE s = CreateNamedPipeA(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE, 1, 4096, 4096, 0, 0);
    check(s != INVALID_HANDLE_VALUE, "CreateNamedPipe (overlapped)");
    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEventA(0, TRUE, FALSE, 0);
    check(!ConnectNamedPipe(s, &ov) && GetLastError() == ERROR_IO_PENDING, "ConnectNamedPipe: pending");
    HANDLE c = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, 0, OPEN_EXISTING, 0, 0);
    check(WaitForSingleObject(ov.hEvent, 5000) == WAIT_OBJECT_0, "the connect event is set when the client comes");
    DWORD n;
    check(GetOverlappedResult(s, &ov, &n, FALSE), "GetOverlappedResult (connect)");

    /* a read with nothing to read stays pending */
    char buf[64];
    ResetEvent(ov.hEvent);
    check(!ReadFile(s, buf, sizeof(buf), &n, &ov) && GetLastError() == ERROR_IO_PENDING, "ReadFile: ERROR_IO_PENDING");
    check(!GetOverlappedResult(s, &ov, &n, FALSE) && GetLastError() == ERROR_IO_INCOMPLETE, "still incomplete");
    WriteFile(c, "later", 5, &n, 0);
    check(WaitForSingleObject(ov.hEvent, 5000) == WAIT_OBJECT_0, "the event is set when data arrives");
    check(GetOverlappedResult(s, &ov, &n, TRUE) && n == 5 && !memcmp(buf, "later", 5), "the pending read got the data");

    /* cancelling */
    check(!ReadFile(s, buf, sizeof(buf), &n, &ov) && GetLastError() == ERROR_IO_PENDING, "another pending read");
    check(CancelIoEx(s, &ov), "CancelIoEx");
    check(!GetOverlappedResult(s, &ov, &n, TRUE) && GetLastError() == ERROR_OPERATION_ABORTED, "cancelled: ERROR_OPERATION_ABORTED");

    /* a completion port */
    HANDLE port = CreateIoCompletionPort(s, 0, 77, 1);
    check(port != 0, "CreateIoCompletionPort on a pipe");
    OVERLAPPED ov2;
    memset(&ov2, 0, sizeof(ov2));
    check(!ReadFile(s, buf, sizeof(buf), &n, &ov2) && GetLastError() == ERROR_IO_PENDING, "a pending read on a port-bound pipe");
    WriteFile(c, "port", 4, &n, 0);
    ULONG_PTR key = 0;
    LPOVERLAPPED got = 0;
    check(GetQueuedCompletionStatus(port, &n, &key, &got, 5000) && key == 77 && got == &ov2 && n == 4 &&
          !memcmp(buf, "port", 4), "GetQueuedCompletionStatus: the read's packet");

    /* a completion routine */
    HANDLE s2 = CreateNamedPipeA("\\\\.\\pipe\\nova-apc", PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED, 0, 1, 4096, 4096, 0, 0);
    HANDLE c2 = CreateFileA("\\\\.\\pipe\\nova-apc", GENERIC_READ | GENERIC_WRITE, 0, 0, OPEN_EXISTING, 0, 0);
    OVERLAPPED ov3;
    memset(&ov3, 0, sizeof(ov3));
    check(ReadFileEx(s2, buf, sizeof(buf), &ov3, done_routine), "ReadFileEx");
    WriteFile(c2, "apc!", 4, &n, 0);
    DWORD r = SleepEx(5000, TRUE);
    check(r == WAIT_IO_COMPLETION && ov3.hEvent == (HANDLE)(ULONG_PTR)(0x1000 + 4), "the completion routine ran (4 bytes)");

    CloseHandle(c2); CloseHandle(s2);
    CloseHandle(port);
    CloseHandle(c);
    CloseHandle(s);
    CloseHandle(ov.hEvent);
}

int main(int argc, char **argv)
{
    GetModuleFileNameA(0, g_self, MAX_PATH);
    if (argc > 1 && !strcmp(argv[1], "upper")) {
        int ch;
        while ((ch = getchar()) != EOF) putchar(toupper(ch));
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "write")) {
        HANDLE h = (HANDLE)(ULONG_PTR)_strtoui64(argv[2], 0, 10);
        DWORD n;
        return WriteFile(h, "inherited", 9, &n, 0) ? 0 : 1;
    }
    g_verbose = argc > 1 && !strcmp(argv[1], "-v");
    anonymous();
    children();
    crt();
    named();
    overlapped();
    printf("pipetest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail != 0;
}
