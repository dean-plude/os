/*
 * setup.c — "Install NovaOS": the installer's window
 *
 * Welcome, choose a disk, confirm that it will be erased (and choose drive
 * C:'s file system: NTFS, which keeps file permissions, or FAT32), watch
 * the progress, then restart into the installed system.  The work happens in
 * fs/setup.c on its own thread; this window polls its status.
 */

#include "apps.h"
#include "../lib/string.h"
#include "../mm/vmm.h"
#include "../ke/printf.h"
#include "../fs/setup.h"
#include "../fs/persist.h"
#include "../wm/desktop.h"

enum { PG_WELCOME, PG_DISK, PG_CONFIRM, PG_PROGRESS, PG_DONE, PG_FAILED };

#define W        640
#define H        440
#define SIDE_W   200
#define PAD      28
#define ROW_H    64
#define BTN_W    132
#define BTN_H    32
#define MAX_DISKS 8

typedef struct {
    int       page;
    SetupDisk disks[MAX_DISKS];
    int       ndisks;
    int       sel;                 /* chosen disk, or -1 */
    bool      fat;                 /* drive C: on FAT32 (else NTFS) */
    SetupStatus st;
} Setup;

static WND *g_setup;

/* ---------------------------------------------------------------------------
 * Layout (client-relative)
 * ------------------------------------------------------------------------- */
static GdiRect r_next(GdiRect c) { return RECT(c.w - PAD - BTN_W, c.h - PAD - BTN_H, BTN_W, BTN_H); }
static GdiRect r_back(GdiRect c) { return RECT(c.w - PAD - 2 * BTN_W - 12, c.h - PAD - BTN_H, BTN_W, BTN_H); }
static GdiRect r_row(int i)      { return RECT(SIDE_W + PAD, 104 + i * (ROW_H + 8), W - SIDE_W - 2 * PAD, ROW_H); }
static GdiRect r_fs(int i)       { return RECT(SIDE_W + PAD, PAD + 170 + i * 28, W - SIDE_W - 2 * PAD, 26); }

static GdiRect off(GdiRect r, GdiRect c) { r.x += c.x; r.y += c.y; return r; }

static void format_bytes(UINT64 b, char *out, int cap)
{
    if (b >= (1ull << 30)) ksnprintf(out, (size_t)cap, "%u.%u GB", (unsigned)(b >> 30), (unsigned)((b % (1ull << 30)) * 10 >> 30));
    else                   ksnprintf(out, (size_t)cap, "%u MB", (unsigned)(b >> 20));
}

static bool disk_usable(const SetupDisk *d) { return !d->too_small; }

/* ---------------------------------------------------------------------------
 * Painting
 * ------------------------------------------------------------------------- */
static void drive_icon(int x, int y, int s, bool accent)
{
    GdiRoundGradV(RECT(x, y + s / 4, s, s / 2), s / 8, GDI_C(0xB8, 0xBE, 0xC8), GDI_C(0x7A, 0x82, 0x90));
    GdiFillRect(RECT(x + s / 8, y + s / 4 + s / 3, s * 3 / 4, 1), GDI_C(0x5A, 0x60, 0x6C));
    GdiFillCircle(x + s * 3 / 4, y + s / 2 + s / 10, s / 16 + 1, accent ? UI_ACCENT : GDI_C(0x4C, 0xD0, 0x6A));
}

static void button(GdiRect r, const char *label, bool primary, bool enabled)
{
    if (enabled) { UiButton(r, label, primary); return; }
    GdiRoundRect(r, 6, UI_CARD, GDI_TRANSPARENT);
    GdiTextCenter(r.x, r.y + (r.h - GDI_FONT_H) / 2, r.w, label, UI_TEXT3);
}

static int wrap(int x, int y, int w, const char *text, GdiColor col)
{
    /* a few lines of wrapped text */
    char line[160];
    const char *p = text;
    while (*p) {
        int n = 0, last_space = -1;
        while (p[n] && p[n] != '\n' && n < (int)sizeof(line) - 1) {
            line[n] = p[n];
            line[n + 1] = '\0';
            if (p[n] == ' ') last_space = n;
            if (GdiTextW(line) > w && last_space > 0) { n = last_space; break; }
            n++;
        }
        line[n] = '\0';
        GdiTextT(x, y, line, col);
        y += 22;
        p += n;
        if (*p == '\n') y += 10;                 /* a new paragraph */
        while (*p == ' ' || *p == '\n') p++;
    }
    return y;
}

static void paint_side(Setup *s, GdiRect c)
{
    GdiFillRect(RECT(c.x, c.y, SIDE_W, c.h), UI_PANEL);
    AppDrawIcon(APP_SETUP, c.x + 24, c.y + 28, 48);
    GdiTextBold(c.x + 24, c.y + 92, "NovaOS Setup", UI_TEXT);
    static const char *steps[] = { "Welcome", "Choose a disk", "Confirm", "Install", "Finish" };
    int cur = s->page == PG_FAILED ? 3 : s->page > 4 ? 4 : s->page;
    for (int i = 0; i < 5; i++) {
        int y = c.y + 140 + i * 30;
        bool done = i < cur, now = i == cur;
        GdiFillCircle(c.x + 30, y + 8, 5, now ? UI_ACCENT : done ? UI_TEXT2 : UI_LINE);
        GdiTextT(c.x + 46, y, steps[i], now ? UI_TEXT : done ? UI_TEXT2 : UI_TEXT3);
    }
}

static void setup_paint(WND *w)
{
    Setup *s = w->user;
    GdiRect c = WmClientRect(w);
    paint_side(s, c);
    int x = c.x + SIDE_W + PAD, y = c.y + PAD, tw = W - SIDE_W - 2 * PAD;
    char buf[256], sz[24];

    switch (s->page) {
    case PG_WELCOME: {
        GdiTextLarge(x, y, "Install NovaOS", UI_TEXT);
        int ny;
        if (SetupIsLive()) {
            ksnprintf(buf, sizeof(buf), "You are running NovaOS from the installation %s. Everything works, but "
                      "nothing you do is kept once the PC restarts.\n"
                      "Setup copies NovaOS to a disk in this PC so it starts from there, with "
                      "drive C: saved on the same disk.", SetupMediaName());
            ny = wrap(x, y + 48, tw, buf, UI_TEXT2);
        } else
            ny = wrap(x, y + 48, tw, "Setup copies this NovaOS to another disk, so that disk can start the PC on "
                                "its own, with drive C: saved on it.", UI_TEXT2);
        wrap(x, ny + 14, tw, "The disk you choose is erased. It needs at least 256 MB.", UI_TEXT3);
        button(off(r_next(c), c), "Next", true, true);
        button(off(r_back(c), c), "Cancel", false, true);
        break; }

    case PG_DISK:
        GdiTextLarge(x, y, "Choose a disk", UI_TEXT);
        GdiTextT(x, y + 40, "NovaOS goes on the disk you pick. Everything on it is erased.", UI_TEXT2);
        if (!s->ndisks)
            wrap(x, y + 90, tw, "No disks were found. NovaOS installs on SATA (AHCI), NVMe and USB disks; "
                                "add one and start Setup again.", UI_TEXT2);
        for (int i = 0; i < s->ndisks; i++) {
            const SetupDisk *d = &s->disks[i];
            GdiRect r = off(r_row(i), c);
            bool usable = disk_usable(d);
            GdiRoundRect(r, 6, i == s->sel ? UI_SELECT : UI_CARD, GDI_TRANSPARENT);
            if (i == s->sel) GdiRoundBorderAlpha(r, 6, UI_ACCENT, 160);
            drive_icon(r.x + 14, r.y + 12, 40, i == s->sel);
            format_bytes(d->bytes, sz, sizeof(sz));
            ksnprintf(buf, sizeof(buf), "%s  -  %s", d->dev->model[0] ? d->dev->model : "Disk", sz);
            GdiTextBold(r.x + 68, r.y + 12, buf, usable ? UI_TEXT : UI_TEXT3);
            if (d->too_small)  ksnprintf(buf, sizeof(buf), "%s: too small (needs 256 MB)", d->dev->name);
            else if (d->boot)  ksnprintf(buf, sizeof(buf), "%s: NovaOS started from this disk", d->dev->name);
            else if (d->holds_c) ksnprintf(buf, sizeof(buf), "%s: drive C: is kept here now", d->dev->name);
            else               ksnprintf(buf, sizeof(buf), "%s: %s", d->dev->name, d->contents);
            GdiSetClip(RECT(r.x, r.y, r.w - 12, r.h));
            GdiTextT(r.x + 68, r.y + 36, buf, UI_TEXT3);
            GdiSetClip(c);
        }
        button(off(r_next(c), c), "Next", true, s->sel >= 0);
        button(off(r_back(c), c), "Back", false, true);
        break;

    case PG_CONFIRM: {
        const SetupDisk *d = &s->disks[s->sel];
        GdiTextLarge(x, y, "Erase this disk?", UI_TEXT);
        GdiRect card = RECT(x, y + 52, tw, 76);
        GdiRoundRect(card, 6, UI_CARD, GDI_TRANSPARENT);
        drive_icon(card.x + 14, card.y + 18, 40, true);
        format_bytes(d->bytes, sz, sizeof(sz));
        ksnprintf(buf, sizeof(buf), "%s  -  %s", d->dev->model[0] ? d->dev->model : "Disk", sz);
        GdiTextBold(card.x + 68, card.y + 18, buf, UI_TEXT);
        ksnprintf(buf, sizeof(buf), "%s: %s", d->dev->name, d->contents);
        GdiTextT(card.x + 68, card.y + 42, buf, UI_TEXT3);
        GdiTextT(x, y + 144, "Drive C: is kept as", UI_TEXT2);
        static const char *const fs[2] = { "NTFS: keeps file permissions (recommended)",
                                           "FAT32: simpler, read by any system" };
        for (int i = 0; i < 2; i++) {
            GdiRect r = off(r_fs(i), c);
            bool on = s->fat == (i == 1);
            GdiFillCircle(r.x + 8, r.y + 12, 7, on ? UI_ACCENT : UI_LINE);
            if (on) GdiFillCircle(r.x + 8, r.y + 12, 3, UI_BG);
            GdiTextT(r.x + 24, r.y + 4, fs[i], on ? UI_TEXT : UI_TEXT2);
        }
        int ny = wrap(x, y + 236, tw, "Everything on this disk will be deleted. This cannot be undone.",
                      GDI_C(0xFF, 0xB0, 0x60));
        wrap(x, ny + 12, tw, d->holds_c || !PersistActive()
                               ? "The files you have on drive C: now are copied to the new installation."
                               : "Drive C: stays saved on the disk it is on now.", UI_TEXT2);
        button(off(r_next(c), c), "Erase and install", true, true);
        button(off(r_back(c), c), "Back", false, true);
        break; }

    case PG_PROGRESS: {
        GdiTextLarge(x, y, "Installing NovaOS", UI_TEXT);
        GdiTextT(x, y + 40, "This takes a moment. Don't turn off the PC.", UI_TEXT2);
        GdiRect bar = RECT(x, y + 110, tw, 8);
        GdiRoundRect(bar, 4, UI_CARD, GDI_TRANSPARENT);
        int fill = tw * s->st.percent / 100;
        if (fill > 8) GdiRoundRect(RECT(bar.x, bar.y, fill, bar.h), 4, UI_ACCENT, GDI_TRANSPARENT);
        GdiTextT(x, y + 132, s->st.step, UI_TEXT2);
        ksnprintf(buf, sizeof(buf), "%d%%", s->st.percent);
        GdiTextT(x + tw - GdiTextW(buf), y + 132, buf, UI_TEXT3);
        break; }

    case PG_DONE: {
        const SetupDisk *d = &s->disks[s->sel];
        GdiTextLarge(x, y, "NovaOS is installed", UI_TEXT);
        ksnprintf(buf, sizeof(buf), "NovaOS is on %s (%s). ", d->dev->name, d->dev->model[0] ? d->dev->model : "disk");
        char msg[512];
        ksnprintf(msg, sizeof(msg), "%s%s%s", buf,
                  s->st.moved_c ? "Your files from this session were copied to it, and drive C: is saved there from now on.\n"
                                : "Drive C: will be saved there once you start from it.\n",
                  !SetupIsLive() ? "Restart and choose that disk in the firmware's boot menu to start from it."
                  : strcmp(SetupMediaName(), "disc") ? "Remove the installation USB stick, then restart to start NovaOS from the disk."
                  : "Remove the installation disc, then restart to start NovaOS from the disk.");
        wrap(x, y + 48, tw, msg, UI_TEXT2);
        button(off(r_next(c), c), "Restart now", true, true);
        button(off(r_back(c), c), "Close", false, true);
        break; }

    case PG_FAILED:
        GdiTextLarge(x, y, "NovaOS could not be installed", UI_TEXT);
        wrap(x, y + 48, tw, s->st.error[0] ? s->st.error : "Something went wrong.", GDI_C(0xFF, 0x8A, 0x80));
        wrap(x, y + 120, tw, "The disk may be left partly written. You can run Setup again.", UI_TEXT3);
        button(off(r_next(c), c), "Close", true, true);
        break;
    }
}

/* ---------------------------------------------------------------------------
 * Input
 * ------------------------------------------------------------------------- */
static void refresh_disks(Setup *s)
{
    s->ndisks = SetupListDisks(s->disks, MAX_DISKS);
    s->sel = -1;
    for (int i = 0; i < s->ndisks && s->sel < 0; i++)
        if (disk_usable(&s->disks[i]) && !s->disks[i].boot) s->sel = i;
}

static void next(WND *w)
{
    Setup *s = w->user;
    switch (s->page) {
    case PG_WELCOME:  refresh_disks(s); s->page = PG_DISK; break;
    case PG_DISK:     if (s->sel >= 0) s->page = PG_CONFIRM; break;
    case PG_CONFIRM:
        if (SetupStart(s->disks[s->sel].dev, !s->fat)) { s->page = PG_PROGRESS; SetupGetStatus(&s->st); }
        else { SetupGetStatus(&s->st); s->page = PG_FAILED; }
        break;
    case PG_DONE:     DesktopRestart(); break;
    case PG_FAILED:   WmDestroyWindow(w); break;
    }
}

static void back(WND *w)
{
    Setup *s = w->user;
    switch (s->page) {
    case PG_WELCOME: WmDestroyWindow(w); break;
    case PG_DISK:    s->page = PG_WELCOME; break;
    case PG_CONFIRM: s->page = PG_DISK; break;
    case PG_DONE:    WmDestroyWindow(w); break;
    }
}

static void setup_mouse(WND *w, WmMouseMsg msg, int x, int y)
{
    Setup *s = w->user;
    GdiRect c = WmClientRect(w);
    GdiRect cl = RECT(0, 0, c.w, c.h);
    if (msg != WM_MOUSE_UP && msg != WM_MOUSE_DBLCLK && msg != WM_MOUSE_DOWN) return;
    if (s->page == PG_DISK && (msg == WM_MOUSE_DOWN || msg == WM_MOUSE_DBLCLK)) {
        for (int i = 0; i < s->ndisks; i++)
            if (UiHit(r_row(i), x, y) && disk_usable(&s->disks[i])) {
                s->sel = i;
                if (msg == WM_MOUSE_DBLCLK) next(w);
                return;
            }
    }
    if (s->page == PG_CONFIRM && msg == WM_MOUSE_DOWN)
        for (int i = 0; i < 2; i++)
            if (UiHit(r_fs(i), x, y)) { s->fat = i == 1; return; }
    if (msg != WM_MOUSE_UP || s->page == PG_PROGRESS) return;
    bool has_back = s->page != PG_FAILED;
    if (UiHit(r_next(cl), x, y)) { if (s->page != PG_DISK || s->sel >= 0) next(w); }
    else if (has_back && UiHit(r_back(cl), x, y)) back(w);
}

static void setup_key(WND *w, const KeyEvent *k)
{
    Setup *s = w->user;
    if (s->page == PG_PROGRESS) return;
    if (k->scancode == KEY_ENTER) { if (s->page != PG_CONFIRM) next(w); return; }   /* erasing needs a click */
    if (k->scancode == KEY_ESC) { if (s->page == PG_FAILED) next(w); else back(w); return; }
    if (s->page == PG_CONFIRM && k->extended && (k->scancode == KEY_UP || k->scancode == KEY_DOWN)) {
        s->fat = k->scancode == KEY_DOWN;
        return;
    }
    if (s->page == PG_DISK && k->extended && s->ndisks) {
        int d = k->scancode == KEY_UP ? -1 : k->scancode == KEY_DOWN ? 1 : 0;
        for (int i = s->sel + d; d && i >= 0 && i < s->ndisks; i += d)
            if (disk_usable(&s->disks[i])) { s->sel = i; break; }
    }
}

static bool setup_tick(WND *w)
{
    Setup *s = w->user;
    if (s->page != PG_PROGRESS) return false;
    SetupStatus st;
    SetupGetStatus(&st);
    bool changed = st.percent != s->st.percent || strcmp(st.step, s->st.step) || st.state != s->st.state;
    s->st = st;
    if (st.state == SETUP_DONE)   s->page = PG_DONE;
    if (st.state == SETUP_FAILED) s->page = PG_FAILED;
    return changed;
}

static void setup_close(WND *w)
{
    Setup *s = w->user;
    kfree(s);
    w->user = NULL;
    if (g_setup == w) g_setup = NULL;
}

void SetupOpen(void)
{
    if (g_setup) { WmSetActive(g_setup); return; }
    Setup *s = kzalloc(sizeof(Setup));
    if (!s) return;
    s->sel = -1;
    SetupGetStatus(&s->st);
    if (s->st.state == SETUP_RUNNING) s->page = PG_PROGRESS;   /* reopened while installing */
    WND *w = AppCreateWindow(APP_SETUP, "Install NovaOS", W, H, UI_BG);
    if (!w) { kfree(s); return; }
    w->user     = s;
    w->on_paint = setup_paint;
    w->on_mouse = setup_mouse;
    w->on_key   = setup_key;
    w->on_tick  = setup_tick;
    w->on_close = setup_close;
    g_setup = w;
}
