/*
 * comctl32.dll — common controls.  NovaOS's window system draws its own
 * controls, so the control classes aren't registered here; what programs
 * call around them works: initialization, image lists (bookkeeping),
 * dynamic arrays (DPA/DSA), task dialogs (shown as message boxes) and
 * mouse tracking.
 */

#include <windows.h>

#define CCAPI __declspec(dllexport)
#define S_OK_ ((HRESULT)0)
#define E_NOTIMPL_ ((HRESULT)0x80004001L)
#define E_INVALIDARG_ ((HRESULT)0x80070057L)

CCAPI void WINAPI InitCommonControls(void) { }
CCAPI BOOL WINAPI InitCommonControlsEx(const void *icc) { (void)icc; return TRUE; }
CCAPI BOOL WINAPI _TrackMouseEvent(LPTRACKMOUSEEVENT tme) { return TrackMouseEvent(tme); }
CCAPI HRESULT WINAPI LoadIconMetric(HINSTANCE h, LPCWSTR name, int metric, HICON *out) { (void)h; (void)name; (void)metric; *out = 0; return E_NOTIMPL_; }
CCAPI HRESULT WINAPI LoadIconWithScaleDown(HINSTANCE h, LPCWSTR name, int cx, int cy, HICON *out) { (void)h; (void)name; (void)cx; (void)cy; *out = 0; return E_NOTIMPL_; }
CCAPI BOOL WINAPI SetWindowSubclass(HWND h, void *fn, UINT_PTR id, DWORD_PTR data) { (void)h; (void)fn; (void)id; (void)data; return FALSE; }
CCAPI BOOL WINAPI RemoveWindowSubclass(HWND h, void *fn, UINT_PTR id) { (void)h; (void)fn; (void)id; return TRUE; }
CCAPI LRESULT WINAPI DefSubclassProc(HWND h, UINT msg, WPARAM w, LPARAM l) { return DefWindowProcA(h, msg, w, l); }
CCAPI HANDLE WINAPI CreatePropertySheetPageW(const void *p) { (void)p; return 0; }
CCAPI INT_PTR WINAPI PropertySheetW(const void *p) { (void)p; return -1; }
CCAPI HWND WINAPI CreateStatusWindowW(LONG style, LPCWSTR text, HWND parent, UINT id) { (void)style; (void)text; (void)parent; (void)id; return 0; }

/* ---- task dialogs: the message and buttons in a message box ---- */
typedef struct {
    UINT cbSize; HWND hwndParent; HINSTANCE hInstance; DWORD dwFlags, dwCommonButtons;
    LPCWSTR pszWindowTitle; LPCWSTR pszMainIcon; LPCWSTR pszMainInstruction, pszContent;
} TASKDIALOGCONFIG_HEAD;

static int show(HWND owner, LPCWSTR title, LPCWSTR main, LPCWSTR content, DWORD buttons)
{
    char t[256] = "", body[2048] = "";
    if (title && (ULONG_PTR)title > 0xFFFF) WideCharToMultiByte(CP_UTF8, 0, title, -1, t, sizeof(t), 0, 0);
    int o = 0;
    if (main && (ULONG_PTR)main > 0xFFFF) o = WideCharToMultiByte(CP_UTF8, 0, main, -1, body, sizeof(body) - 4, 0, 0);
    if (o > 0) { body[o - 1] = '\n'; body[o++] = '\n'; body[o] = 0; } else o = 0;
    if (content && (ULONG_PTR)content > 0xFFFF) WideCharToMultiByte(CP_UTF8, 0, content, -1, body + o, (int)sizeof(body) - o, 0, 0);
    UINT type = (buttons & 6) == 6 ? 3 /* MB_YESNOCANCEL */ : (buttons & 6) ? 4 /* MB_YESNO */ : (buttons & 8) ? 1 /* MB_OKCANCEL */ : 0;
    int r = MessageBoxA(owner, body, t, type);
    return r ? r : 2;
}

CCAPI HRESULT WINAPI TaskDialog(HWND owner, HINSTANCE inst, LPCWSTR title, LPCWSTR main, LPCWSTR content, DWORD buttons,
                                LPCWSTR icon, int *pressed)
{
    (void)inst; (void)icon;
    int r = show(owner, title, main, content, buttons);
    if (pressed) *pressed = r;
    return S_OK_;
}

CCAPI HRESULT WINAPI TaskDialogIndirect(const TASKDIALOGCONFIG_HEAD *c, int *button, int *radio, BOOL *verify)
{
    if (!c) return E_INVALIDARG_;
    int r = show(c->hwndParent, c->pszWindowTitle, c->pszMainInstruction, c->pszContent, c->dwCommonButtons);
    if (button) *button = r;
    if (radio) *radio = 0;
    if (verify) *verify = FALSE;
    return S_OK_;
}

/* ---- image lists: sizes and counts (images are not drawn) ---- */
typedef struct { DWORD magic; int cx, cy, count, grow; } ImageList;
#define IL_MAGIC 0x494D4C21u
static ImageList *il(HANDLE h) { ImageList *l = h; return l && l->magic == IL_MAGIC ? l : 0; }

CCAPI HANDLE WINAPI ImageList_Create(int cx, int cy, UINT flags, int initial, int grow)
{
    (void)flags; (void)initial;
    ImageList *l = LocalAlloc(LMEM_ZEROINIT, sizeof(*l));
    if (l) { l->magic = IL_MAGIC; l->cx = cx; l->cy = cy; l->grow = grow; }
    return l;
}
CCAPI BOOL WINAPI ImageList_Destroy(HANDLE h) { ImageList *l = il(h); if (!l) return FALSE; l->magic = 0; LocalFree(l); return TRUE; }
CCAPI int WINAPI ImageList_Add(HANDLE h, HBITMAP img, HBITMAP mask) { (void)img; (void)mask; ImageList *l = il(h); return l ? l->count++ : -1; }
CCAPI int WINAPI ImageList_AddMasked(HANDLE h, HBITMAP img, COLORREF mask) { (void)img; (void)mask; ImageList *l = il(h); return l ? l->count++ : -1; }
CCAPI int WINAPI ImageList_ReplaceIcon(HANDLE h, int i, HICON icon) { (void)icon; ImageList *l = il(h); if (!l) return -1; return i < 0 ? l->count++ : i; }
CCAPI int WINAPI ImageList_GetImageCount(HANDLE h) { ImageList *l = il(h); return l ? l->count : 0; }
CCAPI BOOL WINAPI ImageList_SetImageCount(HANDLE h, UINT n) { ImageList *l = il(h); if (!l) return FALSE; l->count = (int)n; return TRUE; }
CCAPI BOOL WINAPI ImageList_Remove(HANDLE h, int i) { ImageList *l = il(h); if (!l) return FALSE; if (i < 0) l->count = 0; else if (i < l->count) l->count--; return TRUE; }
CCAPI BOOL WINAPI ImageList_GetIconSize(HANDLE h, int *cx, int *cy) { ImageList *l = il(h); if (!l) return FALSE; *cx = l->cx; *cy = l->cy; return TRUE; }
CCAPI BOOL WINAPI ImageList_SetIconSize(HANDLE h, int cx, int cy) { ImageList *l = il(h); if (!l) return FALSE; l->cx = cx; l->cy = cy; l->count = 0; return TRUE; }
CCAPI BOOL WINAPI ImageList_Draw(HANDLE h, int i, HDC dc, int x, int y, UINT style) { (void)i; (void)dc; (void)x; (void)y; (void)style; return il(h) != 0; }
CCAPI COLORREF WINAPI ImageList_SetBkColor(HANDLE h, COLORREF c) { (void)h; (void)c; return 0xFFFFFFFF; }
CCAPI HICON WINAPI ImageList_GetIcon(HANDLE h, int i, UINT flags) { (void)h; (void)i; (void)flags; return 0; }

/* ---- DSA: a growable array of fixed-size items ---- */
typedef struct { int count, cap, size, grow; BYTE *items; } DSA;

CCAPI DSA *WINAPI DSA_Create(int size, int grow)
{
    DSA *d = LocalAlloc(LMEM_ZEROINIT, sizeof(*d));
    if (d) { d->size = size; d->grow = grow > 0 ? grow : 8; }
    return d;
}

CCAPI int WINAPI DSA_InsertItem(DSA *d, int i, const void *item)
{
    if (!d) return -1;
    if (i < 0 || i > d->count) i = d->count;
    if (d->count == d->cap) {
        int cap = d->cap + d->grow;
        BYTE *n = d->items ? HeapReAlloc(GetProcessHeap(), 0, d->items, (SIZE_T)cap * d->size) : HeapAlloc(GetProcessHeap(), 0, (SIZE_T)cap * d->size);
        if (!n) return -1;
        d->items = n; d->cap = cap;
    }
    BYTE *at = d->items + (SIZE_T)i * d->size;
    for (int k = (d->count - i) * d->size - 1; k >= 0; k--) at[d->size + k] = at[k];
    for (int k = 0; k < d->size; k++) at[k] = ((const BYTE *)item)[k];
    d->count++;
    return i;
}

CCAPI void *WINAPI DSA_GetItemPtr(DSA *d, int i) { return d && i >= 0 && i < d->count ? d->items + (SIZE_T)i * d->size : 0; }
CCAPI BOOL WINAPI DSA_GetItem(DSA *d, int i, void *out)
{
    BYTE *p = DSA_GetItemPtr(d, i);
    if (!p) return FALSE;
    for (int k = 0; k < d->size; k++) ((BYTE *)out)[k] = p[k];
    return TRUE;
}
CCAPI BOOL WINAPI DSA_SetItem(DSA *d, int i, const void *item)
{
    if (!d || i < 0) return FALSE;
    while (i >= d->count) if (DSA_InsertItem(d, d->count, item) < 0) return FALSE;
    BYTE *p = d->items + (SIZE_T)i * d->size;
    for (int k = 0; k < d->size; k++) p[k] = ((const BYTE *)item)[k];
    return TRUE;
}
CCAPI BOOL WINAPI DSA_DeleteItem(DSA *d, int i)
{
    if (!d || i < 0 || i >= d->count) return FALSE;
    BYTE *at = d->items + (SIZE_T)i * d->size;
    for (int k = 0; k < (d->count - i - 1) * d->size; k++) at[k] = at[d->size + k];
    d->count--;
    return TRUE;
}
CCAPI BOOL WINAPI DSA_DeleteAllItems(DSA *d) { if (!d) return FALSE; d->count = 0; return TRUE; }
CCAPI BOOL WINAPI DSA_Destroy(DSA *d) { if (!d) return TRUE; if (d->items) HeapFree(GetProcessHeap(), 0, d->items); LocalFree(d); return TRUE; }

/* ---- DPA: a growable array of pointers ---- */
CCAPI DSA *WINAPI DPA_Create(int grow) { return DSA_Create(sizeof(void *), grow); }
CCAPI int WINAPI DPA_InsertPtr(DSA *d, int i, void *p) { return DSA_InsertItem(d, i, &p); }
CCAPI void *WINAPI DPA_GetPtr(DSA *d, INT_PTR i) { void **p = DSA_GetItemPtr(d, (int)i); return p ? *p : 0; }
CCAPI BOOL WINAPI DPA_SetPtr(DSA *d, int i, void *p) { return DSA_SetItem(d, i, &p); }
CCAPI void *WINAPI DPA_DeletePtr(DSA *d, int i) { void *p = DPA_GetPtr(d, i); return DSA_DeleteItem(d, i) ? p : 0; }
CCAPI BOOL WINAPI DPA_DeleteAllPtrs(DSA *d) { return DSA_DeleteAllItems(d); }
CCAPI BOOL WINAPI DPA_Destroy(DSA *d) { return DSA_Destroy(d); }
CCAPI int WINAPI DPA_GetPtrIndex(DSA *d, const void *p)
{
    for (int i = 0; d && i < d->count; i++) if (((void **)d->items)[i] == p) return i;
    return -1;
}
typedef int (CALLBACK *PFNDACOMPARE)(void *a, void *b, LPARAM l);
CCAPI BOOL WINAPI DPA_Sort(DSA *d, PFNDACOMPARE cmp, LPARAM l)
{
    if (!d) return FALSE;
    void **v = (void **)d->items;
    for (int i = 1; i < d->count; i++) {                   /* insertion sort: stable, lists are small */
        void *x = v[i];
        int j = i - 1;
        while (j >= 0 && cmp(v[j], x, l) > 0) { v[j + 1] = v[j]; j--; }
        v[j + 1] = x;
    }
    return TRUE;
}
typedef int (CALLBACK *PFNDAENUMCALLBACK)(void *p, void *data);
CCAPI void WINAPI DPA_EnumCallback(DSA *d, PFNDAENUMCALLBACK fn, void *data)
{
    for (int i = 0; d && i < d->count; i++) if (!fn(((void **)d->items)[i], data)) break;
}
CCAPI void WINAPI DPA_DestroyCallback(DSA *d, PFNDAENUMCALLBACK fn, void *data) { DPA_EnumCallback(d, fn, data); DPA_Destroy(d); }
