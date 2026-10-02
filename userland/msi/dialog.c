/*
 * dialog.c — the package's own dialogs
 *
 * A package with a user interface describes its dialogs in tables: Dialog
 * (size, title, the default and cancel buttons), Control (each control's
 * type, place, property and text), ControlEvent (what a button does),
 * ControlCondition (when a control is hidden or disabled), EventMapping
 * (controls that follow the engine's progress), plus TextStyle,
 * RadioButton, CheckBox, ListBox, ComboBox and Binary for their pieces.
 *
 * Sizes are installer units, 4/3 of a pixel at 96 DPI. Static controls
 * (text, bitmaps, icons, lines, group boxes) are painted by the dialog
 * window itself; the rest are ordinary child windows.
 */
#include "msi.h"
#include "engine.h"
#include "dialog.h"
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *s_malloc(size_t n) { return malloc(n); }
static void *s_realloc(void *p, size_t n) { return realloc(p, n); }
static void  s_free(void *p) { free(p); }
#define STBI_MALLOC(n) s_malloc(n)
#define STBI_REALLOC(p, n) s_realloc(p, n)
#define STBI_FREE(p) s_free(p)
#define STBI_ASSERT(x) ((void)0)
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
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

#define SC(v) ((v) * 4 / 3)                 /* installer units to pixels */

/* Control attributes */
#define CA_VISIBLE     0x00000001
#define CA_ENABLED     0x00000002
#define CA_SUNKEN      0x00000004
#define CA_INDIRECT    0x00000008
#define CA_RIGHTALIGN  0x00000040
#define CA_PUSHLIKE    0x00010000           /* CheckBox */
#define CA_MULTILINE   0x00010000           /* Edit */
#define CA_SORTED      0x00010000           /* ListBox, ComboBox */
#define CA_COMBOLIST   0x00020000
#define CA_NOPREFIX    0x00020000           /* Text */
#define CA_NOWRAP      0x00040000           /* Text */
#define CA_BITMAP      0x00040000           /* PushButton, CheckBox */
#define CA_ICON        0x00080000
#define CA_PASSWORD    0x00200000
#define CA_HASBORDER   0x01000000           /* RadioButtonGroup */

/* Dialog attributes */
#define DA_VISIBLE     0x0001
#define DA_MODAL       0x0002
#define DA_MINIMIZE    0x0004

enum { END_NONE, END_RETURN, END_EXIT, END_NEW };

#define WM_TREESYNC (WM_APP + 1)
#define ID_BASE 100
#define ID_RADIO 0x4000                     /* + control * 64 + button */

typedef struct {
    char   name[64], type[32], prop[72], next[64];
    char  *text, *help;
    int    x, y, w, h;
    unsigned attr;
    bool   visible, enabled;
    int    ev_visible, ev_enabled;          /* from EventMapping, -1 none */
    HWND   hwnd;
    char  *override;                        /* text from a subscribed event */
    HFONT  font;
    COLORREF color;
    bool   has_color;
    char  *body;                            /* text after the style prefixes */
    uint32_t *px; int pw, ph;               /* Bitmap */
    HICON  icon;                            /* Icon */
    HWND  *radios; char **rvals; int nradio;
    HTREEITEM *items; char **feat; char **fdesc; char **fdir; int *fparent; int nitems;
    char **vals; int nvals;                 /* list and combo items; directory paths */
    char   shown_path[MAX_PATH * 2];
} Ctl;

typedef struct { int ctl; char action[16]; char *cond; } CtlCond;
typedef struct { int ctl; char event[48]; char attr[24]; } Sub;

typedef struct Win {
    struct Dlg *d;
    struct Win *parent;
    char   name[72];
    HWND   hwnd;
    unsigned attr;
    Ctl   *c; int nc;
    CtlCond *conds; int nconds;
    Sub   *subs; int nsubs;
    int    def, cancel, first;              /* control indexes, -1 none */
    int    end;
    char   next_dlg[72];
    bool   ready;
    char **snap_name, **snap_val; int nsnap;
    char **snap_feat; int *snap_state; int nsnapf;
} Win;

struct Dlg {
    Inst  *in;
    MsiDb *db;
    Win   *modeless;
    Win   *stack[16];
    int    depth;
    bool   cancelled;
    int    busy;                            /* an action runs: ignore buttons */
    int    updating;                        /* our own control updates */
    bool   refreshing;
    struct { char name[72]; HFONT f; COLORREF color; bool has_color; } fonts[32];
    int    nfonts;
    char   default_style[72];
    HBRUSH white;
};

static void refresh(Win *w);
static void refresh_all(Dlg *d);
static int  run_modal(Dlg *d, const char *name, Win *parent);
static void publish(Win *w, const char *event, const char *text, int a, int b);

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */
static void to_w(const char *s, WCHAR *out, int cap)
{
    if (!MultiByteToWideChar(CP_UTF8, 0, s ? s : "", -1, out, cap)) out[0] = 0;
    out[cap - 1] = 0;
}

static WCHAR *wdup(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s ? s : "", -1, NULL, 0);
    WCHAR *w = malloc((size_t)(n + 1) * sizeof(WCHAR));
    if (!MultiByteToWideChar(CP_UTF8, 0, s ? s : "", -1, w, n + 1)) w[0] = 0;
    return w;
}

static void to_u8(const WCHAR *w, char *out, int cap)
{
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL)) out[0] = 0;
    out[cap - 1] = 0;
}

static char *xstrdup(const char *s) { return strdup(s ? s : ""); }

typedef struct { MsiRec **r; int n; } Rows;

/* A query with up to two string parameters */
static Rows query(Dlg *d, const char *sql, const char *a, const char *b)
{
    Rows rs = { NULL, 0 };
    MsiView *v;
    char err[128];
    if (msisql_open(d->db, sql, &v, err, sizeof(err))) return rs;
    MsiRec *p = NULL;
    if (a) {
        p = msirec_new(b ? 2 : 1);
        msirec_set_str(p, 1, a);
        if (b) msirec_set_str(p, 2, b);
    }
    if (!msisql_execute(v, p)) {
        int cap = 0;
        MsiRec *r;
        while (!msisql_fetch(v, &r)) {
            if (rs.n == cap) { cap = cap ? cap * 2 : 16; rs.r = realloc(rs.r, (size_t)cap * sizeof(MsiRec *)); }
            rs.r[rs.n++] = r;
        }
    }
    if (p) msirec_free(p);
    msisql_close(v);
    return rs;
}

static void rows_free(Rows *rs)
{
    for (int i = 0; i < rs->n; i++) msirec_free(rs->r[i]);
    free(rs->r);
    rs->r = NULL;
    rs->n = 0;
}

/* A field as text ("" for null); valid until the fourth next call */
static const char *fs(const MsiRec *r, int i)
{
    static char bufs[8][16];
    static int k;
    if (!r || i > r->n) return "";
    const char *s = msirec_str(r, i, bufs[k++ & 7]);
    return s ? s : "";
}

static int fi(const MsiRec *r, int i)
{
    int v = r && i <= r->n ? msirec_int(r, i) : MSI_NULL_INTEGER;
    return v == MSI_NULL_INTEGER ? 0 : v;
}

static void copy(char *dst, size_t cap, const char *s) { snprintf(dst, cap, "%s", s ? s : ""); }

static void format(Dlg *d, const char *s, char *out, int cap) { eng_format(d->in, NULL, s ? s : "", out, cap); }

static void get_prop(Dlg *d, const char *name, char *out, int cap) { snprintf(out, (size_t)cap, "%s", eng_get_prop(d->in, name)); }

/* The property a control works on: its Property column, or with the
 * Indirect attribute the property that one names */
static void bound_prop(Win *w, const Ctl *c, char *out, int cap)
{
    if (c->attr & CA_INDIRECT) get_prop(w->d, c->prop, out, cap);
    else copy(out, (size_t)cap, c->prop);
}

static void bound_value(Win *w, const Ctl *c, char *out, int cap)
{
    char p[128];
    bound_prop(w, c, p, sizeof(p));
    if (p[0]) get_prop(w->d, p, out, cap);
    else out[0] = 0;
}

static void set_bound(Win *w, const Ctl *c, const char *value)
{
    char p[128];
    bound_prop(w, c, p, sizeof(p));
    if (p[0]) eng_set_prop(w->d->in, p, value);
}

static bool is(const Ctl *c, const char *type) { return !strcmp(c->type, type); }

static int find_ctl(Win *w, const char *name)
{
    for (int i = 0; i < w->nc; i++) if (!strcmp(w->c[i].name, name)) return i;
    return -1;
}

/* -----------------------------------------------------------------------
 * Fonts and text
 * ----------------------------------------------------------------------- */
static HFONT style_font(Dlg *d, const char *style, COLORREF *color, bool *has_color)
{
    *has_color = false;
    if (!style[0]) style = d->default_style;
    for (int i = 0; i < d->nfonts; i++)
        if (!strcmp(d->fonts[i].name, style)) { *color = d->fonts[i].color; *has_color = d->fonts[i].has_color; return d->fonts[i].f; }
    HFONT f = NULL;
    COLORREF col = 0;
    bool hc = false;
    Rows rs = query(d, "SELECT `FaceName`, `Size`, `Color`, `StyleBits` FROM `TextStyle` WHERE `TextStyle` = ?", style, NULL);
    if (rs.n) {
        WCHAR face[64];
        to_w(fs(rs.r[0], 1), face, 64);
        int size = fi(rs.r[0], 2), bits = fi(rs.r[0], 4);
        if (size <= 0) size = 8;
        if (!msirec_is_null(rs.r[0], 3)) { col = (COLORREF)fi(rs.r[0], 3); hc = true; }
        f = CreateFontW(-SC(size), 0, 0, 0, bits & 1 ? FW_BOLD : FW_NORMAL, bits & 2 ? TRUE : FALSE,
                        bits & 4 ? TRUE : FALSE, bits & 8 ? TRUE : FALSE, DEFAULT_CHARSET, 0 /* OUT_DEFAULT_PRECIS */,
                        0, DEFAULT_QUALITY, 0, face);
    }
    rows_free(&rs);
    if (!f) f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    if (d->nfonts < (int)(sizeof(d->fonts) / sizeof(d->fonts[0]))) {
        copy(d->fonts[d->nfonts].name, sizeof(d->fonts[0].name), style);
        d->fonts[d->nfonts].f = f;
        d->fonts[d->nfonts].color = col;
        d->fonts[d->nfonts].has_color = hc;
        d->nfonts++;
    }
    *color = col;
    *has_color = hc;
    return f;
}

/* "{\Style}{&Style}text": the last style named, and the text */
static const char *parse_style(const char *s, char *style, int cap)
{
    style[0] = 0;
    while (s[0] == '{' && (s[1] == '\\' || s[1] == '&')) {
        const char *e = strchr(s, '}');
        if (!e) break;
        int n = (int)(e - s - 2);
        if (n >= cap) n = cap - 1;
        memcpy(style, s + 2, (size_t)n);
        style[n] = 0;
        s = e + 1;
    }
    return s;
}

/* A label without hyperlink markup */
static void strip_tags(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '<') { char *e = strchr(p, '>'); if (e) { p = e; continue; } }
        *o++ = *p;
    }
    *o = 0;
}

/* Windows-1252's 0x80-0x9F */
static const WCHAR cp1252[32] = {
    0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D, 0x017D, 0x8F,
    0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178 };

static WCHAR ansi_char(unsigned c) { return c >= 0x80 && c < 0xA0 ? cp1252[c - 0x80] : (WCHAR)c; }

typedef struct { WCHAR *s; size_t n, cap; } WBuf;

static void wb_put(WBuf *b, WCHAR c)
{
    if (b->n + 2 > b->cap) { b->cap = b->cap ? b->cap * 2 : 1024; b->s = realloc(b->s, b->cap * sizeof(WCHAR)); }
    b->s[b->n++] = c;
    b->s[b->n] = 0;
}

static void wb_nl(WBuf *b) { wb_put(b, '\r'); wb_put(b, '\n'); }

/* The plain text of a ScrollableText control's RTF (or of plain text),
 * with CRLF line ends for a multi-line edit control */
static WCHAR *rtf_to_text(const char *s)
{
    WBuf b = { NULL, 0, 0 };
    wb_put(&b, 0);
    b.n = 0;
    if (strncmp(s, "{\\rtf", 5)) {
        WCHAR *w = wdup(s);
        for (WCHAR *p = w; *p; p++) {
            if (*p == '\n' && (p == w || p[-1] != '\r')) wb_put(&b, '\r');
            wb_put(&b, *p);
        }
        free(w);
        return b.s;
    }
    static const char *skip_dest[] = { "fonttbl", "colortbl", "stylesheet", "info", "pict", "header", "footer",
        "headerl", "headerr", "footerl", "footerr", "listtable", "listoverridetable", "rsidtbl", "generator",
        "xmlnstbl", "themedata", "colorschememapping", "latentstyles", "datastore", "mmathPr", "pgdsctbl",
        "filetbl", "revtbl", "author", "operator", "title", "object", "fldinst", NULL };
    bool skip[64] = { false };
    int depth = 0, uc = 1, pending_skip = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; ) {
        unsigned char ch = *p;
        if (ch == '{') { if (depth < 63) { skip[depth + 1] = skip[depth]; depth++; } p++; continue; }
        if (ch == '}') { if (depth > 0) depth--; p++; continue; }
        if (ch == '\r' || ch == '\n') { p++; continue; }
        if (ch != '\\') {
            if (pending_skip) pending_skip--;
            else if (!skip[depth]) wb_put(&b, ansi_char(ch));
            p++;
            continue;
        }
        p++;
        if (*p == '\\' || *p == '{' || *p == '}') {
            if (pending_skip) pending_skip--;
            else if (!skip[depth]) wb_put(&b, *p);
            p++;
            continue;
        }
        if (*p == '\'') {
            unsigned v = 0;
            for (int i = 1; i <= 2; i++) {
                char c = (char)p[i];
                v = v * 16 + (unsigned)(c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0);
                if (!p[i]) break;
            }
            p += p[1] && p[2] ? 3 : 1;
            if (pending_skip) pending_skip--;
            else if (!skip[depth]) wb_put(&b, ansi_char(v));
            continue;
        }
        if (*p == '*') { skip[depth] = true; p++; continue; }
        if (*p == '~') { if (!skip[depth]) wb_put(&b, 0xA0); p++; continue; }
        if (*p == '-' || *p == '_') { p++; continue; }
        if (*p == '\r' || *p == '\n') { if (!skip[depth]) wb_nl(&b); p++; continue; }
        char word[32];
        int n = 0;
        while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) { if (n < 31) word[n++] = (char)*p; p++; }
        word[n] = 0;
        bool has_num = false;
        int num = 0, sign = 1;
        if (*p == '-') { sign = -1; p++; }
        while (*p >= '0' && *p <= '9') { num = num * 10 + (*p - '0'); has_num = true; p++; }
        num *= sign;
        if (*p == ' ') p++;
        if (!n) { if (*p) p++; continue; }
        for (int i = 0; skip_dest[i]; i++) if (!strcmp(word, skip_dest[i])) skip[depth] = true;
        if (skip[depth]) continue;
        if (!strcmp(word, "par") || !strcmp(word, "line") || !strcmp(word, "row")) wb_nl(&b);
        else if (!strcmp(word, "tab") || !strcmp(word, "cell")) wb_put(&b, '\t');
        else if (!strcmp(word, "uc") && has_num) uc = num;
        else if (!strcmp(word, "u") && has_num) { wb_put(&b, (WCHAR)(num < 0 ? num + 65536 : num)); pending_skip = uc; }
        else if (!strcmp(word, "emdash")) wb_put(&b, 0x2014);
        else if (!strcmp(word, "endash")) wb_put(&b, 0x2013);
        else if (!strcmp(word, "bullet")) wb_put(&b, 0x2022);
        else if (!strcmp(word, "lquote")) wb_put(&b, 0x2018);
        else if (!strcmp(word, "rquote")) wb_put(&b, 0x2019);
        else if (!strcmp(word, "ldblquote")) wb_put(&b, 0x201C);
        else if (!strcmp(word, "rdblquote")) wb_put(&b, 0x201D);
    }
    return b.s;
}

/* -----------------------------------------------------------------------
 * Pictures: Binary-table bitmaps (BMP, PNG, JPEG) and icons
 * ----------------------------------------------------------------------- */
static bool binary_data(Dlg *d, const char *name, uint8_t **data, size_t *size, Rows *keep)
{
    *keep = query(d, "SELECT `Data` FROM `Binary` WHERE `Name` = ?", name, NULL);
    if (!keep->n || keep->r[0]->f[1].type != MSIF_STREAM || !keep->r[0]->f[1].data) { rows_free(keep); return false; }
    *data = keep->r[0]->f[1].data;
    *size = keep->r[0]->f[1].size;
    return true;
}

static void load_bitmap(Dlg *d, Ctl *c, const char *name)
{
    uint8_t *data;
    size_t size;
    Rows keep;
    if (!binary_data(d, name, &data, &size, &keep)) return;
    int w, h, n;
    unsigned char *px = stbi_load_from_memory(data, (int)size, &w, &h, &n, 4);
    if (px) {
        c->px = malloc((size_t)w * (size_t)h * 4);
        for (int i = 0; i < w * h; i++)
            c->px[i] = (uint32_t)px[i * 4 + 3] << 24 | (uint32_t)px[i * 4] << 16 | (uint32_t)px[i * 4 + 1] << 8 | px[i * 4 + 2];
        c->pw = w;
        c->ph = h;
        stbi_image_free(px);
    }
    rows_free(&keep);
}

static void load_icon(Dlg *d, Ctl *c, const char *name)
{
    uint8_t *data;
    size_t size;
    Rows keep;
    if (!binary_data(d, name, &data, &size, &keep)) return;
    /* an .ico file: the largest image that fits the control */
    if (size >= 6 && data[0] == 0 && data[1] == 0 && data[2] == 1) {
        int count = data[4] | data[5] << 8, best = -1, bestw = 0;
        for (int i = 0; i < count && 6 + (size_t)i * 16 + 16 <= size; i++) {
            const uint8_t *e = data + 6 + i * 16;
            int iw = e[0] ? e[0] : 256;
            bool fits = iw <= (c->w > c->h ? c->w : c->h);
            if (best < 0 || (fits && (iw > bestw || bestw > (c->w > c->h ? c->w : c->h))) || (!fits && iw < bestw && bestw > (c->w > c->h ? c->w : c->h))) {
                best = i;
                bestw = iw;
            }
        }
        if (best >= 0) {
            const uint8_t *e = data + 6 + best * 16;
            DWORD isz = (DWORD)(e[8] | e[9] << 8 | e[10] << 16 | (DWORD)e[11] << 24);
            DWORD off = (DWORD)(e[12] | e[13] << 8 | e[14] << 16 | (DWORD)e[15] << 24);
            if ((size_t)off + isz <= size)
                c->icon = CreateIconFromResourceEx(data + off, isz, TRUE, 0x00030000, c->w, c->h, 0);
        }
    } else {
        load_bitmap(d, c, name);
    }
    rows_free(&keep);
}

/* -----------------------------------------------------------------------
 * Painting the static controls
 * ----------------------------------------------------------------------- */
static void ctl_text(Win *w, const Ctl *c, char *out, int cap)
{
    if (c->override) copy(out, (size_t)cap, c->override);
    else format(w->d, c->body, out, cap);
    if (is(c, "Hyperlink")) strip_tags(out);
}

static void paint_text(HDC dc, Win *w, Ctl *c)
{
    char text[4096];
    ctl_text(w, c, text, sizeof(text));
    WCHAR *wt = wdup(text);
    RECT r = { c->x, c->y, c->x + c->w, c->y + c->h };
    SelectObject(dc, c->font);
    COLORREF col = c->has_color ? c->color : GetSysColor(COLOR_WINDOWTEXT);
    if (!c->enabled) col = GetSysColor(COLOR_GRAYTEXT);
    if (is(c, "Hyperlink")) col = RGB(0, 102, 204);
    SetTextColor(dc, col);
    UINT f = (c->attr & CA_NOWRAP ? DT_SINGLELINE : DT_WORDBREAK) | (c->attr & CA_NOPREFIX ? DT_NOPREFIX : 0) |
             (c->attr & CA_RIGHTALIGN ? DT_RIGHT : 0);
    DrawTextW(dc, wt, -1, &r, f);
    free(wt);
}

static void paint_line(HDC dc, int x0, int y0, int x1, int y1)
{
    HPEN dark = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_BTNSHADOW));
    HPEN light = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_BTNHIGHLIGHT));
    HGDIOBJ old = SelectObject(dc, dark);
    MoveToEx(dc, x0, y0, NULL);
    LineTo(dc, x1, y1);
    SelectObject(dc, light);
    MoveToEx(dc, x0 + (y0 == y1 ? 0 : 1), y0 + (y0 == y1 ? 1 : 0), NULL);
    LineTo(dc, x1 + (y0 == y1 ? 0 : 1), y1 + (y0 == y1 ? 1 : 0));
    SelectObject(dc, old);
    DeleteObject(dark);
    DeleteObject(light);
}

static void paint_group(HDC dc, Win *w, Ctl *c)
{
    char text[512];
    ctl_text(w, c, text, sizeof(text));
    int top = c->y + 7;
    paint_line(dc, c->x, top, c->x + c->w - 2, top);
    paint_line(dc, c->x, c->y + c->h - 2, c->x + c->w - 2, c->y + c->h - 2);
    paint_line(dc, c->x, top, c->x, c->y + c->h - 2);
    paint_line(dc, c->x + c->w - 2, top, c->x + c->w - 2, c->y + c->h - 2);
    if (text[0]) {
        WCHAR *wt = wdup(text);
        SelectObject(dc, c->font);
        SetBkMode(dc, OPAQUE);
        SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        RECT r = { c->x + 8, c->y, c->x + c->w - 8, c->y + 16 };
        DrawTextW(dc, wt, -1, &r, DT_SINGLELINE);
        SetBkMode(dc, TRANSPARENT);
        free(wt);
    }
}

static void paint(Win *w)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(w->hwnd, &ps);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldfont = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
    for (int pass = 0; pass < 3; pass++) {
        for (int i = 0; i < w->nc; i++) {
            Ctl *c = &w->c[i];
            if (c->hwnd || !c->visible) continue;
            if (pass == 0 && is(c, "Bitmap") && c->px) {
                BITMAPINFO bi;
                memset(&bi, 0, sizeof(bi));
                bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
                bi.bmiHeader.biWidth = c->pw;
                bi.bmiHeader.biHeight = -c->ph;
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                bi.bmiHeader.biCompression = BI_RGB;
                StretchDIBits(dc, c->x, c->y, c->w, c->h, 0, 0, c->pw, c->ph, c->px, &bi, DIB_RGB_COLORS, SRCCOPY);
            } else if (pass == 0 && is(c, "Icon")) {
                if (c->icon) DrawIconEx(dc, c->x, c->y, c->icon, c->w, c->h, 0, NULL, DI_NORMAL);
                else if (c->px) {
                    BITMAPINFO bi;
                    memset(&bi, 0, sizeof(bi));
                    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
                    bi.bmiHeader.biWidth = c->pw;
                    bi.bmiHeader.biHeight = -c->ph;
                    bi.bmiHeader.biPlanes = 1;
                    bi.bmiHeader.biBitCount = 32;
                    bi.bmiHeader.biCompression = BI_RGB;
                    StretchDIBits(dc, c->x, c->y, c->w, c->h, 0, 0, c->pw, c->ph, c->px, &bi, DIB_RGB_COLORS, SRCCOPY);
                }
            } else if (pass == 1 && is(c, "Line")) {
                paint_line(dc, c->x, c->y, c->x + c->w, c->y);
            } else if (pass == 1 && (is(c, "GroupBox") || (is(c, "RadioButtonGroup") && (c->attr & CA_HASBORDER)))) {
                paint_group(dc, w, c);
            } else if (pass == 2 && (is(c, "Text") || is(c, "Hyperlink"))) {
                paint_text(dc, w, c);
            }
        }
    }
    SelectObject(dc, oldfont);
    EndPaint(w->hwnd, &ps);
}

static void invalidate_ctl(Win *w, Ctl *c)
{
    if (!w->hwnd) return;
    if (c->hwnd) { InvalidateRect(c->hwnd, NULL, TRUE); return; }
    RECT r = { c->x, c->y, c->x + c->w, c->y + c->h };
    InvalidateRect(w->hwnd, &r, TRUE);
}

/* -----------------------------------------------------------------------
 * Feature tree (SelectionTree)
 * ----------------------------------------------------------------------- */
static bool feature_checked(Dlg *d, const char *f)
{
    int inst = ISTATE_UNKNOWN, act = ISTATE_UNKNOWN;
    if (eng_feature_state(d->in, f, &inst, &act)) return false;
    return act == ISTATE_LOCAL || act == ISTATE_SOURCE || (act == ISTATE_UNKNOWN && inst == ISTATE_LOCAL);
}

static void tree_set_check(Ctl *c, int i, bool on)
{
    TVITEMW it;
    memset(&it, 0, sizeof(it));
    it.mask = TVIF_STATE;
    it.hItem = c->items[i];
    it.stateMask = TVIS_STATEIMAGEMASK;
    SendMessageW(c->hwnd, TVM_GETITEMW, 0, (LPARAM)&it);
    UINT want = INDEXTOSTATEIMAGEMASK(on ? 2 : 1);
    if ((it.state & TVIS_STATEIMAGEMASK) == want) return;
    it.mask = TVIF_STATE;
    it.state = want;
    it.stateMask = TVIS_STATEIMAGEMASK;
    SendMessageW(c->hwnd, TVM_SETITEMW, 0, (LPARAM)&it);
}

static bool tree_get_check(Ctl *c, int i)
{
    TVITEMW it;
    memset(&it, 0, sizeof(it));
    it.mask = TVIF_STATE;
    it.hItem = c->items[i];
    it.stateMask = TVIS_STATEIMAGEMASK;
    SendMessageW(c->hwnd, TVM_GETITEMW, 0, (LPARAM)&it);
    return (it.state & TVIS_STATEIMAGEMASK) >> 12 == 2;
}

static void tree_refresh(Win *w, Ctl *c)
{
    w->d->updating++;
    for (int i = 0; i < c->nitems; i++) tree_set_check(c, i, feature_checked(w->d, c->feat[i]));
    w->d->updating--;
}

static void tree_fill(Win *w, Ctl *c)
{
    Dlg *d = w->d;
    Rows rs = query(d, "SELECT `Feature`, `Feature_Parent`, `Title`, `Description`, `Display`, `Directory_` FROM `Feature` ORDER BY `Display`", NULL, NULL);
    int n = rs.n;
    c->items = calloc((size_t)n + 1, sizeof(HTREEITEM));
    c->feat = calloc((size_t)n + 1, sizeof(char *));
    c->fdesc = calloc((size_t)n + 1, sizeof(char *));
    c->fdir = calloc((size_t)n + 1, sizeof(char *));
    c->fparent = calloc((size_t)n + 1, sizeof(int));
    bool *done = calloc((size_t)n + 1, sizeof(bool));
    int *slot = calloc((size_t)n + 1, sizeof(int));
    for (int i = 0; i < n; i++) slot[i] = -1;
    /* parents before children, in Display order */
    for (bool progress = true; progress; ) {
        progress = false;
        for (int i = 0; i < n; i++) {
            if (done[i]) continue;
            int disp = fi(rs.r[i], 5);
            const char *parent = fs(rs.r[i], 2);
            int pi = -1;
            if (parent[0] && strcmp(parent, fs(rs.r[i], 1))) {
                for (int j = 0; j < n; j++) if (!strcmp(fs(rs.r[j], 1), parent)) { pi = j; break; }
                if (pi >= 0 && !done[pi]) continue;
            }
            done[i] = true;
            progress = true;
            if (!disp || (pi >= 0 && slot[pi] < 0)) continue;   /* hidden, or under a hidden feature */
            int k = c->nitems++;
            slot[i] = k;
            c->feat[k] = xstrdup(fs(rs.r[i], 1));
            c->fdesc[k] = xstrdup(fs(rs.r[i], 4));
            c->fdir[k] = xstrdup(fs(rs.r[i], 6));
            c->fparent[k] = pi >= 0 ? slot[pi] : -1;
            char title[256];
            format(d, fs(rs.r[i], 3), title, sizeof(title));
            WCHAR wt[256];
            to_w(title[0] ? title : c->feat[k], wt, 256);
            TVINSERTSTRUCTW ins;
            memset(&ins, 0, sizeof(ins));
            ins.hParent = pi >= 0 ? c->items[slot[pi]] : TVI_ROOT;
            ins.hInsertAfter = TVI_LAST;
            ins.item.mask = TVIF_TEXT | TVIF_PARAM;
            ins.item.pszText = wt;
            ins.item.lParam = k;
            c->items[k] = (HTREEITEM)SendMessageW(c->hwnd, TVM_INSERTITEMW, 0, (LPARAM)&ins);
        }
    }
    /* odd Display values start expanded */
    for (int i = 0; i < n; i++)
        if (slot[i] >= 0 && (fi(rs.r[i], 5) & 1)) SendMessageW(c->hwnd, TVM_EXPAND, TVE_EXPAND, (LPARAM)c->items[slot[i]]);
    free(done);
    free(slot);
    rows_free(&rs);
    tree_refresh(w, c);
}

static void tree_selected(Win *w, Ctl *c, int k)
{
    Dlg *d = w->d;
    if (k < 0 || k >= c->nitems) return;
    char desc[1024], path[MAX_PATH * 2] = "";
    format(d, c->fdesc[k], desc, sizeof(desc));
    publish(w, "SelectionDescription", desc, 0, 0);
    publish(w, "SelectionSize", "", 0, 0);
    if (c->fdir[k][0]) {
        eng_target_path(d->in, c->fdir[k], path, sizeof(path));
        if (c->prop[0]) eng_set_prop(d->in, c->prop, c->fdir[k]);
    }
    publish(w, "SelectionPath", path, 0, 0);
    publish(w, "SelectionPathOn", NULL, c->fdir[k][0] != 0, 0);
}

/* After a click or a key in the tree: apply check boxes the user changed
 * (a parent left out takes its children along, a child put in its parents) */
static void tree_sync(Win *w, Ctl *c)
{
    Dlg *d = w->d;
    for (int i = 0; i < c->nitems; i++) {
        bool want = tree_get_check(c, i);
        if (want == feature_checked(d, c->feat[i])) continue;
        eng_set_feature_state(d->in, c->feat[i], want ? ISTATE_LOCAL : ISTATE_ABSENT);
        if (want) {
            for (int p = c->fparent[i]; p >= 0; p = c->fparent[p])
                if (!feature_checked(d, c->feat[p])) eng_set_feature_state(d->in, c->feat[p], ISTATE_LOCAL);
        } else {
            for (int j = 0; j < c->nitems; j++) {
                for (int p = c->fparent[j]; p >= 0; p = c->fparent[p])
                    if (p == i) { eng_set_feature_state(d->in, c->feat[j], ISTATE_ABSENT); break; }
            }
        }
    }
    tree_refresh(w, c);
    refresh_all(d);
}

static void set_all_features(Dlg *d, int state)
{
    Rows rs = query(d, "SELECT `Feature` FROM `Feature`", NULL, NULL);
    for (int i = 0; i < rs.n; i++) eng_set_feature_state(d->in, fs(rs.r[i], 1), state);
    rows_free(&rs);
}

static void refresh_trees(Dlg *d)
{
    Win *ws[18];
    int n = 0;
    if (d->modeless) ws[n++] = d->modeless;
    for (int i = 0; i < d->depth; i++) ws[n++] = d->stack[i];
    for (int k = 0; k < n; k++)
        for (int i = 0; i < ws[k]->nc; i++)
            if (is(&ws[k]->c[i], "SelectionTree") && ws[k]->c[i].hwnd) tree_refresh(ws[k], &ws[k]->c[i]);
}

/* -----------------------------------------------------------------------
 * Folder pickers (DirectoryCombo, DirectoryList, VolumeCostList)
 * ----------------------------------------------------------------------- */
static void free_vals(Ctl *c)
{
    for (int i = 0; i < c->nvals; i++) free(c->vals[i]);
    free(c->vals);
    c->vals = NULL;
    c->nvals = 0;
}

static void add_val(Ctl *c, const char *v)
{
    c->vals = realloc(c->vals, (size_t)(c->nvals + 1) * sizeof(char *));
    c->vals[c->nvals++] = xstrdup(v);
}

static void ensure_slash(char *p, size_t cap)
{
    size_t n = strlen(p);
    if (n && p[n - 1] != '\\' && n + 1 < cap) { p[n] = '\\'; p[n + 1] = 0; }
}

static void fill_dir_list(Win *w, Ctl *c, const char *path)
{
    w->d->updating++;
    SendMessageW(c->hwnd, LB_RESETCONTENT, 0, 0);
    free_vals(c);
    char pat[MAX_PATH * 2];
    snprintf(pat, sizeof(pat), "%s", path);
    ensure_slash(pat, sizeof(pat));
    size_t base = strlen(pat);
    strncat(pat, "*", sizeof(pat) - strlen(pat) - 1);
    WCHAR wp[MAX_PATH * 2];
    to_w(pat, wp, MAX_PATH * 2);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wp, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            char name[MAX_PATH];
            to_u8(fd.cFileName, name, sizeof(name));
            char full[MAX_PATH * 2];
            pat[base] = 0;
            snprintf(full, sizeof(full), "%s%s\\", pat, name);
            add_val(c, full);
            SendMessageW(c->hwnd, LB_ADDSTRING, 0, (LPARAM)fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    w->d->updating--;
}

static void fill_dir_combo(Win *w, Ctl *c, const char *path)
{
    w->d->updating++;
    SendMessageW(c->hwnd, CB_RESETCONTENT, 0, 0);
    free_vals(c);
    char acc[MAX_PATH * 2] = "";
    const char *p = path;
    int level = 0;
    while (*p) {
        const char *e = strchr(p, '\\');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (!n) { if (!e) break; p = e + 1; continue; }
        size_t al = strlen(acc);
        if (al + n + 2 >= sizeof(acc)) break;
        memcpy(acc + al, p, n);
        acc[al + n] = '\\';
        acc[al + n + 1] = 0;
        char label[MAX_PATH * 2];
        snprintf(label, sizeof(label), "%*s%.*s", level * 2, "", (int)n, p);
        WCHAR wl[MAX_PATH * 2];
        to_w(level ? label : acc, wl, MAX_PATH * 2);
        SendMessageW(c->hwnd, CB_ADDSTRING, 0, (LPARAM)wl);
        add_val(c, acc);
        level++;
        if (!e) break;
        p = e + 1;
    }
    if (c->nvals) SendMessageW(c->hwnd, CB_SETCURSEL, (WPARAM)(c->nvals - 1), 0);
    w->d->updating--;
}

static void fill_volumes(Win *w, Ctl *c)
{
    (void)w;
    ULARGE_INTEGER avail, total, freeb;
    WCHAR line[128];
    if (GetDiskFreeSpaceExW(L"C:\\", &avail, &total, &freeb))
        swprintf(line, 128, L"C:\\\tAvailable: %u MB of %u MB", (unsigned)(avail.QuadPart >> 20), (unsigned)(total.QuadPart >> 20));
    else
        wcscpy(line, L"C:\\");
    SendMessageW(c->hwnd, LB_ADDSTRING, 0, (LPARAM)line);
}

static int find_type(Win *w, const char *type)
{
    for (int i = 0; i < w->nc; i++) if (is(&w->c[i], type)) return i;
    return -1;
}

static void dir_up(Win *w)
{
    int i = find_type(w, "DirectoryList");
    if (i < 0) i = find_type(w, "PathEdit");
    if (i < 0) return;
    char p[MAX_PATH * 2];
    bound_value(w, &w->c[i], p, sizeof(p));
    size_t n = strlen(p);
    if (n && p[n - 1] == '\\') p[--n] = 0;
    char *s = strrchr(p, '\\');
    if (!s || s - p < 2) return;                                /* "C:" stays */
    s[1] = 0;
    set_bound(w, &w->c[i], p);
}

static void dir_open(Win *w)
{
    int i = find_type(w, "DirectoryList");
    if (i < 0) return;
    Ctl *c = &w->c[i];
    int sel = (int)SendMessageW(c->hwnd, LB_GETCURSEL, 0, 0);
    if (sel >= 0 && sel < c->nvals) set_bound(w, c, c->vals[sel]);
}

static void dir_new(Win *w)
{
    int i = find_type(w, "DirectoryList");
    if (i < 0) return;
    char p[MAX_PATH * 2], np[MAX_PATH * 2];
    bound_value(w, &w->c[i], p, sizeof(p));
    ensure_slash(p, sizeof(p));
    for (int k = 1; k < 100; k++) {
        if (k == 1) snprintf(np, sizeof(np), "%sNew Folder", p);
        else snprintf(np, sizeof(np), "%sNew Folder (%d)", p, k);
        WCHAR wn[MAX_PATH * 2];
        to_w(np, wn, MAX_PATH * 2);
        if (CreateDirectoryW(wn, NULL)) break;
        if (GetLastError() != ERROR_ALREADY_EXISTS) return;
    }
    w->c[i].shown_path[0] = 0;                                  /* refill */
    refresh(w);
}

/* -----------------------------------------------------------------------
 * Values of the interactive controls
 * ----------------------------------------------------------------------- */
static void list_fill(Win *w, Ctl *c, const char *table, bool combo)
{
    char sql[160];
    snprintf(sql, sizeof(sql), "SELECT `Value`, `Text` FROM `%s` WHERE `Property` = ? ORDER BY `Order`", table);
    Rows rs = query(w->d, sql, c->prop, NULL);
    for (int i = 0; i < rs.n; i++) {
        char text[512];
        format(w->d, fs(rs.r[i], 2)[0] ? fs(rs.r[i], 2) : fs(rs.r[i], 1), text, sizeof(text));
        WCHAR wt[512];
        to_w(text, wt, 512);
        LRESULT pos = SendMessageW(c->hwnd, combo ? CB_ADDSTRING : LB_ADDSTRING, 0, (LPARAM)wt);
        SendMessageW(c->hwnd, combo ? CB_SETITEMDATA : LB_SETITEMDATA, (WPARAM)pos, (LPARAM)c->nvals);
        add_val(c, fs(rs.r[i], 1));
    }
    rows_free(&rs);
}

static void list_select(Win *w, Ctl *c, bool combo)
{
    char v[512];
    bound_value(w, c, v, sizeof(v));
    int count = (int)SendMessageW(c->hwnd, combo ? CB_GETCOUNT : LB_GETCOUNT, 0, 0);
    for (int i = 0; i < count; i++) {
        int k = (int)SendMessageW(c->hwnd, combo ? CB_GETITEMDATA : LB_GETITEMDATA, (WPARAM)i, 0);
        if (k >= 0 && k < c->nvals && !strcmp(c->vals[k], v)) {
            if ((int)SendMessageW(c->hwnd, combo ? CB_GETCURSEL : LB_GETCURSEL, 0, 0) != i)
                SendMessageW(c->hwnd, combo ? CB_SETCURSEL : LB_SETCURSEL, (WPARAM)i, 0);
            return;
        }
    }
    if (combo && !(c->attr & CA_COMBOLIST)) {                   /* an editable combo shows any value */
        WCHAR wv[512];
        to_w(v, wv, 512);
        SetWindowTextW(c->hwnd, wv);
    }
}

static void set_window_text_if(HWND h, const char *u8)
{
    WCHAR *now = NULL;
    int n = GetWindowTextLengthW(h);
    now = malloc((size_t)(n + 1) * sizeof(WCHAR));
    GetWindowTextW(h, now, n + 1);
    WCHAR *want = wdup(u8);
    if (wcscmp(now, want)) SetWindowTextW(h, want);
    free(now);
    free(want);
}

static void update_values(Win *w)
{
    Dlg *d = w->d;
    d->updating++;
    for (int i = 0; i < w->nc; i++) {
        Ctl *c = &w->c[i];
        char v[MAX_PATH * 2];
        if (is(c, "CheckBox")) {
            bound_value(w, c, v, sizeof(v));
            SendMessageW(c->hwnd, BM_SETCHECK, v[0] ? BST_CHECKED : BST_UNCHECKED, 0);
        } else if (is(c, "Edit") || is(c, "PathEdit") || is(c, "MaskedEdit")) {
            if (!c->prop[0]) continue;
            bound_value(w, c, v, sizeof(v));
            if (GetFocus() != c->hwnd || is(c, "PathEdit")) set_window_text_if(c->hwnd, v);
        } else if (is(c, "RadioButtonGroup")) {
            bound_value(w, c, v, sizeof(v));
            for (int k = 0; k < c->nradio; k++)
                SendMessageW(c->radios[k], BM_SETCHECK, !strcmp(c->rvals[k], v) ? BST_CHECKED : BST_UNCHECKED, 0);
        } else if (is(c, "ListBox") || is(c, "ListView")) {
            list_select(w, c, false);
        } else if (is(c, "ComboBox")) {
            list_select(w, c, true);
        } else if (is(c, "DirectoryList") || is(c, "DirectoryCombo")) {
            bound_value(w, c, v, sizeof(v));
            if (strcmp(v, c->shown_path)) {
                copy(c->shown_path, sizeof(c->shown_path), v);
                if (is(c, "DirectoryList")) fill_dir_list(w, c, v);
                else fill_dir_combo(w, c, v);
            }
        }
    }
    d->updating--;
}

/* Visibility, enabled state and the default button from the attributes,
 * the conditions and the events */
static void refresh(Win *w)
{
    if (!w->ready) return;
    Dlg *d = w->d;
    bool *vis = calloc((size_t)w->nc + 1, sizeof(bool)), *en = calloc((size_t)w->nc + 1, sizeof(bool));
    for (int i = 0; i < w->nc; i++) { vis[i] = (w->c[i].attr & CA_VISIBLE) != 0; en[i] = (w->c[i].attr & CA_ENABLED) != 0; }
    int def = w->def;
    for (int k = 0; k < w->nconds; k++) {
        CtlCond *cc = &w->conds[k];
        if (!eng_condition(d->in, cc->cond)) continue;
        if (!strcmp(cc->action, "Hide")) vis[cc->ctl] = false;
        else if (!strcmp(cc->action, "Show")) vis[cc->ctl] = true;
        else if (!strcmp(cc->action, "Disable")) en[cc->ctl] = false;
        else if (!strcmp(cc->action, "Enable")) en[cc->ctl] = true;
        else if (!strcmp(cc->action, "Default")) def = cc->ctl;
    }
    bool repaint = false;
    for (int i = 0; i < w->nc; i++) {
        Ctl *c = &w->c[i];
        if (c->ev_visible >= 0) vis[i] = c->ev_visible != 0;
        if (c->ev_enabled >= 0) en[i] = c->ev_enabled != 0;
        if (c->hwnd) {
            if (vis[i] != c->visible) ShowWindow(c->hwnd, vis[i] ? SW_SHOWNA : SW_HIDE);
            if (en[i] != c->enabled) EnableWindow(c->hwnd, en[i]);
            for (int k = 0; k < c->nradio; k++) {
                if (vis[i] != c->visible) ShowWindow(c->radios[k], vis[i] ? SW_SHOWNA : SW_HIDE);
                if (en[i] != c->enabled) EnableWindow(c->radios[k], en[i]);
            }
        } else if (vis[i] != c->visible || en[i] != c->enabled) {
            repaint = true;
        }
        c->visible = vis[i];
        c->enabled = en[i];
    }
    if (def != w->def && def >= 0 && w->c[def].hwnd && is(&w->c[def], "PushButton")) {
        if (w->def >= 0 && w->c[w->def].hwnd) SendMessageW(w->c[w->def].hwnd, BM_SETSTYLE, BS_PUSHBUTTON, TRUE);
        SendMessageW(w->c[def].hwnd, BM_SETSTYLE, BS_DEFPUSHBUTTON, TRUE);
        w->def = def;
    }
    free(vis);
    free(en);
    update_values(w);
    /* painted text may show properties ([ProductName], a path...) */
    (void)repaint;
    InvalidateRect(w->hwnd, NULL, TRUE);
}

static void refresh_all(Dlg *d)
{
    if (d->refreshing) return;
    d->refreshing = true;
    if (d->modeless) refresh(d->modeless);
    for (int i = 0; i < d->depth; i++) refresh(d->stack[i]);
    d->refreshing = false;
}

/* An event published to the controls of @w subscribed to it */
static void publish(Win *w, const char *event, const char *text, int a, int b)
{
    for (int k = 0; k < w->nsubs; k++) {
        Sub *s = &w->subs[k];
        if (strcmp(s->event, event)) continue;
        Ctl *c = &w->c[s->ctl];
        if (!strcmp(s->attr, "Text")) {
            free(c->override);
            c->override = xstrdup(text);
            if (c->hwnd) { WCHAR *wt = wdup(text); SetWindowTextW(c->hwnd, wt); free(wt); }
            else invalidate_ctl(w, c);
        } else if (!strcmp(s->attr, "Progress") && c->hwnd) {
            int pos = b > 0 ? (int)((long long)a * 1000 / b) : 0;
            if (pos > 1000) pos = 1000;
            if (pos < 0) pos = 0;
            SendMessageW(c->hwnd, PBM_SETPOS, (WPARAM)pos, 0);
        } else if (!strcmp(s->attr, "Visible") || !strcmp(s->attr, "Enabled")) {
            int v = text ? atoi(text) : a;
            if (s->attr[0] == 'V') c->ev_visible = v != 0;
            else c->ev_enabled = v != 0;
            refresh(w);
        }
    }
}

/* -----------------------------------------------------------------------
 * Control events (what pushing a button does)
 * ----------------------------------------------------------------------- */
static void snapshot(Win *w)
{
    Dlg *d = w->d;
    w->snap_name = calloc((size_t)w->nc + 1, sizeof(char *));
    w->snap_val = calloc((size_t)w->nc + 1, sizeof(char *));
    for (int i = 0; i < w->nc; i++) {
        char p[128], v[MAX_PATH * 2];
        bound_prop(w, &w->c[i], p, sizeof(p));
        if (!p[0]) continue;
        get_prop(d, p, v, sizeof(v));
        w->snap_name[w->nsnap] = xstrdup(p);
        w->snap_val[w->nsnap] = xstrdup(v);
        w->nsnap++;
    }
    if (find_type(w, "SelectionTree") >= 0) {
        Rows rs = query(d, "SELECT `Feature` FROM `Feature`", NULL, NULL);
        w->snap_feat = calloc((size_t)rs.n + 1, sizeof(char *));
        w->snap_state = calloc((size_t)rs.n + 1, sizeof(int));
        for (int i = 0; i < rs.n; i++) {
            w->snap_feat[i] = xstrdup(fs(rs.r[i], 1));
            w->snap_state[i] = feature_checked(d, w->snap_feat[i]) ? ISTATE_LOCAL : ISTATE_ABSENT;
        }
        w->nsnapf = rs.n;
        rows_free(&rs);
    }
}

static void reset(Win *w)
{
    Dlg *d = w->d;
    for (int i = 0; i < w->nsnap; i++) eng_set_prop(d->in, w->snap_name[i], w->snap_val[i]);
    for (int i = 0; i < w->nsnapf; i++) eng_set_feature_state(d->in, w->snap_feat[i], w->snap_state[i]);
    refresh_trees(d);
}

/* Exit from a dialog the modeless one spawned cancels the installation */
static void end_with(Win *w, int how)
{
    if (w == w->d->modeless) { if (how == END_EXIT) w->d->cancelled = true; return; }
    w->end = how;
}

static void do_event(Win *w, const char *ev, const char *arg)
{
    Dlg *d = w->d;
    size_t el = strlen(ev);
    eng_log(d->in, "Dialog %s: event %s(%s)", w->name, ev, arg);
    if (ev[0] == '[' && el > 2 && ev[el - 1] == ']') {
        char name[128];
        snprintf(name, sizeof(name), "%.*s", (int)(el - 2), ev + 1);
        eng_set_prop(d->in, name, !strcmp(arg, "{}") ? "" : arg);
    } else if (!strcmp(ev, "NewDialog")) {
        if (w == d->modeless) { run_modal(d, arg, NULL); return; }
        w->end = END_NEW;
        copy(w->next_dlg, sizeof(w->next_dlg), arg);
    } else if (!strcmp(ev, "SpawnDialog") || !strcmp(ev, "SelectionBrowse")) {
        if (run_modal(d, arg, w) == END_EXIT) end_with(w, END_EXIT);
    } else if (!strcmp(ev, "EndDialog")) {
        end_with(w, !strcmp(arg, "Exit") ? END_EXIT : END_RETURN);
    } else if (!strcmp(ev, "DoAction")) {
        d->busy++;
        eng_do_action(d->in, arg);
        d->busy--;
    } else if (!strcmp(ev, "SetTargetPath")) {
        char path[MAX_PATH * 2];
        get_prop(d, arg, path, sizeof(path));
        if (path[0]) eng_set_target_path(d->in, arg, path);
    } else if (!strcmp(ev, "Reset")) {
        reset(w);
    } else if (!strcmp(ev, "AddLocal") || !strcmp(ev, "AddSource")) {
        if (!strcmp(arg, "ALL")) set_all_features(d, ISTATE_LOCAL);
        else eng_set_feature_state(d->in, arg, ISTATE_LOCAL);
        refresh_trees(d);
    } else if (!strcmp(ev, "Remove")) {
        if (!strcmp(arg, "ALL")) { eng_set_prop(d->in, "REMOVE", "ALL"); set_all_features(d, ISTATE_ABSENT); }
        else eng_set_feature_state(d->in, arg, ISTATE_ABSENT);
        refresh_trees(d);
    } else if (!strcmp(ev, "Reinstall")) {
        eng_set_prop(d->in, "REINSTALL", arg);
    } else if (!strcmp(ev, "ReinstallMode")) {
        eng_set_prop(d->in, "REINSTALLMODE", arg);
    } else if (!strcmp(ev, "SetInstallLevel")) {
        eng_set_install_level(d->in, atoi(arg));
        refresh_trees(d);
    } else if (!strcmp(ev, "DirectoryListUp")) {
        dir_up(w);
    } else if (!strcmp(ev, "DirectoryListNew")) {
        dir_new(w);
    } else if (!strcmp(ev, "DirectoryListOpen")) {
        dir_open(w);
    }
    /* EnableRollback, CheckTargetPath, CheckExistingTargetPath,
     * SpawnWaitDialog, ValidateProductID and the rest need nothing here */
}

static void press(Win *w, int idx)
{
    Dlg *d = w->d;
    if (d->busy || idx < 0 || idx >= w->nc) return;
    Ctl *c = &w->c[idx];
    if (!c->visible || !c->enabled) return;
    char name[64];
    copy(name, sizeof(name), c->name);
    Rows rs = query(d, "SELECT `Event`, `Argument`, `Condition` FROM `ControlEvent` WHERE `Dialog_` = ? AND `Control_` = ? ORDER BY `Ordering`",
                    w->name, name);
    for (int i = 0; i < rs.n && !w->end; i++) {
        char ev[64], arg[2048];
        copy(ev, sizeof(ev), fs(rs.r[i], 1));
        if (!strcmp(ev, "SpawnWaitDialog")) continue;           /* its condition is what it waits for */
        const char *cnd = fs(rs.r[i], 3);
        if (cnd[0] && !eng_condition(d->in, cnd)) continue;
        format(d, fs(rs.r[i], 2), arg, sizeof(arg));
        do_event(w, ev, arg);
    }
    rows_free(&rs);
    refresh_all(d);
}

/* -----------------------------------------------------------------------
 * The dialog window
 * ----------------------------------------------------------------------- */
static int ctl_of_id(Win *w, int id, int *radio)
{
    *radio = -1;
    if (id >= ID_RADIO) { *radio = (id - ID_RADIO) % 64; return (id - ID_RADIO) / 64; }
    return id - ID_BASE;
}

static void on_command(Win *w, int id, int code, HWND from)
{
    Dlg *d = w->d;
    (void)from;
    if (id == IDOK) { if (w->def >= 0) press(w, w->def); return; }
    if (id == IDCANCEL) { if (w->cancel >= 0) press(w, w->cancel); return; }
    int radio, i = ctl_of_id(w, id, &radio);
    if (i < 0 || i >= w->nc) return;
    Ctl *c = &w->c[i];
    if (d->updating) return;
    if (is(c, "PushButton") && code == BN_CLICKED) {
        press(w, i);
    } else if (is(c, "CheckBox") && code == BN_CLICKED) {
        bool on = SendMessageW(c->hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
        char val[256] = "1";
        if (on) {
            Rows rs = query(d, "SELECT `Value` FROM `CheckBox` WHERE `Property` = ?", c->prop, NULL);
            if (rs.n && fs(rs.r[0], 1)[0]) format(d, fs(rs.r[0], 1), val, sizeof(val));
            rows_free(&rs);
        }
        set_bound(w, c, on ? val : "");
        press(w, i);
    } else if (is(c, "RadioButtonGroup") && code == BN_CLICKED && radio >= 0 && radio < c->nradio) {
        set_bound(w, c, c->rvals[radio]);
    } else if ((is(c, "Edit") || is(c, "PathEdit") || is(c, "MaskedEdit")) && code == EN_CHANGE) {
        int n = GetWindowTextLengthW(c->hwnd);
        WCHAR *t = malloc((size_t)(n + 1) * sizeof(WCHAR));
        GetWindowTextW(c->hwnd, t, n + 1);
        char *u = malloc((size_t)n * 3 + 1);
        to_u8(t, u, n * 3 + 1);
        set_bound(w, c, u);
        free(t);
        free(u);
    } else if ((is(c, "ListBox") || is(c, "ListView")) && code == LBN_SELCHANGE) {
        int sel = (int)SendMessageW(c->hwnd, LB_GETCURSEL, 0, 0);
        int k = sel >= 0 ? (int)SendMessageW(c->hwnd, LB_GETITEMDATA, (WPARAM)sel, 0) : -1;
        if (k >= 0 && k < c->nvals) set_bound(w, c, c->vals[k]);
    } else if (is(c, "ComboBox") && code == CBN_SELCHANGE) {
        int sel = (int)SendMessageW(c->hwnd, CB_GETCURSEL, 0, 0);
        int k = sel >= 0 ? (int)SendMessageW(c->hwnd, CB_GETITEMDATA, (WPARAM)sel, 0) : -1;
        if (k >= 0 && k < c->nvals) set_bound(w, c, c->vals[k]);
    } else if (is(c, "ComboBox") && code == CBN_EDITCHANGE) {
        WCHAR t[512];
        char u[1024];
        GetWindowTextW(c->hwnd, t, 512);
        to_u8(t, u, sizeof(u));
        set_bound(w, c, u);
    } else if (is(c, "DirectoryList") && code == LBN_DBLCLK) {
        int sel = (int)SendMessageW(c->hwnd, LB_GETCURSEL, 0, 0);
        if (sel >= 0 && sel < c->nvals) set_bound(w, c, c->vals[sel]);
    } else if (is(c, "DirectoryCombo") && code == CBN_SELCHANGE) {
        int sel = (int)SendMessageW(c->hwnd, CB_GETCURSEL, 0, 0);
        if (sel >= 0 && sel < c->nvals) set_bound(w, c, c->vals[sel]);
    }
}

static void on_notify(Win *w, NMHDR *nm)
{
    int radio, i = ctl_of_id(w, (int)nm->idFrom, &radio);
    if (i < 0 || i >= w->nc || !is(&w->c[i], "SelectionTree")) return;
    Ctl *c = &w->c[i];
    if (nm->code == TVN_SELCHANGEDW) {
        NMTREEVIEWW *tv = (NMTREEVIEWW *)nm;
        tree_selected(w, c, (int)tv->itemNew.lParam);
    } else if (nm->code == NM_CLICK || nm->code == TVN_KEYDOWN || nm->code == TVN_ITEMCHANGEDW) {
        if (!w->d->updating) PostMessageW(w->hwnd, WM_TREESYNC, (WPARAM)i, 0);
    }
}

static LRESULT CALLBACK win_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    Win *w = (Win *)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!w) return DefWindowProcW(h, m, wp, lp);
    switch (m) {
    case WM_PAINT:
        paint(w);
        return 0;
    case WM_COMMAND:
        on_command(w, LOWORD(wp), HIWORD(wp), (HWND)lp);
        return 0;
    case WM_NOTIFY:
        on_notify(w, (NMHDR *)lp);
        return 0;
    case WM_TREESYNC:
        if ((int)wp < w->nc && w->c[wp].hwnd) tree_sync(w, &w->c[wp]);
        return 0;
    case WM_CLOSE:
        if (w->cancel >= 0) press(w, w->cancel);
        else if (w != w->d->modeless) end_with(w, END_EXIT);
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
        for (int i = 0; i < w->nc; i++)
            if (w->c[i].hwnd == (HWND)lp && is(&w->c[i], "ScrollableText")) {
                SetBkColor((HDC)wp, RGB(255, 255, 255));
                SetTextColor((HDC)wp, GetSysColor(COLOR_WINDOWTEXT));
                return (LRESULT)w->d->white;
            }
        break;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static HWND child(Win *w, const WCHAR *cls, const WCHAR *text, DWORD style, DWORD ex, int x, int y, int cx, int cy, int id, HFONT font)
{
    HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | style, x, y, cx, cy, w->hwnd, (HMENU)(UINT_PTR)id,
                             GetModuleHandleW(NULL), NULL);
    if (h) SendMessageW(h, WM_SETFONT, (WPARAM)font, 0);
    return h;
}

static void create_control(Win *w, int i)
{
    Dlg *d = w->d;
    Ctl *c = &w->c[i];
    int id = ID_BASE + i;
    DWORD vis = (c->attr & CA_VISIBLE) ? WS_VISIBLE : 0;
    DWORD dis = (c->attr & CA_ENABLED) ? 0 : WS_DISABLED;
    DWORD border = (c->attr & CA_SUNKEN) ? WS_EX_CLIENTEDGE : 0;
    char text[2048];
    if (is(c, "PushButton")) {
        if (c->attr & (CA_BITMAP | CA_ICON)) {
            /* a picture button: its tooltip names it */
            copy(text, sizeof(text), c->help);
            char *bar = strchr(text, '|');
            if (bar) *bar = 0;
            if (!text[0]) copy(text, sizeof(text), "...");
        } else {
            format(d, c->body, text, sizeof(text));
        }
        WCHAR *wt = wdup(text);
        c->hwnd = child(w, L"BUTTON", wt, vis | dis | WS_TABSTOP | (i == w->def ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON),
                        0, c->x, c->y, c->w, c->h, id, c->font);
        free(wt);
    } else if (is(c, "CheckBox")) {
        format(d, c->body, text, sizeof(text));
        WCHAR *wt = wdup(text);
        c->hwnd = child(w, L"BUTTON", wt, vis | dis | WS_TABSTOP | BS_AUTOCHECKBOX | (c->attr & CA_PUSHLIKE ? BS_PUSHLIKE : 0),
                        0, c->x, c->y, c->w, c->h, id, c->font);
        free(wt);
    } else if (is(c, "Edit") || is(c, "PathEdit") || is(c, "MaskedEdit")) {
        DWORD st = vis | dis | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL;
        if (is(c, "Edit") && (c->attr & CA_MULTILINE)) st = (st & ~ES_AUTOHSCROLL) | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL;
        if (c->attr & CA_PASSWORD) st |= ES_PASSWORD;
        c->hwnd = child(w, L"EDIT", L"", st, 0, c->x, c->y, c->w, c->h, id, c->font);
        /* "{80}": the longest value */
        const char *t = c->text;
        if (is(c, "Edit") && t[0] == '{' && t[1] >= '0' && t[1] <= '9')
            SendMessageW(c->hwnd, EM_LIMITTEXT, (WPARAM)atoi(t + 1), 0);
    } else if (is(c, "ScrollableText")) {
        WCHAR *wt = rtf_to_text(c->text);
        c->hwnd = child(w, L"EDIT", wt, vis | dis | WS_TABSTOP | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                        0, c->x, c->y, c->w, c->h, id, c->font);
        free(wt);
    } else if (is(c, "ProgressBar")) {
        c->hwnd = child(w, PROGRESS_CLASSW, L"", vis | PBS_SMOOTH, 0, c->x, c->y, c->w, c->h, id, c->font);
        SendMessageW(c->hwnd, PBM_SETRANGE32, 0, 1000);
    } else if (is(c, "RadioButtonGroup")) {
        Rows rs = query(d, "SELECT `Value`, `X`, `Y`, `Width`, `Height`, `Text` FROM `RadioButton` WHERE `Property` = ? ORDER BY `Order`", c->prop, NULL);
        c->radios = calloc((size_t)rs.n + 1, sizeof(HWND));
        c->rvals = calloc((size_t)rs.n + 1, sizeof(char *));
        for (int k = 0; k < rs.n && k < 64; k++) {
            format(d, fs(rs.r[k], 6), text, sizeof(text));
            char style[72];
            const char *body = parse_style(text, style, sizeof(style));
            WCHAR *wt = wdup(body);
            c->radios[k] = child(w, L"BUTTON", wt, vis | dis | BS_AUTORADIOBUTTON | (k == 0 ? WS_GROUP | WS_TABSTOP : 0), 0,
                                 c->x + SC(fi(rs.r[k], 2)), c->y + SC(fi(rs.r[k], 3)), SC(fi(rs.r[k], 4)), SC(fi(rs.r[k], 5)),
                                 ID_RADIO + i * 64 + k, c->font);
            free(wt);
            c->rvals[k] = xstrdup(fs(rs.r[k], 1));
            c->nradio++;
        }
        rows_free(&rs);
        if (c->nradio) c->hwnd = c->radios[0];
    } else if (is(c, "SelectionTree")) {
        c->hwnd = child(w, WC_TREEVIEWW, L"", vis | dis | WS_TABSTOP | WS_BORDER | TVS_HASLINES | TVS_HASBUTTONS |
                        TVS_LINESATROOT | TVS_CHECKBOXES | TVS_SHOWSELALWAYS, border, c->x, c->y, c->w, c->h, id, c->font);
        tree_fill(w, c);
    } else if (is(c, "DirectoryCombo")) {
        c->hwnd = child(w, L"COMBOBOX", L"", vis | dis | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 0,
                        c->x, c->y, c->w, c->h * 8, id, c->font);
    } else if (is(c, "DirectoryList")) {
        c->hwnd = child(w, L"LISTBOX", L"", vis | dis | WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY, 0,
                        c->x, c->y, c->w, c->h, id, c->font);
    } else if (is(c, "VolumeCostList")) {
        c->hwnd = child(w, L"LISTBOX", L"", vis | dis | WS_BORDER | WS_VSCROLL, 0, c->x, c->y, c->w, c->h, id, c->font);
        fill_volumes(w, c);
    } else if (is(c, "ListBox") || is(c, "ListView")) {
        c->hwnd = child(w, L"LISTBOX", L"", vis | dis | WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY |
                        (c->attr & CA_SORTED ? LBS_SORT : 0), 0, c->x, c->y, c->w, c->h, id, c->font);
        list_fill(w, c, is(c, "ListBox") ? "ListBox" : "ListView", false);
    } else if (is(c, "ComboBox")) {
        c->hwnd = child(w, L"COMBOBOX", L"", vis | dis | WS_TABSTOP | WS_VSCROLL | (c->attr & CA_COMBOLIST ? CBS_DROPDOWNLIST : CBS_DROPDOWN) |
                        (c->attr & CA_SORTED ? CBS_SORT : 0), 0, c->x, c->y, c->w, c->h * 8, id, c->font);
        list_fill(w, c, "ComboBox", true);
    } else if (is(c, "Bitmap")) {
        format(d, c->text, text, sizeof(text));
        load_bitmap(d, c, text);
    } else if (is(c, "Icon")) {
        format(d, c->text, text, sizeof(text));
        load_icon(d, c, text);
    }
    /* Text, Hyperlink, Line, GroupBox, Billboard: painted */
}

static void win_destroy(Win *w)
{
    if (!w) return;
    if (w->hwnd) { SetWindowLongPtrW(w->hwnd, GWLP_USERDATA, 0); DestroyWindow(w->hwnd); }
    for (int i = 0; i < w->nc; i++) {
        Ctl *c = &w->c[i];
        free(c->text); free(c->help); free(c->override); free(c->body); free(c->px);
        if (c->icon) DestroyIcon(c->icon);
        for (int k = 0; k < c->nradio; k++) free(c->rvals[k]);
        free(c->radios); free(c->rvals);
        for (int k = 0; k < c->nitems; k++) { free(c->feat[k]); free(c->fdesc[k]); free(c->fdir[k]); }
        free(c->items); free(c->feat); free(c->fdesc); free(c->fdir); free(c->fparent);
        free_vals(c);
    }
    free(w->c);
    for (int k = 0; k < w->nconds; k++) free(w->conds[k].cond);
    free(w->conds);
    free(w->subs);
    for (int k = 0; k < w->nsnap; k++) { free(w->snap_name[k]); free(w->snap_val[k]); }
    free(w->snap_name); free(w->snap_val);
    for (int k = 0; k < w->nsnapf; k++) free(w->snap_feat[k]);
    free(w->snap_feat); free(w->snap_state);
    free(w);
}

static Win *win_create(Dlg *d, const char *name, Win *parent)
{
    Rows dr = query(d, "SELECT `HCentering`, `VCentering`, `Width`, `Height`, `Attributes`, `Title`, `Control_First`, `Control_Default`, `Control_Cancel` FROM `Dialog` WHERE `Dialog` = ?",
                    name, NULL);
    if (!dr.n) { rows_free(&dr); eng_log(d->in, "Dialog %s not found", name); return NULL; }
    Win *w = calloc(1, sizeof(Win));
    w->d = d;
    w->parent = parent;
    copy(w->name, sizeof(w->name), name);
    w->attr = (unsigned)fi(dr.r[0], 5);
    w->def = w->cancel = w->first = -1;
    int hc = fi(dr.r[0], 1), vc = fi(dr.r[0], 2), cw = SC(fi(dr.r[0], 3)), ch = SC(fi(dr.r[0], 4));
    char title[512], first[64], def[64], cancel[64];
    format(d, fs(dr.r[0], 6), title, sizeof(title));
    copy(first, sizeof(first), fs(dr.r[0], 7));
    copy(def, sizeof(def), fs(dr.r[0], 8));
    copy(cancel, sizeof(cancel), fs(dr.r[0], 9));
    rows_free(&dr);

    Rows cr = query(d, "SELECT `Control`, `Type`, `X`, `Y`, `Width`, `Height`, `Attributes`, `Property`, `Text`, `Control_Next`, `Help` FROM `Control` WHERE `Dialog_` = ?",
                    name, NULL);
    w->c = calloc((size_t)cr.n + 1, sizeof(Ctl));
    for (int i = 0; i < cr.n; i++) {
        Ctl *c = &w->c[w->nc++];
        copy(c->name, sizeof(c->name), fs(cr.r[i], 1));
        copy(c->type, sizeof(c->type), fs(cr.r[i], 2));
        c->x = SC(fi(cr.r[i], 3));
        c->y = SC(fi(cr.r[i], 4));
        c->w = SC(fi(cr.r[i], 5));
        c->h = SC(fi(cr.r[i], 6));
        if (is(c, "Line") && !c->h) c->h = 2;
        c->attr = (unsigned)fi(cr.r[i], 7);
        copy(c->prop, sizeof(c->prop), fs(cr.r[i], 8));
        c->text = xstrdup(fs(cr.r[i], 9));
        copy(c->next, sizeof(c->next), fs(cr.r[i], 10));
        c->help = xstrdup(fs(cr.r[i], 11));
        c->visible = (c->attr & CA_VISIBLE) != 0;
        c->enabled = (c->attr & CA_ENABLED) != 0;
        c->ev_visible = c->ev_enabled = -1;
        char style[72];
        const char *body = parse_style(c->text, style, sizeof(style));
        c->body = xstrdup(body);
        c->font = style_font(d, style, &c->color, &c->has_color);
    }
    rows_free(&cr);
    w->first = find_ctl(w, first);
    w->def = find_ctl(w, def);
    w->cancel = find_ctl(w, cancel);

    Rows cc = query(d, "SELECT `Control_`, `Action`, `Condition` FROM `ControlCondition` WHERE `Dialog_` = ?", name, NULL);
    w->conds = calloc((size_t)cc.n + 1, sizeof(CtlCond));
    for (int i = 0; i < cc.n; i++) {
        int k = find_ctl(w, fs(cc.r[i], 1));
        if (k < 0) continue;
        CtlCond *x = &w->conds[w->nconds++];
        x->ctl = k;
        copy(x->action, sizeof(x->action), fs(cc.r[i], 2));
        x->cond = xstrdup(fs(cc.r[i], 3));
    }
    rows_free(&cc);
    Rows em = query(d, "SELECT `Control_`, `Event`, `Attribute` FROM `EventMapping` WHERE `Dialog_` = ?", name, NULL);
    w->subs = calloc((size_t)em.n + 1, sizeof(Sub));
    for (int i = 0; i < em.n; i++) {
        int k = find_ctl(w, fs(em.r[i], 1));
        if (k < 0) continue;
        Sub *s = &w->subs[w->nsubs++];
        s->ctl = k;
        copy(s->event, sizeof(s->event), fs(em.r[i], 2));
        copy(s->attr, sizeof(s->attr), fs(em.r[i], 3));
    }
    rows_free(&em);

    /* the window: client area of the dialog's size, centred as it asks */
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN | (w->attr & DA_MINIMIZE ? WS_MINIMIZEBOX : 0);
    DWORD ex = WS_EX_CONTROLPARENT | WS_EX_DLGMODALFRAME;
    RECT r = { 0, 0, cw, ch };
    AdjustWindowRectEx(&r, style, FALSE, ex);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);
    int x = sx > ww ? (sx - ww) * hc / 100 : 0, y = sy > wh ? (sy - wh) * vc / 100 : 0;
    WCHAR *wt = wdup(title);
    w->hwnd = CreateWindowExW(ex, L"NovaMsiDialog", wt, style, x, y, ww, wh, parent ? parent->hwnd : NULL, NULL,
                              GetModuleHandleW(NULL), NULL);
    free(wt);
    if (!w->hwnd) { eng_log(d->in, "Dialog %s: cannot create its window", name); win_destroy(w); return NULL; }
    SetWindowLongPtrW(w->hwnd, GWLP_USERDATA, (LONG_PTR)w);
    d->updating++;
    for (int i = 0; i < w->nc; i++) create_control(w, i);
    /* tab order: the Control_Next chain from Control_First */
    if (w->first >= 0) {
        HWND prev = HWND_TOP;
        int k = w->first;
        for (int n = 0; k >= 0 && n < w->nc; n++) {
            Ctl *c = &w->c[k];
            for (int j = 0; j < (c->nradio ? c->nradio : 1); j++) {
                HWND h = c->nradio ? c->radios[j] : c->hwnd;
                if (!h) continue;
                SetWindowPos(h, prev, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                prev = h;
            }
            k = c->next[0] ? find_ctl(w, c->next) : -1;
            if (k == w->first) break;
        }
    }
    d->updating--;
    snapshot(w);
    w->ready = true;
    refresh(w);
    if (w->attr & DA_VISIBLE) {
        ShowWindow(w->hwnd, SW_SHOW);
        UpdateWindow(w->hwnd);
        if (w->first >= 0 && w->c[w->first].hwnd && w->c[w->first].visible && w->c[w->first].enabled) SetFocus(w->c[w->first].hwnd);
        else if (w->def >= 0 && w->c[w->def].hwnd) SetFocus(w->c[w->def].hwnd);
    }
    /* the first item's description in a feature tree */
    for (int i = 0; i < w->nc; i++)
        if (is(&w->c[i], "SelectionTree") && w->c[i].nitems) {
            TreeView_SelectItem(w->c[i].hwnd, w->c[i].items[0]);
            tree_selected(w, &w->c[i], 0);
        } else if (is(&w->c[i], "SelectionTree")) {
            publish(w, "SelectionNoItems", NULL, 0, 0);
        }
    eng_log(d->in, "Dialog %s shown", name);
    return w;
}

static void pump_messages(Dlg *d, Win *modal)
{
    MSG m;
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
        HWND top = modal ? modal->hwnd : d->modeless ? d->modeless->hwnd : NULL;
        if (top && IsDialogMessageW(top, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

static int run_modal(Dlg *d, const char *name, Win *parent)
{
    char cur[72];
    copy(cur, sizeof(cur), name);
    for (;;) {
        if (d->depth >= (int)(sizeof(d->stack) / sizeof(d->stack[0]))) return END_RETURN;
        Win *w = win_create(d, cur, parent);
        if (!w) return END_RETURN;
        if (parent) EnableWindow(parent->hwnd, FALSE);
        d->stack[d->depth++] = w;
        MSG m;
        while (!w->end) {
            if (GetMessageW(&m, NULL, 0, 0) <= 0) { w->end = END_EXIT; break; }
            if (IsDialogMessageW(w->hwnd, &m)) continue;
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        d->depth--;
        if (parent) { EnableWindow(parent->hwnd, TRUE); SetActiveWindow(parent->hwnd); }
        int end = w->end;
        char next[72];
        copy(next, sizeof(next), w->next_dlg);
        eng_log(d->in, "Dialog %s ended (%s)", cur, end == END_EXIT ? "Exit" : end == END_NEW ? next : "Return");
        win_destroy(w);
        if (end == END_NEW && next[0]) { copy(cur, sizeof(cur), next); continue; }
        return end == END_NEW ? END_RETURN : end;
    }
}

/* -----------------------------------------------------------------------
 * Interface
 * ----------------------------------------------------------------------- */
bool dlg_available(MsiDb *db)
{
    MsiTable *dt = msidb_table(db, "Dialog"), *ct = msidb_table(db, "Control");
    MsiTable *ui = msidb_table(db, "InstallUISequence");
    if (!dt || !ct || !ui || !dt->nrows || !ct->nrows) return false;
    /* the UI sequence shows at least one of them */
    char b[16];
    for (int r = 0; r < ui->nrows; r++)
        if (msidb_find(db, dt, 0, msidb_str(db, ui, r, 0, b), 0) >= 0) return true;
    return false;
}

Dlg *dlg_create(Inst *in)
{
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_PROGRESS_CLASS | ICC_TREEVIEW_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = win_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"NovaMsiDialog";
    RegisterClassW(&wc);
    Dlg *d = calloc(1, sizeof(Dlg));
    d->in = in;
    d->db = eng_db(in);
    get_prop(d, "DefaultUIFont", d->default_style, sizeof(d->default_style));
    d->white = CreateSolidBrush(RGB(255, 255, 255));
    return d;
}

void dlg_destroy(Dlg *d)
{
    if (!d) return;
    win_destroy(d->modeless);
    d->modeless = NULL;
    for (int i = 0; i < d->nfonts; i++)
        if (d->fonts[i].f != (HFONT)GetStockObject(DEFAULT_GUI_FONT)) DeleteObject(d->fonts[i].f);
    if (d->white) DeleteObject(d->white);
    pump_messages(d, NULL);
    free(d);
}

bool dlg_exists(Dlg *d, const char *name)
{
    if (!d) return false;
    MsiTable *t = msidb_table(d->db, "Dialog");
    return t && msidb_find(d->db, t, 0, name, 0) >= 0;
}

int dlg_run(Dlg *d, const char *name)
{
    if (d->modeless) { win_destroy(d->modeless); d->modeless = NULL; }
    Rows rs = query(d, "SELECT `Attributes` FROM `Dialog` WHERE `Dialog` = ?", name, NULL);
    unsigned attr = rs.n ? (unsigned)fi(rs.r[0], 1) : 0;
    rows_free(&rs);
    if (!(attr & DA_MODAL)) {
        d->modeless = win_create(d, name, NULL);
        pump_messages(d, NULL);
        return 0;
    }
    return run_modal(d, name, NULL) == END_EXIT ? MSI_ERROR_USEREXIT : 0;
}

void dlg_event(Dlg *d, const char *event, const char *text, int a, int b)
{
    if (!d) return;
    if (d->modeless) publish(d->modeless, event, text, a, b);
    for (int i = 0; i < d->depth; i++) publish(d->stack[i], event, text, a, b);
    pump_messages(d, d->depth ? d->stack[d->depth - 1] : NULL);
}

void dlg_pump(Dlg *d)
{
    if (d) pump_messages(d, d->depth ? d->stack[d->depth - 1] : NULL);
}

bool dlg_cancelled(Dlg *d)
{
    if (!d) return false;
    dlg_pump(d);
    return d->cancelled;
}

HWND dlg_window(Dlg *d)
{
    if (!d) return NULL;
    if (d->depth) return d->stack[d->depth - 1]->hwnd;
    return d->modeless ? d->modeless->hwnd : NULL;
}

void dlg_property_changed(Dlg *d, const char *prop)
{
    (void)prop;
    if (d && !d->updating) refresh_all(d);
}
