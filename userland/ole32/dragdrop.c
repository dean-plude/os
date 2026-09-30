/*
 * dragdrop.c — OLE drag and drop.
 *
 * DoDragDrop runs the drag: it captures the mouse and, for every move,
 * finds the window under the pointer. A window of this process with a
 * registered IDropTarget gets DragEnter/DragOver/Drop with the source's
 * data object. A window of another program gets the drop through the
 * desktop (user32's NovaSendDrop) as a file list, taken from the data
 * object's CF_HDROP; on that side user32 hands it to our hook, which
 * wraps it in a data object for the window's IDropTarget, or to
 * WM_DROPFILES for a plain window.
 */
#define NOVA_BUILD_OLE32
#include <windows.h>
#include <objbase.h>

#define DROP_PROP L"OleDropTargetInterface"

typedef BOOL (WINAPI *DropHook)(HWND hwnd, POINT screen, DWORD effect, const WCHAR *files, DWORD bytes);
__declspec(dllimport) void NovaSetDropHook(void *fn);
__declspec(dllimport) BOOL NovaAcceptDrops(HWND h, DWORD mask, BOOL on);
__declspec(dllimport) UINT32 NovaWindowAt(POINT pt, DWORD *pid, DWORD *flags);
__declspec(dllimport) HWND NovaTopFromKid(UINT32 kid);
__declspec(dllimport) BOOL NovaSendDrop(UINT32 kid, POINT screen, DWORD effect, const WCHAR *files, DWORD bytes);

static BOOL WINAPI drop_hook(HWND hwnd, POINT screen, DWORD effect, const WCHAR *files, DWORD bytes);

/* the target registered on the window or an ancestor */
static IDropTarget *target_of(HWND h, HWND *owner)
{
    for (; h; h = GetParent(h)) {
        IDropTarget *t = (IDropTarget *)GetPropW(h, DROP_PROP);
        if (t) { if (owner) *owner = h; return t; }
    }
    return NULL;
}

WINOLEAPI_(HRESULT) RegisterDragDrop(HWND w, IDropTarget *target)
{
    if (!IsWindow(w)) return DRAGDROP_E_INVALIDHWND;
    if (!target) return E_INVALIDARG;
    if (GetPropW(w, DROP_PROP)) return DRAGDROP_E_ALREADYREGISTERED;
    target->lpVtbl->AddRef(target);
    SetPropW(w, DROP_PROP, target);
    NovaAcceptDrops(w, 2, TRUE);
    NovaSetDropHook((void *)drop_hook);
    return S_OK;
}

WINOLEAPI_(HRESULT) RevokeDragDrop(HWND w)
{
    if (!IsWindow(w)) return DRAGDROP_E_INVALIDHWND;
    IDropTarget *t = (IDropTarget *)RemovePropW(w, DROP_PROP);
    if (!t) return DRAGDROP_E_NOTREGISTERED;
    NovaAcceptDrops(w, 2, FALSE);
    t->lpVtbl->Release(t);
    return S_OK;
}

/* -----------------------------------------------------------------------
 * A data object holding one CF_HDROP (drops from other programs)
 * ----------------------------------------------------------------------- */
typedef struct { IDataObject iface; LONG refs; HGLOBAL hdrop; } FileData;
typedef struct { IEnumFORMATETC iface; LONG refs; int pos; } FileEnum;

static HRESULT STDMETHODCALLTYPE fe_qi(IEnumFORMATETC *This, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IEnumFORMATETC)) { *out = This; This->lpVtbl->AddRef(This); return S_OK; }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE fe_addref(IEnumFORMATETC *This) { return (ULONG)InterlockedIncrement(&((FileEnum *)This)->refs); }
static ULONG STDMETHODCALLTYPE fe_release(IEnumFORMATETC *This)
{
    LONG r = InterlockedDecrement(&((FileEnum *)This)->refs);
    if (!r) HeapFree(GetProcessHeap(), 0, This);
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE fe_next(IEnumFORMATETC *This, ULONG n, FORMATETC *out, ULONG *fetched)
{
    FileEnum *e = (FileEnum *)This;
    ULONG k = 0;
    if (n && e->pos == 0 && out) {
        out[0].cfFormat = CF_HDROP; out[0].ptd = NULL; out[0].dwAspect = DVASPECT_CONTENT; out[0].lindex = -1; out[0].tymed = TYMED_HGLOBAL;
        e->pos = 1; k = 1;
    }
    if (fetched) *fetched = k;
    return k == n ? S_OK : S_FALSE;
}
static HRESULT STDMETHODCALLTYPE fe_skip(IEnumFORMATETC *This, ULONG n) { FileEnum *e = (FileEnum *)This; e->pos += (int)n; return e->pos <= 1 ? S_OK : S_FALSE; }
static HRESULT STDMETHODCALLTYPE fe_reset(IEnumFORMATETC *This) { ((FileEnum *)This)->pos = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE fe_clone(IEnumFORMATETC *This, IEnumFORMATETC **out);
static const IEnumFORMATETCVtbl g_fe_vtbl = { fe_qi, fe_addref, fe_release, fe_next, fe_skip, fe_reset, fe_clone };
static HRESULT STDMETHODCALLTYPE fe_clone(IEnumFORMATETC *This, IEnumFORMATETC **out)
{
    FileEnum *c = HeapAlloc(GetProcessHeap(), 0, sizeof *c);
    if (!c) return E_OUTOFMEMORY;
    c->iface.lpVtbl = &g_fe_vtbl; c->refs = 1; c->pos = ((FileEnum *)This)->pos;
    *out = &c->iface;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE fd_qi(IDataObject *This, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDataObject)) { *out = This; This->lpVtbl->AddRef(This); return S_OK; }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE fd_addref(IDataObject *This) { return (ULONG)InterlockedIncrement(&((FileData *)This)->refs); }
static ULONG STDMETHODCALLTYPE fd_release(IDataObject *This)
{
    FileData *d = (FileData *)This;
    LONG r = InterlockedDecrement(&d->refs);
    if (!r) { GlobalFree(d->hdrop); HeapFree(GetProcessHeap(), 0, d); }
    return (ULONG)r;
}
static BOOL fd_wants(const FORMATETC *f) { return f && f->cfFormat == CF_HDROP && (f->tymed & TYMED_HGLOBAL) && (f->dwAspect & DVASPECT_CONTENT); }
static HRESULT STDMETHODCALLTYPE fd_getdata(IDataObject *This, FORMATETC *fmt, STGMEDIUM *m)
{
    FileData *d = (FileData *)This;
    if (!m) return E_INVALIDARG;
    if (!fd_wants(fmt)) return fmt && fmt->cfFormat != CF_HDROP ? DV_E_FORMATETC : DV_E_TYMED;
    SIZE_T n = GlobalSize(d->hdrop);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n);
    if (!g) return E_OUTOFMEMORY;
    CopyMemory(GlobalLock(g), GlobalLock(d->hdrop), n);
    GlobalUnlock(g); GlobalUnlock(d->hdrop);
    m->tymed = TYMED_HGLOBAL; m->hGlobal = g; m->pUnkForRelease = NULL;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE fd_getdatahere(IDataObject *This, FORMATETC *fmt, STGMEDIUM *m) { (void)This; (void)fmt; (void)m; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE fd_querygetdata(IDataObject *This, FORMATETC *fmt) { (void)This; return fd_wants(fmt) ? S_OK : DV_E_FORMATETC; }
static HRESULT STDMETHODCALLTYPE fd_canonical(IDataObject *This, FORMATETC *in, FORMATETC *out) { (void)This; if (out) { *out = *in; out->ptd = NULL; } return DATA_S_SAMEFORMATETC; }
static HRESULT STDMETHODCALLTYPE fd_setdata(IDataObject *This, FORMATETC *fmt, STGMEDIUM *m, BOOL rel) { (void)This; (void)fmt; (void)m; (void)rel; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE fd_enum(IDataObject *This, DWORD dir, IEnumFORMATETC **out)
{
    (void)This;
    if (!out) return E_POINTER;
    *out = NULL;
    if (dir != DATADIR_GET) return E_NOTIMPL;
    FileEnum *e = HeapAlloc(GetProcessHeap(), 0, sizeof *e);
    if (!e) return E_OUTOFMEMORY;
    e->iface.lpVtbl = &g_fe_vtbl; e->refs = 1; e->pos = 0;
    *out = &e->iface;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE fd_dadvise(IDataObject *This, FORMATETC *f, DWORD a, IAdviseSink *s, DWORD *c) { (void)This; (void)f; (void)a; (void)s; (void)c; return OLE_E_ADVISENOTSUPPORTED; }
static HRESULT STDMETHODCALLTYPE fd_dunadvise(IDataObject *This, DWORD c) { (void)This; (void)c; return OLE_E_ADVISENOTSUPPORTED; }
static HRESULT STDMETHODCALLTYPE fd_enumdadvise(IDataObject *This, IEnumSTATDATA **out) { (void)This; if (out) *out = NULL; return OLE_E_ADVISENOTSUPPORTED; }
static const IDataObjectVtbl g_fd_vtbl = { fd_qi, fd_addref, fd_release, fd_getdata, fd_getdatahere, fd_querygetdata, fd_canonical, fd_setdata, fd_enum, fd_dadvise, fd_dunadvise, fd_enumdadvise };

/* a DROPFILES block from a UTF-16 double-NUL list */
static HGLOBAL make_hdrop(const WCHAR *files, DWORD bytes, POINT client)
{
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + bytes + 4);
    if (!g) return NULL;
    DROPFILES *d = GlobalLock(g);
    d->pFiles = sizeof(DROPFILES);
    d->pt = client;
    d->fNC = FALSE;
    d->fWide = TRUE;
    CopyMemory(d + 1, files, bytes);
    GlobalUnlock(g);
    return g;
}

/* the file list in a data object's CF_HDROP, UTF-16 double-NUL (malloc), or NULL */
static WCHAR *hdrop_list(IDataObject *data, DWORD *bytes)
{
    FORMATETC f = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM m;
    ZeroMemory(&m, sizeof m);
    if (!data || FAILED(data->lpVtbl->GetData(data, &f, &m)) || m.tymed != TYMED_HGLOBAL || !m.hGlobal) return NULL;
    const DROPFILES *d = GlobalLock(m.hGlobal);
    SIZE_T size = GlobalSize(m.hGlobal);
    WCHAR *out = NULL;
    if (d && d->pFiles < size) {
        const BYTE *p = (const BYTE *)d + d->pFiles;
        SIZE_T avail = size - d->pFiles;
        if (d->fWide) {
            SIZE_T n = 0;
            while (n + 1 < avail / 2 && !(((const WCHAR *)p)[n] == 0 && ((const WCHAR *)p)[n + 1] == 0)) n++;
            out = HeapAlloc(GetProcessHeap(), 0, 2 * (n + 2));
            if (out) { CopyMemory(out, p, 2 * n); out[n] = out[n + 1] = 0; *bytes = (DWORD)(2 * (n + 2)); }
        } else {
            SIZE_T n = 0;
            while (n + 1 < avail && !(p[n] == 0 && p[n + 1] == 0)) n++;
            int w = MultiByteToWideChar(CP_ACP, 0, (const char *)p, (int)n, NULL, 0);
            out = HeapAlloc(GetProcessHeap(), 0, 2 * ((SIZE_T)w + 2));
            if (out) { MultiByteToWideChar(CP_ACP, 0, (const char *)p, (int)n, out, w); out[w] = out[w + 1] = 0; *bytes = (DWORD)(2 * (w + 2)); }
        }
    }
    GlobalUnlock(m.hGlobal);
    ReleaseStgMedium(&m);
    return out;
}

/* a drop from another program: to this window's (or an ancestor's) target */
static BOOL WINAPI drop_hook(HWND hwnd, POINT screen, DWORD effect, const WCHAR *files, DWORD bytes)
{
    HWND owner = NULL;
    IDropTarget *t = target_of(hwnd, &owner);
    if (!t) return FALSE;
    FileData *d = HeapAlloc(GetProcessHeap(), 0, sizeof *d);
    if (!d) return FALSE;
    POINT c = screen;
    ScreenToClient(owner, &c);
    d->iface.lpVtbl = &g_fd_vtbl; d->refs = 1;
    d->hdrop = make_hdrop(files, bytes, c);
    if (!d->hdrop) { HeapFree(GetProcessHeap(), 0, d); return FALSE; }
    t->lpVtbl->AddRef(t);
    POINTL pt = { screen.x, screen.y };
    DWORD keys = MK_LBUTTON, eff = effect ? effect : DROPEFFECT_COPY;
    DWORD allowed = eff;
    if (SUCCEEDED(t->lpVtbl->DragEnter(t, &d->iface, keys, pt, &eff))) {
        eff = allowed;
        t->lpVtbl->DragOver(t, keys, pt, &eff);
        eff = allowed;
        t->lpVtbl->Drop(t, &d->iface, keys, pt, &eff);
    }
    t->lpVtbl->Release(t);
    d->iface.lpVtbl->Release(&d->iface);
    return TRUE;
}

/* -----------------------------------------------------------------------
 * The drag
 * ----------------------------------------------------------------------- */
static DWORD key_state(void)
{
    DWORD k = 0;
    if (GetAsyncKeyState(VK_LBUTTON) < 0) k |= MK_LBUTTON;
    if (GetAsyncKeyState(VK_RBUTTON) < 0) k |= MK_RBUTTON;
    if (GetAsyncKeyState(VK_MBUTTON) < 0) k |= MK_MBUTTON;
    if (GetAsyncKeyState(VK_SHIFT) < 0) k |= MK_SHIFT;
    if (GetAsyncKeyState(VK_CONTROL) < 0) k |= MK_CONTROL;
    return k;
}

static void feedback(IDropSource *src, DWORD effect)
{
    if (src->lpVtbl->GiveFeedback(src, effect) == DRAGDROP_S_USEDEFAULTCURSORS)
        SetCursor(LoadCursorW(NULL, effect == DROPEFFECT_NONE ? (LPCWSTR)IDC_NO : (LPCWSTR)IDC_ARROW));
}

WINOLEAPI_(HRESULT) DoDragDrop(IDataObject *data, IDropSource *src, DWORD ok, DWORD *effect)
{
    if (effect) *effect = DROPEFFECT_NONE;
    if (!data || !src) return E_INVALIDARG;
    POINT pt;
    GetCursorPos(&pt);
    HWND capture = GetCapture();
    if (!capture) capture = WindowFromPoint(pt);
    if (!capture) capture = GetActiveWindow();
    if (!capture) return E_FAIL;
    SetCapture(capture);
    data->lpVtbl->AddRef(data);
    src->lpVtbl->AddRef(src);

    HWND local = NULL;                  /* this process's window with the target under the pointer */
    IDropTarget *target = NULL;
    UINT32 remote = 0;                  /* another program's window under the pointer */
    DWORD remote_flags = 0;
    DWORD eff = DROPEFFECT_NONE, keys = key_state();
    HRESULT r = DRAGDROP_S_CANCEL;
    BOOL done = FALSE, drop = FALSE;
    DWORD last_tick = GetTickCount();
    while (!done) {
        MSG m;
        BOOL got = PeekMessageW(&m, NULL, 0, 0, PM_REMOVE);
        if (!got) {
            if (GetTickCount() - last_tick < 50) { MsgWaitForMultipleObjects(0, NULL, FALSE, 50, QS_ALLINPUT); continue; }
            m.message = WM_MOUSEMOVE;                       /* poll: the pointer may have left our windows */
        }
        if (GetCapture() != capture) {                      /* someone took the mouse */
            done = TRUE; r = DRAGDROP_S_CANCEL; break;
        }
        BOOL escape = m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE;
        if (m.message == WM_QUIT) { PostQuitMessage((int)m.wParam); done = TRUE; break; }
        if (m.message == WM_LBUTTONUP || m.message == WM_RBUTTONUP || m.message == WM_MBUTTONUP || m.message == WM_MOUSEMOVE ||
            m.message == WM_KEYDOWN || m.message == WM_KEYUP || m.message == WM_SYSKEYDOWN || !got) {
            last_tick = GetTickCount();
            keys = key_state();
            if (m.message == WM_LBUTTONUP) keys &= ~MK_LBUTTON;
            if (m.message == WM_RBUTTONUP) keys &= ~MK_RBUTTON;
            HRESULT q = src->lpVtbl->QueryContinueDrag(src, escape, keys);
            if (q == DRAGDROP_S_DROP) { drop = TRUE; done = TRUE; }
            else if (q != S_OK) { r = q == DRAGDROP_S_CANCEL ? q : q; done = TRUE; drop = FALSE; if (FAILED(q)) r = q; }
            GetCursorPos(&pt);
            POINTL pl = { pt.x, pt.y };
            if (!done) {
                /* what is under the pointer now */
                DWORD pid = 0, flags = 0;
                UINT32 kid = NovaWindowAt(pt, &pid, &flags);
                HWND nl = NULL;
                IDropTarget *nt = NULL;
                UINT32 nr = 0;
                if (kid && pid == GetCurrentProcessId()) {
                    HWND h = WindowFromPoint(pt);
                    nt = h ? target_of(h, &nl) : NULL;
                } else if (kid && (flags & 3)) nr = kid;
                if (nt != target || nl != local) {
                    if (target) target->lpVtbl->DragLeave(target);
                    target = nt; local = nl;
                    eff = DROPEFFECT_NONE;
                    if (target) { eff = ok; if (FAILED(target->lpVtbl->DragEnter(target, data, keys, pl, &eff))) eff = DROPEFFECT_NONE; eff &= ok; }
                } else if (target) {
                    eff = ok;
                    if (FAILED(target->lpVtbl->DragOver(target, keys, pl, &eff))) eff = DROPEFFECT_NONE;
                    eff &= ok;
                }
                if (!target) {
                    remote = nr; remote_flags = flags;
                    eff = remote ? ((ok & DROPEFFECT_COPY) ? DROPEFFECT_COPY : (ok & DROPEFFECT_MOVE) ? DROPEFFECT_MOVE : (ok & DROPEFFECT_LINK) ? DROPEFFECT_LINK : DROPEFFECT_NONE) : DROPEFFECT_NONE;
                }
                feedback(src, eff);
            }
        } else if (got) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    (void)remote_flags;
    if (drop) {
        if (target) {
            eff = ok;
            if (FAILED(target->lpVtbl->Drop(target, data, keys, (POINTL){ pt.x, pt.y }, &eff))) eff = DROPEFFECT_NONE;
            eff &= ok;
            target = NULL;
        } else if (remote) {
            DWORD bytes = 0;
            WCHAR *list = hdrop_list(data, &bytes);
            if (list && NovaSendDrop(remote, pt, eff, list, bytes)) { /* delivered */ }
            else eff = DROPEFFECT_NONE;
            if (list) HeapFree(GetProcessHeap(), 0, list);
        } else eff = DROPEFFECT_NONE;
        r = eff == DROPEFFECT_NONE ? DRAGDROP_S_CANCEL : DRAGDROP_S_DROP;
    }
    if (target) target->lpVtbl->DragLeave(target);
    if (GetCapture() == capture) ReleaseCapture();
    if (effect) *effect = r == DRAGDROP_S_DROP ? eff : DROPEFFECT_NONE;
    src->lpVtbl->Release(src);
    data->lpVtbl->Release(data);
    return r;
}
