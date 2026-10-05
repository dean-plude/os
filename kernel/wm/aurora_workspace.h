/* Included after the shell's clock and hotspot definitions. These cards
 * are part of the cached background: app windows always draw above them. */
#pragma once

static char g_workspace_user[32] = "User";
static char g_workspace_initials[3] = "U";
static int g_workspace_selected;

static void aurora_load_user(void)
{
    AppUserName(g_workspace_user, sizeof(g_workspace_user));
    int n = 0;
    for (int i = 0; g_workspace_user[i] && n < 2; i++) {
        if (g_workspace_user[i] == ' ' || (i && g_workspace_user[i - 1] != ' ')) continue;
        char c = g_workspace_user[i];
        g_workspace_initials[n++] = c >= 'a' && c <= 'z' ? (char)(c - 32) : c;
    }
    g_workspace_initials[n] = '\0';
}

static const struct { const char *name; Glyph glyph; ActKind kind; int app; } g_workspace_nav[] = {
    { "Home", GL_FOLDER, ACT_APP, APP_EXPLORER },
    { "Apps", GL_WINDOWS, ACT_START, 0 },
    { "Create", GL_PLUS, ACT_APP, APP_NOTEPAD },
    { "Explore", GL_NETWORK, ACT_APP, APP_NETSURF },
    { "Notes", GL_DOCUMENTS, ACT_APP, APP_NOTEPAD },
    { "Calendar", GL_FILE, ACT_APP, APP_CALENDAR },
    { "Photos", GL_PICTURES, ACT_APP, APP_PHOTOS },
    { "Terminal", GL_CODE, ACT_APP, APP_TERMINAL },
    { "Settings", GL_GEAR, ACT_APP, APP_SETTINGS },
    { "Install NovaOS", GL_PC, ACT_APP, APP_SETUP },
};
#define WORKSPACE_NAV_COUNT (9 + (SetupIsLive() ? 1 : 0))

static void run_aurora_action(int row)
{
    if (row < 0 || row >= WORKSPACE_NAV_COUNT) return;
    g_workspace_selected = row;
    if (g_workspace_nav[row].kind == ACT_START) start_open(true);
    else AppActivate((AppId)g_workspace_nav[row].app);
    WmInvalidateBackground();
}

static void workspace_glass(GdiRect r)
{
    GdiDropShadow(RECT(r.x, r.y + 4, r.w, r.h), 16, 12, 65);
    glass(r, 16);
}

static void draw_aurora_sidebar(void)
{
    int count = WORKSPACE_NAV_COUNT;
    workspace_glass(RECT(16, 164, 160, count * 32 + 60));
    int y = 176;
    for (int i = 0; i < count; i++) {
        if (i == 4) {
            GdiAlphaFill(RECT(28, y, 136, 1), GDI_WHITE, 24);
            GdiTextT(30, y + 5, "Pinned", SH_TEXT2);
            y += 24;
        }
        if (i == 8) {
            GdiAlphaFill(RECT(28, y + 2, 136, 1), GDI_WHITE, 24);
            y += 12;
        }
        GdiRect row = RECT(24, y, 144, 30);
        if (i == g_workspace_selected) {
            GdiRoundAlpha(row, 8, GDI_WHITE, 30);
            GdiRoundRect(RECT(row.x, row.y + 7, 2, 16), 1, ACCENT, GDI_TRANSPARENT);
        }
        if (i >= 4 && i <= 7) AppDrawIcon((AppId)g_workspace_nav[i].app, 32, y + 5, 20);
        else AppDrawGlyph(g_workspace_nav[i].glyph, 32, y + 7, 16, i == g_workspace_selected ? ACCENT : SH_TEXT2);
        GdiTextT(62, y + 7, g_workspace_nav[i].name, SH_TEXT);
        HOT_BG(row, ACT_WORKSPACE, i);
        y += 32;
    }
}

static void draw_aurora_workspace(void)
{
    if (!aurora_workspace_visible()) return;
    int sw = GdiScreenW();
    AppDrawGlyph(GL_NOVA, 26, 22, 24, ACCENT);
    GdiTextBold(62, 27, "N O V A O S", SH_TEXT);
    GdiTextLarge(28, 60, g_clock_time, SH_TEXT);
    GdiTextT(28, 97, g_clock_short, SH_TEXT);
    GdiTextT(28, 122, "A brighter tomorrow lives", SH_TEXT2);
    GdiTextT(28, 140, "in your next idea.", SH_TEXT2);

    int search_w = sw >= 1200 ? 520 : 380;
    GdiRect search = RECT((sw - search_w) / 2, 18, search_w, 40);
    glass(search, 20);
    AppDrawGlyph(GL_SEARCH, search.x + 14, search.y + 12, 16, SH_TEXT);
    GdiTextT(search.x + 40, search.y + 12, "Search apps, files and settings", SH_TEXT);
    GdiRoundAlpha(RECT(search.x + search.w - 74, search.y + 8, 62, 24), 6, GDI_WHITE, 24);
    GdiTextCenter(search.x + search.w - 74, search.y + 12, 62, "Ctrl + K", SH_TEXT2);
    HOT_BG(search, ACT_SEARCH, 0);

    draw_aurora_sidebar();
    char user[24];
    fit_text(g_workspace_user, 130, user, sizeof(user), false);
    GdiTextT(sw - 166, 28, user, SH_TEXT);
    GdiFillCircle(sw - 26, 36, 14, GDI_C(67, 82, 136));
    GdiTextCenter(sw - 40, 28, 28, g_workspace_initials, SH_TEXT);
    HOT_BG(RECT(sw - 176, 18, 164, 36), ACT_APP, APP_SETTINGS);

    /* Real local date; selecting any part opens the full Calendar app. */
    int x = sw - 252;
    GdiRect calendar = RECT(x, 80, 232, 228);
    workspace_glass(calendar);
    RtcTime t;
    TzLocalNow(&t);
    static const char *const months[] = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December",
    };
    static const int offset[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    static const int lengths[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int month = t.month >= 1 && t.month <= 12 ? t.month : 1;
    int y = t.year - (month < 3);
    int first = (y + y / 4 - y / 100 + y / 400 + offset[month - 1] + 1) % 7;
    int count = lengths[month - 1];
    if (month == 2 && (t.year % 4 == 0 && (t.year % 100 != 0 || t.year % 400 == 0))) count++;
    char title[32];
    ksnprintf(title, sizeof(title), "%s %u", months[month - 1], t.year);
    GdiTextBold(x + 16, 98, title, SH_TEXT);
    static const char *const weekdays[] = { "S", "M", "T", "W", "T", "F", "S" };
    for (int col = 0; col < 7; col++)
        GdiTextCenter(x + 12 + col * 30, 130, 30, weekdays[col], SH_TEXT2);
    for (int day = 1; day <= count; day++) {
        int cell = first + day - 1;
        int cx = x + 12 + (cell % 7) * 30;
        int cy = 154 + (cell / 7) * 23;
        if (day == t.day) GdiRoundRect(RECT(cx + 3, cy - 3, 24, 22), 11, ACCENT, GDI_TRANSPARENT);
        char date[4];
        ksnprintf(date, sizeof(date), "%d", day);
        GdiTextCenter(cx, cy, 30, date, SH_TEXT);
    }
    HOT_BG(calendar, ACT_CLOCK, 0);

    GdiRect card = RECT(x, 324, 232, 228);
    workspace_glass(card);
    AppDrawGlyph(GL_NOVA, x + 16, 340, 20, ACCENT);
    GdiTextBold(x + 46, 342, "Your workspace", SH_TEXT);
    char greeting[64], fitted[64];
    ksnprintf(greeting, sizeof(greeting), "%s, %s",
              t.hour < 12 ? "Good morning" : t.hour < 18 ? "Good afternoon" : "Good evening",
              g_workspace_user);
    fit_text(greeting, 200, fitted, sizeof(fitted), false);
    GdiTextT(x + 16, 370, fitted, SH_TEXT);
    GdiTextT(x + 16, 388, "What would you like to do?", SH_TEXT2);
    static const struct { const char *title; AppId app; } shortcuts[] = {
        { "Browse your files", APP_EXPLORER },
        { "Write a note", APP_NOTEPAD },
        { "Explore the web", APP_NETSURF },
    };
    for (int i = 0; i < 3; i++) {
        GdiRect button = RECT(x + 12, 416 + i * 40, 208, 36);
        GdiRoundAlpha(button, 8, GDI_WHITE, 18);
        GdiRoundBorderAlpha(button, 8, GDI_WHITE, 30);
        AppDrawIcon(shortcuts[i].app, button.x + 8, button.y + 6, 24);
        GdiTextT(button.x + 42, button.y + 10, shortcuts[i].title, SH_TEXT);
        HOT_BG(button, ACT_APP, shortcuts[i].app);
    }
}
