/*
 * chrometest.exe — what Chromium's browser process needs from Windows to
 * start its GPU, network and page processes (Steam's built-in browser,
 * Edge's WebView2 and every CEF program are Chromium)
 *
 *   chrometest              run the tests
 *   chrometest child H E N  (child) the read-only shared memory handle H
 *                           (decimal), inherited: it must not give write
 *                           access, and it must read what the parent wrote;
 *                           E, an inheritable event the parent left out of
 *                           the handle list (named N), must not be ours
 *
 * Shared memory: Chromium makes an unnamed section whose descriptor has an
 * empty DACL, keeps a writable handle and hands children a read-only one
 * (DuplicateHandle with FILE_MAP_READ).  A child tells which it got by
 * asking DuplicateHandle for FILE_MAP_WRITE: more than the handle was
 * granted is an open of the section, checked against its descriptor, so
 * the read-only handle must fail and the writable one succeed.  A section
 * made without a descriptor lets anything through.  NtQueryObject reports
 * each handle's own GrantedAccess, of sections and files (the browser
 * CHECKs that a file handle it is handed read-only has no write right).
 * Exports by ordinal: Chromium imports some functions by number only
 * (shlwapi IsOS is 437; uxtheme 47 is DrawThemeBackgroundEx), and
 * delay-loads CallNtPowerInformation from api-ms-win-power-base.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass, g_fail;

static void check(int ok, const char *what)
{
    if (ok) g_pass++;
    else { g_fail++; printf("FAIL: %s (error %lu)\n", what, GetLastError()); }
}

#define TEXT_IN_IT "shared with a child"

/* Can @h be duplicated with FILE_MAP_WRITE?  (Chromium's
 * CheckPlatformHandlePermissionsCorrespondToMode) */
static BOOL dup_for_write(HANDLE h)
{
    HANDLE d = 0;
    BOOL ok = DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &d, FILE_MAP_WRITE, FALSE, 0);
    if (ok) CloseHandle(d);
    return ok;
}

static int child(HANDLE ro, HANDLE left_out, const char *name)
{
    int bad = 0;
    HANDLE ev = OpenEventA(SYNCHRONIZE, FALSE, name);
    if (!ev || CompareObjectHandles(left_out, ev)) bad |= 8;    /* only the listed handles came along */
    if (ev) CloseHandle(ev);
    if (dup_for_write(ro)) bad |= 1;                            /* a read-only handle stays read-only */
    else if (GetLastError() != ERROR_ACCESS_DENIED) bad |= 2;
    const char *v = MapViewOfFile(ro, FILE_MAP_READ, 0, 0, 0);
    if (!v || strcmp(v, TEXT_IN_IT)) bad |= 4;                  /* and maps for reading */
    if (v) UnmapViewOfFile(v);
    return bad;
}

typedef LONG (WINAPI *NtQuerySectionFn)(HANDLE, int, PVOID, SIZE_T, PSIZE_T);

static void shared_memory(void)
{
    /* Named, so the empty DACL can be seen refusing an open by name too
     * (Chromium's own are unnamed: the checks below hold for both) */
    SECURITY_DESCRIPTOR sd;
    ACL dacl;
    InitializeAcl(&dacl, sizeof(dacl), ACL_REVISION);
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, &dacl, FALSE);
    SECURITY_ATTRIBUTES sa = { sizeof(sa), &sd, FALSE };
    char name[64];
    snprintf(name, sizeof(name), "CrSharedMem_chrometest_%lu", GetCurrentProcessId());
    HANDLE all = CreateFileMappingA(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, 65536, name);
    check(all != 0, "CreateFileMapping with an empty DACL");
    if (!all) return;
    HANDLE rw = 0, ro = 0;
    check(DuplicateHandle(GetCurrentProcess(), all, GetCurrentProcess(), &rw,
                          FILE_MAP_READ | FILE_MAP_WRITE | SECTION_QUERY, FALSE, 0), "a writable handle (fewer rights)");
    check(DuplicateHandle(GetCurrentProcess(), rw, GetCurrentProcess(), &ro, FILE_MAP_READ | SECTION_QUERY, FALSE, 0),
          "a read-only handle");
    CloseHandle(all);
    check(dup_for_write(rw), "the writable handle duplicates for FILE_MAP_WRITE");
    SetLastError(0);
    check(!dup_for_write(ro) && GetLastError() == ERROR_ACCESS_DENIED,
          "the read-only handle does not (ERROR_ACCESS_DENIED)");
    HANDLE same = 0;
    check(DuplicateHandle(GetCurrentProcess(), ro, GetCurrentProcess(), &same, 0, FALSE, DUPLICATE_SAME_ACCESS) &&
          !dup_for_write(same), "DUPLICATE_SAME_ACCESS keeps it read-only");
    if (same) CloseHandle(same);
    /* NtQueryObject (ObjectBasicInformation) reports each handle's own
     * GrantedAccess: the read-only section handle has no SECTION_MAP_WRITE */
    typedef LONG (WINAPI *NtQueryObjectFn)(HANDLE, int, PVOID, ULONG, PULONG);
    NtQueryObjectFn qo = (NtQueryObjectFn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryObject");
    ULONG bro[14] = { 0 }, brw[14] = { 0 };
    check(qo && qo(ro, 0, bro, sizeof(bro), 0) >= 0 && bro[1] == (FILE_MAP_READ | SECTION_QUERY),
          "NtQueryObject: the read-only handle's GrantedAccess is FILE_MAP_READ | SECTION_QUERY");
    check(qo && qo(rw, 0, brw, sizeof(brw), 0) >= 0 && (brw[1] & FILE_MAP_WRITE),
          "NtQueryObject: the writable handle's has FILE_MAP_WRITE");
    HANDLE r2 = 0;
    check(DuplicateHandle(GetCurrentProcess(), ro, GetCurrentProcess(), &r2, FILE_MAP_READ, FALSE, 0),
          "it duplicates for FILE_MAP_READ (no more than it has)");
    if (r2) CloseHandle(r2);
    SetLastError(0);
    HANDLE byname = OpenFileMappingA(FILE_MAP_READ, FALSE, name);
    check(!byname && GetLastError() == ERROR_ACCESS_DENIED, "the empty DACL refuses an open by name");
    if (byname) CloseHandle(byname);

    /* Not an image section (Chromium refuses those) */
    NtQuerySectionFn q = (NtQuerySectionFn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQuerySection");
    struct { PVOID base; ULONG attrs; LARGE_INTEGER size; } info;
    check(q && q(ro, 0, &info, sizeof(info), 0) == 0 && !(info.attrs & SEC_IMAGE) && info.size.QuadPart == 65536,
          "NtQuerySection: 64 KB, not SEC_IMAGE");

    char *v = MapViewOfFile(rw, FILE_MAP_WRITE, 0, 0, 0);
    check(v != 0, "map the writable handle");
    if (v) { strcpy(v, TEXT_IN_IT); UnmapViewOfFile(v); }

    /* The child gets the read-only handle the way Chromium passes it: an
     * inheritable copy, named in PROC_THREAD_ATTRIBUTE_HANDLE_LIST, its
     * value on the command line */
    HANDLE inh = 0;
    check(DuplicateHandle(GetCurrentProcess(), ro, GetCurrentProcess(), &inh, 0, TRUE, DUPLICATE_SAME_ACCESS),
          "an inheritable copy of it");
    SECURITY_ATTRIBUTES isa = { sizeof(isa), 0, TRUE };
    char evname[64];
    snprintf(evname, sizeof(evname), "chrometest-left-out-%lu", GetCurrentProcessId());
    HANDLE left_out = CreateEventA(&isa, TRUE, FALSE, evname);  /* inheritable, but not in the list */
    SIZE_T n = 0;
    InitializeProcThreadAttributeList(0, 1, 0, &n);
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = malloc(n);
    check(attrs && InitializeProcThreadAttributeList(attrs, 1, 0, &n) &&
          UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &inh, sizeof(inh), 0, 0),
          "a handle list naming it");
    STARTUPINFOEXA si;
    memset(&si, 0, sizeof(si));
    si.StartupInfo.cb = sizeof(si);
    si.lpAttributeList = attrs;
    char self[MAX_PATH], cl[MAX_PATH + 64];
    GetModuleFileNameA(0, self, sizeof(self));
    snprintf(cl, sizeof(cl), "\"%s\" child %llu %llu %s", self, (unsigned long long)(ULONG_PTR)inh,
             (unsigned long long)(ULONG_PTR)left_out, evname);
    PROCESS_INFORMATION pi;
    BOOL started = CreateProcessA(self, cl, 0, 0, TRUE, EXTENDED_STARTUPINFO_PRESENT, 0, 0, &si.StartupInfo, &pi);
    check(started, "CreateProcess with EXTENDED_STARTUPINFO_PRESENT");
    if (started) {
        DWORD code = (DWORD)-1;
        if (WaitForSingleObject(pi.hProcess, 20000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
        else TerminateProcess(pi.hProcess, 99);
        check(!(code & 1) && !(code & 2), "the child's inherited handle is read-only");
        check(!(code & 4) && code != (DWORD)-1, "the child reads what the parent wrote");
        check(!(code & 8) && code != (DWORD)-1, "an inheritable handle left out of the list stays behind");
        if (code) printf("  the child exited with %lu\n", code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    DeleteProcThreadAttributeList(attrs);
    free(attrs);
    CloseHandle(inh);
    CloseHandle(left_out);
    CloseHandle(ro);
    CloseHandle(rw);

    /* Unnamed, as Chromium makes them: the empty DACL holds all the same */
    HANDLE un = CreateFileMappingA(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, 4096, 0), unro = 0;
    check(un && DuplicateHandle(GetCurrentProcess(), un, GetCurrentProcess(), &unro, FILE_MAP_READ, FALSE, 0) &&
          !dup_for_write(unro) && dup_for_write(un), "unnamed: the read-only handle stays read-only");
    if (unro) CloseHandle(unro);
    if (un) CloseHandle(un);
    /* Without a descriptor nothing is refused */
    un = CreateFileMappingA(INVALID_HANDLE_VALUE, 0, PAGE_READWRITE, 0, 4096, 0), unro = 0;
    check(un && DuplicateHandle(GetCurrentProcess(), un, GetCurrentProcess(), &unro, FILE_MAP_READ, FALSE, 0) &&
          dup_for_write(unro), "no descriptor: a read-only handle may be duplicated for writing");
    if (unro) CloseHandle(unro);
    if (un) CloseHandle(un);
}

static void ordinals(void)
{
    struct { const char *dll, *name; WORD ord; } t[] = {
        { "shlwapi.dll", "QISearch", 219 }, { "shlwapi.dll", "IsOS", 437 },
        { "oleaut32.dll", "VarUI4FromStr", 277 }, { "oleaut32.dll", "VarBstrCat", 313 },
        { "oleaut32.dll", "VarBstrCmp", 314 }, { "uxtheme.dll", "DrawThemeBackgroundEx", 47 },
        { "shell32.dll", "SHChangeNotifyRegister", 2 }, { "shell32.dll", "SHChangeNotifyDeregister", 4 },
    };
    for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        HMODULE m = LoadLibraryA(t[i].dll);
        FARPROC byord = m ? GetProcAddress(m, (LPCSTR)(ULONG_PTR)t[i].ord) : 0;
        char what[96];
        snprintf(what, sizeof(what), "%s ordinal %u is %s", t[i].dll, t[i].ord, t[i].name);
        check(byord && byord == GetProcAddress(m, t[i].name), what);
    }
    typedef ULONG (WINAPI *RegFn)(HWND, int, LONG, UINT, int, const void *);
    typedef BOOL (WINAPI *DeregFn)(ULONG);
    HMODULE sh = LoadLibraryA("shell32.dll");
    RegFn reg = (RegFn)GetProcAddress(sh, (LPCSTR)2);
    DeregFn dereg = (DeregFn)GetProcAddress(sh, (LPCSTR)4);
    struct { void *pidl; BOOL recursive; } entry = { 0, FALSE };
    ULONG id = reg ? reg(GetDesktopWindow(), 3, 0x7FFFFFFF, WM_USER + 1, 1, &entry) : 0;
    check(id != 0 && dereg(id) && !dereg(id), "SHChangeNotifyRegister, then Deregister once");

    HMODULE p = LoadLibraryA("api-ms-win-power-base-l1-1-0.dll");
    check(p && GetProcAddress(p, "CallNtPowerInformation"), "api-ms-win-power-base has CallNtPowerInformation");
}

/* The rest of libcef.dll's static imports, looked up as the loader does */
typedef struct { DWORD cb; BYTE *pb; } Blob;
typedef struct { DWORD enc; BYTE *der; DWORD n; struct { DWORD ver; Blob serial; struct { char *oid; Blob params; } alg; Blob issuer;
                 FILETIME nb, na; Blob subject; } *info; HANDLE store; } CertCtx;
static volatile LONG g_proxy_status;
static volatile DWORD g_proxy_error;
static void CALLBACK proxy_cb(void *h, DWORD_PTR ctx, DWORD status, void *info, DWORD n)
{
    (void)h; (void)n;
    if (ctx == 0x5EA && status == 0x00200000 && info) g_proxy_error = *(DWORD *)((BYTE *)info + sizeof(DWORD_PTR));
    if (ctx == 0x5EA) InterlockedExchange(&g_proxy_status, (LONG)status);
}
static void imports(void)
{
    HMODULE k = GetModuleHandleA("kernel32.dll");
    BOOL (WINAPI *fw)(DWORD *) = (void *)GetProcAddress(k, "GetFirmwareType");
    BOOL (WINAPI *cdm)(DWORD *) = (void *)GetProcAddress(k, "GetConsoleDisplayMode");
    DWORD v = 99, m = 99;
    check(fw && fw(&v) && v == 2, "GetFirmwareType is UEFI");
    check(cdm && cdm(&m) && m == 0, "GetConsoleDisplayMode is windowed");

    HMODULE c = LoadLibraryA("crypt32.dll");
    HANDLE (WINAPI *open)(ULONG_PTR, LPCSTR) = (void *)GetProcAddress(c, "CertOpenSystemStoreA");
    const CertCtx *(WINAPI *enumc)(HANDLE, const void *) = (void *)GetProcAddress(c, "CertEnumCertificatesInStore");
    BOOL (WINAPI *ctl)(HANDLE, DWORD, DWORD, const void *) = (void *)GetProcAddress(c, "CertControlStore");
    BOOL (WINAPI *cmpn)(DWORD, Blob *, Blob *) = (void *)GetProcAddress(c, "CertCompareCertificateName");
    BOOL (WINAPI *ver)(ULONG_PTR, DWORD, DWORD, void *, DWORD, void *, DWORD, void *) =
        (void *)GetProcAddress(c, "CryptVerifyCertificateSignatureEx");
    HANDLE root = open ? open(0, "ROOT") : 0;
    const CertCtx *(WINAPI *dupc)(const void *) = (void *)GetProcAddress(c, "CertDuplicateCertificateContext");
    const CertCtx *a = root && enumc ? enumc(root, 0) : 0;
    if (a) a = dupc(a);                                 /* the enumeration frees the one it moves past */
    const CertCtx *b = a ? enumc(root, a) : 0;
    check(ctl && root && ctl(root, 0, 1, 0) && !ctl(root, 0, 99, 0),
          "CertControlStore resyncs and refuses an unknown control");
    check(a && b && cmpn && cmpn(1, &a->info->issuer, &a->info->subject) && !cmpn(1, &a->info->subject, &b->info->subject),
          "CertCompareCertificateName: a root's issuer is its subject, two roots differ");
    check(a && ver && ver(0, 1, 2, (void *)a, 2, (void *)a, 0, 0), "CryptVerifyCertificateSignatureEx: a root signed itself");
    check(a && b && ver && !ver(0, 1, 2, (void *)a, 2, (void *)b, 0, 0) && GetLastError() == 0x80090006,
          "CryptVerifyCertificateSignatureEx: another root did not");

    /* base::win::OSInfo reads kernelbase.dll's version, by bare name */
    HMODULE vd = LoadLibraryA("version.dll");
    DWORD (WINAPI *vsize)(LPCWSTR, DWORD *) = (void *)GetProcAddress(vd, "GetFileVersionInfoSizeW");
    BOOL (WINAPI *vinfo)(LPCWSTR, DWORD, DWORD, void *) = (void *)GetProcAddress(vd, "GetFileVersionInfoW");
    BOOL (WINAPI *vq)(const void *, LPCWSTR, void **, UINT *) = (void *)GetProcAddress(vd, "VerQueryValueW");
    static const WCHAR *const vnames[] = { L"kernelbase.dll", L"kernel32.dll" };
    for (int i = 0; i < 2; i++) {
        DWORD vn = vsize ? vsize(vnames[i], 0) : 0;
        BYTE *vb = vn ? malloc(vn) : 0;
        DWORD *ffi = 0; UINT fl = 0;
        BOOL ok = vb && vinfo(vnames[i], 0, vn, vb) && vq(vb, L"\\", (void **)&ffi, &fl) && fl >= 52 &&
                  ffi[0] == 0xFEEF04BD && ffi[2] >> 16 == 10 && (ffi[3] >> 16) == 18362;
        check(ok, i ? "kernel32.dll has a version resource, found by bare name" : "kernelbase.dll has a version resource, found by bare name");
        free(vb);
    }

    /* The event log: Chromium asks the System log how the last shutdown went */
    HMODULE ev = LoadLibraryA("wevtapi.dll");
    HANDLE (WINAPI *evq)(HANDLE, LPCWSTR, LPCWSTR, DWORD) = (void *)GetProcAddress(ev, "EvtQuery");
    BOOL (WINAPI *evn)(HANDLE, DWORD, HANDLE *, DWORD, DWORD, DWORD *) = (void *)GetProcAddress(ev, "EvtNext");
    BOOL (WINAPI *evc)(HANDLE) = (void *)GetProcAddress(ev, "EvtClose");
    HANDLE q = evq ? evq(0, L"System", L"*[System[Provider[@Name='eventlog'] and (EventID=6008)]]", 1) : 0;
    HANDLE got[4]; DWORD ng = 9;
    check(q && !evn(q, 4, got, INFINITE, 0, &ng) && GetLastError() == 259 && ng == 0 && evc(q),
          "wevtapi: the System log has no events to return");

    HMODULE w = LoadLibraryA("winhttp.dll");
    void *(WINAPI *wopen)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD) = (void *)GetProcAddress(w, "WinHttpOpen");
    void *(WINAPI *setcb)(void *, void *, DWORD, DWORD_PTR) = (void *)GetProcAddress(w, "WinHttpSetStatusCallback");
    DWORD (WINAPI *mk)(void *, void **) = (void *)GetProcAddress(w, "WinHttpCreateProxyResolver");
    DWORD (WINAPI *get)(void *, LPCWSTR, void *, DWORD_PTR) = (void *)GetProcAddress(w, "WinHttpGetProxyForUrlEx");
    BOOL (WINAPI *closeh)(void *) = (void *)GetProcAddress(w, "WinHttpCloseHandle");
    void *sync = wopen ? wopen(L"chrometest", 1, 0, 0, 0) : 0, *async = wopen ? wopen(L"chrometest", 1, 0, 0, 0x10000000) : 0;
    void *r = 0;
    check(mk && mk(sync, &r) == 12018 && !r, "WinHttpCreateProxyResolver needs an asynchronous session");
    check(mk && setcb && setcb(async, (void *)proxy_cb, 0xFFFFFFFF, 0) != (void *)-1 && mk(async, &r) == 0 && r,
          "WinHttpCreateProxyResolver on an asynchronous session");
    struct { DWORD flags, detect; LPCWSTR url; void *r1; DWORD r2; BOOL logon; } opts = { 1, 3, 0, 0, 0, FALSE };
    DWORD st = r && get ? get(r, L"https://store.steampowered.com/", &opts, 0x5EA) : 0;
    for (int i = 0; i < 200 && !g_proxy_status; i++) Sleep(10);
    check(st == ERROR_IO_PENDING && g_proxy_status == 0x00200000 && g_proxy_error == 12180,
          "WinHttpGetProxyForUrlEx completes later: no proxy script (autodetection failed)");
    if (r) closeh(r);
    if (async) closeh(async);
    if (sync) closeh(sync);
}

/* Chromium's browser CHECKs a file handle it is handed read-only: its
 * GrantedAccess (NtQueryObject) must have none of FILE_WRITE_DATA,
 * FILE_APPEND_DATA, FILE_WRITE_EA, FILE_WRITE_ATTRIBUTES, DELETE,
 * WRITE_DAC and WRITE_OWNER (0xD0116) */
static void file_access(void)
{
    typedef LONG (WINAPI *NtQueryObjectFn)(HANDLE, int, PVOID, ULONG, PULONG);
    NtQueryObjectFn qo = (NtQueryObjectFn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryObject");
    char path[MAX_PATH];
    GetTempPathA(sizeof(path), path);
    strcat(path, "chrometest-access.txt");
    HANDLE w = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, CREATE_ALWAYS, 0, 0);
    HANDLE r = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
    ULONG bw[14] = { 0 }, br[14] = { 0 };
    check(qo && w != INVALID_HANDLE_VALUE && qo(w, 0, bw, sizeof(bw), 0) >= 0 && (bw[1] & 0x2 /* FILE_WRITE_DATA */) && (bw[1] & 0x1 /* FILE_READ_DATA */),
          "NtQueryObject: a file opened for writing has FILE_WRITE_DATA");
    check(qo && r != INVALID_HANDLE_VALUE && qo(r, 0, br, sizeof(br), 0) >= 0 && !(br[1] & 0xD0116) && br[1] == 0x120089 /* FILE_GENERIC_READ */,
          "NtQueryObject: one opened GENERIC_READ has FILE_GENERIC_READ and no write right");
    if (w != INVALID_HANDLE_VALUE) CloseHandle(w);
    if (r != INVALID_HANDLE_VALUE) CloseHandle(r);
    DeleteFileA(path);
}

int main(int argc, char **argv)
{
    if (argc >= 5 && !strcmp(argv[1], "child"))
        return child((HANDLE)(ULONG_PTR)strtoull(argv[2], 0, 10), (HANDLE)(ULONG_PTR)strtoull(argv[3], 0, 10), argv[4]);
    shared_memory();
    file_access();
    ordinals();
    imports();
    printf("chrometest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
