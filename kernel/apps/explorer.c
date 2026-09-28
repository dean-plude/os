/*
 * explorer.c — File Explorer for the RAM disk
 *
 * Toolbar (back, up, address, new folder / new file), a sidebar of common
 * folders, and a details list with each file's icon (.ico files, program
 * icons, PNG thumbnails).  Double-click (or Enter) opens a folder, or opens
 * a file by type (Photos, the program itself, else Notepad).
 */

#include "apps.h"
#include "../lib/string.h"
#include "../mm/vmm.h"
#include "../ke/printf.h"

#define TB_H     44          /* toolbar */
#define SIDE_W   176
#define HDR_H    28          /* column header */
#define ROW_H    28
#define STATUS_H 26
#define HIST_MAX 16

typedef struct {
    RamNode *dir;
    int      sel;            /* selected row, -1 = none */
    int      top;            /* first visible row */
    RamNode *back[HIST_MAX];
    int      back_n;
} Explorer;

static const struct { const char *label; const char *path; } g_places[] = {
    { "This PC (C:)", "\\" },
    { "Documents",    "\\Documents" },
    { "Pictures",     "\\Pictures" },
    { "Personal",     "\\Personal" },
    { "Projects",     "\\Projects" },
    { "Windows",      "\\Windows" },
};
#define N_PLACES ((int)(sizeof(g_places) / sizeof(g_places[0])))

/* -----------------------------------------------------------------------
 * Layout (client-relative)
 * ----------------------------------------------------------------------- */
static GdiRect r_back(void)      { return RECT(10, 8, 32, 28); }
static GdiRect r_up(void)        { return RECT(46, 8, 32, 28); }
static GdiRect r_newdir(GdiRect c) { return RECT(c.w - 222, 8, 104, 28); }
static GdiRect r_newfile(GdiRect c){ return RECT(c.w - 112, 8, 100, 28); }
static GdiRect r_list(GdiRect c) { return RECT(SIDE_W, TB_H + HDR_H, c.w - SIDE_W, c.h - TB_H - HDR_H - STATUS_H); }
static int     rows_visible(GdiRect c) { int n = r_list(c).h / ROW_H; return n < 1 ? 1 : n; }

static RamNode *child_at(RamNode *dir, int idx)
{
    RamNode *c = dir->child;
    while (c && idx-- > 0) c = c->next;
    return c;
}

/* -----------------------------------------------------------------------
 * Navigation
 * ----------------------------------------------------------------------- */
static void update_title(WND *w, Explorer *e)
{
    char t[WM_TITLE_MAX];
    ksnprintf(t, sizeof(t), "%s - File Explorer",
              e->dir == RamfsRoot() ? "This PC" : e->dir->name);
    WmSetTitle(w, t);
}

static void set_dir(WND *w, Explorer *e, RamNode *d)
{
    RamfsUnref(e->dir);
    RamfsRef(d);
    e->dir = d;
    e->sel = RamfsCount(d) ? 0 : -1;
    e->top = 0;
    update_title(w, e);
}

static void navigate(WND *w, Explorer *e, RamNode *d)
{
    if (!d || !d->dir || d == e->dir) return;
    if (e->back_n == HIST_MAX) {
        RamfsUnref(e->back[0]);
        memmove(e->back, e->back + 1, sizeof(e->back[0]) * (HIST_MAX - 1));
        e->back_n--;
    }
    RamfsRef(e->dir);                         /* history holds a reference */
    e->back[e->back_n++] = e->dir;
    set_dir(w, e, d);
}

static void go_back(WND *w, Explorer *e)
{
    if (!e->back_n) return;
    RamNode *d = e->back[--e->back_n];
    set_dir(w, e, d);
    RamfsUnref(d);                            /* drop the history reference */
}

static void open_sel(WND *w, Explorer *e)
{
    RamNode *n = e->sel >= 0 ? child_at(e->dir, e->sel) : NULL;
    if (!n) return;
    if (n->dir) navigate(w, e, n);
    else        AppOpenFile(n);
}

static void make_new(WND *w, Explorer *e, bool dir)
{
    char name[RAMFS_NAME_MAX];
    const char *base = dir ? "New folder" : "New Text Document";
    for (int i = 1; i < 100; i++) {
        if (i == 1) ksnprintf(name, sizeof(name), dir ? "%s" : "%s.txt", base);
        else        ksnprintf(name, sizeof(name), dir ? "%s (%d)" : "%s (%d).txt", base, i);
        if (!RamfsFind(e->dir, name)) break;
    }
    RamNode *n = RamfsCreate(e->dir, name, dir);
    if (!n) return;
    int idx = 0;
    for (RamNode *c = e->dir->child; c && c != n; c = c->next) idx++;
    e->sel = idx;
    (void)w;
}

static void ensure_visible(Explorer *e, GdiRect c)
{
    int vis = rows_visible(c);
    if (e->sel < e->top) e->top = e->sel;
    if (e->sel >= e->top + vis) e->top = e->sel - vis + 1;
    if (e->top < 0) e->top = 0;
}

/* -----------------------------------------------------------------------
 * Drawing
 * ----------------------------------------------------------------------- */
static void mini_folder(int x, int y)
{
    GdiRoundRect(RECT(x, y + 2, 8, 4), 1, GDI_C(0xE0, 0xA8, 0x2E), GDI_TRANSPARENT);
    GdiRoundRect(RECT(x, y + 4, 18, 12), 2, GDI_C(0xF6, 0xCE, 0x52), GDI_TRANSPARENT);
}

static void mini_file(int x, int y)
{
    GdiRoundRect(RECT(x + 2, y, 14, 17), 2, GDI_C(0xF2, 0xF2, 0xF2), GDI_C(0xB0, 0xB0, 0xB0));
    for (int i = 0; i < 3; i++)
        GdiFillRect(RECT(x + 5, y + 5 + i * 3, 8, 1), GDI_C(0x4A, 0x7B, 0xD0));
}

static void arrow_glyph(GdiRect b, bool up, GdiColor c)
{
    int cx = b.x + b.w / 2, cy = b.y + b.h / 2;
    if (up) {
        GdiLine((GdiPoint){ (cx - 5) * 16, (cy + 2) * 16 }, (GdiPoint){ cx * 16, (cy - 4) * 16 }, 22, c);
        GdiLine((GdiPoint){ cx * 16, (cy - 4) * 16 }, (GdiPoint){ (cx + 5) * 16, (cy + 2) * 16 }, 22, c);
    } else {
        GdiLine((GdiPoint){ (cx + 2) * 16, (cy - 5) * 16 }, (GdiPoint){ (cx - 4) * 16, cy * 16 }, 22, c);
        GdiLine((GdiPoint){ (cx - 4) * 16, cy * 16 }, (GdiPoint){ (cx + 2) * 16, (cy + 5) * 16 }, 22, c);
    }
}

static GdiRect off(GdiRect r, GdiRect c) { return RECT(r.x + c.x, r.y + c.y, r.w, r.h); }

static void exp_paint(WND *w)
{
    Explorer *e = w->user;
    GdiRect c = WmClientRect(w);

    /* Toolbar */
    GdiFillRect(RECT(c.x, c.y, c.w, TB_H), UI_PANEL);
    GdiFillRect(RECT(c.x, c.y + TB_H - 1, c.w, 1), UI_LINE);
    GdiColor bc = e->back_n ? UI_TEXT : UI_TEXT3;
    arrow_glyph(off(r_back(), c), false, bc);
    arrow_glyph(off(r_up(), c), true, e->dir->parent ? UI_TEXT : UI_TEXT3);
    GdiRect addr = RECT(c.x + 88, c.y + 8, c.w - 88 - 236, 28);
    GdiRoundRect(addr, 4, UI_CARD, UI_LINE);
    char path[RAMFS_PATH_MAX];
    RamfsPath(e->dir, path, sizeof(path));
    GdiSetClip(RECT(addr.x + 8, addr.y, addr.w - 16, addr.h));
    GdiTextT(addr.x + 10, addr.y + 6, path, UI_TEXT);
    GdiSetClip(c);
    UiButton(off(r_newdir(c), c), "New folder", false);
    UiButton(off(r_newfile(c), c), "New file", false);

    /* Sidebar */
    GdiFillRect(RECT(c.x, c.y + TB_H, SIDE_W, c.h - TB_H), UI_PANEL);
    for (int i = 0; i < N_PLACES; i++) {
        int y = c.y + TB_H + 8 + i * 32;
        RamNode *p = RamfsResolve(NULL, g_places[i].path);
        if (p == e->dir) {
            GdiRoundRect(RECT(c.x + 6, y, SIDE_W - 12, 30), 4, UI_HOVER, GDI_TRANSPARENT);
            GdiRoundRect(RECT(c.x + 6, y + 8, 3, 14), 1, UI_ACCENT, GDI_TRANSPARENT);
        }
        mini_folder(c.x + 18, y + 6);
        GdiTextT(c.x + 44, y + 7, g_places[i].label, UI_TEXT);
    }

    /* Column header */
    int lx = c.x + SIDE_W;
    int size_x = c.x + c.w - 240, type_x = c.x + c.w - 140;
    GdiTextT(lx + 44, c.y + TB_H + 6, "Name", UI_TEXT2);
    GdiTextT(size_x, c.y + TB_H + 6, "Size", UI_TEXT2);
    GdiTextT(type_x, c.y + TB_H + 6, "Type", UI_TEXT2);
    GdiFillRect(RECT(lx, c.y + TB_H + HDR_H - 1, c.w - SIDE_W, 1), UI_LINE);

    /* Rows */
    GdiRect lr = off(r_list(c), c);
    GdiSetClip(lr);
    int vis = rows_visible(c), idx = 0, n = 0;
    for (RamNode *f = e->dir->child; f; f = f->next, idx++) {
        n++;
        if (idx < e->top || idx >= e->top + vis) continue;
        int y = lr.y + (idx - e->top) * ROW_H;
        if (idx == e->sel)
            GdiRoundRect(RECT(lx + 6, y + 1, c.w - SIDE_W - 12, ROW_H - 2), 4,
                         w->active ? UI_SELECT : UI_HOVER, GDI_TRANSPARENT);
        if (f->dir) mini_folder(lx + 16, y + 5);
        else if (IconDraw(AppFileIcon(f), lx + 15, y + 5, 18)) { /* its own icon */ }
        else if (!strcmp(AppFileTypeName(f), "Application")) AppDrawProgramIcon(f->name, lx + 15, y + 5, 18);
        else mini_file(lx + 16, y + 5);
        GdiSetClip(RECT(lr.x, lr.y, size_x - lr.x - 12, lr.h));
        GdiTextT(lx + 44, y + 6, f->name, UI_TEXT);
        GdiSetClip(lr);
        if (!f->dir) {
            char sz[24];
            AppFormatSize(f->size, sz, sizeof(sz));
            GdiTextT(size_x, y + 6, sz, UI_TEXT2);
        }
        GdiTextT(type_x, y + 6, AppFileTypeName(f), UI_TEXT2);
    }
    if (!n) GdiTextCenter(lr.x, lr.y + 40, lr.w, "This folder is empty.", UI_TEXT3);
    GdiSetClip(c);

    /* Status bar */
    GdiFillRect(RECT(c.x + SIDE_W, c.y + c.h - STATUS_H, c.w - SIDE_W, STATUS_H), UI_PANEL);
    char st[48];
    ksnprintf(st, sizeof(st), "%d item%s", n, n == 1 ? "" : "s");
    GdiTextT(c.x + SIDE_W + 12, c.y + c.h - STATUS_H + 5, st, UI_TEXT2);
}

/* -----------------------------------------------------------------------
 * Input
 * ----------------------------------------------------------------------- */
static void exp_mouse(WND *w, WmMouseMsg msg, int x, int y)
{
    Explorer *e = w->user;
    GdiRect c = WmClientRect(w);

    if (msg == WM_MOUSE_UP) {
        if (UiHit(r_back(), x, y))        go_back(w, e);
        else if (UiHit(r_up(), x, y))     navigate(w, e, e->dir->parent);
        else if (UiHit(r_newdir(c), x, y))  make_new(w, e, true);
        else if (UiHit(r_newfile(c), x, y)) make_new(w, e, false);
        return;
    }
    if (msg != WM_MOUSE_DOWN && msg != WM_MOUSE_DBLCLK) return;

    if (x < SIDE_W && y >= TB_H) {                 /* sidebar */
        int i = (y - TB_H - 8) / 32;
        if (i >= 0 && i < N_PLACES)
            navigate(w, e, RamfsResolve(NULL, g_places[i].path));
        return;
    }
    GdiRect lr = r_list(c);
    if (UiHit(lr, x, y)) {
        int idx = e->top + (y - lr.y) / ROW_H;
        if (idx < RamfsCount(e->dir)) {
            e->sel = idx;
            if (msg == WM_MOUSE_DBLCLK) open_sel(w, e);
        } else {
            e->sel = -1;
        }
    }
}

static void exp_key(WND *w, const KeyEvent *k)
{
    Explorer *e = w->user;
    int n = RamfsCount(e->dir);
    if (k->extended) {
        if (k->scancode == KEY_UP   && e->sel > 0)     e->sel--;
        if (k->scancode == KEY_DOWN && e->sel < n - 1) e->sel++;
        if (k->scancode == KEY_HOME && n) e->sel = 0;
        if (k->scancode == KEY_END  && n) e->sel = n - 1;
        if (k->scancode == KEY_DELETE && e->sel >= 0) {
            RamNode *victim = child_at(e->dir, e->sel);
            if (victim && RamfsDelete(victim) && e->sel >= RamfsCount(e->dir))
                e->sel = RamfsCount(e->dir) - 1;
        }
        ensure_visible(e, WmClientRect(w));
        return;
    }
    if (k->ch == '\n')      open_sel(w, e);
    else if (k->ch == '\b') navigate(w, e, e->dir->parent);
}

static void exp_close(WND *w)
{
    Explorer *e = w->user;
    RamfsUnref(e->dir);
    for (int i = 0; i < e->back_n; i++) RamfsUnref(e->back[i]);
    kfree(e);
    w->user = NULL;
}

void ExplorerOpen(RamNode *dir)
{
    Explorer *e = kzalloc(sizeof(Explorer));
    if (!e) return;
    WND *w = AppCreateWindow(APP_EXPLORER, "File Explorer", 820, 500, UI_BG);
    if (!w) { kfree(e); return; }
    w->user     = e;
    w->on_paint = exp_paint;
    w->on_mouse = exp_mouse;
    w->on_key   = exp_key;
    w->on_close = exp_close;
    set_dir(w, e, (dir && dir->dir) ? dir : RamfsRoot());
}
