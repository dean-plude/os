/*
 * notepad.c — Notepad: a plain-text editor backed by the RAM disk
 *
 * The text is one buffer with '\n' line breaks and a caret offset.  Lines
 * are drawn in Cascadia Mono, so columns map directly to cell positions.
 */

#include "apps.h"
#include "../lib/string.h"
#include "../mm/vmm.h"
#include "../ke/printf.h"
#include "../wm/clipboard.h"

#define NP_PAD     12
#define NP_LINE_H  18
#define NP_BAR_H   36          /* toolbar */
#define NP_STAT_H  26          /* status bar */
#define NP_BG      GDI_C(0x1F, 0x1F, 0x1F)
#define NP_MAX     (64u * 1024u)      /* editable text size */

typedef struct {
    char    *text;             /* NP_MAX bytes */
    UINT32   len;
    UINT32   caret;
    UINT32   anchor;           /* the other end of the selection (== caret: none) */
    int      top;              /* first visible line */
    int      want_col;         /* column kept while moving up/down */
    bool     dirty;
    RamNode *file;             /* NULL until first save */
    bool     readonly;         /* file larger than NP_MAX: view only */
    char     status[64];       /* transient message ("Saved") */
} Notepad;

/* -----------------------------------------------------------------------
 * Text geometry
 * ----------------------------------------------------------------------- */
static UINT32 line_start(const Notepad *n, UINT32 pos)
{
    while (pos > 0 && n->text[pos - 1] != '\n') pos--;
    return pos;
}

static UINT32 line_end(const Notepad *n, UINT32 pos)
{
    while (pos < n->len && n->text[pos] != '\n') pos++;
    return pos;
}

static int line_of(const Notepad *n, UINT32 pos)
{
    int l = 0;
    for (UINT32 i = 0; i < pos; i++) if (n->text[i] == '\n') l++;
    return l;
}

static UINT32 pos_of_line(const Notepad *n, int line)
{
    UINT32 p = 0;
    while (line > 0 && p < n->len) { if (n->text[p++] == '\n') line--; }
    return p;
}

static int total_lines(const Notepad *n) { return line_of(n, n->len) + 1; }

static GdiRect text_area(WND *w)
{
    GdiRect c = WmClientRect(w);
    return RECT(c.x, c.y + NP_BAR_H, c.w, c.h - NP_BAR_H - NP_STAT_H);
}

static int visible_lines(WND *w)
{
    int v = (text_area(w).h - 2 * 8) / NP_LINE_H;
    return v < 1 ? 1 : v;
}

static void keep_caret_visible(WND *w, Notepad *n)
{
    int l = line_of(n, n->caret), vis = visible_lines(w);
    if (l < n->top) n->top = l;
    if (l >= n->top + vis) n->top = l - vis + 1;
}

/* -----------------------------------------------------------------------
 * File handling
 * ----------------------------------------------------------------------- */
static void update_title(WND *w, Notepad *n)
{
    char t[WM_TITLE_MAX];
    ksnprintf(t, sizeof(t), "%s%s - Notepad", n->dirty ? "*" : "",
              n->file ? n->file->name : "Untitled");
    WmSetTitle(w, t);
}

static void save(WND *w, Notepad *n)
{
    if (n->readonly) {
        ksnprintf(n->status, sizeof(n->status), "Read-only: file is too large to edit");
        return;
    }
    if (!n->file) {
        RamNode *docs = RamfsResolve(NULL, "\\Documents");
        if (!docs) docs = RamfsRoot();
        char name[RAMFS_NAME_MAX];
        for (int i = 1; i < 100; i++) {
            if (i == 1) ksnprintf(name, sizeof(name), "Untitled.txt");
            else        ksnprintf(name, sizeof(name), "Untitled (%d).txt", i);
            if (!RamfsFind(docs, name)) break;
        }
        n->file = RamfsCreate(docs, name, false);
        RamfsRef(n->file);
    }
    if (n->file && RamfsWrite(n->file, n->text, n->len)) {
        char path[RAMFS_PATH_MAX];
        RamfsPath(n->file, path, sizeof(path));
        n->dirty = false;
        ksnprintf(n->status, sizeof(n->status), "Saved to %s", path);
    } else {
        ksnprintf(n->status, sizeof(n->status), "Could not save the file");
    }
    update_title(w, n);
}

/* -----------------------------------------------------------------------
 * Editing
 * ----------------------------------------------------------------------- */
static void insert(Notepad *n, const char *s, UINT32 k)
{
    if (n->readonly || n->len + k > NP_MAX) return;
    memmove(n->text + n->caret + k, n->text + n->caret, n->len - n->caret);
    memcpy(n->text + n->caret, s, k);
    n->len += k;
    n->caret += k;
    n->dirty = true;
}

static void erase(Notepad *n, UINT32 at, UINT32 k)
{
    if (n->readonly || at + k > n->len) return;
    memmove(n->text + at, n->text + at + k, n->len - at - k);
    n->len -= k;
    n->dirty = true;
}

/* The selection as [a, b) */
static bool sel_range(const Notepad *n, UINT32 *a, UINT32 *b)
{
    if (n->anchor > n->len) return false;
    *a = n->anchor < n->caret ? n->anchor : n->caret;
    *b = n->anchor < n->caret ? n->caret : n->anchor;
    return *a != *b;
}

static void delete_selection(Notepad *n)
{
    UINT32 a, b;
    if (!sel_range(n, &a, &b) || n->readonly) return;
    erase(n, a, b - a);
    n->caret = n->anchor = a;
}

static void copy_selection(Notepad *n, bool cut)
{
    UINT32 a, b;
    if (!sel_range(n, &a, &b)) return;
    ClipSetText(n->text + a, b - a);
    if (cut && !n->readonly) delete_selection(n);
    ksnprintf(n->status, sizeof(n->status), cut ? "Cut %u characters" : "Copied %u characters", b - a);
}

static void paste(Notepad *n)
{
    UINT32 len;
    char *t = ClipGetText(&len);
    if (!t) { ksnprintf(n->status, sizeof(n->status), "The clipboard has no text"); return; }
    if (n->readonly) { kfree(t); return; }
    delete_selection(n);
    UINT32 o = 0;
    for (UINT32 i = 0; i < len; i++) if (t[i] != '\r') t[o++] = t[i];    /* Windows line ends -> '\n' */
    if (n->len + o > NP_MAX) o = NP_MAX - n->len;
    insert(n, t, o);
    n->anchor = n->caret;
    kfree(t);
}

static void move_vert(Notepad *n, int dir)
{
    int line = line_of(n, n->caret) + dir;
    if (line < 0 || line >= total_lines(n)) return;
    UINT32 s = pos_of_line(n, line), e = line_end(n, s);
    UINT32 p = s + (UINT32)n->want_col;
    n->caret = p > e ? e : p;
}

static void np_key(WND *w, const KeyEvent *k)
{
    Notepad *n = w->user;
    bool was_dirty = n->dirty;
    n->status[0] = '\0';

    bool vertical = false;
    if (k->ctrl && (k->ch == 's' || k->ch == 'S')) { save(w, n); return; }
    if (k->ctrl && (k->ch == 'a' || k->ch == 'A')) { n->anchor = 0; n->caret = n->len; keep_caret_visible(w, n); return; }
    if (k->ctrl && (k->ch == 'c' || k->ch == 'C')) { copy_selection(n, false); return; }
    if (k->ctrl && (k->ch == 'x' || k->ch == 'X')) { copy_selection(n, true); goto done; }
    if (k->ctrl && (k->ch == 'v' || k->ch == 'V')) { paste(n); goto done; }

    UINT32 a, b;
    bool had_sel = sel_range(n, &a, &b);
    if (k->extended && k->scancode != KEY_DELETE) {
        if (!k->shift && had_sel && (k->scancode == KEY_LEFT || k->scancode == KEY_RIGHT)) {
            n->caret = n->anchor = k->scancode == KEY_LEFT ? a : b;   /* to that end of it */
            goto done;
        }
        if (!k->shift || n->anchor > n->len) n->anchor = n->caret;
    }
    if (k->extended) {
        switch (k->scancode) {
        case KEY_LEFT:   if (n->caret > 0) n->caret--; break;
        case KEY_RIGHT:  if (n->caret < n->len) n->caret++; break;
        case KEY_UP:     move_vert(n, -1); vertical = true; break;
        case KEY_DOWN:   move_vert(n, +1); vertical = true; break;
        case KEY_HOME:   n->caret = line_start(n, n->caret); break;
        case KEY_END:    n->caret = line_end(n, n->caret); break;
        case KEY_PGUP:   for (int i = 0; i < visible_lines(w); i++) move_vert(n, -1); vertical = true; break;
        case KEY_PGDN:   for (int i = 0; i < visible_lines(w); i++) move_vert(n, +1); vertical = true; break;
        case KEY_DELETE:
            if (had_sel) delete_selection(n);
            else if (n->caret < n->len) erase(n, n->caret, 1);
            n->anchor = n->caret;
            break;
        }
        if (!k->shift) n->anchor = n->caret;
    } else if (k->ch == '\b') {
        if (had_sel) delete_selection(n);
        else if (n->caret > 0) { n->caret--; erase(n, n->caret, 1); }
        n->anchor = n->caret;
    } else if (k->ch == '\t') {
        delete_selection(n);
        insert(n, "    ", 4);
        n->anchor = n->caret;
    } else if (k->ch == '\n' || (k->ch >= ' ' && k->ch <= '~')) {
        if (!k->ctrl && !k->alt) { delete_selection(n); insert(n, &k->ch, 1); n->anchor = n->caret; }
    }
done:
    if (!vertical) n->want_col = (int)(n->caret - line_start(n, n->caret));
    keep_caret_visible(w, n);
    if (n->dirty != was_dirty) update_title(w, n);
}

static GdiRect r_save(GdiRect c)  { return RECT(c.x + 10, c.y + 4, 84, 28); }
static GdiRect r_copy(GdiRect c)  { return RECT(c.x + 100, c.y + 4, 70, 28); }
static GdiRect r_paste(GdiRect c) { return RECT(c.x + 176, c.y + 4, 70, 28); }

static void np_mouse(WND *w, WmMouseMsg msg, int x, int y)
{
    Notepad *n = w->user;
    GdiRect c = WmClientRect(w);
    if (msg == WM_MOUSE_UP && UiHit(r_save(c), x + c.x, y + c.y)) { save(w, n); return; }
    if (msg == WM_MOUSE_UP && UiHit(r_copy(c), x + c.x, y + c.y)) { n->status[0] = 0; copy_selection(n, false); return; }
    if (msg == WM_MOUSE_UP && UiHit(r_paste(c), x + c.x, y + c.y)) {
        bool was = n->dirty;
        n->status[0] = 0;
        paste(n);
        keep_caret_visible(w, n);
        if (n->dirty != was) update_title(w, n);
        return;
    }
    if (msg == WM_MOUSE_DBLCLK && y >= NP_BAR_H) {        /* a word */
        UINT32 p = n->caret, e = n->caret;
        #define WORDC(ch) (((ch) >= '0' && (ch) <= '9') || (((ch) | 0x20) >= 'a' && ((ch) | 0x20) <= 'z') || (ch) == '_')
        while (p > 0 && WORDC(n->text[p - 1])) p--;
        while (e < n->len && WORDC(n->text[e])) e++;
        n->anchor = p;
        n->caret = e;
        return;
    }
    if (msg != WM_MOUSE_DOWN && msg != WM_MOUSE_MOVE) return;
    if (y < NP_BAR_H) return;

    /* Place the caret at the clicked cell */
    GdiRect ta = text_area(w);
    int line = n->top + (y + c.y - ta.y - 8) / NP_LINE_H;
    if (line < 0) line = 0;
    if (line >= total_lines(n)) line = total_lines(n) - 1;
    int col = ((x - NP_PAD) * 256 + GdiMonoCellW256() / 2) / GdiMonoCellW256();
    if (col < 0) col = 0;
    UINT32 s = pos_of_line(n, line), e = line_end(n, s);
    n->caret = s + (UINT32)col > e ? e : s + (UINT32)col;
    if (msg == WM_MOUSE_DOWN && !(InputModifiers() & 1u)) n->anchor = n->caret;   /* drags extend it */
    n->want_col = (int)(n->caret - s);
}

static void np_paint(WND *w)
{
    Notepad *n = w->user;
    GdiRect c = WmClientRect(w);
    int cell = GdiMonoCellW256();

    /* Toolbar */
    GdiFillRect(RECT(c.x, c.y, c.w, NP_BAR_H), UI_PANEL);
    UiButton(r_save(c), "Save", false);
    UiButton(r_copy(c), "Copy", false);
    UiButton(r_paste(c), "Paste", false);
    GdiTextT(c.x + 262, c.y + 10, "Ctrl+S save,  Ctrl+C / X / V", UI_TEXT3);
    GdiFillRect(RECT(c.x, c.y + NP_BAR_H - 1, c.w, 1), UI_LINE);

    /* Text */
    GdiRect ta = text_area(w);
    GdiSetClip(ta);
    int vis = visible_lines(w);
    UINT32 p = pos_of_line(n, n->top);
    int caret_line = line_of(n, n->caret);
    for (int l = n->top; l < n->top + vis + 1 && p <= n->len; l++) {
        UINT32 e = line_end(n, p);
        int y = ta.y + 8 + (l - n->top) * NP_LINE_H;
        UINT32 sa, sb;
        if (sel_range(n, &sa, &sb) && sa <= e && sb > p) {      /* the selected part of this line */
            UINT32 f = sa > p ? sa : p, t = sb < e ? sb : e;
            int x0 = ta.x + NP_PAD + ((int)(f - p) * cell) / 256;
            int x1 = ta.x + NP_PAD + ((int)(t - p) * cell) / 256 + (sb > e ? cell / 512 + 3 : 0);
            GdiAlphaFill(RECT(x0, y, x1 - x0, NP_LINE_H), UI_ACCENT, 110);
        }
        GdiTextMonoN(ta.x + NP_PAD, y, n->text + p, (int)(e - p), UI_TEXT);
        if (l == caret_line && w->active) {
            int col = (int)(n->caret - p);
            GdiFillRect(RECT(ta.x + NP_PAD + (col * cell) / 256, y, 2, 17), UI_ACCENT);
        }
        if (e >= n->len) break;
        p = e + 1;
    }
    GdiSetClip(c);

    /* Status bar */
    int sy = c.y + c.h - NP_STAT_H;
    GdiFillRect(RECT(c.x, sy, c.w, NP_STAT_H), UI_PANEL);
    GdiFillRect(RECT(c.x, sy, c.w, 1), UI_LINE);
    char st[64];
    ksnprintf(st, sizeof(st), "Ln %d, Col %d", caret_line + 1,
              (int)(n->caret - line_start(n, n->caret)) + 1);
    GdiTextT(c.x + 12, sy + 5, n->status[0] ? n->status : st, UI_TEXT2);
    GdiTextT(c.x + c.w - 120, sy + 5, "Plain text   LF", UI_TEXT3);
}

static void np_close(WND *w)
{
    Notepad *n = w->user;
    RamfsUnref(n->file);
    kfree(n->text);
    kfree(n);
    w->user = NULL;
}

void NotepadOpen(RamNode *file)
{
    Notepad *n = kzalloc(sizeof(Notepad));
    if (!n) return;
    n->anchor = 0;
    n->text = kmalloc(NP_MAX);
    if (!n->text) { kfree(n); return; }
    if (file && !file->dir) {
        n->file = file;
        RamfsRef(file);
        n->len = file->size;
        if (n->len > NP_MAX) {             /* show the start, never truncate */
            n->len = NP_MAX;
            n->readonly = true;
            ksnprintf(n->status, sizeof(n->status),
                      "Too large to edit: showing the first 64 KB (read-only)");
        }
        if (n->len) memcpy(n->text, file->data, n->len);
    }
    WND *w = AppCreateWindow(APP_NOTEPAD, "Notepad", 700, 460, NP_BG);
    if (!w) { RamfsUnref(n->file); kfree(n->text); kfree(n); return; }
    w->user     = n;
    w->on_paint = np_paint;
    w->on_key   = np_key;
    w->on_mouse = np_mouse;
    w->on_close = np_close;
    update_title(w, n);
}
