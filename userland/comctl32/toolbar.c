/*
 * toolbar.c — the toolbar (ToolbarWindow32): a row of buttons with images
 * from an image list (or bitmaps added with TB_ADDBITMAP) and optional
 * text, below the image or beside it (TBSTYLE_LIST). Buttons can be
 * check buttons, radio groups and drop-downs; a click sends WM_COMMAND to
 * the parent. Unless CCS_NOPARENTALIGN / CCS_NORESIZE, the bar sizes
 * itself along the top (or bottom) of its parent.
 */
#include "cc.h"

#define TB_ADDBUTTONSA_   (WM_USER + 20)
#define TB_INSERTBUTTONA_ (WM_USER + 21)
#define TB_ADDSTRINGA_    (WM_USER + 28)
#define TB_GETBUTTONINFOA_ (WM_USER + 65)
#define TB_SETBUTTONINFOA_ (WM_USER + 66)
#define TB_GETSTRINGW_    (WM_USER + 91)
#define TB_SETLISTGAP_    (WM_USER + 96)
#define TB_SETUNICODEFORMAT_ 0x2005
#define TB_GETUNICODEFORMAT_ 0x2006
#define TBN_GETBUTTONINFOW_ (TBN_FIRST - 20)
#define DDARROW 14                          /* the drop-down arrow part */

typedef struct {
    int image, cmd;
    BYTE state, style;
    DWORD_PTR data;
    WCHAR *text;                            /* own copy, or NULL */
    int cx;                                 /* explicit width (TBIF_SIZE), 0 = computed */
    RECT r;                                 /* laid out */
} TBtn;

typedef struct {
    TBtn *b;
    int n, cap;
    WCHAR **str;                            /* the string pool (TB_ADDSTRING) */
    int nstr;
    HIMAGELIST il, hot_il, dis_il;
    HIMAGELIST own_il;                      /* made from TB_ADDBITMAP bitmaps */
    int bmp_w, bmp_h;                       /* bitmap size */
    int btn_w, btn_h;                       /* explicit button size (TB_SETBUTTONSIZE), 0 = computed */
    HFONT font;
    DWORD ex;
    int hot, pressed, pressed_dd;           /* indices, -1 none */
    int indent;
    int max_rows;
    HWND parent;                            /* where commands go */
    HWND tips;
    int pad_x, pad_y;
    int spacing_x, spacing_y;               /* between buttons (TB_SETMETRICS) */
} TB;

static DWORD style_of(HWND h) { return (DWORD)GetWindowLongW(h, GWL_STYLE); }
static HWND notify_target(HWND h, TB *s) { return s->parent ? s->parent : GetParent(h); }

static LRESULT tb_notify(HWND h, TB *s, UINT code, NMHDR *nm)
{
    NMHDR tmp;
    if (!nm) nm = &tmp;
    nm->hwndFrom = h;
    nm->idFrom = (UINT_PTR)GetDlgCtrlID(h);
    nm->code = code;
    HWND p = notify_target(h, s);
    return p ? SendMessageW(p, WM_NOTIFY, nm->idFrom, (LPARAM)nm) : 0;
}

static const WCHAR *btn_text(TB *s, TBtn *b)
{
    if (b->text) return b->text;
    return NULL;
}

static int has_text(TB *s)
{
    if (s->max_rows == 0) return 0;
    for (int i = 0; i < s->n; i++) if (btn_text(s, &s->b[i]) && btn_text(s, &s->b[i])[0]) return 1;
    return 0;
}

static void image_size(TB *s, int *w, int *h)
{
    *w = s->bmp_w; *h = s->bmp_h;
    if (s->il) ImageList_GetIconSize(s->il, w, h);
}

static int text_w(HWND h, TB *s, const WCHAR *t)
{
    if (!t || !t[0]) return 0;
    HDC dc = GetDC(h);
    HGDIOBJ of = SelectObject(dc, s->font ? s->font : cc_font());
    RECT r = { 0, 0, 0, 0 };
    DrawTextW(dc, t, -1, &r, DT_CALCRECT | DT_SINGLELINE | ((style_of(h) & TBSTYLE_NOPREFIX) ? DT_NOPREFIX : 0));
    SelectObject(dc, of);
    ReleaseDC(h, dc);
    return r.right - r.left;
}

/* the size every button shares (the widest text decides, unless BTNS_AUTOSIZE / list style) */
static void base_size(HWND h, TB *s, int *bw, int *bh)
{
    int iw, ih;
    image_size(s, &iw, &ih);
    int fh = cc_font_h(s->font);
    int text = has_text(s);
    DWORD st = style_of(h);
    if (st & TBSTYLE_LIST) {
        *bh = MAX(ih, text ? fh : 0) + 2 * s->pad_y;
        *bw = iw + 2 * s->pad_x;
    } else {
        int tw = 0;
        for (int i = 0; text && i < s->n; i++) if (!(s->b[i].style & BTNS_SEP)) tw = MAX(tw, text_w(h, s, btn_text(s, &s->b[i])));
        *bw = MAX(iw, tw) + 2 * s->pad_x;
        *bh = ih + (text ? fh + 2 : 0) + 2 * s->pad_y;
    }
    if (s->btn_w) *bw = MAX(*bw, s->btn_w);
    if (s->btn_h) *bh = MAX(*bh, s->btn_h);
}

static int btn_width(HWND h, TB *s, TBtn *b, int bw)
{
    if (b->style & BTNS_SEP) return b->cx ? b->cx : b->image > 0 ? b->image : 8;   /* TBIF_SIZE sizes a separator too */
    if (b->cx) return b->cx;
    int w = bw;
    DWORD st = style_of(h);
    const WCHAR *t = btn_text(s, b);
    if (st & TBSTYLE_LIST) {
        int show = t && t[0] && (!(s->ex & TBSTYLE_EX_MIXEDBUTTONS) || (b->style & BTNS_SHOWTEXT));
        if (show) w += text_w(h, s, t) + 6;
    } else if (b->style & BTNS_AUTOSIZE) {
        int iw, ih;
        image_size(s, &iw, &ih);
        w = MAX(iw, text_w(h, s, t)) + 2 * s->pad_x;
    }
    if ((b->style & BTNS_DROPDOWN) && (s->ex & TBSTYLE_EX_DRAWDDARROWS) && !(b->style & BTNS_WHOLEDROPDOWN)) w += DDARROW;
    else if (b->style & BTNS_WHOLEDROPDOWN) w += DDARROW - 4;
    return w;
}

static int g_layout_width = -1;         /* autosize: lay out for this width instead of the current one */

static void layout_ex(HWND h, TB *s, int allow_wrap)
{
    int bw, bh;
    base_size(h, s, &bw, &bh);
    RECT c;
    GetClientRect(h, &c);
    if (g_layout_width >= 0) c.right = g_layout_width;
    int wrap = allow_wrap && (style_of(h) & TBSTYLE_WRAPABLE) && c.right > 0;
    int x = s->indent, y = 0;
    for (int i = 0; i < s->n; i++) {
        TBtn *b = &s->b[i];
        if (b->state & TBSTATE_HIDDEN) { SetRectEmpty(&b->r); continue; }
        int w = btn_width(h, s, b, bw);
        if (wrap && x > s->indent && x + w > c.right && !(b->style & BTNS_SEP)) { x = s->indent; y += bh + 2; }
        SetRect(&b->r, x, y, x + w, y + bh);
        x += w + s->spacing_x;
        if (b->state & TBSTATE_WRAP) { x = s->indent; y += bh + ((b->style & BTNS_SEP) ? 8 : 2); }
    }
}

static void layout(HWND h, TB *s) { layout_ex(h, s, 1); }

/* the size of the buttons in one row (TB_GETMAXSIZE), or as laid out now */
static void total_size_ex(HWND h, TB *s, SIZE *sz, int one_row)
{
    layout_ex(h, s, !one_row);
    int bw, bh;
    base_size(h, s, &bw, &bh);
    sz->cx = s->indent; sz->cy = bh;
    for (int i = 0; i < s->n; i++) {
        if (IsRectEmpty(&s->b[i].r)) continue;
        sz->cx = MAX(sz->cx, s->b[i].r.right);
        sz->cy = MAX(sz->cy, s->b[i].r.bottom);
    }
    if (one_row) layout(h, s);
}

static void total_size(HWND h, TB *s, SIZE *sz) { total_size_ex(h, s, sz, 0); }

static void autosize(HWND h, TB *s)
{
    DWORD st = style_of(h);
    layout(h, s);
    if (st & CCS_NORESIZE) { InvalidateRect(h, NULL, TRUE); return; }
    HWND p = GetParent(h);
    RECT pc = { 0, 0, 0, 0 }, wr;
    if (p) GetClientRect(p, &pc);
    GetWindowRect(h, &wr);
    SIZE sz;
    g_layout_width = !(st & CCS_NOPARENTALIGN) && p ? pc.right : wr.right - wr.left;
    total_size(h, s, &sz);
    g_layout_width = -1;
    int height = sz.cy + ((st & CCS_NODIVIDER) ? 0 : 2) + 2;
    if (!(st & CCS_NOPARENTALIGN) && p) {
        int y = (st & CCS_BOTTOM) == CCS_BOTTOM ? pc.bottom - height : 0;
        UINT fl = SWP_NOZORDER | SWP_NOACTIVATE;
        if (st & CCS_NOMOVEY) fl |= SWP_NOMOVE;
        SetWindowPos(h, NULL, 0, y, pc.right, height, fl);
    } else {
        SetWindowPos(h, NULL, 0, 0, wr.right - wr.left, height, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
    }
    layout(h, s);
    InvalidateRect(h, NULL, TRUE);
}

static int index_of(TB *s, int cmd)
{
    for (int i = 0; i < s->n; i++) if (s->b[i].cmd == cmd) return i;
    return -1;
}

static int hit(TB *s, POINT pt)
{
    for (int i = 0; i < s->n; i++) if (PtInRect(&s->b[i].r, pt)) return i;
    return -1;
}

static void redraw_btn(HWND h, TB *s, int i)
{
    if (i >= 0 && i < s->n) InvalidateRect(h, &s->b[i].r, TRUE);
}

/* -----------------------------------------------------------------------
 * Buttons and strings
 * ----------------------------------------------------------------------- */
static WCHAR *dup_a(const char *a)
{
    int n = MultiByteToWideChar(CP_ACP, 0, a, -1, NULL, 0);
    WCHAR *w = malloc(2 * (size_t)(n > 0 ? n : 1));
    if (w) { if (n > 0) MultiByteToWideChar(CP_ACP, 0, a, -1, w, n); else w[0] = 0; }
    return w;
}

static int add_string(TB *s, const WCHAR *t)
{
    WCHAR **p = realloc(s->str, sizeof(WCHAR *) * (size_t)(s->nstr + 1));
    if (!p) return -1;
    s->str = p;
    s->str[s->nstr] = wdup(t);
    return s->nstr++;
}

/* TB_ADDSTRING: a resource string (first character separates) or a double-null list */
static int add_strings(TB *s, HINSTANCE inst, LPARAM lp, int wide)
{
    int first = -1;
    if (inst && lp < 0x10000) {
        WCHAR buf[1024];
        int n = LoadStringW(inst, (UINT)lp, buf, 1024);
        if (n <= 1) return -1;
        WCHAR sep = buf[0];
        WCHAR *p = buf + 1;
        for (int i = 1; i <= n; i++) {
            if (buf[i] == sep || buf[i] == 0) {
                buf[i] = 0;
                int k = add_string(s, p);
                if (first < 0) first = k;
                p = buf + i + 1;
            }
        }
        return first;
    }
    if (!lp) return -1;
    if (wide) {
        for (const WCHAR *p = (const WCHAR *)lp; *p; p += wlen(p) + 1) { int k = add_string(s, p); if (first < 0) first = k; }
    } else {
        for (const char *p = (const char *)lp; *p; p += strlen(p) + 1) {
            WCHAR *w = dup_a(p);
            int k = add_string(s, w ? w : L"");
            free(w);
            if (first < 0) first = k;
        }
    }
    return first;
}

static void set_btn_text(TB *s, TBtn *b, INT_PTR str, int wide)
{
    free(b->text);
    b->text = NULL;
    if (str == -1) return;
    if (str >= 0 && str < 0x10000) { if (str < s->nstr && s->str[str]) b->text = wdup(s->str[str]); return; }
    b->text = wide ? wdup((const WCHAR *)str) : dup_a((const char *)str);
}

static BOOL insert_buttons(HWND h, TB *s, int at, int count, const TBBUTTON *in, int wide)
{
    if (count <= 0 || !in) return FALSE;
    if (s->n + count > s->cap) {
        int c = MAX(s->cap * 2, s->n + count + 8);
        TBtn *p = realloc(s->b, sizeof(TBtn) * (size_t)c);
        if (!p) return FALSE;
        s->b = p; s->cap = c;
    }
    if (at < 0 || at > s->n) at = s->n;
    memmove(&s->b[at + count], &s->b[at], sizeof(TBtn) * (size_t)(s->n - at));
    for (int k = 0; k < count; k++) {
        TBtn *b = &s->b[at + k];
        memset(b, 0, sizeof(*b));
        b->image = in[k].iBitmap; b->cmd = in[k].idCommand;
        b->state = in[k].fsState; b->style = in[k].fsStyle;
        b->data = in[k].dwData;
        if (!(b->style & BTNS_SEP)) set_btn_text(s, b, in[k].iString, wide);
    }
    s->n += count;
    if (s->hot >= at) s->hot = -1;
    layout(h, s);
    InvalidateRect(h, NULL, TRUE);
    return TRUE;
}

static void get_button(TB *s, int i, TBBUTTON *out)
{
    TBtn *b = &s->b[i];
    memset(out, 0, sizeof(*out));
    out->iBitmap = b->image; out->idCommand = b->cmd;
    out->fsState = b->state; out->fsStyle = b->style;
    out->dwData = b->data;
    out->iString = (INT_PTR)b->text;
}

/* TB_ADDBITMAP: a bitmap strip cut into images of the bitmap size */
static int add_bitmap(HWND h, TB *s, int count, const TBADDBITMAP *ab)
{
    (void)h;
    if (!ab) return -1;
    if (ab->hInst == HINST_COMMCTRL) {
        /* the standard images: NovaOS draws none, but keeps the numbering */
        int n = ab->nID == IDB_STD_SMALL_COLOR || ab->nID == IDB_STD_LARGE_COLOR ? 15 : 12;
        if (!s->own_il) { s->own_il = ImageList_Create(s->bmp_w, s->bmp_h, ILC_COLOR32 | ILC_MASK, 16, 16); if (!s->il) s->il = s->own_il; }
        int first = ImageList_GetImageCount(s->own_il);
        ImageList_SetImageCount(s->own_il, (UINT)(first + n));
        return first;
    }
    HBITMAP bm = ab->hInst ? LoadBitmapW(ab->hInst, MAKEINTRESOURCEW(ab->nID)) : (HBITMAP)ab->nID;
    if (!bm) return -1;
    if (!s->own_il) {
        s->own_il = ImageList_Create(s->bmp_w, s->bmp_h, ILC_COLOR32 | ILC_MASK, 16, 16);
        if (!s->il) s->il = s->own_il;
    }
    int first = ImageList_GetImageCount(s->own_il);
    ImageList_AddMasked(s->own_il, bm, RGB(192, 192, 192));
    if (ab->hInst) DeleteObject(bm);
    (void)count;
    return first;
}

/* -----------------------------------------------------------------------
 * Painting
 * ----------------------------------------------------------------------- */
static void paint(HWND h, TB *s, HDC dc)
{
    RECT c;
    GetClientRect(h, &c);
    DWORD st = style_of(h);
    if (!(st & TBSTYLE_TRANSPARENT)) {
        NMCUSTOMDRAW cd;
        memset(&cd, 0, sizeof(cd));
        cd.dwDrawStage = CDDS_PREPAINT; cd.hdc = dc; cd.rc = c;
        cc_fill(dc, &c, GetSysColor(COLOR_BTNFACE));
    } else {
        cc_fill(dc, &c, GetSysColor(COLOR_BTNFACE));
    }
    if (!(st & CCS_NODIVIDER)) { RECT d = { 0, 0, c.right, 1 }; cc_fill(dc, &d, RGB(225, 225, 225)); }
    HGDIOBJ of = SelectObject(dc, s->font ? s->font : cc_font());
    SetBkMode(dc, TRANSPARENT);
    int iw, ih;
    image_size(s, &iw, &ih);
    int flat = (st & TBSTYLE_FLAT) != 0;
    for (int i = 0; i < s->n; i++) {
        TBtn *b = &s->b[i];
        if (IsRectEmpty(&b->r)) continue;
        RECT r = b->r;
        if (b->style & BTNS_SEP) {
            if (!(b->state & TBSTATE_WRAP)) { RECT l = { (r.left + r.right) / 2, r.top + 3, (r.left + r.right) / 2 + 1, r.bottom - 3 }; cc_fill(dc, &l, RGB(210, 210, 210)); }
            continue;
        }
        BOOL enabled = (b->state & TBSTATE_ENABLED) != 0;
        BOOL down = (b->state & (TBSTATE_PRESSED | TBSTATE_CHECKED)) || (s->pressed == i && s->hot == i);
        BOOL hot = s->hot == i && enabled;
        RECT box = r;
        InflateRect(&box, -1, -1);
        if (down) { cc_fill(dc, &box, (b->state & TBSTATE_CHECKED) && !hot ? RGB(204, 228, 247) : RGB(188, 220, 244)); cc_frame(dc, &box, RGB(122, 170, 213)); }
        else if (hot) { cc_fill(dc, &box, RGB(229, 243, 255)); cc_frame(dc, &box, RGB(204, 232, 255)); }
        else if (!flat) cc_frame(dc, &box, RGB(208, 208, 208));
        RECT content = r;
        int dd = (b->style & BTNS_DROPDOWN) && (s->ex & TBSTYLE_EX_DRAWDDARROWS) && !(b->style & BTNS_WHOLEDROPDOWN);
        if (dd || (b->style & BTNS_WHOLEDROPDOWN)) {
            RECT a = { r.right - DDARROW, r.top, r.right, r.bottom };
            if (dd && (hot || down)) { RECT l = { a.left, box.top + 2, a.left + 1, box.bottom - 2 }; cc_fill(dc, &l, RGB(160, 190, 220)); }
            cc_arrow(dc, &a, 1, enabled ? RGB(60, 60, 60) : RGB(160, 160, 160));
            content.right = a.left;
        }
        if (down) OffsetRect(&content, 1, 1);
        const WCHAR *t = btn_text(s, b);
        int show_text = t && t[0] && s->max_rows != 0 && (!(st & TBSTYLE_LIST) || !(s->ex & TBSTYLE_EX_MIXEDBUTTONS) || (b->style & BTNS_SHOWTEXT));
        HIMAGELIST il = !enabled && s->dis_il ? s->dis_il : hot && s->hot_il ? s->hot_il : s->il;
        int ix, iy;
        if (st & TBSTYLE_LIST) {
            ix = content.left + s->pad_x; iy = (content.top + content.bottom - ih) / 2;
        } else {
            ix = (content.left + content.right - iw) / 2;
            iy = content.top + s->pad_y;
            if (!show_text) iy = (content.top + content.bottom - ih) / 2;
        }
        if (il && b->image >= 0 && b->image != -2) il_draw(il, b->image, dc, ix, iy, ILD_TRANSPARENT | (!enabled && !s->dis_il ? ILD_BLEND50 : 0));
        if (show_text) {
            SetTextColor(dc, enabled ? GetSysColor(COLOR_BTNTEXT) : GetSysColor(COLOR_GRAYTEXT));
            RECT tr = content;
            UINT fl = DT_SINGLELINE | DT_END_ELLIPSIS | ((st & TBSTYLE_NOPREFIX) || (b->style & BTNS_NOPREFIX) ? DT_NOPREFIX : 0);
            if (st & TBSTYLE_LIST) { tr.left = (il && b->image >= 0) ? ix + iw + 4 : content.left + s->pad_x; fl |= DT_VCENTER | DT_LEFT; }
            else { tr.top = iy + ih + 1; tr.bottom = content.bottom - 1; fl |= DT_CENTER | DT_TOP; }
            DrawTextW(dc, t, -1, &tr, fl);
        }
    }
    SelectObject(dc, of);
}

/* -----------------------------------------------------------------------
 * Clicks
 * ----------------------------------------------------------------------- */
static void click(HWND h, TB *s, int i)
{
    TBtn *b = &s->b[i];
    if (b->style & BTNS_CHECK) {
        if ((b->style & BTNS_GROUP) == BTNS_GROUP) {
            if (b->state & TBSTATE_CHECKED) goto send;
            for (int k = i - 1; k >= 0 && (s->b[k].style & BTNS_GROUP) && !(s->b[k].style & BTNS_SEP); k--) { s->b[k].state &= ~TBSTATE_CHECKED; redraw_btn(h, s, k); }
            for (int k = i + 1; k < s->n && (s->b[k].style & BTNS_GROUP) && !(s->b[k].style & BTNS_SEP); k++) { s->b[k].state &= ~TBSTATE_CHECKED; redraw_btn(h, s, k); }
            b->state |= TBSTATE_CHECKED;
        } else {
            b->state ^= TBSTATE_CHECKED;
        }
        redraw_btn(h, s, i);
    }
send:;
    HWND p = notify_target(h, s);
    if (p) SendMessageW(p, WM_COMMAND, MAKEWPARAM(b->cmd, BN_CLICKED), (LPARAM)h);
}

static BOOL drop_down(HWND h, TB *s, int i)
{
    NMTOOLBARW nm;
    memset(&nm, 0, sizeof(nm));
    nm.iItem = s->b[i].cmd;
    get_button(s, i, &nm.tbButton);
    nm.rcButton = s->b[i].r;
    LRESULT r = tb_notify(h, s, TBN_DROPDOWN, &nm.hdr);
    return r != 1;          /* TBDDRET_NODEFAULT (1): the program did not handle it */
}

/* -----------------------------------------------------------------------
 * The window procedure
 * ----------------------------------------------------------------------- */
static BOOL set_state(HWND h, TB *s, int cmd, BYTE bit, BOOL on)
{
    int i = index_of(s, cmd);
    if (i < 0) return FALSE;
    BYTE o = s->b[i].state;
    if (on) s->b[i].state |= bit; else s->b[i].state &= (BYTE)~bit;
    if (bit == TBSTATE_HIDDEN && o != s->b[i].state) { layout(h, s); InvalidateRect(h, NULL, TRUE); }
    else if (o != s->b[i].state) redraw_btn(h, s, i);
    return TRUE;
}

static BOOL button_info(HWND h, TB *s, int id, TBBUTTONINFOW *bi, int set, int wide)
{
    if (!bi) return FALSE;
    int i = (bi->dwMask & TBIF_BYINDEX) ? id : index_of(s, id);
    if (i < 0 || i >= s->n) return set ? FALSE : -1;
    TBtn *b = &s->b[i];
    if (set) {
        if (bi->dwMask & TBIF_COMMAND) b->cmd = bi->idCommand;
        if (bi->dwMask & TBIF_IMAGE) b->image = bi->iImage;
        if (bi->dwMask & TBIF_STATE) b->state = bi->fsState;
        if (bi->dwMask & TBIF_STYLE) b->style = bi->fsStyle;
        if (bi->dwMask & TBIF_LPARAM) b->data = bi->lParam;
        if (bi->dwMask & TBIF_SIZE) b->cx = bi->cx;
        if (bi->dwMask & TBIF_TEXT) set_btn_text(s, b, bi->pszText ? (INT_PTR)bi->pszText : -1, wide);
        layout(h, s);
        InvalidateRect(h, NULL, TRUE);
        return TRUE;
    }
    if (bi->dwMask & TBIF_COMMAND) bi->idCommand = b->cmd;
    if (bi->dwMask & TBIF_IMAGE) bi->iImage = b->image;
    if (bi->dwMask & TBIF_STATE) bi->fsState = b->state;
    if (bi->dwMask & TBIF_STYLE) bi->fsStyle = b->style;
    if (bi->dwMask & TBIF_LPARAM) bi->lParam = b->data;
    if (bi->dwMask & TBIF_SIZE) bi->cx = (WORD)(b->r.right - b->r.left);
    if ((bi->dwMask & TBIF_TEXT) && bi->pszText && bi->cchText > 0) {
        const WCHAR *t = b->text ? b->text : L"";
        if (wide) { int n = MIN(wlen(t), bi->cchText - 1); memcpy(bi->pszText, t, 2 * (size_t)n); bi->pszText[n] = 0; }
        else WideCharToMultiByte(CP_ACP, 0, t, -1, (char *)bi->pszText, bi->cchText, NULL, NULL);
    }
    return i;
}

LRESULT CALLBACK ToolbarProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    TB *s = ctl_get(h);
    if (!s && msg != WM_NCCREATE) return DefWindowProcW(h, msg, wp, lp);
    int wide = 1;
    switch (msg) {
    case WM_NCCREATE:
        s = calloc(1, sizeof(TB));
        if (!s) return FALSE;
        s->bmp_w = 16; s->bmp_h = 15;
        s->hot = s->pressed = -1;
        s->max_rows = 1;
        s->pad_x = 7; s->pad_y = 3;
        ctl_set(h, s);
        return DefWindowProcW(h, msg, wp, lp);
    case WM_CREATE: autosize(h, s); return 0;
    case WM_NCDESTROY:
        for (int i = 0; i < s->n; i++) free(s->b[i].text);
        for (int i = 0; i < s->nstr; i++) wfree(s->str[i]);
        free(s->str); free(s->b);
        if (s->own_il) ImageList_Destroy(s->own_il);
        free(s);
        ctl_set(h, NULL);
        return DefWindowProcW(h, msg, wp, lp);
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); paint(h, s, dc); EndPaint(h, &ps); return 0; }
    case WM_PRINTCLIENT: paint(h, s, (HDC)wp); return 0;
    case WM_SIZE: layout(h, s); InvalidateRect(h, NULL, TRUE); return 0;
    case WM_SETFONT: s->font = (HFONT)wp; layout(h, s); if (lp) InvalidateRect(h, NULL, TRUE); return 0;
    case WM_GETFONT: return (LRESULT)s->font;
    case WM_MOUSEMOVE: {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        int i = hit(s, pt);
        if (i >= 0 && (s->b[i].style & BTNS_SEP)) i = -1;
        if (i != s->hot) {
            int o = s->hot;
            s->hot = i;
            redraw_btn(h, s, o);
            redraw_btn(h, s, i);
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
        }
        return 0;
    }
    case WM_MOUSELEAVE: { int o = s->hot; if (s->pressed < 0) s->hot = -1; redraw_btn(h, s, o); return 0; }
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        int i = hit(s, pt);
        if (i < 0 || (s->b[i].style & BTNS_SEP) || !(s->b[i].state & TBSTATE_ENABLED)) return 0;
        TBtn *b = &s->b[i];
        int in_arrow = (b->style & BTNS_DROPDOWN) && ((b->style & BTNS_WHOLEDROPDOWN) || !(s->ex & TBSTYLE_EX_DRAWDDARROWS) || pt.x >= b->r.right - DDARROW);
        if (in_arrow) {
            s->pressed = i;
            b->state |= TBSTATE_PRESSED;
            redraw_btn(h, s, i);
            UpdateWindow(h);
            BOOL unhandled = !drop_down(h, s, i);
            if (IsWindow(h) && i < s->n) { s->b[i].state &= ~TBSTATE_PRESSED; s->pressed = -1; redraw_btn(h, s, i); }
            if (unhandled && !(b->style & BTNS_WHOLEDROPDOWN) && IsWindow(h)) click(h, s, i);
            return 0;
        }
        s->pressed = i;
        s->hot = i;
        SetCapture(h);
        redraw_btn(h, s, i);
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        int i = s->pressed;
        if (i < 0) return 0;
        s->pressed = -1;
        ReleaseCapture();
        redraw_btn(h, s, i);
        if (i < s->n && PtInRect(&s->b[i].r, pt)) click(h, s, i);
        return 0;
    }
    case WM_CAPTURECHANGED: if ((HWND)lp != h && s->pressed >= 0) { int i = s->pressed; s->pressed = -1; redraw_btn(h, s, i); } return 0;
    case WM_RBUTTONUP: { tb_notify(h, s, NM_RCLICK, NULL); return DefWindowProcW(h, msg, wp, lp); }
    case WM_NOTIFY: case WM_COMMAND: { HWND p = GetParent(h); return p ? SendMessageW(p, msg, wp, lp) : 0; }

    case TB_BUTTONSTRUCTSIZE: return 0;
    case TB_SETBITMAPSIZE: s->bmp_w = LOWORD(lp) ? LOWORD(lp) : 16; s->bmp_h = HIWORD(lp) ? HIWORD(lp) : 15; layout(h, s); return TRUE;
    case TB_SETBUTTONSIZE: s->btn_w = LOWORD(lp); s->btn_h = HIWORD(lp); layout(h, s); InvalidateRect(h, NULL, TRUE); return TRUE;
    case TB_GETBUTTONSIZE: { int bw, bh; base_size(h, s, &bw, &bh); return MAKELONG(bw, bh); }
    case TB_ADDBITMAP: return add_bitmap(h, s, (int)wp, (const TBADDBITMAP *)lp);
    case TB_ADDSTRINGA_: wide = 0; /* fall through */
    case TB_ADDSTRINGW: return add_strings(s, (HINSTANCE)wp, lp, wide);
    case TB_ADDBUTTONSA_: wide = 0; /* fall through */
    case TB_ADDBUTTONSW: return insert_buttons(h, s, s->n, (int)wp, (const TBBUTTON *)lp, wide);
    case TB_INSERTBUTTONA_: wide = 0; /* fall through */
    case TB_INSERTBUTTONW: return insert_buttons(h, s, (int)wp, 1, (const TBBUTTON *)lp, wide);
    case TB_DELETEBUTTON: {
        int i = (int)wp;
        if (i < 0 || i >= s->n) return FALSE;
        free(s->b[i].text);
        memmove(&s->b[i], &s->b[i + 1], sizeof(TBtn) * (size_t)(s->n - i - 1));
        s->n--;
        s->hot = s->pressed = -1;
        layout(h, s);
        InvalidateRect(h, NULL, TRUE);
        return TRUE;
    }
    case TB_BUTTONCOUNT: return s->n;
    case TB_GETBUTTON: if ((int)wp < 0 || (int)wp >= s->n || !lp) return FALSE; get_button(s, (int)wp, (TBBUTTON *)lp); return TRUE;
    case TB_COMMANDTOINDEX: return index_of(s, (int)wp);
    case TB_ENABLEBUTTON: return set_state(h, s, (int)wp, TBSTATE_ENABLED, LOWORD(lp) != 0);
    case TB_CHECKBUTTON: return set_state(h, s, (int)wp, TBSTATE_CHECKED, LOWORD(lp) != 0);
    case TB_PRESSBUTTON: return set_state(h, s, (int)wp, TBSTATE_PRESSED, LOWORD(lp) != 0);
    case TB_HIDEBUTTON: return set_state(h, s, (int)wp, TBSTATE_HIDDEN, LOWORD(lp) != 0);
    case TB_INDETERMINATE: return set_state(h, s, (int)wp, TBSTATE_INDETERMINATE, LOWORD(lp) != 0);
    case TB_MARKBUTTON: return set_state(h, s, (int)wp, TBSTATE_MARKED, LOWORD(lp) != 0);
    case TB_ISBUTTONENABLED: { int i = index_of(s, (int)wp); return i >= 0 && (s->b[i].state & TBSTATE_ENABLED); }
    case TB_ISBUTTONCHECKED: { int i = index_of(s, (int)wp); return i >= 0 && (s->b[i].state & TBSTATE_CHECKED); }
    case TB_ISBUTTONPRESSED: { int i = index_of(s, (int)wp); return i >= 0 && (s->b[i].state & TBSTATE_PRESSED); }
    case TB_ISBUTTONHIDDEN: { int i = index_of(s, (int)wp); return i >= 0 && (s->b[i].state & TBSTATE_HIDDEN); }
    case TB_SETSTATE: {
        int i = index_of(s, (int)wp);
        if (i < 0) return FALSE;
        BYTE o = s->b[i].state;
        s->b[i].state = (BYTE)LOWORD(lp);
        if ((o ^ s->b[i].state) & (TBSTATE_HIDDEN | TBSTATE_WRAP)) { layout(h, s); InvalidateRect(h, NULL, TRUE); }
        else redraw_btn(h, s, i);
        return TRUE;
    }
    case TB_GETSTATE: { int i = index_of(s, (int)wp); return i >= 0 ? s->b[i].state : -1; }
    case TB_GETITEMRECT: {
        int i = (int)wp;
        if (i < 0 || i >= s->n || !lp) return FALSE;
        layout(h, s);
        *(RECT *)lp = s->b[i].r;
        return TRUE;
    }
    case TB_GETRECT: {
        int i = index_of(s, (int)wp);
        if (i < 0 || !lp) return FALSE;
        layout(h, s);
        *(RECT *)lp = s->b[i].r;
        return TRUE;
    }
    case TB_AUTOSIZE: autosize(h, s); return 0;
    case TB_SETIMAGELIST: { HIMAGELIST o = s->il; s->il = (HIMAGELIST)lp; if (s->il) ImageList_GetIconSize(s->il, &s->bmp_w, &s->bmp_h); layout(h, s); InvalidateRect(h, NULL, TRUE); return (LRESULT)o; }
    case TB_GETIMAGELIST: return (LRESULT)s->il;
    case TB_SETHOTIMAGELIST: { HIMAGELIST o = s->hot_il; s->hot_il = (HIMAGELIST)lp; return (LRESULT)o; }
    case TB_GETHOTIMAGELIST: return (LRESULT)s->hot_il;
    case TB_SETDISABLEDIMAGELIST: { HIMAGELIST o = s->dis_il; s->dis_il = (HIMAGELIST)lp; return (LRESULT)o; }
    case TB_GETDISABLEDIMAGELIST: return (LRESULT)s->dis_il;
    case TB_LOADIMAGES: { TBADDBITMAP ab = { (HINSTANCE)lp, wp }; return add_bitmap(h, s, 0, &ab); }
    case TB_SETSTYLE: SetWindowLongW(h, GWL_STYLE, (LONG)lp); layout(h, s); InvalidateRect(h, NULL, TRUE); return 0;
    case TB_GETSTYLE: return style_of(h);
    case TB_SETEXTENDEDSTYLE: { DWORD o = s->ex, m = wp ? (DWORD)wp : 0xFFFFFFFFu; s->ex = (s->ex & ~m) | ((DWORD)lp & m); layout(h, s); InvalidateRect(h, NULL, TRUE); return (LRESULT)o; }
    case TB_GETEXTENDEDSTYLE: return (LRESULT)s->ex;
    case TB_SETMAXTEXTROWS: s->max_rows = (int)wp; layout(h, s); InvalidateRect(h, NULL, TRUE); return TRUE;
    case TB_GETTEXTROWS: return s->max_rows;
    case TB_SETBUTTONWIDTH: return TRUE;
    case TB_SETINDENT: s->indent = (int)wp; layout(h, s); InvalidateRect(h, NULL, TRUE); return TRUE;
    case TB_SETPARENT: { HWND o = notify_target(h, s); s->parent = (HWND)wp; return (LRESULT)o; }
    case TB_SETTOOLTIPS: s->tips = (HWND)wp; return 0;
    case TB_GETTOOLTIPS: return (LRESULT)s->tips;
    case TB_SETROWS: layout(h, s); if (lp) { RECT r = { 0, 0, 0, 0 }; SIZE sz; total_size(h, s, &sz); r.right = sz.cx; r.bottom = sz.cy; *(RECT *)lp = r; } return 0;
    case TB_GETROWS: { int rows = 1, y = -1; for (int i = 0; i < s->n; i++) if (!IsRectEmpty(&s->b[i].r)) { if (y >= 0 && s->b[i].r.top != y) rows++; y = s->b[i].r.top; } return rows; }
    case TB_GETMAXSIZE: case TB_GETIDEALSIZE: {
        SIZE sz;
        total_size_ex(h, s, &sz, 1);
        if (msg == TB_GETIDEALSIZE) { SIZE *o = (SIZE *)lp; if (!o) return FALSE; if (wp) o->cy = sz.cy; else o->cx = sz.cx; return TRUE; }
        if (lp) *(SIZE *)lp = sz;
        return TRUE;
    }
    case TB_HITTEST: {
        POINT *p = (POINT *)lp;
        if (!p) return -1;
        int i = hit(s, *p);
        if (i < 0) return -1 - s->n;
        return (s->b[i].style & BTNS_SEP) ? -1 - i : i;
    }
    case TB_GETHOTITEM: return s->hot;
    case TB_SETHOTITEM: { int o = s->hot; s->hot = (int)wp; redraw_btn(h, s, o); redraw_btn(h, s, s->hot); return o; }
    case TB_SETPADDING: { LRESULT o = MAKELONG(s->pad_x * 2, s->pad_y * 2); s->pad_x = LOWORD(lp) / 2; s->pad_y = HIWORD(lp) / 2; layout(h, s); return o; }
    case TB_GETPADDING: return MAKELONG(s->pad_x * 2, s->pad_y * 2);
    case TB_GETMETRICS: {
        TBMETRICS *m = (TBMETRICS *)lp;
        if (!m) return 0;
        if (m->dwMask & TBMF_PAD) { m->cxPad = s->pad_x * 2; m->cyPad = s->pad_y * 2; }
        if (m->dwMask & TBMF_BARPAD) { m->cxBarPad = 0; m->cyBarPad = 0; }
        if (m->dwMask & TBMF_BUTTONSPACING) { m->cxButtonSpacing = s->spacing_x; m->cyButtonSpacing = s->spacing_y; }
        return 0;
    }
    case TB_SETMETRICS: {
        const TBMETRICS *m = (const TBMETRICS *)lp;
        if (!m) return 0;
        if (m->dwMask & TBMF_PAD) { s->pad_x = m->cxPad / 2; s->pad_y = m->cyPad / 2; }
        if (m->dwMask & TBMF_BUTTONSPACING) { s->spacing_x = m->cxButtonSpacing; s->spacing_y = m->cyButtonSpacing; }
        layout(h, s);
        InvalidateRect(h, NULL, TRUE);
        return 0;
    }
    case TB_SETDRAWTEXTFLAGS: return 0;
    case TB_SETCMDID: { int i = (int)wp; if (i < 0 || i >= s->n) return FALSE; s->b[i].cmd = (int)lp; return TRUE; }
    case TB_CHANGEBITMAP: { int i = index_of(s, (int)wp); if (i < 0) return FALSE; s->b[i].image = LOWORD(lp); redraw_btn(h, s, i); return TRUE; }
    case TB_GETBITMAP: { int i = index_of(s, (int)wp); return i >= 0 ? s->b[i].image : 0; }
    case TB_GETBUTTONINFOA_: wide = 0; /* fall through */
    case TB_GETBUTTONINFOW: return button_info(h, s, (int)wp, (TBBUTTONINFOW *)lp, 0, wide);
    case TB_SETBUTTONINFOA_: wide = 0; /* fall through */
    case TB_SETBUTTONINFOW: return button_info(h, s, (int)wp, (TBBUTTONINFOW *)lp, 1, wide);
    case TB_GETBUTTONTEXTA: wide = 0; /* fall through */
    case TB_GETBUTTONTEXTW: {
        int i = index_of(s, (int)wp);
        if (i < 0) return -1;
        const WCHAR *t = s->b[i].text ? s->b[i].text : L"";
        if (wide) { if (lp) memcpy((void *)lp, t, 2 * ((size_t)wlen(t) + 1)); return wlen(t); }
        int n = WideCharToMultiByte(CP_ACP, 0, t, -1, NULL, 0, NULL, NULL) - 1;
        if (lp) WideCharToMultiByte(CP_ACP, 0, t, -1, (char *)lp, n + 1, NULL, NULL);
        return n;
    }
    case TB_GETSTRINGW_: {
        int i = HIWORD(wp), cap = LOWORD(wp);
        if (i < 0 || i >= s->nstr) return -1;
        const WCHAR *t = s->str[i] ? s->str[i] : L"";
        if (lp && cap > 0) { int n = MIN(wlen(t), cap - 1); memcpy((void *)lp, t, 2 * (size_t)n); ((WCHAR *)lp)[n] = 0; }
        return wlen(t);
    }
    case TB_CUSTOMIZE: return 0;
    case TB_SETLISTGAP_: return 0;
    case TB_SETUNICODEFORMAT_: case TB_GETUNICODEFORMAT_: return TRUE;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
