/* droptest.exe — drag and drop between windows.
 *   droptest          a window that takes files (DragAcceptFiles, WM_DROPFILES)
 *   droptest ole      a window with an OLE drop target (RegisterDragDrop)
 *   droptest source   a window: press the mouse in it and drag out to drop
 *                     C:\Documents\Welcome.txt on another window (DoDragDrop)
 * What happens is listed in the window and printed to the kernel log. */
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static HWND g_list;
static int g_mode;                          /* 0 files, 1 ole, 2 source */

static void logf(const char *fmt, ...)
{
    char b[512];
    va_list a;
    va_start(a, fmt);
    wvsprintfA(b, fmt, a);
    va_end(a);
    if (g_list) SendMessageA(g_list, LB_ADDSTRING, 0, (LPARAM)b);
    char d[560];
    wsprintfA(d, "droptest: %s\n", b);
    OutputDebugStringA(d);
}

/* ---- a data object with one CF_HDROP ---- */
typedef struct { IDataObject iface; LONG refs; HGLOBAL hdrop; } Data;

static HRESULT STDMETHODCALLTYPE d_qi(IDataObject *t, REFIID iid, void **out)
{
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IDataObject)) { *out = t; t->lpVtbl->AddRef(t); return S_OK; }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE d_addref(IDataObject *t) { return ++((Data *)t)->refs; }
static ULONG STDMETHODCALLTYPE d_release(IDataObject *t) { Data *d = (Data *)t; if (--d->refs) return d->refs; GlobalFree(d->hdrop); free(d); return 0; }
static HRESULT STDMETHODCALLTYPE d_getdata(IDataObject *t, FORMATETC *f, STGMEDIUM *m)
{
    Data *d = (Data *)t;
    if (f->cfFormat != CF_HDROP || !(f->tymed & TYMED_HGLOBAL)) return DV_E_FORMATETC;
    SIZE_T n = GlobalSize(d->hdrop);
    m->tymed = TYMED_HGLOBAL;
    m->hGlobal = GlobalAlloc(GMEM_MOVEABLE, n);
    memcpy(GlobalLock(m->hGlobal), GlobalLock(d->hdrop), n);
    GlobalUnlock(m->hGlobal); GlobalUnlock(d->hdrop);
    m->pUnkForRelease = NULL;
    logf("source: GetData(CF_HDROP)");
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE d_getdatahere(IDataObject *t, FORMATETC *f, STGMEDIUM *m) { (void)t; (void)f; (void)m; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE d_query(IDataObject *t, FORMATETC *f) { (void)t; return f->cfFormat == CF_HDROP ? S_OK : DV_E_FORMATETC; }
static HRESULT STDMETHODCALLTYPE d_canon(IDataObject *t, FORMATETC *i, FORMATETC *o) { (void)t; *o = *i; return DATA_S_SAMEFORMATETC; }
static HRESULT STDMETHODCALLTYPE d_setdata(IDataObject *t, FORMATETC *f, STGMEDIUM *m, BOOL r) { (void)t; (void)f; (void)m; (void)r; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE d_enum(IDataObject *t, DWORD dir, IEnumFORMATETC **o) { (void)t; (void)dir; *o = NULL; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE d_dadvise(IDataObject *t, FORMATETC *f, DWORD a, IAdviseSink *s, DWORD *c) { (void)t; (void)f; (void)a; (void)s; (void)c; return OLE_E_ADVISENOTSUPPORTED; }
static HRESULT STDMETHODCALLTYPE d_dunadvise(IDataObject *t, DWORD c) { (void)t; (void)c; return OLE_E_ADVISENOTSUPPORTED; }
static HRESULT STDMETHODCALLTYPE d_enumadvise(IDataObject *t, IEnumSTATDATA **o) { (void)t; *o = NULL; return OLE_E_ADVISENOTSUPPORTED; }
static const IDataObjectVtbl g_data_vtbl = { d_qi, d_addref, d_release, d_getdata, d_getdatahere, d_query, d_canon, d_setdata, d_enum, d_dadvise, d_dunadvise, d_enumadvise };

static IDataObject *make_data(const WCHAR *path)
{
    Data *d = calloc(1, sizeof *d);
    d->iface.lpVtbl = &g_data_vtbl;
    d->refs = 1;
    size_t n = (wcslen(path) + 2) * 2;
    d->hdrop = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + n);
    DROPFILES *df = GlobalLock(d->hdrop);
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    memcpy(df + 1, path, (wcslen(path) + 1) * 2);
    GlobalUnlock(d->hdrop);
    return &d->iface;
}

/* ---- the drop source ---- */
typedef struct { IDropSource iface; LONG refs; } Source;
static HRESULT STDMETHODCALLTYPE s_qi(IDropSource *t, REFIID iid, void **out)
{
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IDropSource)) { *out = t; return S_OK; }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE s_addref(IDropSource *t) { return ++((Source *)t)->refs; }
static ULONG STDMETHODCALLTYPE s_release(IDropSource *t) { return --((Source *)t)->refs; }
static HRESULT STDMETHODCALLTYPE s_query(IDropSource *t, BOOL esc, DWORD keys)
{
    (void)t;
    if (esc) return DRAGDROP_S_CANCEL;
    if (!(keys & MK_LBUTTON)) return DRAGDROP_S_DROP;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE s_feedback(IDropSource *t, DWORD eff) { (void)t; (void)eff; return DRAGDROP_S_USEDEFAULTCURSORS; }
static const IDropSourceVtbl g_src_vtbl = { s_qi, s_addref, s_release, s_query, s_feedback };

/* ---- the drop target ---- */
typedef struct { IDropTarget iface; LONG refs; } Target;
static HRESULT STDMETHODCALLTYPE t_qi(IDropTarget *t, REFIID iid, void **out)
{
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IDropTarget)) { *out = t; return S_OK; }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE t_addref(IDropTarget *t) { return ++((Target *)t)->refs; }
static ULONG STDMETHODCALLTYPE t_release(IDropTarget *t) { return --((Target *)t)->refs; }
static HRESULT STDMETHODCALLTYPE t_enter(IDropTarget *t, IDataObject *d, DWORD keys, POINTL pt, DWORD *eff)
{
    (void)t; (void)keys;
    FORMATETC f = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    BOOL ok = d->lpVtbl->QueryGetData(d, &f) == S_OK;
    logf("target: DragEnter at %d,%d %s", pt.x, pt.y, ok ? "(has files)" : "(no files)");
    *eff = ok ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE t_over(IDropTarget *t, DWORD keys, POINTL pt, DWORD *eff) { (void)t; (void)keys; (void)pt; *eff = DROPEFFECT_COPY; return S_OK; }
static HRESULT STDMETHODCALLTYPE t_leave(IDropTarget *t) { (void)t; logf("target: DragLeave"); return S_OK; }
static HRESULT STDMETHODCALLTYPE t_drop(IDropTarget *t, IDataObject *d, DWORD keys, POINTL pt, DWORD *eff)
{
    (void)t; (void)keys;
    FORMATETC f = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM m;
    if (SUCCEEDED(d->lpVtbl->GetData(d, &f, &m))) {
        HDROP h = m.hGlobal;
        UINT n = DragQueryFileW(h, 0xFFFFFFFF, NULL, 0);
        logf("target: Drop at %d,%d: %u file(s)", pt.x, pt.y, n);
        for (UINT i = 0; i < n; i++) { char p[MAX_PATH]; DragQueryFileA(h, i, p, MAX_PATH); logf("  %s", p); }
        ReleaseStgMedium(&m);
    } else logf("target: Drop without CF_HDROP");
    *eff = DROPEFFECT_COPY;
    return S_OK;
}
static const IDropTargetVtbl g_tgt_vtbl = { t_qi, t_addref, t_release, t_enter, t_over, t_leave, t_drop };
static Target g_target = { { &g_tgt_vtbl }, 1 };

static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_list = CreateWindowExW(WS_EX_CLIENTEDGE, L"ListBox", NULL, WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT, 10, 80, 380, 160, h, (HMENU)1, NULL, NULL);
        SendMessageW(g_list, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), 0);
        CreateWindowExW(0, L"Static", g_mode == 2 ? L"Press the mouse here and drag to another window" : g_mode == 1 ? L"Drop files here (OLE target)" : L"Drop files here (WM_DROPFILES)",
                        WS_CHILD | WS_VISIBLE, 10, 12, 380, 20, h, (HMENU)2, NULL, NULL);
        if (g_mode == 0) DragAcceptFiles(h, TRUE);
        if (g_mode == 1) logf("RegisterDragDrop: %08x", (unsigned)RegisterDragDrop(h, &g_target.iface));
        return 0;
    case WM_SIZE:
        if (g_list) MoveWindow(g_list, 10, 80, LOWORD(lp) - 20, HIWORD(lp) - 90, TRUE);
        return 0;
    case WM_DROPFILES: {
        HDROP d = (HDROP)wp;
        UINT n = DragQueryFileA(d, 0xFFFFFFFF, NULL, 0);
        POINT pt;
        DragQueryPoint(d, &pt);
        logf("WM_DROPFILES at %d,%d: %u file(s)", pt.x, pt.y, n);
        for (UINT i = 0; i < n; i++) { char p[MAX_PATH]; DragQueryFileA(d, i, p, MAX_PATH); logf("  %s", p); }
        DragFinish(d);
        return 0;
    }
    case WM_LBUTTONDOWN:
        if (g_mode == 2) {
            Source src = { { &g_src_vtbl }, 1 };
            IDataObject *data = make_data(L"C:\\Documents\\Welcome.txt");
            DWORD eff = 0;
            logf("source: DoDragDrop...");
            HRESULT r = DoDragDrop(data, &src.iface, DROPEFFECT_COPY | DROPEFFECT_MOVE, &eff);
            logf("source: DoDragDrop returned %08x, effect %lu", (unsigned)r, eff);
            data->lpVtbl->Release(data);
        }
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "ole")) g_mode = 1;
    if (argc > 1 && !strcmp(argv[1], "source")) g_mode = 2;
    OleInitialize(NULL);
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"DropTest";
    RegisterClassExW(&wc);
    static const WCHAR *const titles[] = { L"Drop target (files)", L"Drop target (OLE)", L"Drag source" };
    int x = g_mode == 2 ? 60 : 700, y = g_mode == 2 ? 120 : 120 + (g_mode == 1 ? 300 : 0);
    HWND h = CreateWindowExW(0, L"DropTest", titles[g_mode], WS_OVERLAPPEDWINDOW, x, y, 420, 280, NULL, NULL, wc.hInstance, NULL);
    ShowWindow(h, SW_SHOW);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    OleUninitialize();
    return 0;
}
