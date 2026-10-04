/*
 * version.dll — file version information: the RT_VERSION resource of a
 * PE file (read from disk) and the queries into it.
 *
 * The block is VS_VERSIONINFO as stored in the file: a tree of nodes
 * { WORD wLength, wValueLength, wType; WCHAR szKey[]; (pad to 4) value;
 *   (pad to 4) children }.  For the A functions the block is returned twice
 * as large; strings queried through VerQueryValueA are converted into the
 * second half.
 */

#include <windows.h>

#define VERAPI __declspec(dllexport)

static int wlen(const WCHAR *s) { int n = 0; while (s[n]) n++; return n; }

/* Read the whole file.  A bare name ("kernel32.dll") is found the way
 * Windows finds it (it loads the file as a data DLL): along the search
 * path, the program's folder and the system folder among it. */
static BYTE *read_file(LPCWSTR name, DWORD *size)
{
    WCHAR found[MAX_PATH];
    const WCHAR *c = name;
    while (*c && *c != '\\' && *c != '/' && *c != ':') c++;
    if (!*c && SearchPathW(0, name, 0, MAX_PATH, found, 0)) name = found;
    HANDLE h = CreateFileW(name, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD n = GetFileSize(h, 0), got = 0;
    BYTE *b = n && n != INVALID_FILE_SIZE ? LocalAlloc(0, n) : 0;
    if (b && (!ReadFile(h, b, n, &got, 0) || got != n)) { LocalFree(b); b = 0; }
    CloseHandle(h);
    *size = n;
    if (!b) SetLastError(1812 /* ERROR_RESOURCE_DATA_NOT_FOUND */);
    return b;
}

/* The version resource's bytes inside the file image @f, or 0 */
static const BYTE *find_version(const BYTE *f, DWORD size, DWORD *len)
{
    if (size < 0x40 || f[0] != 'M' || f[1] != 'Z') return 0;
    DWORD pe = *(const DWORD *)(f + 0x3C);
    if (pe + 0x108 > size || *(const DWORD *)(f + pe) != 0x4550) return 0;
    WORD nsec = *(const WORD *)(f + pe + 6), optsz = *(const WORD *)(f + pe + 20);
    const BYTE *opt = f + pe + 24;
    WORD magic = *(const WORD *)opt;
    DWORD dd = magic == 0x20B ? 112 : 96;                   /* data directories: PE32+ / PE32 */
    DWORD res_rva = *(const DWORD *)(opt + dd + 8 * 2);
    if (!res_rva) return 0;
    const BYTE *sec = opt + optsz;
    /* RVA -> file offset */
    #define OFF(rva) ({ DWORD r_ = (rva), o_ = 0xFFFFFFFF; \
        for (WORD i_ = 0; i_ < nsec; i_++) { const BYTE *s_ = sec + 40 * i_; DWORD va_ = *(const DWORD *)(s_ + 12), \
            vs_ = *(const DWORD *)(s_ + 8), raw_ = *(const DWORD *)(s_ + 20), rs_ = *(const DWORD *)(s_ + 16); \
            if (vs_ < rs_) vs_ = rs_; \
            if (r_ >= va_ && r_ < va_ + vs_) { o_ = raw_ + (r_ - va_); break; } } o_; })
    DWORD root = OFF(res_rva);
    if (root >= size) return 0;
    /* three levels: type (16) -> first name -> first language */
    DWORD dir = root;
    for (int level = 0; level < 3; level++) {
        if (dir + 16 > size) return 0;
        WORD named = *(const WORD *)(f + dir + 12), ids = *(const WORD *)(f + dir + 14);
        const BYTE *e = f + dir + 16;
        DWORD target = 0xFFFFFFFF;
        for (int i = 0; i < named + ids; i++, e += 8) {
            if ((const BYTE *)e + 8 > f + size) return 0;
            DWORD id = *(const DWORD *)e, off = *(const DWORD *)(e + 4);
            if (level == 0 && (i < named || id != 16)) continue;   /* RT_VERSION */
            target = off;
            break;
        }
        if (target == 0xFFFFFFFF) return 0;
        if (level < 2) {
            if (!(target & 0x80000000)) return 0;
            dir = root + (target & 0x7FFFFFFF);
        } else {
            DWORD de = root + target;                               /* IMAGE_RESOURCE_DATA_ENTRY */
            if (de + 16 > size) return 0;
            DWORD rva = *(const DWORD *)(f + de), n = *(const DWORD *)(f + de + 4), off = OFF(rva);
            if (off >= size || n > size - off) return 0;
            *len = n;
            return f + off;
        }
    }
    return 0;
    #undef OFF
}

VERAPI DWORD WINAPI GetFileVersionInfoSizeW(LPCWSTR name, LPDWORD handle)
{
    if (handle) *handle = 0;
    DWORD size, len;
    BYTE *f = read_file(name, &size);
    if (!f) return 0;
    const BYTE *v = find_version(f, size, &len);
    LocalFree(f);
    if (!v) { SetLastError(1813 /* ERROR_RESOURCE_TYPE_NOT_FOUND */); return 0; }
    return len * 2;                                         /* room for VerQueryValueA's conversions */
}

VERAPI BOOL WINAPI GetFileVersionInfoW(LPCWSTR name, DWORD handle, DWORD n, LPVOID data)
{
    (void)handle;
    DWORD size, len;
    BYTE *f = read_file(name, &size);
    if (!f) return FALSE;
    const BYTE *v = find_version(f, size, &len);
    if (!v) { LocalFree(f); SetLastError(1813); return FALSE; }
    DWORD k = len < n ? len : n;
    for (DWORD i = 0; i < k; i++) ((BYTE *)data)[i] = v[i];
    if (n > len) for (DWORD i = len; i < n; i++) ((BYTE *)data)[i] = 0;
    LocalFree(f);
    return TRUE;
}

static WCHAR *a2w_path(LPCSTR s, WCHAR *w) { MultiByteToWideChar(CP_UTF8, 0, s, -1, w, MAX_PATH); return w; }

VERAPI DWORD WINAPI GetFileVersionInfoSizeA(LPCSTR name, LPDWORD handle) { WCHAR w[MAX_PATH]; return GetFileVersionInfoSizeW(a2w_path(name, w), handle); }
VERAPI BOOL WINAPI GetFileVersionInfoA(LPCSTR name, DWORD h, DWORD n, LPVOID data) { WCHAR w[MAX_PATH]; return GetFileVersionInfoW(a2w_path(name, w), h, n, data); }
VERAPI DWORD WINAPI GetFileVersionInfoSizeExW(DWORD flags, LPCWSTR name, LPDWORD handle) { (void)flags; return GetFileVersionInfoSizeW(name, handle); }
VERAPI BOOL WINAPI GetFileVersionInfoExW(DWORD flags, LPCWSTR name, DWORD h, DWORD n, LPVOID data) { (void)flags; return GetFileVersionInfoW(name, h, n, data); }

/* ---- the node tree ---- */
typedef struct { WORD len, vlen, type; WCHAR key[1]; } Node;

static const BYTE *align4(const BYTE *base, const BYTE *p) { return base + ((p - base + 3) & ~3); }
static const BYTE *node_value(const BYTE *base, const Node *n) { return align4(base, (const BYTE *)(n->key + wlen(n->key) + 1)); }

static const Node *child(const BYTE *base, const Node *n, const WCHAR *key, int klen)
{
    const BYTE *end = (const BYTE *)n + n->len;
    const BYTE *c = node_value(base, n) + n->vlen * (n->type == 1 ? 2 : 1);
    c = align4(base, c);
    while (c + 6 < end) {
        const Node *k = (const Node *)c;
        if (!k->len) break;
        int i = 0;
        while (i < klen && k->key[i]) {
            WCHAR a = k->key[i], b = key[i];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
            i++;
        }
        if (i == klen && !k->key[i]) return k;
        c = align4(base, c + k->len);
    }
    return 0;
}

VERAPI BOOL WINAPI VerQueryValueW(LPCVOID block, LPCWSTR sub, LPVOID *out, PUINT len)
{
    const BYTE *base = block;
    const Node *n = (const Node *)base;
    if (!n || n->len < 6) { SetLastError(13 /* ERROR_INVALID_DATA */); return FALSE; }
    for (const WCHAR *p = sub; *p; ) {
        while (*p == '\\') p++;
        if (!*p) break;
        int k = 0;
        while (p[k] && p[k] != '\\') k++;
        n = child(base, n, p, k);
        if (!n) { SetLastError(1813); return FALSE; }
        p += k;
    }
    *out = (void *)node_value(base, n);
    if (len) *len = n->vlen;
    return n->vlen != 0 || n == (const Node *)base;
}

VERAPI BOOL WINAPI VerQueryValueA(LPCVOID block, LPCSTR sub, LPVOID *out, PUINT len)
{
    WCHAR w[256];
    MultiByteToWideChar(CP_UTF8, 0, sub, -1, w, 256);
    UINT l = 0;
    if (!VerQueryValueW(block, w, out, &l)) return FALSE;
    BOOL is_string = (sub[0] == '\\' || sub[0] == '/') && (sub[1] | 32) == 's';       /* \StringFileInfo\... */
    if (!is_string) { if (len) *len = l; return TRUE; }
    /* convert into the block's second half */
    const Node *root = block;
    BYTE *scratch = (BYTE *)block + root->len;
    static DWORD used;                                      /* per block would be better; strings are short */
    if (used > root->len - 256) used = 0;
    char *a = (char *)scratch + used;
    int k = WideCharToMultiByte(CP_UTF8, 0, *out, (int)l, a, 255, 0, 0);
    if (k <= 0) return FALSE;
    a[k] = 0;
    used += (DWORD)k + 1;
    *out = a;
    if (len) *len = (UINT)k;
    return TRUE;
}

VERAPI DWORD WINAPI VerLanguageNameW(DWORD lang, LPWSTR buf, DWORD n)
{
    const char *s = (lang & 0x3FF) == 9 ? "English (United States)" : lang == 0 ? "Language Neutral" : "Unknown language";
    int k = MultiByteToWideChar(CP_UTF8, 0, s, -1, 0, 0) - 1;
    if (buf && n) { int m = (int)n - 1 < k ? (int)n - 1 : k; MultiByteToWideChar(CP_UTF8, 0, s, m, buf, m); buf[m] = 0; }
    return (DWORD)k;
}

VERAPI DWORD WINAPI VerLanguageNameA(DWORD lang, LPSTR buf, DWORD n)
{
    WCHAR w[64];
    DWORD k = VerLanguageNameW(lang, w, 64);
    if (buf && n) WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)n, 0, 0);
    return k;
}

VERAPI DWORD WINAPI VerFindFileW(DWORD flags, LPCWSTR file, LPCWSTR windir, LPCWSTR appdir, LPWSTR cur, PUINT nc, LPWSTR dest, PUINT nd)
{
    (void)flags; (void)file; (void)windir;
    if (cur && *nc) cur[0] = 0;
    UINT k = appdir ? (UINT)wlen(appdir) : 0;
    if (dest && *nd > k) { for (UINT i = 0; i <= k; i++) dest[i] = appdir ? appdir[i] : 0; *nd = k; }
    *nc = 0;
    return 0;
}

VERAPI DWORD WINAPI VerInstallFileW(DWORD flags, LPCWSTR src, LPCWSTR dst, LPCWSTR srcdir, LPCWSTR dstdir, LPCWSTR cur, LPWSTR tmp, PUINT n)
{
    (void)flags; (void)src; (void)dst; (void)srcdir; (void)dstdir; (void)cur; (void)tmp; (void)n;
    return 0x10;                                            /* VIF_ACCESSVIOLATION: not supported */
}
