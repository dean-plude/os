/*
 * calendar.c — Calendar: clock panel and a month view from the RTC
 */

#include "apps.h"
#include "../lib/string.h"
#include "../mm/vmm.h"
#include "../ke/printf.h"
#include "../hal/rtc.h"
#include "../ke/timezone.h"

#define LEFT_W  230
#define CELL    52

typedef struct { int year, month; } Calendar;

static const char *g_month[12] = {
    "January", "February", "March", "April", "May", "June", "July",
    "August", "September", "October", "November", "December",
};
static const char *g_wday[7] = { "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa" };
static const char *g_wday_long[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
};

/* 0 = Sunday (Sakamoto's method) */
static int day_of_week(int y, int m, int d)
{
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

static int days_in(int y, int m)
{
    static const int dm[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return dm[m - 1] + (m == 2 && leap);
}

static GdiRect r_prev(GdiRect c) { return RECT(c.w - 96, 20, 36, 32); }
static GdiRect r_next(GdiRect c) { return RECT(c.w - 52, 20, 36, 32); }

static void chevron(GdiRect b, bool right, GdiColor col)
{
    int cx = b.x + b.w / 2, cy = b.y + b.h / 2, d = right ? 1 : -1;
    GdiLine((GdiPoint){ (cx - 2 * d) * 16, (cy - 5) * 16 }, (GdiPoint){ (cx + 3 * d) * 16, cy * 16 }, 22, col);
    GdiLine((GdiPoint){ (cx + 3 * d) * 16, cy * 16 }, (GdiPoint){ (cx - 2 * d) * 16, (cy + 5) * 16 }, 22, col);
}

static void cal_paint(WND *w)
{
    Calendar *cal = w->user;
    GdiRect c = WmClientRect(w);
    RtcTime now;
    TzLocalNow(&now);

    /* Clock panel */
    GdiFillRect(RECT(c.x, c.y, LEFT_W, c.h), UI_PANEL);
    int h12 = now.hour % 12 ? now.hour % 12 : 12;
    char tm[16], date[48];
    ksnprintf(tm, sizeof(tm), "%d:%02u %s", h12, now.minute, now.hour < 12 ? "AM" : "PM");
    GdiTextLarge(c.x + 24, c.y + 28, tm, UI_TEXT);
    ksnprintf(date, sizeof(date), "%s,", g_wday_long[day_of_week(now.year, now.month, now.day)]);
    GdiTextT(c.x + 24, c.y + 68, date, UI_TEXT2);
    ksnprintf(date, sizeof(date), "%s %u, %u", g_month[now.month - 1], now.day, now.year);
    GdiTextT(c.x + 24, c.y + 86, date, UI_TEXT2);
    GdiFillRect(RECT(c.x + 24, c.y + 124, LEFT_W - 48, 1), UI_LINE);
    GdiTextBold(c.x + 24, c.y + 140, "Today", UI_TEXT);
    GdiTextT(c.x + 24, c.y + 162, "No events", UI_TEXT3);

    /* Month header */
    int gx = c.x + LEFT_W + 24;
    char head[32];
    ksnprintf(head, sizeof(head), "%s %d", g_month[cal->month - 1], cal->year);
    GdiTextLarge(gx, c.y + 20, head, UI_TEXT);
    GdiRect rp = r_prev(c), rn = r_next(c);
    rp.x += c.x; rp.y += c.y; rn.x += c.x; rn.y += c.y;
    GdiRoundRect(rp, 4, UI_CARD, GDI_TRANSPARENT);
    GdiRoundRect(rn, 4, UI_CARD, GDI_TRANSPARENT);
    chevron(rp, false, UI_TEXT);
    chevron(rn, true, UI_TEXT);

    /* Grid */
    int gy = c.y + 74;
    for (int i = 0; i < 7; i++)
        GdiTextCenter(gx + i * CELL, gy, CELL, g_wday[i], UI_TEXT3);
    gy += 28;
    int first = day_of_week(cal->year, cal->month, 1);
    int ndays = days_in(cal->year, cal->month);
    for (int d = 1; d <= ndays; d++) {
        int slot = first + d - 1;
        int x = gx + (slot % 7) * CELL, y = gy + (slot / 7) * CELL;
        bool today = (d == now.day && cal->month == now.month && cal->year == now.year);
        char num[4];
        ksnprintf(num, sizeof(num), "%d", d);
        if (today) GdiFillCircle(x + CELL / 2, y + CELL / 2 - 1, 18, UI_ACCENT);
        GdiTextCenter(x, y + (CELL - GDI_FONT_H) / 2, CELL, num, today ? GDI_BLACK : UI_TEXT);
    }
}

static void step(Calendar *cal, int dir)
{
    cal->month += dir;
    if (cal->month < 1)  { cal->month = 12; cal->year--; }
    if (cal->month > 12) { cal->month = 1;  cal->year++; }
}

static void cal_mouse(WND *w, WmMouseMsg msg, int x, int y)
{
    Calendar *cal = w->user;
    GdiRect c = WmClientRect(w);
    if (msg != WM_MOUSE_DOWN && msg != WM_MOUSE_DBLCLK) return;
    if (UiHit(r_prev(c), x, y)) step(cal, -1);
    if (UiHit(r_next(c), x, y)) step(cal, +1);
}

static void cal_key(WND *w, const KeyEvent *k)
{
    Calendar *cal = w->user;
    if (!k->extended) return;
    if (k->scancode == KEY_LEFT || k->scancode == KEY_PGUP)  step(cal, -1);
    if (k->scancode == KEY_RIGHT || k->scancode == KEY_PGDN) step(cal, +1);
    if (k->scancode == KEY_HOME) {
        RtcTime now; TzLocalNow(&now);
        cal->year = now.year; cal->month = now.month;
    }
}

static void cal_close(WND *w) { kfree(w->user); w->user = NULL; }

void CalendarOpen(void)
{
    Calendar *cal = kzalloc(sizeof(Calendar));
    if (!cal) return;
    RtcTime now;
    TzLocalNow(&now);
    cal->year = now.year;
    cal->month = (now.month >= 1 && now.month <= 12) ? now.month : 1;
    WND *w = AppCreateWindow(APP_CALENDAR, "Calendar", LEFT_W + 24 + 7 * CELL + 24, 420, UI_BG);
    if (!w) { kfree(cal); return; }
    w->user     = cal;
    w->on_paint = cal_paint;
    w->on_mouse = cal_mouse;
    w->on_key   = cal_key;
    w->on_close = cal_close;
}
