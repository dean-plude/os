/*
 * res.c — resources: strings, icons (DIB and PNG images from icon groups,
 * .ico files), cursors, bitmaps; the system's own icons
 */
#include "u32.h"

static void *s_malloc(size_t n) { return malloc(n); }
static void *s_realloc(void *p, size_t n) { return realloc(p, n); }
static void  s_free(void *p) { free(p); }
#define STBI_MALLOC(n) s_malloc(n)
#define STBI_REALLOC(p, n) s_realloc(p, n)
#define STBI_FREE(p) s_free(p)
#define STBI_ASSERT(x) ((void)0)
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#include "../../third_party/stb/stb_image.h"
#pragma clang diagnostic pop

int _fltused = 1;                   /* floats are used (the system icons) */

const void *find_res(HINSTANCE inst, LPCWSTR name, LPCWSTR type, DWORD *size)
{
    if (!inst) inst = GetModuleHandleW(NULL);
    HRSRC r = FindResourceW(inst, name, type);
    if (!r) return NULL;
    HGLOBAL g = LoadResource(inst, r);
    if (!g) return NULL;
    if (size) *size = SizeofResource(inst, r);
    return LockResource(g);
}

/* -----------------------------------------------------------------------
 * Strings
 * ----------------------------------------------------------------------- */
USERAPI int LoadStringW(HINSTANCE inst, UINT id, LPWSTR buf, int n)
{
    DWORD size;
    const WCHAR *p = find_res(inst, (LPCWSTR)(ULONG_PTR)((id >> 4) + 1), RT_STRING, &size);
    if (!p) { if (buf && n) buf[0] = 0; return 0; }
    for (UINT i = 0; i < (id & 15); i++) p += 1 + *p;       /* length-prefixed entries */
    int len = *p;
    if (!buf) return 0;
    if (!n) { *(const WCHAR **)buf = p + 1; return len; }   /* a pointer to the resource itself */
    int k = len < n - 1 ? len : n - 1;
    memcpy(buf, p + 1, 2 * (size_t)k);
    buf[k] = 0;
    return k;
}

USERAPI int LoadStringA(HINSTANCE inst, UINT id, LPSTR buf, int n)
{
    WCHAR w[4096];
    int k = LoadStringW(inst, id, w, 4096);
    if (!buf || n <= 0) return 0;
    int m = WideCharToMultiByte(CP_ACP, 0, w, k, buf, n - 1, 0, 0);
    if (m < 0) m = 0;
    buf[m] = 0;
    return m;
}

/* -----------------------------------------------------------------------
 * Icons
 * ----------------------------------------------------------------------- */
Icon *icon_of(HICON h)
{
    Icon *ic = (Icon *)h;
    return ic && handle_live(ic) && ic->magic == ICON_MAGIC ? ic : NULL;
}

static Icon *new_icon(int w, int h)
{
    static LONG serial;
    Icon *ic = calloc(1, sizeof(Icon));
    if (!ic) return NULL;
    ic->magic = ICON_MAGIC;
    ic->serial = (DWORD)InterlockedIncrement(&serial);
    handle_add(ic);
    ic->w = w; ic->h = h;
    ic->argb = calloc((size_t)w * h, 4);
    if (!ic->argb) { free(ic); return NULL; }
    return ic;
}

static void free_icon(Icon *ic)
{
    if (ic && ic->ani) {
        Ani *a = ic->ani;
        ic->ani = NULL;
        for (int i = 1; i < a->nframes; i++) free_icon(a->frames[i]);
        free(a->frames); free(a->seq); free(a->rate); free(a);
    }
    while (ic) {
        Icon *n = ic->more;
        ic->magic = 0;
        handle_remove(ic);
        free(ic->argb);
        free(ic);
        ic = n;
    }
}

/* One image of an icon resource or file: a PNG, or a DIB (colors + AND mask) */
static Icon *decode_image(const BYTE *p, DWORD size, int cursor)
{
    if (cursor && size > 4) { p += 4; size -= 4; }          /* the cursor's hot spot */
    if (size >= 8 && p[0] == 0x89 && p[1] == 'P' && p[2] == 'N' && p[3] == 'G') {
        int w, h, n;
        unsigned char *px = stbi_load_from_memory(p, (int)size, &w, &h, &n, 4);
        if (!px) return NULL;
        Icon *ic = new_icon(w, h);
        if (ic) for (int i = 0; i < w * h; i++)
            ic->argb[i] = (DWORD)px[i * 4 + 3] << 24 | (DWORD)px[i * 4] << 16 | (DWORD)px[i * 4 + 1] << 8 | px[i * 4 + 2];
        stbi_image_free(px);
        return ic;
    }
    if (size < sizeof(BITMAPINFOHEADER)) return NULL;
    const BITMAPINFOHEADER *bi = (const BITMAPINFOHEADER *)p;
    int w = bi->biWidth, h = bi->biHeight / 2, bpp = bi->biBitCount;
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) return NULL;
    int ncolors = bpp <= 8 ? (bi->biClrUsed ? (int)bi->biClrUsed : 1 << bpp) : 0;
    const RGBQUAD *pal = (const RGBQUAD *)(p + bi->biSize);
    const BYTE *xor = p + bi->biSize + ncolors * 4;
    int xstride = ((w * bpp + 31) / 32) * 4, astride = ((w + 31) / 32) * 4;
    const BYTE *and = xor + (size_t)xstride * h;
    if ((size_t)(and - p) + (size_t)astride * h > size + 4) and = NULL;
    Icon *ic = new_icon(w, h);
    if (!ic) return NULL;
    int any_alpha = 0;
    for (int y = 0; y < h; y++) {
        const BYTE *row = xor + (size_t)(h - 1 - y) * xstride;
        for (int x = 0; x < w; x++) {
            DWORD c = 0;
            switch (bpp) {
            case 32: c = *(const DWORD *)(row + x * 4); if (c >> 24) any_alpha = 1; break;
            case 24: c = row[x * 3] | row[x * 3 + 1] << 8 | row[x * 3 + 2] << 16; break;
            case 8: { RGBQUAD q = pal[row[x]]; c = q.rgbBlue | q.rgbGreen << 8 | q.rgbRed << 16; break; }
            case 4: { RGBQUAD q = pal[(row[x / 2] >> (x & 1 ? 0 : 4)) & 15]; c = q.rgbBlue | q.rgbGreen << 8 | q.rgbRed << 16; break; }
            case 1: { RGBQUAD q = pal[(row[x / 8] >> (7 - (x & 7))) & 1]; c = q.rgbBlue | q.rgbGreen << 8 | q.rgbRed << 16; break; }
            }
            ic->argb[(size_t)y * w + x] = c;
        }
    }
    if (!any_alpha) {
        for (int y = 0; y < h; y++) {
            const BYTE *row = and ? and + (size_t)(h - 1 - y) * astride : NULL;
            for (int x = 0; x < w; x++) {
                int transparent = row ? (row[x / 8] >> (7 - (x & 7))) & 1 : 0;
                DWORD *d = &ic->argb[(size_t)y * w + x];
                *d = transparent ? 0 : (*d & 0xFFFFFF) | 0xFF000000u;
            }
        }
    }
    ic->cursor = cursor;
    return ic;
}

/* The best image of an icon group for cx x cy */
static int pick_entry(const BYTE *dir, int cursor, int cx, int cy)
{
    int n = *(const WORD *)(dir + 4);
    int best = -1, bestd = 1 << 30, bestbpp = 0;
    for (int i = 0; i < n; i++) {
        const BYTE *e = dir + 6 + i * 14;
        int w = cursor ? *(const WORD *)e : e[0], h = cursor ? *(const WORD *)(e + 2) / 2 : e[1];
        if (!cursor) { if (!w) w = 256; if (!h) h = 256; }
        int bpp = cursor ? *(const WORD *)(e + 6) : *(const WORD *)(e + 6);
        int d = abs(w - cx) + abs(h - cy);
        if (w < cx) d += 1000;                              /* rather scale down than up */
        if (d < bestd || (d == bestd && bpp > bestbpp)) { best = i; bestd = d; bestbpp = bpp; }
    }
    return best;
}

USERAPI int LookupIconIdFromDirectoryEx(PBYTE dir, BOOL icon, int cx, int cy, UINT flags)
{
    (void)flags;
    if (!dir) return 0;
    if (!cx) cx = GetSystemMetrics(icon ? SM_CXICON : SM_CXCURSOR);
    if (!cy) cy = GetSystemMetrics(icon ? SM_CYICON : SM_CYCURSOR);
    int i = pick_entry(dir, !icon, cx, cy);
    return i < 0 ? 0 : *(const WORD *)(dir + 6 + i * 14 + 12);
}

USERAPI int LookupIconIdFromDirectory(PBYTE dir, BOOL icon) { return LookupIconIdFromDirectoryEx(dir, icon, 0, 0, 0); }

static Icon *ani_from_mem(const BYTE *p, DWORD size, int cx, int cy, int cursor);

USERAPI HICON CreateIconFromResourceEx(PBYTE bits, DWORD size, BOOL icon, DWORD ver, int cx, int cy, UINT flags)
{
    (void)ver; (void)flags;
    if (bits && size >= 12 && !memcmp(bits, "RIFF", 4))      /* an animated cursor (.ani) */
        return (HICON)ani_from_mem(bits, size, cx ? cx : 32, cy ? cy : 32, !icon);
    Icon *ic = decode_image(bits, size, !icon);
    if (ic && !icon && size >= 4) { ic->hot.x = *(const WORD *)bits; ic->hot.y = *(const WORD *)(bits + 2); }
    return (HICON)ic;
}

USERAPI HICON CreateIconFromResource(PBYTE bits, DWORD size, BOOL icon, DWORD ver) { return CreateIconFromResourceEx(bits, size, icon, ver, 0, 0, 0); }

typedef struct Shared { struct Shared *next; HINSTANCE inst; ULONG_PTR name; WCHAR *sname; int type, cx, cy; HANDLE h; } Shared;
static Shared *g_shared;

static HANDLE shared_find(HINSTANCE inst, LPCWSTR name, int type, int cx, int cy)
{
    for (Shared *s = g_shared; s; s = s->next) {
        if (s->inst != inst || s->type != type || s->cx != cx || s->cy != cy) continue;
        if ((ULONG_PTR)name < 0x10000 ? s->name == (ULONG_PTR)name : (s->sname && !wcsicmp_(s->sname, name))) return s->h;
    }
    return 0;
}

static void shared_add(HINSTANCE inst, LPCWSTR name, int type, int cx, int cy, HANDLE h)
{
    Shared *s = calloc(1, sizeof(Shared));
    if (!s) return;
    s->inst = inst; s->type = type; s->cx = cx; s->cy = cy; s->h = h;
    if ((ULONG_PTR)name < 0x10000) s->name = (ULONG_PTR)name; else s->sname = wstrdup(name);
    s->next = g_shared;
    g_shared = s;
    Icon *ic = icon_of((HICON)h);
    if (ic) ic->shared = 1;
}

HICON load_icon_res(HINSTANCE inst, LPCWSTR name, int cx, int cy, int cursor)
{
    DWORD size;
    const BYTE *dir = find_res(inst, name, cursor ? RT_GROUP_CURSOR : RT_GROUP_ICON, &size);
    if (!dir) {                                             /* an animated one? (ANICURSOR, ANIICON) */
        const BYTE *ani = find_res(inst, name, cursor ? RT_ANICURSOR : RT_ANIICON, &size);
        return ani ? (HICON)ani_from_mem(ani, size, cx, cy, cursor) : 0;
    }
    int n = *(const WORD *)(dir + 4);
    /* every image, the best one first */
    int best = pick_entry(dir, cursor, cx, cy);
    if (best < 0) return 0;
    Icon *first = NULL, *last = NULL;
    for (int k = -1; k < n; k++) {
        int i = k < 0 ? best : k;
        if (k >= 0 && k == best) continue;
        WORD id = *(const WORD *)(dir + 6 + i * 14 + 12);
        DWORD isz;
        const BYTE *img = find_res(inst, MAKEINTRESOURCEW(id), cursor ? RT_CURSOR : RT_ICON, &isz);
        if (!img) continue;
        Icon *ic = decode_image(img, isz, cursor);
        if (!ic) continue;
        if (cursor) { ic->hot.x = *(const WORD *)img; ic->hot.y = *(const WORD *)(img + 2); }
        if (!first) first = ic; else last->more = ic;
        last = ic;
    }
    return (HICON)first;
}

/* -----------------------------------------------------------------------
 * The system's icons, drawn: application, error, question, warning, information
 * ----------------------------------------------------------------------- */
static void cover_circle(Icon *ic, float cx, float cy, float r, DWORD color)
{
    for (int y = 0; y < ic->h; y++)
        for (int x = 0; x < ic->w; x++) {
            int hits = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    float px = x + (sx + 0.5f) / 4 - cx, py = y + (sy + 0.5f) / 4 - cy;
                    if (px * px + py * py <= r * r) hits++;
                }
            if (!hits) continue;
            int a = hits * 255 / 16;
            ic->argb[(size_t)y * ic->w + x] = (DWORD)a << 24 | (color & 0xFFFFFF);
        }
}

static void cover_triangle(Icon *ic, DWORD color)
{
    float w = (float)ic->w, h = (float)ic->h;
    for (int y = 0; y < ic->h; y++)
        for (int x = 0; x < ic->w; x++) {
            int hits = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    float px = x + (sx + 0.5f) / 4, py = y + (sy + 0.5f) / 4;
                    float top = h * 0.06f, bot = h * 0.94f;
                    if (py < top || py > bot) continue;
                    float half = (py - top) / (bot - top) * w * 0.47f;
                    if (px >= w / 2 - half && px <= w / 2 + half) hits++;
                }
            if (!hits) continue;
            ic->argb[(size_t)y * ic->w + x] = (DWORD)(hits * 255 / 16) << 24 | (color & 0xFFFFFF);
        }
}

/* A white (or dark) glyph over the shape, drawn with the UI font */
static void stamp_glyph(Icon *ic, WCHAR ch, DWORD color, int yoff)
{
    int w = ic->w, h = ic->h;
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void *bits;
    HBITMAP bm = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bm) return;
    HDC dc = CreateCompatibleDC(NULL);
    HGDIOBJ ob = SelectObject(dc, bm);
    RECT r = { 0, yoff, w, h + yoff };
    fill_rect(dc, &r, 0x000000);
    HFONT f = CreateFontW(-h * 7 / 10, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HGDIOBJ of = SelectObject(dc, f);
    SetTextColor(dc, 0xFFFFFF);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, &ch, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
    SelectObject(dc, of);
    DeleteObject(f);
    GdiFlush();
    const DWORD *src = bits;
    for (int i = 0; i < w * h; i++) {
        int cov = (int)(src[i] & 0xFF);
        if (!cov) continue;
        DWORD d = ic->argb[i];
        int a = (int)(d >> 24);
        int dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
        int cr = (color >> 16) & 0xFF, cg = (color >> 8) & 0xFF, cb = color & 0xFF;
        dr = (cr * cov + dr * (255 - cov)) / 255; dg = (cg * cov + dg * (255 - cov)) / 255; db = (cb * cov + db * (255 - cov)) / 255;
        ic->argb[i] = (DWORD)a << 24 | (DWORD)dr << 16 | (DWORD)dg << 8 | (DWORD)db;
    }
    SelectObject(dc, ob);
    DeleteDC(dc);
    DeleteObject(bm);
}

static Icon *make_sys(int which, int size)
{
    Icon *ic = new_icon(size, size);
    if (!ic) return NULL;
    float c = size / 2.0f, r = size * 0.47f;
    switch (which) {
    case 32513: cover_circle(ic, c, c, r, 0xE81123); stamp_glyph(ic, 0x2715, 0xFFFFFF, 0); break;   /* error */
    case 32514: cover_circle(ic, c, c, r, 0x0063B1); stamp_glyph(ic, '?', 0xFFFFFF, 0); break;      /* question */
    case 32515: cover_triangle(ic, 0xFFB900); stamp_glyph(ic, '!', 0x000000, size / 12); break;    /* warning */
    case 32516: cover_circle(ic, c, c, r, 0x0063B1); stamp_glyph(ic, 'i', 0xFFFFFF, 0); break;      /* information */
    case 32518: cover_circle(ic, c, c, r, 0x0078D7); stamp_glyph(ic, '!', 0xFFFFFF, 0); break;      /* shield */
    default: {                                                                                       /* application */
        for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++) {
                int border = x < 2 || y < 2 || x >= size - 2 || y >= size - 2;
                int title = y < size / 4;
                DWORD col = border ? 0x606060 : title ? 0x0078D7 : 0xFFFFFF;
                if (x >= 1 && y >= 3 && x < size - 1 && y < size - 1) ic->argb[(size_t)y * size + x] = 0xFF000000u | col;
            }
    }
    }
    return ic;
}

HICON sys_icon(int which)
{
    static Icon *cache[8][2];
    int k = which >= 32512 && which <= 32518 ? which - 32512 : 0;
    if (!cache[k][0]) {
        cache[k][0] = make_sys(32512 + k, 32);
        cache[k][1] = make_sys(32512 + k, 16);
        if (cache[k][0]) { cache[k][0]->shared = 1; cache[k][0]->more = cache[k][1]; }
        if (cache[k][1]) cache[k][1]->shared = 1;
    }
    return (HICON)cache[k][0];
}

/* NovaOS's drawing of system pointer @which at @scale (the kernel draws
 * the same outlines on the screen), or NULL if there is no such pointer */
static Icon *sys_cursor_image(int which, int scale)
{
    int side = 32 * scale;
    DWORD *buf = malloc(16 + (size_t)side * side * 4);
    if (!buf) return NULL;
    Icon *ic = NULL;
    if (NtNovaGuiCtl(0, CTL_SYSCURSOR_IMAGE, (ULONG_PTR)(which | scale << 16), buf) && (int)buf[0] == side) {
        ic = new_icon(side, side);
        if (ic) {
            memcpy(ic->argb, buf + 4, (size_t)side * side * 4);
            ic->hot.x = (LONG)buf[2]; ic->hot.y = (LONG)buf[3];
            ic->cursor = 1; ic->shared = 1; ic->sys = which;
        }
    }
    free(buf);
    return ic;
}

/* IDC_* (32512..32672): 32 x 32, with a 64 x 64 image for 2x displays */
static HCURSOR sys_cursor(int which)
{
    static Icon *cache[192];
    if (which < 32512 || which >= 32512 + 192) which = 32512;
    int k = which - 32512;
    if (!cache[k]) {
        Icon *ic = sys_cursor_image(which, 1);
        if (!ic) return which == 32512 ? NULL : sys_cursor(32512);
        ic->more = sys_cursor_image(which, 2);
        cache[k] = ic;
    }
    return (HCURSOR)cache[k];
}

USERAPI HICON LoadIconW(HINSTANCE inst, LPCWSTR name)
{
    if (!inst || ((ULONG_PTR)name >= 32512 && (ULONG_PTR)name <= 32518)) {
        if ((ULONG_PTR)name < 0x10000 && (!inst || !find_res(inst, name, RT_GROUP_ICON, NULL))) return sys_icon((int)(ULONG_PTR)name);
    }
    int cx = GetSystemMetrics(SM_CXICON), cy = GetSystemMetrics(SM_CYICON);
    HANDLE h = shared_find(inst, name, IMAGE_ICON, cx, cy);
    if (h) return h;
    h = load_icon_res(inst, name, cx, cy, 0);
    if (h) shared_add(inst, name, IMAGE_ICON, cx, cy, h);
    return h;
}

USERAPI HICON LoadIconA(HINSTANCE inst, LPCSTR name)
{
    if ((ULONG_PTR)name < 0x10000) return LoadIconW(inst, (LPCWSTR)name);
    WCHAR *w = a2w(name, -1);
    HICON r = LoadIconW(inst, w);
    free(w);
    return r;
}

USERAPI HCURSOR LoadCursorW(HINSTANCE inst, LPCWSTR name)
{
    if (!inst && (ULONG_PTR)name < 0x10000) return sys_cursor((int)(ULONG_PTR)name);
    HANDLE h = shared_find(inst, name, IMAGE_CURSOR, 32, 32);
    if (h) return h;
    h = load_icon_res(inst, name, 32, 32, 1);
    if (!h) h = sys_cursor(32512);
    else shared_add(inst, name, IMAGE_CURSOR, 32, 32, h);
    return h;
}

USERAPI HCURSOR LoadCursorA(HINSTANCE inst, LPCSTR name)
{
    if ((ULONG_PTR)name < 0x10000) return LoadCursorW(inst, (LPCWSTR)name);
    WCHAR *w = a2w(name, -1);
    HCURSOR r = LoadCursorW(inst, w);
    free(w);
    return r;
}

static HICON icon_from_file(LPCWSTR name, int cx, int cy, int cursor);

/* A .cur or .ani file */
USERAPI HCURSOR LoadCursorFromFileW(LPCWSTR f)
{
    if (!f) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    int cx = GetSystemMetrics(SM_CXCURSOR), cy = GetSystemMetrics(SM_CYCURSOR);
    HCURSOR c = icon_from_file(f, cx, cy, 1);
    if (!c && GetLastError() == ERROR_SUCCESS) SetLastError(ERROR_FILE_NOT_FOUND);
    return c;
}

USERAPI HCURSOR LoadCursorFromFileA(LPCSTR f)
{
    if (!f) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    WCHAR *w = a2w(f, -1);
    HCURSOR r = LoadCursorFromFileW(w);
    free(w);
    return r;
}

static void *read_file(LPCWSTR name, DWORD *size)
{
    HANDLE f = CreateFileW(name, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    DWORD n = GetFileSize(f, NULL), got = 0;
    void *buf = n && n < 64 * 1024 * 1024 ? malloc(n) : NULL;
    if (buf && (!ReadFile(f, buf, n, &got, NULL) || got != n)) { free(buf); buf = NULL; }
    CloseHandle(f);
    if (size) *size = n;
    return buf;
}

/* The images of an .ico / .cur file in memory, the best one for cx x cy first */
static Icon *icon_from_mem(const BYTE *p, DWORD size, int cx, int cy, int cursor)
{
    Icon *first = NULL, *last = NULL;
    if (size < 6 || *(const WORD *)(p + 2) < 1 || *(const WORD *)(p + 2) > 2) return NULL;
    int n = *(const WORD *)(p + 4);
    int best = -1, bestd = 1 << 30;
    for (int i = 0; i < n && 6 + i * 16 + 16 <= (int)size; i++) {
        const BYTE *e = p + 6 + i * 16;
        int w = e[0] ? e[0] : 256, h = e[1] ? e[1] : 256;
        int d = abs(w - cx) + abs(h - cy) + (w < cx ? 1000 : 0);
        if (d < bestd) { bestd = d; best = i; }
    }
    for (int k = -1; k < n; k++) {
        int i = k < 0 ? best : k;
        if (i < 0 || (k >= 0 && k == best)) continue;
        const BYTE *e = p + 6 + i * 16;
        DWORD isz = *(const DWORD *)(e + 8), off = *(const DWORD *)(e + 12);
        if (off > size || isz > size - off) continue;
        Icon *ic = decode_image(p + off, isz, 0);
        if (!ic) continue;
        ic->cursor = cursor;
        if (cursor && *(const WORD *)(p + 2) == 2) { ic->hot.x = *(const WORD *)(e + 4); ic->hot.y = *(const WORD *)(e + 6); }
        else if (cursor) { ic->hot.x = ic->w / 2; ic->hot.y = ic->h / 2; }
        if (!first) first = ic; else last->more = ic;
        last = ic;
    }
    return first;
}

/* -----------------------------------------------------------------------
 * Animated cursors (.ani): a RIFF "ACON" file.  "anih" gives the frame and
 * step counts and the default rate (jiffies, 1/60 s), "rate" and "seq "
 * (optional) each step's time and frame, and LIST "fram" holds the frames,
 * one "icon" chunk each: a whole .ico or .cur file.
 * ----------------------------------------------------------------------- */
#define ANI_ICON     1                  /* anih flags: frames are .ico/.cur files */
#define ANI_SEQUENCE 2                  /* there is a "seq " chunk */

static Icon *ani_from_mem(const BYTE *p, DWORD size, int cx, int cy, int cursor)
{
    if (size < 12 || memcmp(p, "RIFF", 4) || memcmp(p + 8, "ACON", 4)) return NULL;
    DWORD riff = *(const DWORD *)(p + 4);
    if (riff < size - 8) size = riff + 8;
    DWORD nframes = 0, nsteps = 0, disp = 0, flags = 0;
    const DWORD *rate = NULL, *seq = NULL;
    DWORD nrate = 0, nseq = 0;
    const BYTE *fram = NULL;
    DWORD framsz = 0;
    for (DWORD off = 12; off + 8 <= size; ) {
        const BYTE *ck = p + off;
        DWORD len = *(const DWORD *)(ck + 4);
        if (len > size - off - 8) len = size - off - 8;
        const BYTE *d = ck + 8;
        if (!memcmp(ck, "anih", 4) && len >= 36) {
            nframes = *(const DWORD *)(d + 4); nsteps = *(const DWORD *)(d + 8);
            disp = *(const DWORD *)(d + 28); flags = *(const DWORD *)(d + 32);
        } else if (!memcmp(ck, "rate", 4)) { rate = (const DWORD *)d; nrate = len / 4; }
        else if (!memcmp(ck, "seq ", 4)) { seq = (const DWORD *)d; nseq = len / 4; }
        else if (!memcmp(ck, "LIST", 4) && len >= 4 && !memcmp(d, "fram", 4)) { fram = d + 4; framsz = len - 4; }
        off += 8 + ((len + 1) & ~1u);
    }
    if (!fram || !nframes || nframes > 1024) return NULL;
    if (!nsteps) nsteps = nframes;
    if (nsteps > 4096) return NULL;
    Ani *a = calloc(1, sizeof(Ani));
    Icon **frames = calloc(nframes, sizeof(Icon *));
    DWORD *sq = calloc(nsteps, 4), *rt = calloc(nsteps, 4);
    int n = 0;
    for (DWORD off = 0; a && frames && sq && rt && off + 8 <= framsz && n < (int)nframes; ) {
        const BYTE *ck = fram + off;
        DWORD len = *(const DWORD *)(ck + 4);
        if (len > framsz - off - 8) len = framsz - off - 8;
        if (!memcmp(ck, "icon", 4)) {
            Icon *ic = (flags & ANI_ICON) ? icon_from_mem(ck + 8, len, cx, cy, cursor) : decode_image(ck + 8, len, 0);
            if (!ic) break;
            if (!(flags & ANI_ICON)) { ic->cursor = cursor; ic->hot.x = ic->w / 2; ic->hot.y = ic->h / 2; }
            frames[n++] = ic;
        }
        off += 8 + ((len + 1) & ~1u);
    }
    if (!a || !frames || !sq || !rt || n != (int)nframes) {
        for (int i = 0; i < n; i++) free_icon(frames[i]);
        free(a); free(frames); free(sq); free(rt);
        return NULL;
    }
    for (DWORD i = 0; i < nsteps; i++) {
        DWORD f = (flags & ANI_SEQUENCE) && seq && i < nseq ? seq[i] : i;
        sq[i] = f < nframes ? f : 0;
        rt[i] = rate && i < nrate ? rate[i] : disp;
        if (!rt[i]) rt[i] = 1;
    }
    a->nframes = (int)nframes; a->nsteps = (int)nsteps;
    a->frames = frames; a->seq = sq; a->rate = rt;
    frames[0]->ani = a;
    return frames[0];
}

Icon *icon_step(Icon *ic, UINT step)
{
    if (!ic || !ic->ani) return ic;
    return ic->ani->frames[ic->ani->seq[step % (UINT)ic->ani->nsteps]];
}

/* user32's GetCursorFrameInfo (no header declares it): the frame shown at
 * @step, its time in jiffies and the number of steps */
USERAPI HCURSOR GetCursorFrameInfo(HCURSOR h, DWORD reserved, DWORD step, DWORD *rate, DWORD *nsteps)
{
    (void)reserved;
    Icon *ic = icon_of(h);
    if (!ic) return 0;
    if (!ic->ani) { if (rate) *rate = 0; if (nsteps) *nsteps = 1; return h; }
    if (step >= (DWORD)ic->ani->nsteps) return 0;
    if (rate) *rate = ic->ani->rate[step];
    if (nsteps) *nsteps = (DWORD)ic->ani->nsteps;
    return (HCURSOR)ic->ani->frames[ic->ani->seq[step]];
}

/* An .ico, .cur or .ani file */
static HICON icon_from_file(LPCWSTR name, int cx, int cy, int cursor)
{
    DWORD size;
    BYTE *p = read_file(name, &size);
    if (!p) return 0;
    Icon *ic = size >= 12 && !memcmp(p, "RIFF", 4) ? ani_from_mem(p, size, cx, cy, cursor) : icon_from_mem(p, size, cx, cy, cursor);
    free(p);
    return (HICON)ic;
}

/* -----------------------------------------------------------------------
 * The pointer: the kernel draws it over our windows (SetCursor)
 * ----------------------------------------------------------------------- */
/* @ic's image nearest to cx x cy */
static Icon *nearest(Icon *ic, int cx, int cy)
{
    Icon *best = ic;
    for (Icon *i = ic; i; i = i->more) {
        int d = abs(i->w - cx) + abs(i->h - cy), bd = abs(best->w - cx) + abs(best->h - cy);
        if (d < bd || (d == bd && i->w > best->w)) best = i;
    }
    return best;
}

int display_scale(void)
{
    INT32 m[4];
    int lw = GetSystemMetrics(SM_CXSCREEN);
    if (lw <= 0 || !NtNovaGuiCtl(0, CTL_DISPLAY_MODE, (ULONG_PTR)(LONG_PTR)-1, m)) return 1;
    int s = m[0] / lw;
    return s < 1 ? 1 : s > 2 ? 2 : s;
}

/* @fi resampled to w x h into @out: averaged (premultiplied) when it
 * grows, so a 32 x 32 cursor on a 2x display is smooth, not blocky */
static void resample(const Icon *fi, DWORD *out, int w, int h)
{
    if (fi->w == w && fi->h == h) { memcpy(out, fi->argb, (size_t)w * h * 4); return; }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            if (fi->w >= w) { out[y * w + x] = fi->argb[(size_t)(y * fi->h / h) * fi->w + x * fi->w / w]; continue; }
            /* source position of this pixel's centre, in 1/256 */
            int sx = ((2 * x + 1) * fi->w * 128) / w - 128, sy = ((2 * y + 1) * fi->h * 128) / h - 128;
            if (sx < 0) sx = 0;
            if (sy < 0) sy = 0;
            int x0 = sx >> 8, y0 = sy >> 8, fx = sx & 255, fy = sy & 255;
            int x1 = x0 + 1 < fi->w ? x0 + 1 : x0, y1 = y0 + 1 < fi->h ? y0 + 1 : y0;
            DWORD q[4] = { fi->argb[y0 * fi->w + x0], fi->argb[y0 * fi->w + x1], fi->argb[y1 * fi->w + x0], fi->argb[y1 * fi->w + x1] };
            int wt[4] = { (256 - fx) * (256 - fy), fx * (256 - fy), (256 - fx) * fy, fx * fy };
            unsigned a = 0, r = 0, g = 0, b = 0;
            for (int i = 0; i < 4; i++) {
                unsigned qa = q[i] >> 24, k = (unsigned)wt[i] * qa;
                a += k; r += ((q[i] >> 16) & 255) * k; g += ((q[i] >> 8) & 255) * k; b += (q[i] & 255) * k;
            }
            out[y * w + x] = a ? (a >> 16) << 24 | (r / a) << 16 | (g / a) << 8 | (b / a) : 0;
        }
}

/* SET_CURSOR's shape for @ic at display scale @s (device pixels when s > 1) */
static INT32 *cursor_blob(Icon *ic, int s)
{
    int nf = ic->ani ? ic->ani->nframes : 1, ns = ic->ani ? ic->ani->nsteps : 1;
    if (nf > 64) nf = 64;
    if (ns > 256) ns = 256;
    Icon *f0 = nearest(ic, GetSystemMetrics(SM_CXCURSOR), GetSystemMetrics(SM_CYCURSOR));
    int w = (f0->w > 64 ? 64 : f0->w) * s, h = (f0->h > 64 ? 64 : f0->h) * s;
    INT32 *buf = malloc(24 + (size_t)ns * 8 + (size_t)nf * w * h * 4);
    if (!buf) return NULL;
    Icon *d0 = nearest(ic, w, h);                   /* the image used at this scale */
    POINT hot = ic->cursor ? d0->hot : (POINT){ d0->w / 2, d0->h / 2 };
    buf[0] = w; buf[1] = h; buf[2] = hot.x * w / d0->w; buf[3] = hot.y * h / d0->h; buf[4] = nf; buf[5] = ns;
    DWORD *st = (DWORD *)(buf + 6), *px = st + ns * 2;
    for (int i = 0; i < ns; i++) {
        DWORD fr = ic->ani ? ic->ani->seq[i] : 0;
        st[i * 2] = fr < (DWORD)nf ? fr : 0;
        st[i * 2 + 1] = ic->ani ? ic->ani->rate[i] : 0;
    }
    for (int k = 0; k < nf; k++)                    /* each frame at the first one's size */
        resample(nearest(ic->ani ? ic->ani->frames[k] : ic, w, h), px + (size_t)k * w * h, w, h);
    return buf;
}

void cursor_to_kernel(HCURSOR c, int hidden)
{
    static DWORD sent_serial = ~0u;
    static int sent_hidden = -1, sent_scale;
    Icon *ic = icon_of(c);
    DWORD serial = ic ? ic->serial : 0;
    int s = ic && !ic->sys ? display_scale() : 1;
    hidden = hidden || !c;
    if (serial == sent_serial && hidden == sent_hidden && s == sent_scale) return;
    sent_serial = serial; sent_hidden = hidden; sent_scale = s;
    if (hidden) { NtNovaGuiCtl(0, CTL_SET_CURSOR, 1, NULL); return; }
    if (!ic || ic->sys == 32512) { NtNovaGuiCtl(0, CTL_SET_CURSOR, 0, NULL); return; }
    if (ic->sys) {                                  /* the kernel draws it, sharp at any scale */
        if (!NtNovaGuiCtl(0, CTL_SET_CURSOR, 3, (PVOID)(ULONG_PTR)ic->sys)) NtNovaGuiCtl(0, CTL_SET_CURSOR, 0, NULL);
        return;
    }
    INT32 *buf = cursor_blob(ic, s);
    if (!buf || !NtNovaGuiCtl(0, CTL_SET_CURSOR, s > 1 ? 4 : 2, buf)) NtNovaGuiCtl(0, CTL_SET_CURSOR, 0, NULL);
    free(buf);
}

/* Replaces system pointer @id for every program, and destroys @c as
 * Windows does */
USERAPI BOOL SetSystemCursor(HCURSOR c, DWORD id)
{
    Icon *ic = icon_of(c);
    if (!ic) { SetLastError(1402 /* ERROR_INVALID_CURSOR_HANDLE */); return FALSE; }
    int s = display_scale();
    INT32 *buf = cursor_blob(ic, s);
    BOOL ok = buf && NtNovaGuiCtl(0, CTL_SET_SYSCURSOR, id | (s > 1 ? 0x10000 : 0), buf);
    free(buf);
    if (!ok) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    DestroyCursor(c);
    return TRUE;
}

/* -----------------------------------------------------------------------
 * Bitmaps
 * ----------------------------------------------------------------------- */
static HBITMAP dib_from_info(const BITMAPINFO *bi, const void *bits_in, UINT flags)
{
    const BITMAPINFOHEADER *h = &bi->bmiHeader;
    int w = h->biWidth, ht = h->biHeight < 0 ? -h->biHeight : h->biHeight;
    if (w <= 0 || ht <= 0) return 0;
    BITMAPINFO out;
    if ((flags & LR_CREATEDIBSECTION) && h->biBitCount == 24 && h->biCompression == BI_RGB) {
        /* the program wants the DIB as stored: a 24-bit section in the
         * resource's row order, whose bits it reads back with GetObject */
        memset(&out, 0, sizeof(out));
        out.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        out.bmiHeader.biWidth = w; out.bmiHeader.biHeight = h->biHeight;
        out.bmiHeader.biPlanes = 1; out.bmiHeader.biBitCount = 24;
        void *v;
        HBITMAP bm = CreateDIBSection(NULL, &out, DIB_RGB_COLORS, &v, NULL, 0);
        if (!bm) return 0;
        const BYTE *src = bits_in ? bits_in : (const BYTE *)bi + h->biSize + (h->biClrUsed ? h->biClrUsed * 4 : 0);
        memcpy(v, src, (size_t)(((w * 3) + 3) & ~3) * ht);
        return bm;
    }
    memset(&out, 0, sizeof(out));
    out.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    out.bmiHeader.biWidth = w; out.bmiHeader.biHeight = -ht;
    out.bmiHeader.biPlanes = 1; out.bmiHeader.biBitCount = 32;
    void *bits;
    HBITMAP bm = CreateDIBSection(NULL, &out, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bm) return 0;
    int bpp = h->biBitCount;
    int ncolors = bpp <= 8 ? (h->biClrUsed ? (int)h->biClrUsed : 1 << bpp) : 0;
    const RGBQUAD *pal = (const RGBQUAD *)((const BYTE *)bi + h->biSize);
    const BYTE *src = bits_in ? bits_in : (const BYTE *)pal + ncolors * 4 + (h->biCompression == BI_BITFIELDS ? 12 : 0);
    int stride = ((w * bpp + 31) / 32) * 4;
    DWORD *dst = bits;
    int any_alpha = 0;
    for (int y = 0; y < ht; y++) {
        const BYTE *row = src + (size_t)(h->biHeight > 0 ? ht - 1 - y : y) * stride;
        for (int x = 0; x < w; x++) {
            DWORD c = 0;
            switch (bpp) {
            case 32: c = *(const DWORD *)(row + x * 4); if (c >> 24) any_alpha = 1; break;
            case 24: c = row[x * 3] | row[x * 3 + 1] << 8 | row[x * 3 + 2] << 16; break;
            case 16: { WORD v = *(const WORD *)(row + x * 2); c = ((v >> 10) & 31) * 255 / 31 << 16 | ((v >> 5) & 31) * 255 / 31 << 8 | (v & 31) * 255 / 31; break; }
            case 8: { RGBQUAD q = pal[row[x]]; c = q.rgbBlue | q.rgbGreen << 8 | q.rgbRed << 16; break; }
            case 4: { RGBQUAD q = pal[(row[x / 2] >> (x & 1 ? 0 : 4)) & 15]; c = q.rgbBlue | q.rgbGreen << 8 | q.rgbRed << 16; break; }
            case 1: { RGBQUAD q = pal[(row[x / 8] >> (7 - (x & 7))) & 1]; c = q.rgbBlue | q.rgbGreen << 8 | q.rgbRed << 16; break; }
            }
            dst[(size_t)y * w + x] = c;
        }
    }
    if (!any_alpha) for (int i = 0; i < w * ht; i++) dst[i] |= 0xFF000000u;
    else for (int i = 0; i < w * ht; i++) {                 /* premultiplied, as AlphaBlend takes it */
        DWORD c = dst[i]; int a = (int)(c >> 24);
        dst[i] = (DWORD)a << 24 | ((c >> 16 & 0xFF) * a / 255) << 16 | ((c >> 8 & 0xFF) * a / 255) << 8 | (c & 0xFF) * a / 255;
    }
    return bm;
}

HBITMAP load_bitmap_res(HINSTANCE inst, LPCWSTR name, UINT flags)
{
    DWORD size;
    const BITMAPINFO *bi = find_res(inst, name, RT_BITMAP, &size);
    if (!bi) return 0;
    return dib_from_info(bi, NULL, flags);
}

USERAPI HBITMAP LoadBitmapW(HINSTANCE inst, LPCWSTR name)
{
    if (!inst) return 0;                                    /* OBM_*: none */
    HBITMAP b = load_bitmap_res(inst, name, 0);
    if (!b) SetLastError(ERROR_RESOURCE_NAME_NOT_FOUND);
    return b;
}

USERAPI HBITMAP LoadBitmapA(HINSTANCE inst, LPCSTR name)
{
    if ((ULONG_PTR)name < 0x10000) return LoadBitmapW(inst, (LPCWSTR)name);
    WCHAR *w = a2w(name, -1);
    HBITMAP r = LoadBitmapW(inst, w);
    free(w);
    return r;
}

USERAPI HANDLE LoadImageW(HINSTANCE inst, LPCWSTR name, UINT type, int cx, int cy, UINT flags)
{
    if ((flags & LR_DEFAULTSIZE) || type == IMAGE_ICON || type == IMAGE_CURSOR) {
        if (!cx) cx = (flags & LR_DEFAULTSIZE) ? GetSystemMetrics(type == IMAGE_CURSOR ? SM_CXCURSOR : SM_CXICON) : 0;
        if (!cy) cy = (flags & LR_DEFAULTSIZE) ? GetSystemMetrics(type == IMAGE_CURSOR ? SM_CYCURSOR : SM_CYICON) : 0;
    }
    if (flags & LR_LOADFROMFILE) {
        if (type == IMAGE_BITMAP) {
            DWORD size;
            BYTE *p = read_file(name, &size);
            if (!p) return 0;
            HBITMAP b = 0;
            if (size > 14 + sizeof(BITMAPINFOHEADER) && p[0] == 'B' && p[1] == 'M')
                b = dib_from_info((const BITMAPINFO *)(p + 14), p + *(DWORD *)(p + 10), flags);
            free(p);
            return b;
        }
        return icon_from_file(name, cx ? cx : 32, cy ? cy : 32, type == IMAGE_CURSOR);
    }
    switch (type) {
    case IMAGE_BITMAP: { HBITMAP b = inst ? load_bitmap_res(inst, name, flags) : 0; if (!b) SetLastError(ERROR_RESOURCE_NAME_NOT_FOUND); return b; }
    case IMAGE_ICON: case IMAGE_CURSOR: {
        if (!inst && (ULONG_PTR)name < 0x10000) return type == IMAGE_ICON ? sys_icon((int)(ULONG_PTR)name) : sys_cursor((int)(ULONG_PTR)name);
        int w = cx ? cx : 32, h = cy ? cy : 32;
        if (flags & LR_SHARED) {
            HANDLE s = shared_find(inst, name, (int)type, w, h);
            if (s) return s;
        }
        HANDLE r = load_icon_res(inst, name, w, h, type == IMAGE_CURSOR);
        if (r && (flags & LR_SHARED)) shared_add(inst, name, (int)type, w, h, r);
        if (!r) SetLastError(ERROR_RESOURCE_NAME_NOT_FOUND);
        return r;
    }
    }
    return 0;
}

USERAPI HANDLE LoadImageA(HINSTANCE inst, LPCSTR name, UINT type, int cx, int cy, UINT flags)
{
    if ((ULONG_PTR)name < 0x10000) return LoadImageW(inst, (LPCWSTR)name, type, cx, cy, flags);
    WCHAR *w = a2w(name, -1);
    HANDLE r = LoadImageW(inst, w, type, cx, cy, flags);
    free(w);
    return r;
}

/* -----------------------------------------------------------------------
 * Making and taking apart icons
 * ----------------------------------------------------------------------- */
/* A monochrome image's pixel from its AND (transparent) and XOR bits:
 * black, white, transparent, or "invert the screen", which the desktop
 * cannot draw and gets black (it is mostly the edge of an I-beam) */
static DWORD mono_pixel(int and, int xor)
{
    if (!and) return xor ? 0xFFFFFFFFu : 0xFF000000u;
    return xor ? 0xFF000000u : 0;
}

/* GDK makes its cursors here: a 32-bit image with alpha and a 1-bit mask
 * from a pixbuf, or (cursors drawn in two colours) a monochrome mask
 * twice the cursor's height, the AND half over the XOR half */
USERAPI HICON CreateIconIndirect(PICONINFO ii)
{
    if (!ii) return 0;
    BITMAP bm;
    HBITMAP src = ii->hbmColor ? ii->hbmColor : ii->hbmMask;
    if (!src || !GetObjectW(src, sizeof(bm), &bm)) return 0;
    int mono = !ii->hbmColor;
    int w = bm.bmWidth, h = mono ? bm.bmHeight / 2 : bm.bmHeight;
    if (w <= 0 || h <= 0) return 0;
    Icon *ic = new_icon(w, h);
    if (!ic) return 0;
    ic->cursor = !ii->fIcon;
    ic->hot.x = (LONG)ii->xHotspot; ic->hot.y = (LONG)ii->yHotspot;
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    HDC dc = GetDC(NULL);
    int any_alpha = 0;
    if (ii->hbmColor) {
        GetDIBits(dc, ii->hbmColor, 0, (UINT)h, ic->argb, &bi, DIB_RGB_COLORS);
        for (int i = 0; i < w * h; i++) if (ic->argb[i] >> 24) { any_alpha = 1; break; }
    }
    if (!any_alpha) {
        DWORD *mask = malloc((size_t)w * h * 4 * (mono ? 2 : 1));
        if (mask && ii->hbmMask) {
            bi.bmiHeader.biHeight = -h * (mono ? 2 : 1);
            GetDIBits(dc, ii->hbmMask, 0, (UINT)(h * (mono ? 2 : 1)), mask, &bi, DIB_RGB_COLORS);
            for (int i = 0; i < w * h; i++) {
                int and = (mask[i] & 0xFFFFFF) != 0;
                if (mono) ic->argb[i] = mono_pixel(and, (mask[(size_t)w * h + i] & 0xFFFFFF) != 0);
                else ic->argb[i] = and ? 0 : (ic->argb[i] | 0xFF000000u);
            }
        } else for (int i = 0; i < w * h; i++) ic->argb[i] |= 0xFF000000u;
        free(mask);
    } else {
        /* the alpha channel decides; the mask is only for old displays */
    }
    ReleaseDC(NULL, dc);
    return (HICON)ic;
}

USERAPI HICON CreateIcon(HINSTANCE inst, int w, int h, BYTE planes, BYTE bpp, const BYTE *and, const BYTE *xor)
{
    (void)inst; (void)planes;
    Icon *ic = new_icon(w, h);
    if (!ic) return 0;
    int astride = ((w + 15) / 16) * 2, xstride = ((w * bpp + 15) / 16) * 2;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int transparent = (and[y * astride + x / 8] >> (7 - (x & 7))) & 1;
            DWORD c = 0;
            if (bpp == 1) { ic->argb[(size_t)y * w + x] = mono_pixel(transparent, (xor[y * xstride + x / 8] >> (7 - (x & 7))) & 1); continue; }
            if (bpp == 32) c = *(const DWORD *)(xor + y * xstride + x * 4);
            else if (bpp == 24) { const BYTE *q = xor + y * xstride + x * 3; c = (DWORD)q[2] << 16 | (DWORD)q[1] << 8 | q[0]; }
            ic->argb[(size_t)y * w + x] = transparent ? 0 : (c & 0xFFFFFF) | 0xFF000000u;
        }
    return (HICON)ic;
}

USERAPI HCURSOR CreateCursor(HINSTANCE inst, int hx, int hy, int w, int h, const void *and, const void *xor)
{
    HICON ic = CreateIcon(inst, w, h, 1, 1, and, xor);
    Icon *i = icon_of(ic);
    if (i) { i->cursor = 1; i->hot.x = hx; i->hot.y = hy; }
    return ic;
}

USERAPI BOOL GetIconInfo(HICON h, PICONINFO ii)
{
    Icon *ic = icon_of(h);
    if (!ic || !ii) return FALSE;
    memset(ii, 0, sizeof(*ii));
    ii->fIcon = !ic->cursor;
    ii->xHotspot = ic->cursor ? (DWORD)ic->hot.x : (DWORD)ic->w / 2;
    ii->yHotspot = ic->cursor ? (DWORD)ic->hot.y : (DWORD)ic->h / 2;
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = ic->w; bi.bmiHeader.biHeight = -ic->h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void *bits;
    ii->hbmColor = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (ii->hbmColor) memcpy(bits, ic->argb, (size_t)ic->w * ic->h * 4);
    void *mbits;
    ii->hbmMask = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &mbits, NULL, 0);
    if (ii->hbmMask) {
        DWORD *m = mbits;
        for (int i = 0; i < ic->w * ic->h; i++) m[i] = (ic->argb[i] >> 24) ? 0 : 0xFFFFFF;
    }
    return TRUE;
}

USERAPI BOOL GetIconInfoExW(HICON h, void *ix)
{
    BYTE *p = ix;                                           /* cbSize, then ICONINFO */
    if (!p) return FALSE;
    return GetIconInfo(h, (PICONINFO)(p + 8));
}

static Icon *copy_icon(Icon *ic, int cx, int cy)
{
    if (!ic) return NULL;
    Icon *first = NULL, *last = NULL;
    for (Icon *s = ic; s; s = s->more) {
        int w = cx && s == ic ? cx : s->w, h = cy && s == ic ? cy : s->h;
        Icon *n = new_icon(w, h);
        if (!n) break;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) n->argb[(size_t)y * w + x] = s->argb[(size_t)(y * s->h / h) * s->w + x * s->w / w];
        n->cursor = s->cursor; n->hot = s->hot;
        if (!first) first = n; else last->more = n;
        last = n;
    }
    return first;
}

USERAPI HICON CopyIcon(HICON h) { return (HICON)copy_icon(icon_of(h), 0, 0); }

USERAPI HANDLE CopyImage(HANDLE h, UINT type, int cx, int cy, UINT flags)
{
    if (type == IMAGE_ICON || type == IMAGE_CURSOR) {
        Icon *ic = icon_of((HICON)h);
        if (!ic) return 0;
        HANDLE r = (HANDLE)copy_icon(ic, cx, cy);
        if (r && (flags & LR_COPYDELETEORG)) DestroyIcon((HICON)h);
        return r;
    }
    if (type == IMAGE_BITMAP) {
        BITMAP bm;
        if (!GetObjectW(h, sizeof(bm), &bm)) return 0;
        int w = cx ? cx : bm.bmWidth, ht = cy ? cy : bm.bmHeight;
        BITMAPINFO bi;
        memset(&bi, 0, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -ht; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        void *bits;
        HBITMAP nb = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (!nb) return 0;
        HDC s = CreateCompatibleDC(NULL), d = CreateCompatibleDC(NULL);
        HGDIOBJ os = SelectObject(s, h), od = SelectObject(d, nb);
        StretchBlt(d, 0, 0, w, ht, s, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
        SelectObject(s, os); SelectObject(d, od);
        DeleteDC(s); DeleteDC(d);
        if (flags & LR_COPYDELETEORG) DeleteObject(h);
        return nb;
    }
    return 0;
}

USERAPI BOOL DestroyIcon(HICON h)
{
    Icon *ic = icon_of(h);
    if (!ic) return FALSE;
    if (!ic->shared) free_icon(ic);
    return TRUE;
}

USERAPI BOOL DestroyCursor(HCURSOR h) { return DestroyIcon(h); }

USERAPI HICON CreateIconFromResourceEx_(void) { return 0; }
USERAPI UINT PrivateExtractIconsW(LPCWSTR file, int idx, int cx, int cy, HICON *icons, UINT *ids, UINT n, UINT flags)
{
    (void)ids; (void)flags;
    if (!icons || !n) return 0;
    HMODULE m = LoadLibraryExW(file, NULL, LOAD_LIBRARY_AS_DATAFILE);
    if (!m) return 0;
    HICON ic = load_icon_res(m, MAKEINTRESOURCEW(idx < 0 ? -idx : idx + 1), cx, cy, 0);
    icons[0] = ic;
    return ic ? 1 : 0;
}
