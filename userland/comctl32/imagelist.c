/*
 * imagelist.c — image lists: equal-sized images kept as 32-bit ARGB, drawn
 * with their transparency (from an alpha channel, a mask or a color key)
 */
#include "cc.h"

#define IL_MAGIC 0x494D4C32u

typedef struct _IMAGELIST {
    DWORD magic;
    int cx, cy, count, cap, grow;
    UINT flags;
    DWORD *px;                      /* count images of cx * cy ARGB (not premultiplied) */
    COLORREF bk;
    int overlay[16];
} IL;

static IL *il(HIMAGELIST h)
{
    IL *l = (IL *)h;
    return l && l->magic == IL_MAGIC ? l : NULL;
}

static int reserve(IL *l, int n)
{
    if (n <= l->cap) return 1;
    int nc = MAX(n, l->cap + MAX(l->grow, 4));
    DWORD *p = realloc(l->px, (size_t)nc * l->cx * l->cy * 4);
    if (!p) return 0;
    l->px = p;
    l->cap = nc;
    return 1;
}

static DWORD *img(IL *l, int i) { return l->px + (size_t)i * l->cx * l->cy; }

CC HIMAGELIST WINAPI ImageList_Create(int cx, int cy, UINT flags, int initial, int grow)
{
    if (cx <= 0 || cy <= 0) return NULL;
    IL *l = calloc(1, sizeof(IL));
    if (!l) return NULL;
    l->magic = IL_MAGIC;
    l->cx = cx; l->cy = cy; l->flags = flags; l->grow = grow > 0 ? grow : 4;
    l->bk = CLR_NONE;
    for (int i = 0; i < 16; i++) l->overlay[i] = -1;
    reserve(l, initial > 0 ? initial : 4);
    return (HIMAGELIST)l;
}

CC BOOL WINAPI ImageList_Destroy(HIMAGELIST h)
{
    IL *l = il(h);
    if (!l) return FALSE;
    l->magic = 0;
    free(l->px);
    free(l);
    return TRUE;
}

CC int WINAPI ImageList_GetImageCount(HIMAGELIST h) { IL *l = il(h); return l ? l->count : 0; }

CC BOOL WINAPI ImageList_SetImageCount(HIMAGELIST h, UINT n)
{
    IL *l = il(h);
    if (!l || !reserve(l, (int)n)) return FALSE;
    for (int i = l->count; i < (int)n; i++) memset(img(l, i), 0, (size_t)l->cx * l->cy * 4);
    l->count = (int)n;
    return TRUE;
}

CC BOOL WINAPI ImageList_GetIconSize(HIMAGELIST h, int *cx, int *cy)
{
    IL *l = il(h);
    if (!l) return FALSE;
    if (cx) *cx = l->cx;
    if (cy) *cy = l->cy;
    return TRUE;
}

CC BOOL WINAPI ImageList_SetIconSize(HIMAGELIST h, int cx, int cy)
{
    IL *l = il(h);
    if (!l || cx <= 0 || cy <= 0) return FALSE;
    free(l->px);
    l->px = NULL; l->cap = 0; l->count = 0;
    l->cx = cx; l->cy = cy;
    reserve(l, 4);
    return TRUE;
}

CC COLORREF WINAPI ImageList_SetBkColor(HIMAGELIST h, COLORREF c) { IL *l = il(h); if (!l) return CLR_NONE; COLORREF o = l->bk; l->bk = c; return o; }
CC COLORREF WINAPI ImageList_GetBkColor(HIMAGELIST h) { IL *l = il(h); return l ? l->bk : CLR_NONE; }
CC BOOL WINAPI ImageList_SetOverlayImage(HIMAGELIST h, int i, int ov) { IL *l = il(h); if (!l || ov < 1 || ov > 15) return FALSE; l->overlay[ov] = i; return TRUE; }

/* A bitmap's pixels as 32-bit top-down BGRA; *w, *h its size */
static DWORD *bitmap_pixels(HBITMAP bm, int *w, int *h, int *has_alpha)
{
    BITMAP b;
    if (!bm || !GetObjectW(bm, sizeof(b), &b)) return NULL;
    *w = b.bmWidth; *h = b.bmHeight;
    DWORD *p = malloc((size_t)*w * *h * 4);
    if (!p) return NULL;
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = *w; bi.bmiHeader.biHeight = -*h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    HDC dc = GetDC(NULL);
    GetDIBits(dc, bm, 0, (UINT)*h, p, &bi, DIB_RGB_COLORS);
    ReleaseDC(NULL, dc);
    *has_alpha = 0;
    if (b.bmBitsPixel == 32) for (int i = 0; i < *w * *h; i++) if (p[i] >> 24) { *has_alpha = 1; break; }
    return p;
}

/* Add the images in a strip; transparent where @mask says (mask bitmap / color key) */
static int add_strip(IL *l, HBITMAP bm, HBITMAP mask, COLORREF key, int use_key)
{
    int w, h, alpha;
    DWORD *p = bitmap_pixels(bm, &w, &h, &alpha);
    if (!p) return -1;
    DWORD *m = NULL;
    int mw = 0, mh = 0, ma;
    if (mask) m = bitmap_pixels(mask, &mw, &mh, &ma);
    int n = w / l->cx;
    if (n < 1) n = 1;
    if (!reserve(l, l->count + n)) { free(p); free(m); return -1; }
    int first = l->count;
    DWORD keyc = (key & 0xFF) << 16 | (key & 0xFF00) | ((key >> 16) & 0xFF);
    /* premultiplied (DIB section) alpha: not knowable; treat the colors as straight */
    for (int k = 0; k < n; k++) {
        DWORD *d = img(l, first + k);
        for (int y = 0; y < l->cy; y++)
            for (int x = 0; x < l->cx; x++) {
                int sx = k * l->cx + x;
                DWORD c = sx < w && y < h ? p[(size_t)y * w + sx] : 0;
                if (alpha) {
                    int a = (int)(c >> 24);
                    if (a && a < 255) {                     /* un-premultiply */
                        int r = MIN(255, (int)((c >> 16) & 0xFF) * 255 / a), g = MIN(255, (int)((c >> 8) & 0xFF) * 255 / a), b = MIN(255, (int)(c & 0xFF) * 255 / a);
                        c = (DWORD)a << 24 | (DWORD)r << 16 | (DWORD)g << 8 | (DWORD)b;
                    }
                } else {
                    int transparent = 0;
                    if (m && sx < mw && y < mh) transparent = (m[(size_t)y * mw + sx] & 0xFFFFFF) != 0;
                    else if (use_key) transparent = (c & 0xFFFFFF) == keyc;
                    c = transparent ? 0 : (c | 0xFF000000u);
                }
                d[(size_t)y * l->cx + x] = c;
            }
    }
    l->count += n;
    free(p); free(m);
    return first;
}

CC int WINAPI ImageList_Add(HIMAGELIST h, HBITMAP bm, HBITMAP mask)
{
    IL *l = il(h);
    return l ? add_strip(l, bm, (l->flags & ILC_MASK) ? mask : NULL, 0, 0) : -1;
}

CC int WINAPI ImageList_AddMasked(HIMAGELIST h, HBITMAP bm, COLORREF key)
{
    IL *l = il(h);
    return l ? add_strip(l, bm, NULL, key, 1) : -1;
}

CC BOOL WINAPI ImageList_Replace(HIMAGELIST h, int i, HBITMAP bm, HBITMAP mask)
{
    IL *l = il(h);
    if (!l || i < 0 || i >= l->count) return FALSE;
    int n = l->count;
    int at = add_strip(l, bm, mask, 0, 0);
    if (at < 0) return FALSE;
    memcpy(img(l, i), img(l, at), (size_t)l->cx * l->cy * 4);
    l->count = n;
    return TRUE;
}

/* An icon's pixels (ARGB) scaled to the list's size */
static int icon_into(IL *l, DWORD *d, HICON icon)
{
    ICONINFO ii;
    if (!GetIconInfo(icon, &ii)) return 0;
    int w, h, alpha;
    DWORD *p = bitmap_pixels(ii.hbmColor, &w, &h, &alpha);
    int mw = 0, mh = 0, ma;
    DWORD *m = alpha ? NULL : bitmap_pixels(ii.hbmMask, &mw, &mh, &ma);
    if (p) {
        for (int y = 0; y < l->cy; y++)
            for (int x = 0; x < l->cx; x++) {
                int sx = x * w / l->cx, sy = y * h / l->cy;
                DWORD c = p[(size_t)sy * w + sx];
                if (!alpha) {
                    int t = m && sx < mw && sy < mh ? (m[(size_t)sy * mw + sx] & 0xFFFFFF) != 0 : 0;
                    c = t ? 0 : c | 0xFF000000u;
                }
                d[(size_t)y * l->cx + x] = c;
            }
    }
    free(p); free(m);
    DeleteObject(ii.hbmColor);
    DeleteObject(ii.hbmMask);
    return p != NULL;
}

CC int WINAPI ImageList_ReplaceIcon(HIMAGELIST h, int i, HICON icon)
{
    IL *l = il(h);
    if (!l || !icon) return -1;
    if (i < 0 || i >= l->count) {
        if (!reserve(l, l->count + 1)) return -1;
        i = l->count;
        memset(img(l, i), 0, (size_t)l->cx * l->cy * 4);
        l->count++;
    }
    icon_into(l, img(l, i), icon);
    return i;
}

#undef ImageList_AddIcon
CC int WINAPI ImageList_AddIcon(HIMAGELIST h, HICON icon) { return ImageList_ReplaceIcon(h, -1, icon); }

CC BOOL WINAPI ImageList_Remove(HIMAGELIST h, int i)
{
    IL *l = il(h);
    if (!l) return FALSE;
    if (i < 0) { l->count = 0; return TRUE; }
    if (i >= l->count) return FALSE;
    size_t sz = (size_t)l->cx * l->cy;
    memmove(img(l, i), img(l, i + 1), sz * 4 * (size_t)(l->count - i - 1));
    l->count--;
    return TRUE;
}

CC BOOL WINAPI ImageList_Copy(HIMAGELIST hd, int di, HIMAGELIST hs, int si, UINT flags)
{
    IL *d = il(hd), *s = il(hs);
    if (!d || !s || si < 0 || si >= s->count || di < 0 || di >= d->count || d->cx != s->cx || d->cy != s->cy) return FALSE;
    size_t sz = (size_t)d->cx * d->cy * 4;
    if (flags & 1 /* ILCF_SWAP */) {
        DWORD *t = malloc(sz);
        if (!t) return FALSE;
        memcpy(t, img(d, di), sz); memcpy(img(d, di), img(s, si), sz); memcpy(img(s, si), t, sz);
        free(t);
    } else memcpy(img(d, di), img(s, si), sz);
    return TRUE;
}

CC HIMAGELIST WINAPI ImageList_Duplicate(HIMAGELIST h)
{
    IL *l = il(h);
    if (!l) return NULL;
    IL *n = (IL *)ImageList_Create(l->cx, l->cy, l->flags, l->count, l->grow);
    if (!n) return NULL;
    reserve(n, l->count);
    memcpy(n->px, l->px, (size_t)l->count * l->cx * l->cy * 4);
    n->count = l->count;
    n->bk = l->bk;
    memcpy(n->overlay, l->overlay, sizeof(n->overlay));
    return (HIMAGELIST)n;
}

/* Draw image @i at (x, y), scaled to cx x cy (0: its size) */
static BOOL draw(IL *l, int i, HDC dc, int x, int y, int cx, int cy, COLORREF bk, UINT style)
{
    if (i < 0 || i >= l->count) return FALSE;
    if (!cx) cx = l->cx;
    if (!cy) cy = l->cy;
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = l->cx; bi.bmiHeader.biHeight = -l->cy; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void *bits;
    HBITMAP bm = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bm) return FALSE;
    DWORD *d = bits;
    const DWORD *s = img(l, i);
    int blend = (style & (ILD_BLEND25 | ILD_BLEND50)) != 0;
    COLORREF hi = GetSysColor(COLOR_HIGHLIGHT);
    for (int k = 0; k < l->cx * l->cy; k++) {
        DWORD c = s[k];
        int a = (int)(c >> 24);
        int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
        if (blend && a) {
            int f = (style & ILD_BLEND50) ? 128 : 64;
            r = (r * (256 - f) + GetRValue(hi) * f) >> 8; g = (g * (256 - f) + GetGValue(hi) * f) >> 8; b = (b * (256 - f) + GetBValue(hi) * f) >> 8;
        }
        if (bk != CLR_NONE && bk != CLR_DEFAULT && !(style & ILD_TRANSPARENT)) {
            int br = GetRValue(bk), bg = GetGValue(bk), bb = GetBValue(bk);
            r = (r * a + br * (255 - a)) / 255; g = (g * a + bg * (255 - a)) / 255; b = (b * a + bb * (255 - a)) / 255;
            a = 255;
        }
        /* premultiplied for AlphaBlend */
        d[k] = (DWORD)a << 24 | (DWORD)(r * a / 255) << 16 | (DWORD)(g * a / 255) << 8 | (DWORD)(b * a / 255);
    }
    HDC mem = CreateCompatibleDC(dc);
    HGDIOBJ o = SelectObject(mem, bm);
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    AlphaBlend(dc, x, y, cx, cy, mem, 0, 0, l->cx, l->cy, bf);
    SelectObject(mem, o);
    DeleteDC(mem);
    DeleteObject(bm);
    int ov = (int)((style & ILD_OVERLAYMASK) >> 8);
    if (ov > 0 && ov < 16 && l->overlay[ov] >= 0 && l->overlay[ov] != i) draw(l, l->overlay[ov], dc, x, y, cx, cy, CLR_NONE, ILD_TRANSPARENT);
    return TRUE;
}

int il_draw(HIMAGELIST h, int i, HDC dc, int x, int y, UINT style)
{
    IL *l = il(h);
    return l ? draw(l, i, dc, x, y, 0, 0, l->bk, style) : 0;
}

CC BOOL WINAPI ImageList_Draw(HIMAGELIST h, int i, HDC dc, int x, int y, UINT style)
{
    IL *l = il(h);
    return l ? draw(l, i, dc, x, y, 0, 0, l->bk, style) : FALSE;
}

CC BOOL WINAPI ImageList_DrawEx(HIMAGELIST h, int i, HDC dc, int x, int y, int cx, int cy, COLORREF bk, COLORREF fg, UINT style)
{
    (void)fg;
    IL *l = il(h);
    if (!l) return FALSE;
    if (bk == CLR_DEFAULT) bk = l->bk;
    return draw(l, i, dc, x, y, cx, cy, bk, style);
}

CC BOOL WINAPI ImageList_DrawIndirect(IMAGELISTDRAWPARAMS *p)
{
    if (!p) return FALSE;
    return ImageList_DrawEx(p->himl, p->i, p->hdcDst, p->x, p->y, p->cx, p->cy, p->rgbBk, p->rgbFg, p->fStyle);
}

CC HICON WINAPI ImageList_GetIcon(HIMAGELIST h, int i, UINT flags)
{
    (void)flags;
    IL *l = il(h);
    if (!l || i < 0 || i >= l->count) return NULL;
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = l->cx; bi.bmiHeader.biHeight = -l->cy; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void *bits, *mbits;
    HBITMAP color = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HBITMAP mask = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &mbits, NULL, 0);
    if (!color || !mask) { DeleteObject(color); DeleteObject(mask); return NULL; }
    memcpy(bits, img(l, i), (size_t)l->cx * l->cy * 4);
    for (int k = 0; k < l->cx * l->cy; k++) ((DWORD *)mbits)[k] = (img(l, i)[k] >> 24) ? 0 : 0xFFFFFF;
    ICONINFO ii = { TRUE, 0, 0, mask, color };
    HICON r = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return r;
}

CC BOOL WINAPI ImageList_GetImageInfo(HIMAGELIST h, int i, IMAGEINFO *info)
{
    IL *l = il(h);
    if (!l || !info || i < 0 || i >= l->count) return FALSE;
    memset(info, 0, sizeof(*info));
    SetRect(&info->rcImage, i * l->cx, 0, (i + 1) * l->cx, l->cy);
    return TRUE;
}

CC HIMAGELIST WINAPI ImageList_LoadImageW(HINSTANCE inst, LPCWSTR name, int cx, int grow, COLORREF key, UINT type, UINT flags)
{
    if (type == IMAGE_ICON) {
        HICON ic = LoadImageW(inst, name, IMAGE_ICON, cx, cx, flags);
        if (!ic) return NULL;
        HIMAGELIST l = ImageList_Create(cx, cx, ILC_COLOR32 | ILC_MASK, 1, grow);
        ImageList_ReplaceIcon(l, -1, ic);
        DestroyIcon(ic);
        return l;
    }
    HBITMAP bm = LoadImageW(inst, name, IMAGE_BITMAP, 0, 0, flags);
    if (!bm) return NULL;
    BITMAP b;
    GetObjectW(bm, sizeof(b), &b);
    HIMAGELIST l = ImageList_Create(cx, b.bmHeight, ILC_COLOR32 | ILC_MASK, b.bmWidth / MAX(cx, 1), grow);
    if (key == CLR_NONE) ImageList_Add(l, bm, NULL);
    else ImageList_AddMasked(l, bm, key == CLR_DEFAULT ? 0xFF00FF : key);
    DeleteObject(bm);
    return l;
}

CC HIMAGELIST WINAPI ImageList_LoadImageA(HINSTANCE inst, LPCSTR name, int cx, int grow, COLORREF key, UINT type, UINT flags)
{
    if ((ULONG_PTR)name < 0x10000) return ImageList_LoadImageW(inst, (LPCWSTR)name, cx, grow, key, type, flags);
    WCHAR w[260];
    MultiByteToWideChar(CP_ACP, 0, name, -1, w, 260);
    return ImageList_LoadImageW(inst, w, cx, grow, key, type, flags);
}

CC HIMAGELIST WINAPI ImageList_Merge(HIMAGELIST a, int ia, HIMAGELIST b, int ib, int dx, int dy)
{
    (void)b; (void)ib; (void)dx; (void)dy;
    IL *l = il(a);
    if (!l) return NULL;
    HIMAGELIST n = ImageList_Create(l->cx, l->cy, l->flags, 1, 1);
    IL *nl = il(n);
    if (nl && ia >= 0 && ia < l->count) { reserve(nl, 1); memcpy(nl->px, img(l, ia), (size_t)l->cx * l->cy * 4); nl->count = 1; }
    return n;
}

/* Dragging images: not shown */
CC BOOL WINAPI ImageList_BeginDrag(HIMAGELIST h, int i, int dx, int dy) { (void)h; (void)i; (void)dx; (void)dy; return TRUE; }
CC void WINAPI ImageList_EndDrag(void) { }
CC BOOL WINAPI ImageList_DragEnter(HWND h, int x, int y) { (void)h; (void)x; (void)y; return TRUE; }
CC BOOL WINAPI ImageList_DragLeave(HWND h) { (void)h; return TRUE; }
CC BOOL WINAPI ImageList_DragMove(int x, int y) { (void)x; (void)y; return TRUE; }
CC BOOL WINAPI ImageList_DragShowNolock(BOOL show) { (void)show; return TRUE; }
CC BOOL WINAPI ImageList_SetDragCursorImage(HIMAGELIST h, int i, int dx, int dy) { (void)h; (void)i; (void)dx; (void)dy; return TRUE; }
CC HIMAGELIST WINAPI ImageList_GetDragImage(POINT *p, POINT *hot) { (void)p; (void)hot; return NULL; }
CC HIMAGELIST WINAPI ImageList_Read(void *stream) { (void)stream; return NULL; }
CC BOOL WINAPI ImageList_Write(HIMAGELIST h, void *stream) { (void)h; (void)stream; return FALSE; }
CC HRESULT WINAPI ImageList_CoCreateInstance(const void *clsid, void *outer, const void *iid, void **out) { (void)clsid; (void)outer; (void)iid; if (out) *out = NULL; return 0x80004001L; }
CC HRESULT WINAPI HIMAGELIST_QueryInterface(HIMAGELIST h, const void *iid, void **out) { (void)h; (void)iid; if (out) *out = NULL; return 0x80004002L; }
