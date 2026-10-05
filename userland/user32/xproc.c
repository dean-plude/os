/*
 * xproc.c — windows of other processes
 *
 * On Windows an HWND names the same window in every process: WebView2's
 * host parents the browser process's window in its own (SetParent), sizes
 * and shows it (SetWindowPos, ShowWindow) and sends it messages, and
 * Chromium's GPU process draws into it (GetDC, BitBlt).  Here each
 * process's windows live in its own user32, so a call on another process's
 * window travels to that process through the kernel (CTL_XSEND, um_gui.c)
 * and runs there, in the thread that owns the window, as a sent message
 * would: SetWindowPos on it sends it WM_WINDOWPOSCHANGING as on Windows.
 *
 * A window embedded in another process's keeps its own desktop window;
 * the desktop keeps it over the parent's client area, clipped to it
 * (CTL_EMBED).  The host tells the desktop where the parent is whenever
 * its windows move (embeds_follow).
 */
#include "u32.h"

#define XMSG_MAX (64 * 1024)

/* What travels: user32's own layout, the same in 32- and 64-bit processes */
enum { XOP_POST = 1, XOP_SEND, XOP_SETPOS, XOP_SHOW, XOP_GETLONG, XOP_SETLONG, XOP_FOCUS, XOP_ENABLE,
       XOP_CLASS, XOP_TEXT, XOP_THREAD, XOP_EMBEDDED };
typedef struct {
    UINT32 op, msg;
    UINT64 hwnd, wp, lp;
    INT32  a[8];
    UINT32 wide;                    /* (XOP_SEND) the sender's character set */
    UINT32 n;                       /* bytes of data after the header */
} XHdr;

typedef struct { UINT64 result; UINT32 len, room, wait, drop; } XRes;   /* CTL_XRESULT's block */

/* -----------------------------------------------------------------------
 * Sending
 * ----------------------------------------------------------------------- */
static UINT32 x_queue(HWND h, int answer, const XHdr *hd, const void *data)
{
    size_t n = sizeof(XHdr) + hd->n;
    BYTE *b = malloc(8 + n);
    if (!b) return 0;
    ((UINT32 *)b)[0] = answer ? 1 : 0;
    ((UINT32 *)b)[1] = (UINT32)n;
    memcpy(b + 8, hd, sizeof(XHdr));
    if (hd->n) memcpy(b + 8 + sizeof(XHdr), data, hd->n);
    UINT32 seq = (UINT32)NtNovaGuiCtl(0, CTL_XSEND, (ULONG_PTR)h, b);
    free(b);
    return seq;
}

/* Run @hd in @h's process and wait for the answer, answering what other
 * processes send us meanwhile (so two programs sending each other never
 * stall).  @out gets up to @room bytes of the answer's data.  *failed:
 * 1 no such window (or its process ended), 2 timed out */
static LRESULT x_call(HWND h, XHdr *hd, const void *data, DWORD timeout, void *out, UINT32 room, UINT32 *outlen, int *failed)
{
    int dummy;
    if (!failed) failed = &dummy;
    *failed = 0;
    if (outlen) *outlen = 0;
    if (hd->n > XMSG_MAX - sizeof(XHdr)) { *failed = 1; return 0; }
    UINT32 seq = x_queue(h, 1, hd, data);
    if (!seq) { *failed = 1; SetLastError(ERROR_INVALID_WINDOW_HANDLE); return 0; }
    XRes *r = malloc(sizeof(XRes) + room + 1);
    if (!r) { *failed = 1; return 0; }
    ULONGLONG until = timeout == INFINITE ? ~0ULL : GetTickCount64() + timeout;
    LRESULT res = 0;
    for (;;) {
        memset(r, 0, sizeof(*r));
        r->room = room;
        r->wait = 20;
        long k = (long)NtNovaGuiCtl(0, CTL_XRESULT, seq, r);
        if (k == 1) {
            res = (LRESULT)r->result;
            if (out && r->len <= room) { memcpy(out, r + 1, r->len); if (outlen) *outlen = r->len; }
            break;
        }
        if (k == 2) { *failed = 1; SetLastError(ERROR_INVALID_WINDOW_HANDLE); break; }
        if (k == 3) x_claim();
        process_sent();
        if (GetTickCount64() >= until) {
            r->drop = 1;
            NtNovaGuiCtl(0, CTL_XRESULT, seq, r);
            *failed = 2;
            SetLastError(ERROR_TIMEOUT);
            break;
        }
    }
    free(r);
    return res;
}

static void hdr(XHdr *hd, UINT op, HWND h)
{
    memset(hd, 0, sizeof(*hd));
    hd->op = op;
    hd->hwnd = (UINT64)(ULONG_PTR)h;
}

BOOL x_post(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    XHdr hd;
    hdr(&hd, XOP_POST, h);
    hd.msg = msg; hd.wp = (UINT64)wp; hd.lp = (UINT64)(LONG_PTR)lp;
    if (x_queue(h, 0, &hd, NULL)) return TRUE;
    SetLastError(ERROR_INVALID_WINDOW_HANDLE);
    return FALSE;
}

/* Messages whose parameters point into the sender's memory: only the
 * ones carried here (text, WM_COPYDATA) can cross */
static int pointer_msg(UINT m)
{
    switch (m) {
    case WM_CREATE: case WM_NCCREATE: case WM_WINDOWPOSCHANGING: case WM_WINDOWPOSCHANGED: case WM_NCCALCSIZE:
    case WM_GETMINMAXINFO: case WM_STYLECHANGING: case WM_STYLECHANGED: case WM_DRAWITEM: case WM_MEASUREITEM:
    case WM_DELETEITEM: case WM_COMPAREITEM: case WM_NOTIFY: case WM_HELP: case WM_MDICREATE: case WM_GETDLGCODE:
        return 1;
    }
    return 0;
}

LRESULT x_send(HWND h, UINT msg, WPARAM wp, LPARAM lp, int wide, DWORD timeout, int *failed)
{
    int dummy;
    if (!failed) failed = &dummy;
    *failed = 0;
    if (pointer_msg(msg)) { *failed = 1; SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    XHdr hd;
    hdr(&hd, XOP_SEND, h);
    hd.msg = msg; hd.wp = (UINT64)wp; hd.lp = (UINT64)(LONG_PTR)lp; hd.wide = 1;
    if (msg == WM_SETTEXT) {                                /* the text goes with it, as Unicode */
        WCHAR *t = !lp ? NULL : wide ? (WCHAR *)lp : a2w((const char *)lp, -1);
        hd.n = t ? (UINT32)(wlen(t) + 1) * 2 : 0;
        LRESULT r = x_call(h, &hd, t, timeout, NULL, 0, NULL, failed);
        if (!wide && t) free(t);
        return r;
    }
    if (msg == WM_GETTEXT) {                                /* the text comes back */
        int max = (int)wp;
        if (max <= 0 || !lp) return 0;
        if (max > 16384) max = 16384;
        hd.wp = (UINT64)max;
        WCHAR *buf = malloc(2 * (size_t)max + 2);
        if (!buf) return 0;
        UINT32 got = 0;
        x_call(h, &hd, NULL, timeout, buf, 2 * (UINT32)max, &got, failed);
        int n = (int)(got / 2);
        if (n > 0 && !buf[n - 1]) n--;
        int r;
        if (wide) { memcpy((WCHAR *)lp, buf, 2 * (size_t)n); ((WCHAR *)lp)[n] = 0; r = n; }
        else {
            r = n ? WideCharToMultiByte(CP_ACP, 0, buf, n, (char *)lp, max - 1, NULL, NULL) : 0;
            ((char *)lp)[r] = 0;
        }
        free(buf);
        return r;
    }
    if (msg == WM_COPYDATA) {                               /* the data goes with it */
        const COPYDATASTRUCT *cd = (const COPYDATASTRUCT *)lp;
        if (!cd) return 0;
        hd.a[0] = (INT32)(UINT32)cd->dwData;
        hd.a[1] = (INT32)(UINT32)((UINT64)cd->dwData >> 32);
        hd.n = cd->cbData;
        hd.lp = 0;
        return x_call(h, &hd, cd->lpData, timeout, NULL, 0, NULL, failed);
    }
    hd.wide = wide ? 1 : 0;
    return x_call(h, &hd, NULL, timeout, NULL, 0, NULL, failed);
}

/* Calls a window's own process answers (5 seconds, as Windows waits on a
 * hung window before calling it so) */
#define X_TIMEOUT 5000

static LRESULT x_op(HWND h, UINT op, const INT32 *a, int na, LPARAM lp, int *failed)
{
    XHdr hd;
    hdr(&hd, op, h);
    for (int i = 0; i < na; i++) hd.a[i] = a[i];
    hd.lp = (UINT64)(LONG_PTR)lp;
    return x_call(h, &hd, NULL, X_TIMEOUT, NULL, 0, NULL, failed);
}

BOOL x_set_pos(HWND h, HWND after, int x, int y, int cx, int cy, UINT flags)
{
    INT32 a[5] = { x, y, cx, cy, (INT32)flags };
    int failed;
    LRESULT r = x_op(h, XOP_SETPOS, a, 5, (LPARAM)after, &failed);
    return failed == 1 ? FALSE : failed == 2 ? TRUE : (BOOL)r;   /* (timed out: it still moves when it answers) */
}

BOOL x_show(HWND h, int cmd)
{
    INT32 a[1] = { cmd };
    return (BOOL)x_op(h, XOP_SHOW, a, 1, 0, NULL);
}

LONG_PTR x_get_long(HWND h, int index, int *failed)
{
    INT32 a[1] = { index };
    return (LONG_PTR)x_op(h, XOP_GETLONG, a, 1, 0, failed);
}

LONG_PTR x_set_long(HWND h, int index, LONG_PTR v, int *failed)
{
    INT32 a[1] = { index };
    return (LONG_PTR)x_op(h, XOP_SETLONG, a, 1, (LPARAM)v, failed);
}

HWND x_set_focus(HWND h) { return (HWND)x_op(h, XOP_FOCUS, NULL, 0, 0, NULL); }

/* the thread that owns @h (0: not known) */
DWORD x_thread(HWND h) { return (DWORD)x_op(h, XOP_THREAD, NULL, 0, 0, NULL); }

BOOL x_enable(HWND h, BOOL on)
{
    INT32 a[1] = { on };
    return (BOOL)x_op(h, XOP_ENABLE, a, 1, 0, NULL);
}

static int x_string(HWND h, UINT op, WCHAR *buf, int n)
{
    if (!buf || n <= 0) return -1;
    XHdr hd;
    hdr(&hd, op, h);
    hd.a[0] = n;
    UINT32 got = 0;
    int failed;
    x_call(h, &hd, NULL, X_TIMEOUT, buf, 2 * (UINT32)n, &got, &failed);
    if (failed) { buf[0] = 0; return -1; }
    int k = (int)(got / 2);
    if (k > 0 && !buf[k - 1]) k--;
    if (k > n - 1) k = n - 1;
    buf[k] = 0;
    return k;
}

int x_get_text(HWND h, WCHAR *buf, int n) { return x_string(h, XOP_TEXT, buf, n); }
int x_class_name(HWND h, WCHAR *buf, int n) { int k = x_string(h, XOP_CLASS, buf, n); return k < 0 ? 0 : k; }

/* -----------------------------------------------------------------------
 * Answering: other processes' calls on our windows
 * ----------------------------------------------------------------------- */
typedef struct { UINT32 seq, noreply; XHdr hd; BYTE data[]; } XJob;

static void x_reply(UINT32 seq, LRESULT r, const void *data, UINT32 n)
{
    struct { UINT64 result; UINT32 len, pad; } *b = malloc(16 + n);
    if (!b) return;
    b->result = (UINT64)r; b->len = n; b->pad = 0;
    if (n) memcpy(b + 1, data, n);
    NtNovaGuiCtl(0, CTL_XREPLY, seq, b);
    free(b);
}

/* In the thread that owns the window */
static void x_run(void *arg)
{
    XJob *j = arg;
    XHdr *hd = &j->hd;
    HWND h = (HWND)(ULONG_PTR)hd->hwnd;
    LRESULT r = 0;
    void *out = NULL;
    UINT32 outn = 0;
    if (W_quiet(h)) switch (hd->op) {
    case XOP_SEND:
        if (hd->msg == WM_SETTEXT) {
            WCHAR *t = hd->n ? (WCHAR *)j->data : NULL;
            if (t) t[hd->n / 2 - 1] = 0;
            r = SendMessageW(h, WM_SETTEXT, 0, (LPARAM)t);
        } else if (hd->msg == WM_GETTEXT) {
            int max = (int)hd->wp;
            out = malloc(2 * (size_t)max + 2);
            if (out) {
                r = SendMessageW(h, WM_GETTEXT, (WPARAM)max, (LPARAM)out);
                if (r < 0) r = 0;
                if (r > max - 1) r = max - 1;
                outn = 2 * (UINT32)r;
            }
        } else if (hd->msg == WM_COPYDATA) {
            COPYDATASTRUCT cd;
            cd.dwData = (ULONG_PTR)((UINT64)(UINT32)hd->a[0] | (UINT64)(UINT32)hd->a[1] << 32);
            cd.cbData = hd->n;
            cd.lpData = hd->n ? j->data : NULL;
            r = SendMessageW(h, WM_COPYDATA, (WPARAM)hd->wp, (LPARAM)&cd);
        } else {
            r = hd->wide ? SendMessageW(h, hd->msg, (WPARAM)hd->wp, (LPARAM)hd->lp)
                         : SendMessageA(h, hd->msg, (WPARAM)hd->wp, (LPARAM)hd->lp);
        }
        break;
    case XOP_SETPOS:
        r = SetWindowPos(h, (HWND)(ULONG_PTR)hd->lp, hd->a[0], hd->a[1], hd->a[2], hd->a[3], (UINT)hd->a[4]);
        break;
    case XOP_SHOW: r = ShowWindow(h, hd->a[0]); break;
    case XOP_GETLONG: r = GetWindowLongPtrW(h, hd->a[0]); break;
    case XOP_SETLONG:
        /* (code and module addresses mean nothing in another process) */
        if (hd->a[0] != GWLP_WNDPROC && hd->a[0] != GWLP_HINSTANCE) r = SetWindowLongPtrW(h, hd->a[0], (LONG_PTR)hd->lp);
        break;
    case XOP_FOCUS: r = (LRESULT)SetFocus(h); break;
    case XOP_ENABLE: r = EnableWindow(h, hd->a[0]); break;
    case XOP_THREAD: r = GetWindowThreadProcessId(h, NULL); break;
    case XOP_EMBEDDED: wnd_embed_child(h, (HWND)(ULONG_PTR)hd->lp); break;
    case XOP_CLASS: case XOP_TEXT: {
        int n = hd->a[0] > 0 && hd->a[0] <= 16384 ? hd->a[0] : 256;
        out = malloc(2 * (size_t)n);
        if (!out) break;
        int k = hd->op == XOP_CLASS ? GetClassNameW(h, out, n) : InternalGetWindowText(h, out, n);
        outn = 2 * (UINT32)k;
        r = k;
        break;
    }
    }
    if (!j->noreply) x_reply(j->seq, r, out, outn);
    free(out);
    free(j);
}

int x_claim(void)
{
    int any = 0;
    UINT32 *b = malloc(16 + XMSG_MAX);
    if (!b) return 0;
    for (;;) {
        memset(b, 0, 16);
        b[3] = XMSG_MAX;
        if (NtNovaGuiCtl(0, CTL_XFETCH, 0, b) != 1) break;
        any = 1;
        UINT32 seq = b[0], flags = b[1], len = b[2];
        XHdr *hd = (XHdr *)(b + 4);
        if (len < sizeof(XHdr) || hd->n > len - sizeof(XHdr)) { if (flags & 1) x_reply(seq, 0, NULL, 0); continue; }
        HWND h = (HWND)(ULONG_PTR)hd->hwnd;
        Wnd *w = W_quiet(h);
        if (hd->op == XOP_POST) {
            if (w) post_msg(w, h, hd->msg, (WPARAM)hd->wp, (LPARAM)hd->lp);
            continue;
        }
        if (!w || (!(flags & 1) && hd->op != XOP_EMBEDDED)) { if (flags & 1) x_reply(seq, 0, NULL, 0); continue; }
        XJob *j = malloc(sizeof(XJob) + hd->n + 2);
        if (!j) { x_reply(seq, 0, NULL, 0); continue; }
        j->seq = seq;
        j->noreply = !(flags & 1);
        j->hd = *hd;
        memcpy(j->data, hd + 1, hd->n);
        j->data[hd->n] = j->data[hd->n + 1] = 0;
        DWORD tid = w == W_quiet(GetDesktopWindow()) ? g_main_tid : w->tid;
        if (!tid || tid == GetCurrentThreadId()) x_run(j);
        else x_send_queue(tid, x_run, j);
    }
    free(b);
    return any;
}

/* -----------------------------------------------------------------------
 * Embedding another process's window in ours (SetParent)
 * ----------------------------------------------------------------------- */
#define MAX_EMBEDS 32
typedef struct { HWND child, parent; UINT32 kid; INT32 last[8]; } Embed;
static Embed g_embeds[MAX_EMBEDS];
static int g_nembeds;

static Embed *embed_of(HWND child)
{
    for (int i = 0; i < MAX_EMBEDS; i++) if (g_embeds[i].child && g_embeds[i].child == child) return &g_embeds[i];
    return NULL;
}

HWND x_embed_parent(HWND child)
{
    Embed *e = embed_of(child);
    return e && W_quiet(e->parent) ? e->parent : 0;
}

/* Where @parent's client area is in its top-level window's client area and
 * how much of it shows, in the desktop's (logical) pixels:
 * in[1..2] origin, in[3..6] visible part (l, t, r, b), in[7] it shows */
static void embed_geometry(Wnd *parent, INT32 in[9])
{
    Wnd *t = top_of(parent);
    POINT o;
    wnd_to_bitmap(parent, 1, &o);
    RECT c = { o.x, o.y, o.x + (parent->client.right - parent->client.left), o.y + (parent->client.bottom - parent->client.top) };
    for (Wnd *a = parent->parent; a; a = a->parent) {
        POINT ao;
        wnd_to_bitmap(a, 1, &ao);
        RECT ac = { ao.x, ao.y, ao.x + (a->client.right - a->client.left), ao.y + (a->client.bottom - a->client.top) };
        if (!IntersectRect(&c, &c, &ac)) SetRectEmpty(&c);
    }
    int k = dpi_wnd_aware(t) ? dpi_k(t) : 1;
    in[1] = o.x / k; in[2] = o.y / k;
    in[3] = c.left / k; in[4] = c.top / k; in[5] = (c.right + k - 1) / k; in[6] = (c.bottom + k - 1) / k;
    in[7] = wnd_visible(parent);
}

HWND x_set_parent(HWND h, Wnd *parent)
{
    HWND old = x_embed_parent(h);
    if (!old) old = GetDesktopWindow();
    Embed *e = embed_of(h);
    if (!parent) {                                          /* a top-level window again */
        INT32 in[9] = { 0 };
        if (!NtNovaGuiCtl(0, CTL_EMBED, (ULONG_PTR)h, in) && !e) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return 0; }
        if (e) { e->child = 0; g_nembeds--; }
        return old;
    }
    Wnd *t = top_of(parent);
    if (!ensure_kernel_window(t)) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return 0; }
    INT32 in[9];
    embed_geometry(parent, in);
    in[0] = 1;
    in[8] = (INT32)(ULONG_PTR)parent->h;
    if (!NtNovaGuiCtl(t->kid, CTL_EMBED, (ULONG_PTR)h, in)) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return 0; }
    if (!e) {                                               /* (a child window there becomes a desktop window: it is told, without waiting) */
        XHdr hd;
        hdr(&hd, XOP_EMBEDDED, h);
        hd.lp = (UINT64)(ULONG_PTR)parent->h;
        x_queue(h, 0, &hd, NULL);
    }
    LOCK();
    if (!e) for (int i = 0; i < MAX_EMBEDS && !e; i++) if (!g_embeds[i].child) { e = &g_embeds[i]; g_nembeds++; }
    if (e) { e->child = h; e->parent = parent->h; e->kid = t->kid; memcpy(e->last, in, sizeof(e->last)); }
    UNLOCK();
    return old;
}

void embeds_follow(void)
{
    if (!g_nembeds) return;
    for (int i = 0; i < MAX_EMBEDS; i++) {
        Embed *e = &g_embeds[i];
        if (!e->child) continue;
        Wnd *p = W_quiet(e->parent);
        INT32 in[9];
        if (!p || (p->flags & WF_DESTROYING)) {             /* the parent went: the window is a top-level one again */
            memset(in, 0, sizeof(in));
            NtNovaGuiCtl(0, CTL_EMBED, (ULONG_PTR)e->child, in);
            e->child = 0; g_nembeds--;
            continue;
        }
        Wnd *t = top_of(p);
        if (!t->kid) continue;
        embed_geometry(p, in);
        in[0] = t->kid == e->kid ? 2 : 1;                   /* (moved to another of our top-level windows: embed anew) */
        in[8] = (INT32)(ULONG_PTR)p->h;
        if (in[0] == 2 && !memcmp(in + 1, e->last + 1, 7 * sizeof(INT32))) continue;
        if (!NtNovaGuiCtl(t->kid, CTL_EMBED, (ULONG_PTR)e->child, in)) { e->child = 0; g_nembeds--; continue; }
        e->kid = t->kid;
        memcpy(e->last, in, sizeof(e->last));
    }
}

/* -----------------------------------------------------------------------
 * The other side: our window embedded in another process's
 * ----------------------------------------------------------------------- */
void embed_notified(Wnd *top, HWND parent)
{
    if (top->parent) return;
    top->foreign_parent = parent;
    memset(top->klog, 0xFF, sizeof(top->klog));             /* (the desktop's rectangle is new) */
    top_sync_from_kernel(top, 1);
}

/* The foreign parent's client origin on screen, in the calling thread's
 * coordinates; *root: the host's top-level window */
int embed_origin(Wnd *top, POINT *p, HWND *root)
{
    INT32 out[3];
    if (!top->foreign_parent || !top->kid || !NtNovaGuiCtl(top->kid, CTL_EMBED_INFO, 0, out)) return 0;
    p->x = out[1]; p->y = out[2];
    dpi_to_proc(p);
    if (root) *root = 0;
    return 1;
}
