/* errnotest.exe — the C runtime's per-thread data: errno, _doserrno,
 * rand's seed, strtok's position and gmtime's buffer belong to the thread
 * that set them, in msvcrt.dll and in ucrtbase.dll; a new thread starts
 * with errno 0, and reading errno leaves GetLastError alone */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <windows.h>

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

__declspec(dllimport) unsigned long *__doserrno(void);
__declspec(dllimport) int _get_errno(int *v);
__declspec(dllimport) int _set_errno(int v);

#define NTHREADS 6
static HANDLE g_ready[NTHREADS], g_go;
static int (*ucrt_errno_set)(int);
static int *(*ucrt_errno)(void);
static volatile LONG g_bad[NTHREADS];

/* each thread fails in its own way, waits until every thread has, then
 * checks that it still reads its own errno */
static DWORD WINAPI worker(void *arg)
{
    int i = (int)(INT_PTR)arg, bad = 0, want, got = -1;
    bad |= errno != 0;                                  /* a fresh thread: errno 0 */
    bad |= (*__doserrno() != 0) << 1;
    bad |= (*ucrt_errno() != 0) << 2;
    bad |= (rand() != 41) << 3;                         /* rand seeded with 1 */
    switch (i % 3) {
    case 0:                                             /* a failing open: ENOENT, Win32 error 2 or 3 */
        bad |= (fopen("C:\\no\\such\\dir\\file.txt", "r") != NULL) << 10;
        want = ENOENT;
        bad |= (*__doserrno() != ERROR_FILE_NOT_FOUND && *__doserrno() != ERROR_PATH_NOT_FOUND) << 4;
        break;
    case 1:                                             /* an overflowing conversion: ERANGE */
        strtol("99999999999999999999999", NULL, 10);
        want = ERANGE;
        break;
    default:
        _set_errno(100 + i);
        want = 100 + i;
        break;
    }
    ucrt_errno_set(200 + i);                            /* ucrtbase keeps its own errno, also per thread */
    char text[] = "a,b,c", *tok = strtok(text, ",");    /* strtok's position is per thread too */
    time_t t = (time_t)i * 86400;
    struct tm *tm = gmtime(&t);
    SetEvent(g_ready[i]);
    WaitForSingleObject(g_go, INFINITE);
    _get_errno(&got);
    bad |= (errno != want || got != want) << 5;
    bad |= (*ucrt_errno() != 200 + i) << 6;
    bad |= (!tok || strcmp(tok, "a") || !(tok = strtok(NULL, ",")) || strcmp(tok, "b")) << 7;
    bad |= (tm->tm_mday != 1 + i) << 8;                 /* nobody else's gmtime overwrote it */
    SetLastError(0x1234);
    errno = EINVAL;
    bad |= (GetLastError() != 0x1234) << 9;             /* errno lookups keep GetLastError */
    g_bad[i] = bad;
    return 0;
}

static DWORD WINAPI fresh(void *arg) { (void)arg; return (DWORD)(errno | *ucrt_errno() | *__doserrno()); }

int main(void)
{
    HMODULE ucrt = LoadLibraryW(L"ucrtbase.dll");
    ucrt_errno = ucrt ? (int *(*)(void))GetProcAddress(ucrt, "_errno") : NULL;
    ucrt_errno_set = ucrt ? (int (*)(int))GetProcAddress(ucrt, "_set_errno") : NULL;
    CHECK("ucrtbase exports _errno and _set_errno", ucrt_errno && ucrt_errno_set);
    if (!ucrt_errno || !ucrt_errno_set) { printf("errnotest: %d passed, %d failed\n", pass, fail); return 1; }

    errno = EDOM;
    *ucrt_errno() = EILSEQ;
    srand(12345);
    rand();
    char text[] = "x;y;z";
    strtok(text, ";");
    g_go = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE th[NTHREADS];
    for (int i = 0; i < NTHREADS; i++) {
        g_ready[i] = CreateEventW(NULL, TRUE, FALSE, NULL);
        th[i] = CreateThread(NULL, 0, worker, (void *)(INT_PTR)i, 0, NULL);
    }
    WaitForMultipleObjects(NTHREADS, g_ready, TRUE, INFINITE);
    SetEvent(g_go);
    WaitForMultipleObjects(NTHREADS, th, TRUE, INFINITE);
    for (int i = 0; i < NTHREADS; i++) {
        if (g_bad[i]) printf("thread %d: check bits %#lx\n", i, (unsigned long)g_bad[i]);
        CHECK("each thread saw only its own runtime state", g_bad[i] == 0);
        CloseHandle(th[i]);
        CloseHandle(g_ready[i]);
    }
    CHECK("main thread's errno untouched", errno == EDOM);
    CHECK("main thread's ucrtbase errno untouched", *ucrt_errno() == EILSEQ);
    char *tok = strtok(NULL, ";");
    CHECK("main thread's strtok position untouched", tok && !strcmp(tok, "y"));

    /* threads that come after the others ended start clean, many times over
     * (their blocks are freed at thread exit and fresh ones made) */
    int dirty = 0;
    for (int i = 0; i < 64; i++) {
        DWORD code = 1;
        HANDLE h = CreateThread(NULL, 0, fresh, NULL, 0, NULL);
        WaitForSingleObject(h, INFINITE);
        GetExitCodeThread(h, &code);
        CloseHandle(h);
        dirty += code != 0;
    }
    CHECK("64 later threads start with errno 0", dirty == 0);

    printf("errnotest: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
