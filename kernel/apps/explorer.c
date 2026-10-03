/*
 * explorer.c — File Explorer for the RAM disk
 *
 * A command bar (back, up, a clickable breadcrumb path, + Folder / + File),
 * a sidebar of common
 * folders, and a details list with each file's icon (.ico files, program
 * icons, PNG thumbnails).  Double-click (or Enter) opens a folder, or opens
 * a file by type (Photos, the program itself, else Notepad).
 *
 * This PC (no folder: e->dir is NULL) lists the drives, C: and every
 * mounted volume, with their free space and size.
 *
 * The list has a vertical scroll bar when its rows don't fit and a
 * horizontal one when the window is narrower than its columns need; the
 * sidebar has one when its places and drives don't fit (UiScroll, the
 * apps' version of user32's scroll bars).
 */

#include "apps.h"
#include "../lib/string.h"
#include "../mm/vmm.h"
#include "../ke/printf.h"
#include "../wm/clipboard.h"

#define TB_H     48          /* command bar */
#define SIDE_W   184
#define HDR_H    28          /* column header */
#define ROW_H    28
#define STATUS_H 26
#define HIST_MAX 16

#define CRUMB_MAX 12
#define SIDE_ROW 32
#define COLS_W   250         /* the Size and Type columns, at the right */
#define LIST_MIN_W (48 + 200 + COLS_W)   /* narrower than this, the list scrolls sideways */
#define WHEEL_ROWS 3         /* rows a wheel notch scrolls (Windows' default) */

typedef struct {
    RamNode *dir;            /* NULL: This PC */
    int      sel;            /* selected row, -1 = none */
    UiScroll vbar;           /* the list: rows (pos: the first row shown) */
    UiScroll hbar;           /* the list: pixels across */
    UiScroll sbar;           /* the sidebar: pixels down */
    int      logged[9];      /* the view last written to the log */
    RamNode *back[HIST_MAX];
    int      back_n;
    /* breadcrumb segments as last drawn (client-relative), for clicks */
    GdiRect  crumb_r[CRUMB_MAX];
    RamNode *crumb_dir[CRUMB_MAX];   /* NULL: This PC */
    int      ncrumb;
    char     status[80];     /* the last copy/paste, in the status bar */
} Explorer;

static const struct { const char *label; const char *path; Glyph glyph; } g_places[] = {
    { "This PC",   NULL,          GL_PC },
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
static GdiRect r_copy(GdiRect c)    { return RECT(c.w - 372, 8, 84, 32); }
static GdiRect r_paste(GdiRect c)   { return RECT(c.w - 284, 8, 84, 32); }
static GdiRect r_crumbs(GdiRect c)  { return RECT(84, 8, c.w - 84 - 382, 32); }
static GdiRect r_list(GdiRect c) { return RECT(SIDE_W, TB_H + HDR_H, c.w - SIDE_W, c.h - TB_H - HDR_H - STATUS_H); }
static GdiRect r_side(GdiRect c) { return RECT(0, TB_H, SIDE_W - 1, c.h - TB_H); }

/* "USB DRIVE (E:)": a drive's name, from its root */
static void drive_name(RamNode *root, char *buf, int cap)
{
    const char *label = NULL;
    char letter = RamfsDriveLetter(root);
    if (letter == 'C' || !RamfsDriveInfo(root, &label, NULL, NULL)) { ksnprintf(buf, cap, "Local Disk (C:)"); return; }
    ksnprintf(buf, cap, "%s (%c:)", label && *label ? label : "Local Disk", letter);
}

/* The drives after the places in the sidebar: D: to Z: as they are now */
static int sidebar_drives(char *letters)
{
    int n = 0;
    for (char l = 'D'; l <= 'Z'; l++)
        if (RamfsDriveRoot(l)) letters[n++] = l;
    return n;
}

static RamNode *child_at(RamNode *dir, int idx)
{
    RamNode *c = dir->child;
    while (c && idx-- > 0) c = c->next;
    return c;
}

/* This PC's rows: drive C: and then D: to Z: as they are now */
static int pc_drives(RamNode **roots)
{
    char letters[26];
    int nd = sidebar_drives(letters);
    roots[0] = RamfsRoot();
    for (int i = 0; i < nd; i++) roots[i + 1] = RamfsDriveRoot(letters[i]);
    return nd + 1;
}

/* The number of rows, and the node on row @idx (a drive's root on This PC) */
static int rows_of(const Explorer *e)
{
    RamNode *roots[27];
    return e->dir ? RamfsCount(e->dir) : pc_drives(roots);
}

static RamNode *row_at(const Explorer *e, int idx)
{
    if (idx < 0) return NULL;
    if (e->dir) return child_at(e->dir, idx);
    RamNode *roots[27];
    return idx < pc_drives(roots) ? roots[idx] : NULL;
}

/* -----------------------------------------------------------------------
 * Scrolling
 * ----------------------------------------------------------------------- */
typedef struct {
    GdiRect list;            /* the rows' view, less the scroll bars (client-relative) */
    int     vis;             /* whole rows in it */
    int     cw;              /* the columns' width (wider than the view: scrolls sideways) */
    int     n;               /* rows */
} Layout;

/* The view for client @c, with the bars' ranges set: each bar is shown
 * when its content doesn't fit, which takes room from the other way */
static Layout layout(Explorer *e, GdiRect c)
{
    Layout L;
    GdiRect lr = r_list(c);
    L.n = rows_of(e);
    bool vb = false, hb = false;
    for (int pass = 0; pass < 2; pass++) {
        hb = LIST_MIN_W > lr.w - (vb ? UI_SB_W : 0);
        vb = L.n > (lr.h - (hb ? UI_SB_W : 0)) / ROW_H;
    }
    L.list = RECT(lr.x, lr.y, lr.w - (vb ? UI_SB_W : 0), lr.h - (hb ? UI_SB_W : 0));
    L.vis = L.list.h / ROW_H < 1 ? 1 : L.list.h / ROW_H;
    L.cw = L.list.w > LIST_MIN_W ? L.list.w : LIST_MIN_W;
    e->vbar.vert = true;
    e->vbar.line = 1;
    UiScrollSet(&e->vbar, L.n, L.vis);
    e->hbar.line = 20;
    UiScrollSet(&e->hbar, L.cw, L.list.w);

    char letters[26];
    int nd = sidebar_drives(letters);
    e->sbar.vert = true;
    e->sbar.line = SIDE_ROW;
    UiScrollSet(&e->sbar, 8 + N_PLACES * SIDE_ROW + (nd ? 8 + nd * SIDE_ROW : 0) + 8, r_side(c).h);
    return L;
}

static void ensure_visible(Explorer *e, const Layout *L)
{
    if (e->sel < 0) return;
    if (e->sel < e->vbar.pos) UiScrollTo(&e->vbar, e->sel);
    if (e->sel >= e->vbar.pos + L->vis) UiScrollTo(&e->vbar, e->sel - L->vis + 1);
}

/* One line in the serial log when the view changes (what the self-test
 * reads): the folder, the rows shown, the bars and where they are */
static void log_view(WND *w, Explorer *e, const Layout *L)
{
    if (e->vbar.held != UI_SB_NONE || e->hbar.held != UI_SB_NONE || e->sbar.held != UI_SB_NONE) return;
    GdiRect c = WmClientRect(w);
    int now[9] = { (int)(UINT64)e->dir, e->vbar.pos, L->n, e->hbar.pos, UiScrollNeeded(&e->vbar),
                   UiScrollNeeded(&e->hbar), UiScrollNeeded(&e->sbar), c.x + L->list.x, c.y + L->list.y };
    if (!memcmp(now, e->logged, sizeof(now))) return;
    memcpy(e->logged, now, sizeof(now));
    char path[RAMFS_PATH_MAX];
    if (e->dir) RamfsPath(e->dir, path, sizeof(path));
    else ksnprintf(path, sizeof(path), "This PC");
    int last = e->vbar.pos + L->vis < L->n ? e->vbar.pos + L->vis : L->n;
    kprintf("[EXPLORER] %s: rows %d-%d of %d, scrolled %d px; bars:%s%s%s; list %d,%d %dx%d\n", path,
            L->n ? e->vbar.pos + 1 : 0, last, L->n, e->hbar.pos,
            UiScrollNeeded(&e->vbar) ? " vertical" : "", UiScrollNeeded(&e->hbar) ? " horizontal" : "",
            UiScrollNeeded(&e->sbar) ? " sidebar" : "",
            c.x + L->list.x, c.y + L->list.y, L->list.w, L->list.h);
}

/* -----------------------------------------------------------------------
 * Navigation
 * ----------------------------------------------------------------------- */
static void update_title(WND *w, Explorer *e)
{
    char t[WM_TITLE_MAX];
    char dn[80];
    if (e->dir && !e->dir->parent) drive_name(e->dir, dn, sizeof(dn));
    ksnprintf(t, sizeof(t), "%s - File Explorer",
              !e->dir ? "This PC" : !e->dir->parent ? dn : e->dir->name);
    WmSetTitle(w, t);
}

static void set_dir(WND *w, Explorer *e, RamNode *d)
{
    RamfsUnref(e->dir);
    RamfsRef(d);
    e->dir = d;
    e->sel = rows_of(e) ? 0 : -1;
    e->vbar.pos = e->hbar.pos = 0;
    e->status[0] = 0;
    update_title(w, e);
}

/* To folder @d, or This PC when @d is NULL */
static void navigate(WND *w, Explorer *e, RamNode *d)
{
    if ((d && !d->dir) || d == e->dir) return;
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
    RamNode *n = row_at(e, e->sel);
    if (!n) return;
    if (n->dir) navigate(w, e, n);
    else        AppOpenFile(n);
}

static void make_new(WND *w, Explorer *e, bool dir)
{
    char name[RAMFS_NAME_MAX];
    const char *base = dir ? "New folder" : "New Text Document";
    if (!e->dir) return;
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
    Layout L = layout(e, WmClientRect(w));
    ensure_visible(e, &L);
}

/* -----------------------------------------------------------------------
 * Copy and paste (the system clipboard: CF_HDROP, which programs read too)
 * ----------------------------------------------------------------------- */
static void clip_copy(Explorer *e, bool cut)
{
    RamNode *f = e->dir ? row_at(e, e->sel) : NULL;
    if (!f) return;
    char path[RAMFS_PATH_MAX];
    RamfsPath(f, path, sizeof(path));
    const char *paths[1] = { path };
    ClipSetFiles(paths, 1);
    if (cut) {                                   /* "Preferred DropEffect": move */
        UINT8 *eff = kzalloc(4);
        if (eff) { eff[0] = 2; ClipPut(0xC000, "Preferred DropEffect", eff, 4); }
    }
    ksnprintf(e->status, sizeof(e->status), "%s \"%s\"", cut ? "Cut" : "Copied", f->name);
}

/* "name - Copy.ext", "name - Copy (2).ext"... until it is free in @dir */
static void free_name(RamNode *dir, const char *name, char *out, int cap)
{
    if (!RamfsFind(dir, name)) { ksnprintf(out, (UINT32)cap, "%s", name); return; }
    const char *dot = strrchr(name, '.');
    int stem = dot && dot != name ? (int)(dot - name) : (int)strlen(name);
    for (int i = 1; i < 100; i++) {
        if (i == 1) ksnprintf(out, (UINT32)cap, "%.*s - Copy%s", stem, name, name + stem);
        else ksnprintf(out, (UINT32)cap, "%.*s - Copy (%d)%s", stem, name, i, name + stem);
        if (!RamfsFind(dir, out)) return;
    }
}

static bool copy_tree(RamNode *src, RamNode *dir, const char *name)
{
    if (src == dir) return false;
    for (RamNode *p = dir; p; p = p->parent) if (p == src) return false;   /* not into itself */
    RamNode *d = RamfsCreate(dir, name, src->dir);
    if (!d) return false;
    RamfsRef(src);                          /* (a file or folder on another drive is read in meanwhile) */
    bool ok = RamfsLoad(src);
    if (!src->dir) {
        ok = ok && RamfsWrite(d, src->data, src->size);
        RamfsUnref(src);
        return ok;
    }
    RamfsUnref(src);
    for (RamNode *c = src->child; c; c = c->next) ok = copy_tree(c, d, c->name) && ok;
    return ok;
}

static void clip_paste(Explorer *e)
{
    UINT8 eff[4] = { 1, 0, 0, 0 };
    if (!e->dir) return;
    ClipGet(0xC000, "Preferred DropEffect", eff, 4);
    bool move = eff[0] & 2;
    int done = 0, failed = 0;
    char path[RAMFS_PATH_MAX], name[RAMFS_NAME_MAX];
    for (int i = 0; ClipGetFile(i, path, sizeof(path)); i++) {
        const char *p = path;
        if ((p[0] | 0x20) == 'c' && p[1] == ':') p += 2;
        RamNode *src = RamfsResolve(NULL, p);
        if (!src) { failed++; continue; }
        if (move) {
            if (src->parent == e->dir) { done++; continue; }
            free_name(e->dir, src->name, name, sizeof(name));
            if (RamfsRename(src, e->dir, name, false)) done++; else failed++;
        } else {
            free_name(e->dir, src->name, name, sizeof(name));
            if (copy_tree(src, e->dir, name)) done++; else failed++;
        }
    }
    if (move && done) ClipEmpty(0);                /* moved files are not there to paste again */
    if (!done && !failed) ksnprintf(e->status, sizeof(e->status), "Nothing to paste");
    else ksnprintf(e->status, sizeof(e->status), "%s %d item%s%s", move ? "Moved" : "Pasted", done,
                   done == 1 ? "" : "s", failed ? " (some failed)" : "");
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
    RamNode *top = e->dir;
    for (RamNode *d = e->dir; d && d->parent && depth < CRUMB_MAX - 2; d = d->parent) { chain[depth++] = d; top = d->parent; }
    int n = 0;
    static char drive_label[80];
    label[n] = "This PC";         dir[n++] = NULL;
    if (top) {
        drive_name(top, drive_label, sizeof(drive_label));
        label[n] = drive_label;   dir[n++] = top;
    }
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
    if (e->dir && RamfsDetached(e->dir)) {   /* its drive was unplugged */
        for (int i = 0; i < e->back_n; i++) RamfsUnref(e->back[i]);
        e->back_n = 0;
        set_dir(w, e, NULL);
    }

    /* Command bar */
    GdiFillRect(RECT(c.x, c.y, c.w, TB_H), UI_PANEL);
    GdiFillRect(RECT(c.x, c.y + TB_H - 1, c.w, 1), UI_LINE);
    cmd_button(off(r_back(), c), GL_BACK, NULL, e->back_n > 0);
    cmd_button(off(r_up(), c), GL_UP, NULL, e->dir != NULL);
    draw_crumbs(e, off(r_crumbs(c), c), c);
    cmd_button(off(r_copy(c), c), GL_FILE, "Copy", e->dir && e->sel >= 0);
    cmd_button(off(r_paste(c), c), GL_FILE, "Paste", e->dir && ClipList((ClipEntry[1]){ { 0 } }, 1) > 0);
    cmd_button(off(r_newdir(c), c), GL_PLUS, "Folder", e->dir != NULL);
    cmd_button(off(r_newfile(c), c), GL_PLUS, "File", e->dir != NULL);

    Layout L = layout(e, c);

    /* Sidebar: places with line glyphs, divided from the list; it scrolls
     * when they and the drives don't fit */
    GdiRect sv = off(r_side(c), c);
    int sw = SIDE_W - (UiScrollNeeded(&e->sbar) ? UI_SB_W : 0);   /* its items' room */
    GdiFillRect(RECT(c.x, c.y + TB_H, SIDE_W, c.h - TB_H), UI_PANEL);
    GdiFillRect(RECT(c.x + SIDE_W - 1, c.y + TB_H, 1, c.h - TB_H), UI_LINE);
    GdiSetClip(sv);
    int sy = c.y + TB_H - e->sbar.pos;
    for (int i = 0; i < N_PLACES; i++) {
        int y = sy + 8 + i * SIDE_ROW;
        bool here = g_places[i].path ? e->dir && RamfsResolve(NULL, g_places[i].path) == e->dir : !e->dir;
        if (here) {
            GdiRoundRect(RECT(c.x + 6, y, sw - 13, 30), 4, UI_HOVER, GDI_TRANSPARENT);
            GdiRoundRect(RECT(c.x + 6, y + 8, 3, 14), 1, UI_ACCENT, GDI_TRANSPARENT);
        }
        AppDrawGlyph(g_places[i].glyph, c.x + 18, y + 7, 16, here ? UI_TEXT : UI_TEXT2);
        GdiTextT(c.x + 44, y + 7, g_places[i].label, here ? UI_TEXT : UI_TEXT2);
    }
    /* Then the other drives (NTFS volumes, USB sticks) */
    char letters[26];
    int nd = sidebar_drives(letters);
    if (nd) GdiFillRect(RECT(c.x + 14, sy + 8 + N_PLACES * SIDE_ROW + 3, sw - 28, 1), UI_LINE);
    for (int i = 0; i < nd; i++) {
        int y = sy + 16 + (N_PLACES + i) * SIDE_ROW;
        RamNode *root = RamfsDriveRoot(letters[i]);
        bool here = root && root == e->dir;
        if (here) {
            GdiRoundRect(RECT(c.x + 6, y, sw - 13, 30), 4, UI_HOVER, GDI_TRANSPARENT);
            GdiRoundRect(RECT(c.x + 6, y + 8, 3, 14), 1, UI_ACCENT, GDI_TRANSPARENT);
        }
        char dn[80];
        drive_name(root, dn, sizeof(dn));
        AppDrawGlyph(GL_PC, c.x + 18, y + 7, 16, here ? UI_TEXT : UI_TEXT2);
        GdiSetClip(RECT(c.x + 44, y > sv.y ? y : sv.y, sw - 52, 30 - (y > sv.y ? 0 : sv.y - y)));
        GdiTextT(c.x + 44, y + 7, dn, here ? UI_TEXT : UI_TEXT2);
        GdiSetClip(sv);
    }
    GdiSetClip(c);
    UiScrollDraw(&e->sbar, RECT(SIDE_W - 1 - UI_SB_W, TB_H, UI_SB_W, r_side(c).h), c);

    /* Column header: it scrolls sideways with the rows */
    GdiRect lr = off(L.list, c);
    int lx = lr.x - e->hbar.pos;                    /* the columns' left edge */
    int size_x = lx + L.cw - COLS_W, type_x = lx + L.cw - COLS_W + 100;
    int name_w = size_x - 12 - lr.x;                /* the Name column's room, from the view's edge */
    if (name_w < 0) name_w = 0;
    GdiSetClip(RECT(lr.x, c.y + TB_H, lr.w, HDR_H));
    GdiTextT(lx + 48, c.y + TB_H + 6, "Name", UI_TEXT2);
    GdiTextT(size_x, c.y + TB_H + 6, e->dir ? "Size" : "Free space", UI_TEXT2);
    GdiTextT(type_x, c.y + TB_H + 6, e->dir ? "Type" : "Total size", UI_TEXT2);
    GdiSetClip(c);
    GdiFillRect(RECT(c.x + SIDE_W, c.y + TB_H + HDR_H - 1, c.w - SIDE_W, 1), UI_LINE);

    /* Rows: the whole ones in view and the part of the next */
    GdiSetClip(lr);
    int top = e->vbar.pos, idx = 0, n = 0;
    RamNode *roots[27];
    int nroots = e->dir ? 0 : pc_drives(roots);
    for (int i = 0; i < nroots; i++, idx++) {       /* This PC: the drives */
        n++;
        if (idx < top || idx > top + L.vis) continue;
        int y = lr.y + (idx - top) * ROW_H;
        if (idx == e->sel)
            GdiRoundRect(RECT(lx + 6, y + 1, L.cw - 12, ROW_H - 2), 4,
                         w->active ? UI_SELECT : UI_HOVER, GDI_TRANSPARENT);
        AppDrawGlyph(GL_PC, lx + 20, y + 6, 16, UI_TEXT);
        char dn[80], fs[24], ts[24];
        UINT64 total, free;
        drive_name(roots[i], dn, sizeof(dn));
        AppDriveSpace(roots[i], &total, &free);
        AppFormatSize(free, fs, sizeof(fs));
        AppFormatSize(total, ts, sizeof(ts));
        GdiSetClip(RECT(lr.x, lr.y, name_w, lr.h));
        GdiTextT(lx + 48, y + 6, dn, UI_TEXT);
        GdiSetClip(lr);
        GdiTextT(size_x, y + 6, fs, UI_TEXT2);
        GdiTextT(type_x, y + 6, ts, UI_TEXT2);
    }
    for (RamNode *f = e->dir ? e->dir->child : NULL; f; f = f->next, idx++) {
        n++;
        if (idx < top || idx > top + L.vis) continue;
        int y = lr.y + (idx - top) * ROW_H;
        if (idx == e->sel)
            GdiRoundRect(RECT(lx + 6, y + 1, L.cw - 12, ROW_H - 2), 4,
                         w->active ? UI_SELECT : UI_HOVER, GDI_TRANSPARENT);
        row_icon(f, lx + 18, y + 4);
        GdiSetClip(RECT(lr.x, lr.y, name_w, lr.h));
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

    /* The list's scroll bars, and the corner between them */
    GdiRect full = r_list(c);
    UiScrollDraw(&e->vbar, RECT(full.x + full.w - UI_SB_W, full.y, UI_SB_W, L.list.h), c);
    UiScrollDraw(&e->hbar, RECT(full.x, full.y + full.h - UI_SB_W, L.list.w, UI_SB_W), c);
    if (UiScrollNeeded(&e->vbar) && UiScrollNeeded(&e->hbar))
        GdiFillRect(RECT(c.x + full.x + L.list.w, c.y + full.y + L.list.h, UI_SB_W, UI_SB_W), UI_PANEL);

    /* Status bar */
    GdiFillRect(RECT(c.x + SIDE_W, c.y + c.h - STATUS_H, c.w - SIDE_W, STATUS_H), UI_PANEL);
    GdiFillRect(RECT(c.x + SIDE_W, c.y + c.h - STATUS_H, c.w - SIDE_W, 1), UI_LINE);
    char st[48];
    ksnprintf(st, sizeof(st), "%d item%s", n, n == 1 ? "" : "s");
    GdiTextT(c.x + SIDE_W + 12, c.y + c.h - STATUS_H + 5, st, UI_TEXT2);
    if (e->status[0]) GdiTextT(c.x + SIDE_W + 110, c.y + c.h - STATUS_H + 5, e->status, UI_TEXT3);
    log_view(w, e, &L);
}

/* -----------------------------------------------------------------------
 * Input
 * ----------------------------------------------------------------------- */
static void exp_mouse(WND *w, WmMouseMsg msg, int x, int y)
{
    Explorer *e = w->user;
    GdiRect c = WmClientRect(w);
    Layout L = layout(e, c);

    /* the scroll bars first: a press on one, and its drag until release */
    UiScroll *bars[3] = { &e->vbar, &e->hbar, &e->sbar };
    for (int i = 0; i < 3; i++)
        if (UiScrollMouse(bars[i], msg, x, y)) return;

    if (msg == WM_MOUSE_WHEEL || msg == WM_MOUSE_HWHEEL) {
        int d = WmWheelDelta();                    /* + = away from the user (up), or right */
        if (msg == WM_MOUSE_HWHEEL)    UiScrollTo(&e->hbar, e->hbar.pos + d * e->hbar.line * 2);
        else if (x < SIDE_W)           UiScrollTo(&e->sbar, e->sbar.pos - d * WHEEL_ROWS * SIDE_ROW);
        else if (UiScrollNeeded(&e->vbar) || !UiScrollNeeded(&e->hbar))
                                       UiScrollTo(&e->vbar, e->vbar.pos - d * WHEEL_ROWS);
        else                           UiScrollTo(&e->hbar, e->hbar.pos - d * e->hbar.line * 2);
        return;
    }

    if (msg == WM_MOUSE_UP) {
        if (UiHit(r_back(), x, y))        go_back(w, e);
        else if (UiHit(r_up(), x, y))     { if (e->dir) navigate(w, e, e->dir->parent); }
        else if (UiHit(r_copy(c), x, y))    clip_copy(e, false);
        else if (UiHit(r_paste(c), x, y))   clip_paste(e);
        else if (UiHit(r_newdir(c), x, y))  make_new(w, e, true);
        else if (UiHit(r_newfile(c), x, y)) make_new(w, e, false);
        else
            for (int i = 0; i < e->ncrumb; i++)
                if (UiHit(e->crumb_r[i], x, y)) { navigate(w, e, e->crumb_dir[i]); break; }
        return;
    }
    if (msg != WM_MOUSE_DOWN && msg != WM_MOUSE_DBLCLK) return;

    if (x < SIDE_W && y >= TB_H) {                 /* sidebar */
        int sy = y + e->sbar.pos;                   /* in its content */
        int i = (sy - TB_H - 8) / SIDE_ROW;
        if (sy - TB_H - 8 >= 0 && i < N_PLACES) {
            navigate(w, e, g_places[i].path ? RamfsResolve(NULL, g_places[i].path) : NULL);
            return;
        }
        char letters[26];
        int nd = sidebar_drives(letters);
        int k = (sy - TB_H - 16) / SIDE_ROW - N_PLACES;
        if (sy - TB_H - 16 >= 0 && k >= 0 && k < nd) navigate(w, e, RamfsDriveRoot(letters[k]));
        return;
    }
    if (UiHit(L.list, x, y)) {
        int idx = e->vbar.pos + (y - L.list.y) / ROW_H;
        if (idx < L.n) {
            e->sel = idx;
            ensure_visible(e, &L);                  /* (the part-shown row at the bottom) */
            if (msg == WM_MOUSE_DBLCLK) open_sel(w, e);
        } else {
            e->sel = -1;
        }
    }
}

/* A held scroll bar arrow or trough repeats */
static bool exp_tick(WND *w)
{
    Explorer *e = w->user;
    if (!e) return false;
    bool moved = UiScrollTick(&e->vbar);
    moved = UiScrollTick(&e->hbar) || moved;
    return UiScrollTick(&e->sbar) || moved;
}

static void exp_key(WND *w, const KeyEvent *k)
{
    Explorer *e = w->user;
    Layout L = layout(e, WmClientRect(w));
    int n = L.n;
    if (k->ctrl && (k->ch == 'c' || k->ch == 'C')) { clip_copy(e, false); return; }
    if (k->ctrl && (k->ch == 'x' || k->ch == 'X')) { clip_copy(e, true); return; }
    if (k->ctrl && (k->ch == 'v' || k->ch == 'V')) { clip_paste(e); return; }
    if (k->ctrl && (k->ch == 'a' || k->ch == 'A')) return;
    if (k->extended) {
        int top = e->vbar.pos, bottom = top + L.vis - 1;
        if (k->scancode == KEY_UP   && e->sel > 0)     e->sel--;
        if (k->scancode == KEY_DOWN && e->sel < n - 1) e->sel++;
        if (k->scancode == KEY_HOME && n) e->sel = 0;
        if (k->scancode == KEY_END  && n) e->sel = n - 1;
        /* Page Down: to the last row in view, then a page on (and Page Up
         * the same way up), as a Windows list view does */
        if (k->scancode == KEY_PGDN && n)
            e->sel = e->sel < bottom ? bottom : e->sel + L.vis - 1;
        if (k->scancode == KEY_PGUP && n)
            e->sel = e->sel > top ? top : e->sel - (L.vis - 1);
        if (e->sel >= n) e->sel = n - 1;
        if (e->sel < 0 && n && (k->scancode == KEY_PGUP || k->scancode == KEY_PGDN)) e->sel = 0;
        if (k->scancode == KEY_LEFT)  UiScrollTo(&e->hbar, e->hbar.pos - e->hbar.line);
        if (k->scancode == KEY_RIGHT) UiScrollTo(&e->hbar, e->hbar.pos + e->hbar.line);
        if (k->scancode == KEY_DELETE && e->dir && e->sel >= 0) {
            RamNode *victim = child_at(e->dir, e->sel);
            if (victim && RamfsDelete(victim) && e->sel >= RamfsCount(e->dir))
                e->sel = RamfsCount(e->dir) - 1;
            L = layout(e, WmClientRect(w));
        }
        if (k->scancode != KEY_LEFT && k->scancode != KEY_RIGHT) ensure_visible(e, &L);
        return;
    }
    if (k->ch == '\n')      open_sel(w, e);
    else if (k->ch == '\b' && e->dir) navigate(w, e, e->dir->parent);
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
    w->on_tick  = exp_tick;
    w->tick_lock_free = true;                 /* (the bars only: no files) */
    w->rbutton  = true;                       /* the wheel scrolls */
    set_dir(w, e, dir && dir->dir && dir != RamfsRoot() ? dir : NULL);   /* C:\\ itself opens This PC */
}
