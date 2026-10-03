/*
 * path.c — shlwapi.dll's path and string functions (A and W from the one
 * template in path_t.h), byte-size formatting and wnsprintf.
 */

#define NOVA_BUILD_SHLWAPI
#include <windows.h>
#include <stdarg.h>

/* lower case for a UTF-16 code unit (ASCII, Latin-1, Greek, Cyrillic) */
static WCHAR CharLowerOne(WCHAR c)
{
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 32;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    return c;
}

static void lstrcpyW_(WCHAR *d, const WCHAR *s) { while ((*d++ = *s++)) ; }

#define T char
#define F(n) n##A
#define FIND_T WIN32_FIND_DATAA
#define FIND_FIRST FindFirstFileA
#define FIND_NEXT FindNextFileA
#include "path_t.h"
#undef T
#undef F
#undef FIND_T
#undef FIND_FIRST
#undef FIND_NEXT

#define T WCHAR
#define F(n) n##W
#define FIND_T WIN32_FIND_DATAW
#define FIND_FIRST FindFirstFileW
#define FIND_NEXT FindNextFileW
#include "path_t.h"
#undef T
#undef F

/* The file system (these go through kernel32) */
LWSTDAPI_(BOOL) PathFileExistsA(LPCSTR p) { return p && GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }
LWSTDAPI_(BOOL) PathFileExistsW(LPCWSTR p) { return p && GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

LWSTDAPI_(BOOL) PathIsDirectoryA(LPCSTR p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) ? FILE_ATTRIBUTE_DIRECTORY : FALSE;
}

LWSTDAPI_(BOOL) PathIsDirectoryW(LPCWSTR p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) ? FILE_ATTRIBUTE_DIRECTORY : FALSE;
}

LWSTDAPI_(LPSTR) PathBuildRootA(LPSTR out, int drive) { PathBuildRoot_A(out, drive); return out; }
LWSTDAPI_(LPWSTR) PathBuildRootW(LPWSTR out, int drive) { PathBuildRoot_W(out, drive); return out; }

/* Find @file in the directories of %PATH% (or @dirs first) */
LWSTDAPI_(BOOL) PathFindOnPathW(LPWSTR file, LPCWSTR *dirs)
{
    WCHAR cand[MAX_PATH * 2];
    for (int i = 0; dirs && dirs[i]; i++)
        if (PathCombineW(cand, dirs[i], file) && PathFileExistsW(cand)) { lstrcpyW_(file, cand); return TRUE; }
    WCHAR path[2048];
    DWORD n = GetEnvironmentVariableW((const WCHAR[]){ 'P', 'A', 'T', 'H', 0 }, path, 2048);
    if (!n || n >= 2048) return FALSE;
    for (WCHAR *p = path; *p; ) {
        WCHAR *e = p;
        while (*e && *e != ';') e++;
        WCHAR save = *e;
        *e = 0;
        if (*p && PathCombineW(cand, p, file) && PathFileExistsW(cand)) { lstrcpyW_(file, cand); return TRUE; }
        if (!save) break;
        p = e + 1;
    }
    return FALSE;
}

LWSTDAPI_(BOOL) PathFindOnPathA(LPSTR file, LPCSTR *dirs)
{
    (void)dirs;
    WCHAR w[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, file, -1, w, MAX_PATH);
    if (!PathFindOnPathW(w, 0)) return FALSE;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, file, MAX_PATH, 0, 0);
    return TRUE;
}

/* Relative path from @from to @to (both absolute, same root) */
LWSTDAPI_(BOOL) PathRelativePathToW(LPWSTR out, LPCWSTR from, DWORD fattr, LPCWSTR to, DWORD tattr)
{
    WCHAR f[MAX_PATH], t[MAX_PATH];
    lstrcpyW_(f, from);
    lstrcpyW_(t, to);
    if (!(fattr & FILE_ATTRIBUTE_DIRECTORY)) PathRemoveFileSpecW(f);
    (void)tattr;
    if (!PathIsSameRootW(f, t)) return FALSE;
    int common = PathCommonPrefixW(f, t, 0);
    int o = 0;
    out[0] = 0;
    /* one ".." per remaining component of @from */
    const WCHAR *rest = f + common;
    while (*rest == '\\') rest++;
    if (*rest) {
        for (const WCHAR *c = rest; ; c++) {
            if (!*c || *c == '\\') { if (o) out[o++] = '\\'; out[o++] = '.'; out[o++] = '.'; }
            if (!*c) break;
        }
    } else {
        out[o++] = '.';
    }
    const WCHAR *tr = t + common;
    while (*tr == '\\') tr++;
    if (*tr) { out[o++] = '\\'; while (*tr && o < MAX_PATH - 1) out[o++] = *tr++; }
    out[o] = 0;
    return TRUE;
}

LWSTDAPI_(BOOL) PathSearchAndQualifyW(LPCWSTR p, LPWSTR out, UINT n)
{
    return GetFullPathNameW(p, n, out, 0) != 0;
}

LWSTDAPI_(BOOL) PathCompactPathExW(LPWSTR out, LPCWSTR p, UINT max, DWORD flags)
{
    (void)flags;
    UINT n = (UINT)lstrlenW(p);
    if (n < max) { lstrcpyW_(out, p); return TRUE; }
    if (max < 4) { if (max) out[0] = 0; return FALSE; }
    UINT keep = max - 4;                        /* "...\" + tail */
    const WCHAR *tail = p + n - keep;
    out[0] = out[1] = out[2] = '.';
    UINT i = 0;
    for (; tail[i]; i++) out[3 + i] = tail[i];
    out[3 + i] = 0;
    return TRUE;
}

/* PathCompactPath: fit a path into `dx` pixels of the DC's font, "..." standing for the front */
LWSTDAPI_(BOOL) PathCompactPathW(HDC hdc, LPWSTR p, UINT dx)
{
    if (!p) return FALSE;
    HDC dc = hdc ? hdc : GetDC(0);
    SIZE sz;
    int n = lstrlenW(p);
    BOOL ok = TRUE;
    if (GetTextExtentPoint32W(dc, p, n, &sz) && (UINT)sz.cx > dx) {
        /* keep the last component and as much of its parent as fits after "..." */
        WCHAR buf[MAX_PATH + 4];
        int cut = 1;
        for (; cut < n; cut++) {
            buf[0] = buf[1] = buf[2] = '.';
            lstrcpyW_(buf + 3, p + cut);
            if (GetTextExtentPoint32W(dc, buf, lstrlenW(buf), &sz) && (UINT)sz.cx <= dx) break;
        }
        if (cut >= n) { buf[0] = buf[1] = buf[2] = '.'; buf[3] = 0; ok = FALSE; }
        lstrcpyW_(p, buf);
    }
    if (!hdc) ReleaseDC(0, dc);
    return ok;
}
LWSTDAPI_(BOOL) PathCompactPathA(HDC hdc, LPSTR p, UINT dx)
{
    WCHAR w[MAX_PATH + 4];
    if (!p || !MultiByteToWideChar(CP_ACP, 0, p, -1, w, MAX_PATH)) return FALSE;
    BOOL r = PathCompactPathW(hdc, w, dx);
    WideCharToMultiByte(CP_ACP, 0, w, -1, p, MAX_PATH, 0, 0);
    return r;
}

/* what a character may be in a path (GCT_*) */
LWSTDAPI_(UINT) PathGetCharTypeW(WCHAR c)
{
    if (c < ' ' || c == '"' || c == '<' || c == '>' || c == '|' || c == '/') return 0;   /* GCT_INVALID */
    if (c == '*' || c == '?') return 4;                                                 /* GCT_WILD */
    if (c == '\\' || c == ':') return 8;                                                /* GCT_SEPARATOR */
    UINT t = 1;                                                                         /* GCT_LFNCHAR */
    if (c >= 0x80 || !(c == ' ' || c == '+' || c == ',' || c == ';' || c == '=' || c == '[' || c == ']'))
        t |= 2;                                                                         /* GCT_SHORTCHAR */
    return t;
}
LWSTDAPI_(UINT) PathGetCharTypeA(UCHAR c) { return PathGetCharTypeW(c); }

/* ---- byte sizes ---- */
LWSTDAPI_(LPWSTR) StrFormatByteSizeW(LONGLONG v, LPWSTR buf, UINT n) { return format_size_W((ULONGLONG)v, buf, n); }
LWSTDAPI_(LPSTR) StrFormatByteSize64A(LONGLONG v, LPSTR buf, UINT n)  { return format_size_A((ULONGLONG)v, buf, n); }
LWSTDAPI_(LPSTR) StrFormatByteSizeA(DWORD v, LPSTR buf, UINT n)       { return format_size_A(v, buf, n); }
LWSTDAPI_(LPWSTR) StrFormatKBSizeW(LONGLONG v, LPWSTR buf, UINT n)
{
    WCHAR tmp[32];
    ULONGLONG kb = ((ULONGLONG)v + 1023) / 1024;
    int k = 0;
    char d[24];
    int dn = 0;
    do { d[dn++] = (char)('0' + kb % 10); kb /= 10; } while (kb);
    for (int i = dn - 1; i >= 0; i--) { tmp[k++] = (WCHAR)d[i]; if (i && i % 3 == 0) tmp[k++] = ','; }
    tmp[k++] = ' '; tmp[k++] = 'K'; tmp[k++] = 'B'; tmp[k] = 0;
    UINT i = 0;
    for (; i + 1 < n && tmp[i]; i++) buf[i] = tmp[i];
    if (n) buf[i] = 0;
    return buf;
}

/* Natural order: digit runs compare as numbers ("file2" < "file10") */
LWSTDAPI_(int) StrCmpLogicalW(LPCWSTR a, LPCWSTR b)
{
    while (*a && *b) {
        if (*a >= '0' && *a <= '9' && *b >= '0' && *b <= '9') {
            while (*a == '0') a++;
            while (*b == '0') b++;
            const WCHAR *ea = a, *eb = b;
            while (*ea >= '0' && *ea <= '9') ea++;
            while (*eb >= '0' && *eb <= '9') eb++;
            if (ea - a != eb - b) return ea - a < eb - b ? -1 : 1;
            for (; a < ea; a++, b++) if (*a != *b) return *a < *b ? -1 : 1;
            continue;
        }
        WCHAR x = CharLowerOne(*a >= 'A' && *a <= 'Z' ? *a + 32 : *a), y = CharLowerOne(*b >= 'A' && *b <= 'Z' ? *b + 32 : *b);
        if (x != y) return x < y ? -1 : 1;
        a++; b++;
    }
    return *a ? 1 : *b ? -1 : 0;
}

/* ---- printf-style (through the C runtime) ---- */
__declspec(dllimport) int _vsnprintf(char *s, size_t n, const char *fmt, va_list ap);
__declspec(dllimport) int _vsnwprintf(wchar_t *s, size_t n, const wchar_t *fmt, va_list ap);

LWSTDAPI_(int) wvnsprintfA(LPSTR buf, int n, LPCSTR fmt, va_list ap)
{
    if (n <= 0) return -1;
    int r = _vsnprintf(buf, (size_t)n, fmt, ap);
    if (r < 0 || r >= n) { buf[n - 1] = 0; return -1; }
    return r;
}

LWSTDAPI_(int) wvnsprintfW(LPWSTR buf, int n, LPCWSTR fmt, va_list ap)
{
    if (n <= 0) return -1;
    int r = _vsnwprintf((wchar_t *)buf, (size_t)n, (const wchar_t *)fmt, ap);
    if (r < 0 || r >= n) { buf[n - 1] = 0; return -1; }
    return r;
}

__declspec(dllexport) int __cdecl wnsprintfA(LPSTR buf, int n, LPCSTR fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    int r = wvnsprintfA(buf, n, fmt, a);
    va_end(a);
    return r;
}

__declspec(dllexport) int __cdecl wnsprintfW(LPWSTR buf, int n, LPCWSTR fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    int r = wvnsprintfW(buf, n, fmt, a);
    va_end(a);
    return r;
}

/* StrCpyW/StrCatW: shlwapi exports them too */
LWSTDAPI_(LPWSTR) StrCpyW(LPWSTR d, LPCWSTR s) { lstrcpyW_(d, s); return d; }
LWSTDAPI_(LPWSTR) StrCatW(LPWSTR d, LPCWSTR s) { lstrcpyW_(d + lstrlenW(d), s); return d; }
