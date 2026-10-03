/*
 * savetest.exe — NovaOS keeps answering while it saves drive C:
 *
 * Writes a large file to C:\Temp (32 MiB, or `savetest MIB`), which NovaOS
 * saves to its disk once drive C: has been quiet for a second, then for 12
 * seconds keeps timing three calls on three threads, each call behind one
 * of the locks a save used to hold for the whole disk write:
 * GetProcessHandleCount (the big kernel lock), GetMonitorInfo (the desktop
 * lock) and GetFileAttributes (the file-system lock).  It passes when none
 * of them waited 250 ms or more (before saves let go of the locks, a 64 MiB
 * save kept the desktop and file-system locks for 300-600 ms in QEMU), and
 * the file-system call less than 100 ms (the desktop's redraws hold the
 * desktop lock for 50-100 ms in QEMU without KVM, but take the file-system
 * lock only around what they read from files).
 * The kernel logs the save itself ("[PERSIST] Saved ..."), with the time
 * it held the file-system lock; the self-test checks that line too.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

WINBASEAPI BOOL WINAPI GetProcessHandleCount(HANDLE process, PDWORD count);   /* (not in our headers) */

#define WINDOW_MS 12000
#define LIMIT_MS  250
#define FS_LIMIT_MS 100

static LARGE_INTEGER g_freq;

static double ms_since(const LARGE_INTEGER *a)
{
    LARGE_INTEGER b;
    QueryPerformanceCounter(&b);
    return (double)(b.QuadPart - a->QuadPart) * 1000.0 / (double)g_freq.QuadPart;
}

/* Thread @arg times one of the calls over and over until g_stop */
static volatile LONG g_stop;
static double g_worst[3];
static long g_rounds[3];

static DWORD WINAPI probe(LPVOID arg)
{
    int which = (int)(INT_PTR)arg;
    POINT origin = { 0, 0 };
    HMONITOR mon = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    while (!g_stop) {
        LARGE_INTEGER t;
        DWORD handles;
        MONITORINFO mi = { sizeof(mi) };
        QueryPerformanceCounter(&t);
        if (which == 0) GetProcessHandleCount(GetCurrentProcess(), &handles);
        else if (which == 1) GetMonitorInfoA(mon, &mi);
        else GetFileAttributesA("C:\\Windows");
        double ms = ms_since(&t);
        if (ms > g_worst[which]) g_worst[which] = ms;
        g_rounds[which]++;
        Sleep(1);
    }
    return 0;
}

int main(int argc, char **argv)
{
    int mib = argc > 1 ? atoi(argv[1]) : 32;
    if (mib < 1 || mib > 200) mib = 32;
    QueryPerformanceFrequency(&g_freq);
    CreateDirectoryA("C:\\Temp", NULL);
    const char *path = "C:\\Temp\\savetest.bin";
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { printf("FAIL: cannot create %s (%lu)\n", path, GetLastError()); return 1; }
    static unsigned char chunk[1 << 20];
    for (int i = 0; i < mib; i++) {
        for (int k = 0; k < (int)sizeof(chunk); k += 4) *(DWORD *)(chunk + k) = (DWORD)(i << 20 | k);
        DWORD n;
        if (!WriteFile(f, chunk, sizeof(chunk), &n, NULL) || n != sizeof(chunk)) {
            printf("FAIL: writing %s (%lu)\n", path, GetLastError());
            CloseHandle(f);
            return 1;
        }
    }
    CloseHandle(f);
    printf("savetest: wrote %d MiB to %s; timing calls for %d s while it is saved\n", mib, path, WINDOW_MS / 1000);

    HANDLE th[3];
    for (int i = 0; i < 3; i++) th[i] = CreateThread(NULL, 0, probe, (LPVOID)(INT_PTR)i, 0, NULL);
    Sleep(WINDOW_MS);
    g_stop = 1;
    WaitForMultipleObjects(3, th, TRUE, INFINITE);
    double worst_bkl = g_worst[0], worst_desk = g_worst[1], worst_fs = g_worst[2];
    long rounds = g_rounds[0] + g_rounds[1] + g_rounds[2];
    DeleteFileA(path);
    printf("savetest: longest waits over %ld rounds: kernel lock %.1f ms, desktop lock %.1f ms, file system %.1f ms\n",
           rounds, worst_bkl, worst_desk, worst_fs);
    if (worst_bkl >= LIMIT_MS || worst_desk >= LIMIT_MS) {
        printf("savetest: FAIL (a call waited %d ms or more)\n", LIMIT_MS);
        return 1;
    }
    if (worst_fs >= FS_LIMIT_MS) {
        printf("savetest: FAIL (the file-system call waited %d ms or more)\n", FS_LIMIT_MS);
        return 1;
    }
    printf("savetest: PASS\n");
    return 0;
}
