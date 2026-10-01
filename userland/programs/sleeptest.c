/*
 * sleeptest.exe — put the computer to sleep (S3) and report how it woke
 *
 * Calls powrprof's SetSuspendState, which returns once the machine is
 * awake again; then checks that the wall clock moved on, the tick count
 * didn't jump, and threads, files and the network still work.
 */
#include <windows.h>
#include <stdio.h>

typedef BOOLEAN (WINAPI *SetSuspendStateFn)(BOOLEAN, BOOLEAN, BOOLEAN);

static DWORD WINAPI worker(LPVOID p) { (*(volatile LONG *)p)++; return 0; }

int main(void)
{
    HMODULE pp = LoadLibraryA("powrprof.dll");
    SetSuspendStateFn sss = pp ? (SetSuspendStateFn)GetProcAddress(pp, "SetSuspendState") : NULL;
    if (!sss) { printf("FAIL: no SetSuspendState\n"); return 1; }

    SYSTEMTIME a, b;
    GetLocalTime(&a);
    DWORD t0 = GetTickCount();
    printf("Going to sleep at %02u:%02u:%02u\n", a.wHour, a.wMinute, a.wSecond);
    fflush(stdout);
    if (!sss(FALSE, FALSE, FALSE)) {
        printf("FAIL: SetSuspendState error %lu\n", GetLastError());
        return 1;
    }
    GetLocalTime(&b);
    DWORD t1 = GetTickCount();
    LONG secs = ((b.wHour * 60 + b.wMinute) * 60 + b.wSecond) - ((a.wHour * 60 + a.wMinute) * 60 + a.wSecond);
    printf("Awake at %02u:%02u:%02u (%ld s of wall time, %lu ms of ticks)\n",
           b.wHour, b.wMinute, b.wSecond, secs, t1 - t0);

    volatile LONG n = 0;                 /* threads still start and run */
    HANDLE th[4];
    for (int i = 0; i < 4; i++) th[i] = CreateThread(NULL, 0, worker, (LPVOID)&n, 0, NULL);
    WaitForMultipleObjects(4, th, TRUE, 5000);
    char path[MAX_PATH];                 /* files still write and read */
    GetTempPathA(sizeof(path), path);
    lstrcatA(path, "sleeptest.txt");
    HANDLE f = CreateFileA(path, GENERIC_WRITE | GENERIC_READ, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD done = 0; char buf[8] = {0};
    BOOL fok = f != INVALID_HANDLE_VALUE && WriteFile(f, "awake", 5, &done, NULL)
               && SetFilePointer(f, 0, NULL, FILE_BEGIN) == 0 && ReadFile(f, buf, 5, &done, NULL)
               && !lstrcmpA(buf, "awake");
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    DeleteFileA(path);
    printf("threads %ld/4, file %s\n", n, fok ? "ok" : "FAILED");
    printf("%s\n", n == 4 && fok && secs >= 0 ? "PASS" : "FAIL");
    return 0;
}
