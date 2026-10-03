/*
 * comboex.c — ComboBoxEx32: a combo box whose items carry an image, an
 * indent and an lParam. It wraps a plain ComboBox child (with its edit
 * box for CBS_DROPDOWN); the child's notifications go on to our parent as
 * our own, and Enter or leaving the edit box reports CBEN_ENDEDIT.
 */
#include "cc.h"

#define CBEM_INSERTITEMA_ (WM_USER + 1)
#define CBEM_SETITEMA_    (WM_USER + 5)
#define CBEM_GETITEMA_    (WM_USER + 4)
#define CBEN_ENDEDITA_    (CBEN_FIRST - 5)
#define CBEN_GETDISPINFOA_ (CBEN_FIRST - 0)
#define CBEM_SETUNICODEFORMAT_ 0x2005
#define CBEM_GETUNICODEFORMAT_ 0x2006

typedef struct { WCHAR *text; int image, sel_image, overlay, indent; LPARAM lp; } CItem;

typedef struct {
    HWND combo, edit;
    CItem *it;
    int n, cap;
    HIMAGELIST il;
    DWORD ex;
    int changed;                    /* the edit text changed since the last CBEN_ENDEDIT */
    int began;                      /* CBEN_BEGINEDIT sent */
    HFONT font;
} CBX;

static LRESULT CALLBACK edit_sub(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref);

static void set_text(WCHAR **slot, const void *t, int wide)
{
    wfree(*slot);
    *slot = NULL;
    if (!t) return;
    if (wide || t == LPSTR_TEXTCALLBACKW) { *slot = wdup(t); return; }
    int n = MultiByteToWideChar(CP_ACP, 0, t, -1, NULL, 0);
    *slot = malloc(2 * (size_t)(n > 0 ? n : 1));
    if (*slot) { if (n > 0) MultiByteToWideChar(CP_ACP, 0, t, -1, *slot, n); else (*slot)[0] = 0; }
}

static const WCHAR *item_text(HWND h, CBX *s, int i, WCHAR *buf, int cap)
{
    if (i < 0 || i >= s->n) return L"";
    if (s->it[i].text != LPSTR_TEXTCALLBACKW) return s->it[i].text ? s->it[i].text : L"";
    NMCOMBOBOXEXW nm;
    memset(&nm, 0, sizeof(nm));
    nm.ceItem.mask = CBEIF_TEXT;
    nm.ceItem.iItem = i;
    nm.ceItem.pszText = buf;
    nm.ceItem.cchTextMax = cap;
    nm.ceItem.lParam = s->it[i].lp;
    buf[0] = 0;
    cc_notify(h, CBEN_GETDISPINFOW, &nm.hdr);
    return nm.ceItem.pszText ? nm.ceItem.pszText : L"";
}

/* the text an item shows in the inner combo (indented with spaces) */
static void combo_string(HWND h, CBX *s, int i, WCHAR *out, int cap)
{
    WCHAR b[520];
    const WCHAR *t = item_text(h, s, i, b, 520);
    int n = 0;
    int ind = s->it[i].indent;
    (void)ind;
    while (*t && n < cap - 1) out[n++] = *t++;
    out[n] = 0;
}

static int insert_item(HWND h, CBX *s, const COMBOBOXEXITEMW *in, int wide)
{
    if (!in) return -1;
    if (s->n == s->cap) {
        int c = s->cap ? s->cap * 2 : 16;
        CItem *p = realloc(s->it, sizeof(CItem) * (size_t)c);
        if (!p) return -1;
        s->it = p; s->cap = c;
    }
    int at = (int)in->iItem;
    if (at < 0 || at > s->n) at = s->n;
    memmove(&s->it[at + 1], &s->it[at], sizeof(CItem) * (size_t)(s->n - at));
    CItem *it = &s->it[at];
    memset(it, 0, sizeof(*it));
    it->image = it->sel_image = -1;
    if (in->mask & CBEIF_TEXT) set_text(&it->text, in->pszText, wide);
    if (in->mask & CBEIF_IMAGE) it->image = in->iImage;
    if (in->mask & CBEIF_SELECTEDIMAGE) it->sel_image = in->iSelectedImage;
    if (in->mask & CBEIF_OVERLAY) it->overlay = in->iOverlay;
    if (in->mask & CBEIF_INDENT) it->indent = in->iIndent;
    if (in->mask & CBEIF_LPARAM) it->lp = in->lParam;
    s->n++;
    WCHAR b[520];
    combo_string(h, s, at, b, 520);
    SendMessageW(s->combo, CB_INSERTSTRING, (WPARAM)at, (LPARAM)b);
    NMCOMBOBOXEXW nm;
    memset(&nm, 0, sizeof(nm));
    nm.ceItem = *in;
    nm.ceItem.iItem = at;
    cc_notify(h, CBEN_INSERTITEM, &nm.hdr);
    return at;
}

static BOOL set_item(HWND h, CBX *s, const COMBOBOXEXITEMW *in, int wide)
{
    if (!in) return FALSE;
    int i = (int)in->iItem;
    if (i == -1) {                              /* the edit box / selection field */
        if ((in->mask & CBEIF_TEXT) && in->pszText) {
            if (wide) SetWindowTextW(s->combo, in->pszText);
            else SetWindowTextA(s->combo, (const char *)in->pszText);
            s->changed = 0;
        }
        return TRUE;
    }
    if (i < 0 || i >= s->n) return FALSE;
    CItem *it = &s->it[i];
    if (in->mask & CBEIF_TEXT) set_text(&it->text, in->pszText, wide);
    if (in->mask & CBEIF_IMAGE) it->image = in->iImage;
    if (in->mask & CBEIF_SELECTEDIMAGE) it->sel_image = in->iSelectedImage;
    if (in->mask & CBEIF_OVERLAY) it->overlay = in->iOverlay;
    if (in->mask & CBEIF_INDENT) it->indent = in->iIndent;
    if (in->mask & CBEIF_LPARAM) it->lp = in->lParam;
    if (in->mask & CBEIF_TEXT) {
        int cur = (int)SendMessageW(s->combo, CB_GETCURSEL, 0, 0);
        WCHAR b[520];
        combo_string(h, s, i, b, 520);
        SendMessageW(s->combo, CB_DELETESTRING, (WPARAM)i, 0);
        SendMessageW(s->combo, CB_INSERTSTRING, (WPARAM)i, (LPARAM)b);
        if (cur == i) SendMessageW(s->combo, CB_SETCURSEL, (WPARAM)i, 0);
    }
    return TRUE;
}

static BOOL get_item(HWND h, CBX *s, COMBOBOXEXITEMW *out, int wide)
{
    if (!out) return FALSE;
    int i = (int)out->iItem;
    if (i == -1) {
        if ((out->mask & CBEIF_TEXT) && out->pszText && out->cchTextMax > 0) {
            if (wide) GetWindowTextW(s->combo, out->pszText, out->cchTextMax);
            else GetWindowTextA(s->combo, (char *)out->pszText, out->cchTextMax);
        }
        int cur = (int)SendMessageW(s->combo, CB_GETCURSEL, 0, 0);
        if (cur >= 0 && cur < s->n) {
            if (out->mask & CBEIF_IMAGE) out->iImage = s->it[cur].image;
            if (out->mask & CBEIF_SELECTEDIMAGE) out->iSelectedImage = s->it[cur].sel_image;
            if (out->mask & CBEIF_LPARAM) out->lParam = s->it[cur].lp;
            if (out->mask & CBEIF_INDENT) out->iIndent = s->it[cur].indent;
        }
        return TRUE;
    }
    if (i < 0 || i >= s->n) return FALSE;
    CItem *it = &s->it[i];
    if ((out->mask & CBEIF_TEXT) && out->pszText && out->cchTextMax > 0) {
        WCHAR b[520];
        const WCHAR *t = item_text(h, s, i, b, 520);
        if (wide) { int n = MIN(wlen(t), out->cchTextMax - 1); memcpy(out->pszText, t, 2 * (size_t)n); out->pszText[n] = 0; }
        else WideCharToMultiByte(CP_ACP, 0, t, -1, (char *)out->pszText, out->cchTextMax, NULL, NULL);
    }
    if (out->mask & CBEIF_IMAGE) out->iImage = it->image;
    if (out->mask & CBEIF_SELECTEDIMAGE) out->iSelectedImage = it->sel_image;
    if (out->mask & CBEIF_OVERLAY) out->iOverlay = it->overlay;
    if (out->mask & CBEIF_INDENT) out->iIndent = it->indent;
    if (out->mask & CBEIF_LPARAM) out->lParam = it->lp;
    return TRUE;
}

static void end_edit(HWND h, CBX *s, int why)
{
    NMCBEENDEDITW nm;
    memset(&nm, 0, sizeof(nm));
    nm.fChanged = s->changed;
    nm.iNewSelection = (int)SendMessageW(s->combo, CB_GETCURSEL, 0, 0);
    GetWindowTextW(s->combo, nm.szText, 260);
    nm.iWhy = why;
    s->began = 0;
    LRESULT r = cc_notify(h, CBEN_ENDEDITW, &nm.hdr);
    if (!r) s->changed = 0;
}

static void begin_edit(HWND h, CBX *s)
{
    if (s->began) return;
    s->began = 1;
    cc_notify(h, CBEN_BEGINEDIT, NULL);
}

static LRESULT CALLBACK edit_sub(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    HWND cbx = (HWND)ref;
    CBX *s = ctl_get(cbx);
    (void)id;
    if (!s) return DefSubclassProc(h, msg, wp, lp);
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_RETURN) {
            BOOL dropped = (BOOL)SendMessageW(s->combo, CB_GETDROPPEDSTATE, 0, 0);
            if (dropped) SendMessageW(s->combo, CB_SHOWDROPDOWN, FALSE, 0);
            end_edit(cbx, s, CBENF_RETURN);
            return 0;
        }
        if (wp == VK_ESCAPE && !SendMessageW(s->combo, CB_GETDROPPEDSTATE, 0, 0)) {
            end_edit(cbx, s, CBENF_ESCAPE);
            return 0;
        }
        break;
    case WM_CHAR:
        if (wp == '\r' || wp == 27) return 0;
        break;
    case WM_SETFOCUS: begin_edit(cbx, s); break;
    case WM_KILLFOCUS: {
        LRESULT r = DefSubclassProc(h, msg, wp, lp);
        if (s->began && IsWindow(cbx)) end_edit(cbx, s, CBENF_KILLFOCUS);
        return r;
    }
    }
    return DefSubclassProc(h, msg, wp, lp);
}

LRESULT CALLBACK ComboExProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    CBX *s = ctl_get(h);
    if (!s && msg != WM_NCCREATE) return DefWindowProcW(h, msg, wp, lp);
    int wide = 1;
    switch (msg) {
    case WM_NCCREATE:
        s = calloc(1, sizeof(CBX));
        if (!s) return FALSE;
        ctl_set(h, s);
        return DefWindowProcW(h, msg, wp, lp);
    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        DWORD st = (DWORD)cs->style;
        DWORD cst = WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | CBS_AUTOHSCROLL | (st & (CBS_DROPDOWN | CBS_DROPDOWNLIST | CBS_SIMPLE | CBS_SORT | CBS_NOINTEGRALHEIGHT));
        if (!(cst & 3)) cst |= CBS_DROPDOWN;
        s->combo = CreateWindowExW(0, L"ComboBox", NULL, cst, 0, 0, cs->cx, cs->cy, h, (HMENU)(INT_PTR)GetDlgCtrlID(h), NULL, NULL);
        if (!s->combo) return -1;
        SendMessageW(s->combo, WM_SETFONT, (WPARAM)cc_font_for(h), 0);
        s->edit = GetWindow(s->combo, GW_CHILD);
        if (s->edit) SetWindowSubclass(s->edit, edit_sub, 1, (DWORD_PTR)h);
        /* like a combo box, the window is the selection field; the height asked for is the list's */
        RECT r;
        GetWindowRect(s->combo, &r);
        SetWindowPos(h, NULL, 0, 0, cs->cx, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_WINDOWPOSCHANGING: {
        WINDOWPOS *wp_ = (WINDOWPOS *)lp;
        if (s->combo && wp_ && !(wp_->flags & SWP_NOSIZE)) {
            RECT r;
            GetWindowRect(s->combo, &r);
            wp_->cy = r.bottom - r.top;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }
    case WM_NCDESTROY:
        for (int i = 0; i < s->n; i++) wfree(s->it[i].text);
        free(s->it); free(s);
        ctl_set(h, NULL);
        return DefWindowProcW(h, msg, wp, lp);
    case WM_SIZE: {
        RECT c;
        GetClientRect(h, &c);
        RECT cr;
        GetWindowRect(s->combo, &cr);
        SetWindowPos(s->combo, NULL, 0, 0, c.right, cr.bottom - cr.top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); return 0; }
    case WM_SETFOCUS: SetFocus(s->edit ? s->edit : s->combo); return 0;
    case WM_ENABLE: EnableWindow(s->combo, (BOOL)wp); return 0;
    case WM_SETFONT: s->font = (HFONT)wp; SendMessageW(s->combo, WM_SETFONT, wp, lp); return 0;
    case WM_DPICHANGED_AFTERPARENT: {                        /* the window's DPI changed: the field's height */
        if (!s->font) SendMessageW(s->combo, WM_SETFONT, (WPARAM)cc_font_for(h), 0);
        RECT c, r;
        GetClientRect(h, &c);
        GetWindowRect(s->combo, &r);
        SetWindowPos(h, NULL, 0, 0, c.right, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_GETFONT: return (LRESULT)s->font;
    case WM_SETTEXT: case WM_GETTEXT: case WM_GETTEXTLENGTH:
        return SendMessageW(s->combo, msg, wp, lp);
    case WM_COMMAND:
        if ((HWND)lp == s->combo) {
            UINT code = HIWORD(wp);
            if (code == CBN_EDITCHANGE) { s->changed = 1; begin_edit(h, s); }
            if (code == CBN_SELCHANGE) s->changed = 0;
            HWND p = GetParent(h);
            return p ? SendMessageW(p, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(h), code), (LPARAM)h) : 0;
        }
        return 0;
    case WM_NOTIFY: { HWND p = GetParent(h); return p ? SendMessageW(p, msg, wp, lp) : 0; }
    case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORSTATIC: { HWND p = GetParent(h); return p ? SendMessageW(p, msg, wp, lp) : 0; }

    case CBEM_INSERTITEMA_: wide = 0; /* fall through */
    case CBEM_INSERTITEMW: return insert_item(h, s, (const COMBOBOXEXITEMW *)lp, wide);
    case CBEM_SETITEMA_: wide = 0; /* fall through */
    case CBEM_SETITEMW: return set_item(h, s, (const COMBOBOXEXITEMW *)lp, wide);
    case CBEM_GETITEMA_: wide = 0; /* fall through */
    case CBEM_GETITEMW: return get_item(h, s, (COMBOBOXEXITEMW *)lp, wide);
    case CBEM_DELETEITEM: {
        int i = (int)wp;
        if (i < 0 || i >= s->n) return CB_ERR;
        NMCOMBOBOXEXW nm;
        memset(&nm, 0, sizeof(nm));
        nm.ceItem.iItem = i; nm.ceItem.lParam = s->it[i].lp; nm.ceItem.mask = CBEIF_LPARAM;
        cc_notify(h, CBEN_DELETEITEM, &nm.hdr);
        wfree(s->it[i].text);
        memmove(&s->it[i], &s->it[i + 1], sizeof(CItem) * (size_t)(s->n - i - 1));
        s->n--;
        return SendMessageW(s->combo, CB_DELETESTRING, wp, 0);
    }
    case CB_RESETCONTENT:
        for (int i = s->n - 1; i >= 0; i--) {
            NMCOMBOBOXEXW nm;
            memset(&nm, 0, sizeof(nm));
            nm.ceItem.iItem = i; nm.ceItem.lParam = s->it[i].lp; nm.ceItem.mask = CBEIF_LPARAM;
            cc_notify(h, CBEN_DELETEITEM, &nm.hdr);
            wfree(s->it[i].text);
        }
        s->n = 0;
        return SendMessageW(s->combo, CB_RESETCONTENT, 0, 0);
    case CBEM_SETIMAGELIST: { HIMAGELIST o = s->il; s->il = (HIMAGELIST)lp; return (LRESULT)o; }
    case CBEM_GETIMAGELIST: return (LRESULT)s->il;
    case CBEM_GETCOMBOCONTROL: return (LRESULT)s->combo;
    case CBEM_GETEDITCONTROL: return (LRESULT)s->edit;
    case CBEM_SETEXSTYLE: case CBEM_SETEXTENDEDSTYLE: { DWORD o = s->ex, m = (msg == CBEM_SETEXTENDEDSTYLE && wp) ? (DWORD)wp : 0xFFFFFFFFu; s->ex = (s->ex & ~m) | ((DWORD)lp & m); return (LRESULT)o; }
    case CBEM_GETEXSTYLE: return (LRESULT)s->ex;
    case CBEM_HASEDITCHANGED: return s->changed;
    case CBEM_SETUNICODEFORMAT_: case CBEM_GETUNICODEFORMAT_: return TRUE;
    case CB_ADDSTRING: case CB_INSERTSTRING: return CB_ERR;      /* items go in with CBEM_INSERTITEM */
    }
    if (msg >= CB_GETEDITSEL && msg <= CB_GETCOMBOBOXINFO + 16) return SendMessageW(s->combo, msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}
