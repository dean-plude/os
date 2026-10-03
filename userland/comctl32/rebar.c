/*
 * rebar.c — the rebar (ReBarWindow32): bands holding child windows
 * (toolbars, combo boxes), in rows. A band has an optional gripper and
 * label, its child keeps its minimum size and the last band of a row
 * takes the rest of the width. Commands and notifications from the
 * children go on to the rebar's parent.
 */
#include "cc.h"

#define RB_INSERTBANDA_  (WM_USER + 1)
#define RB_SETBANDINFOA_ (WM_USER + 6)
#define RB_GETBANDINFOA_ (WM_USER + 29)
#define RB_SETBKCOLOR_   (WM_USER + 19)
#define RB_GETBKCOLOR_   (WM_USER + 20)
#define RB_SETTEXTCOLOR_ (WM_USER + 21)
#define RB_SIZETORECT_   (WM_USER + 23)
#define RB_GETBANDBORDERS_ (WM_USER + 34)
#define RB_GETBANDMARGINS_ (WM_USER + 40)
#define RB_SETPARENT_    (WM_USER + 7)
#define RB_SETUNICODEFORMAT_ 0x2005
#define RB_GETUNICODEFORMAT_ 0x2006
#define GRIPPER 10

typedef struct {
    UINT style, id;
    WCHAR *text;
    HWND child;
    UINT cx_min, cy_min, cx, cy_child, cy_max, cy_integral, cx_ideal, cx_header;
    COLORREF fore, back;
    int image;
    LPARAM lp;
    RECT r;                         /* laid out: the band */
    RECT cr;                        /* where its child goes */
} Band;

typedef struct {
    Band *b;
    int n, cap;
    HFONT font;
    HIMAGELIST il;
    COLORREF bk, text;
    int height;
    HWND parent;
    int in_layout;
} RB;

static DWORD style_of(HWND h) { return (DWORD)GetWindowLongW(h, GWL_STYLE); }

static int header_w(HWND h, RB *s, Band *b)
{
    if (b->style & 0x0800 /* RBBS_USEHEADERSIZE? keep */) {}
    int w = 0;
    if (!(b->style & RBBS_NOGRIPPER) && (!(style_of(h) & RBS_FIXEDORDER) || (b->style & RBBS_GRIPPERALWAYS))) w += GRIPPER;
    if (b->text && b->text[0]) {
        HDC dc = GetDC(h);
        HGDIOBJ of = SelectObject(dc, s->font ? s->font : cc_font_for(h));
        w += cc_text_w(dc, b->text, -1) + 8;
        SelectObject(dc, of);
        ReleaseDC(h, dc);
    }
    if (s->il && b->image >= 0) { int ix, iy; ImageList_GetIconSize(s->il, &ix, &iy); w += ix + 4; }
    if (b->cx_header) w = (int)b->cx_header;
    return w + 2;
}

static void layout(HWND h, RB *s)
{
    if (s->in_layout) return;
    s->in_layout = 1;
    RECT c;
    GetClientRect(h, &c);
    int width = c.right;
    int y = 0, i = 0;
    int border = (style_of(h) & RBS_BANDBORDERS) ? 1 : 0;
    while (i < s->n) {
        /* one row: from band i up to the next break */
        int first = i, last = i, rowh = 0;
        for (int k = i; k < s->n; k++) {
            if (s->b[k].style & RBBS_HIDDEN) { last = k; continue; }
            if (k > first && (s->b[k].style & RBBS_BREAK)) break;
            last = k;
            int ch = (int)s->b[k].cy_min;
            if (style_of(h) & RBS_VARHEIGHT) ch = MAX(ch, (int)s->b[k].cy_child);
            rowh = MAX(rowh, ch + 4);
        }
        if (rowh == 0) rowh = 4;
        int x = 0;
        int lastvis = -1;
        for (int k = first; k <= last; k++) if (!(s->b[k].style & RBBS_HIDDEN)) lastvis = k;
        for (int k = first; k <= last; k++) {
            Band *b = &s->b[k];
            if (b->style & RBBS_HIDDEN) { SetRectEmpty(&b->r); SetRectEmpty(&b->cr); continue; }
            int hw = header_w(h, s, b);
            int want = MAX((int)b->cx, hw + (int)b->cx_min + 4);
            if (b->cx_ideal) want = MAX(want, hw + (int)b->cx_ideal + 4);
            int w = k == lastvis ? MAX(width - x, hw + (int)b->cx_min) : want;
            SetRect(&b->r, x, y, x + w, y + rowh);
            int cy = (style_of(h) & RBS_VARHEIGHT) ? MAX((int)b->cy_min, MIN((int)b->cy_child ? (int)b->cy_child : (int)b->cy_min, rowh - 4)) : (int)b->cy_min;
            if (!cy) cy = rowh - 4;
            int cleft = x + hw, cright = x + w - 2 - border;
            if (b->style & RBBS_FIXEDSIZE) cright = MIN(cright, cleft + (int)b->cx_min);
            SetRect(&b->cr, cleft, y + (rowh - cy) / 2, MAX(cleft, cright), y + (rowh - cy) / 2 + cy);
            x += w;
        }
        y += rowh + border;
        i = last + 1;
    }
    int old = s->height;
    s->height = y;
    for (int k = 0; k < s->n; k++) {
        Band *b = &s->b[k];
        if (!b->child || !IsWindow(b->child)) continue;
        if (b->style & RBBS_HIDDEN) { ShowWindow(b->child, SW_HIDE); continue; }
        RECT r = b->cr;
        /* a combo box's height is its drop-down's: keep what the program gave */
        WCHAR cls[32];
        int ch = r.bottom - r.top;
        if (GetClassNameW(b->child, cls, 32) && (!lstrcmpiW(cls, L"ComboBox") || !lstrcmpiW(cls, WC_COMBOBOXEXW))) {
            RECT wr; GetWindowRect(b->child, &wr);
            ch = MAX(wr.bottom - wr.top, ch);
        }
        SetWindowPos(b->child, NULL, r.left, r.top, r.right - r.left, ch, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    /* our own height follows the bands, unless the program sizes us */
    DWORD st = style_of(h);
    if (!(st & CCS_NORESIZE)) {
        RECT wr;
        GetWindowRect(h, &wr);
        RECT cl;
        GetClientRect(h, &cl);
        int frame = (wr.bottom - wr.top) - cl.bottom;
        if (cl.bottom != s->height) {
            HWND p = GetParent(h);
            if (!(st & CCS_NOPARENTALIGN) && p) {
                RECT pc; GetClientRect(p, &pc);
                SetWindowPos(h, NULL, 0, 0, pc.right, s->height + frame, SWP_NOZORDER | SWP_NOACTIVATE);
            } else {
                SetWindowPos(h, NULL, 0, 0, wr.right - wr.left, s->height + frame, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
            }
        }
    }
    s->in_layout = 0;
    InvalidateRect(h, NULL, TRUE);
    if (old != s->height) cc_notify(h, RBN_HEIGHTCHANGE, NULL);
}

static void set_band(HWND h, RB *s, Band *b, const REBARBANDINFOW *in, int wide)
{
    (void)h;
    UINT m = in->fMask;
    if (m & RBBIM_STYLE) b->style = in->fStyle;
    if (m & RBBIM_COLORS) { b->fore = in->clrFore; b->back = in->clrBack; }
    if (m & RBBIM_TEXT) {
        wfree(b->text);
        b->text = NULL;
        if (in->lpText) {
            if (wide) b->text = wdup(in->lpText);
            else {
                int n = MultiByteToWideChar(CP_ACP, 0, (const char *)in->lpText, -1, NULL, 0);
                b->text = malloc(2 * (size_t)(n > 0 ? n : 1));
                if (b->text) { if (n > 0) MultiByteToWideChar(CP_ACP, 0, (const char *)in->lpText, -1, b->text, n); else b->text[0] = 0; }
            }
        }
    }
    if (m & RBBIM_IMAGE) b->image = in->iImage;
    if (m & RBBIM_CHILD) {
        if (b->child && b->child != in->hwndChild && IsWindow(b->child)) ShowWindow(b->child, SW_HIDE);
        b->child = in->hwndChild;
        if (b->child) SetParent(b->child, h);
    }
    if (m & RBBIM_CHILDSIZE) {
        b->cx_min = in->cxMinChild; b->cy_min = in->cyMinChild;
        if (in->cbSize >= offsetof(REBARBANDINFOW, cxIdeal)) { b->cy_child = in->cyChild; b->cy_max = in->cyMaxChild; b->cy_integral = in->cyIntegral; }
    }
    if (m & RBBIM_SIZE) b->cx = in->cx;
    if (m & RBBIM_ID) b->id = in->wID;
    if ((m & RBBIM_IDEALSIZE) && in->cbSize > offsetof(REBARBANDINFOW, cxIdeal)) b->cx_ideal = in->cxIdeal;
    if ((m & RBBIM_LPARAM) && in->cbSize > offsetof(REBARBANDINFOW, lParam)) b->lp = in->lParam;
    if ((m & RBBIM_HEADERSIZE) && in->cbSize > offsetof(REBARBANDINFOW, cxHeader)) b->cx_header = in->cxHeader;
    (void)s;
}

static void get_band(RB *s, Band *b, REBARBANDINFOW *out, int wide)
{
    (void)s;
    UINT m = out->fMask;
    if (m & RBBIM_STYLE) out->fStyle = b->style;
    if (m & RBBIM_COLORS) { out->clrFore = b->fore; out->clrBack = b->back; }
    if ((m & RBBIM_TEXT) && out->lpText && out->cch) {
        const WCHAR *t = b->text ? b->text : L"";
        if (wide) { int n = MIN(wlen(t), (int)out->cch - 1); memcpy(out->lpText, t, 2 * (size_t)n); out->lpText[n] = 0; }
        else WideCharToMultiByte(CP_ACP, 0, t, -1, (char *)out->lpText, (int)out->cch, NULL, NULL);
    }
    if (m & RBBIM_IMAGE) out->iImage = b->image;
    if (m & RBBIM_CHILD) out->hwndChild = b->child;
    if (m & RBBIM_CHILDSIZE) {
        out->cxMinChild = b->cx_min; out->cyMinChild = b->cy_min;
        if (out->cbSize >= offsetof(REBARBANDINFOW, cxIdeal)) { out->cyChild = b->cy_child; out->cyMaxChild = b->cy_max; out->cyIntegral = b->cy_integral; }
    }
    if (m & RBBIM_SIZE) out->cx = (UINT)(b->r.right - b->r.left);
    if (m & RBBIM_ID) out->wID = b->id;
    if ((m & RBBIM_IDEALSIZE) && out->cbSize > offsetof(REBARBANDINFOW, cxIdeal)) out->cxIdeal = b->cx_ideal;
    if ((m & RBBIM_LPARAM) && out->cbSize > offsetof(REBARBANDINFOW, lParam)) out->lParam = b->lp;
    if ((m & RBBIM_HEADERSIZE) && out->cbSize > offsetof(REBARBANDINFOW, cxHeader)) out->cxHeader = b->cx_header;
}

static void paint(HWND h, RB *s, HDC dc)
{
    RECT c;
    GetClientRect(h, &c);
    COLORREF bk = s->bk == CLR_DEFAULT ? GetSysColor(COLOR_BTNFACE) : s->bk;
    cc_fill(dc, &c, bk);
    HGDIOBJ of = SelectObject(dc, s->font ? s->font : cc_font_for(h));
    SetBkMode(dc, TRANSPARENT);
    int borders = (style_of(h) & RBS_BANDBORDERS) != 0;
    for (int k = 0; k < s->n; k++) {
        Band *b = &s->b[k];
        if (IsRectEmpty(&b->r)) continue;
        if (b->back != CLR_DEFAULT && (b->back != 0 || b->fore != 0)) {}
        int x = b->r.left + 2;
        if (!(b->style & RBBS_NOGRIPPER) && (!(style_of(h) & RBS_FIXEDORDER) || (b->style & RBBS_GRIPPERALWAYS))) {
            for (int y = b->r.top + 4; y + 2 <= b->r.bottom - 3; y += 4) { RECT d = { x + 2, y, x + 4, y + 2 }; cc_fill(dc, &d, RGB(170, 170, 170)); }
            x += GRIPPER;
        }
        if (s->il && b->image >= 0) {
            int ix, iy;
            ImageList_GetIconSize(s->il, &ix, &iy);
            il_draw(s->il, b->image, dc, x, (b->r.top + b->r.bottom - iy) / 2, ILD_TRANSPARENT);
            x += ix + 4;
        }
        if (b->text && b->text[0]) {
            SetTextColor(dc, s->text == CLR_DEFAULT ? GetSysColor(COLOR_BTNTEXT) : s->text);
            RECT t = { x, b->r.top, b->cr.left, b->r.bottom };
            DrawTextW(dc, b->text, -1, &t, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        }
        if (borders) {
            RECT v = { b->r.right - 1, b->r.top + 2, b->r.right, b->r.bottom - 2 };
            cc_fill(dc, &v, RGB(215, 215, 215));
            RECT u = { b->r.left, b->r.bottom, b->r.right, b->r.bottom + 1 };
            cc_fill(dc, &u, RGB(215, 215, 215));
        }
    }
    SelectObject(dc, of);
}

static int insert_band(HWND h, RB *s, int at, const REBARBANDINFOW *in, int wide)
{
    if (!in) return FALSE;
    if (s->n == s->cap) {
        int c = s->cap ? s->cap * 2 : 4;
        Band *p = realloc(s->b, sizeof(Band) * (size_t)c);
        if (!p) return FALSE;
        s->b = p; s->cap = c;
    }
    if (at < 0 || at > s->n) at = s->n;
    memmove(&s->b[at + 1], &s->b[at], sizeof(Band) * (size_t)(s->n - at));
    Band *b = &s->b[at];
    memset(b, 0, sizeof(*b));
    b->image = -1;
    b->fore = b->back = CLR_DEFAULT;
    s->n++;
    set_band(h, s, b, in, wide);
    layout(h, s);
    return TRUE;
}

LRESULT CALLBACK RebarProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    RB *s = ctl_get(h);
    if (!s && msg != WM_NCCREATE) return DefWindowProcW(h, msg, wp, lp);
    int wide = 1;
    switch (msg) {
    case WM_NCCREATE:
        s = calloc(1, sizeof(RB));
        if (!s) return FALSE;
        s->bk = s->text = CLR_DEFAULT;
        ctl_set(h, s);
        return DefWindowProcW(h, msg, wp, lp);
    case WM_NCDESTROY:
        for (int k = 0; k < s->n; k++) wfree(s->b[k].text);
        free(s->b); free(s);
        ctl_set(h, NULL);
        return DefWindowProcW(h, msg, wp, lp);
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); paint(h, s, dc); EndPaint(h, &ps); return 0; }
    case WM_PRINTCLIENT: paint(h, s, (HDC)wp); return 0;
    case WM_SIZE: if (!s->in_layout) layout(h, s); return 0;
    case WM_SETFONT: s->font = (HFONT)wp; layout(h, s); return 0;
    case WM_GETFONT: return (LRESULT)s->font;
    case WM_NOTIFY: case WM_COMMAND: case WM_DRAWITEM: case WM_MEASUREITEM: case WM_CTLCOLOREDIT: case WM_CTLCOLORSTATIC: case WM_CTLCOLORLISTBOX: {
        HWND p = s->parent ? s->parent : GetParent(h);
        return p ? SendMessageW(p, msg, wp, lp) : 0;
    }
    case RB_INSERTBANDA_: wide = 0; /* fall through */
    case RB_INSERTBANDW: return insert_band(h, s, (int)wp, (const REBARBANDINFOW *)lp, wide);
    case RB_DELETEBAND: {
        int i = (int)wp;
        if (i < 0 || i >= s->n) return FALSE;
        if (s->b[i].child && IsWindow(s->b[i].child)) ShowWindow(s->b[i].child, SW_HIDE);
        wfree(s->b[i].text);
        memmove(&s->b[i], &s->b[i + 1], sizeof(Band) * (size_t)(s->n - i - 1));
        s->n--;
        layout(h, s);
        return TRUE;
    }
    case RB_SETBANDINFOA_: wide = 0; /* fall through */
    case RB_SETBANDINFOW: {
        int i = (int)wp;
        if (i < 0 || i >= s->n || !lp) return FALSE;
        set_band(h, s, &s->b[i], (const REBARBANDINFOW *)lp, wide);
        layout(h, s);
        return TRUE;
    }
    case RB_GETBANDINFOA_: wide = 0; /* fall through */
    case RB_GETBANDINFOW: {
        int i = (int)wp;
        if (i < 0 || i >= s->n || !lp) return FALSE;
        get_band(s, &s->b[i], (REBARBANDINFOW *)lp, wide);
        return TRUE;
    }
    case RB_GETBANDCOUNT: return s->n;
    case RB_GETROWCOUNT: { int rows = 0, y = -1; for (int k = 0; k < s->n; k++) if (!IsRectEmpty(&s->b[k].r) && s->b[k].r.top != y) { rows++; y = s->b[k].r.top; } return rows; }
    case RB_GETROWHEIGHT: { int i = (int)wp; return i >= 0 && i < s->n ? s->b[i].r.bottom - s->b[i].r.top : 0; }
    case RB_GETBARHEIGHT: return s->height;
    case RB_IDTOINDEX: for (int k = 0; k < s->n; k++) if (s->b[k].id == (UINT)wp) return k; return -1;
    case RB_SHOWBAND: {
        int i = (int)wp;
        if (i < 0 || i >= s->n) return FALSE;
        if (lp) s->b[i].style &= ~RBBS_HIDDEN; else s->b[i].style |= RBBS_HIDDEN;
        layout(h, s);
        return TRUE;
    }
    case RB_MAXIMIZEBAND: case RB_MINIMIZEBAND: layout(h, s); return 0;
    case RB_GETRECT: { int i = (int)wp; if (i < 0 || i >= s->n || !lp) return FALSE; *(RECT *)lp = s->b[i].r; return TRUE; }
    case RB_GETBANDBORDERS_: { int i = (int)wp; if (i < 0 || i >= s->n || !lp) return 0; RECT *r = (RECT *)lp; r->left = s->b[i].cr.left - s->b[i].r.left; r->top = 2; r->right = 2; r->bottom = 2; return 0; }
    case RB_GETBANDMARGINS_: if (lp) memset((void *)lp, 0, 16); return 0;
    case RB_SETBARINFO: { const REBARINFO *ri = (const REBARINFO *)lp; if (ri && (ri->fMask & 1)) s->il = ri->himl; layout(h, s); return TRUE; }
    case RB_GETBARINFO: { REBARINFO *ri = (REBARINFO *)lp; if (ri && (ri->fMask & 1)) ri->himl = s->il; return TRUE; }
    case RB_SETBKCOLOR_: { COLORREF o = s->bk; s->bk = (COLORREF)lp; InvalidateRect(h, NULL, TRUE); return o; }
    case RB_GETBKCOLOR_: return s->bk;
    case RB_SETTEXTCOLOR_: { COLORREF o = s->text; s->text = (COLORREF)lp; InvalidateRect(h, NULL, TRUE); return o; }
    case RB_SIZETORECT_: layout(h, s); return TRUE;
    case RB_SETPARENT_: { HWND o = s->parent ? s->parent : GetParent(h); s->parent = (HWND)wp; return (LRESULT)o; }
    case RB_SETUNICODEFORMAT_: case RB_GETUNICODEFORMAT_: return TRUE;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
