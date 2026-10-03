/* crtthreads.exe — the per-thread pieces a multi-threaded program leans
 * on beyond errno: fiber-local storage callbacks (run when a thread ends,
 * when a fiber is deleted and on every thread's value when the slot is
 * freed; FLS slots are not TLS slots), _configthreadlocale's per-thread
 * locale in msvcrt.dll and ucrtbase.dll, and getenv/_wgetenv results that
 * stay intact while other threads read and change variables */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <wchar.h>
#include <windows.h>

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

/* ---------------------------------------------------------------- FLS */
typedef struct { DWORD tid; volatile LONG freed; DWORD freed_on; } Rec;
static volatile LONG g_calls;
static void WINAPI fls_cb(void *v)
{
    Rec *r = v;
    InterlockedIncrement(&r->freed);
    r->freed_on = GetCurrentThreadId();
    InterlockedIncrement(&g_calls);
}

static DWORD g_idx;
static HANDLE g_set, g_go;
static Rec g_rec[6];

static DWORD WINAPI exiting(void *arg)
{
    Rec *r = arg;
    r->tid = GetCurrentThreadId();
    return !FlsSetValue(g_idx, r) || FlsGetValue(g_idx) != r;
}

static DWORD WINAPI parked(void *arg)
{
    Rec *r = arg;
    r->tid = GetCurrentThreadId();
    FlsSetValue(g_idx, r);
    ReleaseSemaphore(g_set, 1, NULL);
    WaitForSingleObject(g_go, INFINITE);
    return FlsGetValue(g_idx) != NULL;              /* the slot was freed under it */
}

static int count_tls(void)
{
    DWORD got[1100];
    int n = 0;
    while (n < 1100 && (got[n] = TlsAlloc()) != TLS_OUT_OF_INDEXES) n++;
    for (int i = 0; i < n; i++) TlsFree(got[i]);
    return n;
}

/* the fiber calls, from kernel32 by name */
static LPVOID (WINAPI *ConvertThreadToFiber_)(LPVOID);
static BOOL (WINAPI *ConvertFiberToThread_)(void);
static LPVOID (WINAPI *CreateFiber_)(SIZE_T, void (WINAPI *)(LPVOID), LPVOID);
static VOID (WINAPI *SwitchToFiber_)(LPVOID);
static VOID (WINAPI *DeleteFiber_)(LPVOID);

static DWORD g_fidx;
static void *g_main_fiber;
static Rec g_frec;
static volatile int g_fiber_saw;
static void WINAPI fiber_main(void *p)
{
    (void)p;
    g_fiber_saw = FlsGetValue(g_fidx) == NULL;      /* a new fiber starts with no values */
    FlsSetValue(g_fidx, &g_frec);
    SwitchToFiber_(g_main_fiber);
}

static void test_fls(void)
{
    /* FLS slots come from their own pool, not the 64 TLS slots */
    int tls_before = count_tls();
    DWORD many[100];
    int nmany = 0;
    while (nmany < 100 && (many[nmany] = FlsAlloc(NULL)) != FLS_OUT_OF_INDEXES) nmany++;
    CHECK("100 FLS slots", nmany == 100);
    CHECK("FLS slots leave the TLS slots alone", count_tls() == tls_before);
    for (int i = 0; i < nmany; i++) FlsFree(many[i]);

    /* a thread's value gets the callback when the thread ends, on that thread */
    g_idx = FlsAlloc(fls_cb);
    CHECK("FlsAlloc", g_idx != FLS_OUT_OF_INDEXES);
    HANDLE th[4];
    for (int i = 0; i < 4; i++) th[i] = CreateThread(NULL, 0, exiting, &g_rec[i], 0, NULL);
    WaitForMultipleObjects(4, th, TRUE, INFINITE);
    int ok = 1;
    for (int i = 0; i < 4; i++) {
        DWORD code = 1;
        GetExitCodeThread(th[i], &code);
        ok &= code == 0 && g_rec[i].freed == 1 && g_rec[i].freed_on == g_rec[i].tid;
        CloseHandle(th[i]);
    }
    CHECK("thread exit runs the FLS callback once, on the exiting thread", ok && g_calls == 4);

    /* FlsFree hands every live thread's value to the callback */
    g_calls = 0;
    g_set = CreateSemaphoreW(NULL, 0, 2, NULL);
    g_go = CreateEventW(NULL, TRUE, FALSE, NULL);
    th[0] = CreateThread(NULL, 0, parked, &g_rec[4], 0, NULL);
    th[1] = CreateThread(NULL, 0, parked, &g_rec[5], 0, NULL);
    WaitForSingleObject(g_set, INFINITE);
    WaitForSingleObject(g_set, INFINITE);
    Rec mine = { GetCurrentThreadId(), 0, 0 };
    FlsSetValue(g_idx, &mine);
    CHECK("FlsFree", FlsFree(g_idx));
    CHECK("FlsFree calls back for every thread's value",
          g_calls == 3 && mine.freed == 1 && g_rec[4].freed == 1 && g_rec[5].freed == 1);
    SetEvent(g_go);
    WaitForMultipleObjects(2, th, TRUE, INFINITE);
    DWORD c0 = 1, c1 = 1;
    GetExitCodeThread(th[0], &c0);
    GetExitCodeThread(th[1], &c1);
    CHECK("a freed slot reads NULL and is not called back again at exit", c0 == 0 && c1 == 0 && g_calls == 3);
    CloseHandle(th[0]); CloseHandle(th[1]);
    CHECK("a freed index is invalid", !FlsSetValue(g_idx, &mine) && GetLastError() == ERROR_INVALID_PARAMETER);

    /* each fiber has its own values; deleting a fiber calls back for its own */
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    ConvertThreadToFiber_ = (void *)GetProcAddress(k32, "ConvertThreadToFiber");
    ConvertFiberToThread_ = (void *)GetProcAddress(k32, "ConvertFiberToThread");
    CreateFiber_ = (void *)GetProcAddress(k32, "CreateFiber");
    SwitchToFiber_ = (void *)GetProcAddress(k32, "SwitchToFiber");
    DeleteFiber_ = (void *)GetProcAddress(k32, "DeleteFiber");
    g_calls = 0;
    g_fidx = FlsAlloc(fls_cb);
    Rec mainrec = { GetCurrentThreadId(), 0, 0 };
    g_main_fiber = ConvertThreadToFiber_(NULL);
    FlsSetValue(g_fidx, &mainrec);
    void *f = CreateFiber_(0, fiber_main, NULL);
    SwitchToFiber_(f);
    CHECK("a new fiber starts with no FLS values", g_fiber_saw);
    CHECK("the fiber's value is not the thread's", FlsGetValue(g_fidx) == &mainrec);
    DeleteFiber_(f);
    CHECK("DeleteFiber calls back for the fiber's value only", g_frec.freed == 1 && mainrec.freed == 0 && g_calls == 1);
    ConvertFiberToThread_();
    FlsFree(g_fidx);
    CHECK("FlsFree calls back for the thread's value", mainrec.freed == 1);
}

/* ------------------------------------------------------------- locale */
typedef struct {
    const char *dll;
    int (__cdecl *cfg)(int);
    char *(__cdecl *setloc)(int, const char *);
    wchar_t *(__cdecl *wsetloc)(int, const wchar_t *);
    HANDLE step[3];
    volatile LONG bad;
} Crt;

static int is(const char *a, const char *b) { return a && !strcmp(a, b); }

static DWORD WINAPI own_locale(void *arg)
{
    Crt *c = arg;
    LONG bad = 0;
    bad |= (c->cfg(0) != 2) << 0;                   /* a thread starts on the process's locale */
    bad |= (c->cfg(1) != 2) << 1;
    bad |= !is(c->setloc(LC_ALL, NULL), "en-US") << 2;   /* its copy starts as the process's */
    bad |= !is(c->setloc(LC_ALL, "de-DE"), "de-DE") << 3;
    SetEvent(c->step[0]);
    WaitForSingleObject(c->step[1], INFINITE);      /* the main thread switches to fr-FR */
    bad |= !is(c->setloc(LC_ALL, NULL), "de-DE") << 4;
    wchar_t *w = c->wsetloc(LC_NUMERIC, NULL);
    bad |= !(w && !wcscmp(w, L"de-DE")) << 5;
    bad |= (c->cfg(0) != 1) << 6;
    bad |= (c->cfg(2) != 1) << 7;                   /* back to the process's */
    bad |= !is(c->setloc(LC_ALL, NULL), "fr-FR") << 8;
    bad |= (c->cfg(1) != 2) << 9;                   /* and its own again: a fresh copy */
    bad |= !is(c->setloc(LC_TIME, "ja-JP"), "ja-JP") << 10;
    c->bad = bad;
    SetEvent(c->step[2]);
    return 0;
}

static void test_locale(const char *dll)
{
    Crt c = { dll };
    HMODULE m = LoadLibraryA(dll);
    c.cfg = m ? (void *)GetProcAddress(m, "_configthreadlocale") : NULL;
    c.setloc = m ? (void *)GetProcAddress(m, "setlocale") : NULL;
    c.wsetloc = m ? (void *)GetProcAddress(m, "_wsetlocale") : NULL;
    CHECK(dll, c.cfg && c.setloc && c.wsetloc);
    if (!c.cfg || !c.setloc || !c.wsetloc) return;
    for (int i = 0; i < 3; i++) c.step[i] = CreateEventW(NULL, FALSE, FALSE, NULL);
    c.setloc(LC_ALL, "en-US");
    HANDLE t = CreateThread(NULL, 0, own_locale, &c, 0, NULL);
    WaitForSingleObject(c.step[0], INFINITE);
    CHECK("another thread's own locale leaves the process's alone", is(c.setloc(LC_ALL, NULL), "en-US"));
    CHECK("the main thread switches", is(c.setloc(LC_ALL, "fr-FR"), "fr-FR"));
    SetEvent(c.step[1]);
    WaitForSingleObject(c.step[2], INFINITE);
    if (c.bad) printf("%s: locale thread check bits %#lx\n", dll, (unsigned long)c.bad);
    CHECK("per-thread locale on its thread", c.bad == 0);
    CHECK("its LC_TIME stayed its own", is(c.setloc(LC_TIME, NULL), "fr-FR"));
    CHECK("_configthreadlocale rejects other values", c.cfg(5) == -1);
    CHECK("the main thread uses the process's locale", c.cfg(0) == 2);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    for (int i = 0; i < 3; i++) CloseHandle(c.step[i]);
    c.setloc(LC_ALL, "C");
}

/* ------------------------------------------------------------- getenv */
#define ROUNDS 1500
static volatile LONG g_stop, g_torn;

static int uniform(const char *s, size_t *len)
{
    size_t n = strlen(s);
    for (size_t i = 1; i < n; i++) if (s[i] != s[0]) return 0;
    if (len) *len = n;
    return n > 0;
}
static int wuniform(const wchar_t *s)
{
    size_t n = wcslen(s);
    for (size_t i = 1; i < n; i++) if (s[i] != s[0]) return 0;
    return n > 0;
}

static DWORD WINAPI env_writer(void *arg)
{
    (void)arg;
    static char v[1201];
    for (int k = 0; k < ROUNDS; k++) {
        size_t n = 1 + (size_t)(k * 37) % 1200;      /* past the old 512-byte limit too */
        memset(v, 'a' + k % 26, n);
        v[n] = 0;
        SetEnvironmentVariableA("CRTTHREADS_VAR", v);
    }
    g_stop = 1;
    return 0;
}

static DWORD WINAPI env_reader(void *arg)
{
    int wide = (int)(INT_PTR)arg;
    const char *keep = NULL;
    const wchar_t *wkeep = NULL;
    while (!g_stop) {
        if (wide) {
            const wchar_t *w = _wgetenv(L"CRTTHREADS_VAR");
            if (!w || !wuniform(w) || (wkeep && !wuniform(wkeep))) InterlockedIncrement(&g_torn);
            wkeep = w;
        } else {
            const char *s = getenv("CRTTHREADS_VAR");
            if (!s || !uniform(s, NULL) || (keep && !uniform(keep, NULL))) InterlockedIncrement(&g_torn);
            keep = s;                               /* an earlier result must stay intact */
        }
    }
    return 0;
}

static void test_getenv(void)
{
    SetEnvironmentVariableA("CRTTHREADS_VAR", "zzzz");
    HANDLE th[4];
    th[0] = CreateThread(NULL, 0, env_reader, (void *)0, 0, NULL);
    th[1] = CreateThread(NULL, 0, env_reader, (void *)0, 0, NULL);
    th[2] = CreateThread(NULL, 0, env_reader, (void *)1, 0, NULL);
    th[3] = CreateThread(NULL, 0, env_writer, NULL, 0, NULL);
    WaitForMultipleObjects(4, th, TRUE, INFINITE);
    for (int i = 0; i < 4; i++) CloseHandle(th[i]);
    if (g_torn) printf("getenv: %ld torn or missing results\n", (long)g_torn);
    CHECK("getenv/_wgetenv results stay whole while another thread changes the variable", g_torn == 0);

    static char big[3001];
    memset(big, 'q', 3000);
    SetEnvironmentVariableA("CRTTHREADS_BIG", big);
    size_t n = 0;
    const char *s = getenv("CRTTHREADS_BIG");
    CHECK("getenv returns a 3000-byte value", s && uniform(s, &n) && n == 3000);
    const char *again = getenv("CRTTHREADS_BIG");
    CHECK("an unchanged value comes back as the same copy", again == s);
    const wchar_t *w = _wgetenv(L"CRTTHREADS_BIG");
    CHECK("_wgetenv returns it too", w && wcslen(w) == 3000);
    CHECK("a missing variable is NULL", !getenv("CRTTHREADS_NONE") && !_wgetenv(L"CRTTHREADS_NONE"));
}

int main(void)
{
    test_fls();
    test_locale("msvcrt.dll");
    test_locale("ucrtbase.dll");
    test_getenv();
    printf("crtthreads: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
