/*
 * explorer.c — File Explorer for the RAM disk
 *
 * A command bar (back, up, a clickable breadcrumb path, + Folder / + File),
 * a sidebar of common
 * folders, and a details list with each file's icon (.ico files, program
 * icons, PNG thumbnails).  Double-click (or Enter) opens a folder, or opens
 * a file by type (Photos, the program itself, else Notepad).
 */

#include "apps.h"
#include "../lib/string.h"
#include "../mm/vmm.h"
#include "../ke/printf.h"

#define TB_H     48          /* command bar */
#define SIDE_W   184
#define HDR_H    28          /* column header */
#define ROW_H    28
#define STATUS_H 26
#define HIST_MAX 16

#define CRUMB_MAX 12

typedef struct {
    RamNode *dir;
    int      sel;            /* selected row, -1 = none */
    int      top;            /* first visible row */
    RamNode *back[HIST_MAX];
    int      back_n;
    /* breadcrumb segments as last drawn (client-relative), for clicks */
    GdiRect  crumb_r[CRUMB_MAX];
    RamNode *crumb_dir[CRUMB_MAX];
    int      ncrumb;
} Explorer;

static const struct { const char *label; const char *path; Glyph glyph; } g_places[] = {
    { "This PC",   "\\",          GL_PC },
    { "Documents", "\\Documents", GL_DOCUMENTS },
    { "Downloads", "\\Downloads", GL_DOWNLOADS },
    { "Pictures",  "\\Pictures",  GL_PICTURES },
    { "Personal",  "\\Personal",  GL_PERSON },
    { "Projects",  "\\Projects",  GL_CODE },
    { "Windows",   "\\Windows",   GL_WINDOWS },
};
#define N_PLACES ((int)(sizeof(g_places) / sizeof(g_places[0])))

/* -----------------------------------------------------------------------
 * Layout (client-relative)
 * ----------------------------------------------------------------------- */
static GdiRect r_back(void)       { return RECT(8, 8, 32, 32); }
static GdiRect r_up(void)         { return RECT(42, 8, 32, 32); }
static GdiRect r_newdir(GdiRect c)  { return RECT(c.w - 196, 8, 92, 32); }
static GdiRect r_newfile(GdiRect c) { return RECT(c.w - 98, 8, 88, 32); }
static GdiRect r_crumbs(GdiRect c)  { return RECT(84, 8, c.w - 84 - 206, 32); }
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
/* A command-bar button: a glyph, and a label if there is one */
static void cmd_button(GdiRect r, Glyph g, const char *label, bool enabled)
{
    GdiColor fg = enabled ? UI_TEXT : UI_TEXT3;
    if (label) {
        GdiRoundRect(r, 6, UI_CARD, GDI_TRANSPARENT);
        GdiRoundBorderAlpha(r, 6, GDI_WHITE, 18);
        AppDrawGlyph(g, r.x + 10, r.y + (r.h - 14) / 2, 14, fg);
        GdiTextT(r.x + 30, r.y + (r.h - GDI_FONT_H) / 2, label, fg);
    } else {
        AppDrawGlyph(g, r.x + (r.w - 16) / 2, r.y + (r.h - 16) / 2, 16, fg);
    }
}

/* This PC > Local Disk (C:) > Documents > ...: each part navigates; when
 * the path is too long the leading parts collapse to "..." */
static void draw_crumbs(Explorer *e, GdiRect bar, GdiRect c)
{
    GdiRoundRect(bar, 6, UI_CARD, GDI_TRANSPARENT);
    GdiRoundBorderAlpha(bar, 6, GDI_WHITE, 18);
    const char *label[CRUMB_MAX];
    RamNode *dir[CRUMB_MAX];
    RamNode *chain[CRUMB_MAX];
    int depth = 0;
    for (RamNode *d = e->dir; d && d != RamfsRoot() && depth < CRUMB_MAX - 2; d = d->parent) chain[depth++] = d;
    int n = 0;
    label[n] = "This PC";         dir[n++] = RamfsRoot();
    label[n] = "Local Disk (C:)"; dir[n++] = RamfsRoot();
    for (int i = depth - 1; i >= 0; i--) { label[n] = chain[i]->name; dir[n++] = chain[i]; }

    int chev = 18, avail = bar.w - 20;
    int first = 0;
    for (;;) {                                  /* drop leading parts until it fits */
        int wsum = first ? GdiTextW("...") + chev : 0;
        for (int i = first; i < n; i++) wsum += GdiTextW(label[i]) + (i + 1 < n ? chev : 0);
        if (wsum <= avail || first >= n - 1) break;
        first++;
    }
    GdiSetClip(RECT(bar.x + 4, bar.y, bar.w - 8, bar.h));
    int x = bar.x + 10, ty = bar.y + (bar.h - GDI_FONT_H) / 2;
    e->ncrumb = 0;
    if (first) {
        GdiTextT(x, ty, "...", UI_TEXT2);
        x += GdiTextW("...");
        AppDrawGlyph(GL_CHEVRON, x + 4, bar.y + (bar.h - 10) / 2, 10, UI_TEXT3);
        x += chev;
    }
    for (int i = first; i < n; i++) {
        int tw = GdiTextW(label[i]);
        GdiTextT(x, ty, label[i], i == n - 1 ? UI_TEXT : UI_TEXT2);
        if (e->ncrumb < CRUMB_MAX) {
            e->crumb_r[e->ncrumb] = RECT(x - 4 - c.x, bar.y - c.y, tw + 8, bar.h);
            e->crumb_dir[e->ncrumb++] = dir[i];
        }
        x += tw;
        if (i + 1 < n) {
            AppDrawGlyph(GL_CHEVRON, x + 4, bar.y + (bar.h - 10) / 2, 10, UI_TEXT3);
            x += chev;
        }
    }
    GdiSetClip(c);
}

/* The icon of a row: the folder (special ones show their purpose), the
 * file's own icon, a program tile, or a document */
static void row_icon(RamNode *f, int x, int y)
{
    if (f->dir)                            AppDrawFolderKindIcon(AppFolderKind(f), x, y, 20);
    else if (IconDraw(AppFileIcon(f), x, y, 20)) { /* its own icon */ }
    else if (!strcmp(AppFileTypeName(f), "Application")) AppDrawProgramIcon(f->name, x, y, 20);
    else                                   AppDrawFileIcon(x, y, 20);
}

static GdiRect off(GdiRect r, GdiRect c) { return RECT(r.x + c.x, r.y + c.y, r.w, r.h); }

static void exp_paint(WND *w)
{
    Explorer *e = w->user;
    GdiRect c = WmClientRect(w);

    /* Command bar */
    GdiFillRect(RECT(c.x, c.y, c.w, TB_H), UI_PANEL);
    GdiFillRect(RECT(c.x, c.y + TB_H - 1, c.w, 1), UI_LINE);
    cmd_button(off(r_back(), c), GL_BACK, NULL, e->back_n > 0);
    cmd_button(off(r_up(), c), GL_UP, NULL, e->dir->parent != NULL);
    draw_crumbs(e, off(r_crumbs(c), c), c);
    cmd_button(off(r_newdir(c), c), GL_PLUS, "Folder", true);
    cmd_button(off(r_newfile(c), c), GL_PLUS, "File", true);

    /* Sidebar: places with line glyphs, divided from the list */
    GdiFillRect(RECT(c.x, c.y + TB_H, SIDE_W, c.h - TB_H), UI_PANEL);
    GdiFillRect(RECT(c.x + SIDE_W - 1, c.y + TB_H, 1, c.h - TB_H), UI_LINE);
    for (int i = 0; i < N_PLACES; i++) {
        int y = c.y + TB_H + 8 + i * 32;
        RamNode *p = RamfsResolve(NULL, g_places[i].path);
        bool here = p == e->dir;
        if (here) {
            GdiRoundRect(RECT(c.x + 6, y, SIDE_W - 13, 30), 4, UI_HOVER, GDI_TRANSPARENT);
            GdiRoundRect(RECT(c.x + 6, y + 8, 3, 14), 1, UI_ACCENT, GDI_TRANSPARENT);
        }
        AppDrawGlyph(g_places[i].glyph, c.x + 18, y + 7, 16, here ? UI_TEXT : UI_TEXT2);
        GdiTextT(c.x + 44, y + 7, g_places[i].label, here ? UI_TEXT : UI_TEXT2);
    }

    /* Column header */
    int lx = c.x + SIDE_W;
    int size_x = c.x + c.w - 250, type_x = c.x + c.w - 150;
    GdiTextT(lx + 48, c.y + TB_H + 6, "Name", UI_TEXT2);
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
        row_icon(f, lx + 18, y + 4);
        GdiSetClip(RECT(lr.x, lr.y, size_x - lr.x - 12, lr.h));
        GdiTextT(lx + 48, y + 6, f->name, UI_TEXT);
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
    GdiFillRect(RECT(c.x + SIDE_W, c.y + c.h - STATUS_H, c.w - SIDE_W, 1), UI_LINE);
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
        else
            for (int i = 0; i < e->ncrumb; i++)
                if (UiHit(e->crumb_r[i], x, y)) { navigate(w, e, e->crumb_dir[i]); break; }
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
