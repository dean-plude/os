/*
 * settings.c — Settings: live system, display, personalization, storage,
 *               network and about pages
 */

#include "apps.h"
#include "../lib/string.h"
#include "../mm/vmm.h"
#include "../mm/pmm.h"
#include "../ke/printf.h"
#include "../net/net.h"
#include "../wm/desktop.h"
#include "../fs/persist.h"
#include "../hal/display.h"

#define SIDE_W 200
#define ITEM_H 36

typedef struct {
    int page; UINT32 net_sig;
    int mon;                         /* Display page: the monitor chosen */
    bool drag, moved;                /* a monitor being dragged in the arrangement */
    int drag_dx, drag_dy;            /* the press inside it (diagram px) */
    int drag_x, drag_y;              /* its top left now (diagram px) */
} Settings;

static const char *g_pages[] = { "System", "Display", "Personalization", "Storage", "Network", "About" };
static const Glyph g_page_glyphs[] = { GL_PC, GL_WINDOWS, GL_PICTURES, GL_FOLDER, GL_NETWORK, GL_NOVA };
#define N_PAGES ((int)(sizeof(g_pages) / sizeof(g_pages[0])))

/* A labelled row inside a card: "Label ........ value" */
static void row(int x, int y, int w, const char *label, const char *value)
{
    GdiRoundRect(RECT(x, y, w, 44), 6, UI_CARD, GDI_TRANSPARENT);
    GdiTextT(x + 16, y + 14, label, UI_TEXT);
    GdiTextT(x + w - 16 - GdiTextW(value), y + 14, value, UI_TEXT2);
}

static void bar(int x, int y, int w, UINT64 used, UINT64 total)
{
    GdiRoundRect(RECT(x, y, w, 8), 4, UI_HOVER, GDI_TRANSPARENT);
    int fw = total ? (int)((UINT64)w * used / total) : 0;
    if (fw > 0) GdiRoundRect(RECT(x, y, fw < 8 ? 8 : fw, 8), 4, UI_ACCENT, GDI_TRANSPARENT);
}

static void ramdisk_usage(RamNode *d, UINT64 *bytes, int *files)
{
    for (RamNode *c = d->child; c; c = c->next) {
        if (c->dir) ramdisk_usage(c, bytes, files);
        else { *bytes += c->size; (*files)++; }
    }
}

static void page_system(int x, int y, int w)
{
    char cpu[64], up[32], mem[48];
    AppCpuName(cpu, sizeof(cpu));
    AppUptime(up, sizeof(up));
    uint64_t total, free_p, used;
    pmm_stats(&total, &free_p, &used);
    ksnprintf(mem, sizeof(mem), "%u MB (%u MB in use)",
              (unsigned)(total * 4 / 1024), (unsigned)(used * 4 / 1024));

    GdiRoundRect(RECT(x, y, w, 96), 8, UI_CARD, GDI_TRANSPARENT);
    GdiRoundGradV(RECT(x + 20, y + 20, 88, 56), 6, GDI_C(0x3A, 0x8A, 0xF0), GDI_C(0x2A, 0xC8, 0xC8));
    GdiTextLarge(x + 128, y + 20, "NOVA-PC", UI_TEXT);
    GdiTextT(x + 128, y + 54, "NovaOS 0.9  -  Phase 9.5 desktop", UI_TEXT2);
    y += 112;
    row(x, y, w, "Processor", cpu);           y += 50;
    row(x, y, w, "Installed memory", mem);    y += 50;
    row(x, y, w, "System type", "64-bit operating system, x64 processor"); y += 50;
    row(x, y, w, "Uptime", up);
}

/* Display page: with more than one monitor, an arrangement of them on top
 * (drag one to move it; click one to choose it), then the chosen one's
 * resolution buttons (chips), modes_dy() below the page's top; set_mouse
 * finds them with the same numbers */
#define CHIP_W   116
#define CHIP_H   32
#define CHIP_GAP 8
#define ARR_H    150
#define ARR_DY   26
#define SNAP     48                  /* logical px: edges this close line up */

static bool multi(void) { return GdiMonitorCount() > 1; }

static int modes_dy(void) { return multi() ? ARR_DY + ARR_H + 16 + 26 + 50 + 26 : 26 + 50 + 50 + 26; }

static int chip_cols(int w)
{
    int n = (w + CHIP_GAP) / (CHIP_W + CHIP_GAP);
    return n < 1 ? 1 : n;
}

static int chosen(Settings *st) { return st->mon >= 0 && st->mon < GdiMonitorCount() ? st->mon : 0; }

/* The arrangement: the virtual desktop scaled into the card at @top (page
 * coordinates x, w), with room around for a monitor to be dragged */
typedef struct { int ox, oy, num, den; GdiRect v; } Arr;

static Arr arr_of(int x, int top, int w)
{
    Arr a;
    a.v = GdiVirtualRect();
    GdiRect v = a.v;
    int bw = v.w + 2 * 1280 / 2, bh = v.h + 400;             /* the desktop and a margin */
    a.num = 1; a.den = 1;
    int aw = w - 32, ah = ARR_H - 24;
    if ((INT64)bw * ah > (INT64)bh * aw) { a.num = aw; a.den = bw; } else { a.num = ah; a.den = bh; }
    a.ox = x + 16 + (aw - v.w * a.num / a.den) / 2 - v.x * a.num / a.den;
    a.oy = top + 12 + (ah - v.h * a.num / a.den) / 2 - v.y * a.num / a.den;
    return a;
}

static GdiRect arr_rect(const Arr *a, GdiRect m)
{
    return RECT(a->ox + m.x * a->num / a->den, a->oy + m.y * a->num / a->den,
                m.w * a->num / a->den, m.h * a->num / a->den);
}

static void page_display(Settings *st, int x, int y, int w)
{
    char res[96], scale[48], s1[32];
    int mon = chosen(st);
    DisplayMode cur = DisplayHeadMode(mon);
    int top = y;
    if (multi()) {
        GdiTextBold(x, y, "Arrange displays", UI_TEXT);
        GdiRoundRect(RECT(x, y + ARR_DY, w, ARR_H), 8, UI_CARD, GDI_TRANSPARENT);
        Arr a = arr_of(x, y + ARR_DY, w);
        for (int i = 0; i < GdiMonitorCount(); i++) {
            GdiRect r = arr_rect(&a, GdiMonitorRect(i));
            if (st->drag && i == mon) { r.x = st->drag_x; r.y = st->drag_y; }
            GdiRoundRect(RECT(r.x + 1, r.y + 1, r.w - 2, r.h - 2), 4, i == mon ? UI_ACCENT : UI_HOVER,
                         i == mon ? GDI_WHITE : UI_LINE);
            ksnprintf(s1, sizeof(s1), "%d", i + 1);
            GdiTextLarge(r.x + (r.w - GdiTextLargeW(s1)) / 2, r.y + r.h / 2 - 15, s1, i == mon ? GDI_WHITE : UI_TEXT);
        }
        y += ARR_DY + ARR_H + 16;
        GdiRect m = GdiMonitorRect(mon);
        ksnprintf(s1, sizeof(s1), "Display %d%s", mon + 1, mon == 0 ? " (main display)" : "");
        GdiTextBold(x, y, s1, UI_TEXT);              y += 26;
        ksnprintf(res, sizeof(res), "%d x %d, %d%%, at (%d, %d) - %s", cur.w, cur.h, GdiMonitorScale(mon) * 100,
                  m.x, m.y, DisplayHeadName(mon) ? DisplayHeadName(mon) : "");
        row(x, y, w, "Resolution", res);
    } else {
        int s = GdiScale();
        ksnprintf(res, sizeof(res), "%d x %d", cur.w, cur.h);
        ksnprintf(scale, sizeof(scale), "%d%% (desktop %d x %d)", s * 100, GdiScreenW(), GdiScreenH());
        GdiTextBold(x, y, "Scale & layout", UI_TEXT);   y += 26;
        row(x, y, w, "Display resolution", res);         y += 50;
        row(x, y, w, "Scale", scale);
    }

    GdiTextBold(x, top + modes_dy() - 26, "Resolution", UI_TEXT);
    y = top + modes_dy();
    int cols = chip_cols(w), n = DisplayHeadModeCount(mon);
    for (int i = 0; i < n; i++) {
        DisplayMode m;
        DisplayHeadModeAt(mon, i, &m);
        int cx = x + (i % cols) * (CHIP_W + CHIP_GAP), cy = y + (i / cols) * (CHIP_H + CHIP_GAP);
        bool on = m.w == cur.w && m.h == cur.h;
        GdiRoundRect(RECT(cx, cy, CHIP_W, CHIP_H), 6, on ? UI_ACCENT : UI_CARD, GDI_TRANSPARENT);
        ksnprintf(s1, sizeof(s1), "%d x %d", m.w, m.h);
        GdiTextCenter(cx, cy + 8, CHIP_W, s1, on ? GDI_WHITE : UI_TEXT);
    }
    y += ((n + cols - 1) / cols) * (CHIP_H + CHIP_GAP) + 18;

    GdiTextBold(x, y, "Display adapter", UI_TEXT);   y += 26;
    row(x, y, w, "Driver", DisplayDriverName());     y += 50;
    if (DisplayAdapterName()) { row(x, y, w, "Adapter", DisplayAdapterName()); y += 50; }
    row(x, y, w, "Presentation", DisplayCanFlip() ? "Back buffer, page flipping" : "Back buffer, copied to the screen");
}

/* Where a monitor dropped with its top left at (x, y) goes: touching
 * another monitor along the nearest edge, overlapping none, and lined up
 * with an edge of it when that is close */
static void snap_place(int mon, int *px, int *py)
{
    GdiRect me = GdiMonitorRect(mon);
    int bx = *px, by = *py;
    INT64 best = -1;
    for (int j = 0; j < GdiMonitorCount(); j++) {
        if (j == mon) continue;
        GdiRect o = GdiMonitorRect(j);
        for (int side = 0; side < 4; side++) {
            int x = *px, y = *py;
            if (side < 2) {                          /* left or right of it */
                x = side == 0 ? o.x - me.w : o.x + o.w;
                if (y < o.y - me.h + 1) y = o.y - me.h + 1;
                if (y > o.y + o.h - 1) y = o.y + o.h - 1;
                if (y - o.y < SNAP && y - o.y > -SNAP) y = o.y;
                else if (y + me.h - (o.y + o.h) < SNAP && y + me.h - (o.y + o.h) > -SNAP) y = o.y + o.h - me.h;
            } else {                                 /* above or below it */
                y = side == 2 ? o.y - me.h : o.y + o.h;
                if (x < o.x - me.w + 1) x = o.x - me.w + 1;
                if (x > o.x + o.w - 1) x = o.x + o.w - 1;
                if (x - o.x < SNAP && x - o.x > -SNAP) x = o.x;
                else if (x + me.w - (o.x + o.w) < SNAP && x + me.w - (o.x + o.w) > -SNAP) x = o.x + o.w - me.w;
            }
            bool clash = false;
            for (int k = 0; k < GdiMonitorCount() && !clash; k++) {
                GdiRect q = GdiMonitorRect(k);
                if (k != mon && x < q.x + q.w && q.x < x + me.w && y < q.y + q.h && q.y < y + me.h) clash = true;
            }
            if (clash) continue;
            INT64 d = (INT64)(x - *px) * (x - *px) + (INT64)(y - *py) * (y - *py);
            if (best < 0 || d < best) { best = d; bx = x; by = y; }
        }
    }
    *px = bx; *py = by;
}

static void page_storage(int x, int y, int w)
{
    uint64_t total, free_p, used;
    pmm_stats(&total, &free_p, &used);
    UINT64 bytes = 0;
    int files = 0;
    ramdisk_usage(RamfsRoot(), &bytes, &files);
    char a[64], b[64];

    GdiRoundRect(RECT(x, y, w, 92), 8, UI_CARD, GDI_TRANSPARENT);
    GdiTextBold(x + 16, y + 14, "Memory (RAM)", UI_TEXT);
    ksnprintf(a, sizeof(a), "%u MB used of %u MB", (unsigned)(used * 4 / 1024),
              (unsigned)(total * 4 / 1024));
    GdiTextT(x + 16, y + 38, a, UI_TEXT2);
    bar(x + 16, y + 66, w - 32, used, total);
    y += 108;

    GdiRoundRect(RECT(x, y, w, 92), 8, UI_CARD, GDI_TRANSPARENT);
    GdiTextBold(x + 16, y + 14, "Local Disk (C:)", UI_TEXT);
    AppFormatSize(bytes, b, sizeof(b));
    char where[40];
    PersistWhere(where, sizeof(where));
    if (PersistActive())
        ksnprintf(a, sizeof(a), "%s in %d file%s; saved to disk %s", b, files, files == 1 ? "" : "s", where);
    else
        ksnprintf(a, sizeof(a), "%s in %d file%s; no disk found, reset on reboot", b, files, files == 1 ? "" : "s");
    GdiTextT(x + 16, y + 38, a, UI_TEXT2);
    UINT64 dfree, dtotal;
    if (PersistSpace(&dfree, &dtotal)) bar(x + 16, y + 66, w - 32, dtotal - dfree, dtotal);   /* the disk's use */
    else bar(x + 16, y + 66, w - 32, bytes, bytes + 1024 * 1024);
}

static void page_network(int x, int y, int w)
{
    NetStatus ns;
    NetGetStatus(&ns);
    char mac[24], ip[20], mask[20], gw[20], dns[48], d2[20];

    GdiRoundRect(RECT(x, y, w, 72), 8, UI_CARD, GDI_TRANSPARENT);
    GdiTextBold(x + 16, y + 14, "Ethernet", UI_TEXT);
    const char *state = !ns.present ? "No network adapter found" :
                        !ns.link ? "Disconnected (no link)" :
                        !ns.configured ? "Connecting (waiting for DHCP)..." : "Connected";
    GdiColor dot = ns.configured ? GDI_C(0x2E, 0xB8, 0x5C) :
                   ns.link ? GDI_C(0xE8, 0xA8, 0x20) : GDI_C(0xD0, 0x40, 0x40);
    GdiRoundRect(RECT(x + 16, y + 44, 10, 10), 5, dot, GDI_TRANSPARENT);
    GdiTextT(x + 34, y + 40, state, UI_TEXT2);
    y += 88;
    if (!ns.present) return;

    ksnprintf(mac, sizeof(mac), "%02X-%02X-%02X-%02X-%02X-%02X", ns.mac[0], ns.mac[1],
              ns.mac[2], ns.mac[3], ns.mac[4], ns.mac[5]);
    row(x, y, w, "Adapter", ns.adapter);          y += 50;
    row(x, y, w, "Physical address (MAC)", mac);  y += 50;
    if (!ns.configured) return;
    NetFormatIp(ns.ip, ip, sizeof(ip));
    NetFormatIp(ns.mask, mask, sizeof(mask));
    NetFormatIp(ns.gw, gw, sizeof(gw));
    NetFormatIp(ns.dns[0], dns, sizeof(dns));
    if (ns.dns[1]) {
        NetFormatIp(ns.dns[1], d2, sizeof(d2));
        strcat(dns, ", ");
        strcat(dns, d2);
    }
    row(x, y, w, "IPv4 address (DHCP)", ip);      y += 50;
    row(x, y, w, "Subnet mask", mask);            y += 50;
    row(x, y, w, "Default gateway", gw);          y += 50;
    row(x, y, w, "DNS servers", dns);                 y += 50;
    char roots[48];
    int imported, n = NetRootCount(&imported);
    if (imported) ksnprintf(roots, sizeof(roots), "%d (%d imported)", n, imported);
    else          ksnprintf(roots, sizeof(roots), "%d (Mozilla CA list)", n);
    row(x, y, w, "Trusted root certificates (HTTPS)", roots);
}

/* Repaint the Network page when the connection state changes */
static bool set_tick(WND *w)
{
    Settings *st = w->user;
    if (!st || st->page != SETTINGS_NETWORK) return false;
    NetStatus ns;
    NetGetStatus(&ns);
    UINT32 sig = ns.ip ^ ns.gw ^ ns.dns[0] ^ (ns.link ? 1u : 0) ^ (ns.configured ? 2u : 0);
    if (sig == st->net_sig) return false;
    st->net_sig = sig;
    return true;
}

/* Personalization: one card per wallpaper theme; click to apply */
#define THEME_CARD_H 150
static int theme_card_w(int w)
{
    int n = DesktopThemeCount();
    return (w - (n - 1) * 16) / n;
}

static void page_personalize(int x, int y, int w)
{
    GdiTextT(x, y, "Background and colours", UI_TEXT2);
    y += 30;
    int n = DesktopThemeCount(), cw = theme_card_w(w);
    for (int i = 0; i < n; i++) {
        int cx = x + i * (cw + 16);
        bool sel = i == DesktopTheme();
        GdiRoundRect(RECT(cx, y, cw, THEME_CARD_H + 40), 8, UI_CARD,
                     sel ? UI_ACCENT : GDI_TRANSPARENT);
        if (sel) GdiRoundRect(RECT(cx + 1, y + 1, cw - 2, THEME_CARD_H + 38), 7,
                              GDI_TRANSPARENT, UI_ACCENT);
        DesktopDrawThemePreview(i, RECT(cx + 8, y + 8, cw - 16, THEME_CARD_H - 16));
        GdiTextT(cx + 12, y + THEME_CARD_H + 4, DesktopThemeName(i), UI_TEXT);
        if (sel) GdiTextT(cx + cw - 12 - GdiTextW("Active"), y + THEME_CARD_H + 4,
                          "Active", UI_ACCENT);
    }
    y += THEME_CARD_H + 60;
    row(x, y, w, "Accent colour", "Follows the theme");
}

static void page_about(int x, int y, int w)
{
    GdiTextLarge(x, y, "NovaOS", UI_TEXT);             y += 40;
    GdiTextT(x, y, "Version 0.9.8  -  a Windows-compatible OS research project", UI_TEXT2); y += 34;
    row(x, y, w, "Kernel", "Nova, NT-style syscalls (Win10 1903 ABI)"); y += 50;
    row(x, y, w, "Desktop", "Kernel GDI + window manager");            y += 50;
    row(x, y, w, "UI font", "Inter 4.1 (SIL OFL 1.1)");                 y += 50;
    row(x, y, w, "Monospace font", "Cascadia Mono (SIL OFL 1.1)");
}

static void set_paint(WND *w)
{
    Settings *st = w->user;
    GdiRect c = WmClientRect(w);

    GdiFillRect(RECT(c.x, c.y, SIDE_W, c.h), UI_PANEL);
    GdiFillRect(RECT(c.x + SIDE_W - 1, c.y, 1, c.h), UI_LINE);
    GdiTextLarge(c.x + 20, c.y + 16, "Settings", UI_TEXT);
    for (int i = 0; i < N_PAGES; i++) {
        int y = c.y + 64 + i * ITEM_H;
        if (i == st->page) {
            GdiRoundRect(RECT(c.x + 8, y, SIDE_W - 16, ITEM_H - 4), 4, UI_HOVER, GDI_TRANSPARENT);
            GdiRoundRect(RECT(c.x + 8, y + 9, 3, 14), 1, UI_ACCENT, GDI_TRANSPARENT);
        }
        AppDrawGlyph(g_page_glyphs[i], c.x + 22, y + 8, 16, i == st->page ? UI_TEXT : UI_TEXT2);
        GdiTextT(c.x + 48, y + 8, g_pages[i], i == st->page ? UI_TEXT : UI_TEXT2);
    }

    int x = c.x + SIDE_W + 28, y = c.y + 20, w2 = c.w - SIDE_W - 56;
    GdiTextLarge(x, y, g_pages[st->page], UI_TEXT);
    y += 52;
    switch (st->page) {
    case SETTINGS_SYSTEM:      page_system(x, y, w2);     break;
    case SETTINGS_DISPLAY:     page_display(st, x, y, w2); break;
    case SETTINGS_PERSONALIZE: page_personalize(x, y, w2); break;
    case SETTINGS_STORAGE:     page_storage(x, y, w2);    break;
    case SETTINGS_NETWORK:     page_network(x, y, w2);    break;
    case SETTINGS_ABOUT:       page_about(x, y, w2);      break;
    }
}

static void set_mouse(WND *w, WmMouseMsg msg, int x, int y)
{
    Settings *st = w->user;
    GdiRect c = WmClientRect(w);
    if (st->page == SETTINGS_DISPLAY && multi()) {
        /* the arrangement (layout matches set_paint + page_display) */
        Arr a = arr_of(SIDE_W + 28, 20 + 52 + ARR_DY, c.w - SIDE_W - 56);
        if (msg == WM_MOUSE_DOWN) {
            for (int i = GdiMonitorCount() - 1; i >= 0; i--) {
                GdiRect r = arr_rect(&a, GdiMonitorRect(i));
                if (x < r.x || y < r.y || x >= r.x + r.w || y >= r.y + r.h) continue;
                st->mon = i;
                st->drag = i > 0;                   /* the main display stays at (0, 0) */
                st->moved = false;
                st->drag_dx = x - r.x; st->drag_dy = y - r.y;
                st->drag_x = r.x + c.x; st->drag_y = r.y + c.y;
                return;
            }
        } else if (msg == WM_MOUSE_MOVE && st->drag) {
            st->drag_x = x - st->drag_dx + c.x;
            st->drag_y = y - st->drag_dy + c.y;
            st->moved = true;
            return;
        } else if (msg == WM_MOUSE_UP && st->drag) {
            st->drag = false;
            if (!st->moved) return;                 /* a click: just chose it */
            int mx = (x - st->drag_dx - a.ox) * a.den / a.num, my = (y - st->drag_dy - a.oy) * a.den / a.num;
            snap_place(st->mon, &mx, &my);
            GdiRect m = GdiMonitorRect(st->mon);
            if (mx != m.x || my != m.y) DesktopSetMonitorOrigin(st->mon, mx, my, true);
            return;
        }
    }
    if (msg != WM_MOUSE_DOWN) return;
    if (x >= SIDE_W) {
        if (st->page == SETTINGS_DISPLAY) {
            /* resolution chips (layout matches set_paint + page_display) */
            int px = SIDE_W + 28, py = 20 + 52 + modes_dy(), pw = c.w - SIDE_W - 56;
            int cols = chip_cols(pw);
            if (x < px || y < py) return;
            int col = (x - px) / (CHIP_W + CHIP_GAP), r = (y - py) / (CHIP_H + CHIP_GAP);
            if (col >= cols || (x - px) % (CHIP_W + CHIP_GAP) >= CHIP_W ||
                (y - py) % (CHIP_H + CHIP_GAP) >= CHIP_H) return;
            DisplayMode m;
            int mon = chosen(st);
            if (!DisplayHeadModeAt(mon, r * cols + col, &m)) return;
            if (DesktopSetHeadMode(mon, m.w, m.h)) DesktopSaveHeadMode(mon, m.w, m.h);
            return;
        }
        /* theme cards (layout matches set_paint + page_personalize) */
        if (st->page != SETTINGS_PERSONALIZE) return;
        int px = SIDE_W + 28, py = 20 + 52 + 30, pw = c.w - SIDE_W - 56;
        int cw = theme_card_w(pw);
        if (y < py || y >= py + THEME_CARD_H + 40 || x < px) return;
        int i = (x - px) / (cw + 16);
        if (i < DesktopThemeCount() && (x - px) % (cw + 16) < cw) DesktopSetTheme(i);
        return;
    }
    int i = (y - 64) / ITEM_H;
    if (y >= 64 && i >= 0 && i < N_PAGES) st->page = i;
}

static void set_key(WND *w, const KeyEvent *k)
{
    Settings *st = w->user;
    if (!k->extended) return;
    if (k->scancode == KEY_UP && st->page > 0) st->page--;
    if (k->scancode == KEY_DOWN && st->page < N_PAGES - 1) st->page++;
}

static void set_close(WND *w) { kfree(w->user); w->user = NULL; }

void SettingsOpen(void)
{
    Settings *st = kzalloc(sizeof(Settings));
    if (!st) return;
    WND *w = AppCreateWindow(APP_SETTINGS, "Settings", 860, 580, UI_BG);
    if (!w) { kfree(st); return; }
    w->user     = st;
    w->on_paint = set_paint;
    w->on_mouse = set_mouse;
    w->on_key   = set_key;
    w->on_close = set_close;
    w->on_tick  = set_tick;
}

void SettingsOpenPage(int page)
{
    WND *w = WmFindApp(APP_SETTINGS);
    if (!w) {
        SettingsOpen();
        w = WmFindApp(APP_SETTINGS);
        if (!w) return;
    }
    Settings *st = w->user;
    if (st && page >= 0 && page < N_PAGES) st->page = page;
    WmSetActive(w);
    WmInvalidate();
}
