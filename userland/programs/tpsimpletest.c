/* One-shot thread-pool callbacks: correct context and x86 calling
 * convention, no handle leak across more than 4096 submissions. */
#include <windows.h>
#include <winternl.h>
#include <stdio.h>

typedef VOID (WINAPI *SimpleFn)(PVOID instance, PVOID ctx);
WINBASEAPI BOOL WINAPI TrySubmitThreadpoolCallback(SimpleFn fn, PVOID ctx, PVOID env);
WINBASEAPI BOOL WINAPI GetProcessHandleCount(HANDLE process, PDWORD count);

__declspec(dllimport) NTSTATUS NTAPI LdrLockLoaderLock(ULONG flags, PULONG state, PULONG_PTR cookie);
__declspec(dllimport) NTSTATUS NTAPI LdrUnlockLoaderLock(ULONG flags, ULONG_PTR cookie);

static DWORD WINAPI warm_thread(PVOID unused)
{
    (void)unused;
    return 0;
}

/* The loader lazily creates one process-wide contention event. Force
 * that initialization and join the warm-up thread before taking the
 * leak baseline; retain exact handle checks for every measured batch. */
static BOOL warm_loader_lock(void)
{
    PEB *peb = *(PEB **)(NtCurrentTebBytes() + TEB_PEB);
    PRTL_CRITICAL_SECTION lock = peb->LoaderLock;
    ULONG state = 0;
    ULONG_PTR cookie = 0;
    if (!lock || LdrLockLoaderLock(0, &state, &cookie) != 0 || !cookie) return FALSE;
    HANDLE thread = CreateThread(NULL, 0, warm_thread, NULL, 0, NULL);
    BOOL initialized = FALSE;
    if (thread) {
        DWORD start = GetTickCount();
        while (!*(HANDLE volatile *)&lock->LockSemaphore && GetTickCount() - start < 5000) Sleep(1);
        initialized = *(HANDLE volatile *)&lock->LockSemaphore != NULL;
    }
    NTSTATUS unlocked = LdrUnlockLoaderLock(0, cookie);
    if (!thread) return FALSE;
    DWORD done = WaitForSingleObject(thread, 10000);
    CloseHandle(thread);
    return initialized && unlocked == 0 && done == WAIT_OBJECT_0;
}

#define BATCH 32
#define ROUNDS 160
static HANDLE g_done;
static volatile LONG g_calls, g_finished, g_bad;
static HANDLE g_threads[BATCH];
static int g_context;

static VOID WINAPI callback(PVOID instance, PVOID ctx)
{
    if (!instance || ctx != &g_context) InterlockedIncrement(&g_bad);
    LONG slot = InterlockedIncrement(&g_calls) - 1;
    if (slot < 0 || slot >= BATCH) {
        InterlockedIncrement(&g_bad);
        return;
    }
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                         &g_threads[slot], SYNCHRONIZE, FALSE, 0))
        InterlockedIncrement(&g_bad);
    if (InterlockedIncrement(&g_finished) == BATCH) SetEvent(g_done);
}

/* Callback completion precedes RtlExitUserThread and DLL_THREAD_DETACH.
 * Join the actual threads so unfinished teardown cannot accumulate across
 * batches and exhaust the smaller WOW64 thread table. Close our temporary
 * handles before checking the exact leak baseline. */
static BOOL join_batch(void)
{
    BOOL ok = TRUE;
    DWORD start = GetTickCount();
    for (int i = 0; i < BATCH; i++) {
        DWORD elapsed = GetTickCount() - start;
        if (!g_threads[i] || WaitForSingleObject(g_threads[i],
                elapsed < 10000 ? 10000 - elapsed : 0) != WAIT_OBJECT_0) ok = FALSE;
        if (g_threads[i]) {
            if (!CloseHandle(g_threads[i])) ok = FALSE;
            g_threads[i] = NULL;
        }
    }
    return ok;
}

int main(void)
{
    if (!warm_loader_lock()) {
        printf("FAIL loader-lock warm-up (%lu)\n", (unsigned long)GetLastError());
        return 1;
    }
    g_done = CreateEventW(NULL, FALSE, FALSE, NULL);
    DWORD before = 0, after = 0;
    if (!g_done || !GetProcessHandleCount(GetCurrentProcess(), &before)) {
        printf("FAIL initial handle count (%lu)\n", (unsigned long)GetLastError());
        return 1;
    }
    for (int round = 0; round < ROUNDS; round++) {
        g_calls = g_finished = 0;
        for (int i = 0; i < BATCH; i++) {
            if (!TrySubmitThreadpoolCallback(callback, &g_context, NULL)) {
                printf("FAIL submission %d (%lu)\n", round * BATCH + i, (unsigned long)GetLastError());
                return 1;
            }
        }
        if (WaitForSingleObject(g_done, 10000) != WAIT_OBJECT_0 || g_calls != BATCH || g_bad) {
            printf("FAIL callback batch %d: %ld calls, %ld bad contexts\n", round, (long)g_calls, (long)g_bad);
            return 1;
        }
        if (!join_batch()) {
            printf("FAIL thread teardown after batch %d (%lu)\n", round, (unsigned long)GetLastError());
            return 1;
        }
        if (!GetProcessHandleCount(GetCurrentProcess(), &after) || after > before) {
            printf("FAIL handles after batch %d: %lu before, %lu after\n", round,
                   (unsigned long)before, (unsigned long)after);
            return 1;
        }
    }
    CloseHandle(g_done);
    printf("tpsimpletest: %d callbacks, correct context, no leaked handles\n", BATCH * ROUNDS);
    return 0;
}
