/*
 * filedlg.c — the Open and Save As dialog, and GetOpenFileName /
 * GetSaveFileName on top of it.
 *
 * A folder path with an Up button, the folder's contents in a list view
 * (folders first, then the files the chosen filter matches, with the
 * shell's icons, sizes and dates; at the top, the drives), the file name
 * and the file types.  Double-click or Enter opens a folder or picks a
 * file; typing a path or a folder name goes there, typing a wildcard
 * filters the list.  Open checks that the file exists, Save As asks
 * before replacing one, and a name without an extension gets the
 * filter's or the default one.
 */

#include <windows.h>
#include <commctrl.h>
#include "filedlg.h"

#define CDAPI __declspec(dllexport)

__declspec(dllimport) BOOL WINAPI Shell_GetImageLists(HIMAGELIST *large, HIMAGELIST *small);
typedef struct { HICON hIcon; int iIcon; DWORD dwAttributes; WCHAR szDisplayName[MAX_PATH]; WCHAR szTypeName[80]; } SHFILEINFOW_;
__declspec(dllimport) DWORD_PTR WINAPI SHGetFileInfoW(LPCWSTR path, DWORD attrs, SHFILEINFOW_ *info, UINT n, UINT flags);
#define SHGFI_SYSICONINDEX_     0x4000
#define SHGFI_SMALLICON_        0x0001
#define SHGFI_TYPENAME_         0x0400
#define SHGFI_USEFILEATTRIBUTES_ 0x0010

enum { ID_LOOKIN = 1090, ID_DIR = 1091, ID_UP = 1092, ID_NEWDIR = 1093, ID_LIST = 1094,
       ID_NAMELABEL = 1095, ID_NAME = 1096, ID_TYPELABEL = 1097, ID_TYPE = 1098 };

struct FdEntry {
    WCHAR name[MAX_PATH];
    BOOL dir;
    ULONGLONG size;
    FILETIME mtime;
};

static DWORD g_error;                   /* CommDlgExtendedError */

static int wlen(const WCHAR *s) { int n = 0; if (s) while (s[n]) n++; return n; }
static void wcopy(WCHAR *d, const WCHAR *s, int cap)
{
    int i = 0;
    for (; s && s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = 0;
}
static WCHAR lower(WCHAR c) { return c >= 'A' && c <= 'Z' ? (WCHAR)(c + 32) : c; }

/* a wildcard pattern (* and ?), case-insensitive */
static BOOL match1(const WCHAR *p, const WCHAR *pe, const WCHAR *s)
{
    while (p < pe) {
        if (*p == '*') {
            while (p < pe && *p == '*') p++;
            if (p == pe) return TRUE;
            for (; *s; s++) if (match1(p, pe, s)) return TRUE;
            return FALSE;
        }
        if (!*s) return FALSE;
        if (*p != '?' && lower(*p) != lower(*s)) return FALSE;
        p++; s++;
    }
    /* "*.*" matches names without a dot too, as on Windows */
    return !*s;
}

/* SPEC: patterns separated by ';' (spaces around them ignored) */
static BOOL match_spec(const WCHAR *spec, const WCHAR *name)
{
    if (!spec || !*spec) return TRUE;
    for (const WCHAR *p = spec; *p;) {
        while (*p == ' ' || *p == ';') p++;
        const WCHAR *e = p;
        while (*e && *e != ';') e++;
        const WCHAR *t = e;
        while (t > p && t[-1] == ' ') t--;
        if (t > p) {
            if (t - p == 3 && p[0] == '*' && p[1] == '.' && p[2] == '*') return TRUE;
            if (match1(p, t, name)) return TRUE;
        }
        p = e;
    }
    return FALSE;
}

static BOOL has_wild(const WCHAR *s) { for (; *s; s++) if (*s == '*' || *s == '?') return TRUE; return FALSE; }

static const WCHAR *leaf(const WCHAR *p)
{
    const WCHAR *l = p;
    for (const WCHAR *s = p; *s; s++) if ((*s == '\\' || *s == '/') && s[1]) l = s + 1;
    return l;
}

static BOOL has_ext(const WCHAR *name)
{
    const WCHAR *l = leaf(name), *dot = 0;
    for (const WCHAR *s = l; *s; s++) if (*s == '.') dot = s;
    return dot && dot != l && dot[1];
}

static void join(WCHAR *out, const WCHAR *dir, const WCHAR *name)
{
    int n = wlen(dir);
    wcopy(out, dir, MAX_PATH);
    if (n && out[n - 1] != '\\' && n < MAX_PATH - 1) out[n++] = '\\', out[n] = 0;
    if (n + wlen(name) < MAX_PATH) wcopy(out + n, name, MAX_PATH - n);
}

static void strip_slash(WCHAR *p)
{
    int n = wlen(p);
    while (n > 3 && (p[n - 1] == '\\' || p[n - 1] == '/')) p[--n] = 0;
}

static BOOL is_dir(const WCHAR *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static BOOL exists(const WCHAR *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

/* NAME, relative to the folder shown, as a full path */
static BOOL resolve(FileDlg *d, const WCHAR *name, WCHAR *out)
{
    WCHAR tmp[MAX_PATH];
    BOOL abs = (name[0] && name[1] == ':') || name[0] == '\\' || name[0] == '/';
    if (abs || !d->cur[0]) wcopy(tmp, name, MAX_PATH);
    else join(tmp, d->cur, name);
    if (!GetFullPathNameW(tmp, MAX_PATH, out, 0)) return FALSE;
    strip_slash(out);
    return TRUE;
}

/* -----------------------------------------------------------------------
 * The list
 * ----------------------------------------------------------------------- */
static const WCHAR *cur_spec(FileDlg *d)
{
    if (d->custom[0]) return d->custom;
    if (d->filter >= 0 && d->filter < d->nfilters) return d->filters[d->filter].spec;
    return L"*.*";
}

static void add_entry(FileDlg *d, const WCHAR *name, BOOL dir, ULONGLONG size, FILETIME mt)
{
    if (d->nents == d->cap) {
        int cap = d->cap ? d->cap * 2 : 256;
        FdEntry *n = d->ents ? HeapReAlloc(GetProcessHeap(), 0, d->ents, sizeof(FdEntry) * cap)
                             : HeapAlloc(GetProcessHeap(), 0, sizeof(FdEntry) * cap);
        if (!n) return;
        d->ents = n; d->cap = cap;
    }
    FdEntry *e = &d->ents[d->nents++];
    wcopy(e->name, name, MAX_PATH);
    e->dir = dir; e->size = size; e->mtime = mt;
}

static int ent_cmp(const FdEntry *a, const FdEntry *b)
{
    if (a->dir != b->dir) return a->dir ? -1 : 1;
    return lstrcmpiW(a->name, b->name);
}

static void sort_entries(FileDlg *d)
{
    /* shell sort: folders are a few hundred entries at most */
    for (int gap = d->nents / 2; gap > 0; gap /= 2)
        for (int i = gap; i < d->nents; i++) {
            FdEntry t = d->ents[i];
            int j = i;
            for (; j >= gap && ent_cmp(&d->ents[j - gap], &t) > 0; j -= gap) d->ents[j] = d->ents[j - gap];
            d->ents[j] = t;
        }
}

static void fmt_size(ULONGLONG v, WCHAR *out)
{
    ULONGLONG kb = (v + 1023) / 1024;
    WCHAR t[32];
    int n = 0, k = 0;
    do { if (k && k % 3 == 0) t[n++] = ','; t[n++] = (WCHAR)('0' + kb % 10); kb /= 10; k++; } while (kb);
    int o = 0;
    while (n) out[o++] = t[--n];
    out[o++] = ' '; out[o++] = 'K'; out[o++] = 'B'; out[o] = 0;
}

static void fmt_time(FILETIME ft, WCHAR *out)
{
    FILETIME lt;
    SYSTEMTIME st;
    out[0] = 0;
    if (!ft.dwHighDateTime && !ft.dwLowDateTime) return;
    if (!FileTimeToLocalFileTime(&ft, &lt) || !FileTimeToSystemTime(&lt, &st)) return;
    wsprintfW(out, L"%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
}

static void fill(FileDlg *d)
{
    HWND lv = GetDlgItem(d->hwnd, ID_LIST);
    d->nents = 0;
    if (!d->cur[0]) {
        WCHAR drives[128];
        DWORD n = GetLogicalDriveStringsW(128, drives);
        FILETIME z = { 0, 0 };
        for (WCHAR *s = drives; n && *s; s += wlen(s) + 1) add_entry(d, s, TRUE, 0, z);
    } else {
        WCHAR pat[MAX_PATH];
        join(pat, d->cur, L"*");
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW(pat, &fd);
        BOOL hidden = (d->fos & FOS_FORCESHOWHIDDEN) != 0;
        if (f != INVALID_HANDLE_VALUE) {
            do {
                const WCHAR *nm = fd.cFileName;
                if (nm[0] == '.' && (!nm[1] || (nm[1] == '.' && !nm[2]))) continue;
                if (!hidden && (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
                BOOL dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                if (!dir && ((d->fos & FOS_PICKFOLDERS) || !match_spec(cur_spec(d), nm))) continue;
                add_entry(d, nm, dir, ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow, fd.ftLastWriteTime);
            } while (FindNextFileW(f, &fd));
            FindClose(f);
        }
    }
    sort_entries(d);

    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    SendMessageW(lv, LVM_DELETEALLITEMS, 0, 0);
    for (int i = 0; i < d->nents; i++) {
        FdEntry *e = &d->ents[i];
        WCHAR full[MAX_PATH], buf[64];
        if (d->cur[0]) join(full, d->cur, e->name); else wcopy(full, e->name, MAX_PATH);
        SHFILEINFOW_ sfi;
        SHGetFileInfoW(full, e->dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, &sfi, sizeof sfi,
                       SHGFI_SYSICONINDEX_ | SHGFI_SMALLICON_ | SHGFI_TYPENAME_ | SHGFI_USEFILEATTRIBUTES_);
        LVITEMW it = { 0 };
        it.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
        it.iItem = i;
        it.pszText = e->name;
        it.iImage = sfi.iIcon;
        it.lParam = i;
        int at = (int)SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&it);
        if (at < 0) continue;
        it.mask = LVIF_TEXT;
        it.iItem = at;
        if (!e->dir) {
            fmt_size(e->size, buf);
            it.iSubItem = 1; it.pszText = buf;
            SendMessageW(lv, LVM_SETITEMTEXTW, at, (LPARAM)&it);
        }
        it.iSubItem = 2; it.pszText = d->cur[0] ? sfi.szTypeName : (WCHAR *)L"Local Disk";
        SendMessageW(lv, LVM_SETITEMTEXTW, at, (LPARAM)&it);
        fmt_time(e->mtime, buf);
        it.iSubItem = 3; it.pszText = buf;
        SendMessageW(lv, LVM_SETITEMTEXTW, at, (LPARAM)&it);
    }
    SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lv, NULL, TRUE);
}

static void show_folder(FileDlg *d)
{
    SetDlgItemTextW(d->hwnd, ID_DIR, d->cur[0] ? d->cur : L"This PC");
    EnableWindow(GetDlgItem(d->hwnd, ID_UP), d->cur[0] != 0);
    EnableWindow(GetDlgItem(d->hwnd, ID_NEWDIR), d->cur[0] != 0);
    fill(d);
}

static void go(FileDlg *d, const WCHAR *dir)
{
    wcopy(d->cur, dir, MAX_PATH);
    strip_slash(d->cur);
    if (d->hwnd) {
        show_folder(d);
        if (d->on_folder) d->on_folder(d);
    }
}

static void go_up(FileDlg *d)
{
    WCHAR p[MAX_PATH];
    wcopy(p, d->cur, MAX_PATH);
    int n = wlen(p);
    if (n <= 3) p[0] = 0;
    else {
        while (n > 0 && p[n - 1] != '\\') n--;
        p[n > 3 ? n - 1 : n] = 0;
    }
    go(d, p);
}

static void new_folder(FileDlg *d)
{
    if (!d->cur[0]) return;
    WCHAR p[MAX_PATH], name[40];
    for (int i = 1; i < 100; i++) {
        if (i == 1) wcopy(name, L"New folder", 40);
        else wsprintfW(name, L"New folder (%d)", i);
        join(p, d->cur, name);
        if (CreateDirectoryW(p, NULL)) {
            fill(d);
            HWND lv = GetDlgItem(d->hwnd, ID_LIST);
            for (int k = 0; k < d->nents; k++)
                if (!lstrcmpiW(d->ents[k].name, name)) {
                    LVITEMW st = { 0 };
                    st.stateMask = st.state = LVIS_SELECTED | LVIS_FOCUSED;
                    SendMessageW(lv, LVM_SETITEMSTATE, k, (LPARAM)&st);
                    SendMessageW(lv, LVM_ENSUREVISIBLE, k, FALSE);
                }
            return;
        }
        if (GetLastError() != ERROR_ALREADY_EXISTS) break;
    }
    MessageBeep(MB_ICONWARNING);
}

/* the entry behind list row I */
static FdEntry *row_entry(FileDlg *d, int i)
{
    if (i < 0) return 0;
    LVITEMW it = { 0 };
    it.mask = LVIF_PARAM;
    it.iItem = i;
    if (!SendMessageW(GetDlgItem(d->hwnd, ID_LIST), LVM_GETITEMW, 0, (LPARAM)&it)) return 0;
    return it.lParam >= 0 && it.lParam < d->nents ? &d->ents[it.lParam] : 0;
}

static int next_selected(FileDlg *d, int after)
{
    return (int)SendMessageW(GetDlgItem(d->hwnd, ID_LIST), LVM_GETNEXTITEM, (WPARAM)after, LVNI_SELECTED);
}

/* the selected files into the name box: one name, or "a" "b" for several */
static void selection_to_name(FileDlg *d)
{
    WCHAR buf[2048];
    int n = 0, files = 0;
    buf[0] = 0;
    for (int i = next_selected(d, -1); i >= 0; i = next_selected(d, i)) {
        FdEntry *e = row_entry(d, i);
        if (!e || (e->dir && !(d->fos & FOS_PICKFOLDERS))) continue;
        if (!d->cur[0]) continue;
        files++;
        int k = wlen(e->name);
        if (n + k + 4 >= 2048) break;
        if (files > 1) {
            if (files == 2) {                        /* quote the first one too */
                for (int j = n; j >= 0; j--) buf[j + 1] = buf[j];
                buf[0] = '"'; n++;
                buf[n++] = '"';
            }
            buf[n++] = ' '; buf[n++] = '"';
            for (int j = 0; j < k; j++) buf[n++] = e->name[j];
            buf[n++] = '"';
        } else {
            for (int j = 0; j < k; j++) buf[n++] = e->name[j];
        }
        buf[n] = 0;
        if (!(d->fos & FOS_ALLOWMULTISELECT)) break;
    }
    if (files) SetDlgItemTextW(d->hwnd, ID_NAME, buf);
    if (d->on_sel) d->on_sel(d);
}

/* -----------------------------------------------------------------------
 * Choosing
 * ----------------------------------------------------------------------- */
void fd_default_ext(FileDlg *d, WCHAR *out)
{
    out[0] = 0;
    /* the filter's own extension when it names exactly one ("*.txt") */
    if (!d->custom[0] && d->filter >= 0 && d->filter < d->nfilters) {
        const WCHAR *s = d->filters[d->filter].spec;
        while (s && *s == ' ') s++;
        if (s && s[0] == '*' && s[1] == '.' && s[2] && s[2] != '*') {
            int k = 0;
            for (const WCHAR *p = s + 2; *p && *p != ';' && *p != ' '; p++) {
                if (*p == '*' || *p == '?') { k = 0; break; }
                if (k < 31) out[k++] = *p;
            }
            out[k] = 0;
            if (k) return;
        }
    }
    wcopy(out, d->defext[0] == '.' ? d->defext + 1 : d->defext, 32);
}

static void add_result(FileDlg *d, const WCHAR *name)
{
    int at = 0;
    for (int i = 0; i < d->res_count; i++) at += wlen(d->res_names + at) + 1;
    int k = wlen(name);
    if (at + k + 2 > FD_NAMES) return;
    wcopy(d->res_names + at, name, k + 1);
    d->res_names[at + k + 1] = 0;
    d->res_count++;
}

static void msg(FileDlg *d, const WCHAR *path, const WCHAR *text, UINT type, int *answer)
{
    WCHAR m[MAX_PATH + 160];
    wsprintfW(m, text, path);
    int r = MessageBoxW(d->hwnd, m, d->title[0] ? d->title : (d->save ? L"Save As" : L"Open"), type);
    if (answer) *answer = r;
}

/* checks one chosen path; may add the default extension */
static BOOL check_file(FileDlg *d, WCHAR *full)
{
    if (!has_ext(full) && !exists(full)) {
        WCHAR ext[32];
        fd_default_ext(d, ext);
        int n = wlen(full);
        if (ext[0] && n + 1 + wlen(ext) < MAX_PATH) { full[n++] = '.'; wcopy(full + n, ext, MAX_PATH - n); }
    }
    if (d->fos & FOS_NOVALIDATE) return TRUE;
    WCHAR dir[MAX_PATH];
    wcopy(dir, full, MAX_PATH);
    int n = wlen(dir);
    while (n > 0 && dir[n - 1] != '\\') n--;
    dir[n > 3 ? n - 1 : n] = 0;
    if (!is_dir(dir)) {
        msg(d, dir, L"%s\nPath does not exist.\nCheck the path and try again.", MB_OK | MB_ICONWARNING, 0);
        return FALSE;
    }
    BOOL there = exists(full);
    if (!d->save && (d->fos & FOS_FILEMUSTEXIST) && !there) {
        msg(d, leaf(full), L"%s\nFile not found.\nCheck the file name and try again.", MB_OK | MB_ICONWARNING, 0);
        return FALSE;
    }
    if (d->save && there && (d->fos & FOS_OVERWRITEPROMPT)) {
        int a;
        msg(d, leaf(full), L"%s already exists.\nDo you want to replace it?", MB_YESNO | MB_ICONWARNING, &a);
        if (a != IDYES) return FALSE;
    }
    if (d->save && !there && (d->fos & FOS_CREATEPROMPT)) {
        int a;
        msg(d, leaf(full), L"%s does not exist.\nDo you want to create it?", MB_YESNO | MB_ICONQUESTION, &a);
        if (a != IDYES) return FALSE;
    }
    return TRUE;
}

static void finish(FileDlg *d)
{
    if (d->on_ok && !d->on_ok(d)) { d->res_count = 0; return; }
    EndDialog(d->hwnd, IDOK);
}

/* splits "a" "b" c into names; returns how many */
static int split_names(const WCHAR *text, WCHAR (*out)[MAX_PATH], int max)
{
    int n = 0;
    const WCHAR *p = text;
    if (*p != '"') { wcopy(out[0], text, MAX_PATH); return 1; }
    while (*p && n < max) {
        while (*p == ' ') p++;
        if (!*p) break;
        int k = 0;
        if (*p == '"') {
            p++;
            while (*p && *p != '"') { if (k < MAX_PATH - 1) out[n][k++] = *p; p++; }
            if (*p == '"') p++;
        } else {
            while (*p && *p != ' ') { if (k < MAX_PATH - 1) out[n][k++] = *p; p++; }
        }
        out[n][k] = 0;
        if (k) n++;
    }
    return n;
}

static void on_ok(FileDlg *d)
{
    HWND focus = GetFocus();
    d->res_count = 0;
    d->res_dir[0] = 0;

    if (focus == GetDlgItem(d->hwnd, ID_DIR)) {             /* Enter in the folder box: go there */
        WCHAR t[MAX_PATH], full[MAX_PATH];
        GetDlgItemTextW(d->hwnd, ID_DIR, t, MAX_PATH);
        if (!lstrcmpiW(t, L"This PC") || !t[0]) { go(d, L""); return; }
        if (resolve(d, t, full) && is_dir(full)) { go(d, full); return; }
        msg(d, t, L"Cannot find '%s'.", MB_OK | MB_ICONWARNING, 0);
        return;
    }
    if (focus == GetDlgItem(d->hwnd, ID_LIST)) {            /* Enter on a folder in the list: open it */
        int i = next_selected(d, -1);
        FdEntry *e = row_entry(d, i);
        if (e && e->dir && (!(d->fos & FOS_PICKFOLDERS) || !d->cur[0])) {
            WCHAR p[MAX_PATH];
            if (d->cur[0]) join(p, d->cur, e->name); else wcopy(p, e->name, MAX_PATH);
            go(d, p);
            return;
        }
    }

    WCHAR text[2048];
    GetDlgItemTextW(d->hwnd, ID_NAME, text, 2048);
    int n = wlen(text);
    while (n && text[n - 1] == ' ') text[--n] = 0;
    WCHAR *t = text;
    while (*t == ' ') t++;

    if (d->fos & FOS_PICKFOLDERS) {
        WCHAR full[MAX_PATH];
        if (!*t) {
            if (!d->cur[0]) { MessageBeep(MB_ICONWARNING); return; }
            wcopy(full, d->cur, MAX_PATH);
        } else if (!resolve(d, t, full) || !is_dir(full)) {
            msg(d, t, L"%s\nPath does not exist.\nCheck the path and try again.", MB_OK | MB_ICONWARNING, 0);
            return;
        }
        WCHAR parent[MAX_PATH];
        wcopy(parent, full, MAX_PATH);
        int k = wlen(parent);
        while (k > 0 && parent[k - 1] != '\\') k--;
        parent[k > 3 ? k - 1 : k] = 0;
        wcopy(d->res_dir, parent, MAX_PATH);
        add_result(d, wlen(full) <= 3 ? full : leaf(full));
        finish(d);
        return;
    }

    if (!*t) {                                              /* nothing typed: open the selected folder */
        FdEntry *e = row_entry(d, next_selected(d, -1));
        if (e && e->dir) {
            WCHAR p[MAX_PATH];
            if (d->cur[0]) join(p, d->cur, e->name); else wcopy(p, e->name, MAX_PATH);
            go(d, p);
        }
        return;
    }

    WCHAR (*names)[MAX_PATH] = HeapAlloc(GetProcessHeap(), 0, sizeof(WCHAR[MAX_PATH]) * 64);
    if (!names) return;
    int count = split_names(t, names, 64);
    if (count > 1 && !(d->fos & FOS_ALLOWMULTISELECT)) count = 1;

    if (count == 1) {
        WCHAR full[MAX_PATH];
        if (has_wild(names[0])) {                           /* a wildcard: filter by it */
            WCHAR dirpart[MAX_PATH];
            const WCHAR *l = leaf(names[0]);
            if (l != names[0]) {
                int k = (int)(l - names[0]);
                wcopy(dirpart, names[0], k + 1);
                if (resolve(d, dirpart, full) && is_dir(full)) wcopy(d->cur, full, MAX_PATH);
            }
            wcopy(d->custom, l, MAX_PATH);
            show_folder(d);
            SetDlgItemTextW(d->hwnd, ID_NAME, d->custom);
            goto out;
        }
        if (!resolve(d, names[0], full)) { MessageBeep(MB_ICONWARNING); goto out; }
        if (is_dir(full)) {                                 /* a folder: go there */
            go(d, full);
            SetDlgItemTextW(d->hwnd, ID_NAME, L"");
            goto out;
        }
        if (!check_file(d, full)) goto out;
        WCHAR dir[MAX_PATH];
        wcopy(dir, full, MAX_PATH);
        int k = wlen(dir);
        while (k > 0 && dir[k - 1] != '\\') k--;
        dir[k > 3 ? k - 1 : k] = 0;
        wcopy(d->res_dir, dir, MAX_PATH);
        add_result(d, leaf(full));
        finish(d);
        goto out;
    }

    /* several names, all in the folder shown */
    if (!d->cur[0]) { MessageBeep(MB_ICONWARNING); goto out; }
    wcopy(d->res_dir, d->cur, MAX_PATH);
    for (int i = 0; i < count; i++) {
        WCHAR full[MAX_PATH];
        if (!resolve(d, names[i], full) || !check_file(d, full)) { d->res_count = 0; goto out; }
        add_result(d, leaf(full));
    }
    finish(d);
out:
    HeapFree(GetProcessHeap(), 0, names);
}

/* -----------------------------------------------------------------------
 * The dialog
 * ----------------------------------------------------------------------- */
static void init(FileDlg *d)
{
    HWND h = d->hwnd;
    if (d->title[0]) SetWindowTextW(h, d->title);
    else SetWindowTextW(h, (d->fos & FOS_PICKFOLDERS) ? L"Select Folder" : d->save ? L"Save As" : L"Open");
    SetDlgItemTextW(h, IDOK, d->ok_label[0] ? d->ok_label : (d->fos & FOS_PICKFOLDERS) ? L"Select Folder" : d->save ? L"&Save" : L"&Open");
    SetDlgItemTextW(h, ID_NAMELABEL, d->name_label[0] ? d->name_label : (d->fos & FOS_PICKFOLDERS) ? L"Fol&der:" : L"File &name:");

    HWND lv = GetDlgItem(h, ID_LIST);
    SendMessageW(lv, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_FULLROWSELECT, LVS_EX_FULLROWSELECT);
    HIMAGELIST small = 0;
    Shell_GetImageLists(NULL, &small);
    if (small) SendMessageW(lv, LVM_SETIMAGELIST, LVSIL_SMALL, (LPARAM)small);
    static const WCHAR *const cols[] = { L"Name", L"Size", L"Type", L"Date modified" };
    static const int widths[] = { 250, 80, 130, 120 };
    for (int i = 0; i < 4; i++) {
        LVCOLUMNW c = { 0 };
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM | LVCF_FMT;
        c.fmt = i == 1 ? LVCFMT_RIGHT : LVCFMT_LEFT;
        c.cx = widths[i];
        c.pszText = (WCHAR *)cols[i];
        c.iSubItem = i;
        SendMessageW(lv, LVM_INSERTCOLUMNW, i, (LPARAM)&c);
    }

    HWND cb = GetDlgItem(h, ID_TYPE);
    for (int i = 0; i < d->nfilters; i++) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)d->filters[i].name);
    if (!d->nfilters || (d->fos & FOS_PICKFOLDERS)) {
        EnableWindow(cb, FALSE);
        if (d->fos & FOS_PICKFOLDERS) { ShowWindow(cb, SW_HIDE); ShowWindow(GetDlgItem(h, ID_TYPELABEL), SW_HIDE); }
    }
    if (d->filter < 0 || d->filter >= d->nfilters) d->filter = 0;
    SendMessageW(cb, CB_SETCURSEL, d->filter, 0);

    WCHAR start[MAX_PATH];
    wcopy(start, d->dir, MAX_PATH);
    if (!start[0] || !is_dir(start)) GetCurrentDirectoryW(MAX_PATH, start);
    wcopy(d->cur, start, MAX_PATH);
    strip_slash(d->cur);
    show_folder(d);
    SetDlgItemTextW(h, ID_NAME, d->name);
    if (d->on_type) d->on_type(d);
    if (d->on_folder) d->on_folder(d);
}

static INT_PTR CALLBACK dlg_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    FileDlg *d = (FileDlg *)GetWindowLongPtrW(h, DWLP_USER);
    switch (m) {
    case WM_INITDIALOG:
        d = (FileDlg *)lp;
        SetWindowLongPtrW(h, DWLP_USER, (LONG_PTR)d);
        d->hwnd = h;
        init(d);
        SetFocus(GetDlgItem(h, ID_NAME));
        SendDlgItemMessageW(h, ID_NAME, EM_SETSEL, 0, -1);
        return FALSE;
    case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)lp;
        if (!d || nm->idFrom != ID_LIST) break;
        if (nm->code == LVN_ITEMCHANGED) {
            NMLISTVIEW *v = (NMLISTVIEW *)lp;
            if ((v->uChanged & LVIF_STATE) && ((v->uNewState ^ v->uOldState) & LVIS_SELECTED)) selection_to_name(d);
        } else if (nm->code == LVN_ITEMACTIVATE || nm->code == NM_DBLCLK) {
            int i = next_selected(d, -1);
            FdEntry *e = row_entry(d, i);
            if (!e) break;
            if (e->dir) {
                WCHAR p[MAX_PATH];
                if (d->cur[0]) join(p, d->cur, e->name); else wcopy(p, e->name, MAX_PATH);
                go(d, p);
            } else {
                selection_to_name(d);
                SetFocus(GetDlgItem(h, ID_NAME));
                on_ok(d);
            }
        } else if (nm->code == LVN_KEYDOWN && ((NMLVKEYDOWN *)lp)->wVKey == VK_BACK) {
            if (d->cur[0]) go_up(d);
        }
        return TRUE;
    }
    case WM_COMMAND:
        if (!d) break;
        switch (LOWORD(wp)) {
        case IDOK: on_ok(d); return TRUE;
        case IDCANCEL: EndDialog(h, IDCANCEL); return TRUE;
        case ID_UP: go_up(d); return TRUE;
        case ID_NEWDIR: new_folder(d); return TRUE;
        case ID_TYPE:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int i = (int)SendDlgItemMessageW(h, ID_TYPE, CB_GETCURSEL, 0, 0);
                if (i >= 0) {
                    d->filter = i;
                    d->custom[0] = 0;
                    fill(d);
                    if (d->on_type) d->on_type(d);
                }
            }
            return TRUE;
        }
        break;
    case WM_CLOSE:
        EndDialog(h, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

/* an in-memory dialog template */
typedef struct { WORD *p; } Tb;
static void t_word(Tb *t, WORD w) { *t->p++ = w; }
static void t_str(Tb *t, const WCHAR *s) { do t_word(t, *s); while (*s++); }
static void t_align(Tb *t) { if ((ULONG_PTR)t->p & 2) t_word(t, 0); }
static void t_item(Tb *t, DWORD style, short x, short y, short cx, short cy, WORD id, WORD atom, const WCHAR *cls,
                   const WCHAR *text)
{
    t_align(t);
    DLGITEMTEMPLATE *it = (DLGITEMTEMPLATE *)t->p;
    it->style = style | WS_CHILD | WS_VISIBLE; it->dwExtendedStyle = 0;
    it->x = x; it->y = y; it->cx = cx; it->cy = cy; it->id = id;
    t->p = (WORD *)(it + 1);
    if (cls) t_str(t, cls);
    else { t_word(t, 0xFFFF); t_word(t, atom); }
    t_str(t, text);
    t_word(t, 0);
}

BOOL fd_run(FileDlg *d)
{
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    static DWORD buf[1024];
    Tb t = { (WORD *)buf };
    DLGTEMPLATE *dt = (DLGTEMPLATE *)t.p;
    dt->style = DS_MODALFRAME | DS_SETFONT | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    dt->dwExtendedStyle = 0;
    dt->cdit = 12;
    dt->x = dt->y = 0; dt->cx = 380; dt->cy = 232;
    t.p = (WORD *)(dt + 1);
    t_word(&t, 0); t_word(&t, 0);
    t_str(&t, d->save ? L"Save As" : L"Open");
    t_word(&t, 8); t_str(&t, L"MS Shell Dlg");
    t_item(&t, SS_LEFT, 7, 9, 40, 9, ID_LOOKIN, 0x82, 0, L"Look &in:");
    t_item(&t, ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 50, 7, 230, 13, ID_DIR, 0x81, 0, L"");
    t_item(&t, BS_PUSHBUTTON | WS_TABSTOP, 285, 6, 40, 14, ID_UP, 0x80, 0, L"&Up");
    t_item(&t, BS_PUSHBUTTON | WS_TABSTOP, 328, 6, 45, 14, ID_NEWDIR, 0x80, 0, L"New &folder");
    DWORD lvs = LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SHAREIMAGELISTS | WS_BORDER | WS_TABSTOP |
                ((d->fos & FOS_ALLOWMULTISELECT) ? 0 : LVS_SINGLESEL);
    t_item(&t, lvs, 7, 26, 366, 150, ID_LIST, 0, WC_LISTVIEWW, L"");
    t_item(&t, SS_LEFT, 7, 186, 50, 9, ID_NAMELABEL, 0x82, 0, L"File &name:");
    t_item(&t, ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 62, 184, 245, 13, ID_NAME, 0x81, 0, L"");
    t_item(&t, BS_DEFPUSHBUTTON | WS_TABSTOP, 313, 183, 60, 14, IDOK, 0x80, 0, L"&Open");
    t_item(&t, SS_LEFT, 7, 205, 50, 9, ID_TYPELABEL, 0x82, 0, L"Files of &type:");
    t_item(&t, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 62, 203, 245, 100, ID_TYPE, 0x85, 0, L"");
    t_item(&t, BS_PUSHBUTTON | WS_TABSTOP, 313, 202, 60, 14, IDCANCEL, 0x80, 0, L"Cancel");
    t_item(&t, SS_ETCHEDHORZ, 7, 222, 366, 1, 0xFFFF, 0x82, 0, L"");

    d->hwnd = 0;
    d->res_count = 0;
    d->custom[0] = 0;
    HMODULE self = GetModuleHandleW(L"comdlg32.dll");
    INT_PTR r = DialogBoxIndirectParamW(self, (LPCDLGTEMPLATEW)buf, d->owner, dlg_proc, (LPARAM)d);
    if (d->hwnd) {                                      /* keep what was typed, for GetFileName after Show */
        d->hwnd = 0;
    }
    HeapFree(GetProcessHeap(), 0, d->ents);
    d->ents = 0; d->nents = d->cap = 0;
    return r == IDOK && d->res_count > 0;
}

void fd_get_name(FileDlg *d, WCHAR *out, int cap)
{
    if (d->hwnd) GetDlgItemTextW(d->hwnd, ID_NAME, out, cap);
    else if (d->res_count) wcopy(out, d->res_names, cap);
    else wcopy(out, d->name, cap);
}

void fd_set_name(FileDlg *d, const WCHAR *name)
{
    wcopy(d->name, name ? name : L"", MAX_PATH);
    if (d->hwnd) SetDlgItemTextW(d->hwnd, ID_NAME, d->name);
}

void fd_set_folder(FileDlg *d, const WCHAR *dir)
{
    wcopy(d->dir, dir ? dir : L"", MAX_PATH);
    if (d->hwnd && is_dir(d->dir)) go(d, d->dir);
}

void fd_set_filter(FileDlg *d, int index)
{
    d->filter = index;
    if (d->hwnd) {
        SendDlgItemMessageW(d->hwnd, ID_TYPE, CB_SETCURSEL, index, 0);
        d->custom[0] = 0;
        fill(d);
    }
}

BOOL fd_result(FileDlg *d, int n, WCHAR *out)
{
    if (n < 0 || n >= d->res_count) return FALSE;
    const WCHAR *s = d->res_names;
    for (int i = 0; i < n; i++) s += wlen(s) + 1;
    if (wlen(s) <= 3 && s[1] == ':') { wcopy(out, s, MAX_PATH); return TRUE; }   /* a drive (folder picking) */
    join(out, d->res_dir, s);
    return TRUE;
}

void fd_close(FileDlg *d, int code) { if (d->hwnd) EndDialog(d->hwnd, code); }

/* -----------------------------------------------------------------------
 * GetOpenFileName / GetSaveFileName
 * ----------------------------------------------------------------------- */
typedef UINT_PTR (CALLBACK *OFNHOOKPROC_)(HWND, UINT, WPARAM, LPARAM);
typedef struct {
    DWORD lStructSize; HWND hwndOwner; HINSTANCE hInstance; LPCWSTR lpstrFilter; LPWSTR lpstrCustomFilter;
    DWORD nMaxCustFilter, nFilterIndex; LPWSTR lpstrFile; DWORD nMaxFile; LPWSTR lpstrFileTitle; DWORD nMaxFileTitle;
    LPCWSTR lpstrInitialDir, lpstrTitle; DWORD Flags; WORD nFileOffset, nFileExtension; LPCWSTR lpstrDefExt;
    LPARAM lCustData; OFNHOOKPROC_ lpfnHook; LPCWSTR lpTemplateName; void *pvReserved; DWORD dwReserved, FlagsEx;
} OPENFILENAMEW_;
typedef struct {
    DWORD lStructSize; HWND hwndOwner; HINSTANCE hInstance; LPCSTR lpstrFilter; LPSTR lpstrCustomFilter;
    DWORD nMaxCustFilter, nFilterIndex; LPSTR lpstrFile; DWORD nMaxFile; LPSTR lpstrFileTitle; DWORD nMaxFileTitle;
    LPCSTR lpstrInitialDir, lpstrTitle; DWORD Flags; WORD nFileOffset, nFileExtension; LPCSTR lpstrDefExt;
    LPARAM lCustData; OFNHOOKPROC_ lpfnHook; LPCSTR lpTemplateName; void *pvReserved; DWORD dwReserved, FlagsEx;
} OPENFILENAMEA_;

#define OFN_READONLY_          0x00000001
#define OFN_OVERWRITEPROMPT_   0x00000002
#define OFN_NOCHANGEDIR_       0x00000008
#define OFN_NOVALIDATE_        0x00000100
#define OFN_ALLOWMULTISELECT_  0x00000200
#define OFN_PATHMUSTEXIST_     0x00000800
#define OFN_FILEMUSTEXIST_     0x00001000
#define OFN_CREATEPROMPT_      0x00002000
#define OFN_EXPLORER_          0x00080000
#define OFN_FORCESHOWHIDDEN_   0x10000000
#define CDERR_INITIALIZATION_  0x0002
#define CDERR_STRUCTSIZE_      0x0001
#define FNERR_BUFFERTOOSMALL_  0x3003

CDAPI DWORD WINAPI CommDlgExtendedError(void) { return g_error; }

static BOOL file_dialog(OPENFILENAMEW_ *o, BOOL save)
{
    g_error = 0;
    if (!o || o->lStructSize < 76 /* the oldest OPENFILENAME */) { g_error = CDERR_STRUCTSIZE_; return FALSE; }
    if (!o->lpstrFile || !o->nMaxFile) { g_error = CDERR_INITIALIZATION_; return FALSE; }

    FileDlg *d = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *d);
    FdFilter *f = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(FdFilter) * 64);
    if (!d || !f) { HeapFree(GetProcessHeap(), 0, d); HeapFree(GetProcessHeap(), 0, f); g_error = CDERR_INITIALIZATION_; return FALSE; }

    d->owner = o->hwndOwner;
    d->save = save;
    DWORD fl = o->Flags;
    d->fos = (fl & (OFN_OVERWRITEPROMPT_ | OFN_NOCHANGEDIR_ | OFN_NOVALIDATE_ | OFN_ALLOWMULTISELECT_ |
                    OFN_PATHMUSTEXIST_ | OFN_FILEMUSTEXIST_ | OFN_CREATEPROMPT_ | OFN_FORCESHOWHIDDEN_));
    if (o->lpstrTitle) wcopy(d->title, o->lpstrTitle, 256);
    if (o->lpstrDefExt) wcopy(d->defext, o->lpstrDefExt, 32);

    /* "Text\0*.txt\0All\0*.*\0\0" */
    for (const WCHAR *p = o->lpstrFilter; p && *p && d->nfilters < 64;) {
        const WCHAR *spec = p + wlen(p) + 1;
        if (!*spec) break;
        f[d->nfilters].name = (WCHAR *)p;
        f[d->nfilters].spec = (WCHAR *)spec;
        d->nfilters++;
        p = spec + wlen(spec) + 1;
    }
    d->filters = f;
    d->filter = o->nFilterIndex ? (int)o->nFilterIndex - 1 : 0;

    /* the file box: a name, or a path whose folder is where to start */
    WCHAR init[MAX_PATH];
    wcopy(init, o->lpstrFile, MAX_PATH);
    if (init[0] && !has_wild(init)) {
        const WCHAR *l = leaf(init);
        if (l != init) {
            int k = (int)(l - init);
            wcopy(d->dir, init, k + 1);
            strip_slash(d->dir);
            if (k == 3) d->dir[3] = 0;
        }
        if (is_dir(init)) { wcopy(d->dir, init, MAX_PATH); d->name[0] = 0; }
        else wcopy(d->name, l, MAX_PATH);
    } else if (init[0]) {
        wcopy(d->name, init, MAX_PATH);
    }
    if (!d->dir[0] && o->lpstrInitialDir) wcopy(d->dir, o->lpstrInitialDir, MAX_PATH);

    BOOL ok = fd_run(d);
    if (ok) {
        BOOL multi = d->res_count > 1;
        int dirlen = wlen(d->res_dir);
        if (!multi) {
            WCHAR full[MAX_PATH];
            fd_result(d, 0, full);
            int n = wlen(full);
            if ((DWORD)n + 1 > o->nMaxFile) {
                g_error = FNERR_BUFFERTOOSMALL_;
                if (o->nMaxFile >= 2) *(WORD *)o->lpstrFile = (WORD)(n + 1);
                ok = FALSE;
            } else {
                wcopy(o->lpstrFile, full, n + 1);
                const WCHAR *l = leaf(full);
                o->nFileOffset = (WORD)(l - full);
                const WCHAR *dot = 0;
                for (const WCHAR *s = l; *s; s++) if (*s == '.') dot = s;
                o->nFileExtension = (WORD)(dot ? dot + 1 - full : 0);
                if (o->lpstrFileTitle && o->nMaxFileTitle) wcopy(o->lpstrFileTitle, l, (int)o->nMaxFileTitle);
            }
        } else {
            /* the folder, then each name: NUL-separated (Explorer style) or space-separated (old style) */
            WCHAR sep = (fl & OFN_EXPLORER_) ? 0 : ' ';
            int need = dirlen + 1;
            const WCHAR *s = d->res_names;
            for (int i = 0; i < d->res_count; i++) { need += wlen(s) + 1; s += wlen(s) + 1; }
            need++;
            if ((DWORD)need > o->nMaxFile) {
                g_error = FNERR_BUFFERTOOSMALL_;
                if (o->nMaxFile >= 2) *(WORD *)o->lpstrFile = (WORD)need;
                ok = FALSE;
            } else {
                int at = 0;
                for (int i = 0; i < dirlen; i++) o->lpstrFile[at++] = d->res_dir[i];
                o->lpstrFile[at++] = sep;
                o->nFileOffset = (WORD)at;
                s = d->res_names;
                for (int i = 0; i < d->res_count; i++) {
                    int k = wlen(s);
                    for (int j = 0; j < k; j++) o->lpstrFile[at++] = s[j];
                    o->lpstrFile[at++] = (i + 1 < d->res_count) ? sep : 0;
                    s += k + 1;
                }
                o->lpstrFile[at] = 0;
                o->nFileExtension = 0;
            }
        }
        if (ok) {
            o->nFilterIndex = d->nfilters ? (DWORD)d->filter + 1 : o->nFilterIndex;
            o->Flags &= ~OFN_READONLY_;
            if (!(fl & OFN_NOCHANGEDIR_) && d->res_dir[0]) SetCurrentDirectoryW(d->res_dir);
        }
    }
    HeapFree(GetProcessHeap(), 0, f);
    HeapFree(GetProcessHeap(), 0, d);
    return ok;
}

CDAPI BOOL WINAPI GetOpenFileNameW(OPENFILENAMEW_ *o) { return file_dialog(o, FALSE); }
CDAPI BOOL WINAPI GetSaveFileNameW(OPENFILENAMEW_ *o) { return file_dialog(o, TRUE); }

/* a NUL-separated list ending in an empty string, ANSI to UTF-16 */
static WCHAR *multi_a2w(const char *s)
{
    if (!s) return 0;
    int n = 0;
    while (s[n] || s[n + 1]) n++;
    n += 2;
    int wn = MultiByteToWideChar(CP_ACP, 0, s, n, 0, 0);
    WCHAR *w = HeapAlloc(GetProcessHeap(), 0, sizeof(WCHAR) * (wn + 1));
    if (w) MultiByteToWideChar(CP_ACP, 0, s, n, w, wn);
    return w;
}

static WCHAR *a2w(const char *s)
{
    if (!s) return 0;
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, 0, 0);
    WCHAR *w = HeapAlloc(GetProcessHeap(), 0, sizeof(WCHAR) * n);
    if (w) MultiByteToWideChar(CP_ACP, 0, s, -1, w, n);
    return w;
}

static BOOL file_dialog_a(OPENFILENAMEA_ *o, BOOL save)
{
    g_error = 0;
    if (!o || o->lStructSize < 76) { g_error = CDERR_STRUCTSIZE_; return FALSE; }
    if (!o->lpstrFile || !o->nMaxFile) { g_error = CDERR_INITIALIZATION_; return FALSE; }
    OPENFILENAMEW_ w = { 0 };
    w.lStructSize = sizeof w;
    w.hwndOwner = o->hwndOwner;
    w.hInstance = o->hInstance;
    w.lpstrFilter = multi_a2w(o->lpstrFilter);
    w.nFilterIndex = o->nFilterIndex;
    DWORD cap = o->nMaxFile < 32768 ? 32768 : o->nMaxFile;
    w.lpstrFile = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(WCHAR) * cap);
    w.nMaxFile = cap;
    if (w.lpstrFile) MultiByteToWideChar(CP_ACP, 0, o->lpstrFile, -1, w.lpstrFile, (int)cap);
    WCHAR title[MAX_PATH];
    if (o->lpstrFileTitle) { w.lpstrFileTitle = title; w.nMaxFileTitle = MAX_PATH; }
    w.lpstrInitialDir = a2w(o->lpstrInitialDir);
    w.lpstrTitle = a2w(o->lpstrTitle);
    w.Flags = o->Flags;
    w.lpstrDefExt = a2w(o->lpstrDefExt);

    BOOL ok = w.lpstrFile && file_dialog(&w, save);
    if (ok) {
        /* the result: one path or a NUL-separated list */
        int n = 0;
        if (w.nFileExtension == 0 && w.nFileOffset && !w.lpstrFile[w.nFileOffset - 1]) {
            while (w.lpstrFile[n] || w.lpstrFile[n + 1]) n++;
            n += 2;
        } else {
            n = wlen(w.lpstrFile) + 1;
        }
        int an = WideCharToMultiByte(CP_ACP, 0, w.lpstrFile, n, 0, 0, 0, 0);
        if ((DWORD)an > o->nMaxFile) {
            g_error = FNERR_BUFFERTOOSMALL_;
            if (o->nMaxFile >= 2) *(WORD *)o->lpstrFile = (WORD)an;
            ok = FALSE;
        } else {
            WideCharToMultiByte(CP_ACP, 0, w.lpstrFile, n, o->lpstrFile, (int)o->nMaxFile, 0, 0);
            o->nFileOffset = (WORD)WideCharToMultiByte(CP_ACP, 0, w.lpstrFile, w.nFileOffset, 0, 0, 0, 0);
            o->nFileExtension = w.nFileExtension ? (WORD)WideCharToMultiByte(CP_ACP, 0, w.lpstrFile, w.nFileExtension, 0, 0, 0, 0) : 0;
            if (o->lpstrFileTitle && o->nMaxFileTitle)
                WideCharToMultiByte(CP_ACP, 0, title, -1, o->lpstrFileTitle, (int)o->nMaxFileTitle, 0, 0);
            o->nFilterIndex = w.nFilterIndex;
            o->Flags = w.Flags;
        }
    } else if (g_error == FNERR_BUFFERTOOSMALL_ && w.lpstrFile && o->nMaxFile >= 2) {
        *(WORD *)o->lpstrFile = *(WORD *)w.lpstrFile;
    }
    HeapFree(GetProcessHeap(), 0, (void *)w.lpstrFilter);
    HeapFree(GetProcessHeap(), 0, w.lpstrFile);
    HeapFree(GetProcessHeap(), 0, (void *)w.lpstrInitialDir);
    HeapFree(GetProcessHeap(), 0, (void *)w.lpstrTitle);
    HeapFree(GetProcessHeap(), 0, (void *)w.lpstrDefExt);
    return ok;
}

CDAPI BOOL WINAPI GetOpenFileNameA(OPENFILENAMEA_ *o) { return file_dialog_a(o, FALSE); }
CDAPI BOOL WINAPI GetSaveFileNameA(OPENFILENAMEA_ *o) { return file_dialog_a(o, TRUE); }
