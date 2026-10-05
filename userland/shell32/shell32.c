/*
 * shell32.dll — the shell's API for programs: known folders, command-line
 * splitting, ShellExecute, file operations, item ID lists and a few
 * desktop hooks.
 *
 * Folders: the user's folders are the top-level ones on drive C:
 * (C:\Documents, C:\Downloads, C:\Pictures, ...), per-application data
 * lives under C:\AppData and C:\ProgramData, programs under C:\Programs.
 * An item ID list here is one item holding a full path (UTF-16).
 */

#define NOVA_BUILD_SHELL32
#include <windows.h>
#include <commctrl.h>
#include <winternl.h>

#define S_OK_          ((HRESULT)0)
#define S_FALSE_       ((HRESULT)1)
#define E_FAIL_        ((HRESULT)0x80004005L)
#define E_INVALIDARG_  ((HRESULT)0x80070057L)
#define E_OUTOFMEMORY_ ((HRESULT)0x8007000EL)
#define E_NOTIMPL_     ((HRESULT)0x80004001L)
#define HR_WIN32(e)    ((HRESULT)(0x80070000L | (e)))

static int wlen(const WCHAR *s) { int n = 0; if (s) while (s[n]) n++; return n; }
static void wcopy(WCHAR *d, const WCHAR *s) { while ((*d++ = *s++)) ; }
static int u2w(const char *s, WCHAR *w, int cap) { int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, w, cap); return n > 0 ? n - 1 : 0; }
static int w2a(const WCHAR *w, char *a, int cap) { int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, a, cap, 0, 0); return n > 0 ? n - 1 : 0; }

/* -----------------------------------------------------------------------
 * Known folders
 * ----------------------------------------------------------------------- */
typedef struct { GUID id; int csidl; const char *path; BOOL create; } Folder;

#define G(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) { a, b, c, { d0, d1, d2, d3, d4, d5, d6, d7 } }
static const Folder g_folders[] = {
    { G(0xB4BFCC3A, 0xDB2C, 0x424C, 0xB0, 0x29, 0x7F, 0xE9, 0x9A, 0x87, 0xC6, 0x41), 0x10, "C:\\Desktop", TRUE },          /* Desktop */
    { G(0xFDD39AD0, 0x238F, 0x46AF, 0xAD, 0xB4, 0x6C, 0x85, 0x48, 0x03, 0x69, 0xC7), 0x05, "C:\\Documents", TRUE },        /* Documents */
    { G(0x374DE290, 0x123F, 0x4565, 0x91, 0x64, 0x39, 0xC4, 0x92, 0x5E, 0x46, 0x7B), -1,   "C:\\Downloads", TRUE },        /* Downloads */
    { G(0x33E28130, 0x4E1E, 0x4676, 0x83, 0x5A, 0x98, 0x39, 0x5C, 0x3B, 0xC3, 0xBB), 0x27, "C:\\Pictures", TRUE },         /* Pictures */
    { G(0x4BD8D571, 0x6D19, 0x48D3, 0xBE, 0x97, 0x42, 0x22, 0x20, 0x08, 0x0E, 0x43), 0x0D, "C:\\Music", TRUE },            /* Music */
    { G(0x18989B1D, 0x99B5, 0x455B, 0x84, 0x1C, 0xAB, 0x7C, 0x74, 0xE4, 0xDD, 0xFC), 0x0E, "C:\\Videos", TRUE },           /* Videos */
    { G(0x5E6C858F, 0x0E22, 0x4760, 0x9A, 0xFE, 0xEA, 0x33, 0x17, 0xB6, 0x71, 0x73), 0x28, "C:\\", FALSE },                /* Profile */
    { G(0x3EB685DB, 0x65F9, 0x4CF6, 0xA0, 0x3A, 0xE3, 0xEF, 0x65, 0x72, 0x9F, 0x3D), 0x1A, "C:\\AppData\\Roaming", TRUE }, /* RoamingAppData */
    { G(0xF1B32785, 0x6FBA, 0x4FCF, 0x9D, 0x55, 0x7B, 0x8E, 0x7F, 0x15, 0x70, 0x91), 0x1C, "C:\\AppData\\Local", TRUE },   /* LocalAppData */
    { G(0xA520A1A4, 0x1780, 0x4FF6, 0xBD, 0x18, 0x16, 0x73, 0x43, 0xC5, 0xAF, 0x16), -1,   "C:\\AppData\\LocalLow", TRUE },/* LocalAppDataLow */
    { G(0x62AB5D82, 0xFDC1, 0x4DC3, 0xA9, 0xDD, 0x07, 0x0D, 0x1D, 0x49, 0x5D, 0x97), 0x23, "C:\\ProgramData", TRUE },      /* ProgramData */
    { G(0x905E63B6, 0xC1BF, 0x494E, 0xB2, 0x9C, 0x65, 0xB7, 0x32, 0xD3, 0xD2, 0x1A), 0x26, "C:\\Programs", FALSE },        /* ProgramFiles */
    { G(0x7C5A40EF, 0xA0FB, 0x4BFC, 0x87, 0x4A, 0xC0, 0xF2, 0xE0, 0xB9, 0xFA, 0x8E), 0x2A, "C:\\Programs", FALSE },        /* ProgramFilesX86 */
    { G(0x6D809377, 0x6AF0, 0x444B, 0x89, 0x57, 0xA3, 0x77, 0x3F, 0x02, 0x20, 0x0E), -1,   "C:\\Programs", FALSE },        /* ProgramFilesX64 */
    { G(0xF7F1ED05, 0x9F6D, 0x47A2, 0xAA, 0xAE, 0x29, 0xD3, 0x17, 0xC6, 0xF0, 0x66), 0x2B, "C:\\Programs\\Common Files", TRUE },  /* ProgramFilesCommon */
    { G(0xDE974D24, 0xD9C6, 0x4D3E, 0xBF, 0x91, 0xF4, 0x45, 0x51, 0x20, 0xB9, 0x17), 0x2C, "C:\\Programs\\Common Files", TRUE },  /* ProgramFilesCommonX86 */
    { G(0x6365D5A7, 0x0F0D, 0x45E5, 0x87, 0xF6, 0x0D, 0xA5, 0x6B, 0x6A, 0x4F, 0x7D), -1,   "C:\\Programs\\Common Files", TRUE },  /* ProgramFilesCommonX64 */
    { G(0x1AC14E77, 0x02E7, 0x4E5D, 0xB7, 0x44, 0x2E, 0xB1, 0xAE, 0x51, 0x98, 0xB7), 0x25, "C:\\Windows\\System32", FALSE },/* System */
    { G(0xD65231B0, 0xB2F1, 0x4857, 0xA4, 0xCE, 0xA8, 0xE7, 0xC6, 0xEA, 0x7D, 0x27), 0x29, "C:\\Windows\\System32", FALSE },/* SystemX86 */
    { G(0xF38BF404, 0x1D43, 0x42F2, 0x93, 0x05, 0x67, 0xDE, 0x0B, 0x28, 0xFC, 0x23), 0x24, "C:\\Windows", FALSE },         /* Windows */
    { G(0xFD228CB7, 0xAE11, 0x4AE3, 0x86, 0x4C, 0x16, 0xF3, 0x91, 0x0A, 0xB8, 0xFE), 0x14, "C:\\Windows\\Fonts", TRUE },   /* Fonts */
    { G(0x0762D272, 0xC50A, 0x4BB0, 0xA3, 0x82, 0x69, 0x7D, 0xCD, 0x72, 0x9B, 0x80), -1,   "C:\\", FALSE },                /* UserProfiles */
    { G(0xDFDF76A2, 0xC82A, 0x4D63, 0x90, 0x6A, 0x56, 0x44, 0xAC, 0x45, 0x73, 0x85), -1,   "C:\\", FALSE },                /* Public */
    { G(0xED4824AF, 0xDCE4, 0x45A8, 0x81, 0xE2, 0xFC, 0x79, 0x65, 0x08, 0x36, 0x34), 0x2E, "C:\\Documents", TRUE },        /* PublicDocuments */
    { G(0x8983036C, 0x27C0, 0x404B, 0x8F, 0x08, 0x10, 0x2D, 0x10, 0xDC, 0xFD, 0x74), 0x09, "C:\\AppData\\Roaming\\SendTo", TRUE },
    { G(0x625B53C3, 0xAB48, 0x4EC1, 0xBA, 0x1F, 0xA1, 0xEF, 0x41, 0x46, 0xFC, 0x19), 0x0B, "C:\\AppData\\Roaming\\Start Menu", TRUE },
    { G(0xA77F5D77, 0x2E2B, 0x44C3, 0xA6, 0xA2, 0xAB, 0xA6, 0x01, 0x05, 0x4A, 0x51), 0x02, "C:\\AppData\\Roaming\\Start Menu\\Programs", TRUE },
    { G(0xB97D20BB, 0xF46A, 0x4C97, 0xBA, 0x10, 0x5E, 0x36, 0x08, 0x43, 0x08, 0x54), 0x07, "C:\\AppData\\Roaming\\Start Menu\\Programs\\Startup", TRUE },
    { G(0xAE50C081, 0xEBD2, 0x438A, 0x86, 0x55, 0x8A, 0x09, 0x2E, 0x34, 0x98, 0x7A), 0x08, "C:\\AppData\\Roaming\\Recent", TRUE },
    { G(0xA63293E8, 0x664E, 0x48DB, 0xA0, 0x79, 0xDF, 0x75, 0x9E, 0x05, 0x09, 0xF7), 0x15, "C:\\AppData\\Roaming\\Templates", TRUE },
    { G(0x1777F761, 0x68AD, 0x4D8A, 0x87, 0xBD, 0x30, 0xB7, 0x59, 0xFA, 0x33, 0xDD), 0x06, "C:\\Favorites", TRUE },         /* Favorites */
    { G(0x4C5C32FF, 0xBB9D, 0x43B0, 0xB5, 0xB4, 0x2D, 0x72, 0xE5, 0x4E, 0xAA, 0xA4), -1,   "C:\\Documents\\Saved Games", TRUE },
    { G(0x56784854, 0xC6CB, 0x462B, 0x81, 0x69, 0x88, 0xE3, 0x50, 0xAC, 0xB8, 0x82), -1,   "C:\\Contacts", TRUE },
    { G(0xB7BEDE81, 0xDF94, 0x4682, 0xA7, 0xD8, 0x57, 0xA5, 0x26, 0x20, 0xB8, 0x6F), -1,   "C:\\Pictures\\Screenshots", TRUE },
};

static int guid_eq(const GUID *a, const GUID *b)
{
    const BYTE *x = (const BYTE *)a, *y = (const BYTE *)b;
    for (int i = 0; i < 16; i++) if (x[i] != y[i]) return 0;
    return 1;
}

/* mkdir -p */
static BOOL create_dirs(const WCHAR *path)
{
    WCHAR tmp[MAX_PATH];
    int n = wlen(path);
    if (n >= MAX_PATH) return FALSE;
    wcopy(tmp, path);
    for (int i = 3; i <= n; i++) {
        if (tmp[i] == '\\' || tmp[i] == '/' || !tmp[i]) {
            WCHAR save = tmp[i];
            tmp[i] = 0;
            if (GetFileAttributesW(tmp) == INVALID_FILE_ATTRIBUTES && !CreateDirectoryW(tmp, 0)) return FALSE;
            tmp[i] = save;
        }
    }
    return TRUE;
}

static const Folder *folder_by_csidl(int csidl)
{
    csidl &= 0xFF;
    if (csidl == 0) csidl = 0x10;                       /* CSIDL_DESKTOP: the desktop folder */
    if (csidl == 0x19) csidl = 0x10;                    /* COMMON_DESKTOPDIRECTORY */
    if (csidl == 0x16) csidl = 0x0B;                    /* COMMON_STARTMENU */
    if (csidl == 0x17) csidl = 0x02;                    /* COMMON_PROGRAMS */
    if (csidl == 0x18) csidl = 0x07;                    /* COMMON_STARTUP */
    for (unsigned i = 0; i < sizeof(g_folders) / sizeof(g_folders[0]); i++)
        if (g_folders[i].csidl == csidl) return &g_folders[i];
    return 0;
}

static HRESULT folder_path(const Folder *f, BOOL create, WCHAR *out)
{
    u2w(f->path, out, MAX_PATH);
    if ((create || f->create) && GetFileAttributesW(out) == INVALID_FILE_ATTRIBUTES && !create_dirs(out))
        return HR_WIN32(GetLastError());
    return S_OK_;
}

SHSTDAPI_(HRESULT) SHGetKnownFolderPath(REFGUID id, DWORD flags, HANDLE token, LPWSTR *out)
{
    (void)token;
    *out = 0;
    for (unsigned i = 0; i < sizeof(g_folders) / sizeof(g_folders[0]); i++) {
        if (!guid_eq(&g_folders[i].id, id)) continue;
        WCHAR p[MAX_PATH];
        HRESULT hr = folder_path(&g_folders[i], (flags & 0x8000 /* KF_FLAG_CREATE */) != 0, p);
        if (hr) return hr;
        *out = LocalAlloc(0, 2 * ((SIZE_T)wlen(p) + 1));    /* freed with CoTaskMemFree (the same heap) */
        if (!*out) return E_OUTOFMEMORY_;
        wcopy(*out, p);
        return S_OK_;
    }
    return HR_WIN32(2);                                     /* E_FILENOTFOUND: unknown folder */
}

SHSTDAPI_(HRESULT) SHGetFolderPathW(HWND hwnd, int csidl, HANDLE token, DWORD flags, LPWSTR path)
{
    (void)hwnd; (void)token; (void)flags;
    const Folder *f = folder_by_csidl(csidl);
    if (!f) { path[0] = 0; return E_INVALIDARG_; }
    return folder_path(f, (csidl & 0x8000) != 0, path);
}

SHSTDAPI_(HRESULT) SHGetFolderPathA(HWND hwnd, int csidl, HANDLE token, DWORD flags, LPSTR path)
{
    WCHAR w[MAX_PATH];
    HRESULT hr = SHGetFolderPathW(hwnd, csidl, token, flags, w);
    if (hr) { path[0] = 0; return hr; }
    w2a(w, path, MAX_PATH);
    return S_OK_;
}

SHSTDAPI_(HRESULT) SHGetFolderPathAndSubDirW(HWND hwnd, int csidl, HANDLE token, DWORD flags, LPCWSTR sub, LPWSTR path)
{
    HRESULT hr = SHGetFolderPathW(hwnd, csidl, token, flags, path);
    if (hr || !sub) return hr;
    int n = wlen(path);
    if (n + wlen(sub) + 2 >= MAX_PATH) return E_INVALIDARG_;
    if (n && path[n - 1] != '\\') path[n++] = '\\';
    wcopy(path + n, sub);
    if ((csidl & 0x8000) && !create_dirs(path)) return HR_WIN32(GetLastError());
    return S_OK_;
}

SHSTDAPI_(BOOL) SHGetSpecialFolderPathW(HWND hwnd, LPWSTR path, int csidl, BOOL create)
{
    return SHGetFolderPathW(hwnd, csidl | (create ? 0x8000 : 0), 0, 0, path) == S_OK_;
}

SHSTDAPI_(BOOL) SHGetSpecialFolderPathA(HWND hwnd, LPSTR path, int csidl, BOOL create)
{
    return SHGetFolderPathA(hwnd, csidl | (create ? 0x8000 : 0), 0, 0, path) == S_OK_;
}

SHSTDAPI_(HRESULT) SHSetKnownFolderPath(REFGUID id, DWORD flags, HANDLE token, LPCWSTR path)
{
    (void)id; (void)flags; (void)token; (void)path;
    return HR_WIN32(ERROR_ACCESS_DENIED);                   /* the folders are fixed */
}

/* -----------------------------------------------------------------------
 * Item ID lists: one item, the full path
 * ----------------------------------------------------------------------- */
typedef struct { WORD cb; BYTE abID[1]; } SHITEMID;
typedef struct { SHITEMID mkid; } ITEMIDLIST, *LPITEMIDLIST;
typedef const ITEMIDLIST *LPCITEMIDLIST;

static LPITEMIDLIST pidl_from_path(const WCHAR *path)
{
    int n = wlen(path);
    WORD cb = (WORD)(2 + 2 * (n + 1));
    BYTE *p = LocalAlloc(LMEM_ZEROINIT, (SIZE_T)cb + 2);
    if (!p) return 0;
    *(WORD *)p = cb;
    for (int i = 0; i <= n; i++) ((WCHAR *)(p + 2))[i] = path[i];
    return (LPITEMIDLIST)p;
}

SHSTDAPI_(UINT) ILGetSize(LPCITEMIDLIST pidl)
{
    if (!pidl) return 0;
    UINT n = 0;
    const BYTE *p = (const BYTE *)pidl;
    while (*(const WORD *)(p + n)) n += *(const WORD *)(p + n);
    return n + 2;
}

SHSTDAPI_(void) ILFree(LPITEMIDLIST pidl) { if (pidl) LocalFree(pidl); }

SHSTDAPI_(LPITEMIDLIST) ILClone(LPCITEMIDLIST pidl)
{
    UINT n = ILGetSize(pidl);
    if (!n) return 0;
    BYTE *c = LocalAlloc(0, n);
    if (c) for (UINT i = 0; i < n; i++) c[i] = ((const BYTE *)pidl)[i];
    return (LPITEMIDLIST)c;
}

SHSTDAPI_(LPITEMIDLIST) ILCreateFromPathW(LPCWSTR path)
{
    WCHAR full[MAX_PATH];
    if (!GetFullPathNameW(path, MAX_PATH, full, 0)) return 0;
    return pidl_from_path(full);
}

SHSTDAPI_(LPITEMIDLIST) ILCreateFromPathA(LPCSTR path)
{
    WCHAR w[MAX_PATH];
    u2w(path, w, MAX_PATH);
    return ILCreateFromPathW(w);
}

SHSTDAPI_(BOOL) SHGetPathFromIDListW(LPCITEMIDLIST pidl, LPWSTR path)
{
    if (!pidl || !pidl->mkid.cb) { path[0] = 0; return FALSE; }
    const WCHAR *s = (const WCHAR *)pidl->mkid.abID;
    int n = wlen(s);
    if (n >= MAX_PATH) { path[0] = 0; return FALSE; }
    wcopy(path, s);
    return TRUE;
}

SHSTDAPI_(BOOL) SHGetPathFromIDListA(LPCITEMIDLIST pidl, LPSTR path)
{
    WCHAR w[MAX_PATH];
    if (!SHGetPathFromIDListW(pidl, w)) { path[0] = 0; return FALSE; }
    w2a(w, path, MAX_PATH);
    return TRUE;
}

SHSTDAPI_(BOOL) SHGetPathFromIDListEx(LPCITEMIDLIST pidl, LPWSTR path, DWORD n, int opts)
{
    (void)opts;
    WCHAR w[MAX_PATH];
    if (!SHGetPathFromIDListW(pidl, w) || (DWORD)wlen(w) >= n) return FALSE;
    wcopy(path, w);
    return TRUE;
}

SHSTDAPI_(HRESULT) SHGetFolderLocation(HWND hwnd, int csidl, HANDLE token, DWORD flags, LPITEMIDLIST *out)
{
    WCHAR p[MAX_PATH];
    HRESULT hr = SHGetFolderPathW(hwnd, csidl, token, flags, p);
    if (hr) { *out = 0; return hr; }
    *out = pidl_from_path(p);
    return *out ? S_OK_ : E_OUTOFMEMORY_;
}

SHSTDAPI_(HRESULT) SHGetSpecialFolderLocation(HWND hwnd, int csidl, LPITEMIDLIST *out)
{
    return SHGetFolderLocation(hwnd, csidl, 0, 0, out);
}

SHSTDAPI_(HRESULT) SHGetKnownFolderIDList(REFGUID id, DWORD flags, HANDLE token, LPITEMIDLIST *out)
{
    LPWSTR p;
    HRESULT hr = SHGetKnownFolderPath(id, flags, token, &p);
    if (hr) { *out = 0; return hr; }
    *out = pidl_from_path(p);
    LocalFree(p);
    return *out ? S_OK_ : E_OUTOFMEMORY_;
}

SHSTDAPI_(HRESULT) SHParseDisplayName(LPCWSTR name, void *bc, LPITEMIDLIST *out, DWORD in, DWORD *attrs)
{
    (void)bc; (void)in;
    if (attrs) *attrs = 0;
    if (GetFileAttributesW(name) == INVALID_FILE_ATTRIBUTES) { *out = 0; return HR_WIN32(GetLastError()); }
    *out = ILCreateFromPathW(name);
    return *out ? S_OK_ : E_OUTOFMEMORY_;
}

/* SHBrowseForFolder: browse.c */

/* -----------------------------------------------------------------------
 * Command lines
 * ----------------------------------------------------------------------- */
/* Split like the C runtime: quotes group, backslashes escape quotes */
SHSTDAPI_(LPWSTR *) CommandLineToArgvW(LPCWSTR cmd, int *argc)
{
    if (!argc) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    WCHAR self[MAX_PATH];
    if (!cmd || !*cmd) {                                    /* the program's own path */
        GetModuleFileNameW(0, self, MAX_PATH);
        cmd = self;
    }
    int len = wlen(cmd), count = 0;
    /* worst case: every character its own argument */
    SIZE_T size = sizeof(LPWSTR) * ((SIZE_T)len + 2) + 2 * ((SIZE_T)len + 2) * 2;
    LPWSTR *argv = LocalAlloc(0, size);
    if (!argv) return 0;
    WCHAR *buf = (WCHAR *)(argv + len + 2);
    const WCHAR *p = cmd;
    /* the program name: up to the next space, quotes toggle, no escapes */
    argv[count++] = buf;
    BOOL q = FALSE;
    while (*p && (q || (*p != ' ' && *p != '\t'))) { if (*p == '"') q = !q; else *buf++ = *p; p++; }
    *buf++ = 0;
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[count++] = buf;
        q = FALSE;
        while (*p && (q || (*p != ' ' && *p != '\t'))) {
            int bs = 0;
            while (*p == '\\') { bs++; p++; }
            if (*p == '"') {
                for (int i = 0; i < bs / 2; i++) *buf++ = '\\';
                if (bs & 1) { *buf++ = '"'; p++; }
                else if (q && p[1] == '"') { *buf++ = '"'; p += 2; }
                else { q = !q; p++; }
            } else {
                for (int i = 0; i < bs; i++) *buf++ = '\\';
                if (*p && (q || (*p != ' ' && *p != '\t'))) *buf++ = *p++;
            }
        }
        *buf++ = 0;
    }
    argv[count] = 0;
    *argc = count;
    return argv;
}

/* -----------------------------------------------------------------------
 * ShellExecute
 * ----------------------------------------------------------------------- */
static int ends_with(const WCHAR *s, const char *suffix)
{
    int n = wlen(s), k = 0;
    while (suffix[k]) k++;
    if (n < k) return 0;
    for (int i = 0; i < k; i++) {
        WCHAR c = s[n - k + i];
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c != (WCHAR)suffix[i]) return 0;
    }
    return 1;
}

static int starts_with(const WCHAR *s, const char *prefix)
{
    for (; *prefix; s++, prefix++) {
        WCHAR c = *s;
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c != (WCHAR)*prefix) return 0;
    }
    return 1;
}

NTSYSAPI NTSTATUS NTAPI NtOpenProcessToken(HANDLE p, ACCESS_MASK access, PHANDLE token);
NTSYSAPI NTSTATUS NTAPI NtQueryInformationToken(HANDLE t, ULONG cls, PVOID buf, ULONG n, PULONG ret);

/* The calling process's token's TOKEN_INFORMATION_CLASS @cls as a DWORD
 * (TokenElevationType 18, TokenElevation 20), or @dflt */
static DWORD token_dword(ULONG cls, DWORD dflt)
{
    HANDLE t;
    DWORD v = dflt;
    ULONG n;
    if (NtOpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t) < 0) return dflt;
    if (NtQueryInformationToken(t, cls, &v, sizeof(v), &n) < 0) v = dflt;
    NtClose(t);
    return v;
}

/* The elevated half of the caller's split token (TokenLinkedToken), for
 * the "runas" verb; NULL when the caller is elevated already */
static HANDLE elevated_token(void)
{
    HANDLE t, linked = 0;
    ULONG n;
    if (token_dword(18 /* TokenElevationType */, 1) != 3 /* TokenElevationTypeLimited */) return 0;
    if (NtOpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t) < 0) return 0;
    if (NtQueryInformationToken(t, 19 /* TokenLinkedToken */, &linked, sizeof(linked), &n) < 0) linked = 0;
    NtClose(t);
    return linked;
}

/* Run @file (a program, or a web address in the browser); 0 or an SE_ERR_* code.
 * The "runas" verb runs it as administrator: NovaOS has no consent prompt
 * yet, so it is elevated at once (UAC's "never notify"). */
static int execute(LPCWSTR verb, LPCWSTR file, LPCWSTR params, LPCWSTR dir, HANDLE *proc)
{
    WCHAR cmd[2048], prog[MAX_PATH];
    int o = 0;
    if (starts_with(file, "http://") || starts_with(file, "https://")) {
        u2w("C:\\Programs\\NetSurf\\netsurf.exe", prog, MAX_PATH);
        if (GetFileAttributesW(prog) == INVALID_FILE_ATTRIBUTES) return 31;   /* SE_ERR_NOASSOC */
        params = file;
    } else if (ends_with(file, ".exe") || ends_with(file, ".com") || !wlen(file) ||
               GetFileAttributesW(file) == INVALID_FILE_ATTRIBUTES) {
        if (wlen(file) >= MAX_PATH) return 2;
        wcopy(prog, file);                                  /* a program (searched like CreateProcess) */
    } else {
        return 31;                                          /* documents: no associations are registered */
    }
    cmd[o++] = '"';
    for (const WCHAR *c = prog; *c && o < 1500; c++) cmd[o++] = *c;
    cmd[o++] = '"';
    if (params && *params) {
        cmd[o++] = ' ';
        for (const WCHAR *c = params; *c && o < 2040; c++) cmd[o++] = *c;
    }
    cmd[o] = 0;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    for (unsigned i = 0; i < sizeof(si); i++) ((BYTE *)&si)[i] = 0;
    si.cb = sizeof(si);
    HANDLE token = verb && !lstrcmpiW(verb, L"runas") ? elevated_token() : 0;
    if (!CreateProcessW(0, cmd, 0, 0, FALSE, token ? CREATE_SUSPENDED : 0, 0, dir, &si, &pi)) {
        DWORD e = GetLastError();
        if (token) NtClose(token);
        return e == ERROR_FILE_NOT_FOUND ? 2 : e == ERROR_PATH_NOT_FOUND ? 3 : e == ERROR_NOT_ENOUGH_MEMORY ? 8 : 5;
    }
    if (token) {
        struct { HANDLE token, thread; } at = { token, pi.hThread };
        NTSTATUS st = NtSetInformationProcess(pi.hProcess, 9 /* ProcessAccessToken */, &at, sizeof(at));
        NtClose(token);
        if (st < 0) {
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return 5;                                       /* SE_ERR_ACCESSDENIED */
        }
        ResumeThread(pi.hThread);
    }
    if (pi.hThread) CloseHandle(pi.hThread);
    if (proc) *proc = pi.hProcess;
    else CloseHandle(pi.hProcess);
    return 0;
}

SHSTDAPI_(HINSTANCE) ShellExecuteW(HWND hwnd, LPCWSTR verb, LPCWSTR file, LPCWSTR params, LPCWSTR dir, INT show)
{
    (void)hwnd; (void)show;
    int e = execute(verb, file, params, dir, 0);
    return (HINSTANCE)(ULONG_PTR)(e ? e : 42);
}

SHSTDAPI_(HINSTANCE) ShellExecuteA(HWND hwnd, LPCSTR verb, LPCSTR file, LPCSTR params, LPCSTR dir, INT show)
{
    WCHAR v[32], f[MAX_PATH], p[1024], d[MAX_PATH];
    if (verb) u2w(verb, v, 32);
    u2w(file, f, MAX_PATH);
    if (params) u2w(params, p, 1024);
    if (dir) u2w(dir, d, MAX_PATH);
    return ShellExecuteW(hwnd, verb ? v : 0, f, params ? p : 0, dir ? d : 0, show);
}

typedef struct {
    DWORD cbSize; ULONG fMask; HWND hwnd; LPCWSTR lpVerb, lpFile, lpParameters, lpDirectory; int nShow;
    HINSTANCE hInstApp; void *lpIDList; LPCWSTR lpClass; HANDLE hkeyClass; DWORD dwHotKey;
    HANDLE hIcon; HANDLE hProcess;
} SHELLEXECUTEINFOW;

SHSTDAPI_(BOOL) ShellExecuteExW(SHELLEXECUTEINFOW *info)
{
    WCHAR path[MAX_PATH];
    LPCWSTR file = info->lpFile;
    if ((info->fMask & 0x04 /* SEE_MASK_IDLIST */) && info->lpIDList && SHGetPathFromIDListW(info->lpIDList, path)) file = path;
    HANDLE proc = 0;
    int e = execute(info->lpVerb, file ? file : (const WCHAR[]){ 0 }, info->lpParameters, info->lpDirectory, &proc);
    info->hInstApp = (HINSTANCE)(ULONG_PTR)(e ? e : 42);
    if (e) {
        SetLastError(e == 31 ? 1155 /* ERROR_NO_ASSOCIATION */ : e == 2 ? ERROR_FILE_NOT_FOUND : e == 3 ? ERROR_PATH_NOT_FOUND : ERROR_ACCESS_DENIED);
        return FALSE;
    }
    if (info->fMask & 0x40 /* SEE_MASK_NOCLOSEPROCESS */) info->hProcess = proc;
    else if (proc) CloseHandle(proc);
    return TRUE;
}

SHSTDAPI_(HINSTANCE) FindExecutableW(LPCWSTR file, LPCWSTR dir, LPWSTR out)
{
    (void)dir;
    if (ends_with(file, ".exe") && GetFileAttributesW(file) != INVALID_FILE_ATTRIBUTES) { wcopy(out, file); return (HINSTANCE)42; }
    out[0] = 0;
    return (HINSTANCE)31;
}

/* -----------------------------------------------------------------------
 * File operations
 * ----------------------------------------------------------------------- */
SHSTDAPI_(int) SHCreateDirectoryExW(HWND hwnd, LPCWSTR path, LPSECURITY_ATTRIBUTES sa)
{
    (void)hwnd; (void)sa;
    WCHAR full[MAX_PATH];
    if (!GetFullPathNameW(path, MAX_PATH, full, 0)) return 161 /* ERROR_BAD_PATHNAME */;
    if (GetFileAttributesW(full) != INVALID_FILE_ATTRIBUTES) return ERROR_ALREADY_EXISTS;
    return create_dirs(full) ? ERROR_SUCCESS : (int)GetLastError();
}

SHSTDAPI_(int) SHCreateDirectoryExA(HWND hwnd, LPCSTR path, LPSECURITY_ATTRIBUTES sa)
{
    WCHAR w[MAX_PATH];
    u2w(path, w, MAX_PATH);
    return SHCreateDirectoryExW(hwnd, w, sa);
}

SHSTDAPI_(int) SHCreateDirectory(HWND hwnd, LPCWSTR path) { return SHCreateDirectoryExW(hwnd, path, 0); }

static BOOL delete_tree(const WCHAR *path)
{
    DWORD a = GetFileAttributesW(path);
    if (a == INVALID_FILE_ATTRIBUTES) return FALSE;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) return DeleteFileW(path);
    WCHAR pat[MAX_PATH + 4];
    int n = wlen(path);
    if (n > MAX_PATH - 3) return FALSE;
    wcopy(pat, path);
    pat[n] = '\\'; pat[n + 1] = '*'; pat[n + 2] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == '.' && (!fd.cFileName[1] || (fd.cFileName[1] == '.' && !fd.cFileName[2]))) continue;
            WCHAR child[MAX_PATH];
            if (n + 1 + wlen(fd.cFileName) >= MAX_PATH) continue;
            wcopy(child, path);
            child[n] = '\\';
            wcopy(child + n + 1, fd.cFileName);
            delete_tree(child);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryW(path);
}

static BOOL copy_tree(const WCHAR *from, const WCHAR *to)
{
    DWORD a = GetFileAttributesW(from);
    if (a == INVALID_FILE_ATTRIBUTES) return FALSE;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) return CopyFileW(from, to, FALSE);
    if (!create_dirs(to)) return FALSE;
    WCHAR pat[MAX_PATH + 4];
    int n = wlen(from), tn = wlen(to);
    if (n > MAX_PATH - 3) return FALSE;
    wcopy(pat, from);
    pat[n] = '\\'; pat[n + 1] = '*'; pat[n + 2] = 0;
    WIN32_FIND_DATAW fd;
    BOOL ok = TRUE;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == '.' && (!fd.cFileName[1] || (fd.cFileName[1] == '.' && !fd.cFileName[2]))) continue;
            WCHAR s[MAX_PATH], d[MAX_PATH];
            int fl = wlen(fd.cFileName);
            if (n + 1 + fl >= MAX_PATH || tn + 1 + fl >= MAX_PATH) { ok = FALSE; continue; }
            wcopy(s, from); s[n] = '\\'; wcopy(s + n + 1, fd.cFileName);
            wcopy(d, to); d[tn] = '\\'; wcopy(d + tn + 1, fd.cFileName);
            ok = copy_tree(s, d) && ok;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return ok;
}

typedef struct {
    HWND hwnd; UINT wFunc; LPCWSTR pFrom, pTo; WORD fFlags; BOOL fAnyOperationsAborted;
    LPVOID hNameMappings; LPCWSTR lpszProgressTitle;
} SHFILEOPSTRUCTW;

/* FO_MOVE 1, FO_COPY 2, FO_DELETE 3, FO_RENAME 4; the lists are double-NUL-terminated */
SHSTDAPI_(int) SHFileOperationW(SHFILEOPSTRUCTW *op)
{
    op->fAnyOperationsAborted = FALSE;
    const WCHAR *to = op->pTo;
    for (const WCHAR *from = op->pFrom; from && *from; from += wlen(from) + 1) {
        WCHAR dest[MAX_PATH];
        if (op->wFunc != 3) {
            if (!to || !*to) return 0x7C;                   /* DE_INVALIDFILES */
            wcopy(dest, to);
            /* into a directory: keep the name */
            DWORD da = GetFileAttributesW(dest);
            if (op->wFunc != 4 && da != INVALID_FILE_ATTRIBUTES && (da & FILE_ATTRIBUTE_DIRECTORY)) {
                const WCHAR *name = from;
                for (const WCHAR *c = from; *c; c++) if (*c == '\\' || *c == '/') name = c + 1;
                int dl = wlen(dest);
                if (dl + wlen(name) + 2 >= MAX_PATH) return 0x7C;
                if (dl && dest[dl - 1] != '\\') dest[dl++] = '\\';
                wcopy(dest + dl, name);
            }
            if (op->fFlags & 0x0001 /* FOF_MULTIDESTFILES */) to += wlen(to) + 1;
        }
        BOOL ok;
        switch (op->wFunc) {
        case 1: case 4: ok = MoveFileExW(from, dest, 2 | ((op->fFlags & 0x10 /* FOF_NOCONFIRMATION */) ? 1 : 0)); break;
        case 2: ok = copy_tree(from, dest); break;
        case 3: ok = delete_tree(from); break;
        default: return ERROR_INVALID_PARAMETER;
        }
        if (!ok) return (int)GetLastError() ? (int)GetLastError() : 0x7C;
    }
    return 0;
}

typedef struct {
    HWND hwnd; UINT wFunc; LPCSTR pFrom, pTo; WORD fFlags; BOOL fAnyOperationsAborted;
    LPVOID hNameMappings; LPCSTR lpszProgressTitle;
} SHFILEOPSTRUCTA;

static WCHAR *multi_a2w(LPCSTR s)
{
    if (!s) return 0;
    int n = 0;
    while (s[n] || s[n + 1]) n++;
    n += 2;
    WCHAR *w = LocalAlloc(LMEM_ZEROINIT, 2 * (SIZE_T)n);
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, n, w, n);
    return w;
}

SHSTDAPI_(int) SHFileOperationA(SHFILEOPSTRUCTA *op)
{
    SHFILEOPSTRUCTW w;
    w.hwnd = op->hwnd; w.wFunc = op->wFunc; w.fFlags = op->fFlags;
    w.pFrom = multi_a2w(op->pFrom);
    w.pTo = multi_a2w(op->pTo);
    w.hNameMappings = 0; w.lpszProgressTitle = 0;
    int r = SHFileOperationW(&w);
    op->fAnyOperationsAborted = w.fAnyOperationsAborted;
    LocalFree((void *)w.pFrom);
    LocalFree((void *)w.pTo);
    return r;
}

/* -----------------------------------------------------------------------
 * File information (no icons: programs draw their own)
 * ----------------------------------------------------------------------- */
typedef struct { HICON hIcon; int iIcon; DWORD dwAttributes; WCHAR szDisplayName[MAX_PATH]; WCHAR szTypeName[80]; } SHFILEINFOW;

HIMAGELIST sys_image_list(int small);
int sys_icon_index(const WCHAR *path, DWORD attrs);

SHSTDAPI_(DWORD_PTR) SHGetFileInfoW(LPCWSTR path, DWORD attrs, SHFILEINFOW *info, UINT n, UINT flags)
{
    (void)n;
    WCHAR pbuf[MAX_PATH];
    if ((flags & 0x8 /* SHGFI_PIDL */) && path) {
        if (!SHGetPathFromIDListW((LPCITEMIDLIST)path, pbuf)) return 0;
        path = pbuf;
    }
    if (!path) return 0;
    DWORD a = attrs;
    if (!(flags & 0x10 /* SHGFI_USEFILEATTRIBUTES */)) {
        a = GetFileAttributesW(path);
        if (a == INVALID_FILE_ATTRIBUTES) return 0;
    }
    int small = (flags & 0x1 /* SHGFI_SMALLICON */) != 0;
    if (info) for (unsigned i = 0; i < sizeof(*info); i++) ((BYTE *)info)[i] = 0;
    if (info && (flags & (0x4000 /* SHGFI_SYSICONINDEX */ | 0x100 /* SHGFI_ICON */))) {
        info->iIcon = sys_icon_index(path, a);
        if (flags & 0x100) info->hIcon = ImageList_GetIcon(sys_image_list(small), info->iIcon, ILD_TRANSPARENT);
    }
    if (info && (flags & 0x200 /* SHGFI_DISPLAYNAME */)) {
        const WCHAR *name = path;
        for (const WCHAR *c = path; *c; c++) if ((*c == '\\' || *c == '/') && c[1]) name = c + 1;
        int k = 0;
        for (; name[k] && k < MAX_PATH - 1; k++) info->szDisplayName[k] = name[k];
        if (k && info->szDisplayName[k - 1] == '\\' && k > 3) k--;
        info->szDisplayName[k] = 0;
    }
    if (info && (flags & 0x400 /* SHGFI_TYPENAME */)) {
        const char *t = (a & FILE_ATTRIBUTE_DIRECTORY) ? "File folder" : ends_with(path, ".exe") ? "Application" :
                        ends_with(path, ".txt") ? "Text Document" : ends_with(path, ".dll") ? "Application extension" :
                        ends_with(path, ".7z") ? "7Z Archive" : ends_with(path, ".zip") ? "Compressed (zipped) Folder" : "File";
        u2w(t, info->szTypeName, 80);
    }
    if (info && (flags & 0x800 /* SHGFI_ATTRIBUTES */))
        info->dwAttributes = (a & FILE_ATTRIBUTE_DIRECTORY) ? 0x20000000 | 0x80000000 : 0x40000000;   /* FOLDER|HASSUBFOLDER : FILESYSTEM */
    if (flags & 0x4000) return (DWORD_PTR)sys_image_list(small);
    return 1;
}

typedef struct { HICON hIcon; int iIcon; DWORD dwAttributes; char szDisplayName[MAX_PATH]; char szTypeName[80]; } SHFILEINFOA;
SHSTDAPI_(DWORD_PTR) SHGetFileInfoA(LPCSTR path, DWORD attrs, SHFILEINFOA *info, UINT n, UINT flags)
{
    (void)n;
    WCHAR w[MAX_PATH];
    LPCWSTR p = (LPCWSTR)path;
    if (!(flags & 0x8) && path) { MultiByteToWideChar(CP_ACP, 0, path, -1, w, MAX_PATH); p = w; }
    SHFILEINFOW wi;
    DWORD_PTR r = SHGetFileInfoW(p, attrs, info ? &wi : NULL, sizeof(wi), flags);
    if (r && info) {
        info->hIcon = wi.hIcon; info->iIcon = wi.iIcon; info->dwAttributes = wi.dwAttributes;
        WideCharToMultiByte(CP_ACP, 0, wi.szDisplayName, -1, info->szDisplayName, MAX_PATH, NULL, NULL);
        WideCharToMultiByte(CP_ACP, 0, wi.szTypeName, -1, info->szTypeName, 80, NULL, NULL);
    }
    return r;
}

SHSTDAPI_(HICON) ExtractIconW(HINSTANCE h, LPCWSTR file, UINT i) { (void)h; (void)file; (void)i; return 0; }
SHSTDAPI_(UINT) ExtractIconExW(LPCWSTR file, int i, HICON *large, HICON *small, UINT n)
{
    (void)file; (void)i;
    for (UINT k = 0; k < n; k++) { if (large) large[k] = 0; if (small) small[k] = 0; }
    return 0;
}
SHSTDAPI_(UINT) ExtractIconExA(LPCSTR file, int i, HICON *large, HICON *small, UINT n)
{
    (void)file;
    return ExtractIconExW(NULL, i, large, small, n);
}
/* No taskbar app bars (GTK asks whether the taskbar auto-hides) */
SHSTDAPI_(UINT_PTR) SHAppBarMessage(DWORD msg, void *data) { (void)msg; (void)data; return 0; }
SHSTDAPI_(HICON) ExtractAssociatedIconW(HINSTANCE h, LPWSTR path, WORD *i) { (void)h; (void)path; (void)i; return 0; }

/* -----------------------------------------------------------------------
 * Desktop integration that NovaOS programs don't get
 * ----------------------------------------------------------------------- */
/* Files dropped on a window (WM_DROPFILES): the handle holds a DROPFILES
 * block with the list; user32 makes it from drops other programs send */
__declspec(dllimport) BOOL WINAPI NovaAcceptDrops(HWND h, DWORD mask, BOOL on);

SHSTDAPI_(void) DragAcceptFiles(HWND h, BOOL accept)
{
    LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    SetWindowLongW(h, GWL_EXSTYLE, accept ? ex | WS_EX_ACCEPTFILES : ex & ~WS_EX_ACCEPTFILES);
    NovaAcceptDrops(h, 1, accept);
}

/* the i-th file (or the count for i = -1): UTF-16 in the block, or ANSI */
static const void *drop_file(HANDLE drop, UINT i, int *wide, UINT *count)
{
    const DROPFILES *d = drop ? GlobalLock(drop) : NULL;
    if (!d) { *count = 0; return NULL; }
    const BYTE *p = (const BYTE *)d + d->pFiles;
    *wide = d->fWide;
    UINT n = 0;
    const void *found = NULL;
    if (d->fWide) {
        const WCHAR *w = (const WCHAR *)p;
        while (*w) { if (n == i) found = w; n++; while (*w) w++; w++; }
    } else {
        const char *a = (const char *)p;
        while (*a) { if (n == i) found = a; n++; while (*a) a++; a++; }
    }
    *count = n;
    return found;
}

SHSTDAPI_(UINT) DragQueryFileW(HANDLE drop, UINT i, LPWSTR buf, UINT n)
{
    int wide = 1;
    UINT count;
    const void *f = drop_file(drop, i, &wide, &count);
    UINT r;
    if (i == 0xFFFFFFFF) r = count;
    else if (!f) r = 0;
    else if (wide) {
        UINT len = wlen(f);
        if (!buf || !n) r = len;
        else { UINT k = len < n - 1 ? len : n - 1; for (UINT j = 0; j < k; j++) buf[j] = ((const WCHAR *)f)[j]; buf[k] = 0; r = k; }
    } else {
        int len = MultiByteToWideChar(CP_ACP, 0, f, -1, NULL, 0) - 1;
        if (!buf || !n) r = (UINT)len;
        else { WCHAR t[MAX_PATH]; MultiByteToWideChar(CP_ACP, 0, f, -1, t, MAX_PATH); UINT k = (UINT)len < n - 1 ? (UINT)len : n - 1; for (UINT j = 0; j < k; j++) buf[j] = t[j]; buf[k] = 0; r = k; }
    }
    if (drop) GlobalUnlock(drop);
    return r;
}

SHSTDAPI_(UINT) DragQueryFileA(HANDLE drop, UINT i, LPSTR buf, UINT n)
{
    if (i == 0xFFFFFFFF) return DragQueryFileW(drop, i, NULL, 0);
    WCHAR w[MAX_PATH];
    UINT len = DragQueryFileW(drop, i, w, MAX_PATH);
    if (!buf || !n) return len ? (UINT)WideCharToMultiByte(CP_ACP, 0, w, -1, NULL, 0, NULL, NULL) - 1 : 0;
    if (!len && !w[0]) { buf[0] = 0; return 0; }
    int k = WideCharToMultiByte(CP_ACP, 0, w, -1, buf, (int)n, NULL, NULL);
    if (!k) { buf[n - 1] = 0; return n - 1; }
    return (UINT)k - 1;
}

SHSTDAPI_(BOOL) DragQueryPoint(HANDLE drop, POINT *pt)
{
    const DROPFILES *d = drop ? GlobalLock(drop) : NULL;
    if (!d) { if (pt) pt->x = pt->y = 0; return FALSE; }
    if (pt) *pt = d->pt;
    BOOL nc = d->fNC;
    GlobalUnlock(drop);
    return !nc;
}

SHSTDAPI_(void) DragFinish(HANDLE drop) { if (drop) GlobalFree(drop); }
SHSTDAPI_(BOOL) Shell_NotifyIconW(DWORD msg, void *data) { (void)msg; (void)data; return FALSE; }   /* no tray icons */
SHSTDAPI_(BOOL) Shell_NotifyIconA(DWORD msg, void *data) { (void)msg; (void)data; return FALSE; }
SHSTDAPI_(HRESULT) Shell_NotifyIconGetRect(const void *id, RECT *r) { (void)id; if (r) SetRectEmpty(r); return E_FAIL; }
SHSTDAPI_(void) SHChangeNotify(LONG ev, UINT flags, LPCVOID a, LPCVOID b) { (void)ev; (void)flags; (void)a; (void)b; }

/* Change-notification registrations (ordinals 2 and 4): each gets an ID
 * that SHChangeNotifyDeregister ends.  Nothing reports shell changes to
 * them yet (SHChangeNotify above delivers nothing), so a window that
 * registers simply hears no news, as on a system where nothing changes. */
static volatile LONG g_notify_next, g_notify_live[64];
SHSTDAPI_(ULONG) SHChangeNotifyRegister(HWND w, int sources, LONG events, UINT msg, int n, const void *entries)
{
    (void)sources; (void)events; (void)msg; (void)entries;
    if (!w || n < 1) return 0;
    for (int i = 0; i < 64; i++)
        if (!g_notify_live[i]) {
            LONG id = InterlockedIncrement(&g_notify_next);
            if (!InterlockedCompareExchange(&g_notify_live[i], id, 0)) return (ULONG)id;
        }
    return 0;
}
SHSTDAPI_(BOOL) SHChangeNotifyDeregister(ULONG id)
{
    for (int i = 0; id && i < 64; i++)
        if (InterlockedCompareExchange(&g_notify_live[i], 0, (LONG)id) == (LONG)id) return TRUE;
    return FALSE;
}
SHSTDAPI_(void) SHAddToRecentDocs(UINT flags, LPCVOID pv) { (void)flags; (void)pv; }
SHSTDAPI_(BOOL) IsUserAnAdmin(void) { return token_dword(20 /* TokenElevation */, 0) != 0; }   /* elevated? */
SHSTDAPI_(HRESULT) SHQueryRecycleBinW(LPCWSTR root, void *info)
{
    (void)root;
    DWORD *q = info;                                        /* SHQUERYRBINFO: cbSize, i64Size, i64NumItems */
    if (q && q[0] >= 20) { for (int i = 1; i < 5; i++) q[i] = 0; }
    return S_OK_;
}
SHSTDAPI_(HRESULT) SHEmptyRecycleBinW(HWND h, LPCWSTR root, DWORD flags) { (void)h; (void)root; (void)flags; return S_OK_; }
SHSTDAPI_(int) ShellAboutW(HWND h, LPCWSTR app, LPCWSTR other, HICON icon)
{
    (void)icon;
    WCHAR text[512];
    int o = 0;
    const char *head = "NovaOS\n";
    while (*head) text[o++] = (WCHAR)*head++;
    for (const WCHAR *c = other; c && *c && o < 500; c++) text[o++] = *c;
    text[o] = 0;
    char a[1024], t[256];
    w2a(text, a, sizeof(a));
    if (app) w2a(app, t, sizeof(t)); else t[0] = 0;
    MessageBoxA(h, a, t, 0);
    return 1;
}

/* SHGetMalloc: the IMalloc every shell caller frees PIDLs with (the process heap) */
typedef struct Malloc { const struct MallocVtbl *v; } Malloc;
struct MallocVtbl {
    HRESULT (WINAPI *QueryInterface)(Malloc *, REFIID, void **);
    ULONG (WINAPI *AddRef)(Malloc *);
    ULONG (WINAPI *Release)(Malloc *);
    void *(WINAPI *Alloc)(Malloc *, SIZE_T);
    void *(WINAPI *Realloc)(Malloc *, void *, SIZE_T);
    void (WINAPI *Free)(Malloc *, void *);
    SIZE_T (WINAPI *GetSize)(Malloc *, void *);
    int (WINAPI *DidAlloc)(Malloc *, void *);
    void (WINAPI *HeapMinimize)(Malloc *);
};
static HRESULT WINAPI m_qi(Malloc *m, REFIID r, void **o) { (void)r; *o = m; return S_OK_; }
static ULONG WINAPI m_ref(Malloc *m) { (void)m; return 1; }
static void *WINAPI m_alloc(Malloc *m, SIZE_T n) { (void)m; return HeapAlloc(GetProcessHeap(), 0, n); }
static void *WINAPI m_realloc(Malloc *m, void *p, SIZE_T n)
{
    (void)m;
    if (!p) return HeapAlloc(GetProcessHeap(), 0, n);
    if (!n) { HeapFree(GetProcessHeap(), 0, p); return 0; }
    return HeapReAlloc(GetProcessHeap(), 0, p, n);
}
static void WINAPI m_free(Malloc *m, void *p) { (void)m; if (p) HeapFree(GetProcessHeap(), 0, p); }
static SIZE_T WINAPI m_size(Malloc *m, void *p) { (void)m; return p ? HeapSize(GetProcessHeap(), 0, p) : (SIZE_T)-1; }
static int WINAPI m_did(Malloc *m, void *p) { (void)m; (void)p; return -1; }
static void WINAPI m_min(Malloc *m) { (void)m; }
static const struct MallocVtbl g_malloc_vtbl = { m_qi, m_ref, m_ref, m_alloc, m_realloc, m_free, m_size, m_did, m_min };
static Malloc g_malloc = { &g_malloc_vtbl };

SHSTDAPI_(HRESULT) SHGetMalloc(void **out) { *out = &g_malloc; return S_OK_; }
SHSTDAPI_(HRESULT) SHGetDesktopFolder(void **out) { *out = 0; return E_NOTIMPL_; }
SHSTDAPI_(HRESULT) SHGetStockIconInfo(int id, UINT flags, void *info) { (void)id; (void)flags; (void)info; return E_NOTIMPL_; }
SHSTDAPI_(HRESULT) SetCurrentProcessExplicitAppUserModelID(LPCWSTR id) { (void)id; return S_OK_; }
SHSTDAPI_(HRESULT) GetCurrentProcessExplicitAppUserModelID(LPWSTR *id) { *id = 0; return E_FAIL_; }
/* Shell items: shellitem.c */

/* A window's property store (its AppUserModelID, relaunch command...):
 * kept in memory by propsys; the taskbar does not read it */
typedef HRESULT (WINAPI *PSCreateMemoryPropertyStore_t)(REFIID, void **);
SHSTDAPI_(HRESULT) SHGetPropertyStoreForWindow(HWND h, REFIID iid, void **out)
{
    (void)h;
    if (!out) return E_POINTER;
    *out = 0;
    HMODULE ps = LoadLibraryA("propsys.dll");
    PSCreateMemoryPropertyStore_t create = ps ? (PSCreateMemoryPropertyStore_t)GetProcAddress(ps, "PSCreateMemoryPropertyStore") : 0;
    return create ? create(iid, out) : E_NOTIMPL;
}

/* Notifications are always welcome (QUNS_ACCEPTS_NOTIFICATIONS) */
SHSTDAPI_(HRESULT) SHQueryUserNotificationState(int *state)
{
    if (!state) return E_POINTER;
    *state = 5;
    return S_OK;
}

/* Explorer selecting files in a folder window: no Explorer to ask */
SHSTDAPI_(HRESULT) SHOpenFolderAndSelectItems(const void *folder, UINT n, const void **items, DWORD flags)
{
    (void)folder; (void)n; (void)items; (void)flags;
    return E_NOTIMPL;
}

/* The default-programs registration object (IApplicationAssociationRegistration):
 * Windows makes CLSID_ApplicationAssociationRegistration, which NovaOS has no
 * server for, so the class is not registered, as on a Windows without it */
SHSTDAPI_(HRESULT) SHCreateAssociationRegistration(REFIID iid, void **out)
{
    (void)iid;
    if (!out) return E_POINTER;
    *out = 0;
    return (HRESULT)0x80040154L;                        /* REGDB_E_CLASSNOTREG */
}
