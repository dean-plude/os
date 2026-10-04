/*
 * taskschd.dll — Task Scheduler 2.0's scripting objects (the TaskScheduler
 * class: ITaskService, ITaskFolder, IRegisteredTask...).
 *
 * Tasks are kept where Windows keeps them: each task's XML definition is a
 * file under C:\Windows\System32\Tasks (a task folder is a directory there;
 * UTF-16 with a byte-order mark, as the Task Scheduler service writes it)
 * and each is listed in the registry's task cache,
 * HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Schedule\TaskCache:
 * Tree\PATH holds its Id and Tasks\{Id} its Path, URI, Author and
 * Description.  Programs register, read, enable, disable, run and delete
 * tasks; what is registered survives restarts.
 *
 * Not here yet: the Task Scheduler service that starts tasks on their
 * triggers (time, logon, boot), so a task runs only when a program calls
 * IRegisteredTask::Run (its <Exec> actions are started then); the
 * ITaskDefinition object model (triggers, actions, settings as objects:
 * a definition is its XmlText); and IDispatch type information.
 */
#include <windows.h>
#include <oleauto.h>
#include <string.h>

#define TSAPI __declspec(dllexport)
#ifndef ERROR_BAD_NETPATH
#define ERROR_BAD_NETPATH        53
#endif
#ifndef ERROR_SERVICE_DISABLED
#define ERROR_SERVICE_DISABLED   1058
#endif
#ifndef ERROR_ONLY_IF_CONNECTED
#define ERROR_ONLY_IF_CONNECTED  1251
#endif
#ifndef MAX_COMPUTERNAME_LENGTH
#define MAX_COMPUTERNAME_LENGTH  15
#endif

static const GUID CLSID_TaskScheduler_ = { 0x0F87369F, 0xA4E5, 0x4CFC, { 0xBD, 0x3E, 0x73, 0xE6, 0x15, 0x45, 0x72, 0xDD } };
static const GUID IID_ITaskService_ = { 0x2FABA4C7, 0x4DA9, 0x4013, { 0x96, 0x97, 0x20, 0xCC, 0x3F, 0xD4, 0x0F, 0x85 } };
static const GUID IID_ITaskFolder_ = { 0x8CFAC062, 0xA080, 0x4C15, { 0x9A, 0x88, 0xAA, 0x7C, 0x2A, 0xF8, 0x0D, 0xFC } };
static const GUID IID_IRegisteredTask_ = { 0x9C86F320, 0xDEE3, 0x4DD1, { 0xB9, 0x72, 0xA3, 0x03, 0xF2, 0x6B, 0x06, 0x1E } };
static const GUID IID_ITaskDefinition_ = { 0xF5BC8FC5, 0x536D, 0x4F77, { 0xB8, 0x52, 0xFB, 0xC1, 0x35, 0x6F, 0xDE, 0xB6 } };
static const GUID IID_IRunningTask_ = { 0x653758FB, 0x7B9A, 0x4F1E, { 0xA4, 0x71, 0xBE, 0xEB, 0x8E, 0x9B, 0x83, 0x4E } };
static const GUID IID_IRegisteredTaskCollection_ = { 0x86627EB4, 0x42A7, 0x41E4, { 0xA4, 0xD9, 0xAC, 0x33, 0xA7, 0x2F, 0x2D, 0x52 } };
static const GUID IID_ITaskFolderCollection_ = { 0x79184A66, 0x8664, 0x423F, { 0x97, 0xF1, 0x63, 0x73, 0x56, 0xA5, 0xD8, 0x12 } };
static const GUID IID_IRunningTaskCollection_ = { 0x6A67614B, 0x6828, 0x4FEC, { 0xAA, 0x54, 0x6D, 0x52, 0xE8, 0xF1, 0xF2, 0xDB } };

#define HR_WIN32(e)            ((HRESULT)(0x80070000L | (e)))
#define SCHED_S_TASK_HAS_NOT_RUN ((HRESULT)0x00041303L)
#define SCHED_E_MALFORMEDXML   ((HRESULT)0x8004131AL)
#define TASK_VALIDATE_ONLY 0x1
#define TASK_CREATE        0x2
#define TASK_UPDATE        0x4
#define TASK_DISABLE       0x8
#define TASK_STATE_DISABLED 1
#define TASK_STATE_READY    3

int _fltused = 0x9875;      /* DATE (a double) results */
static volatile LONG g_objects, g_locks;

static void *zalloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }
static void zfree(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }
static int wlen(const WCHAR *s) { int n = 0; if (s) while (s[n]) n++; return n; }

static void note(const char *what)
{
    char m[160];
    int n = 0;
    for (const char *s = "taskschd: "; *s; s++) m[n++] = *s;
    for (const char *s = what; *s && n < 156; s++) m[n++] = *s;
    m[n++] = '\n';
    m[n] = 0;
    OutputDebugStringA(m);
}

/* ---------------------------------------------------------------------------
 * The store: task paths are "\Folder\Name" ("\" is the root folder)
 * ------------------------------------------------------------------------- */
#define CACHE L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Schedule\\TaskCache"

/* WoW64 sends a 32-bit program's System32 to SysWOW64; the tasks are the
 * machine's, in the real System32 */
typedef struct { PVOID old; BOOL off; } NoRedirect;
static void redirect_off(NoRedirect *r) { r->off = Wow64DisableWow64FsRedirection(&r->old); }
static void redirect_on(NoRedirect *r) { if (r->off) Wow64RevertWow64FsRedirection(r->old); }

/* the file (or directory) of task path @path */
static void store_path(const WCHAR *path, WCHAR *out)
{
    int n = (int)GetWindowsDirectoryW(out, MAX_PATH);
    const WCHAR *t = L"\\System32\\Tasks";
    while (*t) out[n++] = *t++;
    for (const WCHAR *p = path; *p && n < 2 * MAX_PATH - 1; p++)
        if (!(p == path && *p == '\\' && !p[1])) out[n++] = *p;
    out[n] = 0;
}

/* @folder joined with @name: an absolute name ("\x") stands alone; "\\" and
 * trailing separators are tidied away.  FALSE if the result is too long or
 * has an empty or "."/".." component */
static BOOL join_path(const WCHAR *folder, const WCHAR *name, WCHAR *out, int cap)
{
    int n = 0;
    if (!name || name[0] != '\\')
        for (const WCHAR *p = folder; *p && n < cap - 1; p++) out[n++] = *p;
    for (const WCHAR *p = name ? name : L""; *p; p++) {
        WCHAR c = *p == '/' ? '\\' : *p;
        if (c == '\\' && n && out[n - 1] == '\\') continue;
        if (n >= cap - 1) return FALSE;
        out[n++] = c;
    }
    if (!n || out[0] != '\\') {
        if (n >= cap - 1) return FALSE;
        memmove(out + 1, out, n * sizeof(WCHAR));
        out[0] = '\\';
        n++;
    }
    while (n > 1 && out[n - 1] == '\\') n--;
    out[n] = 0;
    for (const WCHAR *p = out + 1; *p;) {
        const WCHAR *e = p;
        while (*e && *e != '\\') e++;
        if (e == p || (e - p == 1 && p[0] == '.') || (e - p == 2 && p[0] == '.' && p[1] == '.')) return FALSE;
        for (const WCHAR *c = p; c < e; c++)
            if (*c < 32 || *c == ':' || *c == '*' || *c == '?' || *c == '"' || *c == '<' || *c == '>' || *c == '|') return FALSE;
        p = *e ? e + 1 : e;
    }
    return TRUE;
}

static const WCHAR *last_part(const WCHAR *path)
{
    const WCHAR *s = path;
    for (const WCHAR *p = path; *p; p++) if (*p == '\\') s = p + 1;
    return *s ? s : path;
}

static DWORD attrs_of(const WCHAR *path)
{
    WCHAR f[2 * MAX_PATH];
    NoRedirect r;
    store_path(path, f);
    redirect_off(&r);
    DWORD a = GetFileAttributesW(f);
    redirect_on(&r);
    return a;
}
static BOOL is_folder(const WCHAR *path) { DWORD a = attrs_of(path); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
static BOOL is_task(const WCHAR *path) { DWORD a = attrs_of(path); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }

/* the task's XML (a new string, NULL if there is no such task) */
static WCHAR *read_task(const WCHAR *path)
{
    WCHAR f[2 * MAX_PATH];
    NoRedirect r;
    store_path(path, f);
    redirect_off(&r);
    HANDLE h = CreateFileW(f, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING, 0, 0);
    redirect_on(&r);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD size = GetFileSize(h, 0), got = 0;
    BYTE *b = size < (16u << 20) ? zalloc(size + 4) : 0;
    if (b && !ReadFile(h, b, size, &got, 0)) got = 0;
    CloseHandle(h);
    if (!b) return 0;
    WCHAR *w;
    if (got >= 2 && b[0] == 0xFF && b[1] == 0xFE) {                 /* UTF-16 */
        w = zalloc(got);
        if (w) memcpy(w, b + 2, got - 2);
    } else {                                                        /* UTF-8, with or without a BOM */
        int skip = got >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF ? 3 : 0;
        int n = MultiByteToWideChar(CP_UTF8, 0, (char *)b + skip, (int)got - skip, 0, 0);
        w = zalloc((n + 1) * sizeof(WCHAR));
        if (w) MultiByteToWideChar(CP_UTF8, 0, (char *)b + skip, (int)got - skip, w, n);
    }
    zfree(b);
    return w;
}

/* ---------------------------------------------------------------------------
 * A little of the task XML: element text by name, and changing <Enabled>
 * ------------------------------------------------------------------------- */
static const WCHAR *find_w(const WCHAR *s, const WCHAR *end, const WCHAR *what)
{
    int n = wlen(what);
    for (; s + n <= end; s++)
        if (!memcmp(s, what, n * sizeof(WCHAR))) return s;
    return 0;
}

/* the text of the first <@tag> within [@s, @end) (entities decoded), or
 * NULL; @after gets where the element ends */
static WCHAR *element_text(const WCHAR *s, const WCHAR *end, const WCHAR *tag, const WCHAR **after)
{
    WCHAR open[64], close[64];
    int n = wlen(tag);
    if (n > 60) return 0;
    open[0] = '<'; memcpy(open + 1, tag, n * sizeof(WCHAR)); open[n + 1] = 0;
    close[0] = '<'; close[1] = '/'; memcpy(close + 2, tag, n * sizeof(WCHAR)); close[n + 2] = '>'; close[n + 3] = 0;
    const WCHAR *p = s;
    for (;;) {
        p = find_w(p, end, open);
        if (!p) return 0;
        WCHAR c = p[n + 1];
        if (c == '>' || c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '/') break;
        p++;
    }
    const WCHAR *gt = p;
    while (gt < end && *gt != '>') gt++;
    if (gt >= end) return 0;
    WCHAR *out;
    if (gt[-1] == '/') {                                            /* <Tag/> */
        if (after) *after = gt + 1;
        out = zalloc(sizeof(WCHAR));
        return out;
    }
    const WCHAR *e = find_w(gt + 1, end, close);
    if (!e) return 0;
    if (after) *after = e + n + 3;
    out = zalloc((e - gt) * sizeof(WCHAR));
    if (!out) return 0;
    int o = 0;
    for (const WCHAR *q = gt + 1; q < e;) {
        static const struct { const WCHAR *name; WCHAR c; } ent[] = {
            { L"&amp;", '&' }, { L"&lt;", '<' }, { L"&gt;", '>' }, { L"&quot;", '"' }, { L"&apos;", '\'' } };
        int done = 0;
        if (*q == '&') {
            for (int i = 0; i < 5 && !done; i++) {
                int l = wlen(ent[i].name);
                if (q + l <= e && !memcmp(q, ent[i].name, l * sizeof(WCHAR))) { out[o++] = ent[i].c; q += l; done = 1; }
            }
            if (!done && q + 2 < e && q[1] == '#') {
                unsigned v = 0;
                const WCHAR *r = q + 2;
                BOOL hex = *r == 'x';
                if (hex) r++;
                for (; r < e && *r != ';'; r++)
                    v = hex ? v * 16 + (*r <= '9' ? *r - '0' : (*r | 32) - 'a' + 10) : v * 10 + (*r - '0');
                if (r < e) { out[o++] = (WCHAR)v; q = r + 1; done = 1; }
            }
        }
        if (!done) out[o++] = *q++;
    }
    out[o] = 0;
    return out;
}

static BOOL xml_bool(const WCHAR *xml, const WCHAR *section, const WCHAR *tag, BOOL dflt)
{
    int n = wlen(xml);
    const WCHAR *s = xml, *e = xml + n;
    WCHAR *sec = section ? element_text(s, e, section, 0) : 0;
    if (section && !sec) return dflt;
    WCHAR *v = sec ? element_text(sec, sec + wlen(sec), tag, 0) : element_text(s, e, tag, 0);
    BOOL r = dflt;
    if (v) {
        const WCHAR *p = v;
        while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t') p++;
        r = *p == 't' || *p == '1';
    }
    zfree(v);
    zfree(sec);
    return r;
}

/* @xml with <Settings>'s <Enabled> set to @on (a new string) */
static WCHAR *set_enabled(const WCHAR *xml, BOOL on)
{
    int n = wlen(xml);
    const WCHAR *end = xml + n, *set = find_w(xml, end, L"<Settings"), *set_end = 0, *en = 0, *en_end = 0;
    const WCHAR *val = on ? L"true" : L"false";
    if (set) set_end = find_w(set, end, L"</Settings>");
    if (set && set_end && (en = find_w(set, set_end, L"<Enabled>")) && (en_end = find_w(en, set_end, L"</Enabled>"))) {
        en += 9;                                                    /* replace the text */
    } else if (set && set_end) {
        const WCHAR *gt = set;                                      /* insert after <Settings> */
        while (gt < set_end && *gt != '>') gt++;
        en = en_end = gt + 1;
        val = on ? L"<Enabled>true</Enabled>" : L"<Enabled>false</Enabled>";
    } else {
        const WCHAR *t = find_w(xml, end, L"</Task>");              /* add a <Settings> */
        if (!t) return 0;
        en = en_end = t;
        val = on ? L"<Settings><Enabled>true</Enabled></Settings>" : L"<Settings><Enabled>false</Enabled></Settings>";
    }
    int a = (int)(en - xml), v = wlen(val), b = (int)(end - en_end);
    WCHAR *out = zalloc((a + v + b + 1) * sizeof(WCHAR));
    if (!out) return 0;
    memcpy(out, xml, a * sizeof(WCHAR));
    memcpy(out + a, val, v * sizeof(WCHAR));
    memcpy(out + a + v, en_end, b * sizeof(WCHAR));
    return out;
}

/* a task definition: one <Task> root element of the Task Scheduler schema
 * with at least one action */
static BOOL valid_task_xml(const WCHAR *xml)
{
    int n = wlen(xml);
    const WCHAR *e = xml + n, *t = find_w(xml, e, L"<Task"), *te = find_w(xml, e, L"</Task>");
    if (!t || !te || te < t || (t[5] != ' ' && t[5] != '>' && t[5] != '\r' && t[5] != '\n' && t[5] != '\t')) return FALSE;
    return find_w(t, te, L"<Actions") != 0;
}

/* ---------------------------------------------------------------------------
 * Writing and deleting tasks (with their task-cache entries)
 * ------------------------------------------------------------------------- */
static void guid_string(const GUID *g, WCHAR *out)
{
    static const char hex[] = "0123456789ABCDEF";
    const BYTE *b = (const BYTE *)g;
    static const int order[16] = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
    int n = 0;
    out[n++] = '{';
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[n++] = '-';
        out[n++] = hex[b[order[i]] >> 4];
        out[n++] = hex[b[order[i]] & 15];
    }
    out[n++] = '}';
    out[n] = 0;
}

static void cache_key(const WCHAR *part, const WCHAR *rest, WCHAR *out)
{
    int n = 0;
    for (const WCHAR *s = CACHE; *s; s++) out[n++] = *s;
    for (const WCHAR *s = part; *s; s++) out[n++] = *s;
    for (const WCHAR *s = rest; *s && n < 1000; s++) out[n++] = *s;
    out[n] = 0;
}

static void cache_value(HKEY k, const WCHAR *name, const WCHAR *xml, const WCHAR *tag)
{
    WCHAR *info = element_text(xml, xml + wlen(xml), L"RegistrationInfo", 0);
    WCHAR *v = info ? element_text(info, info + wlen(info), tag, 0) : 0;
    if (v) RegSetValueExW(k, name, 0, REG_SZ, (BYTE *)v, (wlen(v) + 1) * sizeof(WCHAR));
    zfree(v);
    zfree(info);
}

static void cache_add(const WCHAR *path, const WCHAR *xml)
{
    WCHAR key[1024], id[40];
    HKEY k;
    DWORD type, n = sizeof id;
    cache_key(L"\\Tree", path, key);
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, key, 0, 0, 0, KEY_ALL_ACCESS, 0, &k, 0)) return;
    if (RegQueryValueExW(k, L"Id", 0, &type, (BYTE *)id, &n) || type != REG_SZ) {
        GUID g;
        CoCreateGuid(&g);
        guid_string(&g, id);
        RegSetValueExW(k, L"Id", 0, REG_SZ, (BYTE *)id, (wlen(id) + 1) * sizeof(WCHAR));
        DWORD index = 3;
        RegSetValueExW(k, L"Index", 0, REG_DWORD, (BYTE *)&index, 4);
    }
    RegCloseKey(k);
    cache_key(L"\\Tasks\\", id, key);
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, key, 0, 0, 0, KEY_ALL_ACCESS, 0, &k, 0)) return;
    RegSetValueExW(k, L"Path", 0, REG_SZ, (BYTE *)path, (wlen(path) + 1) * sizeof(WCHAR));
    cache_value(k, L"URI", xml, L"URI");
    cache_value(k, L"Author", xml, L"Author");
    cache_value(k, L"Description", xml, L"Description");
    RegCloseKey(k);
}

static void cache_remove(const WCHAR *path)
{
    WCHAR key[1024], id[40];
    HKEY k;
    DWORD type, n = sizeof id;
    cache_key(L"\\Tree", path, key);
    if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &k)) {
        if (!RegQueryValueExW(k, L"Id", 0, &type, (BYTE *)id, &n) && type == REG_SZ) {
            WCHAR tk[1024];
            cache_key(L"\\Tasks\\", id, tk);
            RegDeleteKeyW(HKEY_LOCAL_MACHINE, tk);
        }
        RegCloseKey(k);
    }
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, key);
}

/* each folder of @path's parent, made if missing */
static BOOL make_parents(const WCHAR *path)
{
    WCHAR f[2 * MAX_PATH], base[2 * MAX_PATH];
    NoRedirect r;
    store_path(L"\\", base);
    store_path(path, f);
    int bl = wlen(base);
    redirect_off(&r);
    CreateDirectoryW(base, 0);
    for (int i = bl + 1; f[i]; i++)
        if (f[i] == '\\') { f[i] = 0; CreateDirectoryW(f, 0); f[i] = '\\'; }
    redirect_on(&r);
    return TRUE;
}

static HRESULT write_task(const WCHAR *path, const WCHAR *xml)
{
    WCHAR f[2 * MAX_PATH];
    NoRedirect r;
    make_parents(path);
    store_path(path, f);
    redirect_off(&r);
    HANDLE h = CreateFileW(f, GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    redirect_on(&r);
    if (h == INVALID_HANDLE_VALUE) return HR_WIN32(GetLastError());
    static const BYTE bom[2] = { 0xFF, 0xFE };
    DWORD put;
    BOOL ok = WriteFile(h, bom, 2, &put, 0) && WriteFile(h, xml, wlen(xml) * sizeof(WCHAR), &put, 0);
    DWORD e = GetLastError();
    CloseHandle(h);
    if (!ok) return HR_WIN32(e);
    cache_add(path, xml);
    return S_OK;
}

static HRESULT delete_task(const WCHAR *path)
{
    WCHAR f[2 * MAX_PATH];
    NoRedirect r;
    if (!is_task(path)) return HR_WIN32(ERROR_FILE_NOT_FOUND);
    store_path(path, f);
    redirect_off(&r);
    BOOL ok = DeleteFileW(f);
    DWORD e = GetLastError();
    redirect_on(&r);
    if (!ok) return HR_WIN32(e);
    cache_remove(path);
    return S_OK;
}

/* the names of @folder's tasks (@dirs: subfolders) as a NUL-separated list
 * ending in an empty name; @count gets how many */
static WCHAR *list_folder(const WCHAR *folder, BOOL dirs, LONG *count)
{
    WCHAR f[2 * MAX_PATH];
    WIN32_FIND_DATAW fd;
    NoRedirect r;
    int cap = 256, n = 0;
    WCHAR *out = zalloc(cap * sizeof(WCHAR));
    *count = 0;
    if (!out) return 0;
    store_path(folder, f);
    int l = wlen(f);
    f[l] = '\\'; f[l + 1] = '*'; f[l + 2] = 0;
    redirect_off(&r);
    HANDLE h = FindFirstFileW(f, &fd);
    redirect_on(&r);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            BOOL dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (dir != dirs || (fd.cFileName[0] == '.' && (!fd.cFileName[1] || (fd.cFileName[1] == '.' && !fd.cFileName[2]))))
                continue;
            int k = wlen(fd.cFileName);
            if (n + k + 2 > cap) {
                WCHAR *g = zalloc((cap * 2 + k) * sizeof(WCHAR));
                if (!g) break;
                memcpy(g, out, n * sizeof(WCHAR));
                zfree(out);
                out = g;
                cap = cap * 2 + k;
            }
            memcpy(out + n, fd.cFileName, (k + 1) * sizeof(WCHAR));
            n += k + 1;
            (*count)++;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    out[n] = 0;
    return out;
}

/* ---------------------------------------------------------------------------
 * The objects.  Each is a vtable pointer, a reference count and its data;
 * the IDispatch methods (for scripts) give no type information yet.
 * ------------------------------------------------------------------------- */
typedef struct Obj { const void *const *vtbl; LONG refs; const GUID *iid; void (*free)(struct Obj *); } Obj;

static HRESULT STDMETHODCALLTYPE obj_qi(Obj *o, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDispatch) || IsEqualIID(riid, o->iid)) {
        *ppv = o;
        InterlockedIncrement(&o->refs);
        return S_OK;
    }
    *ppv = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE obj_addref(Obj *o) { return (ULONG)InterlockedIncrement(&o->refs); }
static ULONG STDMETHODCALLTYPE obj_release(Obj *o)
{
    LONG r = InterlockedDecrement(&o->refs);
    if (!r) {
        if (o->free) o->free(o);
        zfree(o);
        InterlockedDecrement(&g_objects);
    }
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE disp_count(Obj *o, UINT *n) { (void)o; if (!n) return E_POINTER; *n = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE disp_info(Obj *o, UINT i, LCID l, void **ti) { (void)o; (void)i; (void)l; if (ti) *ti = 0; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE disp_ids(Obj *o, REFIID r, LPOLESTR *names, UINT n, LCID l, DISPID *ids)
{ (void)o; (void)r; (void)names; (void)l; for (UINT i = 0; ids && i < n; i++) ids[i] = DISPID_UNKNOWN; return DISP_E_UNKNOWNNAME; }
static HRESULT STDMETHODCALLTYPE disp_invoke(Obj *o, DISPID id, REFIID r, LCID l, WORD f, DISPPARAMS *p, VARIANT *res, EXCEPINFO *e, UINT *a)
{ (void)o; (void)id; (void)r; (void)l; (void)f; (void)p; (void)res; (void)e; (void)a; return DISP_E_MEMBERNOTFOUND; }

#define OBJ_BASE (const void *)obj_qi, (const void *)obj_addref, (const void *)obj_release, \
                 (const void *)disp_count, (const void *)disp_info, (const void *)disp_ids, (const void *)disp_invoke

static void *new_obj(SIZE_T size, const void *const *vtbl, const GUID *iid, void (*free)(Obj *))
{
    Obj *o = zalloc(size);
    if (!o) return 0;
    o->vtbl = vtbl;
    o->refs = 1;
    o->iid = iid;
    o->free = free;
    InterlockedIncrement(&g_objects);
    return o;
}

static HRESULT put_bstr(const WCHAR *s, BSTR *out)
{
    if (!out) return E_POINTER;
    *out = SysAllocString(s ? s : L"");
    return *out ? S_OK : E_OUTOFMEMORY;
}

static HRESULT not_impl(const char *what) { note(what); return E_NOTIMPL; }

/* ---- ITaskDefinition: its XML text ---- */
typedef struct { Obj o; WCHAR *xml; BSTR data; } Definition;
static void def_free(Obj *o) { zfree(((Definition *)o)->xml); SysFreeString(((Definition *)o)->data); }

static HRESULT STDMETHODCALLTYPE def_sub_get(Obj *o, void **out) { (void)o; if (out) *out = 0; return not_impl("ITaskDefinition object model (triggers, actions, settings) not implemented"); }
static HRESULT STDMETHODCALLTYPE def_sub_put(Obj *o, void *in) { (void)o; (void)in; return not_impl("ITaskDefinition object model (triggers, actions, settings) not implemented"); }
static HRESULT STDMETHODCALLTYPE def_get_data(Definition *d, BSTR *out) { return put_bstr(d->data, out); }
static HRESULT STDMETHODCALLTYPE def_put_data(Definition *d, BSTR in)
{
    BSTR c = in ? SysAllocString(in) : 0;
    if (in && !c) return E_OUTOFMEMORY;
    SysFreeString(d->data);
    d->data = c;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE def_get_xml(Definition *d, BSTR *out)
{
    static const WCHAR empty[] = L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"  <RegistrationInfo />\r\n  <Triggers />\r\n  <Settings />\r\n  <Actions />\r\n</Task>\r\n";
    return put_bstr(d->xml ? d->xml : empty, out);
}
static HRESULT STDMETHODCALLTYPE def_put_xml(Definition *d, BSTR in)
{
    if (!in) return E_INVALIDARG;
    if (!valid_task_xml(in)) return SCHED_E_MALFORMEDXML;
    int n = wlen(in);
    WCHAR *c = zalloc((n + 1) * sizeof(WCHAR));
    if (!c) return E_OUTOFMEMORY;
    memcpy(c, in, n * sizeof(WCHAR));
    zfree(d->xml);
    d->xml = c;
    return S_OK;
}
static const void *const def_vtbl[] = { OBJ_BASE,
    def_sub_get, def_sub_put,                       /* RegistrationInfo */
    def_sub_get, def_sub_put,                       /* Triggers */
    def_sub_get, def_sub_put,                       /* Settings */
    def_get_data, def_put_data,
    def_sub_get, def_sub_put,                       /* Principal */
    def_sub_get, def_sub_put,                       /* Actions */
    def_get_xml, def_put_xml };

static HRESULT new_definition(const WCHAR *xml, void **out)
{
    Definition *d = new_obj(sizeof(Definition), def_vtbl, &IID_ITaskDefinition_, def_free);
    if (!d) return E_OUTOFMEMORY;
    if (xml) {
        int n = wlen(xml);
        d->xml = zalloc((n + 1) * sizeof(WCHAR));
        if (d->xml) memcpy(d->xml, xml, n * sizeof(WCHAR));
    }
    *out = d;
    return S_OK;
}

/* ---- IRunningTask: a task started by Run ---- */
typedef struct { Obj o; WCHAR path[MAX_PATH]; GUID guid; DWORD pid; HANDLE process; } Running;
static void run_free(Obj *o) { if (((Running *)o)->process) CloseHandle(((Running *)o)->process); }
static HRESULT STDMETHODCALLTYPE run_name(Running *r, BSTR *out) { return put_bstr(last_part(r->path), out); }
static HRESULT STDMETHODCALLTYPE run_guid(Running *r, BSTR *out) { WCHAR g[40]; guid_string(&r->guid, g); return put_bstr(g, out); }
static HRESULT STDMETHODCALLTYPE run_path(Running *r, BSTR *out) { return put_bstr(r->path, out); }
static HRESULT STDMETHODCALLTYPE run_state(Running *r, int *out)
{
    if (!out) return E_POINTER;
    *out = r->process && WaitForSingleObject(r->process, 0) == WAIT_TIMEOUT ? 4 /* TASK_STATE_RUNNING */ : TASK_STATE_READY;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE run_action(Running *r, BSTR *out) { (void)r; return put_bstr(L"", out); }
static HRESULT STDMETHODCALLTYPE run_stop(Running *r) { if (r->process) TerminateProcess(r->process, 0x41306 /* SCHED_S_TASK_TERMINATED */); return S_OK; }
static HRESULT STDMETHODCALLTYPE run_refresh(Running *r) { (void)r; return S_OK; }
static HRESULT STDMETHODCALLTYPE run_pid(Running *r, DWORD *out) { if (!out) return E_POINTER; *out = r->pid; return S_OK; }
static const void *const run_vtbl[] = { OBJ_BASE, run_name, run_guid, run_path, run_state, run_action, run_stop, run_refresh, run_pid };

/* ---- the collections (of tasks, folders or running tasks) ---- */
typedef struct { Obj o; LONG n; Obj **items; } Collection;
static void coll_free(Obj *o)
{
    Collection *c = (Collection *)o;
    for (LONG i = 0; i < c->n; i++) if (c->items[i]) obj_release(c->items[i]);
    zfree(c->items);
}
static HRESULT STDMETHODCALLTYPE coll_count(Collection *c, LONG *out) { if (!out) return E_POINTER; *out = c->n; return S_OK; }
static HRESULT STDMETHODCALLTYPE coll_item(Collection *c, VARIANT index, Obj **out)
{
    if (!out) return E_POINTER;
    *out = 0;
    LONG i;
    VARIANT v;
    VariantInit(&v);
    if (V_VT(&index) == VT_BSTR) {                                  /* by name */
        for (i = 0; i < c->n; i++) {
            BSTR name = 0;
            ((HRESULT (STDMETHODCALLTYPE *)(Obj *, BSTR *))c->items[i]->vtbl[7])(c->items[i], &name);
            BOOL same = name && !lstrcmpiW(name, V_BSTR(&index));
            SysFreeString(name);
            if (same) break;
        }
    } else {                                                        /* by number, from 1 */
        if (FAILED(VariantChangeType(&v, &index, 0, VT_I4))) return E_INVALIDARG;
        i = V_I4(&v) - 1;
    }
    if (i < 0 || i >= c->n) return E_INVALIDARG;
    *out = c->items[i];
    obj_addref(*out);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE coll_enum(Collection *c, IUnknown **out) { (void)c; if (out) *out = 0; return not_impl("_NewEnum not implemented"); }
static const void *const coll_vtbl[] = { OBJ_BASE, coll_count, coll_item, coll_enum };

static Collection *new_collection(const GUID *iid, LONG n)
{
    Collection *c = new_obj(sizeof(Collection), coll_vtbl, iid, coll_free);
    if (!c) return 0;
    c->items = n ? zalloc(n * sizeof(Obj *)) : 0;
    if (n && !c->items) { obj_release(&c->o); return 0; }
    return c;
}

/* ---- IRegisteredTask ---- */
typedef struct { Obj o; WCHAR path[MAX_PATH]; } Task;

static HRESULT STDMETHODCALLTYPE task_name(Task *t, BSTR *out) { return put_bstr(last_part(t->path), out); }
static HRESULT STDMETHODCALLTYPE task_path(Task *t, BSTR *out) { return put_bstr(t->path, out); }
static HRESULT STDMETHODCALLTYPE task_enabled(Task *t, VARIANT_BOOL *out)
{
    if (!out) return E_POINTER;
    WCHAR *x = read_task(t->path);
    if (!x) return HR_WIN32(ERROR_FILE_NOT_FOUND);
    *out = xml_bool(x, L"Settings", L"Enabled", TRUE) ? VARIANT_TRUE : VARIANT_FALSE;
    zfree(x);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE task_state(Task *t, int *out)
{
    VARIANT_BOOL on;
    if (!out) return E_POINTER;
    HRESULT hr = task_enabled(t, &on);
    if (FAILED(hr)) return hr;
    *out = on ? TASK_STATE_READY : TASK_STATE_DISABLED;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE task_put_enabled(Task *t, VARIANT_BOOL on)
{
    WCHAR *x = read_task(t->path);
    if (!x) return HR_WIN32(ERROR_FILE_NOT_FOUND);
    WCHAR *y = set_enabled(x, on != VARIANT_FALSE);
    HRESULT hr = y ? write_task(t->path, y) : SCHED_E_MALFORMEDXML;
    zfree(x);
    zfree(y);
    return hr;
}

/* Run: start each <Exec> action's command (environment variables in it
 * expanded), as the Task Scheduler service does when a trigger fires */
static HRESULT STDMETHODCALLTYPE task_run_ex(Task *t, VARIANT params, LONG flags, LONG session, BSTR user, Obj **out)
{
    (void)params; (void)flags; (void)session; (void)user;
    if (out) *out = 0;
    WCHAR *x = read_task(t->path);
    if (!x) return HR_WIN32(ERROR_FILE_NOT_FOUND);
    if (!xml_bool(x, L"Settings", L"Enabled", TRUE)) { zfree(x); return HR_WIN32(ERROR_SERVICE_DISABLED); }
    const WCHAR *p = x, *end = x + wlen(x);
    Running *run = 0;
    HRESULT hr = S_OK;
    for (;;) {
        const WCHAR *after;
        WCHAR *exec = element_text(p, end, L"Exec", &after);
        if (!exec) break;
        p = after;
        const WCHAR *ee = exec + wlen(exec);
        WCHAR *cmd = element_text(exec, ee, L"Command", 0), *args = element_text(exec, ee, L"Arguments", 0),
              *dir = element_text(exec, ee, L"WorkingDirectory", 0);
        WCHAR line[4096], ecmd[MAX_PATH], edir[MAX_PATH];
        if (cmd) {
            ExpandEnvironmentStringsW(cmd, ecmd, MAX_PATH);
            int n = 0;
            BOOL quote = ecmd[0] != '"';
            if (quote) line[n++] = '"';
            for (WCHAR *c = ecmd; *c && n < 4000; c++) line[n++] = *c;
            if (quote) line[n++] = '"';
            if (args && args[0]) {
                WCHAR eargs[2048];
                ExpandEnvironmentStringsW(args, eargs, 2048);
                line[n++] = ' ';
                for (WCHAR *c = eargs; *c && n < 4094; c++) line[n++] = *c;
            }
            line[n] = 0;
            if (dir && dir[0]) ExpandEnvironmentStringsW(dir, edir, MAX_PATH);
            STARTUPINFOW si = { sizeof si };
            PROCESS_INFORMATION pi;
            if (CreateProcessW(0, line, 0, 0, FALSE, 0, 0, dir && dir[0] ? edir : 0, &si, &pi)) {
                CloseHandle(pi.hThread);
                if (out && !run && (run = new_obj(sizeof(Running), run_vtbl, &IID_IRunningTask_, run_free))) {
                    lstrcpynW(run->path, t->path, MAX_PATH);
                    CoCreateGuid(&run->guid);
                    run->pid = pi.dwProcessId;
                    run->process = pi.hProcess;
                } else CloseHandle(pi.hProcess);
            } else hr = HR_WIN32(GetLastError());
        }
        zfree(cmd); zfree(args); zfree(dir); zfree(exec);
    }
    zfree(x);
    if (out) *out = run ? &run->o : 0;
    return hr;
}
static HRESULT STDMETHODCALLTYPE task_run(Task *t, VARIANT params, Obj **out)
{
    return task_run_ex(t, params, 0, 0, 0, out);
}
static HRESULT STDMETHODCALLTYPE task_instances(Task *t, LONG flags, Obj **out)
{
    (void)t; (void)flags;
    if (!out) return E_POINTER;
    Collection *c = new_collection(&IID_IRunningTaskCollection_, 0);
    *out = c ? &c->o : 0;
    return c ? S_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE task_last_run(Task *t, DATE *out) { (void)t; if (!out) return E_POINTER; *out = 0; return SCHED_S_TASK_HAS_NOT_RUN; }
static HRESULT STDMETHODCALLTYPE task_last_result(Task *t, LONG *out) { (void)t; if (!out) return E_POINTER; *out = SCHED_S_TASK_HAS_NOT_RUN; return S_OK; }
static HRESULT STDMETHODCALLTYPE task_missed(Task *t, LONG *out) { (void)t; if (!out) return E_POINTER; *out = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE task_next_run(Task *t, DATE *out) { (void)t; if (!out) return E_POINTER; *out = 0; return S_FALSE; }
static HRESULT STDMETHODCALLTYPE task_definition(Task *t, void **out)
{
    if (!out) return E_POINTER;
    *out = 0;
    WCHAR *x = read_task(t->path);
    if (!x) return HR_WIN32(ERROR_FILE_NOT_FOUND);
    HRESULT hr = new_definition(x, out);
    zfree(x);
    return hr;
}
static HRESULT STDMETHODCALLTYPE task_xml(Task *t, BSTR *out)
{
    if (!out) return E_POINTER;
    WCHAR *x = read_task(t->path);
    if (!x) return HR_WIN32(ERROR_FILE_NOT_FOUND);
    HRESULT hr = put_bstr(x, out);
    zfree(x);
    return hr;
}
/* everyone may read and run tasks, administrators and SYSTEM own them */
static HRESULT STDMETHODCALLTYPE task_get_sd(Task *t, LONG info, BSTR *out)
{
    (void)t; (void)info;
    return put_bstr(L"O:BAG:SYD:(A;;FA;;;BA)(A;;FA;;;SY)(A;;FRFX;;;WD)", out);
}
static HRESULT STDMETHODCALLTYPE task_set_sd(Task *t, BSTR sddl, LONG flags) { (void)t; (void)flags; return sddl ? S_OK : E_INVALIDARG; }
static HRESULT STDMETHODCALLTYPE task_stop(Task *t, LONG flags) { (void)t; (void)flags; return S_OK; }
static HRESULT STDMETHODCALLTYPE task_run_times(Task *t, const SYSTEMTIME *a, const SYSTEMTIME *b, DWORD *n, SYSTEMTIME **out)
{
    (void)t; (void)a; (void)b;
    if (!n) return E_POINTER;
    *n = 0;
    if (out) *out = 0;
    return S_FALSE;
}
static const void *const task_vtbl[] = { OBJ_BASE, task_name, task_path, task_state, task_enabled, task_put_enabled,
    task_run, task_run_ex, task_instances, task_last_run, task_last_result, task_missed, task_next_run,
    task_definition, task_xml, task_get_sd, task_set_sd, task_stop, task_run_times };

static Task *new_task(const WCHAR *path)
{
    Task *t = new_obj(sizeof(Task), task_vtbl, &IID_IRegisteredTask_, 0);
    if (t) lstrcpynW(t->path, path, MAX_PATH);
    return t;
}

/* ---- ITaskFolder ---- */
typedef struct { Obj o; WCHAR path[MAX_PATH]; } Folder;
static Folder *new_folder(const WCHAR *path);

static HRESULT STDMETHODCALLTYPE folder_name(Folder *f, BSTR *out) { return put_bstr(f->path[1] ? last_part(f->path) : f->path, out); }
static HRESULT STDMETHODCALLTYPE folder_path(Folder *f, BSTR *out) { return put_bstr(f->path, out); }
static HRESULT STDMETHODCALLTYPE folder_get_folder(Folder *f, BSTR name, Obj **out)
{
    WCHAR p[MAX_PATH];
    if (!out) return E_POINTER;
    *out = 0;
    if (!join_path(f->path, name, p, MAX_PATH)) return E_INVALIDARG;
    if (p[1] && !is_folder(p)) return HR_WIN32(ERROR_FILE_NOT_FOUND);
    Folder *g = new_folder(p);
    *out = g ? &g->o : 0;
    return g ? S_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE folder_folders(Folder *f, LONG flags, Obj **out)
{
    (void)flags;
    LONG n;
    if (!out) return E_POINTER;
    *out = 0;
    WCHAR *names = list_folder(f->path, TRUE, &n);
    Collection *c = names ? new_collection(&IID_ITaskFolderCollection_, n) : 0;
    if (c) {
        for (WCHAR *s = names; *s && c->n < n; s += wlen(s) + 1) {
            WCHAR p[MAX_PATH];
            if (join_path(f->path, s, p, MAX_PATH)) { Folder *g = new_folder(p); if (g) c->items[c->n++] = &g->o; }
        }
    }
    zfree(names);
    *out = c ? &c->o : 0;
    return c ? S_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE folder_create(Folder *f, BSTR name, VARIANT sddl, Obj **out)
{
    (void)sddl;
    WCHAR p[MAX_PATH], d[2 * MAX_PATH];
    NoRedirect r;
    if (out) *out = 0;
    if (!join_path(f->path, name, p, MAX_PATH) || !p[1]) return E_INVALIDARG;
    if (is_folder(p) || is_task(p)) return HR_WIN32(ERROR_ALREADY_EXISTS);
    make_parents(p);
    store_path(p, d);
    redirect_off(&r);
    BOOL ok = CreateDirectoryW(d, 0);
    DWORD e = GetLastError();
    redirect_on(&r);
    if (!ok) return HR_WIN32(e);
    WCHAR key[1024];
    HKEY k;
    cache_key(L"\\Tree", p, key);
    if (!RegCreateKeyExW(HKEY_LOCAL_MACHINE, key, 0, 0, 0, KEY_ALL_ACCESS, 0, &k, 0)) RegCloseKey(k);
    if (!out) return S_OK;
    Folder *g = new_folder(p);
    *out = g ? &g->o : 0;
    return g ? S_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE folder_delete(Folder *f, BSTR name, LONG flags)
{
    (void)flags;
    WCHAR p[MAX_PATH], d[2 * MAX_PATH];
    NoRedirect r;
    if (!join_path(f->path, name, p, MAX_PATH) || !p[1]) return E_INVALIDARG;
    if (!is_folder(p)) return HR_WIN32(ERROR_FILE_NOT_FOUND);
    store_path(p, d);
    redirect_off(&r);
    BOOL ok = RemoveDirectoryW(d);                                  /* only an empty folder goes */
    DWORD e = GetLastError();
    redirect_on(&r);
    if (!ok) return HR_WIN32(e);
    WCHAR key[1024];
    cache_key(L"\\Tree", p, key);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, key);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE folder_get_task(Folder *f, BSTR name, Obj **out)
{
    WCHAR p[MAX_PATH];
    if (!out) return E_POINTER;
    *out = 0;
    if (!name || !join_path(f->path, name, p, MAX_PATH) || !p[1]) return E_INVALIDARG;
    if (!is_task(p)) return HR_WIN32(ERROR_FILE_NOT_FOUND);
    Task *t = new_task(p);
    *out = t ? &t->o : 0;
    return t ? S_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE folder_tasks(Folder *f, LONG flags, Obj **out)
{
    (void)flags;                                    /* TASK_ENUM_HIDDEN: none is hidden from the list */
    LONG n;
    if (!out) return E_POINTER;
    *out = 0;
    WCHAR *names = list_folder(f->path, FALSE, &n);
    Collection *c = names ? new_collection(&IID_IRegisteredTaskCollection_, n) : 0;
    if (c) {
        for (WCHAR *s = names; *s && c->n < n; s += wlen(s) + 1) {
            WCHAR p[MAX_PATH];
            if (join_path(f->path, s, p, MAX_PATH)) { Task *t = new_task(p); if (t) c->items[c->n++] = &t->o; }
        }
    }
    zfree(names);
    *out = c ? &c->o : 0;
    return c ? S_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE folder_delete_task(Folder *f, BSTR name, LONG flags)
{
    (void)flags;
    WCHAR p[MAX_PATH];
    if (!name || !join_path(f->path, name, p, MAX_PATH) || !p[1]) return E_INVALIDARG;
    return delete_task(p);
}

/* RegisterTask: TASK_CREATE fails on an existing task, TASK_UPDATE on a
 * missing one, TASK_VALIDATE_ONLY only checks, TASK_DISABLE registers it
 * disabled.  A task without a name gets a GUID for one. */
static HRESULT STDMETHODCALLTYPE folder_register(Folder *f, BSTR name, BSTR xml, LONG flags, VARIANT user, VARIANT pw,
                                                 int logon, VARIANT sddl, Obj **out)
{
    (void)user; (void)pw; (void)logon; (void)sddl;
    WCHAR p[MAX_PATH], g[40];
    if (out) *out = 0;
    if (!xml) return E_INVALIDARG;
    if (!name || !name[0]) {
        GUID id;
        CoCreateGuid(&id);
        guid_string(&id, g);
        name = g;
    }
    if (!join_path(f->path, name, p, MAX_PATH) || !p[1]) return E_INVALIDARG;
    if (!valid_task_xml(xml)) return SCHED_E_MALFORMEDXML;
    if (flags & TASK_VALIDATE_ONLY) return S_OK;
    BOOL exists = is_task(p);
    if (is_folder(p)) return HR_WIN32(ERROR_ALREADY_EXISTS);
    if (exists && !(flags & TASK_UPDATE)) return HR_WIN32(ERROR_ALREADY_EXISTS);
    if (!exists && !(flags & TASK_CREATE)) return HR_WIN32(ERROR_FILE_NOT_FOUND);
    WCHAR *disabled = (flags & TASK_DISABLE) ? set_enabled(xml, FALSE) : 0;
    HRESULT hr = write_task(p, disabled ? disabled : xml);
    zfree(disabled);
    if (FAILED(hr) || !out) return hr;
    Task *t = new_task(p);
    *out = t ? &t->o : 0;
    return t ? S_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE folder_register_def(Folder *f, BSTR name, Obj *def, LONG flags, VARIANT user,
                                                     VARIANT pw, int logon, VARIANT sddl, Obj **out)
{
    if (out) *out = 0;
    if (!def) return E_INVALIDARG;
    BSTR xml = 0;
    HRESULT hr = ((HRESULT (STDMETHODCALLTYPE *)(Obj *, BSTR *))def->vtbl[19])(def, &xml);   /* get_XmlText */
    if (SUCCEEDED(hr)) hr = folder_register(f, name, xml, flags, user, pw, logon, sddl, out);
    SysFreeString(xml);
    return hr;
}
static HRESULT STDMETHODCALLTYPE folder_get_sd(Folder *f, LONG info, BSTR *out) { (void)f; (void)info; return put_bstr(L"O:BAG:SYD:(A;;FA;;;BA)(A;;FA;;;SY)(A;;FRFX;;;WD)", out); }
static HRESULT STDMETHODCALLTYPE folder_set_sd(Folder *f, BSTR sddl, LONG flags) { (void)f; (void)flags; return sddl ? S_OK : E_INVALIDARG; }
static const void *const folder_vtbl[] = { OBJ_BASE, folder_name, folder_path, folder_get_folder, folder_folders,
    folder_create, folder_delete, folder_get_task, folder_tasks, folder_delete_task, folder_register,
    folder_register_def, folder_get_sd, folder_set_sd };

static Folder *new_folder(const WCHAR *path)
{
    Folder *f = new_obj(sizeof(Folder), folder_vtbl, &IID_ITaskFolder_, 0);
    if (f) lstrcpynW(f->path, path, MAX_PATH);
    return f;
}

/* ---- ITaskService ---- */
typedef struct { Obj o; BOOL connected; } Service;

static HRESULT STDMETHODCALLTYPE svc_get_folder(Service *s, BSTR path, Obj **out)
{
    WCHAR p[MAX_PATH];
    if (!out) return E_POINTER;
    *out = 0;
    if (!s->connected) return HR_WIN32(ERROR_ONLY_IF_CONNECTED);
    if (!join_path(L"\\", path && path[0] ? path : L"\\", p, MAX_PATH)) return E_INVALIDARG;
    if (p[1] && !is_folder(p)) return HR_WIN32(ERROR_FILE_NOT_FOUND);
    Folder *f = new_folder(p);
    *out = f ? &f->o : 0;
    return f ? S_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE svc_running(Service *s, LONG flags, Obj **out)
{
    (void)flags;
    if (!out) return E_POINTER;
    *out = 0;
    if (!s->connected) return HR_WIN32(ERROR_ONLY_IF_CONNECTED);
    Collection *c = new_collection(&IID_IRunningTaskCollection_, 0);
    *out = c ? &c->o : 0;
    return c ? S_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE svc_new_task(Service *s, DWORD flags, void **out)
{
    if (!out) return E_POINTER;
    *out = 0;
    if (flags) return E_INVALIDARG;
    if (!s->connected) return HR_WIN32(ERROR_ONLY_IF_CONNECTED);
    return new_definition(0, out);
}
/* only this computer: a server name other than its own is refused */
static HRESULT STDMETHODCALLTYPE svc_connect(Service *s, VARIANT server, VARIANT user, VARIANT domain, VARIANT pw)
{
    (void)user; (void)domain; (void)pw;
    if (V_VT(&server) == VT_BSTR && V_BSTR(&server) && V_BSTR(&server)[0]) {
        WCHAR me[MAX_COMPUTERNAME_LENGTH + 1];
        DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
        const WCHAR *name = V_BSTR(&server);
        while (*name == '\\') name++;
        if (GetComputerNameW(me, &n) && lstrcmpiW(name, me) && lstrcmpiW(name, L"localhost") && lstrcmpW(name, L"."))
            return HR_WIN32(ERROR_BAD_NETPATH);
    }
    s->connected = TRUE;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE svc_connected(Service *s, VARIANT_BOOL *out) { if (!out) return E_POINTER; *out = s->connected ? VARIANT_TRUE : VARIANT_FALSE; return S_OK; }
static HRESULT STDMETHODCALLTYPE svc_server(Service *s, BSTR *out)
{
    WCHAR me[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    if (!s->connected) return HR_WIN32(ERROR_ONLY_IF_CONNECTED);
    if (!GetComputerNameW(me, &n)) me[0] = 0;
    return put_bstr(me, out);
}
static HRESULT STDMETHODCALLTYPE svc_user(Service *s, BSTR *out)
{
    WCHAR me[256];
    DWORD n = 256;
    if (!s->connected) return HR_WIN32(ERROR_ONLY_IF_CONNECTED);
    if (!GetUserNameW(me, &n)) me[0] = 0;
    return put_bstr(me, out);
}
static HRESULT STDMETHODCALLTYPE svc_domain(Service *s, BSTR *out)
{
    WCHAR me[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    if (!s->connected) return HR_WIN32(ERROR_ONLY_IF_CONNECTED);
    if (!GetComputerNameW(me, &n)) me[0] = 0;
    return put_bstr(me, out);
}
/* Windows 10's Task Scheduler is version 1.6 */
static HRESULT STDMETHODCALLTYPE svc_version(Service *s, DWORD *out)
{
    if (!out) return E_POINTER;
    if (!s->connected) return HR_WIN32(ERROR_ONLY_IF_CONNECTED);
    *out = 0x10006;
    return S_OK;
}
static const void *const svc_vtbl[] = { OBJ_BASE, svc_get_folder, svc_running, svc_new_task, svc_connect,
    svc_connected, svc_server, svc_user, svc_domain, svc_version };

/* ---------------------------------------------------------------------------
 * The class factory and the DLL's exports
 * ------------------------------------------------------------------------- */
typedef struct { const void *const *vtbl; } Factory;
static HRESULT STDMETHODCALLTYPE cf_qi(Factory *f, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IClassFactory)) { *ppv = f; return S_OK; }
    *ppv = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE cf_addref(Factory *f) { (void)f; return 2; }
static ULONG STDMETHODCALLTYPE cf_release(Factory *f) { (void)f; return 1; }
static HRESULT STDMETHODCALLTYPE cf_create(Factory *f, IUnknown *outer, REFIID riid, void **ppv)
{
    (void)f;
    if (!ppv) return E_POINTER;
    *ppv = 0;
    if (outer) return CLASS_E_NOAGGREGATION;
    Service *s = new_obj(sizeof(Service), svc_vtbl, &IID_ITaskService_, 0);
    if (!s) return E_OUTOFMEMORY;
    HRESULT hr = obj_qi(&s->o, riid, ppv);
    obj_release(&s->o);
    return hr;
}
static HRESULT STDMETHODCALLTYPE cf_lock(Factory *f, BOOL lock)
{
    (void)f;
    if (lock) InterlockedIncrement(&g_locks); else InterlockedDecrement(&g_locks);
    return S_OK;
}
static const void *const cf_vtbl[] = { cf_qi, cf_addref, cf_release, cf_create, cf_lock };
static Factory g_factory = { cf_vtbl };

TSAPI HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = 0;
    if (!IsEqualCLSID(clsid, &CLSID_TaskScheduler_)) return CLASS_E_CLASSNOTAVAILABLE;
    return cf_qi(&g_factory, riid, ppv);
}

TSAPI HRESULT WINAPI DllCanUnloadNow(void) { return g_objects || g_locks ? S_FALSE : S_OK; }
