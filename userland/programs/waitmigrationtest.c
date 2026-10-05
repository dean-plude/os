/* Wait-any and wait-all while threads yield across CPU run queues.
 * Unsignaled events force wait_objects to register the current thread. */
#include <windows.h>
#include <stdio.h>

#define ROUNDS 2000
#define MAX_WORKERS 32
static HANDLE g_events[2];
static volatile LONG g_bad, g_finished;

static DWORD WINAPI worker(LPVOID arg)
{
    int id = (int)(INT_PTR)arg;
    for (int i = 0; i < ROUNDS; i++) {
        SwitchToThread();
        DWORD r = WaitForMultipleObjects(2, g_events, (i + id) & 1, 1);
        if (r != WAIT_TIMEOUT) { InterlockedIncrement(&g_bad); break; }
    }
    InterlockedIncrement(&g_finished);
    return 0;
}

int main(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int n = (int)si.dwNumberOfProcessors * 4;
    if (n < 8) n = 8;
    if (n > MAX_WORKERS) n = MAX_WORKERS;
    g_events[0] = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_events[1] = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_events[0] || !g_events[1]) { printf("FAIL create events\n"); return 1; }
    HANDLE threads[MAX_WORKERS];
    for (int i = 0; i < n; i++) {
        threads[i] = CreateThread(NULL, 0, worker, (LPVOID)(INT_PTR)i, 0, NULL);
        if (!threads[i]) { printf("FAIL create worker %d\n", i); return 1; }
    }
    if (WaitForMultipleObjects(n, threads, TRUE, 120000) != WAIT_OBJECT_0 || g_bad || g_finished != n) {
        printf("FAIL wait workers: %ld finished, %ld bad waits\n", (long)g_finished, (long)g_bad);
        return 1;
    }
    for (int i = 0; i < n; i++) CloseHandle(threads[i]);
    CloseHandle(g_events[0]);
    CloseHandle(g_events[1]);
    printf("waitmigrationtest: %d workers, %d waits, 0 failed\n", n, n * ROUNDS);
    return 0;
}
