/* tlsslots.exe — TLS indexes past the TEB's 64 (the 1024 expansion slots)
 * and fiber-local storage callbacks at process exit.
 *
 * TLS: TlsAlloc hands out 64 + 1024 indexes; each thread's values are its
 * own, a new thread reads zero in all of them, and an index freed and
 * allocated again reads zero in every thread, an expansion one too.
 *
 * FLS at exit: a child copy ("tlsslots child FILE ret|exit") sets FLS
 * values on its main thread and on a thread that never ends, then leaves
 * by returning from main or by ExitProcess.  The main thread's callbacks
 * must run before DLL_PROCESS_DETACH (seen by this program's TLS
 * callback), once each; the other thread's value reaches its callback
 * when the detach frees the index, also once. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <winternl.h>

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

#define MAXIDX 1200

/* ---------------------------------------------------------------- TLS */
static DWORD g_idx[MAXIDX];
static int g_n;
static HANDLE g_ready, g_go;
static volatile LONG g_bad;

static void *val(DWORD idx, DWORD salt) { return (void *)(ULONG_PTR)(((ULONG_PTR)idx << 12) | (salt & 0xFFF) | 1); }

static DWORD WINAPI worker(void *arg)
{
    DWORD salt = (DWORD)(ULONG_PTR)arg;
    for (int i = 0; i < g_n; i++)
        if (TlsGetValue(g_idx[i]) != NULL) { InterlockedIncrement(&g_bad); break; }   /* a new thread starts at zero */
    for (int i = 0; i < g_n; i++)
        if (!TlsSetValue(g_idx[i], val(g_idx[i], salt))) InterlockedIncrement(&g_bad);
    Sleep(5);                                       /* the other threads write theirs meanwhile */
    for (int i = 0; i < g_n; i++)
        if (TlsGetValue(g_idx[i]) != val(g_idx[i], salt)) { InterlockedIncrement(&g_bad); break; }
    return 0;
}

static DWORD g_low, g_high;
static volatile int g_low_after, g_high_after;
static DWORD WINAPI parked(void *arg)
{
    (void)arg;
    TlsSetValue(g_low, (void *)0x1234);
    TlsSetValue(g_high, (void *)0x5678);
    ReleaseSemaphore(g_ready, 1, NULL);
    WaitForSingleObject(g_go, INFINITE);
    g_low_after = TlsGetValue(g_low) == NULL;
    g_high_after = TlsGetValue(g_high) == NULL;
    return 0;
}

static void test_tls(void)
{
    while (g_n < MAXIDX && (g_idx[g_n] = TlsAlloc()) != TLS_OUT_OF_INDEXES) g_n++;
    DWORD top = 0;
    for (int i = 0; i < g_n; i++) if (g_idx[i] > top) top = g_idx[i];
    printf("tlsslots: %d TLS indexes free, highest %lu\n", g_n, (unsigned long)top);
    CHECK("more than 1024 TLS indexes (64 + 1024 expansion slots)", g_n > 1024 && g_n < 64 + 1024 + 1);
    CHECK("the highest index is 1087", top == 64 + 1024 - 1);
    CHECK("TlsAlloc fails once all are taken", TlsAlloc() == TLS_OUT_OF_INDEXES);

    int ok = 1;
    for (int i = 0; i < g_n; i++) ok &= TlsSetValue(g_idx[i], val(g_idx[i], 0)) != 0;
    for (int i = 0; i < g_n; i++) ok &= TlsGetValue(g_idx[i]) == val(g_idx[i], 0);
    CHECK("every index holds its value on the main thread", ok);
    void *exp = *(void **)(NtCurrentTebBytes() + TEB_TLS_EXPANSION);
    CHECK("TEB.TlsExpansionSlots holds the thread's array", exp != NULL);

    HANDLE th[4];
    for (int i = 0; i < 4; i++) th[i] = CreateThread(NULL, 0, worker, (void *)(ULONG_PTR)(i + 1), 0, NULL);
    WaitForMultipleObjects(4, th, TRUE, INFINITE);
    for (int i = 0; i < 4; i++) CloseHandle(th[i]);
    CHECK("four threads start at zero and keep their own values in all of them", g_bad == 0);
    ok = 1;
    for (int i = 0; i < g_n; i++) ok &= TlsGetValue(g_idx[i]) == val(g_idx[i], 0);
    CHECK("the main thread's values are untouched", ok);

    /* a freed index reads zero again in every thread, once reallocated */
    g_low = g_idx[0];
    g_high = g_idx[g_n - 1];
    g_ready = CreateSemaphoreW(NULL, 0, 1, NULL);
    g_go = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE p = CreateThread(NULL, 0, parked, NULL, 0, NULL);
    WaitForSingleObject(g_ready, INFINITE);
    CHECK("TlsFree (a TEB slot)", TlsFree(g_low));
    CHECK("TlsFree (an expansion slot)", TlsFree(g_high));
    CHECK("a freed slot comes back", TlsAlloc() == g_low && TlsAlloc() == g_high);
    CHECK("it reads zero on this thread", TlsGetValue(g_low) == NULL && TlsGetValue(g_high) == NULL);
    SetEvent(g_go);
    WaitForSingleObject(p, INFINITE);
    CloseHandle(p);
    CHECK("and on another thread, a TEB slot", g_low_after);
    CHECK("and on another thread, an expansion slot", g_high_after);

    SetLastError(0);
    CHECK("an index past 1087 is invalid",
          TlsGetValue(64 + 1024) == NULL && GetLastError() == ERROR_INVALID_PARAMETER && !TlsSetValue(64 + 1024, (void *)1));
    ok = 1;
    for (int i = 0; i < g_n; i++) ok &= TlsFree(g_idx[i]) != 0;
    CHECK("TlsFree all", ok);
    CHECK("a freed index cannot be freed twice", !TlsFree(g_idx[0]));
    int again = 0;
    DWORD x[MAXIDX];
    while (again < MAXIDX && (x[again] = TlsAlloc()) != TLS_OUT_OF_INDEXES) again++;
    for (int i = 0; i < again; i++) TlsFree(x[i]);
    CHECK("all of them come back", again == g_n);
}

/* ---------------------------------------------------- FLS at process exit */
static HANDLE g_log;
static DWORD g_fls;

static void logline(const char *s)
{
    DWORD n;
    if (g_log) WriteFile(g_log, s, (DWORD)strlen(s), &n, NULL);
}

static void WINAPI fls_cb(void *v) { logline((const char *)v); }

static void WINAPI tls_cb(PVOID h, DWORD reason, PVOID r)
{
    (void)h; (void)r;
    if (reason != DLL_PROCESS_DETACH || !g_log) return;
    logline("detach\n");
    FlsFree(g_fls);                                 /* the other thread's value */
    logline("end\n");
}
#pragma section(".CRT$XLB", long, read)
__declspec(allocate(".CRT$XLB")) PIMAGE_TLS_CALLBACK tlsslots_tls_cb = tls_cb;

static DWORD WINAPI stays(void *arg)
{
    (void)arg;
    FlsSetValue(g_fls, "other\n");
    SetEvent(g_ready);
    Sleep(INFINITE);
    return 0;
}

static int child(const char *file, const char *how)
{
    g_log = CreateFileA(file, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    if (g_log == INVALID_HANDLE_VALUE) return 2;
    g_fls = FlsAlloc(fls_cb);
    DWORD second = FlsAlloc(fls_cb);
    FlsSetValue(g_fls, "main\n");
    FlsSetValue(second, "main2\n");
    g_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    CloseHandle(CreateThread(NULL, 0, stays, NULL, 0, NULL));
    WaitForSingleObject(g_ready, INFINITE);
    if (!strcmp(how, "exit")) ExitProcess(0);
    return 0;
}

static void test_fls_exit(const char *self, const char *how)
{
    const char *f = "tlsslots-exit.tmp";
    char cl[MAX_PATH * 2], buf[256] = { 0 };
    snprintf(cl, sizeof(cl), "\"%s\" child %s %s", self, f, how);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessA(self, cl, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { CHECK("start the child", 0); return; }
    DWORD code = 1;
    BOOL ended = WaitForSingleObject(pi.hProcess, 20000) == WAIT_OBJECT_0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    HANDLE h = CreateFileA(f, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    DWORD n = 0;
    if (h != INVALID_HANDLE_VALUE) { ReadFile(h, buf, sizeof(buf) - 1, &n, NULL); CloseHandle(h); }
    DeleteFileA(f);
    printf("tlsslots: child (%s) logged: ", how);
    for (char *c = buf; *c; c++) putchar(*c == '\n' ? ' ' : *c);
    printf("\n");
    char what[96];
    snprintf(what, sizeof(what), "the child (%s) ends with code 0", how);
    CHECK(what, ended && code == 0);
    snprintf(what, sizeof(what), "%s: the main thread's FLS callbacks run once each, before DLL_PROCESS_DETACH", how);
    CHECK(what, !strcmp(buf, "main\nmain2\ndetach\nother\nend\n") || !strcmp(buf, "main2\nmain\ndetach\nother\nend\n"));
}

int main(int argc, char **argv)
{
    (void)*(PIMAGE_TLS_CALLBACK volatile *)&tlsslots_tls_cb;   /* keep the callback linked in */
    if (argc == 4 && !strcmp(argv[1], "child")) return child(argv[2], argv[3]);
    char self[MAX_PATH];
    GetModuleFileNameA(NULL, self, sizeof(self));
    test_tls();
    test_fls_exit(self, "ret");
    test_fls_exit(self, "exit");
    printf("tlsslots: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
