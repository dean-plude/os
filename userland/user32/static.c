/*
 * static.c — the STATIC class: labels, icons, bitmaps, frames and lines
 */
#include "u32.h"

typedef struct { HANDLE image; int image_type; int owns; } Stc;

static void set_image_from_text(Wnd *w, Stc *s)
{
    DWORD t = w->style & SS_TYPEMASK;
    const WCHAR *name = w->text;
    if (!name || !*name) return;
    LPCWSTR res = name;
    if (name[0] == '#') {                                   /* "#123": a resource number */
        int v = 0;
        for (const WCHAR *p = name + 1; *p >= '0' && *p <= '9'; p++) v = v * 10 + (*p - '0');
        res = MAKEINTRESOURCEW(v);
    }
    if (t == SS_ICON) {
        HICON ic = LoadImageW(w->inst, res, IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
        if (!ic) ic = LoadIconW(NULL, res);
        s->image = ic; s->image_type = IMAGE_ICON;
    } else if (t == SS_BITMAP) {
        s->image = LoadImageW(w->inst, res, IMAGE_BITMAP, 0, 0, 0);
        s->image_type = IMAGE_BITMAP;
    }
}

static void fit_to_image(Wnd *w, Stc *s)
{
    if (!s->image || (w->style & (SS_CENTERIMAGE | SS_REALSIZECONTROL))) return;
    int cx = 0, cy = 0;
    if (s->image_type == IMAGE_ICON || s->image_type == IMAGE_CURSOR) { if (!icon_size((HICON)s->image, &cx, &cy)) return; if (cx > 32 && !(w->style & SS_REALSIZEIMAGE)) cx = cy = 32; }
    else { BITMAP bm; if (!GetObjectW(s->image, sizeof(bm), &bm)) return; cx = bm.bmWidth; cy = bm.bmHeight; }
    wnd_set_pos(w, 0, 0, 0, cx, cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void paint(Wnd *w, HDC dc)
{
    Stc *s = w->ctl;
    RECT r = { 0, 0, w->client.right - w->client.left, w->client.bottom - w->client.top };
    DWORD st = w->style, t = st & SS_TYPEMASK;
    int disabled = (st & WS_DISABLED) != 0;
    switch (t) {
    case SS_BLACKRECT: fill_rect(dc, &r, sys_color(COLOR_3DDKSHADOW)); return;
    case SS_GRAYRECT:  fill_rect(dc, &r, sys_color(COLOR_3DSHADOW)); return;
    case SS_WHITERECT: fill_rect(dc, &r, sys_color(COLOR_3DHILIGHT)); return;
    case SS_BLACKFRAME: frame_rect(dc, &r, sys_color(COLOR_3DDKSHADOW)); return;
    case SS_GRAYFRAME:  frame_rect(dc, &r, sys_color(COLOR_3DSHADOW)); return;
    case SS_WHITEFRAME: frame_rect(dc, &r, sys_color(COLOR_3DHILIGHT)); return;
    case SS_ETCHEDHORZ: { RECT l = { 0, 0, r.right, 1 }; fill_rect(dc, &l, 0xDCDCDC); return; }
    case SS_ETCHEDVERT: { RECT l = { 0, 0, 1, r.bottom }; fill_rect(dc, &l, 0xDCDCDC); return; }
    case SS_ETCHEDFRAME: frame_rect(dc, &r, 0xDCDCDC); return;
    case SS_OWNERDRAW: {
        DRAWITEMSTRUCT di;
        memset(&di, 0, sizeof(di));
        di.CtlType = ODT_STATIC; di.CtlID = (UINT)w->id; di.itemAction = ODA_DRAWENTIRE;
        di.itemState = disabled ? ODS_DISABLED : 0;
        di.hwndItem = w->h; di.hDC = dc; di.rcItem = r;
        if (w->parent) send_msg(w->parent, WM_DRAWITEM, (WPARAM)w->id, (LPARAM)&di);
        return;
    }
    }
    HBRUSH bg = ctl_color(w, WM_CTLCOLORSTATIC, dc);
    if (t == SS_ICON || t == SS_BITMAP || t == SS_ENHMETAFILE) {
        if (bg && !(w->exstyle & WS_EX_TRANSPARENT)) FillRect(dc, &r, bg);
        if (!s || !s->image) return;
        int cx = 0, cy = 0;
        if (s->image_type == IMAGE_ICON || s->image_type == IMAGE_CURSOR) {
            icon_size((HICON)s->image, &cx, &cy);
            if (!(st & SS_REALSIZEIMAGE) && cx > r.right && !(st & SS_CENTERIMAGE)) { cx = r.right; cy = r.bottom; }
            int x = 0, y = 0;
            if (st & SS_CENTERIMAGE) { x = (r.right - cx) / 2; y = (r.bottom - cy) / 2; }
            else if (st & SS_RIGHTJUST) x = r.right - cx;
            if (!(st & SS_CENTERIMAGE) && !(st & SS_REALSIZEIMAGE)) { cx = MIN(cx, r.right); cy = MIN(cy, r.bottom); }
            draw_icon(dc, x, y, (HICON)s->image, cx, cy);
        } else if (s->image_type == IMAGE_BITMAP) {
            BITMAP bm;
            if (!GetObjectW(s->image, sizeof(bm), &bm)) return;
            int x = 0, y = 0;
            if (st & SS_CENTERIMAGE) { x = (r.right - bm.bmWidth) / 2; y = (r.bottom - bm.bmHeight) / 2; }
            else if (st & SS_RIGHTJUST) x = r.right - bm.bmWidth;
            HDC m = CreateCompatibleDC(dc);
            HGDIOBJ o = SelectObject(m, s->image);
            if (bm.bmBitsPixel == 32) {
                BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
                AlphaBlend(dc, x, y, bm.bmWidth, bm.bmHeight, m, 0, 0, bm.bmWidth, bm.bmHeight, bf);
            } else BitBlt(dc, x, y, bm.bmWidth, bm.bmHeight, m, 0, 0, SRCCOPY);
            SelectObject(m, o);
            DeleteDC(m);
        }
        return;
    }
    /* text */
    if (bg && !(w->exstyle & WS_EX_TRANSPARENT)) FillRect(dc, &r, bg);
    if (st & SS_SUNKEN) { DrawEdge(dc, &r, BDR_SUNKENOUTER, BF_RECT | BF_ADJUST); }
    HGDIOBJ of = SelectObject(dc, ctl_font(w));
    SetBkMode(dc, TRANSPARENT);
    if (disabled) SetTextColor(dc, sys_color(COLOR_GRAYTEXT));
    UINT fmt = DT_EXPANDTABS | DT_HIDEPREFIX;
    if (st & SS_NOPREFIX) fmt |= DT_NOPREFIX;
    switch (t) {
    case SS_CENTER: fmt |= DT_CENTER | DT_WORDBREAK; break;
    case SS_RIGHT: fmt |= DT_RIGHT | DT_WORDBREAK; break;
    case SS_SIMPLE: fmt |= DT_SINGLELINE | DT_NOCLIP; break;
    case SS_LEFTNOWORDWRAP: break;                          /* line breaks still start lines; no wrapping */
    default: fmt |= DT_WORDBREAK; break;
    }
    if (st & SS_CENTERIMAGE) { fmt |= DT_SINGLELINE | DT_VCENTER; fmt &= ~DT_WORDBREAK; }
    switch (st & SS_ELLIPSISMASK) {
    case SS_ENDELLIPSIS: fmt |= DT_END_ELLIPSIS | DT_SINGLELINE; fmt &= ~DT_WORDBREAK; break;
    case SS_PATHELLIPSIS: fmt |= DT_PATH_ELLIPSIS | DT_SINGLELINE; fmt &= ~DT_WORDBREAK; break;
    case SS_WORDELLIPSIS: fmt |= DT_WORD_ELLIPSIS | DT_SINGLELINE; fmt &= ~DT_WORDBREAK; break;
    }
    if (st & SS_EDITCONTROL) fmt |= DT_EDITCONTROL;
    DrawTextW(dc, w->text ? w->text : L"", -1, &r, fmt);
    SelectObject(dc, of);
}

LRESULT CALLBACK StaticProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    Wnd *w = W_quiet(h);
    if (!w) return 0;
    Stc *s = w->ctl;
    DWORD t = w->style & SS_TYPEMASK;
    switch (msg) {
    case WM_NCCREATE:
        w->ctl = calloc(1, sizeof(Stc));
        if ((w->style & SS_TYPEMASK) == SS_ETCHEDHORZ || (w->style & SS_TYPEMASK) == SS_ETCHEDVERT || (w->style & SS_TYPEMASK) == SS_ETCHEDFRAME)
            w->exstyle &= ~WS_EX_CLIENTEDGE;
        return DefWindowProcW(h, msg, wp, lp);
    case WM_CREATE:
        if (s && (t == SS_ICON || t == SS_BITMAP)) { set_image_from_text(w, s); fit_to_image(w, s); }
        return 0;
    case WM_NCDESTROY:
        free(w->ctl);
        w->ctl = NULL;
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (dc) paint(w, dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_PRINTCLIENT: paint(w, (HDC)wp); return 0;
    case WM_NCHITTEST:
        if (!(w->style & SS_NOTIFY)) return HTTRANSPARENT;
        return DefWindowProcW(h, msg, wp, lp);
    case WM_LBUTTONDOWN: case WM_NCLBUTTONDOWN:
        if (w->style & SS_NOTIFY) notify_parent(w, STN_CLICKED);
        return 0;
    case WM_LBUTTONDBLCLK: case WM_NCLBUTTONDBLCLK:
        if (w->style & SS_NOTIFY) notify_parent(w, STN_DBLCLK);
        return 0;
    case WM_ENABLE:
        invalidate(w, NULL, TRUE, 0);
        if (w->style & SS_NOTIFY) notify_parent(w, wp ? STN_ENABLE : STN_DISABLE);
        return 0;
    case WM_GETDLGCODE: return DLGC_STATIC;
    case WM_SETTEXT: {
        LRESULT r = DefWindowProcW(h, msg, wp, lp);
        if (s && (t == SS_ICON || t == SS_BITMAP)) { set_image_from_text(w, s); fit_to_image(w, s); }
        invalidate(w, NULL, TRUE, 0);
        return r;
    }
    case WM_SETFONT: w->font = (HFONT)wp; if (lp) invalidate(w, NULL, TRUE, 0); return 0;
    case WM_GETFONT: return (LRESULT)w->font;
    case STM_GETICON: return (LRESULT)(s ? s->image : 0);
    case STM_SETICON: {
        if (!s) return 0;
        HANDLE old = s->image;
        s->image = (HANDLE)wp; s->image_type = IMAGE_ICON;
        fit_to_image(w, s);
        invalidate(w, NULL, TRUE, 0);
        return (LRESULT)old;
    }
    case STM_GETIMAGE: return (LRESULT)(s ? s->image : 0);
    case STM_SETIMAGE: {
        if (!s) return 0;
        HANDLE old = s->image;
        s->image = (HANDLE)lp; s->image_type = (int)wp;
        fit_to_image(w, s);
        invalidate(w, NULL, TRUE, 0);
        if (w->parent) invalidate(w->parent, &w->rect, TRUE, 0);
        return (LRESULT)old;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}
