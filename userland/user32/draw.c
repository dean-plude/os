/*
 * draw.c — USER's drawing: rectangles, edges, frame controls, DrawText,
 * icons, focus rectangles, and the system colors and fonts
 */
#include "u32.h"

/* -----------------------------------------------------------------------
 * System colors (Windows 10's)
 * ----------------------------------------------------------------------- */
static COLORREF g_sys_colors[31] = {
    0xC8C8C8, 0x000000, 0xD1B499, 0xDBCDBF, 0xF0F0F0, 0xFFFFFF, 0x646464, 0x000000, 0x000000, 0x000000,
    0xB4B4B4, 0xFCF7F4, 0xABABAB, 0xD77800, 0xFFFFFF, 0xF0F0F0, 0xA0A0A0, 0x6D6D6D, 0x000000, 0x544E43,
    0xFFFFFF, 0x696969, 0xE3E3E3, 0x000000, 0xE1FFFF, 0x000000, 0xCC6600, 0xEAD1B9, 0xF2E4D7, 0xFF9933, 0xF0F0F0 };
static HBRUSH g_sys_brushes[31];

COLORREF sys_color(int i) { return i >= 0 && i < 31 ? g_sys_colors[i] : 0; }

HBRUSH sys_brush(int i)
{
    if (i < 0 || i >= 31) return 0;
    if (!g_sys_brushes[i]) g_sys_brushes[i] = CreateSolidBrush(g_sys_colors[i]);
    return g_sys_brushes[i];
}

USERAPI DWORD GetSysColor(int i) { return sys_color(i); }
USERAPI HBRUSH GetSysColorBrush(int i) { return sys_brush(i); }
USERAPI BOOL SetSysColors(int n, const INT *idx, const COLORREF *c)
{
    for (int i = 0; i < n; i++) if (idx[i] >= 0 && idx[i] < 31) {
        g_sys_colors[idx[i]] = c[i];
        if (g_sys_brushes[idx[i]]) { DeleteObject(g_sys_brushes[idx[i]]); g_sys_brushes[idx[i]] = 0; }
    }
    return TRUE;
}

static HFONT g_gui_font, g_gui_bold;
HFONT gui_font(void)
{
    if (!g_gui_font) g_gui_font = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    return g_gui_font;
}
HFONT gui_font_bold(void)
{
    if (!g_gui_bold) g_gui_bold = CreateFontW(-12, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    return g_gui_bold;
}

/* -----------------------------------------------------------------------
 * Pixels (for the few things GDI has no call for: XOR, alpha icons)
 * ----------------------------------------------------------------------- */
static DWORD *px(NOVA_DC *d, int x, int y)
{
    if (x < 0 || y < 0 || x >= d->w || y >= d->h) return NULL;
    if (d->has_vis && (x < d->vis.left || x >= d->vis.right || y < d->vis.top || y >= d->vis.bottom)) return NULL;
    if (d->has_clip && (x < d->clip.left || x >= d->clip.right || y < d->clip.top || y >= d->clip.bottom)) return NULL;
    int row = d->flip ? d->h - 1 - y : y;
    return d->bits + (size_t)row * d->stride + x;
}

static void mark_dc(NOVA_DC *d, int x0, int y0, int x1, int y1)
{
    (void)d; (void)x0; (void)y0; (void)x1; (void)y1;       /* window DCs are marked when released */
}

static void xor_rect(HDC dc, const RECT *r, int dotted)
{
    NOVA_DC *d = (NOVA_DC *)dc;
    if (!d || !d->bits) return;
    int ox = d->org_x, oy = d->org_y;
    for (int y = r->top; y < r->bottom; y++)
        for (int x = r->left; x < r->right; x++) {
            if (dotted) {
                int edge = y == r->top || y == r->bottom - 1 || x == r->left || x == r->right - 1;
                if (!edge || ((x + y) & 1)) continue;
            }
            DWORD *p = px(d, x + ox, y + oy);
            if (p) *p ^= 0xFFFFFF;
        }
    mark_dc(d, r->left, r->top, r->right, r->bottom);
}

/* -----------------------------------------------------------------------
 * Rectangles
 * ----------------------------------------------------------------------- */
void fill_rect(HDC dc, const RECT *r, COLORREF c)
{
    COLORREF old = SetBkColor(dc, c);
    ExtTextOutW(dc, 0, 0, ETO_OPAQUE, r, NULL, 0, NULL);
    SetBkColor(dc, old);
}

void frame_rect(HDC dc, const RECT *r, COLORREF c)
{
    RECT e;
    SetRect(&e, r->left, r->top, r->right, r->top + 1); fill_rect(dc, &e, c);
    SetRect(&e, r->left, r->bottom - 1, r->right, r->bottom); fill_rect(dc, &e, c);
    SetRect(&e, r->left, r->top, r->left + 1, r->bottom); fill_rect(dc, &e, c);
    SetRect(&e, r->right - 1, r->top, r->right, r->bottom); fill_rect(dc, &e, c);
}

static HBRUSH real_brush(HBRUSH br)
{
    if ((ULONG_PTR)br > 0 && (ULONG_PTR)br <= 31) return sys_brush((int)(ULONG_PTR)br - 1);
    return br;
}

USERAPI int FillRect(HDC dc, const RECT *r, HBRUSH br)
{
    if (!dc || !r) return 0;
    br = real_brush(br);
    if (!br) return 0;
    HGDIOBJ old = SelectObject(dc, br);
    PatBlt(dc, r->left, r->top, r->right - r->left, r->bottom - r->top, PATCOPY);
    SelectObject(dc, old);
    return 1;
}

USERAPI int FrameRect(HDC dc, const RECT *r, HBRUSH br)
{
    if (!dc || !r || IsRectEmpty(r)) return 0;
    br = real_brush(br);
    HGDIOBJ old = SelectObject(dc, br);
    PatBlt(dc, r->left, r->top, r->right - r->left, 1, PATCOPY);
    PatBlt(dc, r->left, r->bottom - 1, r->right - r->left, 1, PATCOPY);
    PatBlt(dc, r->left, r->top, 1, r->bottom - r->top, PATCOPY);
    PatBlt(dc, r->right - 1, r->top, 1, r->bottom - r->top, PATCOPY);
    SelectObject(dc, old);
    return 1;
}

USERAPI BOOL InvertRect(HDC dc, const RECT *r) { if (!dc || !r) return FALSE; xor_rect(dc, r, 0); return TRUE; }
USERAPI BOOL DrawFocusRect(HDC dc, const RECT *r) { if (!dc || !r) return FALSE; xor_rect(dc, r, 1); return TRUE; }
void draw_focus(HDC dc, const RECT *r) { xor_rect(dc, r, 1); }

USERAPI BOOL PaintRgn_(HDC dc, HRGN r) { (void)dc; (void)r; return TRUE; }

/* -----------------------------------------------------------------------
 * Edges
 * ----------------------------------------------------------------------- */
static void edge_lines(HDC dc, RECT *r, COLORREF tl, COLORREF br, UINT flags)
{
    RECT e;
    if (flags & BF_TOP)    { SetRect(&e, r->left, r->top, r->right, r->top + 1); fill_rect(dc, &e, tl); }
    if (flags & BF_LEFT)   { SetRect(&e, r->left, r->top, r->left + 1, r->bottom); fill_rect(dc, &e, tl); }
    if (flags & BF_BOTTOM) { SetRect(&e, r->left, r->bottom - 1, r->right, r->bottom); fill_rect(dc, &e, br); }
    if (flags & BF_RIGHT)  { SetRect(&e, r->right - 1, r->top, r->right, r->bottom); fill_rect(dc, &e, br); }
    if (flags & BF_LEFT) r->left++;
    if (flags & BF_TOP) r->top++;
    if (flags & BF_RIGHT) r->right--;
    if (flags & BF_BOTTOM) r->bottom--;
}

USERAPI BOOL DrawEdge(HDC dc, LPRECT rc, UINT edge, UINT flags)
{
    if (!dc || !rc) return FALSE;
    RECT r = *rc;
    COLORREF light = sys_color(COLOR_3DLIGHT), hi = sys_color(COLOR_3DHILIGHT), sh = sys_color(COLOR_3DSHADOW),
             dk = sys_color(COLOR_3DDKSHADOW);
    if (flags & BF_MONO) { light = hi = sh = dk = sys_color(COLOR_WINDOWFRAME); }
    if (flags & BF_FLAT) { light = hi = sh = dk = sys_color(COLOR_3DSHADOW); }
    UINT outer = edge & (BDR_RAISEDOUTER | BDR_SUNKENOUTER), inner = edge & (BDR_RAISEDINNER | BDR_SUNKENINNER);
    if (outer == BDR_RAISEDOUTER) edge_lines(dc, &r, (flags & BF_SOFT) ? hi : light, dk, flags);
    else if (outer == BDR_SUNKENOUTER) edge_lines(dc, &r, sh, hi, flags);
    else if (outer) edge_lines(dc, &r, sh, sh, flags);
    if (inner == BDR_RAISEDINNER) edge_lines(dc, &r, (flags & BF_SOFT) ? light : hi, sh, flags);
    else if (inner == BDR_SUNKENINNER) edge_lines(dc, &r, dk, light, flags);
    else if (inner) edge_lines(dc, &r, sh, sh, flags);
    if (flags & BF_MIDDLE) fill_rect(dc, &r, (flags & BF_MONO) ? sys_color(COLOR_WINDOW) : sys_color(COLOR_3DFACE));
    if (flags & BF_ADJUST) *rc = r;
    return TRUE;
}

/* A check mark in a box of @size pixels at (x, y) */
void draw_check_mark(HDC dc, int x, int y, int size, COLORREF c)
{
    /* two strokes, 2px thick: down-right then up-right */
    int s = size;
    POINT a = { x + s * 2 / 10, y + s * 5 / 10 }, b = { x + s * 4 / 10, y + s * 7 / 10 }, e = { x + s * 8 / 10, y + s * 3 / 10 };
    HPEN pen = CreatePen(PS_SOLID, s >= 12 ? 2 : 1, c);
    HGDIOBJ op = SelectObject(dc, pen);
    MoveToEx(dc, a.x, a.y, NULL);
    LineTo(dc, b.x, b.y);
    LineTo(dc, e.x, e.y);
    SelectObject(dc, op);
    DeleteObject(pen);
}

/* A small filled triangle in @r: 0 up, 1 down, 2 left, 3 right */
void draw_arrow(HDC dc, const RECT *r, int dir, COLORREF c)
{
    int w = r->right - r->left, h = r->bottom - r->top;
    int s = MIN(w, h) / 3;
    if (s < 2) s = 2;
    if (s > 5) s = 5;
    int cx = r->left + w / 2, cy = r->top + h / 2;
    for (int i = 0; i < s; i++) {
        RECT l;
        switch (dir) {
        case 0: SetRect(&l, cx - i, cy - s / 2 + i, cx + i + 1, cy - s / 2 + i + 1); break;
        case 1: SetRect(&l, cx - (s - 1 - i), cy - s / 2 + i, cx + (s - 1 - i) + 1, cy - s / 2 + i + 1); break;
        case 2: SetRect(&l, cx - s / 2 + i, cy - i, cx - s / 2 + i + 1, cy + i + 1); break;
        default: SetRect(&l, cx - s / 2 + i, cy - (s - 1 - i), cx - s / 2 + i + 1, cy + (s - 1 - i) + 1); break;
        }
        fill_rect(dc, &l, c);
    }
}

USERAPI BOOL DrawFrameControl(HDC dc, LPRECT rc, UINT type, UINT state)
{
    if (!dc || !rc) return FALSE;
    RECT r = *rc;
    COLORREF fg = (state & DFCS_INACTIVE) ? sys_color(COLOR_GRAYTEXT) : sys_color(COLOR_BTNTEXT);
    switch (type) {
    case DFC_BUTTON: {
        UINT kind = state & 0xFF;
        if (kind == DFCS_BUTTONPUSH) {
            fill_rect(dc, &r, (state & DFCS_PUSHED) ? 0xF7E4CC : 0xE1E1E1);
            frame_rect(dc, &r, (state & DFCS_PUSHED) ? 0x995400 : 0xADADAD);
            return TRUE;
        }
        int s = MIN(r.right - r.left, r.bottom - r.top);
        RECT b = { r.left, r.top, r.left + s, r.top + s };
        if (kind == DFCS_BUTTONRADIO || kind == DFCS_BUTTONRADIOIMAGE || kind == DFCS_BUTTONRADIOMASK) {
            HBRUSH fb = CreateSolidBrush((state & DFCS_INACTIVE) ? 0xF0F0F0 : 0xFFFFFF);
            HPEN pen = CreatePen(PS_SOLID, 1, (state & DFCS_INACTIVE) ? 0xBFBFBF : 0x333333);
            HGDIOBJ ob = SelectObject(dc, fb), op = SelectObject(dc, pen);
            Ellipse(dc, b.left, b.top, b.right, b.bottom);
            if (state & DFCS_CHECKED) {
                HBRUSH db = CreateSolidBrush(fg);
                SelectObject(dc, db);
                SelectObject(dc, GetStockObject(NULL_PEN));
                int m = s * 3 / 10;
                Ellipse(dc, b.left + m, b.top + m, b.right - m + 1, b.bottom - m + 1);
                SelectObject(dc, fb);
                DeleteObject(db);
            }
            SelectObject(dc, ob); SelectObject(dc, op);
            DeleteObject(fb); DeleteObject(pen);
            return TRUE;
        }
        fill_rect(dc, &b, (state & DFCS_INACTIVE) ? 0xF0F0F0 : 0xFFFFFF);
        frame_rect(dc, &b, (state & DFCS_INACTIVE) ? 0xBFBFBF : 0x333333);
        if (state & DFCS_CHECKED) {
            if (kind == DFCS_BUTTON3STATE) { RECT i = b; InflateRect(&i, -3, -3); fill_rect(dc, &i, fg); }
            else draw_check_mark(dc, b.left, b.top, s, fg);
        }
        return TRUE;
    }
    case DFC_SCROLL: {
        int dir = -1;
        switch (state & 0xFF) {
        case DFCS_SCROLLUP: dir = 0; break;
        case DFCS_SCROLLDOWN: case DFCS_SCROLLCOMBOBOX: dir = 1; break;
        case DFCS_SCROLLLEFT: dir = 2; break;
        case DFCS_SCROLLRIGHT: dir = 3; break;
        case DFCS_SCROLLSIZEGRIP: case DFCS_SCROLLSIZEGRIPRIGHT:
            fill_rect(dc, &r, sys_color(COLOR_3DFACE));
            for (int i = 0; i < 3; i++) {
                RECT d = { r.right - 3 - i * 4, r.bottom - 3, r.right - 1 - i * 4, r.bottom - 1 };
                fill_rect(dc, &d, sys_color(COLOR_3DSHADOW));
                RECT e = { r.right - 3, r.bottom - 3 - i * 4, r.right - 1, r.bottom - 1 - i * 4 };
                fill_rect(dc, &e, sys_color(COLOR_3DSHADOW));
            }
            return TRUE;
        }
        if (!(state & DFCS_TRANSPARENT)) fill_rect(dc, &r, (state & DFCS_PUSHED) ? 0x999999 : (state & DFCS_HOT) ? 0xDADADA : sys_color(COLOR_3DFACE));
        if (dir >= 0) draw_arrow(dc, &r, dir, (state & DFCS_PUSHED) ? 0xFFFFFF : fg);
        return TRUE;
    }
    case DFC_CAPTION: {
        if (!(state & DFCS_TRANSPARENT)) fill_rect(dc, &r, (state & DFCS_PUSHED) ? 0xCCCCCC : sys_color(COLOR_3DFACE));
        int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, k = MIN(r.right - r.left, r.bottom - r.top) / 4;
        HPEN pen = CreatePen(PS_SOLID, 1, fg);
        HGDIOBJ op = SelectObject(dc, pen);
        switch (state & 0xFF) {
        case DFCS_CAPTIONCLOSE:
            MoveToEx(dc, cx - k, cy - k, NULL); LineTo(dc, cx + k + 1, cy + k + 1);
            MoveToEx(dc, cx + k, cy - k, NULL); LineTo(dc, cx - k - 1, cy + k + 1);
            break;
        case DFCS_CAPTIONMIN: MoveToEx(dc, cx - k, cy + k, NULL); LineTo(dc, cx + k + 1, cy + k); break;
        case DFCS_CAPTIONMAX: { RECT b = { cx - k, cy - k, cx + k + 1, cy + k + 1 }; frame_rect(dc, &b, fg); break; }
        case DFCS_CAPTIONRESTORE: { RECT b = { cx - k, cy - k + 2, cx + k - 1, cy + k + 1 }; frame_rect(dc, &b, fg); break; }
        case DFCS_CAPTIONHELP: { RECT t = r; SetBkMode(dc, TRANSPARENT); DrawTextW(dc, L"?", 1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE); break; }
        }
        SelectObject(dc, op);
        DeleteObject(pen);
        return TRUE;
    }
    case DFC_MENU: {
        if (!(state & DFCS_TRANSPARENT)) fill_rect(dc, &r, 0xFFFFFF);
        switch (state & 0xFF) {
        case DFCS_MENUARROW: draw_arrow(dc, &r, 3, fg); break;
        case DFCS_MENUCHECK: draw_check_mark(dc, r.left, r.top, MIN(r.right - r.left, r.bottom - r.top), fg); break;
        case DFCS_MENUBULLET: {
            int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
            RECT b = { cx - 2, cy - 2, cx + 3, cy + 3 };
            fill_rect(dc, &b, fg);
            break;
        }
        }
        return TRUE;
    }
    }
    return FALSE;
}

/* -----------------------------------------------------------------------
 * Text
 * ----------------------------------------------------------------------- */
int text_width(HDC dc, const WCHAR *s, int n)
{
    if (n <= 0) return 0;
    SIZE sz;
    GetTextExtentPoint32W(dc, s, n, &sz);
    return sz.cx;
}

int font_height(HDC dc)
{
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    return tm.tmHeight;
}

/* A line of text with '&' prefixes and tabs dealt with */
typedef struct { WCHAR *s; int n; int ul; } Line;

static int expand_line(const WCHAR *src, int n, UINT fmt, WCHAR *out, int *ul)
{
    int k = 0;
    *ul = -1;
    for (int i = 0; i < n; i++) {
        WCHAR c = src[i];
        if (c == '&' && !(fmt & DT_NOPREFIX)) {
            if (i + 1 < n && src[i + 1] == '&') { out[k++] = '&'; i++; continue; }
            if (i + 1 < n) { *ul = k; continue; }
            continue;
        }
        out[k++] = c;
    }
    return k;
}

static int line_width(HDC dc, const WCHAR *s, int n, UINT fmt, int tabw)
{
    if (!(fmt & DT_EXPANDTABS)) return text_width(dc, s, n);
    int x = 0, st = 0;
    for (int i = 0; i <= n; i++) {
        if (i < n && s[i] != '\t') continue;
        x += text_width(dc, s + st, i - st);
        if (i < n) x = (x / tabw + 1) * tabw;
        st = i + 1;
    }
    return x;
}

static void out_line(HDC dc, int x, int y, const WCHAR *s, int n, UINT fmt, int tabw, const RECT *clip, int ul)
{
    UINT eto = (fmt & DT_NOCLIP) ? 0 : ETO_CLIPPED;
    if (!(fmt & DT_EXPANDTABS)) {
        ExtTextOutW(dc, x, y, eto, clip, s, (UINT)n, NULL);
    } else {
        int cx = 0, st = 0;
        for (int i = 0; i <= n; i++) {
            if (i < n && s[i] != '\t') continue;
            if (i > st) ExtTextOutW(dc, x + cx, y, eto, clip, s + st, (UINT)(i - st), NULL);
            cx += text_width(dc, s + st, i - st);
            if (i < n) cx = (cx / tabw + 1) * tabw;
            st = i + 1;
        }
    }
    if (ul >= 0 && ul < n && !(fmt & DT_HIDEPREFIX)) {
        int ux = x + line_width(dc, s, ul, fmt, tabw), uw = text_width(dc, s + ul, 1);
        TEXTMETRICW tm;
        GetTextMetricsW(dc, &tm);
        RECT u = { ux, y + tm.tmAscent + 1, ux + uw, y + tm.tmAscent + 2 };
        if (!clip || (u.top >= clip->top && u.bottom <= clip->bottom) || (fmt & DT_NOCLIP)) fill_rect(dc, &u, GetTextColor(dc));
    }
}

/* Shorten a line to fit @maxw with "..." (at the end, or in the middle of a path) */
static int ellipsize(HDC dc, WCHAR *s, int n, int maxw, UINT fmt, int tabw)
{
    if (line_width(dc, s, n, fmt, tabw) <= maxw) return n;
    static const WCHAR dots[] = L"...";
    int dw = text_width(dc, dots, 3);
    if (fmt & DT_PATH_ELLIPSIS) {
        int slash = -1;
        for (int i = n - 1; i >= 0; i--) if (s[i] == '\\' || s[i] == '/') { slash = i; break; }
        if (slash > 0) {
            int tail = n - slash;
            int tw = line_width(dc, s + slash, tail, fmt, tabw);
            int keep = slash;
            while (keep > 0 && line_width(dc, s, keep, fmt, tabw) + dw + tw > maxw) keep--;
            WCHAR tmp[1024];
            int k = 0;
            for (int i = 0; i < keep && k < 1000; i++) tmp[k++] = s[i];
            for (int i = 0; i < 3; i++) tmp[k++] = '.';
            for (int i = 0; i < tail && k < 1020; i++) tmp[k++] = s[slash + i];
            memcpy(s, tmp, 2 * (size_t)k);
            return k;
        }
    }
    int keep = n;
    while (keep > 0 && line_width(dc, s, keep, fmt, tabw) + dw > maxw) keep--;
    s[keep] = '.'; s[keep + 1] = '.'; s[keep + 2] = '.';
    return keep + 3;
}

void draw_text_w(HDC dc, const WCHAR *s, int n, RECT *r, UINT fmt)
{
    DrawTextW(dc, s, n, r, fmt);
}

static int draw_text(HDC dc, const WCHAR *src, int len, RECT *r, UINT fmt, int tabw_chars)
{
    if (!dc || !r) return 0;
    if (!src) len = 0;
    if (len < 0) len = wlen(src);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    int lh = tm.tmHeight + ((fmt & DT_EXTERNALLEADING) ? tm.tmExternalLeading : 0);
    int tabw = (tabw_chars > 0 ? tabw_chars : 8) * (tm.tmAveCharWidth > 0 ? tm.tmAveCharWidth : 7);
    int maxw = r->right - r->left;
    if (fmt & DT_SINGLELINE) fmt &= ~DT_WORDBREAK;

    /* lay out the lines */
    WCHAR *buf = malloc(2 * ((size_t)len + 8));
    if (!buf) return 0;
    int ul_all;
    int blen = expand_line(src, len, fmt, buf, &ul_all);
    typedef struct { int start, n; } Span;
    int cap = 64, count = 0;
    Span *lines = malloc(sizeof(Span) * cap);
    int pos = 0;
    while (pos <= blen) {
        int end = pos;
        if (!(fmt & DT_SINGLELINE)) while (end < blen && buf[end] != '\n' && buf[end] != '\r') end++;
        else end = blen;
        int seg_start = pos, seg_end = end;
        do {
            int e = seg_end;
            if ((fmt & DT_WORDBREAK) && maxw > 0 && line_width(dc, buf + seg_start, e - seg_start, fmt, tabw) > maxw) {
                /* longest prefix that fits, broken at a space if there is one */
                int lo = seg_start, fit = seg_start;
                for (int i = seg_start + 1; i <= e; i++) {
                    if (line_width(dc, buf + seg_start, i - seg_start, fmt, tabw) > maxw) break;
                    fit = i;
                }
                int brk = -1;
                for (int i = fit; i > seg_start; i--) if (i < e && (buf[i] == ' ' || buf[i] == '\t')) { brk = i; break; }
                if (brk < 0 && fit < e && buf[fit] == ' ') brk = fit;
                if (brk > seg_start) e = brk;
                else if (fit > lo) e = fit;
                else e = seg_start + 1;
            }
            if (count == cap) { cap *= 2; lines = realloc(lines, sizeof(Span) * cap); }
            lines[count].start = seg_start; lines[count].n = e - seg_start;
            count++;
            seg_start = e;
            while (seg_start < seg_end && buf[seg_start] == ' ' && (fmt & DT_WORDBREAK)) seg_start++;
        } while (seg_start < seg_end);
        if (end >= blen) break;
        pos = end + 1;
        if (buf[end] == '\r' && pos < blen && buf[pos] == '\n') pos++;
    }
    if (!count) { lines[0].start = 0; lines[0].n = 0; count = 1; }

    int total = count * lh;
    if (fmt & DT_CALCRECT) {
        int w = 0;
        for (int i = 0; i < count; i++) {
            int lw = line_width(dc, buf + lines[i].start, lines[i].n, fmt, tabw);
            if (lw > w) w = lw;
        }
        if (!(fmt & DT_WORDBREAK) || w > maxw || !maxw) r->right = r->left + w;
        if ((fmt & (DT_END_ELLIPSIS | DT_PATH_ELLIPSIS | DT_WORD_ELLIPSIS)) && maxw > 0 && w > maxw) r->right = r->left + maxw;
        r->bottom = r->top + total;
        free(lines); free(buf);
        return total;
    }
    int y = r->top;
    if (fmt & DT_SINGLELINE) {
        if (fmt & DT_VCENTER) y = r->top + (r->bottom - r->top - lh) / 2;
        else if (fmt & DT_BOTTOM) y = r->bottom - lh;
    }
    for (int i = 0; i < count; i++, y += lh) {
        if (!(fmt & DT_NOCLIP) && y >= r->bottom) break;
        WCHAR tmp[1100];
        int n = MIN(lines[i].n, 1024);
        memcpy(tmp, buf + lines[i].start, 2 * (size_t)n);
        int ul = ul_all >= lines[i].start && ul_all < lines[i].start + lines[i].n ? ul_all - lines[i].start : -1;
        if ((fmt & (DT_END_ELLIPSIS | DT_PATH_ELLIPSIS | DT_WORD_ELLIPSIS)) && maxw > 0) {
            int m = ellipsize(dc, tmp, n, maxw, fmt, tabw);
            if (m != n) ul = -1;
            n = m;
        }
        int lw = line_width(dc, tmp, n, fmt, tabw);
        int x = r->left;
        if (fmt & DT_CENTER) x = r->left + (maxw - lw) / 2;
        else if (fmt & DT_RIGHT) x = r->right - lw;
        if (!(fmt & DT_CALCRECT) && !(fmt & DT_PREFIXONLY)) out_line(dc, x, y, tmp, n, fmt, tabw, r, ul);
    }
    free(lines); free(buf);
    if (fmt & (DT_VCENTER | DT_BOTTOM)) return y - r->top;
    return total;
}

USERAPI int DrawTextW(HDC dc, LPCWSTR s, int len, LPRECT r, UINT fmt) { return draw_text(dc, s, len, r, fmt, 8); }

USERAPI int DrawTextExW(HDC dc, LPWSTR s, int len, LPRECT r, UINT fmt, LPDRAWTEXTPARAMS p)
{
    int tabs = (fmt & DT_TABSTOP) ? (int)((fmt >> 8) & 0xFF) : 8;
    if (fmt & DT_TABSTOP) fmt &= 0xFFFF00FF | DT_EXPANDTABS;
    if (p && p->iTabLength) tabs = p->iTabLength;
    RECT rr = *r;
    if (p) { rr.left += p->iLeftMargin; rr.right -= p->iRightMargin; }
    int h = draw_text(dc, s, len, &rr, fmt, tabs);
    if (fmt & DT_CALCRECT) { r->right = rr.right + (p ? p->iRightMargin : 0); r->bottom = rr.bottom; }
    if (p) p->uiLengthDrawn = (UINT)(len < 0 ? wlen(s) : len);
    return h;
}

USERAPI int DrawTextA(HDC dc, LPCSTR s, int len, LPRECT r, UINT fmt)
{
    WCHAR *w = a2w(s ? s : "", len);
    int h = draw_text(dc, w, -1, r, fmt, 8);
    free(w);
    return h;
}

USERAPI int DrawTextExA(HDC dc, LPSTR s, int len, LPRECT r, UINT fmt, LPDRAWTEXTPARAMS p)
{
    WCHAR *w = a2w(s ? s : "", len);
    int h = DrawTextExW(dc, w, -1, r, fmt, p);
    free(w);
    return h;
}

USERAPI LONG TabbedTextOutW(HDC dc, int x, int y, LPCWSTR s, int n, int nt, const INT *tabs, int org)
{
    if (n < 0) n = wlen(s);
    int tw = nt >= 1 && tabs ? tabs[0] : 8 * 7;
    int cx = 0, st = 0, ti = 0;
    for (int i = 0; i <= n; i++) {
        if (i < n && s[i] != '\t') continue;
        if (i > st) TextOutW(dc, x + cx, y, s + st, i - st);
        cx += text_width(dc, s + st, i - st);
        if (i < n) {
            if (nt > 1 && tabs) {
                while (ti < nt && tabs[ti] - org + x <= x + cx) ti++;
                cx = ti < nt ? tabs[ti] - org : (cx / tw + 1) * tw;
            } else cx = (cx / (tw > 0 ? tw : 56) + 1) * (tw > 0 ? tw : 56);
        }
        st = i + 1;
    }
    return MAKELONG(cx, font_height(dc));
}

USERAPI LONG TabbedTextOutA(HDC dc, int x, int y, LPCSTR s, int n, int nt, const INT *tabs, int org)
{
    WCHAR *w = a2w(s, n);
    LONG r = TabbedTextOutW(dc, x, y, w, -1, nt, tabs, org);
    free(w);
    return r;
}

USERAPI DWORD GetTabbedTextExtentW(HDC dc, LPCWSTR s, int n, int nt, const INT *tabs)
{
    (void)nt; (void)tabs;
    if (n < 0) n = wlen(s);
    return (DWORD)MAKELONG(line_width(dc, s, n, DT_EXPANDTABS, 56), font_height(dc));
}

USERAPI DWORD GetTabbedTextExtentA(HDC dc, LPCSTR s, int n, int nt, const INT *tabs)
{
    WCHAR *w = a2w(s, n);
    DWORD r = GetTabbedTextExtentW(dc, w, -1, nt, tabs);
    free(w);
    return r;
}

typedef BOOL (CALLBACK *GRAYSTRINGPROC_)(HDC, LPARAM, int);
USERAPI BOOL GrayStringW(HDC dc, HBRUSH br, GRAYSTRINGPROC_ fn, LPARAM lp, int n, int x, int y, int w, int h)
{
    (void)br; (void)w; (void)h;
    COLORREF old = SetTextColor(dc, sys_color(COLOR_GRAYTEXT));
    BOOL r = fn ? fn(dc, lp, n) : TextOutW(dc, x, y, (LPCWSTR)lp, n ? n : wlen((LPCWSTR)lp));
    SetTextColor(dc, old);
    return r;
}

USERAPI BOOL GrayStringA(HDC dc, HBRUSH br, GRAYSTRINGPROC_ fn, LPARAM lp, int n, int x, int y, int w, int h)
{
    if (fn) return GrayStringW(dc, br, fn, lp, n, x, y, w, h);
    WCHAR *s = a2w((LPCSTR)lp, n ? n : -1);
    BOOL r = GrayStringW(dc, br, NULL, (LPARAM)s, 0, x, y, w, h);
    free(s);
    return r;
}

USERAPI BOOL DrawStateW(HDC dc, HBRUSH br, DRAWSTATEPROC fn, LPARAM lp, WPARAM wp, int x, int y, int cx, int cy, UINT flags)
{
    (void)br;
    UINT type = flags & 0xF;
    COLORREF old = 0;
    int dis = (flags & DSS_DISABLED) != 0;
    if (dis) old = SetTextColor(dc, sys_color(COLOR_GRAYTEXT));
    switch (type) {
    case DST_TEXT: case DST_PREFIXTEXT: {
        RECT r = { x, y, x + (cx ? cx : 10000), y + (cy ? cy : 10000) };
        int oldm = SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, (LPCWSTR)lp, wp ? (int)wp : -1, &r, (type == DST_TEXT ? DT_NOPREFIX : 0) | DT_NOCLIP |
                  ((flags & DSS_HIDEPREFIX) ? DT_HIDEPREFIX : 0));
        SetBkMode(dc, oldm);
        break;
    }
    case DST_ICON: draw_icon(dc, x, y, (HICON)lp, cx, cy); break;
    case DST_BITMAP: {
        HDC m = CreateCompatibleDC(dc);
        HGDIOBJ o = SelectObject(m, (HBITMAP)lp);
        BITMAP bm;
        GetObjectW((HBITMAP)lp, sizeof(bm), &bm);
        BitBlt(dc, x, y, cx ? cx : bm.bmWidth, cy ? cy : bm.bmHeight, m, 0, 0, SRCCOPY);
        SelectObject(m, o);
        DeleteDC(m);
        break;
    }
    case DST_COMPLEX: if (fn) fn(dc, lp, wp, cx, cy); break;
    }
    if (dis) SetTextColor(dc, old);
    return TRUE;
}

USERAPI BOOL DrawStateA(HDC dc, HBRUSH br, DRAWSTATEPROC fn, LPARAM lp, WPARAM wp, int x, int y, int cx, int cy, UINT flags)
{
    UINT type = flags & 0xF;
    if (type == DST_TEXT || type == DST_PREFIXTEXT) {
        WCHAR *s = a2w((LPCSTR)lp, wp ? (int)wp : -1);
        BOOL r = DrawStateW(dc, br, fn, (LPARAM)s, 0, x, y, cx, cy, flags);
        free(s);
        return r;
    }
    return DrawStateW(dc, br, fn, lp, wp, x, y, cx, cy, flags);
}

USERAPI BOOL DrawCaption(HWND h, HDC dc, const RECT *r, UINT flags)
{
    (void)flags;
    Wnd *w = W(h);
    if (!w) return FALSE;
    fill_rect(dc, r, sys_color(COLOR_ACTIVECAPTION));
    RECT t = *r;
    t.left += 4;
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, w->text, -1, &t, DT_SINGLELINE | DT_VCENTER);
    return TRUE;
}

/* -----------------------------------------------------------------------
 * Icons: 32-bit ARGB images, blended over the destination
 * ----------------------------------------------------------------------- */
int icon_size(HICON h, int *cx, int *cy)
{
    Icon *ic = icon_of(h);
    if (!ic) return 0;
    *cx = ic->w; *cy = ic->h;
    return 1;
}

/* The image of @ic nearest to cx x cy */
static Icon *pick(Icon *ic, int cx, int cy)
{
    Icon *best = ic;
    for (Icon *i = ic; i; i = i->more) {
        int d = abs(i->w - cx) + abs(i->h - cy), bd = abs(best->w - cx) + abs(best->h - cy);
        if (d < bd || (d == bd && i->w > best->w)) best = i;
    }
    return best;
}

void draw_icon(HDC dc, int x, int y, HICON h, int cx, int cy)
{
    Icon *ic = icon_of(h);
    NOVA_DC *d = (NOVA_DC *)dc;
    if (!ic || !d || !d->bits) return;
    if (cx <= 0) cx = ic->w;
    if (cy <= 0) cy = ic->h;
    ic = pick(ic, cx, cy);
    if (!ic->argb) return;
    for (int j = 0; j < cy; j++) {
        int sy = j * ic->h / cy;
        for (int i = 0; i < cx; i++) {
            int sx = i * ic->w / cx;
            DWORD s = ic->argb[(size_t)sy * ic->w + sx];
            int a = (int)(s >> 24);
            if (!a) continue;
            DWORD *p = px(d, x + i + d->org_x, y + j + d->org_y);
            if (!p) continue;
            int sr = (s >> 16) & 0xFF, sg = (s >> 8) & 0xFF, sb = s & 0xFF;
            DWORD t = *p;
            int tr, tg, tb;
            if (d->fmt) { tr = (t >> 16) & 0xFF; tg = (t >> 8) & 0xFF; tb = t & 0xFF; }
            else { tr = t & 0xFF; tg = (t >> 8) & 0xFF; tb = (t >> 16) & 0xFF; }
            int r = (sr * a + tr * (255 - a) + 127) / 255, g = (sg * a + tg * (255 - a) + 127) / 255, b = (sb * a + tb * (255 - a) + 127) / 255;
            *p = d->fmt ? 0xFF000000u | (DWORD)r << 16 | (DWORD)g << 8 | (DWORD)b : (DWORD)r | (DWORD)g << 8 | (DWORD)b << 16;
        }
    }
}

USERAPI BOOL DrawIconEx(HDC dc, int x, int y, HICON h, int cx, int cy, UINT step, HBRUSH br, UINT flags)
{
    (void)step;
    Icon *ic = icon_of(h);
    if (!ic) return FALSE;
    if (!cx && !(flags & DI_DEFAULTSIZE)) cx = ic->w;
    if (!cy && !(flags & DI_DEFAULTSIZE)) cy = ic->h;
    if (flags & DI_DEFAULTSIZE) { if (!cx) cx = GetSystemMetrics(ic->cursor ? SM_CXCURSOR : SM_CXICON); if (!cy) cy = GetSystemMetrics(ic->cursor ? SM_CYCURSOR : SM_CYICON); }
    if (br) { RECT r = { x, y, x + cx, y + cy }; FillRect(dc, &r, br); }
    draw_icon(dc, x, y, h, cx, cy);
    return TRUE;
}

USERAPI BOOL DrawIcon(HDC dc, int x, int y, HICON h) { return DrawIconEx(dc, x, y, h, 0, 0, 0, 0, DI_NORMAL | DI_DEFAULTSIZE); }

/* WM_CTLCOLOR* from a control's parent (its default if the parent has none) */
HBRUSH ctl_color(Wnd *w, UINT msg, HDC dc)
{
    Wnd *p = w->parent ? w->parent : w->owner;
    HBRUSH br = 0;
    if (p) br = (HBRUSH)send_msg(p, msg, (WPARAM)dc, (LPARAM)w->h);
    if (!br) br = (HBRUSH)DefWindowProcW(w->h, msg, (WPARAM)dc, (LPARAM)w->h);
    return br;
}
