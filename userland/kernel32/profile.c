/*
 * profile.c — .ini files (GetPrivateProfileString and friends)
 *
 * The whole file is read, changed as lines and written back.  A file
 * named without a folder is in the Windows directory, as on Windows.
 * Files in UTF-16 (with a byte order mark) stay UTF-16; others are read
 * and written in UTF-8, which is ASCII for the files programs write.
 * Section and key names match without regard to case; values lose the
 * spaces around them and one pair of enclosing quotes.
 */
#include <windows.h>
#include <winternl.h>
#include "k32.h"

void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
#define ERROR_INVALID_DATA_ 13

#define K32 __declspec(dllexport)

typedef struct {
    WCHAR **line;
    int     n, cap;
    int     utf16;                       /* the file had a UTF-16LE byte order mark */
} Ini;

static void *zalloc_(SIZE_T n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }
static void zfree_(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

static WCHAR *wdup(const WCHAR *s, int n)
{
    if (n < 0) n = lstrlenW(s);
    WCHAR *d = zalloc_(((SIZE_T)n + 1) * sizeof(WCHAR));
    if (d) { memcpy(d, s, (SIZE_T)n * sizeof(WCHAR)); d[n] = 0; }
    return d;
}

static void ini_free(Ini *f)
{
    for (int i = 0; i < f->n; i++) zfree_(f->line[i]);
    zfree_(f->line);
    f->line = 0;
    f->n = f->cap = 0;
}

static BOOL ini_add(Ini *f, int at, WCHAR *s)
{
    if (!s) return FALSE;
    if (f->n == f->cap) {
        int cap = f->cap ? f->cap * 2 : 32;
        WCHAR **l = zalloc_((SIZE_T)cap * sizeof(WCHAR *));
        if (!l) { zfree_(s); return FALSE; }
        if (f->n) memcpy(l, f->line, (SIZE_T)f->n * sizeof(WCHAR *));
        zfree_(f->line);
        f->line = l;
        f->cap = cap;
    }
    memmove(&f->line[at + 1], &f->line[at], (SIZE_T)(f->n - at) * sizeof(WCHAR *));
    f->line[at] = s;
    f->n++;
    return TRUE;
}

static void ini_del(Ini *f, int at)
{
    zfree_(f->line[at]);
    memmove(&f->line[at], &f->line[at + 1], (SIZE_T)(f->n - at - 1) * sizeof(WCHAR *));
    f->n--;
}

/* The file's path: a bare name is in the Windows directory */
static void ini_path(LPCWSTR name, WCHAR *out, int cap)
{
    if (!name || !*name) name = L"win.ini";
    BOOL dir = FALSE;
    for (const WCHAR *c = name; *c; c++) if (*c == '\\' || *c == '/' || *c == ':') dir = TRUE;
    if (dir) { lstrcpynW(out, name, cap); return; }
    UINT n = GetWindowsDirectoryW(out, (UINT)cap);
    if (!n || (int)n + 2 + lstrlenW(name) > cap) { lstrcpynW(out, name, cap); return; }
    if (out[n - 1] != '\\') out[n++] = '\\';
    lstrcpynW(out + n, name, cap - (int)n);
}

static BOOL ini_load(LPCWSTR name, Ini *f)
{
    memset(f, 0, sizeof(*f));
    WCHAR path[MAX_PATH];
    ini_path(name, path, MAX_PATH);
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0,
                           OPEN_EXISTING, 0, 0);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    DWORD size = GetFileSize(h, 0), got = 0;
    if (size == INVALID_FILE_SIZE || size > 16 * 1024 * 1024) { CloseHandle(h); return FALSE; }
    BYTE *raw = zalloc_((SIZE_T)size + 2);
    if (!raw) { CloseHandle(h); return FALSE; }
    BOOL ok = ReadFile(h, raw, size, &got, 0);
    CloseHandle(h);
    if (!ok) { zfree_(raw); return FALSE; }
    WCHAR *text;
    int len;
    if (got >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
        f->utf16 = 1;
        text = (WCHAR *)(raw + 2);
        len = (int)(got - 2) / 2;
    } else {
        int skip = got >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF ? 3 : 0;
        len = MultiByteToWideChar(CP_UTF8, 0, (char *)raw + skip, (int)got - skip, 0, 0);
        text = zalloc_(((SIZE_T)len + 1) * sizeof(WCHAR));
        if (!text) { zfree_(raw); return FALSE; }
        MultiByteToWideChar(CP_UTF8, 0, (char *)raw + skip, (int)got - skip, text, len);
    }
    for (int i = 0; i < len;) {
        int j = i;
        while (j < len && text[j] != '\n' && text[j] != '\r') j++;
        ini_add(f, f->n, wdup(text + i, j - i));
        if (j < len && text[j] == '\r') j++;
        if (j < len && text[j] == '\n') j++;
        i = j;
    }
    if (!f->utf16) zfree_(text);
    zfree_(raw);
    return TRUE;
}

static BOOL ini_save(LPCWSTR name, Ini *f)
{
    WCHAR path[MAX_PATH];
    ini_path(name, path, MAX_PATH);
    SIZE_T chars = 0;
    for (int i = 0; i < f->n; i++) chars += (SIZE_T)lstrlenW(f->line[i]) + 2;
    WCHAR *text = zalloc_((chars + 2) * sizeof(WCHAR));
    if (!text) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    SIZE_T k = 0;
    if (f->utf16) text[k++] = 0xFEFF;
    for (int i = 0; i < f->n; i++) {
        int n = lstrlenW(f->line[i]);
        memcpy(text + k, f->line[i], (SIZE_T)n * sizeof(WCHAR));
        k += (SIZE_T)n;
        text[k++] = '\r';
        text[k++] = '\n';
    }
    void *out = text;
    DWORD bytes = (DWORD)(k * sizeof(WCHAR));
    char *u = 0;
    if (!f->utf16) {
        int n = WideCharToMultiByte(CP_UTF8, 0, text, (int)k, 0, 0, 0, 0);
        u = zalloc_((SIZE_T)n + 1);
        if (!u) { zfree_(text); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
        WideCharToMultiByte(CP_UTF8, 0, text, (int)k, u, n, 0, 0);
        out = u;
        bytes = (DWORD)n;
    }
    HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    BOOL ok = h != INVALID_HANDLE_VALUE;
    DWORD put = 0;
    if (ok) { ok = WriteFile(h, out, bytes, &put, 0) && put == bytes; CloseHandle(h); }
    zfree_(u);
    zfree_(text);
    return ok;
}

static int is_space(WCHAR c) { return c == ' ' || c == '\t'; }

/* [s, e) without the spaces around it */
static void trim(const WCHAR **s, const WCHAR **e)
{
    while (*s < *e && is_space(**s)) (*s)++;
    while (*e > *s && is_space((*e)[-1])) (*e)--;
}

static int name_eq(const WCHAR *s, const WCHAR *e, const WCHAR *name)
{
    int n = lstrlenW(name);
    const WCHAR *ns = name, *ne = name + n;
    trim(&ns, &ne);
    if (e - s != ne - ns) return 0;
    return CompareStringOrdinal(s, (int)(e - s), ns, (int)(ne - ns), TRUE) == CSTR_EQUAL;
}

/* A section header line: its name as [s, e) */
static int section_of(const WCHAR *line, const WCHAR **s, const WCHAR **e)
{
    while (is_space(*line)) line++;
    if (*line != '[') return 0;
    const WCHAR *b = line + 1, *c = b;
    while (*c && *c != ']') c++;
    *s = b; *e = c;
    trim(s, e);
    return 1;
}

static int is_comment(const WCHAR *line)
{
    while (is_space(*line)) line++;
    return *line == ';' || *line == 0;
}

/* A key line: name [ks, ke), value [vs, ve) */
static int key_of(const WCHAR *line, const WCHAR **ks, const WCHAR **ke, const WCHAR **vs, const WCHAR **ve)
{
    if (is_comment(line)) return 0;
    const WCHAR *eq = line;
    while (*eq && *eq != '=') eq++;
    *ks = line; *ke = eq;
    trim(ks, ke);
    if (*eq) { *vs = eq + 1; *ve = eq + 1 + lstrlenW(eq + 1); }
    else { *vs = *ve = eq; }
    trim(vs, ve);
    return 1;
}

/* The section's header line, or -1; *end: the line after its last */
static int find_section(Ini *f, const WCHAR *app, int *end)
{
    for (int i = 0; i < f->n; i++) {
        const WCHAR *s, *e;
        if (!section_of(f->line[i], &s, &e) || !name_eq(s, e, app)) continue;
        int j = i + 1;
        while (j < f->n && !section_of(f->line[j], &s, &e)) j++;
        if (end) *end = j;
        return i;
    }
    return -1;
}

static int find_key(Ini *f, int sec, int end, const WCHAR *key)
{
    for (int i = sec + 1; i < end; i++) {
        const WCHAR *ks, *ke, *vs, *ve;
        if (key_of(f->line[i], &ks, &ke, &vs, &ve) && name_eq(ks, ke, key)) return i;
    }
    return -1;
}

/* Copy a list of strings into @buf (each NUL-terminated, a NUL after the
 * last); a list cut short ends in two NULs and reports n - 2 */
typedef struct { WCHAR *buf; DWORD n, k; BOOL cut; } List;

static void list_add(List *l, const WCHAR *s, int len)
{
    if (l->cut || !l->buf) return;
    if (l->k + (DWORD)len + 2 > l->n) {
        DWORD room = l->n > l->k + 2 ? l->n - l->k - 2 : 0;
        memcpy(l->buf + l->k, s, room * sizeof(WCHAR));
        l->k += room;
        l->cut = TRUE;
        return;
    }
    memcpy(l->buf + l->k, s, (SIZE_T)len * sizeof(WCHAR));
    l->k += (DWORD)len;
    l->buf[l->k++] = 0;
}

static DWORD list_end(List *l)
{
    if (!l->buf || !l->n) return 0;
    if (l->n == 1) { l->buf[0] = 0; return 0; }
    if (l->cut) { l->buf[l->n - 2] = 0; l->buf[l->n - 1] = 0; return l->n - 2; }
    l->buf[l->k] = 0;
    if (!l->k && l->n > 1) l->buf[1] = 0;
    return l->k;
}

static DWORD copy_value(WCHAR *buf, DWORD n, const WCHAR *s, const WCHAR *e)
{
    if (!buf || !n) return 0;
    if (e - s >= 2 && ((*s == '"' && e[-1] == '"') || (*s == '\'' && e[-1] == '\''))) { s++; e--; }
    DWORD len = (DWORD)(e - s);
    if (len >= n) len = n - 1;
    memcpy(buf, s, len * sizeof(WCHAR));
    buf[len] = 0;
    return len;
}

K32 DWORD WINAPI GetPrivateProfileStringW(LPCWSTR app, LPCWSTR key, LPCWSTR def, LPWSTR buf, DWORD n, LPCWSTR file)
{
    Ini f;
    BOOL loaded = ini_load(file, &f);
    DWORD r;
    if (!app || !key) {
        List l = { buf, n, 0, FALSE };
        if (loaded && !app) {
            for (int i = 0; i < f.n; i++) {
                const WCHAR *s, *e;
                if (section_of(f.line[i], &s, &e)) list_add(&l, s, (int)(e - s));
            }
        } else if (loaded) {
            int end, sec = find_section(&f, app, &end);
            for (int i = sec + 1; sec >= 0 && i < end; i++) {
                const WCHAR *ks, *ke, *vs, *ve;
                if (key_of(f.line[i], &ks, &ke, &vs, &ve)) list_add(&l, ks, (int)(ke - ks));
            }
        }
        r = list_end(&l);
    } else {
        int end, sec = loaded ? find_section(&f, app, &end) : -1;
        int k = sec >= 0 ? find_key(&f, sec, end, key) : -1;
        if (k >= 0) {
            const WCHAR *ks, *ke, *vs, *ve;
            key_of(f.line[k], &ks, &ke, &vs, &ve);
            r = copy_value(buf, n, vs, ve);
        } else {
            const WCHAR *s = def ? def : L"", *e = s + lstrlenW(s);
            while (e > s && is_space(e[-1])) e--;                 /* (Windows trims the default's end) */
            r = buf && n ? (DWORD)((e - s) >= (int)n ? n - 1 : e - s) : 0;
            if (buf && n) { memcpy(buf, s, r * sizeof(WCHAR)); buf[r] = 0; }
        }
        SetLastError(k >= 0 ? 0 : ERROR_FILE_NOT_FOUND);
    }
    if (loaded) ini_free(&f);
    return r;
}

K32 UINT WINAPI GetPrivateProfileIntW(LPCWSTR app, LPCWSTR key, INT def, LPCWSTR file)
{
    WCHAR v[64];
    if (!app || !key || !GetPrivateProfileStringW(app, key, L"", v, 64, file)) return (UINT)def;
    const WCHAR *p = v;
    int neg = *p == '-';
    if (*p == '-' || *p == '+') p++;
    UINT x = 0;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        for (p += 2;; p++) {
            int d = *p >= '0' && *p <= '9' ? *p - '0' : (*p | 0x20) >= 'a' && (*p | 0x20) <= 'f' ? (*p | 0x20) - 'a' + 10 : -1;
            if (d < 0) break;
            x = x * 16 + (UINT)d;
        }
    } else {
        if (*p < '0' || *p > '9') return 0;
        for (; *p >= '0' && *p <= '9'; p++) x = x * 10 + (UINT)(*p - '0');
    }
    return neg ? (UINT)-(INT)x : x;
}

K32 DWORD WINAPI GetPrivateProfileSectionNamesW(LPWSTR buf, DWORD n, LPCWSTR file)
{
    return GetPrivateProfileStringW(0, 0, 0, buf, n, file);
}

K32 DWORD WINAPI GetPrivateProfileSectionW(LPCWSTR app, LPWSTR buf, DWORD n, LPCWSTR file)
{
    Ini f;
    List l = { buf, n, 0, FALSE };
    if (app && ini_load(file, &f)) {
        int end, sec = find_section(&f, app, &end);
        for (int i = sec + 1; sec >= 0 && i < end; i++) {
            if (is_comment(f.line[i])) continue;
            const WCHAR *s = f.line[i], *e = s + lstrlenW(s);
            trim(&s, &e);
            list_add(&l, s, (int)(e - s));
        }
        ini_free(&f);
    }
    return list_end(&l);
}

K32 BOOL WINAPI WritePrivateProfileStringW(LPCWSTR app, LPCWSTR key, LPCWSTR value, LPCWSTR file)
{
    if (!app) {                                    /* flush the cache: there is none */
        if (!key && !value) return TRUE;
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    Ini f;
    if (!ini_load(file, &f)) memset(&f, 0, sizeof(f));
    int end, sec = find_section(&f, app, &end);
    BOOL changed = FALSE;
    if (!key) {                                    /* remove the section */
        if (sec >= 0) { for (int i = end - 1; i >= sec; i--) ini_del(&f, i); changed = TRUE; }
    } else {
        int k = sec >= 0 ? find_key(&f, sec, end, key) : -1;
        if (!value) {
            if (k >= 0) { ini_del(&f, k); changed = TRUE; }
        } else {
            int kl = lstrlenW(key), vl = lstrlenW(value);
            WCHAR *line = zalloc_(((SIZE_T)kl + vl + 2) * sizeof(WCHAR));
            if (!line) { ini_free(&f); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
            memcpy(line, key, (SIZE_T)kl * sizeof(WCHAR));
            line[kl] = '=';
            memcpy(line + kl + 1, value, (SIZE_T)vl * sizeof(WCHAR));
            if (k >= 0) { zfree_(f.line[k]); f.line[k] = line; }
            else {
                if (sec < 0) {
                    int al = lstrlenW(app);
                    WCHAR *hdr = zalloc_(((SIZE_T)al + 3) * sizeof(WCHAR));
                    if (hdr) { hdr[0] = '['; memcpy(hdr + 1, app, (SIZE_T)al * sizeof(WCHAR)); hdr[al + 1] = ']'; }
                    ini_add(&f, f.n, hdr);
                    sec = f.n - 1;
                    end = f.n;
                }
                int at = end;                      /* after the section's last key (not its trailing blanks) */
                while (at > sec + 1 && !*f.line[at - 1]) at--;
                ini_add(&f, at, line);
            }
            changed = TRUE;
        }
    }
    BOOL ok = changed ? ini_save(file, &f) : TRUE;
    ini_free(&f);
    return ok;
}

K32 BOOL WINAPI WritePrivateProfileSectionW(LPCWSTR app, LPCWSTR data, LPCWSTR file)
{
    if (!app) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    Ini f;
    if (!ini_load(file, &f)) memset(&f, 0, sizeof(f));
    int end, sec = find_section(&f, app, &end);
    if (sec >= 0) for (int i = end - 1; i >= sec; i--) ini_del(&f, i);
    if (data) {
        int at = sec >= 0 ? sec : f.n;
        int al = lstrlenW(app);
        WCHAR *hdr = zalloc_(((SIZE_T)al + 3) * sizeof(WCHAR));
        if (hdr) { hdr[0] = '['; memcpy(hdr + 1, app, (SIZE_T)al * sizeof(WCHAR)); hdr[al + 1] = ']'; }
        ini_add(&f, at++, hdr);
        for (const WCHAR *p = data; *p; p += lstrlenW(p) + 1) ini_add(&f, at++, wdup(p, -1));
    }
    BOOL ok = ini_save(file, &f);
    ini_free(&f);
    return ok;
}

/* Structs: the bytes in hexadecimal, then a checksum byte (their sum) */
K32 BOOL WINAPI GetPrivateProfileStructW(LPCWSTR app, LPCWSTR key, LPVOID data, UINT size, LPCWSTR file)
{
    if (!app || !key || !data) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    DWORD cap = size * 2 + 4;
    WCHAR *v = zalloc_(cap * sizeof(WCHAR));
    if (!v) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    DWORD n = GetPrivateProfileStringW(app, key, L"", v, cap, file);
    BOOL ok = n == size * 2 + 2;
    BYTE sum = 0, *out = data;
    for (UINT i = 0; ok && i <= size; i++) {
        int b = 0;
        for (int k = 0; k < 2; k++) {
            WCHAR c = v[i * 2 + k];
            int d = c >= '0' && c <= '9' ? c - '0' : (c | 0x20) >= 'a' && (c | 0x20) <= 'f' ? (c | 0x20) - 'a' + 10 : -1;
            if (d < 0) { ok = FALSE; break; }
            b = b * 16 + d;
        }
        if (i < size) { out[i] = (BYTE)b; sum = (BYTE)(sum + b); }
        else if ((BYTE)b != sum) ok = FALSE;
    }
    zfree_(v);
    if (!ok) SetLastError(ERROR_INVALID_DATA_);
    return ok;
}

K32 BOOL WINAPI WritePrivateProfileStructW(LPCWSTR app, LPCWSTR key, LPVOID data, UINT size, LPCWSTR file)
{
    if (!app || !key) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!data) return WritePrivateProfileStringW(app, key, 0, file);
    WCHAR *v = zalloc_(((SIZE_T)size * 2 + 3) * sizeof(WCHAR));
    if (!v) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    static const char hex[] = "0123456789ABCDEF";
    BYTE sum = 0, *in = data;
    for (UINT i = 0; i < size; i++) {
        v[i * 2] = (WCHAR)hex[in[i] >> 4];
        v[i * 2 + 1] = (WCHAR)hex[in[i] & 15];
        sum = (BYTE)(sum + in[i]);
    }
    v[size * 2] = (WCHAR)hex[sum >> 4];
    v[size * 2 + 1] = (WCHAR)hex[sum & 15];
    BOOL ok = WritePrivateProfileStringW(app, key, v, file);
    zfree_(v);
    return ok;
}

/* -----------------------------------------------------------------------
 * The A versions (the ANSI code page)
 * ----------------------------------------------------------------------- */
static WCHAR *a2w(LPCSTR s)
{
    if (!s) return 0;
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, 0, 0);
    WCHAR *w = zalloc_((SIZE_T)n * sizeof(WCHAR));
    if (w) MultiByteToWideChar(CP_ACP, 0, s, -1, w, n);
    return w;
}

/* A list (@list: NUL-separated, @len characters before its final NUL) or
 * one value converted into @buf, with what the W call returned */
static DWORD a_result(const WCHAR *w, DWORD len, BOOL list, LPSTR buf, DWORD n)
{
    if (!buf || !n) return 0;
    if (!list) {
        if (!WideCharToMultiByte(CP_ACP, 0, w, -1, buf, (int)n, 0, 0)) buf[n - 1] = 0;
        return (DWORD)lstrlenA(buf);
    }
    int r = WideCharToMultiByte(CP_ACP, 0, w, (int)len + 1, buf, (int)n, 0, 0);
    if (r) return (DWORD)r - 1;
    if (n < 2) { buf[0] = 0; return 0; }
    buf[n - 2] = buf[n - 1] = 0;
    return n - 2;
}

K32 DWORD WINAPI GetPrivateProfileStringA(LPCSTR app, LPCSTR key, LPCSTR def, LPSTR buf, DWORD n, LPCSTR file)
{
    if (!buf || !n) return 0;
    WCHAR *wa = a2w(app), *wk = a2w(key), *wd = a2w(def), *wf = a2w(file);
    WCHAR *wb = zalloc_((SIZE_T)n * sizeof(WCHAR));
    DWORD r = wb ? GetPrivateProfileStringW(wa, wk, wd, wb, n, wf) : 0;
    DWORD out = wb ? a_result(wb, r, !app || !key, buf, n) : 0;
    zfree_(wa); zfree_(wk); zfree_(wd); zfree_(wf); zfree_(wb);
    return out;
}

K32 UINT WINAPI GetPrivateProfileIntA(LPCSTR app, LPCSTR key, INT def, LPCSTR file)
{
    WCHAR *wa = a2w(app), *wk = a2w(key), *wf = a2w(file);
    UINT r = GetPrivateProfileIntW(wa, wk, def, wf);
    zfree_(wa); zfree_(wk); zfree_(wf);
    return r;
}

K32 DWORD WINAPI GetPrivateProfileSectionNamesA(LPSTR buf, DWORD n, LPCSTR file)
{
    return GetPrivateProfileStringA(0, 0, 0, buf, n, file);
}

K32 DWORD WINAPI GetPrivateProfileSectionA(LPCSTR app, LPSTR buf, DWORD n, LPCSTR file)
{
    if (!buf || !n) return 0;
    WCHAR *wa = a2w(app), *wf = a2w(file);
    WCHAR *wb = zalloc_((SIZE_T)n * sizeof(WCHAR));
    DWORD r = wb ? GetPrivateProfileSectionW(wa, wb, n, wf) : 0;
    DWORD out = wb ? a_result(wb, r, TRUE, buf, n) : 0;
    zfree_(wa); zfree_(wf); zfree_(wb);
    return out;
}

K32 BOOL WINAPI WritePrivateProfileStringA(LPCSTR app, LPCSTR key, LPCSTR value, LPCSTR file)
{
    WCHAR *wa = a2w(app), *wk = a2w(key), *wv = a2w(value), *wf = a2w(file);
    BOOL r = WritePrivateProfileStringW(wa, wk, wv, wf);
    zfree_(wa); zfree_(wk); zfree_(wv); zfree_(wf);
    return r;
}

K32 BOOL WINAPI WritePrivateProfileSectionA(LPCSTR app, LPCSTR data, LPCSTR file)
{
    WCHAR *wa = a2w(app), *wf = a2w(file), *wd = 0;
    if (data) {
        int len = 0;
        while (data[len] || data[len + 1]) len++;
        len += 2;
        int n = MultiByteToWideChar(CP_ACP, 0, data, len, 0, 0);
        wd = zalloc_((SIZE_T)n * sizeof(WCHAR));
        if (wd) MultiByteToWideChar(CP_ACP, 0, data, len, wd, n);
    }
    BOOL r = WritePrivateProfileSectionW(wa, data ? wd : 0, wf);
    zfree_(wa); zfree_(wf); zfree_(wd);
    return r;
}

K32 BOOL WINAPI GetPrivateProfileStructA(LPCSTR app, LPCSTR key, LPVOID data, UINT size, LPCSTR file)
{
    WCHAR *wa = a2w(app), *wk = a2w(key), *wf = a2w(file);
    BOOL r = GetPrivateProfileStructW(wa, wk, data, size, wf);
    zfree_(wa); zfree_(wk); zfree_(wf);
    return r;
}

K32 BOOL WINAPI WritePrivateProfileStructA(LPCSTR app, LPCSTR key, LPVOID data, UINT size, LPCSTR file)
{
    WCHAR *wa = a2w(app), *wk = a2w(key), *wf = a2w(file);
    BOOL r = WritePrivateProfileStructW(wa, wk, data, size, wf);
    zfree_(wa); zfree_(wk); zfree_(wf);
    return r;
}
