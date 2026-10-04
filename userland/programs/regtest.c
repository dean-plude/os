/* regtest.exe — registry keys opened, closed and deleted side by side.
 *
 * Threads of this process and of child processes it starts (four waves of
 * four) work on the same keys under HKCU\Software\NovaRegtest at once:
 * open and close a shared key, create and delete their own subkeys, delete
 * keys others hold open (those handles then answer ERROR_KEY_DELETED),
 * duplicate key handles within the process and into a child, leave change
 * notifications pending on keys they close, set, read and delete the
 * values of one key at once (as the service control manager's state
 * values are), and enumerate while others add and remove subkeys, while
 * one more thread waits for changes in the kernel (synchronously).  The
 * children inherit a key handle and end with keys still open and watches
 * still pending, so the kernel closes those as their threads close their
 * own.  Nothing may crash; afterwards the shared key must still be whole.
 *
 * "regtest child ROUNDS HANDLE" is a child's side. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define BASE    "Software\\NovaRegtest"
#define THREADS 6
#define CHILDREN 4
#define WAVES   4

static int pass, fail;
static volatile LONG errors;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

static HKEY g_base;
static int g_rounds = 300;

/* An unexpected result inside a worker: counted, the first few printed */
static void bad(const char *what, LONG r, int line)
{
    if (InterlockedIncrement(&errors) <= 10) printf("FAIL: %s: %ld (line %d)\n", what, r, line);
}
#define WANT(what, cond, r) do { if (!(cond)) bad(what, (LONG)(r), __LINE__); } while (0)

static void shared_once(void)
{
    HKEY k;
    LONG r = RegOpenKeyExA(g_base, "Shared", 0, KEY_ALL_ACCESS, &k);
    WANT("open Shared", r == ERROR_SUCCESS, r);
    if (r) return;
    DWORD v = 0, n = sizeof(v), type;
    r = RegQueryValueExA(k, "magic", 0, &type, (BYTE *)&v, &n);
    WANT("Shared's value", r == ERROR_SUCCESS && v == 0x5A5A1234, r);
    RegCloseKey(k);
}

/* A subkey only this thread uses: made, given a value, closed, deleted */
static void own_once(int id, int i)
{
    char name[64];
    HKEY k;
    sprintf(name, "T%lu-%d\\K%d", GetCurrentProcessId(), id, i % 4);
    LONG r = RegCreateKeyExA(g_base, name, 0, 0, REG_OPTION_VOLATILE, KEY_ALL_ACCESS, 0, &k, 0);
    WANT("create own key", r == ERROR_SUCCESS, r);
    if (r) return;
    r = RegSetValueExA(k, "v", 0, REG_DWORD, (const BYTE *)&i, 4);
    WANT("set own value", r == ERROR_SUCCESS, r);
    RegCloseKey(k);
    r = RegDeleteKeyA(g_base, name);
    WANT("delete own key", r == ERROR_SUCCESS, r);
}

/* Keys several threads (and processes) open while others delete them */
static void victim_once(int i)
{
    char name[32];
    HKEY k;
    sprintf(name, "Victim%d", i % 3);
    LONG r = RegCreateKeyExA(g_base, name, 0, 0, REG_OPTION_VOLATILE, KEY_ALL_ACCESS, 0, &k, 0);
    WANT("create victim", r == ERROR_SUCCESS, r);
    if (r) return;
    DWORD v = (DWORD)i;
    r = RegSetValueExA(k, "v", 0, REG_DWORD, (const BYTE *)&v, 4);
    WANT("set victim value", r == ERROR_SUCCESS || r == ERROR_KEY_DELETED, r);
    if (i & 1) {
        r = RegDeleteKeyA(g_base, name);                    /* (someone else may have) */
        WANT("delete victim", r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND || r == ERROR_KEY_DELETED, r);
        DWORD n = sizeof(v);
        r = RegQueryValueExA(k, "v", 0, 0, (BYTE *)&v, &n);
        WANT("query a deleted key", r == ERROR_KEY_DELETED || r == ERROR_SUCCESS, r);
    }
    RegCloseKey(k);
}

/* A watch left pending on a key whose handle is closed, then fired */
static void watch_once(void)
{
    HKEY k;
    LONG r = RegOpenKeyExA(g_base, "Shared", 0, KEY_ALL_ACCESS, &k);
    WANT("open Shared to watch", r == ERROR_SUCCESS, r);
    if (r) return;
    HANDLE ev = CreateEventA(0, TRUE, FALSE, 0);
    r = RegNotifyChangeKeyValue(k, TRUE, REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_CHANGE_NAME, ev, TRUE);
    WANT("watch Shared", r == ERROR_SUCCESS, r);
    RegCloseKey(k);
    CloseHandle(ev);
    if (RegOpenKeyExA(g_base, "Shared", 0, KEY_ALL_ACCESS, &k) == ERROR_SUCCESS) {
        DWORD v = 0x5A5A1234;
        RegSetValueExA(k, "magic", 0, REG_DWORD, (const BYTE *)&v, 4);
        RegCloseKey(k);
    }
}

/* A key handle duplicated, the copies closed in either order */
static void dup_once(int i)
{
    HKEY k, d;
    LONG r = RegOpenKeyExA(g_base, "Shared", 0, KEY_READ, &k);
    WANT("open Shared to duplicate", r == ERROR_SUCCESS, r);
    if (r) return;
    BOOL ok = DuplicateHandle(GetCurrentProcess(), k, GetCurrentProcess(), (HANDLE *)&d, 0, FALSE, DUPLICATE_SAME_ACCESS);
    WANT("duplicate a key handle", ok, GetLastError());
    if (i & 1) { RegCloseKey(k); if (ok) RegCloseKey(d); }
    else { if (ok) RegCloseKey(d); RegCloseKey(k); }
}

/* Values of one key set, read and deleted by everyone at once, as the
 * service control manager's state values are */
static void values_once(int id, int i)
{
    HKEY k;
    LONG r = RegOpenKeyExA(g_base, "Shared", 0, KEY_ALL_ACCESS, &k);
    WANT("open Shared for values", r == ERROR_SUCCESS, r);
    if (r) return;
    char name[32], big[300];
    sprintf(name, "s%lu-%d", GetCurrentProcessId(), id);
    memset(big, 'a' + i % 26, sizeof(big));
    DWORD v = (DWORD)i, n;
    r = RegSetValueExA(k, name, 0, REG_DWORD, (const BYTE *)&v, 4);
    WANT("set a value", r == ERROR_SUCCESS, r);
    r = RegSetValueExA(k, "common", 0, REG_BINARY, (const BYTE *)big, 1 + (DWORD)(i * 37 % (int)sizeof(big)));
    WANT("set the common value", r == ERROR_SUCCESS, r);
    n = sizeof(big);
    r = RegQueryValueExA(k, "common", 0, 0, (BYTE *)big, &n);
    WANT("read the common value", r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND, r);
    char vn[64];
    DWORD vl = sizeof(vn);
    RegEnumValueA(k, (DWORD)(i % 4), vn, &vl, 0, 0, 0, 0);
    r = RegDeleteValueA(k, name);
    WANT("delete a value", r == ERROR_SUCCESS, r);
    if (i % 3 == 0) {
        r = RegDeleteValueA(k, "common");
        WANT("delete the common value", r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND, r);
    }
    RegCloseKey(k);
}

/* Subkeys listed and opened while others come and go */
static void enum_once(void)
{
    char name[256];
    for (DWORD i = 0;; i++) {
        DWORD n = sizeof(name);
        if (RegEnumKeyExA(g_base, i, name, &n, 0, 0, 0, 0) != ERROR_SUCCESS) break;
        HKEY k;
        if (RegOpenKeyExA(g_base, name, 0, KEY_READ, &k) == ERROR_SUCCESS) {
            DWORD subkeys = 0;
            RegQueryInfoKeyA(k, 0, 0, 0, &subkeys, 0, 0, 0, 0, 0, 0, 0);
            RegCloseKey(k);
        }
    }
}

static DWORD WINAPI worker(LPVOID arg)
{
    int id = (int)(INT_PTR)arg;
    for (int i = 0; i < g_rounds; i++) {
        switch ((i + id) % 7) {
        case 0: shared_once(); break;
        case 1: own_once(id, i); break;
        case 2: victim_once(i + id); break;
        case 3: watch_once(); break;
        case 4: dup_once(i); break;
        case 5: enum_once(); break;
        case 6: values_once(id, i); break;
        }
    }
    return 0;
}

/* Waits for Shared to change (synchronously, in the kernel) again and
 * again while the workers change it */
static volatile LONG g_stop, g_sync_waits;
static DWORD WINAPI sync_watcher(LPVOID arg)
{
    (void)arg;
    HKEY k;
    if (RegOpenKeyExA(g_base, "Shared", 0, KEY_ALL_ACCESS, &k)) return 1;
    while (!g_stop) {
        LONG r = RegNotifyChangeKeyValue(k, TRUE, REG_NOTIFY_CHANGE_LAST_SET, 0, FALSE);
        WANT("wait for a change", r == ERROR_SUCCESS, r);
        if (r) break;
        InterlockedIncrement(&g_sync_waits);
    }
    RegCloseKey(k);
    return 0;
}

static void run_threads(int n)
{
    HANDLE t[THREADS + 1];
    g_stop = 0;
    t[n] = CreateThread(0, 0, sync_watcher, 0, 0, 0);
    for (int i = 0; i < n; i++) t[i] = CreateThread(0, 0, worker, (LPVOID)(INT_PTR)i, 0, 0);
    WaitForMultipleObjects((DWORD)n, t, TRUE, INFINITE);
    g_stop = 1;
    HKEY k;                                                 /* (one more change wakes the watcher) */
    while (WaitForSingleObject(t[n], 50) == WAIT_TIMEOUT)
        if (!RegOpenKeyExA(g_base, "Shared", 0, KEY_ALL_ACCESS, &k)) {
            DWORD v = 0x5A5A1234;
            RegSetValueExA(k, "magic", 0, REG_DWORD, (const BYTE *)&v, 4);
            RegCloseKey(k);
        }
    for (int i = 0; i <= n; i++) CloseHandle(t[i]);
}

/* A child: its threads work as the parent's do; it closes the handle it
 * inherited from another thread meanwhile, and ends with keys open and
 * watches pending for the kernel to clean up */
static int child(int argc, char **argv)
{
    g_rounds = argc > 2 ? atoi(argv[2]) : 100;
    HKEY inherited = argc > 3 ? (HKEY)(INT_PTR)strtoull(argv[3], 0, 16) : 0;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, BASE, 0, KEY_ALL_ACCESS, &g_base)) return 2;
    HANDLE t[THREADS];
    for (int i = 0; i < THREADS - 2; i++) t[i] = CreateThread(0, 0, worker, (LPVOID)(INT_PTR)(i + 10), 0, 0);
    DWORD v = 0, n = sizeof(v);
    int ok = inherited && RegQueryValueExA(inherited, "magic", 0, 0, (BYTE *)&v, &n) == ERROR_SUCCESS && v == 0x5A5A1234;
    if (inherited) RegCloseKey(inherited);
    WaitForMultipleObjects(THREADS - 2, t, TRUE, INFINITE);
    HKEY left[4];
    for (int i = 0; i < 4; i++) {
        if (RegOpenKeyExA(g_base, i & 1 ? "Shared" : "Victim0", 0, KEY_ALL_ACCESS, &left[i])) continue;
        HANDLE ev = CreateEventA(0, TRUE, FALSE, 0);
        RegNotifyChangeKeyValue(left[i], TRUE, REG_NOTIFY_CHANGE_LAST_SET, ev, TRUE);
    }
    ExitProcess(ok && !errors ? 0 : 1);                     /* keys, events and watches still open */
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "child")) return child(argc, argv);
    if (argc > 1) g_rounds = atoi(argv[1]);

    RegDeleteTreeA(HKEY_CURRENT_USER, BASE);
    CHECK("create the test key", !RegCreateKeyExA(HKEY_CURRENT_USER, BASE, 0, 0, REG_OPTION_VOLATILE, KEY_ALL_ACCESS, 0, &g_base, 0));
    HKEY shared;
    CHECK("create Shared", !RegCreateKeyExA(g_base, "Shared", 0, 0, REG_OPTION_VOLATILE, KEY_ALL_ACCESS, 0, &shared, 0));
    DWORD magic = 0x5A5A1234;
    CHECK("set Shared's value", !RegSetValueExA(shared, "magic", 0, REG_DWORD, (const BYTE *)&magic, 4));

    /* One process, several threads */
    run_threads(THREADS);
    CHECK("threads: no unexpected results", errors == 0);
    CHECK("synchronous change waits came back", g_sync_waits > 0);
    printf("regtest: %d threads, %d rounds each\n", THREADS, g_rounds);

    /* Several processes: children inherit a key handle; this process's
     * threads keep working while they run and end */
    char exe[MAX_PATH];
    GetModuleFileNameA(0, exe, sizeof(exe));
    HKEY inh;
    CHECK("open an inheritable key", !RegOpenKeyExA(g_base, "Shared", 0, KEY_ALL_ACCESS, &inh));
    CHECK("make it inheritable", SetHandleInformation(inh, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT));
    int started = 0, placed = 0, clean = 0;
    for (int wave = 0; wave < WAVES; wave++) {
        PROCESS_INFORMATION pi[CHILDREN];
        for (int i = 0; i < CHILDREN; i++) {
            char cmd[MAX_PATH + 64];
            sprintf(cmd, "\"%s\" child %d %llx", exe, g_rounds / 2, (unsigned long long)(INT_PTR)inh);
            STARTUPINFOA si = { sizeof(si) };
            if (!CreateProcessA(exe, cmd, 0, 0, TRUE, CREATE_SUSPENDED, 0, 0, &si, &pi[i])) {
                pi[i].hProcess = 0;
                printf("FAIL: CreateProcess: %lu\n", GetLastError());
                continue;
            }
            started++;
            HANDLE d;                                       /* a copy the child never closes */
            if (DuplicateHandle(GetCurrentProcess(), inh, pi[i].hProcess, &d, 0, FALSE, DUPLICATE_SAME_ACCESS)) placed++;
            ResumeThread(pi[i].hThread);
        }
        run_threads(THREADS);
        for (int i = 0; i < CHILDREN; i++) {
            if (!pi[i].hProcess) continue;
            DWORD code = 99;
            WaitForSingleObject(pi[i].hProcess, INFINITE);
            GetExitCodeProcess(pi[i].hProcess, &code);
            if (code) printf("FAIL: child %d of wave %d exited with %lu\n", i, wave, code);
            else clean++;
            CloseHandle(pi[i].hProcess);
            CloseHandle(pi[i].hThread);
        }
    }
    RegCloseKey(inh);
    CHECK("start the children", started == WAVES * CHILDREN);
    CHECK("duplicate a key into each child", placed == WAVES * CHILDREN);
    CHECK("the children's keys", clean == WAVES * CHILDREN);
    CHECK("processes: no unexpected results", errors == 0);
    printf("regtest: %d child processes, %d rounds each\n", WAVES * CHILDREN, g_rounds / 2);

    /* The tree is still whole */
    DWORD v = 0, n = sizeof(v);
    CHECK("Shared's value after", !RegQueryValueExA(shared, "magic", 0, 0, (BYTE *)&v, &n) && v == magic);
    RegCloseKey(shared);
    RegCloseKey(g_base);
    CHECK("delete the test keys", !RegDeleteTreeA(HKEY_CURRENT_USER, BASE));
    HKEY gone;
    CHECK("the test keys are gone", RegOpenKeyExA(HKEY_CURRENT_USER, BASE, 0, KEY_READ, &gone) == ERROR_FILE_NOT_FOUND);
    printf("regtest: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
