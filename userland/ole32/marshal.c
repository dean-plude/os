/*
 * marshal.c — COM between processes: the standard marshaler, the channel
 * calls travel on, and starting local servers.
 *
 * Marshaling an interface pointer (CoMarshalInterface) exports the object:
 * a stub manager per object holds it and one interface stub per interface
 * handed out (IRpcStubBuffer, from the interface's proxy/stub factory:
 * CoGetPSClsid, then that class's IPSFactoryBuffer), and the OBJREF written
 * names the exporting process (the OXID), the object (OID) and the
 * interface (IPID), in Windows' OBJREF_STANDARD layout.  Unmarshaling it in
 * another process makes a proxy manager for the object (one per OXID and
 * OID, so identity holds): its IUnknown is the object's identity there,
 * and each interface asked for is an interface proxy (IRpcProxyBuffer from
 * the same factory) connected to a channel (IRpcChannelBuffer) that sends
 * the proxy's NDR request to the exporting process and returns the reply.
 * QueryInterface for an interface the proxy manager does not have yet asks
 * the object (a remote QueryInterface); the last Release gives the
 * object's references back.  Unmarshaling in the exporting process itself
 * gives the object.  An object with its own IMarshal is custom marshaled
 * (OBJREF_CUSTOM).
 *
 * The transport is a named pipe per process, \\.\pipe\novaole-PID, and
 * one per class a local server registered, \\.\pipe\novaole-class-{CLSID}
 * (what a client connects to to activate it: CoCreateInstance with
 * CLSCTX_LOCAL_SERVER starts the class's LocalServer32 with -Embedding when
 * nobody listens there yet and waits for its CoRegisterClassObject).  Each
 * calling thread has its own connection to each process it calls, so a
 * thread's calls arrive in order and a reply always answers the call its
 * thread is waiting in.  A served connection hands every request to a
 * worker thread, so a call can call back into its caller's process (and
 * the caller's own connection keeps working for calls the callback makes).
 * Requests for an object exported from a single-threaded apartment run on
 * that apartment's thread, posted to a hidden window there; an STA thread
 * waiting for a reply keeps taking those messages, so calls back into it
 * run.  MTA objects are called on the worker thread.
 *
 * IClassFactory needs no proxy/stub DLL: its proxy here sends
 * CreateInstance and LockServer as requests of their own, the way
 * activation asks a class object for an instance.  Not done: OBJREF_HANDLER
 * (handlers), pinging (a client that dies holding references keeps the
 * server's objects alive), call cancellation and message filters, and the
 * DUALSTRINGARRAY's bindings are written but never read (the OXID is the
 * exporting process's ID).
 */

#define NOVA_BUILD_OLE32
#include "com_private.h"

/* ---- wire messages ------------------------------------------------------ */
#define OLE_MAGIC 0x454c4f4eu                     /* "NOLE" */
enum {
    M_CALL = 1,        /* IPID, method, data rep, NDR data -> reply: NDR data */
    M_REMQI,           /* OID, IID -> IPID */
    M_ADDREF,          /* OID, count */
    M_RELEASE,         /* OID, count; handled in order, then answered */
    M_ACTIVATE,        /* CLSID, IID, mode -> OBJREF */
    M_CF_CREATE,       /* OID, IID -> OBJREF (IClassFactory::CreateInstance) */
    M_CF_LOCK,         /* OID, lock (IClassFactory::LockServer) */
    M_REPLY = 0x100,
};
typedef struct { DWORD magic, type, len; LONG hr; } MsgHdr;
typedef struct { GUID ipid; DWORD method, datarep; } CallHdr;
#define HDR ((DWORD)sizeof(MsgHdr))
#define CALL_PREFIX ((DWORD)(sizeof(MsgHdr) + sizeof(CallHdr)))

#define OBJREF_SIGNATURE 0x574f454du              /* "MEOW" */
#define OBJREF_STANDARD 1
#define OBJREF_CUSTOM   4
#define SORF_TABLESTRONG 0x8000u                  /* (ours) a table-strong reference, released by CoReleaseMarshalData */
#define RPC_S_SERVER_UNAVAILABLE_HR ((HRESULT)0x800706BAL)
#define CO_E_SERVER_EXEC_FAILURE_ ((HRESULT)0x80080005L)
#define WM_OLE_CALL (WM_USER + 0x2a1)
#define RPC_E_INVALIDMETHOD ((HRESULT)0x80010104L)

static const IID IID_INovaProxyManager = { 0x6e0f3a10, 0x4d2b, 0x4c55, { 0x9a, 0x51, 0x7c, 0x0d, 0xe0, 0xa1, 0xc0, 0x01 } };

static void *mem_alloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), 0, n ? n : 1); }
static void *mem_zalloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n ? n : 1); }
static void mem_free(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

static void dbg(const char *what, HRESULT hr)
{
    static const char hx[] = "0123456789abcdef";
    char m[160];
    int n = 0;
    for (const char *p = "ole32: "; *p; ) m[n++] = *p++;
    for (; *what && n < 140; ) m[n++] = *what++;
    m[n++] = ' ';
    for (int i = 7; i >= 0; i--) m[n++] = hx[((DWORD)hr >> (4 * i)) & 15];
    m[n++] = '\n';
    m[n] = 0;
    OutputDebugStringA(m);
}

/* "\\.\pipe\novaole-PID" / "\\.\pipe\novaole-class-{CLSID}" */
static WCHAR *put_s(WCHAR *d, const WCHAR *s) { while (*s) *d++ = *s++; *d = 0; return d; }
static WCHAR *put_u(WCHAR *d, DWORD v)
{
    WCHAR t[12];
    int n = 0;
    do t[n++] = (WCHAR)('0' + v % 10); while (v /= 10);
    while (n) *d++ = t[--n];
    *d = 0;
    return d;
}
static void process_pipe(DWORD pid, WCHAR *out) { put_u(put_s(out, L"\\\\.\\pipe\\novaole-"), pid); }
static void class_pipe(REFCLSID clsid, WCHAR *out)
{
    WCHAR *p = put_s(out, L"\\\\.\\pipe\\novaole-class-");
    StringFromGUID2(clsid, p, 39);
}

/* ---- apartments ---------------------------------------------------------- */
typedef struct { void (*fn)(void *); void *arg; HANDLE done; } AptCall;
static __declspec(thread) HWND t_apt_wnd;

static LRESULT CALLBACK apt_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_OLE_CALL) {
        AptCall *c = (AptCall *)lp;
        c->fn(c->arg);
        SetEvent(c->done);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static HWND apt_window(void)
{
    if (t_apt_wnd) return t_apt_wnd;
    static volatile LONG registered;
    if (!InterlockedExchange(&registered, 1)) {
        WNDCLASSW wc;
        ZeroMemory(&wc, sizeof wc);
        wc.lpfnWndProc = apt_wndproc;
        wc.hInstance = GetModuleHandleW(L"ole32.dll");
        wc.lpszClassName = L"OleMainThreadWndClass";
        RegisterClassW(&wc);
    }
    t_apt_wnd = CreateWindowExW(0, L"OleMainThreadWndClass", L"OLEChannelWnd", 0, 0, 0, 0, 0, HWND_MESSAGE, 0,
                                GetModuleHandleW(L"ole32.dll"), 0);
    return t_apt_wnd;
}

void ole_current_apt(OleApt *a)
{
    if (ole_thread_is_sta()) { a->tid = GetCurrentThreadId(); a->wnd = apt_window(); }
    else { a->tid = 0; a->wnd = 0; }
}

/* run fn in the apartment: on its thread if it is an STA, else right here */
static BOOL apt_run(const OleApt *a, void (*fn)(void *), void *arg)
{
    if (!a->wnd || a->tid == GetCurrentThreadId()) { fn(arg); return TRUE; }
    AptCall c = { fn, arg, CreateEventW(0, TRUE, FALSE, 0) };
    if (!c.done) return FALSE;
    BOOL ok = PostMessageW(a->wnd, WM_OLE_CALL, 0, (LPARAM)&c);
    if (ok) WaitForSingleObject(c.done, INFINITE);
    CloseHandle(c.done);
    return ok;
}

/* an STA thread waiting for a reply: calls into its apartment still run */
static void apt_pump(void)
{
    MSG m;
    if (!t_apt_wnd) return;
    while (PeekMessageW(&m, t_apt_wnd, WM_OLE_CALL, WM_OLE_CALL, PM_REMOVE)) DispatchMessageW(&m);
}

/* ---- pipe I/O (overlapped handles: waits end on events) ------------------ */
static BOOL pipe_io(HANDLE h, HANDLE ev, void *buf, DWORD n, BOOL write)
{
    BYTE *p = buf;
    while (n) {
        OVERLAPPED ov;
        ZeroMemory(&ov, sizeof ov);
        ov.hEvent = ev;
        ResetEvent(ev);
        DWORD done = 0;
        BOOL ok = write ? WriteFile(h, p, n, &done, &ov) : ReadFile(h, p, n, &done, &ov);
        if (!ok) {
            if (GetLastError() != ERROR_IO_PENDING) return FALSE;
            if (!GetOverlappedResult(h, &ov, &done, TRUE)) return FALSE;
        } else if (!done && !GetOverlappedResult(h, &ov, &done, TRUE)) return FALSE;
        if (!done) return FALSE;
        p += done;
        n -= done;
    }
    return TRUE;
}

/* a whole message: the header, then its payload (in a buffer with room
 * for a header in front, so a reply can be sent from it in one write) */
static BOOL read_msg(HANDLE h, HANDLE ev, MsgHdr *hdr, BYTE **payload, BOOL pump)
{
    if (pump && t_apt_wnd) {
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(h, 0, 0, 0, &avail, 0)) return FALSE;
            if (avail) break;
            apt_pump();
            Sleep(1);
        }
    }
    if (!pipe_io(h, ev, hdr, HDR, FALSE) || hdr->magic != OLE_MAGIC || hdr->len > 0x10000000u) return FALSE;
    BYTE *b = mem_alloc(HDR + hdr->len);
    if (!b) return FALSE;
    if (hdr->len && !pipe_io(h, ev, b + HDR, hdr->len, FALSE)) { mem_free(b); return FALSE; }
    *payload = b;
    return TRUE;
}

/* ---- client connections: one per thread and process called ------------- */
typedef struct Conn { struct Conn *next; DWORD pid; HANDLE pipe, ev; } Conn;
static __declspec(thread) Conn *t_conns;

static HANDLE open_pipe(const WCHAR *name, DWORD wait_ms)
{
    ULONGLONG until = GetTickCount64() + wait_ms;
    for (;;) {
        HANDLE h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, 0, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, 0);
        if (h != INVALID_HANDLE_VALUE) return h;
        DWORD e = GetLastError();
        if (GetTickCount64() >= until) return INVALID_HANDLE_VALUE;
        if (e == ERROR_PIPE_BUSY) WaitNamedPipeW(name, 200);
        else if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) Sleep(10);
        else return INVALID_HANDLE_VALUE;
    }
}

static Conn *conn_get(DWORD pid)
{
    for (Conn *c = t_conns; c; c = c->next)
        if (c->pid == pid) return c;
    WCHAR name[64];
    process_pipe(pid, name);
    HANDLE h = open_pipe(name, 2000);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    Conn *c = mem_zalloc(sizeof *c);
    if (!c || !(c->ev = CreateEventW(0, TRUE, FALSE, 0))) { CloseHandle(h); mem_free(c); return NULL; }
    c->pid = pid;
    c->pipe = h;
    c->next = t_conns;
    t_conns = c;
    return c;
}

static void conn_drop(Conn *c)
{
    for (Conn **pp = &t_conns; *pp; pp = &(*pp)->next)
        if (*pp == c) { *pp = c->next; break; }
    CloseHandle(c->pipe);
    CloseHandle(c->ev);
    mem_free(c);
}

/* worker threads close what they opened before they end */
static void conns_close_all(void)
{
    while (t_conns) conn_drop(t_conns);
}

/* send msg (MsgHdr first, filled in here but for type) and wait for its
 * reply; the reply's payload is returned at *payload + HDR */
static HRESULT transact_on(HANDLE pipe, HANDLE ev, BYTE *msg, DWORD type, DWORD len, BOOL oneway,
                           BYTE **reply, DWORD *rlen, HRESULT *rhr)
{
    MsgHdr *h = (MsgHdr *)msg;
    h->magic = OLE_MAGIC;
    h->type = type;
    h->len = len;
    h->hr = 0;
    if (!pipe_io(pipe, ev, msg, HDR + len, TRUE)) return RPC_S_SERVER_UNAVAILABLE_HR;
    if (oneway) return S_OK;
    MsgHdr rh;
    BYTE *p = 0;
    if (!read_msg(pipe, ev, &rh, &p, ole_thread_is_sta())) return RPC_S_SERVER_UNAVAILABLE_HR;
    if (rh.type != (type | M_REPLY)) { mem_free(p); return RPC_E_INVALID_OBJREF; }
    if (reply) *reply = p; else mem_free(p);
    if (rlen) *rlen = rh.len;
    if (rhr) *rhr = rh.hr;
    return S_OK;
}

static HRESULT transact(DWORD pid, BYTE *msg, DWORD type, DWORD len, BOOL oneway, BYTE **reply, DWORD *rlen, HRESULT *rhr)
{
    Conn *c = conn_get(pid);
    if (!c) return RPC_S_SERVER_UNAVAILABLE_HR;
    HRESULT hr = transact_on(c->pipe, c->ev, msg, type, len, oneway, reply, rlen, rhr);
    if (FAILED(hr)) conn_drop(c);
    return hr;
}

/* small requests: header + up to 64 bytes */
typedef struct { MsgHdr h; BYTE b[64]; } SmallMsg;

/* ---- the exporter: stub managers ---------------------------------------- */
typedef struct Ifc { struct Ifc *next; GUID ipid; IID iid; IRpcStubBuffer *stub; IUnknown *iface; } Ifc;
typedef struct StubMgr {
    struct StubMgr *next;
    ULONGLONG oid;
    IUnknown *obj;                                 /* the identity, held */
    LONG refs;                                     /* references other processes hold */
    LONG table_refs;                               /* table-strong marshals not released */
    BOOL dead;
    LONG holds;                                    /* the table's, and each request working on it */
    OleApt apt;
    Ifc *ifcs;
} StubMgr;

static SRWLOCK g_exp_lock = SRWLOCK_INIT;
static StubMgr *g_stubs;
static ULONGLONG g_next_oid = 1;
static void exporter_start(void);

static StubMgr *stub_by_oid(ULONGLONG oid)
{
    for (StubMgr *m = g_stubs; m; m = m->next)
        if (m->oid == oid && !m->dead) return m;
    return NULL;
}

static void mgr_unhold(StubMgr *m)
{
    if (!InterlockedDecrement(&m->holds)) mem_free(m);
}

/* ends an export (it is out of the table): runs in the object's apartment */
static void destroy_stub_mgr(void *arg)
{
    StubMgr *m = arg;
    AcquireSRWLockExclusive(&g_exp_lock);
    Ifc *i = m->ifcs;
    m->ifcs = NULL;
    ReleaseSRWLockExclusive(&g_exp_lock);
    while (i) {
        Ifc *n = i->next;
        if (i->stub) { i->stub->lpVtbl->Disconnect(i->stub); i->stub->lpVtbl->Release(i->stub); }
        if (i->iface) i->iface->lpVtbl->Release(i->iface);
        mem_free(i);
        i = n;
    }
    m->obj->lpVtbl->Release(m->obj);
    mgr_unhold(m);
}

/* drop n references (and a table reference); the last one ends the export */
static void stub_release_locked(StubMgr *m, LONG n, LONG table)
{
    m->refs -= n;
    m->table_refs -= table;
    if (m->refs < 0) m->refs = 0;
    if (m->table_refs < 0) m->table_refs = 0;
    BOOL last = !m->refs && !m->table_refs && !m->dead;
    if (last) {
        m->dead = TRUE;
        for (StubMgr **pp = &g_stubs; *pp; pp = &(*pp)->next)
            if (*pp == m) { *pp = m->next; break; }
    }
    if (!last) return;
    OleApt apt = m->apt;
    ReleaseSRWLockExclusive(&g_exp_lock);
    if (!apt_run(&apt, destroy_stub_mgr, m)) destroy_stub_mgr(m);
    AcquireSRWLockExclusive(&g_exp_lock);
}

static void stub_release(StubMgr *m, LONG n, LONG table)
{
    AcquireSRWLockExclusive(&g_exp_lock);
    stub_release_locked(m, n, table);
    ReleaseSRWLockExclusive(&g_exp_lock);
}

static void stub_release_oid(ULONGLONG oid, LONG n, LONG table)
{
    AcquireSRWLockExclusive(&g_exp_lock);
    StubMgr *m = stub_by_oid(oid);
    if (m) stub_release_locked(m, n, table);
    ReleaseSRWLockExclusive(&g_exp_lock);
}

/* the stub manager for an object (its identity unk, AddRef'd by the
 * caller), made in the calling thread's apartment; refs and table refs
 * are added under the lock, so a dying one is never revived */
static StubMgr *stub_for_object(IUnknown *unk, LONG refs, LONG table)
{
    AcquireSRWLockExclusive(&g_exp_lock);
    StubMgr *m = g_stubs;
    while (m && (m->obj != unk || m->dead)) m = m->next;
    if (!m && (m = mem_zalloc(sizeof *m))) {
        m->oid = g_next_oid++;
        m->holds = 1;
        m->obj = unk;
        unk->lpVtbl->AddRef(unk);
        ole_current_apt(&m->apt);
        m->next = g_stubs;
        g_stubs = m;
    }
    if (m) { m->refs += refs; m->table_refs += table; }
    ReleaseSRWLockExclusive(&g_exp_lock);
    return m;
}

static HRESULT ps_factory(REFIID iid, IPSFactoryBuffer **out)
{
    CLSID ps;
    HRESULT hr = CoGetPSClsid(iid, &ps);
    if (FAILED(hr)) return hr;
    return CoGetClassObject(&ps, CLSCTX_INPROC_SERVER, 0, &IID_IPSFactoryBuffer, (void **)out);
}

/* the interface's IPID (and stub) on the object; runs in its apartment */
static HRESULT stub_interface(StubMgr *m, REFIID iid, GUID *ipid)
{
    AcquireSRWLockShared(&g_exp_lock);
    for (Ifc *i = m->ifcs; i; i = i->next)
        if (IsEqualIID(&i->iid, iid)) { *ipid = i->ipid; ReleaseSRWLockShared(&g_exp_lock); return S_OK; }
    ReleaseSRWLockShared(&g_exp_lock);

    IUnknown *iface = 0;
    HRESULT hr = m->obj->lpVtbl->QueryInterface(m->obj, iid, (void **)&iface);
    if (FAILED(hr)) return hr;
    IRpcStubBuffer *stub = 0;
    if (!IsEqualIID(iid, &IID_IUnknown) && !IsEqualIID(iid, &IID_IClassFactory)) {
        IPSFactoryBuffer *f = 0;
        hr = ps_factory(iid, &f);
        if (SUCCEEDED(hr)) {
            hr = f->lpVtbl->CreateStub(f, iid, m->obj, &stub);
            f->lpVtbl->Release(f);
        }
        if (FAILED(hr)) {
            dbg("no stub for an interface:", hr);
            iface->lpVtbl->Release(iface);
            return hr == REGDB_E_IIDNOTREG || hr == REGDB_E_CLASSNOTREG ? E_NOINTERFACE : hr;
        }
    }
    Ifc *n = mem_zalloc(sizeof *n);
    if (!n) { if (stub) stub->lpVtbl->Release(stub); iface->lpVtbl->Release(iface); return E_OUTOFMEMORY; }
    n->iid = *iid;
    n->stub = stub;
    n->iface = iface;
    CoCreateGuid(&n->ipid);
    AcquireSRWLockExclusive(&g_exp_lock);
    BOOL dead = m->dead;
    Ifc *dup = m->ifcs;
    while (dup && !IsEqualIID(&dup->iid, iid)) dup = dup->next;
    if (dead) ;
    else if (!dup) { n->next = m->ifcs; m->ifcs = n; *ipid = n->ipid; }
    else *ipid = dup->ipid;
    ReleaseSRWLockExclusive(&g_exp_lock);
    if (dup || dead) {
        if (stub) { stub->lpVtbl->Disconnect(stub); stub->lpVtbl->Release(stub); }
        iface->lpVtbl->Release(iface);
        mem_free(n);
    }
    return dead ? RPC_E_DISCONNECTED : S_OK;
}

static StubMgr *stub_by_ipid(const GUID *ipid, Ifc **ifc)
{
    for (StubMgr *m = g_stubs; m; m = m->next)
        if (!m->dead)
            for (Ifc *i = m->ifcs; i; i = i->next)
                if (IsEqualGUID(&i->ipid, ipid)) { *ifc = i; return m; }
    return NULL;
}

/* ---- OBJREF ------------------------------------------------------------- */
#define STD_OBJREF_FIXED 64                         /* through the IPID */

static DWORD std_objref_size(void)
{
    /* DUALSTRINGARRAY: ncacn_np, "\pipe\novaole-PID", 0, 0; then the
     * security binding (WinNT, no principal), 0, 0 */
    return STD_OBJREF_FIXED + 4 + 2 * (1 + 25 + 1 + 1 + 2 + 1 + 1);
}

static HRESULT write_std_objref(IStream *s, REFIID iid, DWORD flags, DWORD refs, DWORD pid, ULONGLONG oid, const GUID *ipid)
{
    BYTE b[256];
    DWORD n = 0;
    ZeroMemory(b, sizeof b);
#define PUT(p, len) (CopyMemory(b + n, (p), (len)), n += (len))
    DWORD v = OBJREF_SIGNATURE; PUT(&v, 4);
    v = OBJREF_STANDARD; PUT(&v, 4);
    PUT(iid, 16);
    PUT(&flags, 4);
    PUT(&refs, 4);
    ULONGLONG oxid = pid; PUT(&oxid, 8);
    PUT(&oid, 8);
    PUT(ipid, 16);
    WCHAR str[40];
    WCHAR *e = put_u(put_s(str, L"\\pipe\\novaole-"), pid);
    USHORT entries[48];
    int k = 0;
    entries[k++] = 0x000F;                                     /* ncacn_np */
    for (WCHAR *q = str; q < e; q++) entries[k++] = *q;
    entries[k++] = 0;
    entries[k++] = 0;                                          /* end of the string bindings */
    USHORT sec = (USHORT)k;
    entries[k++] = 0x000A;                                     /* RPC_C_AUTHN_WINNT */
    entries[k++] = 0xFFFF;
    entries[k++] = 0;
    entries[k++] = 0;
    USHORT hdr[2] = { (USHORT)k, sec };
    PUT(hdr, 4);
    PUT(entries, 2 * k);
#undef PUT
    return s->lpVtbl->Write(s, b, n, 0);
}

static HRESULT read_exact(IStream *s, void *p, ULONG n)
{
    ULONG got = 0;
    HRESULT hr = s->lpVtbl->Read(s, p, n, &got);
    if (FAILED(hr)) return hr;
    return got == n ? S_OK : RPC_E_INVALID_OBJREF;
}

typedef struct {
    DWORD flags;
    IID iid;
    /* standard */
    DWORD std_flags, refs;
    ULONGLONG oxid, oid;
    GUID ipid;
    /* custom */
    CLSID clsid;
} ObjRef;

static HRESULT read_objref(IStream *s, ObjRef *o)
{
    BYTE b[24];
    HRESULT hr = read_exact(s, b, 24);
    if (FAILED(hr)) return hr;
    DWORD sig;
    CopyMemory(&sig, b, 4);
    CopyMemory(&o->flags, b + 4, 4);
    CopyMemory(&o->iid, b + 8, 16);
    if (sig != OBJREF_SIGNATURE) return RPC_E_INVALID_OBJREF;
    if (o->flags == OBJREF_STANDARD) {
        BYTE r[44];
        if (FAILED(hr = read_exact(s, r, 44))) return hr;
        CopyMemory(&o->std_flags, r, 4);
        CopyMemory(&o->refs, r + 4, 4);
        CopyMemory(&o->oxid, r + 8, 8);
        CopyMemory(&o->oid, r + 16, 8);
        CopyMemory(&o->ipid, r + 24, 16);
        USHORT ds[2];
        CopyMemory(ds, r + 40, 4);
        for (USHORT i = 0; i < ds[0]; i++) {                   /* skip the bindings */
            USHORT x;
            if (FAILED(hr = read_exact(s, &x, 2))) return hr;
        }
        return S_OK;
    }
    if (o->flags == OBJREF_CUSTOM) {
        BYTE r[24];
        if (FAILED(hr = read_exact(s, r, 24))) return hr;        /* CLSID, cbExtension, size */
        CopyMemory(&o->clsid, r, 16);
        return S_OK;
    }
    return RPC_E_INVALID_OBJREF;
}

/* ---- proxy managers ------------------------------------------------------ */
typedef struct Chan Chan;
typedef struct PIfc { struct PIfc *next; IID iid; GUID ipid; IRpcProxyBuffer *buf; void *iface; Chan *chan; } PIfc;
typedef struct ProxyMgr {
    IUnknown unk;
    const IClassFactoryVtbl *cf_vtbl;              /* the built-in IClassFactory proxy */
    struct ProxyMgr *next;
    LONG refs;
    DWORD pid;
    ULONGLONG oid;
    LONG remote_refs;                              /* references on the object this process holds */
    BOOL dead;
    PIfc *ifcs;
    SRWLOCK lock;
} ProxyMgr;

struct Chan { IRpcChannelBuffer iface; LONG refs; DWORD pid; GUID ipid; };

static SRWLOCK g_pm_lock = SRWLOCK_INIT;
static ProxyMgr *g_proxies;

/* the channel: requests go out with a header in front of the NDR data */
static HRESULT STDMETHODCALLTYPE ch_qi(IRpcChannelBuffer *This, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IRpcChannelBuffer)) {
        *ppv = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    *ppv = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE ch_addref(IRpcChannelBuffer *This) { return (ULONG)InterlockedIncrement(&((Chan *)This)->refs); }
static ULONG STDMETHODCALLTYPE ch_release(IRpcChannelBuffer *This)
{
    LONG r = InterlockedDecrement(&((Chan *)This)->refs);
    if (!r) mem_free(This);
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE ch_getbuffer(IRpcChannelBuffer *This, RPCOLEMESSAGE *msg, REFIID riid)
{
    (void)This; (void)riid;
    BYTE *b = mem_alloc(CALL_PREFIX + msg->cbBuffer);
    if (!b) return E_OUTOFMEMORY;
    ((CallHdr *)(b + HDR))->datarep = 0x10;              /* (not REPLY_TAG: FreeBuffer tells them apart) */
    msg->Buffer = b + CALL_PREFIX;
    msg->dataRepresentation = 0x10;                            /* NDR_LOCAL_DATA_REPRESENTATION */
    return S_OK;
}
/* a request's buffer starts CALL_PREFIX before its data, a reply's HDR;
 * the DWORD just before the data tells which (a request's data
 * representation, or REPLY_TAG in a reply header's hr) */
#define REQ_BASE(p) ((BYTE *)(p) - CALL_PREFIX)
#define REPLY_TAG 0x7265706cu
#define IS_REPLY(p) (*(DWORD *)((BYTE *)(p) - 4) == REPLY_TAG)
static HRESULT STDMETHODCALLTYPE ch_sendreceive(IRpcChannelBuffer *This, RPCOLEMESSAGE *msg, ULONG *status)
{
    Chan *c = (Chan *)This;
    if (status) *status = 0;
    BYTE *base = REQ_BASE(msg->Buffer);
    CallHdr *ch = (CallHdr *)(base + HDR);
    ch->ipid = c->ipid;
    ch->method = msg->iMethod & 0xffff;
    ch->datarep = msg->dataRepresentation;
    BYTE *reply = 0;
    DWORD rlen = 0;
    HRESULT rhr = S_OK;
    HRESULT hr = transact(c->pid, base, M_CALL, (DWORD)sizeof(CallHdr) + msg->cbBuffer, FALSE, &reply, &rlen, &rhr);
    if (FAILED(hr)) return hr;                                  /* the request stays: the proxy frees it */
    mem_free(base);
    if (FAILED(rhr)) {
        mem_free(reply);
        msg->Buffer = 0;
        msg->cbBuffer = 0;
        return rhr;
    }
    ((MsgHdr *)reply)->hr = (LONG)REPLY_TAG;
    msg->Buffer = reply + HDR;
    msg->cbBuffer = rlen;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ch_freebuffer(IRpcChannelBuffer *This, RPCOLEMESSAGE *msg)
{
    (void)This;
    if (!msg->Buffer) return S_OK;
    BYTE *p = msg->Buffer;
    if (IS_REPLY(p)) mem_free(p - HDR);
    else mem_free(REQ_BASE(p));
    msg->Buffer = 0;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ch_getdestctx(IRpcChannelBuffer *This, DWORD *ctx, void **pv)
{
    (void)This;
    if (ctx) *ctx = MSHCTX_LOCAL;
    if (pv) *pv = 0;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ch_isconnected(IRpcChannelBuffer *This) { (void)This; return S_OK; }
static const IRpcChannelBufferVtbl g_chan_vtbl = {
    ch_qi, ch_addref, ch_release, ch_getbuffer, ch_sendreceive, ch_freebuffer, ch_getdestctx, ch_isconnected,
};

static Chan *chan_new(DWORD pid, const GUID *ipid)
{
    Chan *c = mem_zalloc(sizeof *c);
    if (!c) return NULL;
    c->iface.lpVtbl = &g_chan_vtbl;
    c->refs = 1;
    c->pid = pid;
    c->ipid = *ipid;
    return c;
}

static HRESULT std_unmarshal_objref(const ObjRef *o, REFIID riid, void **ppv);
static HRESULT std_marshal(IStream *s, REFIID riid, IUnknown *p, DWORD flags);

/* the server's answer to a request carrying an OBJREF: unmarshal it */
static HRESULT unmarshal_reply(BYTE *reply, DWORD rlen, REFIID riid, void **ppv)
{
    IStream *s;
    HRESULT hr = CreateStreamOnHGlobal(0, TRUE, &s);
    if (FAILED(hr)) return hr;
    s->lpVtbl->Write(s, reply + HDR, rlen, 0);
    LARGE_INTEGER zero = { 0 };
    s->lpVtbl->Seek(s, zero, STREAM_SEEK_SET, 0);
    hr = CoUnmarshalInterface(s, riid, ppv);
    s->lpVtbl->Release(s);
    return hr;
}

/* IClassFactory through the proxy manager */
#define PM_FROM_CF(p) ((ProxyMgr *)((BYTE *)(p) - offsetof(ProxyMgr, cf_vtbl)))
static HRESULT STDMETHODCALLTYPE pcf_qi(IClassFactory *This, REFIID riid, void **ppv) { ProxyMgr *m = PM_FROM_CF(This); return m->unk.lpVtbl->QueryInterface(&m->unk, riid, ppv); }
static ULONG STDMETHODCALLTYPE pcf_addref(IClassFactory *This) { ProxyMgr *m = PM_FROM_CF(This); return m->unk.lpVtbl->AddRef(&m->unk); }
static ULONG STDMETHODCALLTYPE pcf_release(IClassFactory *This) { ProxyMgr *m = PM_FROM_CF(This); return m->unk.lpVtbl->Release(&m->unk); }
static HRESULT STDMETHODCALLTYPE pcf_create(IClassFactory *This, IUnknown *outer, REFIID riid, void **ppv)
{
    ProxyMgr *m = PM_FROM_CF(This);
    if (!ppv) return E_POINTER;
    *ppv = 0;
    if (outer) return CLASS_E_NOAGGREGATION;
    SmallMsg q;
    CopyMemory(q.b, &m->oid, 8);
    CopyMemory(q.b + 8, riid, 16);
    BYTE *reply = 0;
    DWORD rlen = 0;
    HRESULT rhr = S_OK, hr = transact(m->pid, (BYTE *)&q, M_CF_CREATE, 24, FALSE, &reply, &rlen, &rhr);
    if (FAILED(hr)) return hr;
    if (SUCCEEDED(rhr)) rhr = unmarshal_reply(reply, rlen, riid, ppv);
    mem_free(reply);
    return rhr;
}
static HRESULT STDMETHODCALLTYPE pcf_lock(IClassFactory *This, BOOL lock)
{
    ProxyMgr *m = PM_FROM_CF(This);
    SmallMsg q;
    DWORD v = lock != 0;
    CopyMemory(q.b, &m->oid, 8);
    CopyMemory(q.b + 8, &v, 4);
    HRESULT rhr = S_OK, hr = transact(m->pid, (BYTE *)&q, M_CF_LOCK, 12, FALSE, 0, 0, &rhr);
    return FAILED(hr) ? hr : rhr;
}
static const IClassFactoryVtbl g_pcf_vtbl = { pcf_qi, pcf_addref, pcf_release, pcf_create, pcf_lock };

static void pm_destroy(ProxyMgr *m)
{
    PIfc *i = m->ifcs;
    while (i) {
        PIfc *n = i->next;
        if (i->buf) { i->buf->lpVtbl->Disconnect(i->buf); i->buf->lpVtbl->Release(i->buf); }
        if (i->chan) i->chan->iface.lpVtbl->Release(&i->chan->iface);
        mem_free(i);
        i = n;
    }
    if (m->remote_refs > 0) {                       /* give the object's references back */
        SmallMsg q;
        DWORD n = (DWORD)m->remote_refs, z = 0;
        CopyMemory(q.b, &m->oid, 8);
        CopyMemory(q.b + 8, &n, 4);
        CopyMemory(q.b + 12, &z, 4);
        transact(m->pid, (BYTE *)&q, M_RELEASE, 16, FALSE, 0, 0, 0);
    }
    mem_free(m);
}

static ULONG STDMETHODCALLTYPE pm_addref(IUnknown *This) { return (ULONG)InterlockedIncrement(&((ProxyMgr *)This)->refs); }
static ULONG STDMETHODCALLTYPE pm_release(IUnknown *This)
{
    ProxyMgr *m = (ProxyMgr *)This;
    LONG r = InterlockedDecrement(&m->refs);
    if (r) return (ULONG)r;
    AcquireSRWLockExclusive(&g_pm_lock);
    BOOL last = m->refs == 0 && !m->dead;
    if (last) {
        m->dead = TRUE;
        for (ProxyMgr **pp = &g_proxies; *pp; pp = &(*pp)->next)
            if (*pp == m) { *pp = m->next; break; }
    }
    ReleaseSRWLockExclusive(&g_pm_lock);
    if (last) pm_destroy(m);
    return 0;
}

/* an interface proxy for iid with the object's ipid; returns the
 * interface, counted as one reference on the proxy manager */
static HRESULT pm_add_interface(ProxyMgr *m, REFIID iid, const GUID *ipid, void **ppv)
{
    AcquireSRWLockExclusive(&m->lock);
    for (PIfc *i = m->ifcs; i; i = i->next)
        if (IsEqualIID(&i->iid, iid)) {
            *ppv = i->iface;
            ReleaseSRWLockExclusive(&m->lock);
            pm_addref(&m->unk);
            return S_OK;
        }
    ReleaseSRWLockExclusive(&m->lock);
    PIfc *n = mem_zalloc(sizeof *n);
    if (!n) return E_OUTOFMEMORY;
    n->iid = *iid;
    n->ipid = *ipid;
    HRESULT hr = S_OK;
    if (IsEqualIID(iid, &IID_IUnknown)) {
        n->iface = &m->unk;
        pm_addref(&m->unk);
    } else if (IsEqualIID(iid, &IID_IClassFactory)) {
        n->iface = &m->cf_vtbl;
        pm_addref(&m->unk);
    } else {
        IPSFactoryBuffer *f = 0;
        hr = ps_factory(iid, &f);
        if (SUCCEEDED(hr)) {
            hr = f->lpVtbl->CreateProxy(f, &m->unk, iid, &n->buf, &n->iface);   /* AddRefs the proxy manager */
            f->lpVtbl->Release(f);
        }
        if (SUCCEEDED(hr) && !(n->chan = chan_new(m->pid, ipid))) hr = E_OUTOFMEMORY;
        if (SUCCEEDED(hr)) hr = n->buf->lpVtbl->Connect(n->buf, &n->chan->iface);
        if (FAILED(hr)) {
            dbg("no proxy for an interface:", hr);
            if (n->buf) {
                if (n->iface) pm_release(&m->unk);
                n->buf->lpVtbl->Release(n->buf);
            }
            if (n->chan) n->chan->iface.lpVtbl->Release(&n->chan->iface);
            mem_free(n);
            return hr == REGDB_E_IIDNOTREG || hr == REGDB_E_CLASSNOTREG ? E_NOINTERFACE : hr;
        }
    }
    AcquireSRWLockExclusive(&m->lock);
    PIfc *dup = m->ifcs;
    while (dup && !IsEqualIID(&dup->iid, iid)) dup = dup->next;
    if (!dup) { n->next = m->ifcs; m->ifcs = n; }
    ReleaseSRWLockExclusive(&m->lock);
    if (dup) {                                     /* another thread made it first */
        pm_release(&m->unk);
        if (n->buf) { n->buf->lpVtbl->Disconnect(n->buf); n->buf->lpVtbl->Release(n->buf); }
        if (n->chan) n->chan->iface.lpVtbl->Release(&n->chan->iface);
        mem_free(n);
        *ppv = dup->iface;
        pm_addref(&m->unk);
        return S_OK;
    }
    *ppv = n->iface;
    return S_OK;
}

static HRESULT pm_remote_qi(ProxyMgr *m, REFIID riid, void **ppv);
static HRESULT STDMETHODCALLTYPE pm_qi(IUnknown *This, REFIID riid, void **ppv)
{
    ProxyMgr *m = (ProxyMgr *)This;
    if (!ppv) return E_POINTER;
    *ppv = 0;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_INovaProxyManager)) {
        *ppv = &m->unk;
        pm_addref(&m->unk);
        return S_OK;
    }
    if (IsEqualIID(riid, &IID_IMarshal)) return E_NOINTERFACE;     /* marshaled as the proxy it is */
    AcquireSRWLockShared(&m->lock);
    for (PIfc *i = m->ifcs; i; i = i->next)
        if (IsEqualIID(&i->iid, riid)) {
            *ppv = i->iface;
            ReleaseSRWLockShared(&m->lock);
            pm_addref(&m->unk);
            return S_OK;
        }
    ReleaseSRWLockShared(&m->lock);
    return pm_remote_qi(m, riid, ppv);
}

/* ask the object for an interface; on success the proxy manager has it */
static HRESULT pm_remote_qi(ProxyMgr *m, REFIID riid, void **ppv)
{
    SmallMsg q;
    CopyMemory(q.b, &m->oid, 8);
    CopyMemory(q.b + 8, riid, 16);
    BYTE *reply = 0;
    DWORD rlen = 0;
    HRESULT rhr = S_OK, hr = transact(m->pid, (BYTE *)&q, M_REMQI, 24, FALSE, &reply, &rlen, &rhr);
    if (FAILED(hr)) return hr;
    if (SUCCEEDED(rhr) && rlen >= 16) rhr = pm_add_interface(m, riid, (GUID *)(reply + HDR), ppv);
    else if (SUCCEEDED(rhr)) rhr = RPC_E_INVALID_OBJREF;
    mem_free(reply);
    return rhr;
}
static const IUnknownVtbl g_pm_vtbl = { pm_qi, pm_addref, pm_release };

/* the proxy manager for an object of another process (found, or made),
 * AddRef'd */
static ProxyMgr *pm_get(DWORD pid, ULONGLONG oid)
{
    AcquireSRWLockExclusive(&g_pm_lock);
    ProxyMgr *m = g_proxies;
    while (m && (m->pid != pid || m->oid != oid || m->dead)) m = m->next;
    if (m) InterlockedIncrement(&m->refs);
    else if ((m = mem_zalloc(sizeof *m))) {
        m->unk.lpVtbl = &g_pm_vtbl;
        m->cf_vtbl = &g_pcf_vtbl;
        m->refs = 1;
        m->pid = pid;
        m->oid = oid;
        InitializeSRWLock(&m->lock);
        m->next = g_proxies;
        g_proxies = m;
    }
    ReleaseSRWLockExclusive(&g_pm_lock);
    return m;
}

static HRESULT remote_addref(DWORD pid, ULONGLONG oid, DWORD n)
{
    SmallMsg q;
    DWORD z = 0;
    CopyMemory(q.b, &oid, 8);
    CopyMemory(q.b + 8, &n, 4);
    CopyMemory(q.b + 12, &z, 4);
    HRESULT rhr = S_OK, hr = transact(pid, (BYTE *)&q, M_ADDREF, 16, FALSE, 0, 0, &rhr);
    return FAILED(hr) ? hr : rhr;
}

/* ---- marshaling ---------------------------------------------------------- */
static ProxyMgr *as_proxy(IUnknown *p)
{
    ProxyMgr *m = 0;
    if (p && SUCCEEDED(p->lpVtbl->QueryInterface(p, &IID_INovaProxyManager, (void **)&m))) return m;
    return NULL;
}

static HRESULT std_marshal(IStream *s, REFIID riid, IUnknown *p, DWORD flags)
{
    ProxyMgr *pm = as_proxy(p);
    if (pm) {                                      /* a proxy: the OBJREF names the object's own process */
        GUID ipid = { 0 };
        BOOL have = FALSE;
        HRESULT hr = S_OK;
        for (int pass = 0; pass < 2 && !have && SUCCEEDED(hr); pass++) {
            AcquireSRWLockShared(&pm->lock);
            for (PIfc *i = pm->ifcs; i; i = i->next)
                if (IsEqualIID(&i->iid, riid)) { ipid = i->ipid; have = TRUE; }
            ReleaseSRWLockShared(&pm->lock);
            if (!have && !pass) {
                void *itf = 0;
                hr = pm_remote_qi(pm, riid, &itf);
                if (SUCCEEDED(hr)) pm_release(&pm->unk);
            }
        }
        if (SUCCEEDED(hr) && !have) hr = E_NOINTERFACE;
        DWORD refs = (flags & (MSHLFLAGS_TABLESTRONG | MSHLFLAGS_TABLEWEAK)) ? 0 : 1;
        if (SUCCEEDED(hr) && refs) hr = remote_addref(pm->pid, pm->oid, refs);
        if (SUCCEEDED(hr)) hr = write_std_objref(s, riid, 0, refs, pm->pid, pm->oid, &ipid);
        pm_release(&pm->unk);
        return hr;
    }
    IUnknown *unk = 0;
    HRESULT hr = p->lpVtbl->QueryInterface(p, &IID_IUnknown, (void **)&unk);
    if (FAILED(hr)) return hr;
    exporter_start();
    LONG refs = (flags & (MSHLFLAGS_TABLESTRONG | MSHLFLAGS_TABLEWEAK)) ? 0 : 1;
    LONG table = (flags & MSHLFLAGS_TABLESTRONG) ? 1 : 0;
    StubMgr *m = stub_for_object(unk, refs, table);
    unk->lpVtbl->Release(unk);
    if (!m) return E_OUTOFMEMORY;
    GUID ipid;
    hr = stub_interface(m, riid, &ipid);
    if (SUCCEEDED(hr))
        hr = write_std_objref(s, riid, table ? SORF_TABLESTRONG : 0, (DWORD)refs, GetCurrentProcessId(), m->oid, &ipid);
    if (FAILED(hr)) stub_release(m, refs, table);
    return hr;
}

static BOOL custom_marshaler(IUnknown *p, REFIID riid, DWORD ctx, void *pvctx, DWORD flags, IMarshal **out, CLSID *clsid)
{
    IMarshal *mar = 0;
    if (FAILED(p->lpVtbl->QueryInterface(p, &IID_IMarshal, (void **)&mar))) return FALSE;
    if (FAILED(mar->lpVtbl->GetUnmarshalClass(mar, riid, p, ctx, pvctx, flags, clsid)) || IsEqualCLSID(clsid, &CLSID_StdMarshal)) {
        mar->lpVtbl->Release(mar);
        return FALSE;
    }
    *out = mar;
    return TRUE;
}

WINOLEAPI_(HRESULT) CoMarshalInterface(IStream *s, REFIID riid, IUnknown *p, DWORD ctx, void *pvctx, DWORD flags)
{
    if (!s || !riid || !p) return E_INVALIDARG;
    IMarshal *mar;
    CLSID clsid;
    if (custom_marshaler(p, riid, ctx, pvctx, flags, &mar, &clsid)) {
        BYTE b[48];
        DWORD v = OBJREF_SIGNATURE, size = 0, zero = 0;
        mar->lpVtbl->GetMarshalSizeMax(mar, riid, p, ctx, pvctx, flags, &size);
        CopyMemory(b, &v, 4);
        v = OBJREF_CUSTOM;
        CopyMemory(b + 4, &v, 4);
        CopyMemory(b + 8, riid, 16);
        CopyMemory(b + 24, &clsid, 16);
        CopyMemory(b + 40, &zero, 4);
        CopyMemory(b + 44, &size, 4);
        HRESULT hr = s->lpVtbl->Write(s, b, 48, 0);
        if (SUCCEEDED(hr)) hr = mar->lpVtbl->MarshalInterface(mar, s, riid, p, ctx, pvctx, flags);
        mar->lpVtbl->Release(mar);
        return hr;
    }
    HRESULT hr = std_marshal(s, riid, p, flags);
    if (FAILED(hr)) dbg("CoMarshalInterface failed:", hr);
    return hr;
}

WINOLEAPI_(HRESULT) CoGetMarshalSizeMax(ULONG *size, REFIID riid, IUnknown *p, DWORD ctx, void *pvctx, DWORD flags)
{
    if (!size || !p) return E_INVALIDARG;
    IMarshal *mar;
    CLSID clsid;
    if (custom_marshaler(p, riid, ctx, pvctx, flags, &mar, &clsid)) {
        DWORD n = 0;
        HRESULT hr = mar->lpVtbl->GetMarshalSizeMax(mar, riid, p, ctx, pvctx, flags, &n);
        mar->lpVtbl->Release(mar);
        *size = n + 48;
        return hr;
    }
    *size = std_objref_size();
    return S_OK;
}

static HRESULT std_unmarshal_objref(const ObjRef *o, REFIID riid, void **ppv)
{
    const IID *want = riid && !IsEqualIID(riid, &GUID_NULL) ? riid : &o->iid;
    DWORD pid = (DWORD)o->oxid;
    if (pid == GetCurrentProcessId()) {            /* our own object */
        AcquireSRWLockShared(&g_exp_lock);
        StubMgr *m = stub_by_oid(o->oid);
        IUnknown *obj = m ? m->obj : 0;
        if (obj) obj->lpVtbl->AddRef(obj);
        ReleaseSRWLockShared(&g_exp_lock);
        if (!obj) return RPC_E_INVALID_OBJREF;
        HRESULT hr = obj->lpVtbl->QueryInterface(obj, want, ppv);
        obj->lpVtbl->Release(obj);
        if (o->refs) stub_release_oid(o->oid, (LONG)o->refs, 0);
        return hr;
    }
    ProxyMgr *m = pm_get(pid, o->oid);
    if (!m) return E_OUTOFMEMORY;
    HRESULT hr = S_OK;
    if (o->refs) InterlockedExchangeAdd(&m->remote_refs, (LONG)o->refs);
    else if (SUCCEEDED(hr = remote_addref(pid, o->oid, 1))) InterlockedIncrement(&m->remote_refs);
    void *itf = 0;
    if (SUCCEEDED(hr)) hr = pm_add_interface(m, &o->iid, &o->ipid, &itf);
    if (SUCCEEDED(hr)) {
        if (IsEqualIID(want, &o->iid)) { *ppv = itf; itf = 0; }
        else hr = pm_qi(&m->unk, want, ppv);
        if (itf) pm_release(&m->unk);
    }
    pm_release(&m->unk);
    return hr;
}

WINOLEAPI_(HRESULT) CoUnmarshalInterface(IStream *s, REFIID riid, void **ppv)
{
    if (!s || !ppv) return E_INVALIDARG;
    *ppv = 0;
    ObjRef o;
    HRESULT hr = read_objref(s, &o);
    if (FAILED(hr)) return hr;
    if (o.flags == OBJREF_CUSTOM) {
        IMarshal *mar = 0;
        hr = CoCreateInstance(&o.clsid, 0, CLSCTX_INPROC_SERVER, &IID_IMarshal, (void **)&mar);
        if (FAILED(hr)) return hr;
        hr = mar->lpVtbl->UnmarshalInterface(mar, s, riid && !IsEqualIID(riid, &GUID_NULL) ? riid : &o.iid, ppv);
        mar->lpVtbl->Release(mar);
        return hr;
    }
    hr = std_unmarshal_objref(&o, riid, ppv);
    if (FAILED(hr)) dbg("CoUnmarshalInterface failed:", hr);
    return hr;
}

WINOLEAPI_(HRESULT) CoReleaseMarshalData(IStream *s)
{
    if (!s) return E_INVALIDARG;
    ObjRef o;
    HRESULT hr = read_objref(s, &o);
    if (FAILED(hr)) return hr;
    if (o.flags == OBJREF_CUSTOM) {
        IMarshal *mar = 0;
        hr = CoCreateInstance(&o.clsid, 0, CLSCTX_INPROC_SERVER, &IID_IMarshal, (void **)&mar);
        if (FAILED(hr)) return hr;
        hr = mar->lpVtbl->ReleaseMarshalData(mar, s);
        mar->lpVtbl->Release(mar);
        return hr;
    }
    DWORD pid = (DWORD)o.oxid;
    LONG table = (o.std_flags & SORF_TABLESTRONG) ? 1 : 0;
    if (pid == GetCurrentProcessId()) {
        stub_release_oid(o.oid, (LONG)o.refs, table);
        return S_OK;
    }
    if (o.refs) {
        SmallMsg q;
        DWORD z = 0;
        CopyMemory(q.b, &o.oid, 8);
        CopyMemory(q.b + 8, &o.refs, 4);
        CopyMemory(q.b + 12, &z, 4);
        transact(pid, (BYTE *)&q, M_RELEASE, 16, FALSE, 0, 0, 0);
    }
    return S_OK;
}

WINOLEAPI_(HRESULT) CoDisconnectObject(IUnknown *p, DWORD reserved)
{
    (void)reserved;
    if (!p) return E_INVALIDARG;
    IUnknown *unk = 0;
    if (FAILED(p->lpVtbl->QueryInterface(p, &IID_IUnknown, (void **)&unk))) return S_OK;
    AcquireSRWLockExclusive(&g_exp_lock);
    StubMgr *m = g_stubs;
    while (m && (m->obj != unk || m->dead)) m = m->next;
    if (m) {
        m->dead = TRUE;
        for (StubMgr **pp = &g_stubs; *pp; pp = &(*pp)->next)
            if (*pp == m) { *pp = m->next; break; }
    }
    ReleaseSRWLockExclusive(&g_exp_lock);
    unk->lpVtbl->Release(unk);
    if (m) destroy_stub_mgr(m);
    return S_OK;
}

/* ---- the standard marshaler as an object (CoGetStandardMarshal,
 * CoGetStdMarshalEx) --------------------------------------------------------- */
typedef struct { IMarshal iface; IUnknown inner; LONG refs; IUnknown *outer; IUnknown *obj; } StdMarshal;
#define SM_FROM_INNER(p) ((StdMarshal *)((BYTE *)(p) - offsetof(StdMarshal, inner)))
static HRESULT STDMETHODCALLTYPE smi_qi(IUnknown *This, REFIID riid, void **ppv)
{
    StdMarshal *m = SM_FROM_INNER(This);
    if (IsEqualIID(riid, &IID_IUnknown)) *ppv = &m->inner;
    else if (IsEqualIID(riid, &IID_IMarshal)) *ppv = &m->iface;
    else { *ppv = 0; return E_NOINTERFACE; }
    ((IUnknown *)*ppv)->lpVtbl->AddRef((IUnknown *)*ppv);
    return S_OK;
}
static ULONG STDMETHODCALLTYPE smi_addref(IUnknown *This) { return (ULONG)InterlockedIncrement(&SM_FROM_INNER(This)->refs); }
static ULONG STDMETHODCALLTYPE smi_release(IUnknown *This)
{
    StdMarshal *m = SM_FROM_INNER(This);
    LONG r = InterlockedDecrement(&m->refs);
    if (!r) { if (m->obj) m->obj->lpVtbl->Release(m->obj); mem_free(m); }
    return (ULONG)r;
}
static const IUnknownVtbl g_smi_vtbl = { smi_qi, smi_addref, smi_release };
static HRESULT STDMETHODCALLTYPE sm_qi(IMarshal *This, REFIID riid, void **ppv) { StdMarshal *m = (StdMarshal *)This; return m->outer->lpVtbl->QueryInterface(m->outer, riid, ppv); }
static ULONG STDMETHODCALLTYPE sm_addref(IMarshal *This) { StdMarshal *m = (StdMarshal *)This; return m->outer->lpVtbl->AddRef(m->outer); }
static ULONG STDMETHODCALLTYPE sm_release(IMarshal *This) { StdMarshal *m = (StdMarshal *)This; return m->outer->lpVtbl->Release(m->outer); }
static HRESULT STDMETHODCALLTYPE sm_class(IMarshal *This, REFIID riid, void *pv, DWORD ctx, void *pvctx, DWORD flags, CLSID *clsid)
{ (void)This; (void)riid; (void)pv; (void)ctx; (void)pvctx; (void)flags; *clsid = CLSID_StdMarshal; return S_OK; }
static HRESULT STDMETHODCALLTYPE sm_size(IMarshal *This, REFIID riid, void *pv, DWORD ctx, void *pvctx, DWORD flags, DWORD *size)
{ (void)This; (void)riid; (void)pv; (void)ctx; (void)pvctx; (void)flags; *size = std_objref_size(); return S_OK; }
static HRESULT STDMETHODCALLTYPE sm_marshal(IMarshal *This, IStream *s, REFIID riid, void *pv, DWORD ctx, void *pvctx, DWORD flags)
{
    StdMarshal *m = (StdMarshal *)This;
    (void)ctx; (void)pvctx;
    IUnknown *p = pv ? (IUnknown *)pv : m->obj ? m->obj : m->outer;
    return std_marshal(s, riid, p, flags);
}
static HRESULT STDMETHODCALLTYPE sm_unmarshal(IMarshal *This, IStream *s, REFIID riid, void **ppv)
{
    (void)This;
    ObjRef o;
    HRESULT hr = read_objref(s, &o);
    if (FAILED(hr)) return hr;
    if (o.flags != OBJREF_STANDARD) return RPC_E_INVALID_OBJREF;
    return std_unmarshal_objref(&o, riid, ppv);
}
static HRESULT STDMETHODCALLTYPE sm_release_data(IMarshal *This, IStream *s) { (void)This; return CoReleaseMarshalData(s); }
static HRESULT STDMETHODCALLTYPE sm_disconnect(IMarshal *This, DWORD r) { StdMarshal *m = (StdMarshal *)This; return CoDisconnectObject(m->obj ? m->obj : m->outer, r); }
static const IMarshalVtbl g_sm_vtbl = { sm_qi, sm_addref, sm_release, sm_class, sm_size, sm_marshal, sm_unmarshal, sm_release_data, sm_disconnect };

static StdMarshal *std_marshal_new(IUnknown *outer, IUnknown *obj)
{
    StdMarshal *m = mem_zalloc(sizeof *m);
    if (!m) return NULL;
    m->iface.lpVtbl = &g_sm_vtbl;
    m->inner.lpVtbl = &g_smi_vtbl;
    m->refs = 1;
    m->outer = outer ? outer : &m->inner;
    m->obj = obj;
    if (obj) obj->lpVtbl->AddRef(obj);
    return m;
}

WINOLEAPI_(HRESULT) CoGetStandardMarshal(REFIID riid, IUnknown *p, DWORD ctx, void *pvctx, DWORD flags, IMarshal **out)
{
    (void)riid; (void)ctx; (void)pvctx; (void)flags;
    if (!out) return E_INVALIDARG;
    StdMarshal *m = std_marshal_new(0, p);
    if (!m) { *out = 0; return E_OUTOFMEMORY; }
    *out = &m->iface;
    m->refs++;                                      /* (through the inner object: it is its own outer) */
    smi_release(&m->inner);
    return S_OK;
}

WINOLEAPI_(HRESULT) CoGetStdMarshalEx(IUnknown *outer, DWORD flags, IUnknown **out)
{
    (void)flags;
    if (!out) return E_INVALIDARG;
    *out = 0;
    if (!outer) return E_INVALIDARG;
    StdMarshal *m = std_marshal_new(outer, 0);
    if (!m) return E_OUTOFMEMORY;
    *out = &m->inner;
    return S_OK;
}

/* ---- serving requests ---------------------------------------------------- */
typedef struct SConn { HANDLE pipe, ev, wev; SRWLOCK wlock; LONG refs; } SConn;
static void sconn_release(SConn *c)
{
    if (InterlockedDecrement(&c->refs)) return;
    CloseHandle(c->pipe);
    CloseHandle(c->ev);
    CloseHandle(c->wev);
    mem_free(c);
}

static void reply(SConn *c, BYTE *msg, DWORD type, DWORD len, HRESULT hr)
{
    MsgHdr *h = (MsgHdr *)msg;
    h->magic = OLE_MAGIC;
    h->type = type | M_REPLY;
    h->len = len;
    h->hr = hr;
    AcquireSRWLockExclusive(&c->wlock);
    pipe_io(c->pipe, c->wev, msg, HDR + len, TRUE);
    ReleaseSRWLockExclusive(&c->wlock);
}

/* a reply carrying an interface pointer, marshaled for the caller */
static void reply_objref(SConn *c, DWORD type, IUnknown *p, REFIID iid, HRESULT hr)
{
    IStream *s = 0;
    if (SUCCEEDED(hr) && SUCCEEDED(hr = CreateStreamOnHGlobal(0, TRUE, &s))) {
        s->lpVtbl->Write(s, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", HDR, 0);     /* room for the header */
        hr = CoMarshalInterface(s, iid, p, MSHCTX_LOCAL, 0, MSHLFLAGS_NORMAL);
    }
    if (SUCCEEDED(hr)) {
        HGLOBAL g;
        STATSTG st;
        GetHGlobalFromStream(s, &g);
        s->lpVtbl->Stat(s, &st, 1);
        reply(c, GlobalLock(g), type, (DWORD)st.cbSize.QuadPart - HDR, S_OK);
        GlobalUnlock(g);
    } else {
        MsgHdr h;
        reply(c, (BYTE *)&h, type, 0, hr);
    }
    if (s) s->lpVtbl->Release(s);
}

/* the server's side of a channel: GetBuffer makes the reply */
typedef struct { IRpcChannelBuffer iface; LONG refs; } SChan;
static HRESULT STDMETHODCALLTYPE sch_getbuffer(IRpcChannelBuffer *This, RPCOLEMESSAGE *msg, REFIID riid)
{
    (void)This; (void)riid;
    BYTE *b = mem_alloc(HDR + msg->cbBuffer);
    if (!b) return E_OUTOFMEMORY;
    ((MsgHdr *)b)->hr = (LONG)REPLY_TAG;
    msg->Buffer = b + HDR;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE sch_sendreceive(IRpcChannelBuffer *This, RPCOLEMESSAGE *msg, ULONG *status)
{ (void)This; (void)msg; if (status) *status = 0; return E_UNEXPECTED; }
static HRESULT STDMETHODCALLTYPE sch_freebuffer(IRpcChannelBuffer *This, RPCOLEMESSAGE *msg)
{
    (void)This;
    BYTE *p = msg->Buffer;
    if (p && IS_REPLY(p)) mem_free(p - HDR);
    msg->Buffer = 0;
    return S_OK;
}
static ULONG STDMETHODCALLTYPE sch_addref(IRpcChannelBuffer *This) { return (ULONG)InterlockedIncrement(&((SChan *)This)->refs); }
static ULONG STDMETHODCALLTYPE sch_release(IRpcChannelBuffer *This) { return (ULONG)InterlockedDecrement(&((SChan *)This)->refs); }
static const IRpcChannelBufferVtbl g_schan_vtbl = {
    ch_qi, sch_addref, sch_release, sch_getbuffer, sch_sendreceive, sch_freebuffer, ch_getdestctx, ch_isconnected,
};

typedef struct {
    SConn *conn;
    MsgHdr hdr;
    BYTE *msg;                                     /* HDR, then the payload */
    StubMgr *mgr;
    HRESULT hr;
    /* M_CALL */
    IRpcStubBuffer *stub;
    BYTE *out;                                     /* the reply's buffer (REPLY_TAG'd), or NULL */
    DWORD out_len;
    /* M_REMQI */
    GUID ipid;
    /* M_ACTIVATE */
    IUnknown *cls;
} Work;

static void do_call(void *arg)
{
    Work *w = arg;
    CallHdr *ch = (CallHdr *)(w->msg + HDR);
    BYTE *data = w->msg + CALL_PREFIX;
    SChan chan = { { &g_schan_vtbl }, 1 };
    RPCOLEMESSAGE m;
    ZeroMemory(&m, sizeof m);
    m.Buffer = data;
    m.cbBuffer = w->hdr.len - (DWORD)sizeof(CallHdr);
    m.iMethod = ch->method;
    m.dataRepresentation = ch->datarep;
    w->hr = w->stub->lpVtbl->Invoke(w->stub, &m, &chan.iface);
    if (m.Buffer != data && m.Buffer && IS_REPLY(m.Buffer)) {
        w->out = (BYTE *)m.Buffer - HDR;
        w->out_len = m.cbBuffer;
        if (FAILED(w->hr)) { mem_free(w->out); w->out = 0; }
    }
}

static void do_remqi(void *arg)
{
    Work *w = arg;
    IID iid;
    CopyMemory(&iid, w->msg + HDR + 8, 16);
    w->hr = stub_interface(w->mgr, &iid, &w->ipid);
}

static void do_cf_create(void *arg)
{
    Work *w = arg;
    IID iid;
    CopyMemory(&iid, w->msg + HDR + 8, 16);
    IClassFactory *cf = 0;
    IUnknown *obj = 0;
    w->hr = w->mgr->obj->lpVtbl->QueryInterface(w->mgr->obj, &IID_IClassFactory, (void **)&cf);
    if (SUCCEEDED(w->hr)) {
        w->hr = cf->lpVtbl->CreateInstance(cf, 0, &iid, (void **)&obj);
        cf->lpVtbl->Release(cf);
    }
    reply_objref(w->conn, M_CF_CREATE, obj, &iid, w->hr);
    if (obj) obj->lpVtbl->Release(obj);
}

static void do_cf_lock(void *arg)
{
    Work *w = arg;
    DWORD lock;
    CopyMemory(&lock, w->msg + HDR + 8, 4);
    IClassFactory *cf = 0;
    w->hr = w->mgr->obj->lpVtbl->QueryInterface(w->mgr->obj, &IID_IClassFactory, (void **)&cf);
    if (SUCCEEDED(w->hr)) {
        w->hr = cf->lpVtbl->LockServer(cf, lock != 0);
        cf->lpVtbl->Release(cf);
    }
}

static void do_activate(void *arg)
{
    Work *w = arg;
    IID iid;
    DWORD mode;
    CopyMemory(&iid, w->msg + HDR + 16, 16);
    CopyMemory(&mode, w->msg + HDR + 32, 4);
    IUnknown *obj = 0;
    if (mode == OLE_ACTIVATE_CREATE) {
        IClassFactory *cf = 0;
        w->hr = w->cls->lpVtbl->QueryInterface(w->cls, &IID_IClassFactory, (void **)&cf);
        if (SUCCEEDED(w->hr)) {
            w->hr = cf->lpVtbl->CreateInstance(cf, 0, &iid, (void **)&obj);
            cf->lpVtbl->Release(cf);
        }
    } else
        w->hr = w->cls->lpVtbl->QueryInterface(w->cls, &iid, (void **)&obj);
    reply_objref(w->conn, M_ACTIVATE, obj, &iid, w->hr);
    if (obj) obj->lpVtbl->Release(obj);
}

/* the request's stub manager, held (and its object AddRef'd) */
static void hold_mgr(Work *w, StubMgr *m)
{
    if (!m) return;
    InterlockedIncrement(&m->holds);
    m->obj->lpVtbl->AddRef(m->obj);
    w->mgr = m;
}
static StubMgr *work_mgr(Work *w, ULONGLONG oid)
{
    AcquireSRWLockShared(&g_exp_lock);
    hold_mgr(w, stub_by_oid(oid));
    ReleaseSRWLockShared(&g_exp_lock);
    return w->mgr;
}

static DWORD WINAPI worker(void *arg)
{
    Work *w = arg;
    SConn *c = w->conn;
    DWORD type = w->hdr.type;
    MsgHdr small;
    switch (type) {
    case M_CALL: {
        if (w->hdr.len < sizeof(CallHdr)) { reply(c, (BYTE *)&small, type, 0, RPC_E_INVALID_OBJREF); break; }
        CallHdr *ch = (CallHdr *)(w->msg + HDR);
        Ifc *ifc = 0;
        AcquireSRWLockShared(&g_exp_lock);
        StubMgr *m = stub_by_ipid(&ch->ipid, &ifc);
        if (m && ifc->stub) {
            hold_mgr(w, m);
            w->stub = ifc->stub;
            w->stub->lpVtbl->AddRef(w->stub);
        }
        ReleaseSRWLockShared(&g_exp_lock);
        if (!m) { reply(c, (BYTE *)&small, type, 0, RPC_E_DISCONNECTED); break; }
        if (!w->stub) { reply(c, (BYTE *)&small, type, 0, RPC_E_INVALIDMETHOD); break; }
        if (!apt_run(&m->apt, do_call, w)) w->hr = RPC_E_DISCONNECTED;
        w->stub->lpVtbl->Release(w->stub);
        if (w->out) { reply(c, w->out, type, w->out_len, S_OK); mem_free(w->out); }
        else reply(c, (BYTE *)&small, type, 0, FAILED(w->hr) ? w->hr : RPC_E_INVALID_OBJREF);
        break;
    }
    case M_REMQI: {
        ULONGLONG oid;
        CopyMemory(&oid, w->msg + HDR, 8);
        if (w->hdr.len < 24 || !work_mgr(w, oid)) { reply(c, (BYTE *)&small, type, 0, RPC_E_DISCONNECTED); break; }
        if (!apt_run(&w->mgr->apt, do_remqi, w)) w->hr = RPC_E_DISCONNECTED;
        SmallMsg r;
        CopyMemory(r.b, &w->ipid, 16);
        reply(c, (BYTE *)&r, type, SUCCEEDED(w->hr) ? 16 : 0, w->hr);
        break;
    }
    case M_ADDREF: {
        ULONGLONG oid;
        DWORD n;
        CopyMemory(&oid, w->msg + HDR, 8);
        CopyMemory(&n, w->msg + HDR + 8, 4);
        HRESULT hr = RPC_E_DISCONNECTED;
        AcquireSRWLockExclusive(&g_exp_lock);
        StubMgr *m = stub_by_oid(oid);
        if (m) { m->refs += (LONG)n; hr = S_OK; }
        ReleaseSRWLockExclusive(&g_exp_lock);
        reply(c, (BYTE *)&small, type, 0, hr);
        break;
    }
    case M_CF_CREATE: {
        ULONGLONG oid;
        CopyMemory(&oid, w->msg + HDR, 8);
        if (w->hdr.len < 24 || !work_mgr(w, oid)) { reply(c, (BYTE *)&small, type, 0, RPC_E_DISCONNECTED); break; }
        if (!apt_run(&w->mgr->apt, do_cf_create, w)) reply(c, (BYTE *)&small, type, 0, RPC_E_DISCONNECTED);
        break;
    }
    case M_CF_LOCK: {
        ULONGLONG oid;
        CopyMemory(&oid, w->msg + HDR, 8);
        if (w->hdr.len < 12 || !work_mgr(w, oid)) { reply(c, (BYTE *)&small, type, 0, RPC_E_DISCONNECTED); break; }
        if (!apt_run(&w->mgr->apt, do_cf_lock, w)) w->hr = RPC_E_DISCONNECTED;
        reply(c, (BYTE *)&small, type, 0, w->hr);
        break;
    }
    case M_ACTIVATE: {
        CLSID clsid;
        OleApt apt;
        if (w->hdr.len < 36) { reply(c, (BYTE *)&small, type, 0, RPC_E_INVALID_OBJREF); break; }
        CopyMemory(&clsid, w->msg + HDR, 16);
        w->cls = ole_local_class(&clsid, &apt);
        if (!w->cls) { reply(c, (BYTE *)&small, type, 0, CO_E_SERVER_STOPPING); break; }
        if (!apt_run(&apt, do_activate, w)) reply(c, (BYTE *)&small, type, 0, RPC_E_DISCONNECTED);
        w->cls->lpVtbl->Release(w->cls);
        break;
    }
    default:
        reply(c, (BYTE *)&small, type, 0, RPC_E_INVALID_OBJREF);
    }
    if (w->mgr) { w->mgr->obj->lpVtbl->Release(w->mgr->obj); mgr_unhold(w->mgr); }
    mem_free(w->msg);
    mem_free(w);
    conns_close_all();
    sconn_release(c);
    return 0;
}

static DWORD WINAPI serve_conn(void *arg)
{
    SConn *c = arg;
    for (;;) {
        MsgHdr h;
        BYTE *msg = 0;
        if (!read_msg(c->pipe, c->ev, &h, &msg, FALSE)) break;
        if (h.type == M_RELEASE) {                  /* in order with the connection's other requests */
            ULONGLONG oid;
            DWORD n = 0;
            if (h.len >= 12) {
                CopyMemory(&oid, msg + HDR, 8);
                CopyMemory(&n, msg + HDR + 8, 4);
                stub_release_oid(oid, (LONG)n, 0);
            }
            mem_free(msg);
            SmallMsg r;                             /* the object is gone once the caller hears back */
            reply(c, (BYTE *)&r, M_RELEASE, 0, S_OK);
            continue;
        }
        Work *w = mem_zalloc(sizeof *w);
        if (!w) { mem_free(msg); break; }
        w->conn = c;
        w->hdr = h;
        w->msg = msg;
        InterlockedIncrement(&c->refs);
        HANDLE t = CreateThread(0, 0, worker, w, 0, 0);
        if (t) CloseHandle(t);
        else { sconn_release(c); mem_free(msg); mem_free(w); }
    }
    DisconnectNamedPipe(c->pipe);
    sconn_release(c);
    return 0;
}

/* a pipe's listener: an instance always waits, and each connected one is
 * served by a thread of its own */
typedef struct Listener { WCHAR name[80]; volatile LONG stop; HANDLE thread; } Listener;

static HANDLE new_instance(const WCHAR *name)
{
    return CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                            PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, 0);
}

static DWORD WINAPI listen_thread(void *arg)
{
    Listener *l = arg;
    HANDLE ev = CreateEventW(0, TRUE, FALSE, 0);
    HANDLE h = new_instance(l->name);
    while (h != INVALID_HANDLE_VALUE && !l->stop) {
        OVERLAPPED ov;
        ZeroMemory(&ov, sizeof ov);
        ov.hEvent = ev;
        ResetEvent(ev);
        BOOL ok = ConnectNamedPipe(h, &ov);
        DWORD e = ok ? 0 : GetLastError(), n;
        if (!ok && e == ERROR_IO_PENDING) ok = GetOverlappedResult(h, &ov, &n, TRUE);
        else if (!ok && e == ERROR_PIPE_CONNECTED) ok = TRUE;
        HANDLE next = new_instance(l->name);        /* (made before this one is handed on: clients never miss) */
        if (ok && !l->stop) {
            SConn *c = mem_zalloc(sizeof *c);
            if (c) {
                c->pipe = h;
                c->ev = CreateEventW(0, TRUE, FALSE, 0);
                c->wev = CreateEventW(0, TRUE, FALSE, 0);
                InitializeSRWLock(&c->wlock);
                c->refs = 1;
                HANDLE t = CreateThread(0, 0, serve_conn, c, 0, 0);
                if (t) CloseHandle(t);
                else sconn_release(c);
            } else CloseHandle(h);
        } else CloseHandle(h);
        h = next;
    }
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    CloseHandle(ev);
    return 0;
}

static Listener *listen_on(const WCHAR *name)
{
    Listener *l = mem_zalloc(sizeof *l);
    if (!l) return NULL;
    put_s(l->name, name);
    l->thread = CreateThread(0, 0, listen_thread, l, 0, 0);
    if (!l->thread) { mem_free(l); return NULL; }
    return l;
}

static void exporter_start(void)
{
    static volatile LONG started;
    if (started || InterlockedExchange(&started, 1)) return;
    WCHAR name[64];
    process_pipe(GetCurrentProcessId(), name);
    listen_on(name);
}

void *ole_class_listen(REFCLSID clsid)
{
    WCHAR name[80];
    exporter_start();
    class_pipe(clsid, name);
    return listen_on(name);
}

void ole_class_unlisten(void *p)
{
    Listener *l = p;
    if (!l) return;
    l->stop = 1;
    HANDLE h = CreateFileW(l->name, GENERIC_READ | GENERIC_WRITE, 0, 0, OPEN_EXISTING, 0, 0);   /* wake its wait */
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (GetCurrentThreadId() != GetThreadId(l->thread)) WaitForSingleObject(l->thread, 2000);
    CloseHandle(l->thread);
    mem_free(l);
}

/* ---- activation ---------------------------------------------------------- */
/* the server's command line: LocalServer32 (expanded), then " -Embedding" */
static BOOL server_command(REFCLSID clsid, WCHAR *cmd, DWORD n)
{
    WCHAR key[100], raw[MAX_PATH * 2];
    WCHAR *p = put_s(key, L"CLSID\\");
    p += StringFromGUID2(clsid, p, 39) - 1;
    put_s(p, L"\\LocalServer32");
    DWORD size = sizeof raw;
    if (RegGetValueW(HKEY_CLASSES_ROOT, key, 0, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, 0, raw, &size) || !raw[0]) {
        size = sizeof raw;
        if (RegGetValueW(HKEY_CLASSES_ROOT, key, L"ServerExecutable", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, 0, raw, &size) || !raw[0])
            return FALSE;
    }
    DWORD len = ExpandEnvironmentStringsW(raw, cmd, n - 16);
    if (!len || len > n - 16) return FALSE;
    lstrcatW(cmd, L" -Embedding");
    return TRUE;
}

HRESULT ole_local_activate(REFCLSID clsid, DWORD mode, REFIID riid, void **ppv)
{
    WCHAR name[80];
    class_pipe(clsid, name);
    HANDLE h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, 0, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, 0);
    if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY) h = open_pipe(name, 5000);
    if (h == INVALID_HANDLE_VALUE) {                /* nobody serves it: start the server */
        WCHAR cmd[MAX_PATH * 2 + 16];
        if (!server_command(clsid, cmd, sizeof cmd / sizeof cmd[0])) return REGDB_E_CLASSNOTREG;
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof si);
        si.cb = sizeof si;
        if (!CreateProcessW(0, cmd, 0, 0, FALSE, 0, 0, 0, &si, &pi)) {
            dbg("cannot start the local server:", HRESULT_FROM_WIN32(GetLastError()));
            return CO_E_SERVER_EXEC_FAILURE_;
        }
        CloseHandle(pi.hThread);
        ULONGLONG until = GetTickCount64() + 60000;
        for (;;) {
            h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, 0, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, 0);
            if (h != INVALID_HANDLE_VALUE) break;
            if (GetLastError() == ERROR_PIPE_BUSY) { WaitNamedPipeW(name, 200); continue; }
            if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0 || GetTickCount64() > until) {
                CloseHandle(pi.hProcess);
                return CO_E_SERVER_EXEC_FAILURE_;
            }
            if (ole_thread_is_sta()) apt_pump();
            Sleep(10);
        }
        CloseHandle(pi.hProcess);
    }
    HANDLE ev = CreateEventW(0, TRUE, FALSE, 0);
    SmallMsg q;
    CopyMemory(q.b, clsid, 16);
    CopyMemory(q.b + 16, riid, 16);
    CopyMemory(q.b + 32, &mode, 4);
    BYTE *reply = 0;
    DWORD rlen = 0;
    HRESULT rhr = S_OK, hr = transact_on(h, ev, (BYTE *)&q, M_ACTIVATE, 36, FALSE, &reply, &rlen, &rhr);
    CloseHandle(ev);
    CloseHandle(h);
    if (FAILED(hr)) return CO_E_SERVER_EXEC_FAILURE_;
    if (SUCCEEDED(rhr)) rhr = unmarshal_reply(reply, rlen, riid, ppv);
    mem_free(reply);
    return rhr;
}
