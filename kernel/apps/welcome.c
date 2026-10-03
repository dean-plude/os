/*
 * welcome.c — "Welcome to NovaOS": the first-boot setup
 *
 * The first time an installed NovaOS starts (Setup put it on this disk and
 * nobody has finished these screens yet), the desktop opens this window
 * before anything else: a welcome, the user's name, the time zone, the
 * display resolution, and a last page that hands over to the desktop.
 * Nothing here needs the Terminal.  The answers go to the registry (saved
 * on drive C: like every other setting):
 *
 *   HKLM\SOFTWARE\NovaOS\Setup  UserName, TimeZone, FirstBootDone (1 once finished)
 *   HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion  RegisteredOwner
 *   HKLM\SYSTEM\CurrentControlSet\Control\TimeZoneInformation (ke/timezone.c)
 *   the display mode as Settings saves it (DesktopSaveHeadMode)
 *
 * The name becomes USERNAME (GetUserName) for programs started from then
 * on, and is shown on the Start menu.  Systems started from the
 * installation media, and disks NovaOS was not installed on (the QEMU
 * images), never open it on their own; `start welcome` opens it anywhere.
 * Settings' Time & language page opens the time zone page alone
 * (WelcomeTimeZone) to change the zone later.
 */

#include "apps.h"
#include "../lib/string.h"
#include "../mm/vmm.h"
#include "../ke/printf.h"
#include "../ke/timezone.h"
#include "../fs/setup.h"
#include "../fs/persist.h"
#include "../hal/display.h"
#include "../um/um.h"
#include "../wm/desktop.h"

enum { PG_WELCOME, PG_NAME, PG_TIMEZONE, PG_DISPLAY, PG_DONE, N_PAGES };

#define W        720
#define H        480
#define SIDE_W   200
#define PAD      28
#define BTN_W    132
#define BTN_H    32
#define NAME_MAX 20                    /* Windows' limit for a user name */
#define CHIP_W   108
#define CHIP_H   36
#define CHIP_GAP 8
#define MAX_MODES 20
#define ROW_H    26                    /* Time zone: a row of the list */
#define FIND_MAX 24

#define OWNER_KEY "Machine\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion"

typedef struct {
    int  page;
    char name[NAME_MAX + 1];
    int  len;
    int  mode;                         /* Display: the chosen mode, an index into the list */
    int  tz, tz_top;                   /* Time zone: the chosen zone (ke/timezone.h), the first row shown */
    char find[FIND_MAX + 1];           /* ... the text typed to find one */
    int  flen;
    bool first;                        /* opened at first boot (not by `start welcome`) */
    bool tz_only;                      /* only the time zone page (from Settings) */
} Welcome;

static WND *g_welcome;

/* ---------------------------------------------------------------------------
 * The user's name
 * ------------------------------------------------------------------------- */
void AppUserName(char *out, int cap)
{
    if (!um_registry_get_sz(UM_SETUP_KEY, "UserName", out, cap) || !out[0])
        ksnprintf(out, (size_t)cap, "Dean Plude");
}

/* A character Windows allows in a user name */
static bool name_char(char c)
{
    return c >= ' ' && c <= '~' && !strchr("\"/\\[]:;|=,+*?<>@", c);
}

/* The name without its leading and trailing spaces, if one is left
 * (and it does not end with a dot, which Windows refuses) */
static bool name_ok(const Welcome *s, char *out)
{
    int a = 0, b = s->len;
    while (a < b && s->name[a] == ' ') a++;
    while (b > a && s->name[b - 1] == ' ') b--;
    if (a == b || s->name[b - 1] == '.') return false;
    if (out) { memcpy(out, s->name + a, (size_t)(b - a)); out[b - a] = '\0'; }
    return true;
}

/* ---------------------------------------------------------------------------
 * When it opens by itself
 * ------------------------------------------------------------------------- */
bool WelcomeNeeded(void)
{
    UINT32 done = 0;
    if (SetupIsLive() || (um_registry_get_dword(UM_SETUP_KEY, "FirstBootDone", &done) && done))
        return false;
    /* NovaOS was installed on the disk drive C: is kept on: Setup's two
     * partitions, NOVA_EFI and NOVADATA (fs/setup.h) */
    BlockDev *d = PersistDevice();
    char labels[64];
    bool blank;
    return d && PersistDiskLabels(d, labels, sizeof(labels), &blank) >= 2 &&
           strstr(labels, "NOVA_EFI") && strstr(labels, "NOVADATA");
}

/* ---------------------------------------------------------------------------
 * Layout (client-relative)
 * ------------------------------------------------------------------------- */
static GdiRect r_next(GdiRect c) { return RECT(c.w - PAD - BTN_W, c.h - PAD - BTN_H, BTN_W, BTN_H); }
static GdiRect r_back(GdiRect c) { return RECT(c.w - PAD - 2 * BTN_W - 12, c.h - PAD - BTN_H, BTN_W, BTN_H); }
static GdiRect r_field(void)     { return RECT(SIDE_W + PAD, PAD + 110, W - SIDE_W - 2 * PAD, 40); }
static int     mode_cols(void)   { return (W - SIDE_W - 2 * PAD + CHIP_GAP) / (CHIP_W + CHIP_GAP); }
static GdiRect r_chip(int i)
{
    return RECT(SIDE_W + PAD + (i % mode_cols()) * (CHIP_W + CHIP_GAP),
                PAD + 96 + (i / mode_cols()) * (CHIP_H + CHIP_GAP), CHIP_W, CHIP_H);
}
static GdiRect off(GdiRect r, GdiRect c) { r.x += c.x; r.y += c.y; return r; }
static GdiRect r_find(void)      { return RECT(SIDE_W + PAD, PAD + 72, W - SIDE_W - 2 * PAD, 32); }
static GdiRect r_list(GdiRect c)                /* c: the client size */
{
    int y = PAD + 72 + 32 + 8, h = r_next(c).y - 12 - y;
    return RECT(SIDE_W + PAD, y, W - SIDE_W - 2 * PAD, h - h % ROW_H);
}
static int     tz_rows(GdiRect c) { return r_list(c).h / ROW_H; }

static int nmodes(void)
{
    int n = DisplayHeadModeCount(0);
    return n > MAX_MODES ? MAX_MODES : n;
}

static int current_mode(void)
{
    DisplayMode cur = DisplayHeadMode(0), m;
    for (int i = 0; i < nmodes(); i++)
        if (DisplayHeadModeAt(0, i, &m) && m.w == cur.w && m.h == cur.h) return i;
    return -1;
}

/* ---------------------------------------------------------------------------
 * Painting
 * ------------------------------------------------------------------------- */
static void button(GdiRect r, const char *label, bool primary, bool enabled)
{
    if (enabled) { UiButton(r, label, primary); return; }
    GdiRoundRect(r, 6, UI_CARD, GDI_TRANSPARENT);
    GdiTextCenter(r.x, r.y + (r.h - GDI_FONT_H) / 2, r.w, label, UI_TEXT3);
}

static int wrap(int x, int y, int w, const char *text, GdiColor col)
{
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
        if (*p == '\n') y += 10;
        while (*p == ' ' || *p == '\n') p++;
    }
    return y;
}

static void paint_side(Welcome *s, GdiRect c)
{
    GdiFillRect(RECT(c.x, c.y, SIDE_W, c.h), UI_PANEL);
    AppDrawIcon(APP_WELCOME, c.x + 24, c.y + 28, 48);
    GdiTextBold(c.x + 24, c.y + 92, "Set up NovaOS", UI_TEXT);
    static const char *steps[N_PAGES] = { "Welcome", "Your name", "Time zone", "Display", "Finish" };
    if (s->tz_only) {
        GdiTextT(c.x + 46, c.y + 140, "Time zone", UI_TEXT);
        GdiFillCircle(c.x + 30, c.y + 148, 5, UI_ACCENT);
        return;
    }
    for (int i = 0; i < N_PAGES; i++) {
        int y = c.y + 140 + i * 30;
        bool done = i < s->page, now = i == s->page;
        GdiFillCircle(c.x + 30, y + 8, 5, now ? UI_ACCENT : done ? UI_TEXT2 : UI_LINE);
        GdiTextT(c.x + 46, y, steps[i], now ? UI_TEXT : done ? UI_TEXT2 : UI_TEXT3);
    }
}

static void welcome_paint(WND *w)
{
    Welcome *s = w->user;
    GdiRect c = WmClientRect(w);
    paint_side(s, c);
    int x = c.x + SIDE_W + PAD, y = c.y + PAD, tw = W - SIDE_W - 2 * PAD;
    char buf[256], nm[NAME_MAX + 1];

    switch (s->page) {
    case PG_WELCOME:
        GdiTextLarge(x, y, "Welcome to NovaOS", UI_TEXT);
        wrap(x, y + 48, tw, s->first
             ? "NovaOS is installed on this PC. Before you start, choose the name you go by on it, "
               "your time zone and the resolution of your display.\nSettings changes the time zone and "
               "the resolution later on, and `start welcome` in the Terminal brings these screens back."
             : "These are the screens NovaOS shows the first time it starts after it is installed: "
               "the name you go by on this PC, your time zone and the resolution of your display.", UI_TEXT2);
        button(off(r_next(c), c), "Next", true, true);
        if (!s->first) button(off(r_back(c), c), "Cancel", false, true);
        break;

    case PG_NAME: {
        GdiTextLarge(x, y, "Who's going to use this PC?", UI_TEXT);
        GdiTextT(x, y + 40, "Your name is shown on the Start menu and given to programs.", UI_TEXT2);
        GdiRect f = off(r_field(), c);
        GdiRoundRect(f, 6, UI_CARD, GDI_TRANSPARENT);
        GdiRoundBorderAlpha(f, 6, UI_ACCENT, 200);
        GdiTextT(f.x + 12, f.y + (f.h - GDI_FONT_H) / 2, s->name, UI_TEXT);
        int cx = f.x + 12 + GdiTextW(s->name) + 1;
        GdiFillRect(RECT(cx, f.y + 10, 2, f.h - 20), UI_ACCENT);            /* the caret */
        ksnprintf(buf, sizeof(buf), "Up to %d characters, without \" / \\ [ ] : ; | = , + * ? < > @", NAME_MAX);
        GdiTextT(x, f.y + f.h + 12, buf, UI_TEXT3);
        button(off(r_next(c), c), "Next", true, name_ok(s, NULL));
        button(off(r_back(c), c), "Back", false, true);
        break; }

    case PG_TIMEZONE: {
        RtcTime t;
        bool dst;
        TzZoneLocalNow(s->tz, &t, &dst);
        GdiTextLarge(x, y, "Choose your time zone", UI_TEXT);
        ksnprintf(buf, sizeof(buf), "It is %u:%02u there now%s. Type a city to find its zone.",
                  t.hour, t.minute, dst ? " (daylight saving time)" : "");
        GdiTextT(x, y + 40, buf, UI_TEXT2);
        GdiRect f = off(r_find(), c);
        GdiRoundRect(f, 6, UI_CARD, GDI_TRANSPARENT);
        GdiRoundBorderAlpha(f, 6, UI_ACCENT, s->flen ? 200 : 80);
        if (s->flen) {
            GdiTextT(f.x + 12, f.y + (f.h - GDI_FONT_H) / 2, s->find, UI_TEXT);
            GdiFillRect(RECT(f.x + 12 + GdiTextW(s->find) + 1, f.y + 8, 2, f.h - 16), UI_ACCENT);
        } else {
            GdiTextT(f.x + 12, f.y + (f.h - GDI_FONT_H) / 2, "Find a city", UI_TEXT3);
        }
        GdiRect cl = RECT(0, 0, c.w, c.h), l = off(r_list(cl), c);
        GdiRoundRect(l, 6, UI_CARD, GDI_TRANSPARENT);
        for (int r = 0; r < tz_rows(cl) && s->tz_top + r < TzCount(); r++) {
            int i = s->tz_top + r;
            GdiRect row = RECT(l.x, l.y + r * ROW_H, l.w, ROW_H);
            if (i == s->tz) GdiRoundRect(row, 6, UI_ACCENT, GDI_TRANSPARENT);
            GdiTextT(row.x + 10, row.y + (ROW_H - GDI_FONT_H) / 2, TzAt(i)->display, i == s->tz ? GDI_WHITE : UI_TEXT);
        }
        button(off(r_next(c), c), s->tz_only ? "Save" : "Next", true, true);
        button(off(r_back(c), c), s->tz_only ? "Cancel" : "Back", false, true);
        break; }

    case PG_DISPLAY: {
        DisplayMode cur = DisplayHeadMode(0), m;
        GdiTextLarge(x, y, "Choose a resolution", UI_TEXT);
        ksnprintf(buf, sizeof(buf), "The display shows %d x %d now. Pick another to try it; it is kept from then on.",
                  cur.w, cur.h);
        wrap(x, y + 40, tw, buf, UI_TEXT2);
        int n = nmodes();
        if (!n) wrap(x, y + 96, tw, "This display offers no other modes.", UI_TEXT3);
        for (int i = 0; i < n; i++) {
            if (!DisplayHeadModeAt(0, i, &m)) continue;
            GdiRect r = off(r_chip(i), c);
            bool on = i == s->mode;
            GdiRoundRect(r, 6, on ? UI_ACCENT : UI_CARD, GDI_TRANSPARENT);
            ksnprintf(buf, sizeof(buf), "%d x %d", m.w, m.h);
            GdiTextCenter(r.x, r.y + (r.h - GDI_FONT_H) / 2, r.w, buf, on ? GDI_WHITE : UI_TEXT);
        }
        button(off(r_next(c), c), "Next", true, true);
        button(off(r_back(c), c), "Back", false, true);
        break; }

    case PG_DONE: {
        name_ok(s, nm);
        ksnprintf(buf, sizeof(buf), "You're all set, %s", nm);
        GdiTextLarge(x, y, buf, UI_TEXT);
        wrap(x, y + 48, tw, "Your desktop is ready. Programs are in the Start menu and the App Store, and "
                            "Settings changes the time zone, resolution, sound, network and more.",
             UI_TEXT2);
        button(off(r_next(c), c), "Start", true, true);
        button(off(r_back(c), c), "Back", false, true);
        break; }
    }
}

/* ---------------------------------------------------------------------------
 * Actions
 * ------------------------------------------------------------------------- */
static void center(WND *w)
{
    GdiRect f = w->frame, work = WmWorkArea();
    WmSetFrame(w, RECT(work.x + (work.w - f.w) / 2, work.y + (work.h - f.h) / 2, f.w, f.h));
}

static void choose_mode(WND *w, int i)
{
    Welcome *s = w->user;
    DisplayMode m;
    if (i == s->mode || !DisplayHeadModeAt(0, i, &m)) return;
    if (!DesktopSetHeadMode(0, m.w, m.h)) { kprintf("[WELCOME] The display cannot show %dx%d\n", m.w, m.h); return; }
    DesktopSaveHeadMode(0, m.w, m.h);
    s->mode = i;
    kprintf("[WELCOME] Display %dx%d\n", m.w, m.h);
    center(w);
}

/* Time zone: show the chosen row, and find a typed city */
static void tz_show(WND *w)
{
    Welcome *s = w->user;
    GdiRect c = WmClientRect(w);
    int rows = tz_rows(RECT(0, 0, c.w, c.h));
    if (rows < 1) rows = 1;
    if (s->tz < s->tz_top) s->tz_top = s->tz;
    if (s->tz >= s->tz_top + rows) s->tz_top = s->tz - rows + 1;
    if (s->tz_top > TzCount() - rows) s->tz_top = TzCount() - rows;
    if (s->tz_top < 0) s->tz_top = 0;
}

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

static bool contains(const char *hay, const char *needle)
{
    for (; *hay; hay++) {
        int i = 0;
        while (needle[i] && lower(hay[i]) == lower(needle[i])) i++;
        if (!needle[i]) return true;
    }
    return false;
}

static void tz_find(WND *w)
{
    Welcome *s = w->user;
    for (int i = 0; s->flen && i < TzCount(); i++)
        if (contains(TzAt(i)->display, s->find) || contains(TzAt(i)->key, s->find)) {
            s->tz = i;
            break;
        }
    tz_show(w);
}

static void tz_save(WND *w)
{
    Welcome *s = w->user;
    if (s->tz == TzCurrent()) return;
    TzSet(s->tz);
    DesktopClockChanged();
}

static void finish(WND *w)
{
    Welcome *s = w->user;
    char nm[NAME_MAX + 1];
    if (!name_ok(s, nm)) return;
    um_registry_set_sz(UM_SETUP_KEY, "UserName", nm);
    um_registry_set_sz(OWNER_KEY, "RegisteredOwner", nm);
    um_registry_set_dword(UM_SETUP_KEY, "FirstBootDone", 1);
    DisplayMode cur = DisplayHeadMode(0);
    kprintf("[WELCOME] Finished: user \"%s\", time zone %s, display %dx%d\n", nm, TzAt(TzCurrent())->key, cur.w, cur.h);
    WmDestroyWindow(w);
    WmInvalidateBackground();                   /* the Start menu's name */
}

static void next(WND *w)
{
    Welcome *s = w->user;
    switch (s->page) {
    case PG_WELCOME: s->page = PG_NAME; break;
    case PG_NAME:    if (name_ok(s, NULL)) s->page = PG_TIMEZONE; break;
    case PG_TIMEZONE:
        tz_save(w);
        if (s->tz_only) { WmDestroyWindow(w); return; }
        s->page = PG_DISPLAY;
        s->mode = current_mode();
        break;
    case PG_DISPLAY: s->page = PG_DONE; break;
    case PG_DONE:    finish(w); return;
    }
    kprintf("[WELCOME] Page %d\n", s->page + 1);
}

static void back(WND *w)
{
    Welcome *s = w->user;
    if (s->page == PG_WELCOME || s->tz_only) { if (!s->first) WmDestroyWindow(w); return; }
    s->page--;
}

static void welcome_mouse(WND *w, WmMouseMsg msg, int x, int y)
{
    Welcome *s = w->user;
    GdiRect c = WmClientRect(w);
    GdiRect cl = RECT(0, 0, c.w, c.h);
    if (s->page == PG_DISPLAY && msg == WM_MOUSE_DOWN)
        for (int i = 0; i < nmodes(); i++)
            if (UiHit(r_chip(i), x, y)) { choose_mode(w, i); return; }
    if (s->page == PG_TIMEZONE) {
        GdiRect l = r_list(cl);
        if (msg == WM_MOUSE_WHEEL) {
            s->tz_top -= 3 * WmWheelDelta();
            if (s->tz_top > TzCount() - tz_rows(cl)) s->tz_top = TzCount() - tz_rows(cl);
            if (s->tz_top < 0) s->tz_top = 0;
            return;
        }
        if (msg == WM_MOUSE_DOWN && UiHit(l, x, y)) {
            int i = s->tz_top + (y - l.y) / ROW_H;
            if (i < TzCount()) s->tz = i;
            return;
        }
    }
    if (msg != WM_MOUSE_UP) return;
    if (UiHit(r_next(cl), x, y)) next(w);
    else if ((s->page != PG_WELCOME || !s->first) && UiHit(r_back(cl), x, y)) back(w);
}

static void welcome_key(WND *w, const KeyEvent *k)
{
    Welcome *s = w->user;
    if (k->scancode == KEY_ENTER && !k->extended) { next(w); return; }
    if (k->scancode == KEY_ESC) { back(w); return; }
    if (s->page == PG_NAME) {
        if (k->ch == '\b') { if (s->len) s->name[--s->len] = '\0'; }
        else if (!k->ctrl && !k->alt && name_char(k->ch) && s->len < NAME_MAX &&
                 (s->len || k->ch != ' ')) { s->name[s->len++] = k->ch; s->name[s->len] = '\0'; }
        return;
    }
    if (s->page == PG_TIMEZONE) {
        GdiRect c = WmClientRect(w);
        int rows = tz_rows(RECT(0, 0, c.w, c.h)), i = s->tz;
        if (k->extended) {
            if (k->scancode == KEY_UP)   i--;
            if (k->scancode == KEY_DOWN) i++;
            if (k->scancode == KEY_PGUP) i -= rows;
            if (k->scancode == KEY_PGDN) i += rows;
            if (k->scancode == KEY_HOME) i = 0;
            if (k->scancode == KEY_END)  i = TzCount() - 1;
            s->tz = i < 0 ? 0 : i >= TzCount() ? TzCount() - 1 : i;
            tz_show(w);
        } else if (k->ch == '\b') {
            if (s->flen) s->find[--s->flen] = '\0';
            tz_find(w);
        } else if (!k->ctrl && !k->alt && k->ch >= ' ' && k->ch <= '~' && s->flen < FIND_MAX) {
            s->find[s->flen++] = k->ch;
            s->find[s->flen] = '\0';
            tz_find(w);
        }
        return;
    }
    if (s->page == PG_DISPLAY && k->extended && nmodes()) {
        int cols = mode_cols(), i = s->mode < 0 ? 0 : s->mode;
        if (k->scancode == KEY_LEFT)  i--;
        if (k->scancode == KEY_RIGHT) i++;
        if (k->scancode == KEY_UP)    i -= cols;
        if (k->scancode == KEY_DOWN)  i += cols;
        if (i >= 0 && i < nmodes()) choose_mode(w, i);
    }
}

static void welcome_close(WND *w)
{
    kfree(w->user);
    w->user = NULL;
    if (g_welcome == w) g_welcome = NULL;
}

static void welcome_open(bool first, bool tz_only)
{
    if (g_welcome) { WmSetActive(g_welcome); return; }
    Welcome *s = kzalloc(sizeof(Welcome));
    if (!s) return;
    s->first = first;
    s->tz_only = tz_only;
    s->mode = -1;
    s->tz = TzCurrent();
    if (tz_only) s->page = PG_TIMEZONE;
    if (!first) {                               /* opened again: start from the name given */
        char nm[NAME_MAX + 1];
        if (um_registry_get_sz(UM_SETUP_KEY, "UserName", nm, sizeof(nm)))
            for (int i = 0; nm[i] && s->len < NAME_MAX; i++) s->name[s->len++] = nm[i];
    }
    WND *w = AppCreateWindow(APP_WELCOME, tz_only ? "Time zone" : "Welcome to NovaOS", W, H, UI_BG);
    if (!w) { kfree(s); return; }
    w->user     = s;
    w->on_paint = welcome_paint;
    w->on_mouse = welcome_mouse;
    w->on_key   = welcome_key;
    w->on_close = welcome_close;
    g_welcome = w;
    center(w);
    tz_show(w);
    WmSetActive(w);
    kprintf("[WELCOME] Open (%s)\n", first ? "first boot" : tz_only ? "time zone" : "started by hand");
}

void WelcomeOpen(void)      { welcome_open(false, false); }
void WelcomeFirstBoot(void) { welcome_open(true, false); }
void WelcomeTimeZone(void)  { welcome_open(false, true); }
