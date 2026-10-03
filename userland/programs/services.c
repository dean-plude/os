/*
 * services.exe — starts the automatic services at boot
 *
 *   services /autostart     (the desktop runs this once it is up)
 *   services                (lists the services and their state)
 *
 * NovaOS's service control manager lives in advapi32 (service.c): a
 * service is its key under HKLM\SYSTEM\CurrentControlSet\Services, and
 * nothing is running when the machine starts.  /autostart first marks
 * every service stopped (what the registry says about processes of the
 * last boot is stale), then starts each service whose Start is 2
 * (SERVICE_AUTO_START), after the services it depends on (DependOnService,
 * started whatever their own start type, as Windows does), and the ones
 * marked DelayedAutoStart last.  Each result goes to the kernel log.
 */
#include <windows.h>
#include <winsvc.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define SERVICES_KEY L"SYSTEM\\CurrentControlSet\\Services"
#define MAX_SVC      256

typedef struct {
    WCHAR name[128];
    DWORD start, type;
    bool  delayed, tried, ok;
    WCHAR deps[512];              /* DependOnService: names, each ended by 0, then 0 */
} Svc;

static Svc g_svc[MAX_SVC];
static int g_n;

static void say(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    char out[530];
    snprintf(out, sizeof(out), "[SVC] %s\n", line);
    OutputDebugStringA(out);
    fputs(out + 6, stdout);
}

static DWORD get_dword(HKEY k, const WCHAR *v, DWORD def)
{
    DWORD d, size = sizeof(d), type;
    return RegQueryValueExW(k, v, NULL, &type, (BYTE *)&d, &size) || type != REG_DWORD ? def : d;
}

static void set_dword(HKEY k, const WCHAR *v, DWORD d) { RegSetValueExW(k, v, 0, REG_DWORD, (const BYTE *)&d, sizeof(d)); }

static void load(bool reset)
{
    HKEY root;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, SERVICES_KEY, 0, KEY_READ, &root)) return;
    for (DWORD i = 0; g_n < MAX_SVC; i++) {
        WCHAR name[128];
        DWORD nl = 128;
        if (RegEnumKeyExW(root, i, name, &nl, NULL, NULL, NULL, NULL)) break;
        HKEY k;
        if (RegOpenKeyExW(root, name, 0, KEY_ALL_ACCESS, &k)) continue;
        Svc *s = &g_svc[g_n++];
        memset(s, 0, sizeof(*s));
        wcsncpy(s->name, name, 127);
        s->start = get_dword(k, L"Start", 3);
        s->type = get_dword(k, L"Type", 0x10);
        s->delayed = get_dword(k, L"DelayedAutoStart", 0) != 0;
        DWORD size = sizeof(s->deps) - 4, type;
        if (RegQueryValueExW(k, L"DependOnService", NULL, &type, (BYTE *)s->deps, &size) || type != REG_MULTI_SZ)
            s->deps[0] = 0;
        s->deps[size / 2] = 0;
        s->deps[size / 2 + 1] = 0;
        if (reset) {                      /* nothing runs yet in this boot */
            set_dword(k, L"NovaState", SERVICE_STOPPED);
            set_dword(k, L"NovaPid", 0);
        }
        RegCloseKey(k);
    }
    RegCloseKey(root);
}

static Svc *find(const WCHAR *name)
{
    for (int i = 0; i < g_n; i++) if (!_wcsicmp(g_svc[i].name, name)) return &g_svc[i];
    return NULL;
}

static bool start(SC_HANDLE scm, Svc *s, int depth)
{
    if (s->tried) return s->ok;
    s->tried = true;
    char n8[128];
    WideCharToMultiByte(CP_UTF8, 0, s->name, -1, n8, sizeof(n8), NULL, NULL);
    if (depth > 16) { say("%s: dependency loop", n8); return false; }
    for (const WCHAR *d = s->deps; *d; d += wcslen(d) + 1) {
        Svc *dep = find(d);
        if (!dep || dep->start == SERVICE_DISABLED || !start(scm, dep, depth + 1)) {
            char d8[128];
            WideCharToMultiByte(CP_UTF8, 0, d, -1, d8, sizeof(d8), NULL, NULL);
            say("%s: not started, the service it needs (%s) did not start", n8, d8);
            return false;
        }
    }
    SC_HANDLE h = OpenServiceW(scm, s->name, SERVICE_START | SERVICE_QUERY_STATUS);
    if (!h) { say("%s: cannot open it (error %lu)", n8, GetLastError()); return false; }
    s->ok = StartServiceW(h, 0, NULL) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
    if (s->ok) say("%s: started", n8);
    else say("%s: did not start (error %lu)", n8, GetLastError());
    CloseServiceHandle(h);
    return s->ok;
}

static int autostart(void)
{
    load(true);
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) { say("no service control manager (error %lu)", GetLastError()); return 1; }
    int want = 0, ok = 0;
    for (int pass = 0; pass < 2; pass++)       /* the delayed ones after the rest */
        for (int i = 0; i < g_n; i++) {
            Svc *s = &g_svc[i];
            if (s->start != SERVICE_AUTO_START || !(s->type & (SERVICE_WIN32_OWN_PROCESS | SERVICE_WIN32_SHARE_PROCESS))) continue;
            if (s->delayed != (pass == 1)) continue;
            want++;
            if (start(scm, s, 0)) ok++;
        }
    CloseServiceHandle(scm);
    say("Started %d of %d automatic services", ok, want);
    return ok == want ? 0 : 1;
}

static int list(void)
{
    load(false);
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    static const char *states[] = { "?", "stopped", "starting", "stopping", "running", "continuing", "pausing", "paused" };
    static const char *starts[] = { "boot", "system", "automatic", "manual", "disabled" };
    for (int i = 0; i < g_n; i++) {
        SERVICE_STATUS st;
        memset(&st, 0, sizeof(st));
        SC_HANDLE h = scm ? OpenServiceW(scm, g_svc[i].name, SERVICE_QUERY_STATUS) : NULL;
        if (h) { QueryServiceStatus(h, &st); CloseServiceHandle(h); }
        printf("%-32ls %-9s %s%s\n", g_svc[i].name, st.dwCurrentState < 8 ? states[st.dwCurrentState] : "?",
               g_svc[i].start < 5 ? starts[g_svc[i].start] : "?", g_svc[i].delayed ? " (delayed)" : "");
    }
    if (scm) CloseServiceHandle(scm);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && (!_stricmp(argv[1], "/autostart") || !_stricmp(argv[1], "-autostart"))) return autostart();
    return list();
}
