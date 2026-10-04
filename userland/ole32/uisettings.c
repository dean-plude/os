/*
 * Windows.UI.ViewManagement.UISettings and UIViewSettings, the Windows
 * Runtime classes Win32 programs ask for the user's colours and interface
 * settings.  Qt's Windows platform plugin (qwindows.dll, Qt 6) builds its
 * palette from UISettings.GetColorValue (Background, Foreground and the
 * accent shades) and asks UIViewSettings whether the PC is in tablet mode.
 *
 * The values come from where Windows keeps them, under HKEY_CURRENT_USER:
 *   Software\Microsoft\Windows\CurrentVersion\Themes\Personalize
 *       AppsUseLightTheme (DWORD, absent: 1, light, as user32 draws) and
 *       EnableTransparency (absent: 0; NovaOS composes no transparency)
 *   Software\Microsoft\Windows\CurrentVersion\Explorer\Accent
 *       AccentPalette (32 bytes: eight R,G,B,0 entries, the lightest shade
 *       first; absent: the blue NovaOS's dwmapi reports, 0078D7)
 *   Software\Microsoft\Accessibility  TextScaleFactor (percent, 100)
 *   Control Panel\Accessibility       DynamicScrollbars (absent: 0; NovaOS's
 *       scroll bars stay shown)
 * and the rest from user32 (system colours, metrics, caret and mouse
 * times).  Changing those keys raises ColorValuesChanged,
 * TextScaleFactorChanged, AdvancedEffectsEnabledChanged and
 * AutoHideScrollBarsChanged on every UISettings with a handler, from a
 * thread of the process as Windows does (a thread watches the keys once a
 * first handler is added).
 *
 * Interface IDs and method orders: Windows SDK windows.ui.viewmanagement.idl
 * (MinGW-w64 and Wine carry the same IIDs).
 */
#define NOVA_BUILD_OLE32
#include <objbase.h>

typedef struct HSTRING_ *HSTRING;
HRESULT WINAPI WindowsCreateString(const WCHAR *src, UINT32 len, HSTRING *out);     /* winrt.c */
int _fltused = 0x9875;      /* floating point in use: Size and TextScaleFactor (the compiler references it) */

static const GUID IID_IUnknown_u          = { 0x00000000, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_IInspectable_u      = { 0xAF86E2E0, 0xB12D, 0x4C6A, { 0x9C, 0x5A, 0xD7, 0xAA, 0x65, 0x10, 0x1E, 0x90 } };
static const GUID IID_IAgileObject_u      = { 0x94EA2B94, 0xE9CC, 0x49E0, { 0xC0, 0xFF, 0xEE, 0x64, 0xCA, 0x8F, 0x5B, 0x90 } };
static const GUID IID_IActivationFactory_u = { 0x00000035, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_IUISettings  = { 0x85361600, 0x1C63, 0x4627, { 0xBC, 0xB1, 0x3A, 0x89, 0xE0, 0xBC, 0x9C, 0x55 } };
static const GUID IID_IUISettings2 = { 0xBAD82401, 0x2721, 0x44F9, { 0xBB, 0x91, 0x2B, 0xB2, 0x28, 0xBE, 0x44, 0x2F } };
static const GUID IID_IUISettings3 = { 0x03021BE4, 0x5254, 0x4781, { 0x81, 0x94, 0x51, 0x68, 0xF7, 0xD0, 0x6D, 0x7B } };
static const GUID IID_IUISettings4 = { 0x52BB3002, 0x919B, 0x4D6B, { 0x9B, 0x78, 0x8D, 0xD6, 0x6F, 0xF4, 0xB9, 0x3B } };
static const GUID IID_IUISettings5 = { 0x5349D588, 0x0CB5, 0x5F05, { 0xBD, 0x34, 0x70, 0x6B, 0x32, 0x31, 0xF0, 0xBD } };
static const GUID IID_IUISettings6 = { 0xAEF19BD7, 0xFE31, 0x5A04, { 0xAD, 0xA4, 0x46, 0x9A, 0xAE, 0xC6, 0xDF, 0xA9 } };
static const GUID IID_IUIViewSettings        = { 0xC63657F6, 0x8850, 0x470D, { 0x88, 0xF8, 0x45, 0x5E, 0x16, 0xEA, 0x2C, 0x26 } };
static const GUID IID_IUIViewSettingsStatics = { 0x595C97A5, 0xF8F6, 0x41CF, { 0xB0, 0xFB, 0xAA, 0xCD, 0xB8, 0x1F, 0xD5, 0xF6 } };
static const GUID IID_IUIViewSettingsInterop = { 0x3694DBF9, 0x8F68, 0x44BE, { 0x8F, 0xF5, 0x19, 0x5C, 0x98, 0xED, 0xE8, 0xA6 } };

#define E_NOTFOUND_     ((HRESULT)0x80070490L)      /* HRESULT_FROM_WIN32(ERROR_NOT_FOUND) */
#define E_BADWINDOW_    ((HRESULT)0x80070578L)      /* HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE) */
#define E_NOTIMPL_      ((HRESULT)0x80004001L)

typedef struct { BYTE A, R, G, B; } WColor;         /* Windows.UI.Color */
typedef struct { float Width, Height; } WSize;      /* Windows.Foundation.Size */
typedef struct { INT64 value; } EventToken;         /* EventRegistrationToken */

static HSTRING name_of(const WCHAR *s)
{
    HSTRING h = 0;
    UINT32 n = 0;
    while (s[n]) n++;
    WindowsCreateString(s, n, &h);
    return h;
}

/* ---- What the registry says ------------------------------------------- */
#define K_PERSONALIZE L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"
#define K_ACCENT      L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent"
#define K_TEXTSCALE   L"Software\\Microsoft\\Accessibility"
#define K_SCROLLBARS  L"Control Panel\\Accessibility"

/* Windows 10's palette for its default accent (0078D7), lightest first */
static const BYTE g_default_palette[32] = {
    0xA6, 0xD8, 0xFF, 0, 0x76, 0xB9, 0xED, 0, 0x42, 0x9C, 0xE3, 0, 0x00, 0x78, 0xD7, 0,
    0x00, 0x5A, 0x9E, 0, 0x00, 0x42, 0x75, 0, 0x00, 0x26, 0x42, 0, 0xF7, 0x63, 0x0C, 0,
};

typedef struct {
    BOOL  light;            /* apps use the light theme */
    BOOL  effects;          /* transparency */
    BOOL  autohide;         /* scroll bars hide */
    DWORD scale;            /* text scale, percent */
    BYTE  palette[32];
} Snapshot;

static BOOL reg_dword(const WCHAR *key, const WCHAR *value, DWORD *out)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_READ, &k)) return FALSE;
    DWORD type = 0, v = 0, n = sizeof(v);
    LONG r = RegQueryValueExW(k, value, 0, &type, (BYTE *)&v, &n);
    RegCloseKey(k);
    if (r || type != REG_DWORD || n != sizeof(v)) return FALSE;
    *out = v;
    return TRUE;
}

static void read_snapshot(Snapshot *s)
{
    DWORD v;
    s->light = reg_dword(K_PERSONALIZE, L"AppsUseLightTheme", &v) ? v != 0 : TRUE;
    s->effects = reg_dword(K_PERSONALIZE, L"EnableTransparency", &v) ? v != 0 : FALSE;
    s->autohide = reg_dword(K_SCROLLBARS, L"DynamicScrollbars", &v) ? v != 0 : FALSE;
    s->scale = reg_dword(K_TEXTSCALE, L"TextScaleFactor", &v) && v >= 100 && v <= 225 ? v : 100;
    CopyMemory(s->palette, g_default_palette, sizeof(s->palette));
    HKEY k;
    if (!RegOpenKeyExW(HKEY_CURRENT_USER, K_ACCENT, 0, KEY_READ, &k)) {
        BYTE p[32];
        DWORD type = 0, n = sizeof(p);
        if (!RegQueryValueExW(k, L"AccentPalette", 0, &type, p, &n) && type == REG_BINARY && n == sizeof(p))
            CopyMemory(s->palette, p, sizeof(p));
        RegCloseKey(k);
    }
}

static WColor rgb(BYTE r, BYTE g, BYTE b) { WColor c = { 0xFF, r, g, b }; return c; }
static WColor colorref(COLORREF c) { return rgb(GetRValue(c), GetGValue(c), GetBValue(c)); }

/* UIColorType: Background, Foreground, AccentDark3, AccentDark2, AccentDark1,
 * Accent, AccentLight1, AccentLight2, AccentLight3 (Complement is "not
 * used") */
static HRESULT color_value(const Snapshot *s, int type, WColor *out)
{
    if (type == 0) { *out = s->light ? rgb(0xFF, 0xFF, 0xFF) : rgb(0, 0, 0); return S_OK; }
    if (type == 1) { *out = s->light ? rgb(0, 0, 0) : rgb(0xFF, 0xFF, 0xFF); return S_OK; }
    if (type < 2 || type > 8) return E_INVALIDARG;
    const BYTE *e = s->palette + (8 - type) * 4;              /* AccentDark3 is entry 6, AccentLight3 entry 0 */
    *out = rgb(e[0], e[1], e[2]);
    return S_OK;
}

/* ---- Event handlers ----------------------------------------------------- */
enum { EV_TEXTSCALE, EV_COLORS, EV_EFFECTS, EV_AUTOHIDE, EV_ANIMATIONS, EV_DURATION };
typedef struct UISettings UISettings;
typedef struct { UISettings *owner; int event; IUnknown *handler; INT64 token; } Handler;
static SRWLOCK g_lock = SRWLOCK_INIT;
static Handler *g_handlers;
static int g_nhandlers, g_caphandlers;
static INT64 g_next_token = 1;
static BOOL g_watching;
static void start_watching(void);

static HRESULT add_handler(UISettings *owner, int event, IUnknown *h, EventToken *tok)
{
    if (!h || !tok) return E_POINTER;
    AcquireSRWLockExclusive(&g_lock);
    if (g_nhandlers == g_caphandlers) {
        int cap = g_caphandlers ? g_caphandlers * 2 : 8;
        Handler *n = g_handlers ? HeapReAlloc(GetProcessHeap(), 0, g_handlers, cap * sizeof(Handler))
                                : HeapAlloc(GetProcessHeap(), 0, cap * sizeof(Handler));
        if (!n) { ReleaseSRWLockExclusive(&g_lock); return E_OUTOFMEMORY; }
        g_handlers = n;
        g_caphandlers = cap;
    }
    h->lpVtbl->AddRef(h);
    Handler *e = &g_handlers[g_nhandlers++];
    e->owner = owner;
    e->event = event;
    e->handler = h;
    e->token = tok->value = g_next_token++;
    BOOL start = !g_watching;
    g_watching = TRUE;
    ReleaseSRWLockExclusive(&g_lock);
    if (start) start_watching();
    return S_OK;
}

/* Removes @owner's handler @token (any token if @token is 0: all of
 * @owner's, when it goes); an unknown token is not an error */
static void remove_handlers(UISettings *owner, int event, INT64 token)
{
    IUnknown *drop[16];
    for (;;) {
        int n = 0;
        AcquireSRWLockExclusive(&g_lock);
        for (int i = 0; i < g_nhandlers && n < 16; ) {
            Handler *e = &g_handlers[i];
            if (e->owner == owner && (!token || (e->token == token && e->event == event))) {
                drop[n++] = e->handler;
                *e = g_handlers[--g_nhandlers];
            } else i++;
        }
        ReleaseSRWLockExclusive(&g_lock);
        for (int i = 0; i < n; i++) drop[i]->lpVtbl->Release(drop[i]);
        if (n < 16) return;
    }
}

/* ---- UISettings --------------------------------------------------------- */
typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(void *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(void *);
    ULONG   (STDMETHODCALLTYPE *Release)(void *);
    HRESULT (STDMETHODCALLTYPE *GetIids)(void *, ULONG *, GUID **);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(void *, HSTRING *);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(void *, int *);
} InspVtbl;

typedef struct { InspVtbl i;
    HRESULT (STDMETHODCALLTYPE *get_HandPreference)(void *, int *);
    HRESULT (STDMETHODCALLTYPE *get_CursorSize)(void *, WSize *);
    HRESULT (STDMETHODCALLTYPE *get_ScrollBarSize)(void *, WSize *);
    HRESULT (STDMETHODCALLTYPE *get_ScrollBarArrowSize)(void *, WSize *);
    HRESULT (STDMETHODCALLTYPE *get_ScrollBarThumbBoxSize)(void *, WSize *);
    HRESULT (STDMETHODCALLTYPE *get_MessageDuration)(void *, UINT32 *);
    HRESULT (STDMETHODCALLTYPE *get_AnimationsEnabled)(void *, BOOLEAN *);
    HRESULT (STDMETHODCALLTYPE *get_CaretBrowsingEnabled)(void *, BOOLEAN *);
    HRESULT (STDMETHODCALLTYPE *get_CaretBlinkRate)(void *, UINT32 *);
    HRESULT (STDMETHODCALLTYPE *get_CaretWidth)(void *, UINT32 *);
    HRESULT (STDMETHODCALLTYPE *get_DoubleClickTime)(void *, UINT32 *);
    HRESULT (STDMETHODCALLTYPE *get_MouseHoverTime)(void *, UINT32 *);
    HRESULT (STDMETHODCALLTYPE *UIElementColor)(void *, int, WColor *);
} Ui1Vtbl;
typedef struct { InspVtbl i;                                /* the shape of IUISettings2, 3, 4 and 5 */
    HRESULT (STDMETHODCALLTYPE *get)(void *, void *);
    HRESULT (STDMETHODCALLTYPE *add)(void *, IUnknown *, EventToken *);
    HRESULT (STDMETHODCALLTYPE *remove)(void *, EventToken);
} UiEventVtbl;
typedef struct { InspVtbl i;
    HRESULT (STDMETHODCALLTYPE *GetColorValue)(void *, int, WColor *);
    HRESULT (STDMETHODCALLTYPE *add)(void *, IUnknown *, EventToken *);
    HRESULT (STDMETHODCALLTYPE *remove)(void *, EventToken);
} Ui3Vtbl;
typedef struct { InspVtbl i;
    HRESULT (STDMETHODCALLTYPE *add_AnimationsEnabledChanged)(void *, IUnknown *, EventToken *);
    HRESULT (STDMETHODCALLTYPE *remove_AnimationsEnabledChanged)(void *, EventToken);
    HRESULT (STDMETHODCALLTYPE *add_MessageDurationChanged)(void *, IUnknown *, EventToken *);
    HRESULT (STDMETHODCALLTYPE *remove_MessageDurationChanged)(void *, EventToken);
} Ui6Vtbl;

struct UISettings {
    const Ui1Vtbl *v1;
    const UiEventVtbl *v2;
    const Ui3Vtbl *v3;
    const UiEventVtbl *v4, *v5;
    const Ui6Vtbl *v6;
    LONG refs;
};
#define UI_OF(p, f) ((UISettings *)((BYTE *)(p) - __builtin_offsetof(UISettings, f)))

static HRESULT ui_qi(UISettings *u, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, &IID_IUnknown_u) || IsEqualGUID(iid, &IID_IInspectable_u) ||
        IsEqualGUID(iid, &IID_IAgileObject_u) || IsEqualGUID(iid, &IID_IUISettings)) *out = &u->v1;
    else if (IsEqualGUID(iid, &IID_IUISettings2)) *out = &u->v2;
    else if (IsEqualGUID(iid, &IID_IUISettings3)) *out = &u->v3;
    else if (IsEqualGUID(iid, &IID_IUISettings4)) *out = &u->v4;
    else if (IsEqualGUID(iid, &IID_IUISettings5)) *out = &u->v5;
    else if (IsEqualGUID(iid, &IID_IUISettings6)) *out = &u->v6;
    else { *out = 0; return E_NOINTERFACE; }
    InterlockedIncrement(&u->refs);
    return S_OK;
}
static ULONG ui_release(UISettings *u)
{
    LONG r = InterlockedDecrement(&u->refs);
    if (!r) {
        remove_handlers(u, 0, 0);
        HeapFree(GetProcessHeap(), 0, u);
    }
    return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE ui_iids(void *o, ULONG *n, GUID **iids)
{
    (void)o;
    static const GUID *const all[] = { &IID_IUISettings, &IID_IUISettings2, &IID_IUISettings3,
                                       &IID_IUISettings4, &IID_IUISettings5, &IID_IUISettings6 };
    if (!n || !iids) return E_POINTER;
    *iids = CoTaskMemAlloc(sizeof(all) / sizeof(all[0]) * sizeof(GUID));
    if (!*iids) { *n = 0; return E_OUTOFMEMORY; }
    for (ULONG i = 0; i < sizeof(all) / sizeof(all[0]); i++) (*iids)[i] = *all[i];
    *n = sizeof(all) / sizeof(all[0]);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ui_name(void *o, HSTRING *n)
{
    (void)o;
    if (!n) return E_POINTER;
    *n = name_of(L"Windows.UI.ViewManagement.UISettings");
    return *n ? S_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE ui_trust(void *o, int *t) { (void)o; if (!t) return E_POINTER; *t = 0; return S_OK; }   /* BaseTrust */

/* IUnknown's three for each interface pointer */
#define UI_UNKNOWN(f) \
    static HRESULT STDMETHODCALLTYPE f##_qi(void *o, REFIID iid, void **out) { return ui_qi(UI_OF(o, f), iid, out); } \
    static ULONG STDMETHODCALLTYPE f##_addref(void *o) { return (ULONG)InterlockedIncrement(&UI_OF(o, f)->refs); } \
    static ULONG STDMETHODCALLTYPE f##_release(void *o) { return ui_release(UI_OF(o, f)); }
UI_UNKNOWN(v1) UI_UNKNOWN(v2) UI_UNKNOWN(v3) UI_UNKNOWN(v4) UI_UNKNOWN(v5) UI_UNKNOWN(v6)
#define UI_INSP(f) { f##_qi, f##_addref, f##_release, ui_iids, ui_name, ui_trust }

static UINT spi_uint(UINT action, UINT dflt)
{
    UINT v = dflt;
    return SystemParametersInfoW(action, 0, &v, 0) ? v : dflt;
}
static WSize metrics(int cx, int cy) { WSize s = { (float)GetSystemMetrics(cx), (float)GetSystemMetrics(cy) }; return s; }

/* IUISettings */
static HRESULT STDMETHODCALLTYPE ui_hand(void *o, int *v) { (void)o; if (!v) return E_POINTER; *v = 1; return S_OK; }     /* RightHanded */
static HRESULT STDMETHODCALLTYPE ui_cursor(void *o, WSize *v) { (void)o; if (!v) return E_POINTER; *v = metrics(SM_CXCURSOR, SM_CYCURSOR); return S_OK; }
static HRESULT STDMETHODCALLTYPE ui_sbsize(void *o, WSize *v) { (void)o; if (!v) return E_POINTER; *v = metrics(SM_CXVSCROLL, SM_CYHSCROLL); return S_OK; }
static HRESULT STDMETHODCALLTYPE ui_sbarrow(void *o, WSize *v) { (void)o; if (!v) return E_POINTER; *v = metrics(SM_CXHSCROLL, SM_CYVSCROLL); return S_OK; }
static HRESULT STDMETHODCALLTYPE ui_sbthumb(void *o, WSize *v) { (void)o; if (!v) return E_POINTER; *v = metrics(SM_CXHTHUMB, SM_CYVTHUMB); return S_OK; }
static HRESULT STDMETHODCALLTYPE ui_duration(void *o, UINT32 *v) { (void)o; if (!v) return E_POINTER; *v = spi_uint(0x2016 /* SPI_GETMESSAGEDURATION */, 5); return S_OK; }
static HRESULT STDMETHODCALLTYPE ui_anim(void *o, BOOLEAN *v)
{
    (void)o;
    if (!v) return E_POINTER;
    *v = spi_uint(0x1042 /* SPI_GETCLIENTAREAANIMATION */, TRUE) != 0;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ui_caretbrowse(void *o, BOOLEAN *v) { (void)o; if (!v) return E_POINTER; *v = FALSE; return S_OK; }
static HRESULT STDMETHODCALLTYPE ui_blink(void *o, UINT32 *v) { (void)o; if (!v) return E_POINTER; *v = GetCaretBlinkTime(); return S_OK; }
static HRESULT STDMETHODCALLTYPE ui_caretw(void *o, UINT32 *v)
{
    (void)o;
    if (!v) return E_POINTER;
    UINT w = spi_uint(0x2006 /* SPI_GETCARETWIDTH */, 1);
    *v = w ? w : 1;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ui_dblclick(void *o, UINT32 *v) { (void)o; if (!v) return E_POINTER; *v = GetDoubleClickTime(); return S_OK; }
static HRESULT STDMETHODCALLTYPE ui_hover(void *o, UINT32 *v) { (void)o; if (!v) return E_POINTER; *v = spi_uint(0x0066 /* SPI_GETMOUSEHOVERTIME */, 400); return S_OK; }
/* UIElementType: the classic system colours, and AccentColor (1000) */
static HRESULT STDMETHODCALLTYPE ui_element(void *o, int type, WColor *v)
{
    (void)o;
    static const int sys[] = { COLOR_ACTIVECAPTION, COLOR_BACKGROUND, COLOR_BTNFACE, COLOR_BTNTEXT, COLOR_CAPTIONTEXT,
                               COLOR_GRAYTEXT, COLOR_HIGHLIGHT, COLOR_HIGHLIGHTTEXT, COLOR_HOTLIGHT, COLOR_INACTIVECAPTION,
                               COLOR_INACTIVECAPTIONTEXT, COLOR_WINDOW, COLOR_WINDOWTEXT };
    if (!v) return E_POINTER;
    if (type >= 0 && type < (int)(sizeof(sys) / sizeof(sys[0]))) { *v = colorref(GetSysColor(sys[type])); return S_OK; }
    if (type == 1000) {
        Snapshot s;
        read_snapshot(&s);
        return color_value(&s, 5, v);
    }
    return E_INVALIDARG;
}
static const Ui1Vtbl g_ui1 = { UI_INSP(v1), ui_hand, ui_cursor, ui_sbsize, ui_sbarrow, ui_sbthumb, ui_duration, ui_anim,
                               ui_caretbrowse, ui_blink, ui_caretw, ui_dblclick, ui_hover, ui_element };

/* IUISettings2: TextScaleFactor */
static HRESULT STDMETHODCALLTYPE ui_scale(void *o, void *v)
{
    (void)o;
    if (!v) return E_POINTER;
    Snapshot s;
    read_snapshot(&s);
    *(double *)v = s.scale / 100.0;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ui_add_scale(void *o, IUnknown *h, EventToken *t) { return add_handler(UI_OF(o, v2), EV_TEXTSCALE, h, t); }
static HRESULT STDMETHODCALLTYPE ui_rm_scale(void *o, EventToken t) { remove_handlers(UI_OF(o, v2), EV_TEXTSCALE, t.value); return S_OK; }
static const UiEventVtbl g_ui2 = { UI_INSP(v2), ui_scale, ui_add_scale, ui_rm_scale };

/* IUISettings3: GetColorValue(UIColorType, Color *) and ColorValuesChanged */
static HRESULT STDMETHODCALLTYPE ui_add_colors(void *o, IUnknown *h, EventToken *t) { return add_handler(UI_OF(o, v3), EV_COLORS, h, t); }
static HRESULT STDMETHODCALLTYPE ui_rm_colors(void *o, EventToken t) { remove_handlers(UI_OF(o, v3), EV_COLORS, t.value); return S_OK; }
static HRESULT STDMETHODCALLTYPE ui_getcolor(void *o, int type, WColor *v)
{
    (void)o;
    if (!v) return E_POINTER;
    Snapshot s;
    read_snapshot(&s);
    return color_value(&s, type, v);
}
static const Ui3Vtbl g_ui3 = { UI_INSP(v3), ui_getcolor, ui_add_colors, ui_rm_colors };

/* IUISettings4: AdvancedEffectsEnabled (transparency) */
static HRESULT STDMETHODCALLTYPE ui_effects(void *o, void *v)
{
    (void)o;
    if (!v) return E_POINTER;
    Snapshot s;
    read_snapshot(&s);
    *(BOOLEAN *)v = (BOOLEAN)s.effects;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ui_add_effects(void *o, IUnknown *h, EventToken *t) { return add_handler(UI_OF(o, v4), EV_EFFECTS, h, t); }
static HRESULT STDMETHODCALLTYPE ui_rm_effects(void *o, EventToken t) { remove_handlers(UI_OF(o, v4), EV_EFFECTS, t.value); return S_OK; }
static const UiEventVtbl g_ui4 = { UI_INSP(v4), ui_effects, ui_add_effects, ui_rm_effects };

/* IUISettings5: AutoHideScrollBars */
static HRESULT STDMETHODCALLTYPE ui_autohide(void *o, void *v)
{
    (void)o;
    if (!v) return E_POINTER;
    Snapshot s;
    read_snapshot(&s);
    *(BOOLEAN *)v = (BOOLEAN)s.autohide;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ui_add_autohide(void *o, IUnknown *h, EventToken *t) { return add_handler(UI_OF(o, v5), EV_AUTOHIDE, h, t); }
static HRESULT STDMETHODCALLTYPE ui_rm_autohide(void *o, EventToken t) { remove_handlers(UI_OF(o, v5), EV_AUTOHIDE, t.value); return S_OK; }
static const UiEventVtbl g_ui5 = { UI_INSP(v5), ui_autohide, ui_add_autohide, ui_rm_autohide };

/* IUISettings6: the animation and message-duration settings never change */
static HRESULT STDMETHODCALLTYPE ui_add_anim(void *o, IUnknown *h, EventToken *t) { return add_handler(UI_OF(o, v6), EV_ANIMATIONS, h, t); }
static HRESULT STDMETHODCALLTYPE ui_rm_anim(void *o, EventToken t) { remove_handlers(UI_OF(o, v6), EV_ANIMATIONS, t.value); return S_OK; }
static HRESULT STDMETHODCALLTYPE ui_add_dur(void *o, IUnknown *h, EventToken *t) { return add_handler(UI_OF(o, v6), EV_DURATION, h, t); }
static HRESULT STDMETHODCALLTYPE ui_rm_dur(void *o, EventToken t) { remove_handlers(UI_OF(o, v6), EV_DURATION, t.value); return S_OK; }
static const Ui6Vtbl g_ui6 = { UI_INSP(v6), ui_add_anim, ui_rm_anim, ui_add_dur, ui_rm_dur };

static HRESULT new_uisettings(void **out)
{
    UISettings *u = HeapAlloc(GetProcessHeap(), 0, sizeof(UISettings));
    if (!u) return E_OUTOFMEMORY;
    u->v1 = &g_ui1;
    u->v2 = &g_ui2;
    u->v3 = &g_ui3;
    u->v4 = &g_ui4;
    u->v5 = &g_ui5;
    u->v6 = &g_ui6;
    u->refs = 1;
    *out = &u->v1;
    return S_OK;
}

/* ---- The watcher: raises the events when the keys change ---------------- */
static const WCHAR *const g_watched[] = { K_PERSONALIZE, K_ACCENT, K_TEXTSCALE, K_SCROLLBARS };
#define NWATCHED (sizeof(g_watched) / sizeof(g_watched[0]))

/* Calls every handler for @event: TypedEventHandler<UISettings, T>::Invoke
 * (sender, args) is the slot after IUnknown's.  No arguments object is
 * passed; the sender is the UISettings the handler was added to. */
static void raise(int event)
{
    Handler copy[32];
    int n = 0;
    AcquireSRWLockShared(&g_lock);
    for (int i = 0; i < g_nhandlers && n < 32; i++)
        if (g_handlers[i].event == event) {
            copy[n] = g_handlers[i];
            copy[n].handler->lpVtbl->AddRef(copy[n].handler);
            InterlockedIncrement(&copy[n].owner->refs);
            n++;
        }
    ReleaseSRWLockShared(&g_lock);
    for (int i = 0; i < n; i++) {
        typedef HRESULT (STDMETHODCALLTYPE *Invoke)(IUnknown *, void *, void *);
        Invoke inv = (Invoke)((void **)copy[i].handler->lpVtbl)[3];
        inv(copy[i].handler, &copy[i].owner->v1, 0);
        copy[i].handler->lpVtbl->Release(copy[i].handler);
        ui_release(copy[i].owner);
    }
}

/* @arg: an event set once the keys are watched */
static DWORD WINAPI watch_thread(void *arg)
{
    HKEY keys[NWATCHED];
    HANDLE evs[NWATCHED];
    DWORD n = 0;
    for (DWORD i = 0; i < NWATCHED; i++) {
        HKEY k;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, g_watched[i], 0, 0, 0, KEY_READ | KEY_NOTIFY, 0, &k, 0)) continue;
        HANDLE e = CreateEventW(0, FALSE, FALSE, 0);
        if (!e) { RegCloseKey(k); continue; }
        if (RegNotifyChangeKeyValue(k, FALSE, REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET, e, TRUE)) {
            CloseHandle(e);
            RegCloseKey(k);
            continue;
        }
        keys[n] = k;
        evs[n++] = e;
    }
    Snapshot was;
    read_snapshot(&was);
    SetEvent((HANDLE)arg);
    if (!n) return 0;
    for (;;) {
        DWORD w = WaitForMultipleObjects(n, evs, FALSE, INFINITE);
        if (w >= WAIT_OBJECT_0 + n) return 0;
        RegNotifyChangeKeyValue(keys[w - WAIT_OBJECT_0], FALSE, REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET,
                                evs[w - WAIT_OBJECT_0], TRUE);
        Snapshot now;
        read_snapshot(&now);
        BOOL palette = FALSE;
        for (int i = 0; i < 32; i++) palette |= now.palette[i] != was.palette[i];
        if (now.light != was.light || palette) raise(EV_COLORS);
        if (now.effects != was.effects) raise(EV_EFFECTS);
        if (now.scale != was.scale) raise(EV_TEXTSCALE);
        if (now.autohide != was.autohide) raise(EV_AUTOHIDE);
        was = now;
    }
}

/* A change made after the first handler is added raises its event */
static void start_watching(void)
{
    HANDLE ready = CreateEventW(0, TRUE, FALSE, 0);
    HANDLE t = CreateThread(0, 0, watch_thread, ready, 0, 0);
    if (t) {
        WaitForSingleObject(ready, 5000);
        CloseHandle(t);
    }
    if (ready) CloseHandle(ready);
}

/* ---- The UISettings activation factory ---------------------------------- */
typedef struct { InspVtbl i; HRESULT (STDMETHODCALLTYPE *ActivateInstance)(void *, void **); } FactoryVtbl;
typedef struct { const FactoryVtbl *v; } Factory;
static Factory g_uis_factory;

static ULONG STDMETHODCALLTYPE static_ref(void *o) { (void)o; return 2; }
static HRESULT STDMETHODCALLTYPE no_iids(void *o, ULONG *n, GUID **iids) { (void)o; *n = 0; *iids = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE uisf_qi(void *o, REFIID iid, void **out)
{
    (void)o;
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, &IID_IUnknown_u) || IsEqualGUID(iid, &IID_IInspectable_u) ||
        IsEqualGUID(iid, &IID_IAgileObject_u) || IsEqualGUID(iid, &IID_IActivationFactory_u)) { *out = &g_uis_factory; return S_OK; }
    *out = 0;
    return E_NOINTERFACE;
}
static HRESULT STDMETHODCALLTYPE uisf_activate(void *o, void **out) { (void)o; if (!out) return E_POINTER; return new_uisettings(out); }
static const FactoryVtbl g_uisf_vtbl = { { uisf_qi, static_ref, static_ref, no_iids, ui_name, ui_trust }, uisf_activate };
static Factory g_uis_factory = { &g_uisf_vtbl };

/* ---- UIViewSettings: the desktop is never in tablet mode ---------------- */
typedef struct { InspVtbl i; HRESULT (STDMETHODCALLTYPE *get_UserInteractionMode)(void *, int *); } ViewVtbl;
typedef struct { InspVtbl i; HRESULT (STDMETHODCALLTYPE *GetForCurrentView)(void *, void **); } ViewStaticsVtbl;
typedef struct { InspVtbl i; HRESULT (STDMETHODCALLTYPE *GetForWindow)(void *, HWND, REFIID, void **); } ViewInteropVtbl;
typedef struct { const ViewVtbl *v; } View;
typedef struct { const FactoryVtbl *f; const ViewStaticsVtbl *s; const ViewInteropVtbl *i; } ViewFactory;
static View g_view;
static ViewFactory g_view_factory;

static HRESULT STDMETHODCALLTYPE view_name(void *o, HSTRING *n)
{
    (void)o;
    if (!n) return E_POINTER;
    *n = name_of(L"Windows.UI.ViewManagement.UIViewSettings");
    return *n ? S_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE view_qi(void *o, REFIID iid, void **out)
{
    (void)o;
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, &IID_IUnknown_u) || IsEqualGUID(iid, &IID_IInspectable_u) ||
        IsEqualGUID(iid, &IID_IAgileObject_u) || IsEqualGUID(iid, &IID_IUIViewSettings)) { *out = &g_view; return S_OK; }
    *out = 0;
    return E_NOINTERFACE;
}
static HRESULT STDMETHODCALLTYPE view_mode(void *o, int *m) { (void)o; if (!m) return E_POINTER; *m = 0; return S_OK; }   /* Mouse */
static const ViewVtbl g_view_vtbl = { { view_qi, static_ref, static_ref, no_iids, view_name, ui_trust }, view_mode };
static View g_view = { &g_view_vtbl };

static HRESULT STDMETHODCALLTYPE vf_qi(void *o, REFIID iid, void **out)
{
    (void)o;
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, &IID_IUnknown_u) || IsEqualGUID(iid, &IID_IInspectable_u) ||
        IsEqualGUID(iid, &IID_IAgileObject_u) || IsEqualGUID(iid, &IID_IActivationFactory_u)) { *out = &g_view_factory.f; return S_OK; }
    if (IsEqualGUID(iid, &IID_IUIViewSettingsStatics)) { *out = &g_view_factory.s; return S_OK; }
    if (IsEqualGUID(iid, &IID_IUIViewSettingsInterop)) { *out = &g_view_factory.i; return S_OK; }
    *out = 0;
    return E_NOINTERFACE;
}
/* A static class: no instances of its own */
static HRESULT STDMETHODCALLTYPE vf_activate(void *o, void **out) { (void)o; if (out) *out = 0; return E_NOTIMPL_; }
/* GetForCurrentView needs a CoreWindow, which a Win32 thread has not */
static HRESULT STDMETHODCALLTYPE vf_current(void *o, void **out) { (void)o; if (out) *out = 0; return E_NOTFOUND_; }
static HRESULT STDMETHODCALLTYPE vf_forwindow(void *o, HWND w, REFIID iid, void **out)
{
    (void)o;
    if (!out) return E_POINTER;
    *out = 0;
    if (!IsWindow(w)) return E_BADWINDOW_;
    return view_qi(&g_view, iid, out);
}
static const FactoryVtbl g_vf_vtbl = { { vf_qi, static_ref, static_ref, no_iids, view_name, ui_trust }, vf_activate };
static const ViewStaticsVtbl g_vs_vtbl = { { vf_qi, static_ref, static_ref, no_iids, view_name, ui_trust }, vf_current };
static const ViewInteropVtbl g_vi_vtbl = { { vf_qi, static_ref, static_ref, no_iids, view_name, ui_trust }, vf_forwindow };
static ViewFactory g_view_factory = { &g_vf_vtbl, &g_vs_vtbl, &g_vi_vtbl };

static BOOL named(const WCHAR *name, UINT32 len, const WCHAR *want)
{
    UINT32 i = 0;
    while (i < len && want[i] && name[i] == want[i]) i++;
    return i == len && !want[i];
}

/* The factory for UISettings or UIViewSettings, or REGDB_E_CLASSNOTREG */
HRESULT ole32_uisettings_factory(const WCHAR *name, UINT32 len, REFIID iid, void **out)
{
    if (named(name, len, L"Windows.UI.ViewManagement.UISettings")) return uisf_qi(&g_uis_factory, iid, out);
    if (named(name, len, L"Windows.UI.ViewManagement.UIViewSettings")) return vf_qi(&g_view_factory, iid, out);
    *out = 0;
    return REGDB_E_CLASSNOTREG;
}
