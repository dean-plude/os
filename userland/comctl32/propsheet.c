/*
 * propsheet.c — property sheets: a dialog holding a tab control whose
 * pages are the program's dialogs (each created from its template on
 * its first visit), with OK, Cancel, Apply and Help below; or, as a
 * wizard, Back, Next, Finish and Cancel. The pages hear PSN_SETACTIVE,
 * PSN_KILLACTIVE, PSN_APPLY, PSN_RESET, PSN_QUERYCANCEL, PSN_HELP and
 * the wizard notifications through WM_NOTIFY, and talk to the sheet
 * with the PSM_* messages.
 */
#include "cc.h"

#define ID_TAB 0x3020
#define ID_APPLY 0x3021
#define ID_BACK 0x3023
#define ID_NEXT 0x3024
#define ID_FINISH 0x3025
#define PSP_MAGIC 0x50535047u

typedef struct _PSP {
    UINT magic;
    PROPSHEETPAGEW psp;             /* a copy; the program's template and strings stay its own */
    WCHAR *title;
    HWND hwnd;
    int changed;
} Page;

typedef struct {
    PROPSHEETHEADERW hdr;
    Page **pages;
    int n, cap;
    int cur;
    HWND tab;
    HWND hwnd;
    int modeless;
    int result;
    int wizard;
    int cancel_to_close;
    int restart;
    RECT page_rect;                 /* client area for the pages */
    WCHAR *finish_text;
} Sheet;

static Sheet *sheet_of(HWND h) { return (Sheet *)GetWindowLongPtrW(h, DWLP_USER); }

/* -----------------------------------------------------------------------
 * Pages
 * ----------------------------------------------------------------------- */
static Page *page_of(HPROPSHEETPAGE h)
{
    Page *p = (Page *)h;
    return p && (ULONG_PTR)h > 0xFFFF && p->magic == PSP_MAGIC ? p : NULL;
}

CC HPROPSHEETPAGE WINAPI CreatePropertySheetPageW(LPCPROPSHEETPAGEW in)
{
    if (!in || in->dwSize < offsetof(PROPSHEETPAGEW, pfnCallback)) return NULL;
    Page *p = calloc(1, sizeof(Page));
    if (!p) return NULL;
    p->magic = PSP_MAGIC;
    memcpy(&p->psp, in, MIN(in->dwSize, (DWORD)sizeof(PROPSHEETPAGEW)));
    p->psp.dwSize = sizeof(PROPSHEETPAGEW);
    if ((in->dwFlags & PSP_USETITLE) && in->pszTitle && (ULONG_PTR)in->pszTitle > 0xFFFF) p->title = wdup(in->pszTitle);
    if ((in->dwFlags & PSP_USECALLBACK) && in->pfnCallback) in->pfnCallback(NULL, PSPCB_ADDREF, &p->psp);
    return (HPROPSHEETPAGE)p;
}

CC HPROPSHEETPAGE WINAPI CreatePropertySheetPageA(const void *in)
{
    /* PROPSHEETPAGEA has the same layout; only the strings differ */
    const PROPSHEETPAGEW *a = in;
    if (!a) return NULL;
    PROPSHEETPAGEW w;
    memset(&w, 0, sizeof(w));
    memcpy(&w, a, MIN(a->dwSize, (DWORD)sizeof(w)));
    WCHAR title[256], tmpl[256];
    if ((a->dwFlags & PSP_USETITLE) && a->pszTitle && (ULONG_PTR)a->pszTitle > 0xFFFF) { MultiByteToWideChar(CP_ACP, 0, (const char *)a->pszTitle, -1, title, 256); w.pszTitle = title; }
    if (!(a->dwFlags & PSP_DLGINDIRECT) && a->pszTemplate && (ULONG_PTR)a->pszTemplate > 0xFFFF) {
        MultiByteToWideChar(CP_ACP, 0, (const char *)a->pszTemplate, -1, tmpl, 256);
        /* the template name must outlive the page: keep the resource itself */
        HRSRC r = FindResourceW(a->hInstance, tmpl, (LPCWSTR)5 /* RT_DIALOG */);
        HGLOBAL g = r ? LoadResource(a->hInstance, r) : NULL;
        if (!g) return NULL;
        w.pResource = LockResource(g);
        w.dwFlags |= PSP_DLGINDIRECT;
    }
    return CreatePropertySheetPageW(&w);
}

CC BOOL WINAPI DestroyPropertySheetPage(HPROPSHEETPAGE h)
{
    Page *p = page_of(h);
    if (!p) return FALSE;
    if ((p->psp.dwFlags & PSP_USECALLBACK) && p->psp.pfnCallback) p->psp.pfnCallback(NULL, PSPCB_RELEASE, &p->psp);
    wfree(p->title);
    p->magic = 0;
    free(p);
    return TRUE;
}

/* the page's dialog template (from the resource or given directly) */
static const void *page_template(Page *p, DWORD *size)
{
    *size = 0;
    if (p->psp.dwFlags & PSP_DLGINDIRECT) return p->psp.pResource;
    HRSRC r = FindResourceW(p->psp.hInstance, p->psp.pszTemplate, (LPCWSTR)5 /* RT_DIALOG */);
    if (!r) return NULL;
    HGLOBAL g = LoadResource(p->psp.hInstance, r);
    *size = SizeofResource(p->psp.hInstance, r);
    return g ? LockResource(g) : NULL;
}

/* A template's header: is it DLGTEMPLATEEX, its style, size in dialog
 * units, title and number of items; @rest = where the items begin */
typedef struct { int ex; DWORD style; WORD items; short cx, cy; const WORD *rest; const WCHAR *title; } TmplHdr;

static const WORD *skip_sz(const WORD *p)
{
    if (p[0] == 0xFFFF) return p + 2;
    while (*p) p++;
    return p + 1;
}

static void tmpl_header(const void *t, TmplHdr *h)
{
    const WORD *w = t;
    h->ex = w[0] == 1 && w[1] == 0xFFFF;
    const BYTE *b = t;
    if (h->ex) { memcpy(&h->style, b + 12, 4); memcpy(&h->items, b + 16, 2); memcpy(&h->cx, b + 22, 2); memcpy(&h->cy, b + 24, 2); w = (const WORD *)(b + 26); }
    else { memcpy(&h->style, b, 4); memcpy(&h->items, b + 8, 2); memcpy(&h->cx, b + 14, 2); memcpy(&h->cy, b + 16, 2); w = (const WORD *)(b + 18); }
    w = skip_sz(w);                                         /* menu */
    w = skip_sz(w);                                         /* class */
    h->title = (const WCHAR *)w;
    w = skip_sz(w);                                         /* title */
    if (h->style & DS_SETFONT) { w++; if (h->ex) w += 2; w = skip_sz(w); }
    h->rest = w;
}

/* the length of a template (walking its items), for copying one given
 * without a size */
static DWORD tmpl_length(const void *t)
{
    TmplHdr h;
    tmpl_header(t, &h);
    const BYTE *p = (const BYTE *)h.rest;
    for (int i = 0; i < h.items; i++) {
        p = (const BYTE *)(((ULONG_PTR)p + 3) & ~(ULONG_PTR)3);
        p += h.ex ? 24 : 18;
        p = (const BYTE *)skip_sz((const WORD *)p);
        p = (const BYTE *)skip_sz((const WORD *)p);
        WORD extra;
        memcpy(&extra, p, 2);
        p += 2 + extra;
    }
    return (DWORD)(p - (const BYTE *)t);
}

/* a copy of the page's template made into a child: no frame or caption */
static void *page_child_template(Page *p, DWORD *size)
{
    DWORD n;
    const void *t = page_template(p, &n);
    if (!t) return NULL;
    if (!n) n = tmpl_length(t);
    BYTE *c = malloc(n + 8);
    if (!c) return NULL;
    memcpy(c, t, n);
    TmplHdr h;
    tmpl_header(c, &h);
    DWORD style = (h.style & ~(WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME | WS_VISIBLE | DS_MODALFRAME | DS_CENTER)) | WS_CHILD | DS_CONTROL;
    memcpy(c + (h.ex ? 12 : 0), &style, 4);
    *size = n;
    return c;
}

/* -----------------------------------------------------------------------
 * The sheet
 * ----------------------------------------------------------------------- */
static LRESULT page_notify(Sheet *s, int i, UINT code, LPARAM lp)
{
    Page *p = s->pages[i];
    if (!p->hwnd) return 0;
    PSHNOTIFY nm;
    memset(&nm, 0, sizeof(nm));
    nm.hdr.hwndFrom = s->hwnd;
    nm.hdr.idFrom = 0;
    nm.hdr.code = code;
    nm.lParam = lp;
    return SendMessageW(p->hwnd, WM_NOTIFY, 0, (LPARAM)&nm);
}

static void place_page(Sheet *s, Page *p)
{
    if (!p->hwnd) return;
    RECT r = s->page_rect;
    SetWindowPos(p->hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE);
}

static BOOL create_page(Sheet *s, int i)
{
    Page *p = s->pages[i];
    if (p->hwnd) return TRUE;
    if ((p->psp.dwFlags & PSP_USECALLBACK) && p->psp.pfnCallback && !p->psp.pfnCallback(s->hwnd, PSPCB_CREATE, &p->psp)) return FALSE;
    DWORD n;
    void *t = page_child_template(p, &n);
    if (!t) return FALSE;
    p->hwnd = CreateDialogIndirectParamW(p->psp.hInstance, t, s->hwnd, p->psp.pfnDlgProc, (LPARAM)&p->psp);
    free(t);
    if (!p->hwnd) return FALSE;
    place_page(s, p);
    return TRUE;
}

static void show_page(Sheet *s, int i, int show)
{
    Page *p = s->pages[i];
    if (!p->hwnd) return;
    ShowWindow(p->hwnd, show ? SW_SHOW : SW_HIDE);
    if (show) {
        HWND f = GetNextDlgTabItem(p->hwnd, NULL, FALSE);
        if (f) SetFocus(f);
    }
}

static void update_buttons(Sheet *s)
{
    if (s->wizard) return;
    int any = 0;
    for (int i = 0; i < s->n; i++) if (s->pages[i]->changed) any = 1;
    HWND a = GetDlgItem(s->hwnd, ID_APPLY);
    if (a) EnableWindow(a, any);
}

static void set_title(Sheet *s, LPCWSTR t, DWORD style)
{
    WCHAR buf[300];
    int n = 0;
    if ((ULONG_PTR)t < 0x10000 && s->hdr.hInstance) { LoadStringW(s->hdr.hInstance, (UINT)(ULONG_PTR)t, buf, 260); t = buf; }
    else if (t) { for (; t[n] && n < 259; n++) buf[n] = t[n]; buf[n] = 0; t = buf; }
    else buf[0] = 0;
    if (style & PSH_PROPTITLE) {
        WCHAR full[300];
        int k = 0;
        for (const WCHAR *c = buf; *c && k < 280; c++) full[k++] = *c;
        for (const WCHAR *c = L" Properties"; *c && k < 299; c++) full[k++] = *c;
        full[k] = 0;
        SetWindowTextW(s->hwnd, full);
    } else SetWindowTextW(s->hwnd, buf);
}

/* make page I current (creating it), asking the old page first */
static BOOL activate(Sheet *s, int i, int from_tab)
{
    if (i < 0 || i >= s->n) return FALSE;
    if (s->cur >= 0 && s->cur < s->n && s->pages[s->cur]->hwnd) {
        if (page_notify(s, s->cur, PSN_KILLACTIVE, 0)) return FALSE;
    }
    int tries = 0;
    for (;;) {
        if (!create_page(s, i)) return FALSE;
        LRESULT r = page_notify(s, i, PSN_SETACTIVE, 0);
        if (r == 0) break;
        /* -1: stay on the old page; an id: go to that page instead */
        if (r == -1) { i = s->cur >= 0 ? s->cur : 0; if (++tries > 2) break; continue; }
        int k = -1;
        for (int j = 0; j < s->n; j++) if ((ULONG_PTR)s->pages[j]->psp.pszTemplate == (ULONG_PTR)r) k = j;
        if (k < 0 || ++tries > s->n) break;
        i = k;
    }
    int old = s->cur;
    s->cur = i;
    if (old >= 0 && old != i && old < s->n) show_page(s, old, 0);
    if (!from_tab && s->tab) SendMessageW(s->tab, TCM_SETCURSEL, (WPARAM)i, 0);
    show_page(s, i, 1);
    if (s->wizard) {
        /* the title says where we are */
        Page *p = s->pages[i];
        if (p->title) SetWindowTextW(s->hwnd, p->title);
    }
    return TRUE;
}

static int apply_all(Sheet *s, int closing)
{
    if (s->cur >= 0 && page_notify(s, s->cur, PSN_KILLACTIVE, 0)) return 0;
    for (int i = 0; i < s->n; i++) {
        if (!s->pages[i]->hwnd) continue;
        LRESULT r = page_notify(s, i, PSN_APPLY, closing);
        if (r == PSNRET_INVALID || r == PSNRET_INVALID_NOCHANGEPAGE) {
            if (r == PSNRET_INVALID) activate(s, i, 0);
            else if (s->cur >= 0) page_notify(s, s->cur, PSN_SETACTIVE, 0);
            return 0;
        }
        s->pages[i]->changed = 0;
    }
    if (!closing && s->cur >= 0) page_notify(s, s->cur, PSN_SETACTIVE, 0);
    s->result = s->restart ? s->restart : 1;
    update_buttons(s);
    return 1;
}

static void finish(Sheet *s, int result)
{
    s->result = result;
    if (s->modeless) DestroyWindow(s->hwnd);
    else EndDialog(s->hwnd, result);
}

static void do_cancel(Sheet *s)
{
    if (s->cur >= 0 && page_notify(s, s->cur, PSN_QUERYCANCEL, 0)) return;
    for (int i = 0; i < s->n; i++) if (s->pages[i]->hwnd) page_notify(s, i, PSN_RESET, 0);
    finish(s, s->cancel_to_close ? s->result : 0);
}

static void set_wiz_buttons(Sheet *s, DWORD flags)
{
    HWND back = GetDlgItem(s->hwnd, ID_BACK), next = GetDlgItem(s->hwnd, ID_NEXT), fin = GetDlgItem(s->hwnd, ID_FINISH);
    EnableWindow(back, (flags & PSWIZB_BACK) != 0);
    int show_finish = (flags & (PSWIZB_FINISH | PSWIZB_DISABLEDFINISH)) != 0;
    ShowWindow(next, show_finish ? SW_HIDE : SW_SHOW);
    ShowWindow(fin, show_finish ? SW_SHOW : SW_HIDE);
    EnableWindow(next, (flags & PSWIZB_NEXT) != 0);
    EnableWindow(fin, (flags & PSWIZB_FINISH) != 0);
    SendMessageW(s->hwnd, DM_SETDEFID, show_finish ? ID_FINISH : ID_NEXT, 0);
}

static int page_index(Sheet *s, HWND h)
{
    for (int i = 0; i < s->n; i++) if (s->pages[i]->hwnd == h) return i;
    return -1;
}

static int page_from_id(Sheet *s, LPCWSTR id)
{
    for (int i = 0; i < s->n; i++) if (!(s->pages[i]->psp.dwFlags & PSP_DLGINDIRECT) && s->pages[i]->psp.pszTemplate == id) return i;
    return -1;
}

static void add_page(Sheet *s, int at, Page *p)
{
    if (s->n == s->cap) {
        int c = s->cap ? s->cap * 2 : 8;
        Page **v = realloc(s->pages, sizeof(Page *) * (size_t)c);
        if (!v) return;
        s->pages = v; s->cap = c;
    }
    if (at < 0 || at > s->n) at = s->n;
    memmove(&s->pages[at + 1], &s->pages[at], sizeof(Page *) * (size_t)(s->n - at));
    s->pages[at] = p;
    s->n++;
    if (s->tab) {
        TCITEMW ti;
        memset(&ti, 0, sizeof(ti));
        ti.mask = TCIF_TEXT;
        WCHAR buf[256];
        const WCHAR *title = p->title;
        if (!title) {
            DWORD n;
            const void *t = page_template(p, &n);
            if (t) { TmplHdr h; tmpl_header(t, &h); title = h.title[0] == 0xFFFF ? L"" : h.title; }
        }
        if ((p->psp.dwFlags & PSP_USETITLE) && (ULONG_PTR)p->psp.pszTitle < 0x10000) { LoadStringW(p->psp.hInstance, (UINT)(ULONG_PTR)p->psp.pszTitle, buf, 256); title = buf; }
        ti.pszText = (LPWSTR)(title ? title : L"");
        SendMessageW(s->tab, TCM_INSERTITEMW, (WPARAM)at, (LPARAM)&ti);
    }
    if (s->cur >= at && at < s->n - 1) s->cur++;
}

static void remove_page(Sheet *s, int i)
{
    if (i < 0 || i >= s->n) return;
    Page *p = s->pages[i];
    if (p->hwnd) DestroyWindow(p->hwnd);
    p->hwnd = NULL;
    memmove(&s->pages[i], &s->pages[i + 1], sizeof(Page *) * (size_t)(s->n - i - 1));
    s->n--;
    if (s->tab) SendMessageW(s->tab, TCM_DELETEITEM, (WPARAM)i, 0);
    DestroyPropertySheetPage((HPROPSHEETPAGE)p);
    if (s->cur == i) { s->cur = -1; if (s->n) activate(s, MIN(i, s->n - 1), 0); }
    else if (s->cur > i) s->cur--;
}

static void layout(Sheet *s)
{
    RECT c;
    GetClientRect(s->hwnd, &c);
    if (s->tab) {
        RECT t;
        GetWindowRect(s->tab, &t);
        MapWindowPoints(NULL, s->hwnd, (POINT *)&t, 2);
        RECT d = t;
        SendMessageW(s->tab, TCM_ADJUSTRECT, FALSE, (LPARAM)&d);
        s->page_rect = d;
    } else {
        RECT b = { 7, 7, 7, 7 };
        MapDialogRect(s->hwnd, &b);
        HWND cancel = GetDlgItem(s->hwnd, IDCANCEL);
        RECT cr;
        GetWindowRect(cancel, &cr);
        MapWindowPoints(NULL, s->hwnd, (POINT *)&cr, 2);
        SetRect(&s->page_rect, b.left, b.top, c.right - b.left, cr.top - b.top);
    }
    for (int i = 0; i < s->n; i++) place_page(s, s->pages[i]);
}

static INT_PTR CALLBACK sheet_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    Sheet *s = sheet_of(h);
    switch (msg) {
    case WM_INITDIALOG: {
        s = (Sheet *)lp;
        s->hwnd = h;
        SetWindowLongPtrW(h, DWLP_USER, (LONG_PTR)s);
        s->tab = GetDlgItem(h, ID_TAB);
        if (s->hdr.dwFlags & PSH_USEHICON) SendMessageW(h, WM_SETICON, ICON_SMALL, (LPARAM)s->hdr.hIcon);
        else if ((s->hdr.dwFlags & PSH_USEICONID) && s->hdr.pszIcon) SendMessageW(h, WM_SETICON, ICON_SMALL, (LPARAM)LoadIconW(s->hdr.hInstance, s->hdr.pszIcon));
        if (!s->wizard) set_title(s, s->hdr.pszCaption, s->hdr.dwFlags);
        /* the pages given so far go into the tab now (the tab exists) */
        int n = s->n;
        s->n = 0;
        for (int i = 0; i < n; i++) add_page(s, i, s->pages[i]);
        layout(s);
        if (s->finish_text) SetDlgItemTextW(h, ID_FINISH, s->finish_text);
        if ((s->hdr.dwFlags & PSH_USECALLBACK) && s->hdr.pfnCallback) s->hdr.pfnCallback(h, PSCB_INITIALIZED, 0);
        int start = 0;
        if (s->hdr.dwFlags & PSH_USEPSTARTPAGE) { int k = page_from_id(s, s->hdr.pStartPage); if (k >= 0) start = k; }
        else if (s->hdr.nStartPage < (UINT)s->n) start = (int)s->hdr.nStartPage;
        s->cur = -1;
        activate(s, start, 0);
        update_buttons(s);
        if (s->wizard) set_wiz_buttons(s, PSWIZB_NEXT);
        return FALSE;
    }
    case WM_DESTROY:
        if (s) {
            for (int i = 0; i < s->n; i++) if (s->pages[i]->hwnd) { DestroyWindow(s->pages[i]->hwnd); s->pages[i]->hwnd = NULL; }
        }
        return 0;
    case WM_NCDESTROY:
        if (s && s->modeless) {                             /* a modeless sheet cleans up after itself */
            for (int i = 0; i < s->n; i++) DestroyPropertySheetPage((HPROPSHEETPAGE)s->pages[i]);
            free(s->pages); free(s->finish_text); free(s);
            SetWindowLongPtrW(h, DWLP_USER, 0);
        }
        return 0;
    case WM_SIZE: if (s) layout(s); return 0;
    case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)lp;
        if (!s || !nm || nm->hwndFrom != s->tab) return 0;
        if (nm->code == TCN_SELCHANGING) {
            if (s->cur >= 0 && page_notify(s, s->cur, PSN_KILLACTIVE, 0)) { SetWindowLongPtrW(h, DWLP_MSGRESULT, TRUE); return TRUE; }
            return 0;
        }
        if (nm->code == TCN_SELCHANGE) {
            int i = (int)SendMessageW(s->tab, TCM_GETCURSEL, 0, 0);
            if (i >= 0 && i != s->cur) {
                int old = s->cur;
                s->cur = -1;                                /* KILLACTIVE was already asked */
                if (!create_page(s, i)) { s->cur = old; return 0; }
                LRESULT r = page_notify(s, i, PSN_SETACTIVE, 0);
                if (r == -1 && old >= 0) { SendMessageW(s->tab, TCM_SETCURSEL, (WPARAM)old, 0); i = old; }
                s->cur = i;
                if (old >= 0 && old != i) show_page(s, old, 0);
                show_page(s, i, 1);
            }
            return 0;
        }
        return 0;
    }
    case WM_COMMAND:
        if (!s) return 0;
        switch (LOWORD(wp)) {
        case IDOK:
            if (s->wizard) { SendMessageW(h, WM_COMMAND, ID_NEXT, 0); return TRUE; }
            if (apply_all(s, 1)) finish(s, s->result);
            return TRUE;
        case IDCANCEL: do_cancel(s); return TRUE;
        case ID_APPLY:
            if (apply_all(s, 0)) { if (s->cancel_to_close) {} }
            return TRUE;
        case IDHELP: if (s->cur >= 0) page_notify(s, s->cur, PSN_HELP, 0); return TRUE;
        case ID_BACK: case ID_NEXT: {
            if (s->cur < 0) return TRUE;
            LRESULT r = page_notify(s, s->cur, LOWORD(wp) == ID_BACK ? PSN_WIZBACK : PSN_WIZNEXT, 0);
            if (r == -1) return TRUE;
            int i;
            if (r) { i = page_from_id(s, (LPCWSTR)r); if (i < 0) return TRUE; }
            else i = s->cur + (LOWORD(wp) == ID_BACK ? -1 : 1);
            if (i < 0 || i >= s->n) return TRUE;
            int old = s->cur;
            s->cur = -1;
            if (!create_page(s, i)) { s->cur = old; return TRUE; }
            page_notify(s, i, PSN_SETACTIVE, 0);
            s->cur = i;
            if (old >= 0) show_page(s, old, 0);
            show_page(s, i, 1);
            if (s->pages[i]->title) SetWindowTextW(h, s->pages[i]->title);
            return TRUE;
        }
        case ID_FINISH: {
            if (s->cur < 0) return TRUE;
            LRESULT r = page_notify(s, s->cur, PSN_WIZFINISH, 0);
            if (r) return TRUE;
            finish(s, 1);
            return TRUE;
        }
        }
        return 0;
    case WM_CLOSE: if (s) do_cancel(s); return TRUE;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_CLOSE && s) { do_cancel(s); return TRUE; }
        return 0;

    case PSM_SETCURSEL: {
        if (!s) return FALSE;
        int i = (int)wp;
        if (lp) { Page *p = page_of((HPROPSHEETPAGE)lp); for (int k = 0; k < s->n; k++) if (s->pages[k] == p) i = k; }
        BOOL ok = activate(s, i, 0);
        SetWindowLongPtrW(h, DWLP_MSGRESULT, ok);
        return ok;
    }
    case PSM_SETCURSELID: {                                 /* also PSM_INSERTPAGE: same number, a page handle instead of an id */
        if (!s) return FALSE;
        Page *np = (ULONG_PTR)lp > 0xFFFF ? page_of((HPROPSHEETPAGE)lp) : NULL;
        if (np) {
            int at = (int)wp;
            if (wp > 0xFFFF) { at = -1; for (int k = 0; k < s->n; k++) if (s->pages[k] == (Page *)wp) at = k + 1; }
            add_page(s, at, np);
            return TRUE;
        }
        int i = page_from_id(s, (LPCWSTR)lp);
        BOOL ok = i >= 0 && activate(s, i, 0);
        SetWindowLongPtrW(h, DWLP_MSGRESULT, ok);
        return ok;
    }
    case PSM_CHANGED: { int i = s ? page_index(s, (HWND)wp) : -1; if (i >= 0) s->pages[i]->changed = 1; if (s) update_buttons(s); return TRUE; }
    case PSM_UNCHANGED: { int i = s ? page_index(s, (HWND)wp) : -1; if (i >= 0) s->pages[i]->changed = 0; if (s) update_buttons(s); return TRUE; }
    case PSM_GETCURRENTPAGEHWND: { HWND r = s && s->cur >= 0 ? s->pages[s->cur]->hwnd : NULL; SetWindowLongPtrW(h, DWLP_MSGRESULT, (LONG_PTR)r); return (INT_PTR)r; }
    case PSM_GETTABCONTROL: SetWindowLongPtrW(h, DWLP_MSGRESULT, (LONG_PTR)(s ? s->tab : NULL)); return (INT_PTR)(s ? s->tab : NULL);
    case PSM_PRESSBUTTON: {
        static const int ids[] = { ID_BACK, ID_NEXT, ID_FINISH, IDOK, ID_APPLY, IDCANCEL, IDHELP };
        if (wp < 7) PostMessageW(h, WM_COMMAND, (WPARAM)ids[wp], 0);
        return TRUE;
    }
    case PSM_SETTITLEW: if (s) set_title(s, (LPCWSTR)lp, (DWORD)wp); return TRUE;
    case PSM_SETTITLEA: {
        WCHAR w[260];
        if ((ULONG_PTR)lp > 0xFFFF) { MultiByteToWideChar(CP_ACP, 0, (LPCSTR)lp, -1, w, 260); if (s) set_title(s, w, (DWORD)wp); }
        else if (s) set_title(s, (LPCWSTR)lp, (DWORD)wp);
        return TRUE;
    }
    case PSM_APPLY: { BOOL ok = s ? apply_all(s, 0) : FALSE; SetWindowLongPtrW(h, DWLP_MSGRESULT, ok); return ok; }
    case PSM_QUERYSIBLINGS: {
        if (!s) return 0;
        for (int i = 0; i < s->n; i++) {
            if (!s->pages[i]->hwnd) continue;
            LRESULT r = SendMessageW(s->pages[i]->hwnd, PSM_QUERYSIBLINGS, wp, lp);
            if (r) { SetWindowLongPtrW(h, DWLP_MSGRESULT, r); return r; }
        }
        SetWindowLongPtrW(h, DWLP_MSGRESULT, 0);
        return 0;
    }
    case PSM_ADDPAGE: { Page *p = page_of((HPROPSHEETPAGE)lp); if (s && p) add_page(s, -1, p); return p != NULL; }
    case PSM_REMOVEPAGE: {
        if (!s) return FALSE;
        int i = (int)wp;
        if (lp) { Page *p = page_of((HPROPSHEETPAGE)lp); for (int k = 0; k < s->n; k++) if (s->pages[k] == p) i = k; }
        remove_page(s, i);
        return TRUE;
    }
    case PSM_SETWIZBUTTONS: if (s && s->wizard) set_wiz_buttons(s, (DWORD)lp); return TRUE;
    case PSM_SETFINISHTEXTW: if (s) { SetDlgItemTextW(h, ID_FINISH, (LPCWSTR)lp); set_wiz_buttons(s, PSWIZB_FINISH | PSWIZB_BACK); } return TRUE;
    case PSM_SETFINISHTEXTA: if (s) { SetDlgItemTextA(h, ID_FINISH, (LPCSTR)lp); set_wiz_buttons(s, PSWIZB_FINISH | PSWIZB_BACK); } return TRUE;
    case PSM_CANCELTOCLOSE: if (s) { s->cancel_to_close = 1; SetDlgItemTextW(h, IDCANCEL, L"Close"); EnableWindow(GetDlgItem(h, IDOK), FALSE); } return TRUE;
    case PSM_RESTARTWINDOWS: if (s) s->restart = ID_PSRESTARTWINDOWS; return TRUE;
    case PSM_REBOOTSYSTEM: if (s) s->restart = ID_PSREBOOTSYSTEM; return TRUE;
    case PSM_ISDIALOGMESSAGE: { BOOL r = IsDialogMessageW(h, (LPMSG)lp); SetWindowLongPtrW(h, DWLP_MSGRESULT, r); return r; }
    case PSM_GETRESULT: SetWindowLongPtrW(h, DWLP_MSGRESULT, s ? s->result : -1); return s ? s->result : -1;
    case PSM_HWNDTOINDEX: { int i = s ? page_index(s, (HWND)wp) : -1; SetWindowLongPtrW(h, DWLP_MSGRESULT, i); return i; }
    case PSM_INDEXTOHWND: { HWND r = s && wp < (UINT)s->n ? s->pages[wp]->hwnd : NULL; SetWindowLongPtrW(h, DWLP_MSGRESULT, (LONG_PTR)r); return (INT_PTR)r; }
    case PSM_PAGETOINDEX: { int i = -1; if (s) for (int k = 0; k < s->n; k++) if (s->pages[k] == (Page *)wp) i = k; SetWindowLongPtrW(h, DWLP_MSGRESULT, i); return i; }
    case PSM_INDEXTOPAGE: { Page *r = s && wp < (UINT)s->n ? s->pages[wp] : NULL; SetWindowLongPtrW(h, DWLP_MSGRESULT, (LONG_PTR)r); return (INT_PTR)r; }
    case PSM_IDTOINDEX: { int i = s ? page_from_id(s, (LPCWSTR)lp) : -1; SetWindowLongPtrW(h, DWLP_MSGRESULT, i); return i; }
    case PSM_INDEXTOID: { LONG_PTR r = s && wp < (UINT)s->n && !(s->pages[wp]->psp.dwFlags & PSP_DLGINDIRECT) ? (LONG_PTR)s->pages[wp]->psp.pszTemplate : 0; SetWindowLongPtrW(h, DWLP_MSGRESULT, r); return r; }
    case PSM_RECALCPAGESIZES: if (s) layout(s); return TRUE;
    case PSM_SETHEADERTITLEW: case PSM_SETHEADERSUBTITLEW: return TRUE;
    }
    return FALSE;
}

/* -----------------------------------------------------------------------
 * Building the sheet
 * ----------------------------------------------------------------------- */
typedef struct { WORD *p; } Tb;
static void t_word(Tb *t, WORD w) { *t->p++ = w; }
static void t_str(Tb *t, const WCHAR *s) { do t_word(t, *s); while (*s++); }
static void t_align(Tb *t) { if ((ULONG_PTR)t->p & 2) t_word(t, 0); }
static void t_item(Tb *t, DWORD style, DWORD ex, short x, short y, short cx, short cy, WORD id, const WCHAR *cls, WORD atom, const WCHAR *text)
{
    t_align(t);
    DLGITEMTEMPLATE *it = (DLGITEMTEMPLATE *)t->p;
    it->style = style | WS_CHILD | WS_VISIBLE; it->dwExtendedStyle = ex;
    it->x = x; it->y = y; it->cx = cx; it->cy = cy; it->id = id;
    t->p = (WORD *)(it + 1);
    if (cls) t_str(t, cls); else { t_word(t, 0xFFFF); t_word(t, atom); }
    t_str(t, text);
    t_word(t, 0);
}

static void page_size(Page *p, int *cx, int *cy)
{
    DWORD n;
    const void *t = page_template(p, &n);
    if (!t) return;
    TmplHdr h;
    tmpl_header(t, &h);
    if (h.cx > *cx) *cx = h.cx;
    if (h.cy > *cy) *cy = h.cy;
}

CC INT_PTR WINAPI PropertySheetW(LPCPROPSHEETHEADERW hdr)
{
    if (!hdr || hdr->dwSize < 40) return -1;
    Sheet *s = calloc(1, sizeof(Sheet));
    if (!s) return -1;
    memcpy(&s->hdr, hdr, MIN(hdr->dwSize, (DWORD)sizeof(PROPSHEETHEADERW)));
    s->wizard = (hdr->dwFlags & (PSH_WIZARD | PSH_WIZARD97 | PSH_WIZARD_LITE | PSH_AEROWIZARD)) != 0;
    s->modeless = (hdr->dwFlags & PSH_MODELESS) != 0;
    s->cur = -1;
    for (UINT i = 0; i < hdr->nPages; i++) {
        Page *p;
        if (hdr->dwFlags & PSH_PROPSHEETPAGE) {
            const BYTE *b = (const BYTE *)hdr->ppsp;
            const PROPSHEETPAGEW *src = (const PROPSHEETPAGEW *)(b + (size_t)i * hdr->ppsp[0].dwSize);
            p = page_of(CreatePropertySheetPageW(src));
        } else p = page_of(hdr->phpage[i]);
        if (!p) continue;
        add_page(s, -1, p);
    }
    if (!s->n) { free(s->pages); free(s); return -1; }

    int pcx = 200, pcy = 120;
    for (int i = 0; i < s->n; i++) page_size(s->pages[i], &pcx, &pcy);
    int nbtn = s->wizard ? 3 : (2 + !(hdr->dwFlags & PSH_NOAPPLYNOW) + ((hdr->dwFlags & PSH_HASHELP) != 0));
    int bw = 50, bh = 14, gap = 6, m = 7;
    int min_w = nbtn * bw + (nbtn - 1) * gap + 2 * m;
    int tab_extra = s->wizard ? 0 : 16;                    /* the tab row and frame */
    int W = MAX(pcx + 2 * m + 6, min_w), H = pcy + tab_extra + 2 * m + bh + gap + 4;
    static DWORD buf[1024];
    Tb t = { (WORD *)buf };
    DLGTEMPLATE *d = (DLGTEMPLATE *)t.p;
    d->style = DS_MODALFRAME | DS_SETFONT | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU | (hdr->dwFlags & PSH_NOCONTEXTHELP ? 0 : 0);
    d->dwExtendedStyle = 0;
    d->cdit = (WORD)(nbtn + (s->wizard ? 0 : 1) + (s->wizard ? 1 : 0));
    d->x = d->y = 0; d->cx = (short)W; d->cy = (short)H;
    t.p = (WORD *)(d + 1);
    t_word(&t, 0); t_word(&t, 0);
    t_str(&t, s->wizard ? L"Wizard" : L"Properties");
    t_word(&t, 8); t_str(&t, L"MS Shell Dlg");
    int by = H - m - bh;
    int bx = W - m - nbtn * bw - (nbtn - 1) * gap;
    if (s->wizard) {
        t_item(&t, BS_PUSHBUTTON | WS_TABSTOP, 0, (short)bx, (short)by, bw, bh, ID_BACK, NULL, 0x80, L"< &Back");
        bx += bw + gap;
        t_item(&t, BS_DEFPUSHBUTTON | WS_TABSTOP, 0, (short)bx, (short)by, bw, bh, ID_NEXT, NULL, 0x80, L"&Next >");
        t_item(&t, BS_PUSHBUTTON | WS_TABSTOP, 0, (short)bx, (short)by, bw, bh, ID_FINISH, NULL, 0x80, L"Finish");
        bx += bw + gap;
        t_item(&t, BS_PUSHBUTTON | WS_TABSTOP, 0, (short)bx, (short)by, bw, bh, IDCANCEL, NULL, 0x80, L"Cancel");
        /* the line above the buttons */
        t_item(&t, SS_ETCHEDHORZ, 0, (short)m, (short)(by - gap - 1), (short)(W - 2 * m), 1, 0xFFFF, NULL, 0x82, L"");
    } else {
        t_item(&t, WS_TABSTOP | WS_CLIPSIBLINGS, 0, (short)m, (short)m, (short)(W - 2 * m), (short)(H - 2 * m - bh - gap), ID_TAB, WC_TABCONTROLW, 0, L"");
        t_item(&t, BS_DEFPUSHBUTTON | WS_TABSTOP, 0, (short)bx, (short)by, bw, bh, IDOK, NULL, 0x80, L"OK");
        bx += bw + gap;
        t_item(&t, BS_PUSHBUTTON | WS_TABSTOP, 0, (short)bx, (short)by, bw, bh, IDCANCEL, NULL, 0x80, L"Cancel");
        bx += bw + gap;
        if (!(hdr->dwFlags & PSH_NOAPPLYNOW)) { t_item(&t, BS_PUSHBUTTON | WS_TABSTOP | WS_DISABLED, 0, (short)bx, (short)by, bw, bh, ID_APPLY, NULL, 0x80, L"&Apply"); bx += bw + gap; }
        if (hdr->dwFlags & PSH_HASHELP) t_item(&t, BS_PUSHBUTTON | WS_TABSTOP, 0, (short)bx, (short)by, bw, bh, IDHELP, NULL, 0x80, L"Help");
    }
    if ((hdr->dwFlags & PSH_USECALLBACK) && hdr->pfnCallback) hdr->pfnCallback(NULL, PSCB_PRECREATE, (LPARAM)buf);

    HINSTANCE inst = hdr->hInstance ? hdr->hInstance : GetModuleHandleW(NULL);
    INT_PTR r;
    if (s->modeless) {
        HWND h = CreateDialogIndirectParamW(inst, (LPCDLGTEMPLATEW)buf, hdr->hwndParent, sheet_proc, (LPARAM)s);
        if (!h) { for (int i = 0; i < s->n; i++) DestroyPropertySheetPage((HPROPSHEETPAGE)s->pages[i]); free(s->pages); free(s); return -1; }
        ShowWindow(h, SW_SHOW);
        return (INT_PTR)h;                                  /* the sheet frees itself when destroyed: see below */
    }
    r = DialogBoxIndirectParamW(inst, (LPCDLGTEMPLATEW)buf, hdr->hwndParent, sheet_proc, (LPARAM)s);
    if (r == -1) r = -1; else r = s->result;
    for (int i = 0; i < s->n; i++) DestroyPropertySheetPage((HPROPSHEETPAGE)s->pages[i]);
    free(s->pages);
    free(s->finish_text);
    free(s);
    return r;
}

CC INT_PTR WINAPI PropertySheetA(const void *hdr_a)
{
    const PROPSHEETHEADERW *a = hdr_a;
    if (!a) return -1;
    PROPSHEETHEADERW w;
    memset(&w, 0, sizeof(w));
    memcpy(&w, a, MIN(a->dwSize, (DWORD)sizeof(w)));
    WCHAR cap[260];
    if (a->pszCaption && (ULONG_PTR)a->pszCaption > 0xFFFF) { MultiByteToWideChar(CP_ACP, 0, (const char *)a->pszCaption, -1, cap, 260); w.pszCaption = cap; }
    if (a->dwFlags & PSH_PROPSHEETPAGE) {
        /* the pages: made here as W pages */
        HPROPSHEETPAGE *pages = calloc(a->nPages ? a->nPages : 1, sizeof(HPROPSHEETPAGE));
        if (!pages) return -1;
        const BYTE *b = (const BYTE *)a->ppsp;
        UINT n = 0;
        for (UINT i = 0; i < a->nPages; i++) {
            HPROPSHEETPAGE p = CreatePropertySheetPageA(b + (size_t)i * a->ppsp[0].dwSize);
            if (p) pages[n++] = p;
        }
        w.dwFlags &= ~PSH_PROPSHEETPAGE;
        w.nPages = n;
        w.phpage = pages;
        INT_PTR r = PropertySheetW(&w);
        free(pages);
        return r;
    }
    return PropertySheetW(&w);
}
