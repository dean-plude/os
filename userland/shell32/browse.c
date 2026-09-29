/*
 * SHBrowseForFolder: the folder picker.
 *
 * A dialog with the caller's title, the chosen path in an edit box and the
 * subfolders of the current folder in a list: a double-click opens a folder
 * ("..", the parent; at the top, the drives), a single click picks it.
 * The caller's callback gets BFFM_INITIALIZED and BFFM_SELCHANGED and can
 * send BFFM_SETSELECTION / BFFM_ENABLEOK / BFFM_SETOKTEXT back.
 */

#define NOVA_BUILD_SHELL32
#include <windows.h>
#include <commctrl.h>

typedef struct { WORD cb; BYTE abID[1]; } SHITEMID;
typedef struct { SHITEMID mkid; } ITEMIDLIST, *LPITEMIDLIST;
typedef const ITEMIDLIST *LPCITEMIDLIST;
typedef int (CALLBACK *BFFCALLBACK)(HWND, UINT, LPARAM, LPARAM);

typedef struct {
    HWND hwndOwner; LPCITEMIDLIST pidlRoot; LPWSTR pszDisplayName; LPCWSTR lpszTitle;
    UINT ulFlags; BFFCALLBACK lpfn; LPARAM lParam; int iImage;
} BROWSEINFOW;
typedef struct {
    HWND hwndOwner; LPCITEMIDLIST pidlRoot; LPSTR pszDisplayName; LPCSTR lpszTitle;
    UINT ulFlags; BFFCALLBACK lpfn; LPARAM lParam; int iImage;
} BROWSEINFOA;

#define BIF_EDITBOX            0x0010
#define BIF_NEWDIALOGSTYLE     0x0040
#define BIF_NONEWFOLDERBUTTON  0x0200
#define BFFM_INITIALIZED       1
#define BFFM_SELCHANGED        2
#define BFFM_ENABLEOK          (WM_USER + 101)
#define BFFM_SETSELECTIONA     (WM_USER + 102)
#define BFFM_SETSELECTIONW     (WM_USER + 103)
#define BFFM_SETSTATUSTEXTW    (WM_USER + 104)
#define BFFM_SETOKTEXT         (WM_USER + 105)
#define BFFM_SETEXPANDED       (WM_USER + 106)

SHSTDAPI_(BOOL) SHGetPathFromIDListW(LPCITEMIDLIST pidl, LPWSTR path);
SHSTDAPI_(LPITEMIDLIST) ILCreateFromPathW(LPCWSTR path);
SHSTDAPI_(void) ILFree(LPITEMIDLIST pidl);

enum { ID_TITLE = 100, ID_PATH = 101, ID_LIST = 102, ID_NEWFOLDER = 103 };

typedef struct {
    BROWSEINFOW *bi;
    WCHAR cur[MAX_PATH];        /* the folder whose subfolders are listed; "" = the drives */
    WCHAR sel[MAX_PATH];        /* the chosen folder */
} Browse;

static int wlen(const WCHAR *s) { int n = 0; if (s) while (s[n]) n++; return n; }
static void wcopy(WCHAR *d, const WCHAR *s) { while ((*d++ = *s++)) ; }

static void strip_slash(WCHAR *p)
{
    int n = wlen(p);
    while (n > 3 && (p[n - 1] == '\\' || p[n - 1] == '/')) p[--n] = 0;
}

static void join(WCHAR *out, const WCHAR *dir, const WCHAR *name)
{
    int n = wlen(dir);
    wcopy(out, dir);
    if (n && out[n - 1] != '\\') out[n++] = '\\';
    if (n + wlen(name) < MAX_PATH) wcopy(out + n, name);
}

static void notify_sel(HWND h, Browse *b)
{
    if (!b->bi->lpfn) return;
    LPITEMIDLIST p = ILCreateFromPathW(b->sel);
    b->bi->lpfn(h, BFFM_SELCHANGED, (LPARAM)p, b->bi->lParam);
    ILFree(p);
}

static void set_sel(HWND h, Browse *b, const WCHAR *path)
{
    wcopy(b->sel, path);
    SetDlgItemTextW(h, ID_PATH, path);
    notify_sel(h, b);
}

static void fill(HWND h, Browse *b)
{
    HWND lb = GetDlgItem(h, ID_LIST);
    SendMessageW(lb, WM_SETREDRAW, FALSE, 0);
    SendMessageW(lb, LB_RESETCONTENT, 0, 0);
    if (!b->cur[0]) {                                       /* the drives */
        WCHAR drives[128];
        DWORD n = GetLogicalDriveStringsW(128, drives);
        for (WCHAR *d = drives; n && *d; d += wlen(d) + 1) SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)d);
    } else {
        SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)L"..");
        WCHAR pat[MAX_PATH];
        join(pat, b->cur, L"*");
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW(pat, &fd);
        if (f != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (fd.cFileName[0] == '.' && (!fd.cFileName[1] || (fd.cFileName[1] == '.' && !fd.cFileName[2]))) continue;
                if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
                SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)fd.cFileName);
            } while (FindNextFileW(f, &fd));
            FindClose(f);
        }
    }
    SendMessageW(lb, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lb, NULL, TRUE);
}

/* show the contents of PATH (a folder) and choose it */
static void open_folder(HWND h, Browse *b, const WCHAR *path)
{
    wcopy(b->cur, path);
    strip_slash(b->cur);
    fill(h, b);
    set_sel(h, b, b->cur[0] ? b->cur : L"");
}

static void go_up(HWND h, Browse *b)
{
    WCHAR p[MAX_PATH];
    wcopy(p, b->cur);
    int n = wlen(p);
    if (n <= 3) { p[0] = 0; }                               /* a drive root: to the drives */
    else {
        while (n > 0 && p[n - 1] != '\\') n--;
        p[n > 3 ? n - 1 : n] = 0;
    }
    open_folder(h, b, p);
}

/* the list item at index I, as a full path */
static BOOL item_path(HWND h, Browse *b, int i, WCHAR *out)
{
    WCHAR name[MAX_PATH];
    HWND lb = GetDlgItem(h, ID_LIST);
    if (i < 0 || SendMessageW(lb, LB_GETTEXTLEN, i, 0) >= MAX_PATH) return FALSE;
    SendMessageW(lb, LB_GETTEXT, i, (LPARAM)name);
    if (!b->cur[0]) { wcopy(out, name); return TRUE; }
    if (name[0] == '.' && name[1] == '.' && !name[2]) return FALSE;
    join(out, b->cur, name);
    return TRUE;
}

static void new_folder(HWND h, Browse *b)
{
    if (!b->cur[0]) return;
    WCHAR p[MAX_PATH], name[40];
    for (int i = 1; i < 100; i++) {
        wcopy(name, L"New folder");
        if (i > 1) {
            int n = wlen(name);
            name[n++] = ' '; name[n++] = '(';
            if (i >= 10) name[n++] = (WCHAR)('0' + i / 10);
            name[n++] = (WCHAR)('0' + i % 10); name[n++] = ')'; name[n] = 0;
        }
        join(p, b->cur, name);
        if (CreateDirectoryW(p, NULL)) {
            fill(h, b);
            HWND lb = GetDlgItem(h, ID_LIST);
            LRESULT at = SendMessageW(lb, LB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)name);
            if (at >= 0) SendMessageW(lb, LB_SETCURSEL, at, 0);
            set_sel(h, b, p);
            return;
        }
        if (GetLastError() != ERROR_ALREADY_EXISTS) break;
    }
    MessageBeep(MB_ICONWARNING);
}

static void select_path(HWND h, Browse *b, const WCHAR *path)
{
    WCHAR full[MAX_PATH];
    if (!path || !GetFullPathNameW(path, MAX_PATH, full, NULL)) return;
    strip_slash(full);
    DWORD a = GetFileAttributesW(full);
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
        open_folder(h, b, full);
        return;
    }
    /* a folder that does not exist yet: show its nearest existing parent, keep the name */
    WCHAR p[MAX_PATH];
    wcopy(p, full);
    for (int n = wlen(p); n > 3; ) {
        while (n > 0 && p[n - 1] != '\\') n--;
        if (n <= 0) break;
        p[n > 3 ? n - 1 : n] = 0;
        n = wlen(p);
        a = GetFileAttributesW(p);
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) { wcopy(b->cur, p); fill(h, b); break; }
    }
    set_sel(h, b, full);
}

static INT_PTR CALLBACK browse_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    Browse *b = (Browse *)GetWindowLongPtrW(h, DWLP_USER);
    switch (msg) {
    case WM_INITDIALOG: {
        b = (Browse *)lp;
        SetWindowLongPtrW(h, DWLP_USER, (LONG_PTR)b);
        if (b->bi->lpszTitle) SetDlgItemTextW(h, ID_TITLE, b->bi->lpszTitle);
        WCHAR start[MAX_PATH] = { 0 };
        if (!b->bi->pidlRoot || !SHGetPathFromIDListW(b->bi->pidlRoot, start)) {
            if (!GetCurrentDirectoryW(MAX_PATH, start)) start[0] = 0;
        }
        open_folder(h, b, start);
        if (b->bi->lpfn) b->bi->lpfn(h, BFFM_INITIALIZED, 0, b->bi->lParam);
        SetFocus(GetDlgItem(h, ID_LIST));
        return FALSE;
    }
    case BFFM_SETSELECTIONW:
    case BFFM_SETEXPANDED:
    case BFFM_SETSELECTIONA: {
        WCHAR w[MAX_PATH];
        const WCHAR *path = (const WCHAR *)lp;
        if (!wp) { if (!SHGetPathFromIDListW((LPCITEMIDLIST)lp, w)) return TRUE; path = w; }
        else if (msg == BFFM_SETSELECTIONA) { MultiByteToWideChar(CP_ACP, 0, (LPCSTR)lp, -1, w, MAX_PATH); path = w; }
        select_path(h, b, path);
        return TRUE;
    }
    case BFFM_ENABLEOK: EnableWindow(GetDlgItem(h, IDOK), lp != 0); return TRUE;
    case BFFM_SETOKTEXT: SetDlgItemTextW(h, IDOK, (LPCWSTR)lp); return TRUE;
    case BFFM_SETSTATUSTEXTW: return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_LIST: {
            int i = (int)SendMessageW((HWND)lp, LB_GETCURSEL, 0, 0);
            WCHAR p[MAX_PATH];
            if (HIWORD(wp) == LBN_DBLCLK) {
                if (item_path(h, b, i, p)) open_folder(h, b, p);
                else if (i >= 0) go_up(h, b);
            } else if (HIWORD(wp) == LBN_SELCHANGE && item_path(h, b, i, p)) {
                set_sel(h, b, p);
            }
            return TRUE;
        }
        case ID_NEWFOLDER: new_folder(h, b); return TRUE;
        case IDOK: {
            WCHAR p[MAX_PATH];
            GetDlgItemTextW(h, ID_PATH, p, MAX_PATH);
            if (!p[0]) { MessageBeep(MB_ICONWARNING); return TRUE; }
            WCHAR full[MAX_PATH];
            if (!GetFullPathNameW(p, MAX_PATH, full, NULL)) return TRUE;
            strip_slash(full);
            wcopy(b->sel, full);
            EndDialog(h, IDOK);
            return TRUE;
        }
        case IDCANCEL: EndDialog(h, IDCANCEL); return TRUE;
        }
        break;
    }
    return FALSE;
}

/* an in-memory dialog template */
typedef struct { WORD *p; } Tb;
static void t_word(Tb *t, WORD w) { *t->p++ = w; }
static void t_str(Tb *t, const WCHAR *s) { do t_word(t, *s); while (*s++); }
static void t_align(Tb *t) { if ((ULONG_PTR)t->p & 2) t_word(t, 0); }
static void t_item(Tb *t, DWORD style, short x, short y, short cx, short cy, WORD id, WORD cls, const WCHAR *text)
{
    t_align(t);
    DLGITEMTEMPLATE *it = (DLGITEMTEMPLATE *)t->p;
    it->style = style | WS_CHILD | WS_VISIBLE; it->dwExtendedStyle = 0;
    it->x = x; it->y = y; it->cx = cx; it->cy = cy; it->id = id;
    t->p = (WORD *)(it + 1);
    t_word(t, 0xFFFF); t_word(t, cls);
    t_str(t, text);
    t_word(t, 0);
}

SHSTDAPI_(LPITEMIDLIST) SHBrowseForFolderW(BROWSEINFOW *bi)
{
    if (!bi) return 0;
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    static DWORD buf[512];
    Tb t = { (WORD *)buf };
    BOOL newbtn = (bi->ulFlags & BIF_NEWDIALOGSTYLE) && !(bi->ulFlags & BIF_NONEWFOLDERBUTTON);
    DLGTEMPLATE *d = (DLGTEMPLATE *)t.p;
    d->style = DS_MODALFRAME | DS_SETFONT | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    d->dwExtendedStyle = 0;
    d->cdit = newbtn ? 6 : 5;
    d->x = d->y = 0; d->cx = 240; d->cy = 216;
    t.p = (WORD *)(d + 1);
    t_word(&t, 0); t_word(&t, 0);                            /* no menu, the dialog class */
    t_str(&t, L"Browse For Folder");
    t_word(&t, 8); t_str(&t, L"MS Shell Dlg");
    t_item(&t, SS_LEFT, 7, 6, 226, 20, ID_TITLE, 0x82, L"");
    t_item(&t, ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 7, 28, 226, 13, ID_PATH, 0x81, L"");
    t_item(&t, LBS_NOTIFY | WS_VSCROLL | WS_BORDER | WS_TABSTOP | LBS_NOINTEGRALHEIGHT, 7, 46, 226, 142, ID_LIST, 0x83, L"");
    if (newbtn) t_item(&t, BS_PUSHBUTTON | WS_TABSTOP, 7, 195, 76, 14, ID_NEWFOLDER, 0x80, L"&Make New Folder");
    t_item(&t, BS_DEFPUSHBUTTON | WS_TABSTOP, 129, 195, 50, 14, IDOK, 0x80, L"OK");
    t_item(&t, BS_PUSHBUTTON | WS_TABSTOP, 183, 195, 50, 14, IDCANCEL, 0x80, L"Cancel");

    Browse b = { bi };
    HMODULE self = GetModuleHandleW(L"shell32.dll");
    if (DialogBoxIndirectParamW(self, (LPCDLGTEMPLATEW)buf, bi->hwndOwner, browse_proc, (LPARAM)&b) != IDOK) return 0;
    if (bi->pszDisplayName) {
        const WCHAR *name = b.sel;
        for (const WCHAR *s = b.sel; *s; s++) if (*s == '\\' && s[1]) name = s + 1;
        int n = wlen(name);
        if (n >= MAX_PATH) n = MAX_PATH - 1;
        for (int i = 0; i < n; i++) bi->pszDisplayName[i] = name[i];
        bi->pszDisplayName[n] = 0;
    }
    return ILCreateFromPathW(b.sel);
}

SHSTDAPI_(LPITEMIDLIST) SHBrowseForFolderA(BROWSEINFOA *bi)
{
    if (!bi) return 0;
    WCHAR title[512], name[MAX_PATH];
    BROWSEINFOW w = { bi->hwndOwner, bi->pidlRoot, bi->pszDisplayName ? name : NULL, NULL,
                      bi->ulFlags, bi->lpfn, bi->lParam, 0 };
    if (bi->lpszTitle) { MultiByteToWideChar(CP_ACP, 0, bi->lpszTitle, -1, title, 512); w.lpszTitle = title; }
    LPITEMIDLIST r = SHBrowseForFolderW(&w);
    if (r && bi->pszDisplayName) WideCharToMultiByte(CP_ACP, 0, name, -1, bi->pszDisplayName, MAX_PATH, NULL, NULL);
    return r;
}
