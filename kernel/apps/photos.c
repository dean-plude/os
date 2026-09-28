/*
 * photos.c — Photos: a viewer for icons (.ico, .cur) and PNG pictures
 *
 * Opened on a file it shows the picture; for an icon it also lists every
 * image in the file (size and colour depth) in a strip, and the selected
 * one is shown enlarged pixel for pixel.  Opened on its own it shows the
 * pictures in C:\Pictures.  Left/Right step through the pictures of the
 * file's folder.
 */

#include "apps.h"
#include "../lib/string.h"
#include "../mm/vmm.h"
#include "../ke/printf.h"

#define TB_H     44
#define STRIP_H  112
#define TILE     132
#define THUMB    72

typedef struct {
    RamNode *dir;            /* the folder (gallery / stepping), referenced */
    RamNode *file;           /* the picture shown, or NULL: gallery; referenced */
    int      sel;            /* selected image of an icon */
    int      top;            /* first gallery row */
} Photos;

static bool ends_ci(const char *s, const char *ext)
{
    size_t n = strlen(s), e = strlen(ext);
    if (n < e) return false;
    for (size_t i = 0; i < e; i++)
        if ((s[n - e + i] | 0x20) != (ext[i] | 0x20)) return false;
    return true;
}

static bool is_picture(const RamNode *f)
{
    return f && !f->dir && (ends_ci(f->name, ".ico") || ends_ci(f->name, ".cur") || ends_ci(f->name, ".png"));
}

static GdiRect r_gallery_btn(void) { return RECT(10, 8, 120, 28); }
static GdiRect r_prev(GdiRect c)   { return RECT(c.w - 84, 8, 34, 28); }
static GdiRect r_next(GdiRect c)   { return RECT(c.w - 44, 8, 34, 28); }

static void set_file(WND *w, Photos *ph, RamNode *f)
{
    if (f) RamfsRef(f);
    RamfsUnref(ph->file);
    ph->file = f;
    ph->sel = -1;                               /* choose the largest on paint */
    char t[WM_TITLE_MAX];
    ksnprintf(t, sizeof(t), "%s - Photos", f ? f->name : "Pictures");
    WmSetTitle(w, t);
}

/* The next/previous picture in the folder, wrapping around */
static RamNode *step(Photos *ph, int dir)
{
    if (!ph->file || !ph->file->parent) return NULL;
    RamNode *list[128];
    int n = 0, cur = -1;
    for (RamNode *c = ph->file->parent->child; c && n < 128; c = c->next)
        if (is_picture(c)) {
            if (c == ph->file) cur = n;
            list[n++] = c;
        }
    if (n < 2 || cur < 0) return NULL;
    return list[(cur + dir + n) % n];
}

/* The largest (then deepest) image of an icon */
static int largest(GdiIcon *ic)
{
    int best = 0;
    for (int i = 1; i < ic->n; i++) {
        IconImage *a = &ic->img[i], *b = &ic->img[best];
        if (a->w * a->h > b->w * b->h || (a->w * a->h == b->w * b->h && a->bpp > b->bpp)) best = i;
    }
    return best;
}

static void checker(GdiRect r)
{
    GdiFillRect(r, GDI_C(0x30, 0x30, 0x30));
    for (int y = 0; y < r.h; y += 8)
        for (int x = (y / 8) % 2 * 8; x < r.w; x += 16)
            GdiFillRect(RECT(r.x + x, r.y + y, x + 8 <= r.w ? 8 : r.w - x, y + 8 <= r.h ? 8 : r.h - y),
                        GDI_C(0x38, 0x38, 0x38));
}

static void arrow(GdiRect b, bool right)
{
    int cx = b.x + b.w / 2, cy = b.y + b.h / 2, d = right ? 1 : -1;
    GdiLine((GdiPoint){ (cx - 2 * d) * 16, (cy - 5) * 16 }, (GdiPoint){ (cx + 3 * d) * 16, cy * 16 }, 22, UI_TEXT);
    GdiLine((GdiPoint){ (cx + 3 * d) * 16, cy * 16 }, (GdiPoint){ (cx - 2 * d) * 16, (cy + 5) * 16 }, 22, UI_TEXT);
}

static void paint_gallery(Photos *ph, GdiRect c)
{
    GdiRect area = RECT(c.x, c.y + TB_H, c.w, c.h - TB_H);
    int cols = (area.w - 16) / TILE;
    if (cols < 1) cols = 1;
    int i = 0, shown = 0;
    GdiSetClip(area);
    for (RamNode *f = ph->dir ? ph->dir->child : NULL; f; f = f->next) {
        if (!is_picture(f)) continue;
        int row = i / cols, col = i % cols;
        i++;
        if (row < ph->top) continue;
        int x = area.x + 16 + col * TILE, y = area.y + 16 + (row - ph->top) * TILE;
        if (y > area.y + area.h) continue;
        GdiRoundRect(RECT(x, y, TILE - 12, TILE - 12), 6, UI_CARD, GDI_TRANSPARENT);
        AppDrawNodeIcon(f, x + (TILE - 12 - THUMB) / 2, y + 12, THUMB);
        GdiSetClip(RECT(x + 4, y, TILE - 20, TILE));
        GdiTextCenter(x, y + THUMB + 22, TILE - 12, f->name, UI_TEXT2);
        GdiSetClip(area);
        shown++;
    }
    if (!i) GdiTextCenter(area.x, area.y + 60, area.w, "No pictures or icons in C:\\Pictures.", UI_TEXT3);
    GdiSetClip(c);
    (void)shown;
}

static void paint_file(Photos *ph, GdiRect c)
{
    GdiIcon *ic = AppFileIcon(ph->file);
    bool strip = ic && ic->n > 1;
    GdiRect view = RECT(c.x, c.y + TB_H, c.w, c.h - TB_H - (strip ? STRIP_H : 0));
    checker(view);
    if (!ic) {
        GdiTextCenter(view.x, view.y + view.h / 2 - 8, view.w, "This file is not a picture NovaOS can show.", UI_TEXT2);
        return;
    }
    if (ph->sel < 0 || ph->sel >= ic->n) ph->sel = largest(ic);

    /* The selected image: enlarged by a whole factor (pixel art stays
     * sharp) if it is small, else 1:1 or shrunk to fit */
    GdiRect box = RECT(view.x + 16, view.y + 16, view.w - 32, view.h - 32);
    if (IconDecode(ic, ph->sel)) {
        IconImage *im = &ic->img[ph->sel];
        int sc = GdiScale();
        int k = 1;
        while ((k + 1) * im->pw <= box.w * sc && (k + 1) * im->ph <= box.h * sc && (k + 1) * im->pw <= 256 * sc) k++;
        GdiSetClip(view);
        if (k > 1) {
            int wl = (k * im->pw + sc - 1) / sc, hl = (k * im->ph + sc - 1) / sc;
            GdiDrawImageZoom(box.x + (box.w - wl) / 2, box.y + (box.h - hl) / 2, im->px, im->pw, im->ph, k);
        } else {
            IconDrawFit(ic, ph->sel, box);
        }
        GdiSetClip(c);
    } else {
        GdiTextCenter(view.x, view.y + view.h / 2 - 8, view.w, "This image could not be decoded.", UI_TEXT2);
    }

    if (!strip) return;
    GdiRect sr = RECT(c.x, c.y + c.h - STRIP_H, c.w, STRIP_H);
    GdiFillRect(sr, UI_PANEL);
    GdiFillRect(RECT(sr.x, sr.y, sr.w, 1), UI_LINE);
    int cw = 104, x = sr.x + 12;
    GdiSetClip(sr);
    for (int i = 0; i < ic->n; i++, x += cw + 8) {
        IconImage *im = &ic->img[i];
        GdiRect cell = RECT(x, sr.y + 10, cw, STRIP_H - 20);
        GdiRoundRect(cell, 6, i == ph->sel ? UI_SELECT : UI_CARD, i == ph->sel ? UI_ACCENT : GDI_TRANSPARENT);
        IconDrawFit(ic, i, RECT(cell.x + 8, cell.y + 6, cell.w - 16, 48));
        char a[24], b[24];
        ksnprintf(a, sizeof(a), "%d x %d", im->pw ? im->pw : im->w, im->ph ? im->ph : im->h);
        ksnprintf(b, sizeof(b), "%d-bit%s", im->bpp, im->png ? " PNG" : "");
        GdiTextCenter(cell.x, cell.y + 56, cell.w, a, UI_TEXT);
        GdiTextCenter(cell.x, cell.y + 72, cell.w, im->bad ? "unreadable" : b, UI_TEXT3);
    }
    GdiSetClip(c);
}

static void ph_paint(WND *w)
{
    Photos *ph = w->user;
    GdiRect c = WmClientRect(w);

    GdiFillRect(RECT(c.x, c.y, c.w, TB_H), UI_PANEL);
    GdiFillRect(RECT(c.x, c.y + TB_H - 1, c.w, 1), UI_LINE);
    if (ph->file) {
        GdiRect g = r_gallery_btn();
        UiButton(RECT(c.x + g.x, c.y + g.y, g.w, g.h), "All pictures", false);
        GdiTextT(c.x + 146, c.y + 14, ph->file->name, UI_TEXT);
        GdiIcon *ic = AppFileIcon(ph->file);
        char info[48];
        if (ic && ends_ci(ph->file->name, ".png") && IconDecode(ic, 0))
            ksnprintf(info, sizeof(info), "PNG image  -  %d x %d", ic->img[0].pw, ic->img[0].ph);
        else if (ic)
            ksnprintf(info, sizeof(info), "%s  -  %d image%s", ends_ci(ph->file->name, ".cur") ? "Cursor" : "Icon",
                      ic->n, ic->n == 1 ? "" : "s");
        else
            info[0] = '\0';
        GdiTextT(c.x + c.w - 100 - GdiTextW(info), c.y + 14, info, UI_TEXT2);
        GdiRect p = r_prev(c), n = r_next(c);
        GdiRoundRect(RECT(c.x + p.x, c.y + p.y, p.w, p.h), 4, UI_CARD, UI_LINE);
        GdiRoundRect(RECT(c.x + n.x, c.y + n.y, n.w, n.h), 4, UI_CARD, UI_LINE);
        arrow(RECT(c.x + p.x, c.y + p.y, p.w, p.h), false);
        arrow(RECT(c.x + n.x, c.y + n.y, n.w, n.h), true);
        paint_file(ph, c);
    } else {
        char path[RAMFS_PATH_MAX];
        if (ph->dir) RamfsPath(ph->dir, path, sizeof(path)); else strcpy(path, "C:\\Pictures");
        GdiTextT(c.x + 16, c.y + 14, path, UI_TEXT);
        GdiFillRect(RECT(c.x, c.y + TB_H, c.w, c.h - TB_H), UI_BG);
        paint_gallery(ph, c);
    }
}

static void ph_mouse(WND *w, WmMouseMsg msg, int x, int y)
{
    Photos *ph = w->user;
    GdiRect c = WmClientRect(w);
    if (msg != WM_MOUSE_DOWN && msg != WM_MOUSE_DBLCLK) return;

    if (ph->file) {
        if (UiHit(r_gallery_btn(), x, y)) { set_file(w, ph, NULL); return; }
        if (UiHit(r_prev(c), x, y)) { RamNode *f = step(ph, -1); if (f) set_file(w, ph, f); return; }
        if (UiHit(r_next(c), x, y)) { RamNode *f = step(ph, 1); if (f) set_file(w, ph, f); return; }
        GdiIcon *ic = AppFileIcon(ph->file);
        if (ic && ic->n > 1 && y >= c.h - STRIP_H) {
            int i = (x - 12) / (104 + 8);
            if (x >= 12 && i < ic->n && (x - 12) % (104 + 8) < 104) ph->sel = i;
        }
        return;
    }
    /* gallery: a click opens the picture */
    int cols = (c.w - 16) / TILE;
    if (cols < 1) cols = 1;
    if (x < 16 || y < TB_H + 16) return;
    int col = (x - 16) / TILE, row = (y - TB_H - 16) / TILE + ph->top;
    if (col >= cols || (x - 16) % TILE >= TILE - 12) return;
    int want = row * cols + col, i = 0;
    for (RamNode *f = ph->dir ? ph->dir->child : NULL; f; f = f->next) {
        if (!is_picture(f)) continue;
        if (i++ == want) { AppNoteRecentFile(f); set_file(w, ph, f); return; }
    }
}

static void ph_key(WND *w, const KeyEvent *k)
{
    Photos *ph = w->user;
    if (!k->extended) {
        if (k->scancode == KEY_ESC && ph->file) set_file(w, ph, NULL);
        return;
    }
    if (ph->file && (k->scancode == KEY_LEFT || k->scancode == KEY_RIGHT)) {
        RamNode *f = step(ph, k->scancode == KEY_RIGHT ? 1 : -1);
        if (f) set_file(w, ph, f);
    } else if (!ph->file && k->scancode == KEY_DOWN) {
        ph->top++;
    } else if (!ph->file && k->scancode == KEY_UP && ph->top > 0) {
        ph->top--;
    }
}

static void ph_close(WND *w)
{
    Photos *ph = w->user;
    RamfsUnref(ph->file);
    RamfsUnref(ph->dir);
    kfree(ph);
    w->user = NULL;
}

void PhotosOpen(RamNode *file)
{
    Photos *ph = kzalloc(sizeof(Photos));
    if (!ph) return;
    WND *w = AppCreateWindow(APP_PHOTOS, "Photos", 760, 540, UI_BG);
    if (!w) { kfree(ph); return; }
    w->user     = ph;
    w->on_paint = ph_paint;
    w->on_mouse = ph_mouse;
    w->on_key   = ph_key;
    w->on_close = ph_close;
    ph->dir = file && file->parent ? file->parent : RamfsResolve(NULL, "\\Pictures");
    if (ph->dir) RamfsRef(ph->dir);
    set_file(w, ph, file);
}
