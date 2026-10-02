/*
 * msg.c — message queues, the message loop, sending and posting, timers,
 * and turning the desktop's input into messages for child windows
 */
#include "u32.h"

BYTE  g_keys[256];                  /* 0x80 down, 0x01 toggled */
POINT g_cursor;
static DWORD g_msg_time;
static POINT g_msg_pt;

/* -----------------------------------------------------------------------
 * Per-thread queues
 * ----------------------------------------------------------------------- */
typedef struct Sent {
    struct Sent *next;
    HWND h; UINT msg; WPARAM wp; LPARAM lp;
    int wide;
    LRESULT result;
    volatile LONG done;
    HANDLE ev;
    int notify;                     /* SendNotifyMessage: nobody waits */
} Sent;

typedef struct TQ {
    DWORD tid;
    MSG  *q;
    int   head, count, cap;
    Sent *sent;
    int   quit, quit_code;
    int   in_send;                  /* processing a message another thread sent */
    int   paints;                   /* BeginPaint calls (DispatchMessage checks WM_PAINT was handled) */
} TQ;

#define MAX_TQ 64
static TQ *g_tq[MAX_TQ];

static TQ *tq_for(DWORD tid, int create)
{
    for (int i = 0; i < MAX_TQ; i++) if (g_tq[i] && g_tq[i]->tid == tid) return g_tq[i];
    if (!create) return NULL;
    LOCK();
    TQ *q = NULL;
    for (int i = 0; i < MAX_TQ; i++) if (g_tq[i] && g_tq[i]->tid == tid) { q = g_tq[i]; break; }
    for (int i = 0; !q && i < MAX_TQ; i++)
        if (!g_tq[i]) { q = g_tq[i] = calloc(1, sizeof(TQ)); if (q) q->tid = tid; }
    UNLOCK();
    return q;
}

static TQ *my_tq(void) { return tq_for(GetCurrentThreadId(), 1); }

void note_begin_paint(void) { TQ *q = my_tq(); if (q) q->paints++; }

static void wake(DWORD tid)
{
    if (tid != GetCurrentThreadId()) NtNovaGuiCtl(0, CTL_WAKE, tid, NULL);
}

static BOOL q_push(TQ *q, const MSG *m)
{
    LOCK();
    if (q->count == q->cap) {
        int nc = q->cap ? q->cap * 2 : 64;
        if (nc > 65536) { UNLOCK(); return FALSE; }
        MSG *n = malloc(sizeof(MSG) * (size_t)nc);
        if (!n) { UNLOCK(); return FALSE; }
        for (int i = 0; i < q->count; i++) n[i] = q->q[(q->head + i) % q->cap];
        free(q->q);
        q->q = n; q->cap = nc; q->head = 0;
    }
    q->q[(q->head + q->count) % q->cap] = *m;
    q->count++;
    UNLOCK();
    return TRUE;
}

static int filter_ok(const MSG *m, HWND h, UINT mn, UINT mx)
{
    if (h && h != (HWND)-1) {
        if (m->hwnd != h) {
            Wnd *p = W_quiet(h), *c = W_quiet(m->hwnd);
            if (!p || !c || !is_child_of(p, c)) return 0;
        }
    } else if (h == (HWND)-1 && m->hwnd) return 0;          /* thread messages only */
    if (mn || mx) {
        if (mx < mn) return m->message >= mn || m->message <= mx;
        if (m->message < mn || m->message > mx) return 0;
    }
    return 1;
}

static int q_take(TQ *q, MSG *out, HWND h, UINT mn, UINT mx, int remove)
{
    int found = 0;
    LOCK();
    for (int i = 0; i < q->count; i++) {
        MSG *m = &q->q[(q->head + i) % q->cap];
        if (!filter_ok(m, h, mn, mx)) continue;
        *out = *m;
        found = 1;
        if (remove) {
            for (int k = i; k > 0; k--) q->q[(q->head + k) % q->cap] = q->q[(q->head + k - 1) % q->cap];
            q->head = (q->head + 1) % q->cap;
            q->count--;
        }
        break;
    }
    UNLOCK();
    return found;
}

void remove_window_messages(HWND h)
{
    TQ *q = my_tq();
    if (!q) return;
    LOCK();
    int o = 0;
    for (int i = 0; i < q->count; i++) {
        MSG m = q->q[(q->head + i) % q->cap];
        if (m.hwnd != h) q->q[(q->head + o++) % q->cap] = m;
    }
    q->count = o;
    UNLOCK();
}

/* -----------------------------------------------------------------------
 * Window procedures, with ANSI <-> Unicode conversion where the caller and
 * the procedure disagree
 * ----------------------------------------------------------------------- */
enum { CV_NONE, CV_IN_STR, CV_OUT_TEXT, CV_OUT_ITEM, CV_TEXTLEN, CV_CREATE };

static int conv_kind(Wnd *w, UINT msg)
{
    switch (msg) {
    case WM_SETTEXT: case WM_SETTINGCHANGE: case EM_REPLACESEL: case WM_DEVMODECHANGE: return CV_IN_STR;
    case WM_GETTEXT: return CV_OUT_TEXT;
    case EM_GETLINE: return CV_OUT_TEXT;
    case WM_GETTEXTLENGTH: return CV_TEXTLEN;
    case WM_NCCREATE: case WM_CREATE: return CV_CREATE;
    case LB_ADDSTRING: case LB_INSERTSTRING: case LB_FINDSTRING: case LB_FINDSTRINGEXACT: case LB_SELECTSTRING:
        return w && lb_is_string_msg(w, msg) ? CV_IN_STR : CV_NONE;
    case LB_DIR: case CB_DIR: return CV_IN_STR;
    case CB_ADDSTRING: case CB_INSERTSTRING: case CB_FINDSTRING: case CB_FINDSTRINGEXACT: case CB_SELECTSTRING:
        return w && cb_is_string_msg(w, msg) ? CV_IN_STR : CV_NONE;
    case LB_GETTEXT: return w && lb_is_string_msg(w, msg) ? CV_OUT_ITEM : CV_NONE;
    case CB_GETLBTEXT: return w && cb_is_string_msg(w, msg) ? CV_OUT_ITEM : CV_NONE;
    }
    return CV_NONE;
}

LRESULT call_proc(Wnd *w, WNDPROC proc, int wide, HWND h, UINT msg, WPARAM wp, LPARAM lp, int from_wide)
{
    if (!proc) return 0;
    if (wide == from_wide) return proc(h, msg, wp, lp);
    int k = conv_kind(w, msg);
    if (k == CV_NONE) {
        if ((msg == WM_CHAR || msg == WM_SYSCHAR) && !wide && wp >= 0x80) {
            char b[4];
            WCHAR c = (WCHAR)wp;
            int n = WideCharToMultiByte(CP_ACP, 0, &c, 1, b, 4, NULL, NULL);
            LRESULT r = 0;
            for (int i = 0; i < n; i++) r = proc(h, msg, (BYTE)b[i], lp);
            return r;
        }
        return proc(h, msg, wp, lp);
    }
    switch (k) {
    case CV_IN_STR: {
        if (!lp) return proc(h, msg, wp, lp);
        void *c = wide ? (void *)a2w((const char *)lp, -1) : (void *)w2a((const WCHAR *)lp, -1);
        LRESULT r = proc(h, msg, wp, (LPARAM)c);
        free(c);
        return r;
    }
    case CV_OUT_TEXT: case CV_OUT_ITEM: {
        int cap = k == CV_OUT_TEXT ? (int)wp : 4096;
        if (msg == EM_GETLINE) cap = *(WORD *)lp;
        if (!lp || cap <= 0) return proc(h, msg, wp, lp);
        if (wide) {                                         /* ANSI caller, Unicode procedure */
            WCHAR *b = malloc(2 * ((size_t)cap + 1));
            if (!b) return 0;
            if (msg == EM_GETLINE) *(WORD *)b = (WORD)cap;
            LRESULT r = proc(h, msg, k == CV_OUT_TEXT && msg != EM_GETLINE ? (WPARAM)cap : wp, (LPARAM)b);
            if (r < 0) { free(b); return r; }
            int n = (int)r;
            b[MIN(n, cap)] = 0;
            int m = WideCharToMultiByte(CP_ACP, 0, b, n, (char *)lp, k == CV_OUT_TEXT ? cap - (msg != EM_GETLINE) : cap, NULL, NULL);
            if (msg != EM_GETLINE && (k == CV_OUT_ITEM || m < cap)) ((char *)lp)[m] = 0;
            free(b);
            return m;
        } else {                                            /* Unicode caller, ANSI procedure */
            char *b = malloc((size_t)cap * 3 + 1);
            if (!b) return 0;
            if (msg == EM_GETLINE) *(WORD *)b = (WORD)(cap * 3);
            LRESULT r = proc(h, msg, k == CV_OUT_TEXT && msg != EM_GETLINE ? (WPARAM)(cap * 3) : wp, (LPARAM)b);
            if (r < 0) { free(b); return r; }
            int n = (int)r;
            b[n] = 0;
            int m = MultiByteToWideChar(CP_ACP, 0, b, n, (WCHAR *)lp, k == CV_OUT_TEXT ? cap - (msg != EM_GETLINE) : cap);
            if (msg != EM_GETLINE) ((WCHAR *)lp)[m] = 0;
            free(b);
            return m;
        }
    }
    case CV_TEXTLEN: {
        LRESULT n = proc(h, msg, wp, lp);
        if (n <= 0) return n;
        /* the length in the caller's units (at most this; the exact count needs the text) */
        if (wide) {
            WCHAR *b = malloc(2 * ((size_t)n + 1));
            if (!b) return n * 3;
            LRESULT got = proc(h, WM_GETTEXT, (WPARAM)n + 1, (LPARAM)b);
            int m = WideCharToMultiByte(CP_ACP, 0, b, (int)got, NULL, 0, NULL, NULL);
            free(b);
            return m;
        }
        return n;
    }
    case CV_CREATE: {
        CREATESTRUCTW cs = *(CREATESTRUCTW *)lp;
        void *n1 = NULL, *n2 = NULL;
        if ((ULONG_PTR)cs.lpszName >= 0x10000)
            cs.lpszName = n1 = wide ? (void *)a2w((const char *)cs.lpszName, -1) : (void *)w2a(cs.lpszName, -1);
        if ((ULONG_PTR)cs.lpszClass >= 0x10000)
            cs.lpszClass = n2 = wide ? (void *)a2w((const char *)cs.lpszClass, -1) : (void *)w2a(cs.lpszClass, -1);
        LRESULT r = proc(h, msg, wp, (LPARAM)&cs);
        free(n1); free(n2);
        return r;
    }
    }
    return proc(h, msg, wp, lp);
}

/* -----------------------------------------------------------------------
 * Sending
 * ----------------------------------------------------------------------- */
static LRESULT send_cross(Wnd *w, UINT msg, WPARAM wp, LPARAM lp, int wide, DWORD timeout, int *timed_out)
{
    TQ *q = tq_for(w->tid, 1);
    if (!q) return 0;
    Sent *s = calloc(1, sizeof(Sent));
    if (!s) return 0;
    s->h = w->h; s->msg = msg; s->wp = wp; s->lp = lp; s->wide = wide;
    s->ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    LOCK();
    Sent **pp = &q->sent;
    while (*pp) pp = &(*pp)->next;
    *pp = s;
    UNLOCK();
    wake(w->tid);
    ULONGLONG until = timeout == INFINITE ? ~0ULL : GetTickCount64() + timeout;
    while (!s->done) {
        process_sent();
        WaitForSingleObject(s->ev, 10);
        if (!s->done && GetTickCount64() >= until) {
            /* give up: it may still run; it frees itself then */
            s->notify = 1;
            if (timed_out) *timed_out = 1;
            return 0;
        }
        if (!s->done && !W_quiet(s->h)) break;
    }
    LRESULT r = s->result;
    CloseHandle(s->ev);
    free(s);
    return r;
}

void process_sent(void)
{
    TQ *q = tq_for(GetCurrentThreadId(), 0);
    if (!q) return;
    for (;;) {
        LOCK();
        Sent *s = q->sent;
        if (s) q->sent = s->next;
        UNLOCK();
        if (!s) return;
        Wnd *w = W_quiet(s->h);
        q->in_send++;
        LRESULT r = w ? call_proc(w, w->proc, w->wide, s->h, s->msg, s->wp, s->lp, s->wide) : 0;
        q->in_send--;
        if (s->notify) { if (s->ev) CloseHandle(s->ev); free(s); continue; }
        s->result = r;
        InterlockedExchange(&s->done, 1);
        SetEvent(s->ev);
    }
}

LRESULT send_msg(Wnd *w, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!w || !w->used) return 0;
    if (w->tid && w->tid != GetCurrentThreadId() && w->parent == NULL ? 0 : 0) {}
    if (w->tid && w->tid != GetCurrentThreadId()) return send_cross(w, msg, wp, lp, 1, INFINITE, NULL);
    return call_proc(w, w->proc, w->wide, w->h, msg, wp, lp, 1);
}

static LRESULT send_any(HWND h, UINT msg, WPARAM wp, LPARAM lp, int wide)
{
    if (h == HWND_BROADCAST) {
        HWND list[256];
        int n = 0;
        for (Wnd *c = W_quiet(GetDesktopWindow())->child; c && n < 256; c = c->next) list[n++] = c->h;
        for (int i = 0; i < n; i++) { Wnd *w = W_quiet(list[i]); if (w) send_any(list[i], msg, wp, lp, wide); }
        return 1;
    }
    Wnd *w = W(h);
    if (!w) return 0;
    if (w->tid && w->tid != GetCurrentThreadId()) return send_cross(w, msg, wp, lp, wide, INFINITE, NULL);
    return call_proc(w, w->proc, w->wide, h, msg, wp, lp, wide);
}

USERAPI LRESULT SendMessageW(HWND h, UINT msg, WPARAM wp, LPARAM lp) { return send_any(h, msg, wp, lp, 1); }
USERAPI LRESULT SendMessageA(HWND h, UINT msg, WPARAM wp, LPARAM lp) { return send_any(h, msg, wp, lp, 0); }

static LRESULT send_timeout(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT flags, UINT ms, PDWORD_PTR res, int wide)
{
    (void)flags;
    if (h == HWND_BROADCAST) { if (res) *res = 0; send_any(h, msg, wp, lp, wide); return TRUE; }
    Wnd *w = W(h);
    if (!w) return 0;
    int to = 0;
    LRESULT r = w->tid && w->tid != GetCurrentThreadId() ? send_cross(w, msg, wp, lp, wide, ms ? ms : 1, &to)
                                                         : call_proc(w, w->proc, w->wide, h, msg, wp, lp, wide);
    if (to) { SetLastError(ERROR_TIMEOUT); return 0; }
    if (res) *res = (DWORD_PTR)r;
    return TRUE;
}

USERAPI LRESULT SendMessageTimeoutW(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT f, UINT ms, PDWORD_PTR r) { return send_timeout(h, msg, wp, lp, f, ms, r, 1); }
USERAPI LRESULT SendMessageTimeoutA(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT f, UINT ms, PDWORD_PTR r) { return send_timeout(h, msg, wp, lp, f, ms, r, 0); }

static BOOL send_notify(HWND h, UINT msg, WPARAM wp, LPARAM lp, int wide)
{
    if (h == HWND_BROADCAST) { send_any(h, msg, wp, lp, wide); return TRUE; }
    Wnd *w = W(h);
    if (!w) return FALSE;
    if (w->tid && w->tid != GetCurrentThreadId()) {
        TQ *q = tq_for(w->tid, 1);
        Sent *s = calloc(1, sizeof(Sent));
        if (!q || !s) { free(s); return FALSE; }
        s->h = h; s->msg = msg; s->wp = wp; s->lp = lp; s->wide = wide; s->notify = 1;
        LOCK();
        Sent **pp = &q->sent;
        while (*pp) pp = &(*pp)->next;
        *pp = s;
        UNLOCK();
        wake(w->tid);
        return TRUE;
    }
    call_proc(w, w->proc, w->wide, h, msg, wp, lp, wide);
    return TRUE;
}

USERAPI BOOL SendNotifyMessageW(HWND h, UINT msg, WPARAM wp, LPARAM lp) { return send_notify(h, msg, wp, lp, 1); }
USERAPI BOOL SendNotifyMessageA(HWND h, UINT msg, WPARAM wp, LPARAM lp) { return send_notify(h, msg, wp, lp, 0); }

typedef VOID (CALLBACK *SENDASYNCPROC_)(HWND, UINT, ULONG_PTR, LRESULT);
USERAPI BOOL SendMessageCallbackW(HWND h, UINT msg, WPARAM wp, LPARAM lp, SENDASYNCPROC_ fn, ULONG_PTR data)
{
    LRESULT r = SendMessageW(h, msg, wp, lp);
    if (fn) fn(h, msg, data, r);
    return TRUE;
}
USERAPI BOOL SendMessageCallbackA(HWND h, UINT msg, WPARAM wp, LPARAM lp, SENDASYNCPROC_ fn, ULONG_PTR data)
{
    LRESULT r = SendMessageA(h, msg, wp, lp);
    if (fn) fn(h, msg, data, r);
    return TRUE;
}

USERAPI BOOL InSendMessage(void) { TQ *q = tq_for(GetCurrentThreadId(), 0); return q && q->in_send; }
USERAPI DWORD InSendMessageEx(LPVOID r) { (void)r; return InSendMessage() ? 1 /* ISMEX_SEND */ : 0; }
USERAPI BOOL ReplyMessage(LRESULT r) { (void)r; return FALSE; }

/* -----------------------------------------------------------------------
 * Posting
 * ----------------------------------------------------------------------- */
BOOL post_msg(Wnd *w, HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    DWORD tid = w ? w->tid : GetCurrentThreadId();
    TQ *q = tq_for(tid, 1);
    if (!q) return FALSE;
    MSG m;
    memset(&m, 0, sizeof(m));
    m.hwnd = h; m.message = msg; m.wParam = wp; m.lParam = lp;
    m.time = GetTickCount();
    m.pt = g_cursor;
    if (!q_push(q, &m)) { SetLastError(ERROR_NOT_ENOUGH_QUOTA); return FALSE; }
    wake(tid);
    return TRUE;
}

BOOL post_thread(DWORD tid, UINT msg, WPARAM wp, LPARAM lp)
{
    TQ *q = tq_for(tid, 1);
    if (!q) { SetLastError(ERROR_INVALID_THREAD_ID); return FALSE; }
    MSG m;
    memset(&m, 0, sizeof(m));
    m.message = msg; m.wParam = wp; m.lParam = lp; m.time = GetTickCount(); m.pt = g_cursor;
    if (!q_push(q, &m)) return FALSE;
    wake(tid);
    return TRUE;
}

static BOOL post_any(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!h) return post_thread(GetCurrentThreadId(), msg, wp, lp);
    if (h == HWND_BROADCAST) {
        for (Wnd *c = W_quiet(GetDesktopWindow())->child; c; c = c->next) post_msg(c, c->h, msg, wp, lp);
        return TRUE;
    }
    Wnd *w = W(h);
    if (!w) return FALSE;
    return post_msg(w, h, msg, wp, lp);
}

USERAPI BOOL PostMessageW(HWND h, UINT msg, WPARAM wp, LPARAM lp) { return post_any(h, msg, wp, lp); }
USERAPI BOOL PostMessageA(HWND h, UINT msg, WPARAM wp, LPARAM lp) { return post_any(h, msg, wp, lp); }
USERAPI BOOL PostThreadMessageW(DWORD tid, UINT msg, WPARAM wp, LPARAM lp) { return post_thread(tid, msg, wp, lp); }
USERAPI BOOL PostThreadMessageA(DWORD tid, UINT msg, WPARAM wp, LPARAM lp) { return post_thread(tid, msg, wp, lp); }

USERAPI VOID PostQuitMessage(int code)
{
    TQ *q = my_tq();
    if (q) { q->quit = 1; q->quit_code = code; }
}

/* -----------------------------------------------------------------------
 * Timers
 * ----------------------------------------------------------------------- */
typedef struct { int used; HWND h; UINT_PTR id; TIMERPROC fn; DWORD tid; UINT ms; ULONGLONG due; int system; } Timer;
#define MAX_TIMERS 256
static Timer g_timers[MAX_TIMERS];

UINT set_timer_internal(HWND h, UINT_PTR id, UINT ms, TIMERPROC fn, int system)
{
    DWORD tid = GetCurrentThreadId();
    Wnd *w = NULL;
    if (h) { w = W(h); if (!w) return 0; tid = w->tid; }
    if (ms < 10) ms = 10;
    if (ms > 0x7FFFFFFF) ms = 0x7FFFFFFF;
    LOCK();
    Timer *t = NULL;
    for (int i = 0; i < MAX_TIMERS; i++)
        if (g_timers[i].used && g_timers[i].h == h && g_timers[i].id == id && (h || g_timers[i].tid == tid) && g_timers[i].system == system) { t = &g_timers[i]; break; }
    if (!t && !h) {                                         /* a new thread timer: its own id */
        static UINT_PTR next_id = 0x7F00;
        int again;
        do {
            id = ++next_id;
            again = 0;
            for (int i = 0; i < MAX_TIMERS; i++) if (g_timers[i].used && !g_timers[i].h && g_timers[i].id == id) again = 1;
        } while (again);
    }
    for (int i = 0; !t && i < MAX_TIMERS; i++) if (!g_timers[i].used) t = &g_timers[i];
    if (!t) { UNLOCK(); SetLastError(ERROR_NO_SYSTEM_RESOURCES); return 0; }
    t->used = 1; t->h = h; t->id = id; t->fn = fn; t->tid = tid; t->ms = ms; t->system = system;
    t->due = GetTickCount64() + ms;
    UNLOCK();
    wake(tid);
    return (UINT)(id ? id : 1);
}

USERAPI UINT_PTR SetTimer(HWND h, UINT_PTR id, UINT ms, TIMERPROC fn) { return set_timer_internal(h, id, ms, fn, 0); }
USERAPI UINT_PTR SetCoalescableTimer(HWND h, UINT_PTR id, UINT ms, TIMERPROC fn, ULONG tol) { (void)tol; return SetTimer(h, id, ms, fn); }

static BOOL kill_timer(HWND h, UINT_PTR id, int system)
{
    BOOL found = FALSE;
    DWORD tid = GetCurrentThreadId();
    LOCK();
    for (int i = 0; i < MAX_TIMERS; i++)
        if (g_timers[i].used && g_timers[i].h == h && g_timers[i].id == id && g_timers[i].system == system && (h || g_timers[i].tid == tid)) {
            g_timers[i].used = 0;
            found = TRUE;
        }
    UNLOCK();
    return found;
}

USERAPI BOOL KillTimer(HWND h, UINT_PTR id) { return kill_timer(h, id, 0); }
BOOL kill_system_timer(HWND h, UINT_PTR id) { return kill_timer(h, id, 1); }

void kill_window_timers(HWND h)
{
    LOCK();
    for (int i = 0; i < MAX_TIMERS; i++) if (g_timers[i].used && g_timers[i].h == h) g_timers[i].used = 0;
    UNLOCK();
}

/* A due timer for this thread (and the filter): WM_TIMER / WM_SYSTIMER */
static int take_timer(DWORD tid, MSG *m, HWND h, UINT mn, UINT mx, int remove)
{
    ULONGLONG now = GetTickCount64();
    int got = 0;
    LOCK();
    for (int i = 0; i < MAX_TIMERS; i++) {
        Timer *t = &g_timers[i];
        if (!t->used || t->tid != tid || t->due > now) continue;
        MSG c;
        memset(&c, 0, sizeof(c));
        c.hwnd = t->h; c.message = t->system ? WM_SYSTIMER : WM_TIMER; c.wParam = t->id; c.lParam = (LPARAM)t->fn;
        c.time = (DWORD)now; c.pt = g_cursor;
        if (!filter_ok(&c, h, mn, mx)) continue;
        *m = c;
        if (remove) t->due = now + t->ms;
        got = 1;
        break;
    }
    UNLOCK();
    return got;
}

static ULONGLONG next_timer_due(DWORD tid)
{
    ULONGLONG best = ~0ULL;
    for (int i = 0; i < MAX_TIMERS; i++)
        if (g_timers[i].used && g_timers[i].tid == tid && g_timers[i].due < best) best = g_timers[i].due;
    return best;
}

static int is_timer_proc(HWND h, UINT_PTR id, LPARAM fn)
{
    for (int i = 0; i < MAX_TIMERS; i++)
        if (g_timers[i].used && g_timers[i].h == h && g_timers[i].id == id && (LPARAM)g_timers[i].fn == fn) return 1;
    return 0;
}

/* -----------------------------------------------------------------------
 * The desktop's input, turned into messages for the right window
 * ----------------------------------------------------------------------- */
/* Key state: g_keys is as of the last input message taken from the queue
 * (GetKeyState); g_async is what the keyboard is doing now */
BYTE g_async[256];
/* Alt (or F10) went down and no other key or button has since: only then
 * does its release open the menu bar, so Ctrl+Alt+S leaves the menu alone */
int g_alt_tap;

static void key_state(BYTE *keys, UINT msg, WPARAM wp)
{
    BYTE vk = (BYTE)wp;
    if (keys == g_keys) {
        if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
            if (vk == VK_MENU || vk == VK_F10) { if (!(keys[vk] & 0x80)) g_alt_tap = !(keys[VK_CONTROL] & 0x80) && !(keys[VK_SHIFT] & 0x80); }
            else g_alt_tap = 0;
        } else if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN ||
                   msg == WM_NCLBUTTONDOWN || msg == WM_NCRBUTTONDOWN) g_alt_tap = 0;
    }
    switch (msg) {
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (!(keys[vk] & 0x80)) keys[vk] ^= 1;
        keys[vk] |= 0x80;
        if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) keys[vk == VK_SHIFT ? VK_LSHIFT : vk == VK_CONTROL ? VK_LCONTROL : VK_LMENU] |= 0x80;
        break;
    case WM_KEYUP: case WM_SYSKEYUP:
        keys[vk] &= ~0x80;
        if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) keys[vk == VK_SHIFT ? VK_LSHIFT : vk == VK_CONTROL ? VK_LCONTROL : VK_LMENU] &= ~0x80;
        break;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_NCLBUTTONDOWN: case WM_NCLBUTTONDBLCLK: keys[VK_LBUTTON] |= 0x80; break;
    case WM_LBUTTONUP: case WM_NCLBUTTONUP: keys[VK_LBUTTON] &= ~0x80; break;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: case WM_NCRBUTTONDOWN: keys[VK_RBUTTON] |= 0x80; break;
    case WM_RBUTTONUP: case WM_NCRBUTTONUP: keys[VK_RBUTTON] &= ~0x80; break;
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: keys[VK_MBUTTON] |= 0x80; break;
    case WM_MBUTTONUP: keys[VK_MBUTTON] &= ~0x80; break;
    }
}

Wnd *top_by_kid(UINT32 kid)
{
    for (Wnd *c = W_quiet(GetDesktopWindow())->child; c; c = c->next) if (c->kid == kid) return c;
    return NULL;
}

static HWND g_track_leave;           /* TrackMouseEvent(TME_LEAVE) */
static int  g_track_nc;
static HWND g_last_mouse;            /* the window the pointer was last over */
static struct { HWND h; UINT msg; DWORD time; POINT pt; } g_last_click;

void track_mouse_leave(HWND h, int nc) { g_track_leave = h; g_track_nc = nc; }
void cancel_track_mouse(HWND h) { if (g_track_leave == h) g_track_leave = 0; }
HWND tracked_mouse(int *nc) { if (nc) *nc = g_track_nc; return g_track_leave; }

static void queue_input(Wnd *w, UINT msg, WPARAM wp, LPARAM lp, DWORD time)
{
    TQ *q = tq_for(w->tid, 1);
    if (!q) return;
    MSG m;
    m.hwnd = w->h; m.message = msg; m.wParam = wp; m.lParam = lp; m.time = time; m.pt = g_cursor;
    q_push(q, &m);
}

static void leave_check(Wnd *now_over, DWORD time)
{
    if (g_track_leave && (!now_over || now_over->h != g_track_leave)) {
        Wnd *t = W_quiet(g_track_leave);
        HWND h = g_track_leave;
        g_track_leave = 0;
        if (t) queue_input(t, g_track_nc ? WM_NCMOUSELEAVE : WM_MOUSELEAVE, 0, 0, time);
        (void)h;
    }
}

/* Which window (and which part) the pointer is over: WM_NCHITTEST down the tree */
static Wnd *hit_window(Wnd *top, POINT pt, int *hit)
{
    Wnd *w = window_at(pt);
    if (!w || top_of(w) != top) w = top;
    for (;;) {
        LRESULT ht = send_msg(w, WM_NCHITTEST, 0, MAKELPARAM(pt.x, pt.y));
        if (!W_quiet(w->h)) { *hit = HTNOWHERE; return NULL; }
        if (ht == HTTRANSPARENT && w->parent) {
            /* a sibling below it, else the parent */
            Wnd *below = NULL;
            POINT o;
            wnd_screen_origin(w->parent, 1, &o);
            POINT lp = { pt.x - o.x, pt.y - o.y };
            for (Wnd *s = w->next; s; s = s->next)
                if ((s->style & WS_VISIBLE) && PtInRect(&s->rect, lp)) { below = s; break; }
            w = below ? below : w->parent;
            continue;
        }
        *hit = (int)ht;
        return w;
    }
}

static void route_mouse(Wnd *top, const MSG *km)
{
    UINT msg = km->message;
    POINT pt;
    if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL) { pt.x = (short)LOWORD(km->lParam); pt.y = (short)HIWORD(km->lParam); }
    else { pt.x = top->bmp.x + (short)LOWORD(km->lParam); pt.y = top->bmp.y + (short)HIWORD(km->lParam); }
    g_cursor = pt;
    DWORD time = km->time ? km->time : GetTickCount();
    WPARAM mk = km->wParam & 0xFFFF;
    key_state(g_async, msg, km->wParam);
    if (msg == WM_LBUTTONDBLCLK) msg = WM_LBUTTONDOWN;      /* user32 decides what is a double click */

    Wnd *target;
    int hit = HTCLIENT;
    Wnd *cap = W_quiet(g_capture);
    if (cap) target = cap;
    else {
        if (top->style & WS_DISABLED) return;               /* a modal dialog is up */
        target = hit_window(top, pt, &hit);
        if (!target) return;
        while (target && target->parent && (target->style & WS_DISABLED)) { target = target->parent; hit = HTCLIENT; }
        if (!target || (target->style & WS_DISABLED)) return;
    }
    leave_check(target, time);
    if (!W_quiet(target->h)) return;

    if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL) {
        Wnd *f = W_quiet(g_focus);
        Wnd *dest = target;
        if (!dest && f) dest = f;
        queue_input(dest, msg, km->wParam, MAKELPARAM(pt.x, pt.y), time);
        return;
    }
    if (msg == WM_MOUSEMOVE || msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN) {
        if (!cap) send_msg(target, WM_SETCURSOR, (WPARAM)target->h, MAKELPARAM(hit, msg));
        if (!W_quiet(target->h)) return;
    }
    /* double clicks */
    if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN) {
        int dbl = g_last_click.h == target->h && g_last_click.msg == msg && time - g_last_click.time <= GetDoubleClickTime() &&
                  abs(pt.x - g_last_click.pt.x) <= 4 && abs(pt.y - g_last_click.pt.y) <= 4;
        if (dbl && (hit != HTCLIENT || (target->cls && (target->cls->style & CS_DBLCLKS)))) {
            msg += WM_LBUTTONDBLCLK - WM_LBUTTONDOWN;
            g_last_click.h = 0;
        } else {
            g_last_click.h = target->h; g_last_click.msg = msg; g_last_click.time = time; g_last_click.pt = pt;
        }
    }
    g_last_mouse = target->h;
    if (hit == HTCLIENT || cap) {
        POINT o;
        wnd_screen_origin(target, 1, &o);
        queue_input(target, msg, mk, MAKELPARAM(pt.x - o.x, pt.y - o.y), time);
    } else {
        queue_input(target, msg - WM_MOUSEMOVE + WM_NCMOUSEMOVE, (WPARAM)hit, MAKELPARAM(pt.x, pt.y), time);
    }
}

static void route_key(Wnd *top, const MSG *km)
{
    UINT msg = km->message;
    key_state(g_async, msg, km->wParam);
    Wnd *f = W_quiet(g_focus);
    if (top->style & WS_DISABLED) {
        /* a modal dialog went up between the key going down and coming up (Ctrl+Alt+S
         * opening Save As): the dialog gets the keys, so Ctrl and Alt don't stay held */
        Wnd *a = W_quiet(g_active);
        Wnd *alt = f && !(top_of(f)->style & WS_DISABLED) ? top_of(f) : a && !(a->style & WS_DISABLED) ? a : NULL;
        if (alt && alt->tid == top->tid) top = alt;
        else if (msg == WM_KEYUP || msg == WM_SYSKEYUP) { queue_input(top, msg, km->wParam, km->lParam, km->time ? km->time : GetTickCount()); return; }
        else return;
    }
    Wnd *target = f && top_of(f) == top ? f : NULL;
    if (!target) {
        /* the focus is elsewhere (a popup menu) or nowhere: the active window */
        Wnd *a = W_quiet(g_active);
        if (f && (top_of(f)->flags & WF_MENU_TRACK)) target = f;
        else target = a && a->tid == top->tid ? a : top;
        if (!f && target == top && (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR) && !(top->flags & WF_MENU_TRACK)) {
            /* no focus: keys go to the active window as system keys */
            if (msg == WM_KEYDOWN) msg = WM_SYSKEYDOWN;
            else if (msg == WM_KEYUP) msg = WM_SYSKEYUP;
            else msg = WM_SYSCHAR;
        }
    }
    queue_input(target, msg, km->wParam, km->lParam, km->time ? km->time : GetTickCount());
}

/* One message from the desktop: most become queued messages */
static void from_kernel(const MSG *km)
{
    Wnd *top = top_by_kid((UINT32)(ULONG_PTR)km->hwnd);
    if (!top) return;
    switch (km->message) {
    case WM_SIZE: top_sync_from_kernel(top, 1); break;
    case WM_MOVE: top_sync_from_kernel(top, 0); break;
    case WM_ACTIVATE: top_activated(top, km->wParam != 0); break;
    case WM_CLOSE:
        if (!(top->style & WS_DISABLED)) queue_input(top, WM_SYSCOMMAND, SC_CLOSE, 0, GetTickCount());
        break;
    case WM_PAINT: case WM_TIMER: break;
    case WM_MOUSELEAVE: leave_check(NULL, GetTickCount()); break;
    case WM_NOVA_DROP: drop_from_kernel(top, km); break;
    case WM_DISPLAYCHANGE: send_msg(top, WM_DISPLAYCHANGE, km->wParam, km->lParam); break;
    case WM_CHAR: case WM_SYSCHAR: break;                  /* TranslateMessage makes these, as on Windows */
    case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
        route_key(top, km);
        break;
    default:
        if ((km->message >= WM_MOUSEFIRST && km->message <= WM_MOUSELAST) || km->message == WM_MOUSEWHEEL) route_mouse(top, km);
        break;
    }
}

/* Take everything the desktop has for this thread (don't wait) */
static int drain_kernel(void)
{
    int any = 0;
    MSG km;
    while (NtNovaGuiGetMessage(0, &km, 0) == 1) { from_kernel(&km); any = 1; }
    return any;
}

/* -----------------------------------------------------------------------
 * The message loop
 * ----------------------------------------------------------------------- */
static void track(const MSG *m)
{
    g_msg_time = m->time ? m->time : GetTickCount();
    g_msg_pt = m->pt;
    key_state(g_keys, m->message, m->wParam);
}

/* The next message for GetMessage / PeekMessage: 1 got one, 0 none (no
 * wait) or the timeout passed.  Sent messages are handled on the way. */
int pump_one(MSG *m, HWND h, UINT mn, UINT mx, UINT flags, int wait, DWORD timeout)
{
    TQ *q = my_tq();
    if (!q) return 0;
    DWORD tid = q->tid;
    int remove = (flags & PM_REMOVE) != 0;
    ULONGLONG until = timeout == INFINITE ? ~0ULL : GetTickCount64() + timeout;
    for (;;) {
        process_sent();
        if (q_take(q, m, h, mn, mx, remove)) { if (remove) track(m); return 1; }
        if (drain_kernel()) continue;
        if (q->quit && (!mn && !mx ? 1 : (WM_QUIT >= mn && WM_QUIT <= mx))) {
            memset(m, 0, sizeof(*m));
            m->message = WM_QUIT;
            m->wParam = (WPARAM)q->quit_code;
            if (remove) q->quit = 0;
            return 1;
        }
        if (!(flags & PM_QS_NOPAINT_)) {
            MSG pm = { 0 };
            pm.message = WM_PAINT;
            Wnd *pw;
            if ((!mn && !mx) || (WM_PAINT >= mn && WM_PAINT <= mx)) {
                if ((pw = next_paint(tid, h && h != (HWND)-1 ? h : 0))) {
                    memset(m, 0, sizeof(*m));
                    m->hwnd = pw->h; m->message = WM_PAINT; m->time = GetTickCount(); m->pt = g_cursor;
                    return 1;
                }
            }
        }
        caret_blink();
        if (take_timer(tid, m, h, mn, mx, remove)) { if (remove) track(m); return 1; }
        present_thread(tid);
        if (!wait) return 0;
        ULONGLONG now = GetTickCount64();
        if (now >= until) return 0;
        ULONGLONG due = next_timer_due(tid);
        ULONGLONG blink = now + 530;
        if (due > blink) due = blink;
        if (due > until) due = until;
        DWORD ms = due <= now ? 0 : (DWORD)(due - now);
        MSG km;
        long r = NtNovaGuiGetMessage(0, &km, ms + 2);
        if (r == 1) from_kernel(&km);
        else if (r == 0) {                                  /* the process is ending */
            q->quit = 1;
        }
    }
}

USERAPI BOOL GetMessageW(LPMSG m, HWND h, UINT mn, UINT mx)
{
    if (!pump_one(m, h, mn, mx, PM_REMOVE, 1, INFINITE)) return -1;
    return m->message != WM_QUIT;
}

USERAPI BOOL GetMessageA(LPMSG m, HWND h, UINT mn, UINT mx) { return GetMessageW(m, h, mn, mx); }

USERAPI BOOL PeekMessageW(LPMSG m, HWND h, UINT mn, UINT mx, UINT remove)
{
    return pump_one(m, h, mn, mx, remove, 0, 0);
}

USERAPI BOOL PeekMessageA(LPMSG m, HWND h, UINT mn, UINT mx, UINT remove) { return PeekMessageW(m, h, mn, mx, remove); }

USERAPI BOOL WaitMessage(void)
{
    MSG m;
    pump_one(&m, 0, 0, 0, PM_NOREMOVE, 1, INFINITE);
    return TRUE;
}

USERAPI DWORD GetQueueStatus(UINT flags)
{
    MSG m;
    return pump_one(&m, 0, 0, 0, PM_NOREMOVE, 0, 0) ? ((flags & 0xFFFF) | ((flags & 0xFFFF) << 16)) : 0;
}

USERAPI BOOL GetInputState(void) { return GetQueueStatus(QS_INPUT) != 0; }
USERAPI LONG GetMessageTime(void) { return (LONG)g_msg_time; }
USERAPI DWORD GetMessagePos(void) { return (DWORD)MAKELONG((SHORT)g_msg_pt.x, (SHORT)g_msg_pt.y); }
USERAPI LPARAM GetMessageExtraInfo(void) { return 0; }
USERAPI LPARAM SetMessageExtraInfo(LPARAM lp) { (void)lp; return 0; }
USERAPI BOOL SetMessageQueue(int n) { (void)n; return TRUE; }

USERAPI DWORD MsgWaitForMultipleObjectsEx(DWORD n, const HANDLE *hs, DWORD ms, DWORD wake_mask, DWORD flags)
{
    ULONGLONG until = ms == INFINITE ? ~0ULL : GetTickCount64() + ms;
    BOOL all = (flags & MWMO_WAITALL) != 0, alert = (flags & MWMO_ALERTABLE) != 0;
    for (;;) {
        if (n) {
            DWORD r = WaitForMultipleObjectsEx(n, hs, all, 0, alert);
            if (r != WAIT_TIMEOUT) return r;
        }
        MSG m;
        if (wake_mask) {
            process_sent();
            if (pump_one(&m, 0, 0, 0, PM_NOREMOVE | PM_QS_NOPAINT_, 0, 0)) return WAIT_OBJECT_0 + n;
            if ((wake_mask & QS_PAINT) && next_paint(GetCurrentThreadId(), 0)) return WAIT_OBJECT_0 + n;
        }
        ULONGLONG now = GetTickCount64();
        if (now >= until) return WAIT_TIMEOUT;
        DWORD slice = until - now > 10 ? 10 : (DWORD)(until - now);
        if (n) {
            DWORD r = WaitForMultipleObjectsEx(n, hs, all, slice, alert);
            if (r != WAIT_TIMEOUT) return r;
        } else if (wake_mask) {
            MSG km;
            long r = NtNovaGuiGetMessage(0, &km, slice + 2);
            if (r == 1) from_kernel(&km);
        } else Sleep(slice);
    }
}

USERAPI DWORD MsgWaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL all, DWORD ms, DWORD wake)
{
    return MsgWaitForMultipleObjectsEx(n, hs, ms, wake, all ? MWMO_WAITALL : 0);
}

/* WM_KEYDOWN -> WM_CHAR (and WM_SYSKEYDOWN -> WM_SYSCHAR), next in the queue */
static void q_push_front(TQ *q, const MSG *m)
{
    if (!q_push(q, m)) return;                              /* grows the queue; then rotate it to the front */
    LOCK();
    MSG t = q->q[(q->head + q->count - 1) % q->cap];
    for (int k = q->count - 1; k > 0; k--) q->q[(q->head + k) % q->cap] = q->q[(q->head + k - 1) % q->cap];
    q->q[q->head] = t;
    UNLOCK();
}

USERAPI BOOL TranslateMessage(const MSG *m)
{
    if (!m || (m->message != WM_KEYDOWN && m->message != WM_SYSKEYDOWN)) return FALSE;
    WCHAR c[4];
    BYTE keys[256];
    memcpy(keys, g_keys, 256);
    if (m->message == WM_SYSKEYDOWN) keys[VK_CONTROL] &= ~0x80;
    int n = ToUnicode((UINT)m->wParam, (UINT)(m->lParam >> 16) & 0xFF, keys, c, 4, 0);
    if (n <= 0) return FALSE;
    TQ *q = my_tq();
    if (!q) return FALSE;
    for (int i = n - 1; i >= 0; i--) {
        MSG cm = *m;
        cm.message = m->message == WM_SYSKEYDOWN ? WM_SYSCHAR : WM_CHAR;
        cm.wParam = c[i];
        q_push_front(q, &cm);
    }
    return TRUE;
}
USERAPI BOOL TranslateMessageEx(const MSG *m, UINT f) { (void)f; return TranslateMessage(m); }

static LRESULT dispatch(const MSG *m, int wide)
{
    if ((m->message == WM_TIMER || m->message == WM_SYSTIMER) && m->lParam) {
        if (is_timer_proc(m->hwnd, m->wParam, m->lParam)) {
            ((TIMERPROC)m->lParam)(m->hwnd, m->message, m->wParam, GetTickCount());
            return 0;
        }
    }
    Wnd *w = W_quiet(m->hwnd);
    if (!w) return 0;
    if (m->message == WM_PAINT) {
        TQ *q = my_tq();
        int before = q ? q->paints : 0;
        LRESULT r = call_proc(w, w->proc, w->wide, m->hwnd, m->message, m->wParam, m->lParam, wide);
        w = W_quiet(m->hwnd);
        if (w && q && q->paints == before && w->has_upd) validate(w, NULL);   /* never painted: don't loop */
        return r;
    }
    if (m->message == WM_SYSTIMER) {                        /* user32's own (caret, scrolling) */
        return call_proc(w, w->proc, w->wide, m->hwnd, m->message, m->wParam, m->lParam, wide);
    }
    return call_proc(w, w->proc, w->wide, m->hwnd, m->message, m->wParam, m->lParam, wide);
}

USERAPI LRESULT DispatchMessageW(const MSG *m) { return dispatch(m, 1); }
USERAPI LRESULT DispatchMessageA(const MSG *m) { return dispatch(m, 0); }

USERAPI LRESULT CallWindowProcW(WNDPROC fn, HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!fn) return 0;
    Wnd *w = W_quiet(h);
    /* a class's procedure keeps its own character set */
    int wide = 1;
    if (w) {
        if (fn == w->proc) wide = w->wide;
        else if (w->cls && fn == w->cls->proc) wide = w->cls->wide;
    }
    return call_proc(w, fn, wide, h, msg, wp, lp, 1);
}

USERAPI LRESULT CallWindowProcA(WNDPROC fn, HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!fn) return 0;
    Wnd *w = W_quiet(h);
    int wide = 0;
    if (w) {
        if (fn == w->proc) wide = w->wide;
        else if (w->cls && fn == w->cls->proc) wide = w->cls->wide;
    }
    return call_proc(w, fn, wide, h, msg, wp, lp, 0);
}

/* Messages by name: 0xC000 and up (shared with clipboard formats) */
static WCHAR *g_msg_names[1024];
static UINT register_message(LPCWSTR name)
{
    if (!name || !*name) return 0;
    LOCK();
    for (int i = 0; i < 1024; i++) {
        if (!g_msg_names[i]) { g_msg_names[i] = wstrdup(name); UNLOCK(); return 0xC000 + (UINT)i; }
        if (!wcsicmp_(g_msg_names[i], name)) { UNLOCK(); return 0xC000 + (UINT)i; }
    }
    UNLOCK();
    return 0;
}

int registered_name(UINT id, WCHAR *buf, int n)
{
    if (id < 0xC000 || id >= 0xC000 + 1024 || !g_msg_names[id - 0xC000] || n <= 0) return 0;
    const WCHAR *s = g_msg_names[id - 0xC000];
    int k = 0;
    for (; s[k] && k < n - 1; k++) buf[k] = s[k];
    buf[k] = 0;
    return k;
}

USERAPI UINT RegisterWindowMessageW(LPCWSTR name) { return register_message(name); }
USERAPI UINT RegisterWindowMessageA(LPCSTR name) { WCHAR *w = a2w(name, -1); UINT r = register_message(w); free(w); return r; }
USERAPI UINT RegisterClipboardFormatW(LPCWSTR name) { return register_message(name); }
USERAPI UINT RegisterClipboardFormatA(LPCSTR name) { return RegisterWindowMessageA(name); }

/* -----------------------------------------------------------------------
 * Hooks: accepted; the ones programs rely on for their own dialogs are
 * called (WH_MSGFILTER from dialog and menu loops, WH_GETMESSAGE)
 * ----------------------------------------------------------------------- */
typedef struct { int used; int id; HOOKPROC fn; DWORD tid; } Hook;
static Hook g_hooks[32];

USERAPI HHOOK SetWindowsHookExW(int id, HOOKPROC fn, HINSTANCE mod, DWORD tid)
{
    (void)mod;
    for (int i = 0; i < 32; i++)
        if (!g_hooks[i].used) {
            g_hooks[i].used = 1; g_hooks[i].id = id; g_hooks[i].fn = fn; g_hooks[i].tid = tid;
            return (HHOOK)(ULONG_PTR)(0x48000 + i);
        }
    return 0;
}
USERAPI HHOOK SetWindowsHookExA(int id, HOOKPROC fn, HINSTANCE mod, DWORD tid) { return SetWindowsHookExW(id, fn, mod, tid); }
USERAPI HHOOK SetWindowsHookW(int id, HOOKPROC fn) { return SetWindowsHookExW(id, fn, 0, GetCurrentThreadId()); }
USERAPI HHOOK SetWindowsHookA(int id, HOOKPROC fn) { return SetWindowsHookExW(id, fn, 0, GetCurrentThreadId()); }
USERAPI BOOL UnhookWindowsHookEx(HHOOK h)
{
    int i = (int)((ULONG_PTR)h - 0x48000);
    if (i < 0 || i >= 32) return FALSE;
    g_hooks[i].used = 0;
    return TRUE;
}
USERAPI BOOL UnhookWindowsHook(int id, HOOKPROC fn)
{
    for (int i = 0; i < 32; i++) if (g_hooks[i].used && g_hooks[i].id == id && g_hooks[i].fn == fn) g_hooks[i].used = 0;
    return TRUE;
}
USERAPI LRESULT CallNextHookEx(HHOOK h, int code, WPARAM wp, LPARAM lp) { (void)h; (void)code; (void)wp; (void)lp; return 0; }
USERAPI BOOL CallMsgFilterW(LPMSG m, int code)
{
    for (int i = 0; i < 32; i++)
        if (g_hooks[i].used && g_hooks[i].id == WH_MSGFILTER && (!g_hooks[i].tid || g_hooks[i].tid == GetCurrentThreadId()))
            if (g_hooks[i].fn(code, 0, (LPARAM)m)) return TRUE;
    return FALSE;
}
USERAPI BOOL CallMsgFilterA(LPMSG m, int code) { return CallMsgFilterW(m, code); }
USERAPI BOOL CallMsgFilter(LPMSG m, int code) { return CallMsgFilterW(m, code); }
USERAPI HWINEVENTHOOK SetWinEventHook(DWORD a, DWORD b, HMODULE m, WINEVENTPROC fn, DWORD pid, DWORD tid, DWORD f)
{ (void)a; (void)b; (void)m; (void)fn; (void)pid; (void)tid; (void)f; return (HWINEVENTHOOK)(ULONG_PTR)0x48100; }
USERAPI BOOL UnhookWinEvent(HWINEVENTHOOK h) { (void)h; return TRUE; }
USERAPI void NotifyWinEvent(DWORD ev, HWND h, LONG obj, LONG child) { (void)ev; (void)h; (void)obj; (void)child; }
USERAPI BOOL IsWinEventHookInstalled(DWORD ev) { (void)ev; return FALSE; }

USERAPI BOOL AttachThreadInput(DWORD a, DWORD b, BOOL attach) { (void)a; (void)b; (void)attach; return TRUE; }
USERAPI DWORD WaitForInputIdle(HANDLE p, DWORD ms) { (void)p; (void)ms; return 0; }
USERAPI BOOL GetGUIThreadInfo(DWORD tid, PGUITHREADINFO gi)
{
    (void)tid;
    if (!gi) return FALSE;
    DWORD cb = gi->cbSize;
    memset(gi, 0, cb);
    gi->cbSize = cb;
    gi->hwndActive = GetActiveWindow();
    gi->hwndFocus = GetFocus();
    gi->hwndCapture = GetCapture();
    return TRUE;
}

/* -----------------------------------------------------------------------
 * Capture and mouse tracking
 * ----------------------------------------------------------------------- */
USERAPI HWND SetCapture(HWND h)
{
    HWND old = g_capture;
    Wnd *w = W_quiet(h);
    if (!w) return 0;
    if (old == h) return old;
    Wnd *o = W_quiet(old);
    g_capture = h;
    g_capture_nc = 0;
    Wnd *t = top_of(w);
    if (o && top_of(o) != t && top_of(o)->kid) NtNovaGuiCtl(top_of(o)->kid, CTL_CAPTURE, 0, NULL);
    if (t->kid) NtNovaGuiCtl(t->kid, CTL_CAPTURE, 1, NULL);
    if (o) send_msg(o, WM_CAPTURECHANGED, 0, (LPARAM)h);
    return W_quiet(old) ? old : 0;
}

USERAPI BOOL ReleaseCapture(void)
{
    HWND old = g_capture;
    if (!old) return TRUE;
    g_capture = 0;
    g_capture_nc = 0;
    Wnd *o = W_quiet(old);
    if (o && top_of(o)->kid) NtNovaGuiCtl(top_of(o)->kid, CTL_CAPTURE, 0, NULL);
    if (o) send_msg(o, WM_CAPTURECHANGED, 0, 0);
    return TRUE;
}

USERAPI HWND GetCapture(void) { return W_quiet(g_capture) ? g_capture : 0; }

USERAPI BOOL TrackMouseEvent(LPTRACKMOUSEEVENT t)
{
    if (!t) return FALSE;
    if (t->dwFlags & TME_QUERY) {
        t->dwFlags = g_track_leave ? (TME_LEAVE | (g_track_nc ? TME_NONCLIENT : 0)) : 0;
        t->hwndTrack = g_track_leave;
        return TRUE;
    }
    if (t->dwFlags & TME_CANCEL) { if (g_track_leave == t->hwndTrack) g_track_leave = 0; return TRUE; }
    if (t->dwFlags & TME_LEAVE) {
        Wnd *w = W_quiet(t->hwndTrack);
        if (!w) return FALSE;
        /* already outside: WM_MOUSELEAVE right away */
        if (g_last_mouse != t->hwndTrack) { post_msg(w, w->h, (t->dwFlags & TME_NONCLIENT) ? WM_NCMOUSELEAVE : WM_MOUSELEAVE, 0, 0); return TRUE; }
        g_track_leave = t->hwndTrack;
        g_track_nc = (t->dwFlags & TME_NONCLIENT) != 0;
    }
    return TRUE;
}

