/*
 * qtthemetest — what Qt's Windows platform plugin (qwindows.dll, in GOG
 * GALAXY's client) asks of the Windows Runtime and CNG:
 *
 *   UISettings      activates (RoGetActivationFactory + ActivateInstance,
 *                   and RoActivateInstance); IUISettings to 6; the colours
 *                   GetColorValue gives for the light and dark app modes
 *                   and the accent palette; UIElementColor, the metrics and
 *                   times; AdvancedEffectsEnabled; ColorValuesChanged raised
 *                   when the theme's keys change, and not after removal
 *   UIViewSettings  GetForWindow (IUIViewSettingsInterop) says Mouse
 *   unknown classes REGDB_E_CLASSNOTREG
 *   BCryptEnumContextFunctions  Schannel's cipher suites
 *
 * The theme's registry values are put back as they were.
 */
#include <windows.h>
#include <objbase.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

typedef struct HSTRING_ *HSTRING;
static HRESULT (WINAPI *pCreateString)(const WCHAR *, UINT32, HSTRING *);
static HRESULT (WINAPI *pDeleteString)(HSTRING);
static const WCHAR *(WINAPI *pRawBuffer)(HSTRING, UINT32 *);
static HRESULT (WINAPI *pGetFactory)(HSTRING, REFIID, void **);
static HRESULT (WINAPI *pActivate)(HSTRING, void **);

static const GUID IID_IInspectable_t = { 0xAF86E2E0, 0xB12D, 0x4C6A, { 0x9C, 0x5A, 0xD7, 0xAA, 0x65, 0x10, 0x1E, 0x90 } };
static const GUID IID_IAgileObject_t = { 0x94EA2B94, 0xE9CC, 0x49E0, { 0xC0, 0xFF, 0xEE, 0x64, 0xCA, 0x8F, 0x5B, 0x90 } };
static const GUID IID_IActivationFactory_t = { 0x00000035, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID IID_IUISettings  = { 0x85361600, 0x1C63, 0x4627, { 0xBC, 0xB1, 0x3A, 0x89, 0xE0, 0xBC, 0x9C, 0x55 } };
static const GUID IID_IUISettings2 = { 0xBAD82401, 0x2721, 0x44F9, { 0xBB, 0x91, 0x2B, 0xB2, 0x28, 0xBE, 0x44, 0x2F } };
static const GUID IID_IUISettings3 = { 0x03021BE4, 0x5254, 0x4781, { 0x81, 0x94, 0x51, 0x68, 0xF7, 0xD0, 0x6D, 0x7B } };
static const GUID IID_IUISettings4 = { 0x52BB3002, 0x919B, 0x4D6B, { 0x9B, 0x78, 0x8D, 0xD6, 0x6F, 0xF4, 0xB9, 0x3B } };
static const GUID IID_IUISettings5 = { 0x5349D588, 0x0CB5, 0x5F05, { 0xBD, 0x34, 0x70, 0x6B, 0x32, 0x31, 0xF0, 0xBD } };
static const GUID IID_IUISettings6 = { 0xAEF19BD7, 0xFE31, 0x5A04, { 0xAD, 0xA4, 0x46, 0x9A, 0xAE, 0xC6, 0xDF, 0xA9 } };
static const GUID IID_IUIViewSettings = { 0xC63657F6, 0x8850, 0x470D, { 0x88, 0xF8, 0x45, 0x5E, 0x16, 0xEA, 0x2C, 0x26 } };
static const GUID IID_IUIViewSettingsInterop = { 0x3694DBF9, 0x8F68, 0x44BE, { 0x8F, 0xF5, 0x19, 0x5C, 0x98, 0xED, 0xE8, 0xA6 } };

typedef struct { BYTE A, R, G, B; } WColor;
typedef struct { float Width, Height; } WSize;
typedef struct { INT64 value; } EventToken;

/* method @slot of interface @p (IInspectable's six come first) */
#define SLOT(p, n) (((void **)*(void **)(p))[n])
#define RELEASE(p) ((ULONG (STDMETHODCALLTYPE *)(void *))SLOT(p, 2))(p)
static HRESULT qi(void *p, REFIID iid, void **out) { return ((HRESULT (STDMETHODCALLTYPE *)(void *, REFIID, void **))SLOT(p, 0))(p, iid, out); }

static HSTRING hs(const WCHAR *s)
{
    HSTRING h = 0;
    pCreateString(s, (UINT32)wcslen(s), &h);
    return h;
}

static HRESULT get_color(void *ui3, int type, WColor *c)
{
    return ((HRESULT (STDMETHODCALLTYPE *)(void *, int, WColor *))SLOT(ui3, 6))(ui3, type, c);
}
static BOOL is(WColor c, BYTE r, BYTE g, BYTE b) { return c.A == 0xFF && c.R == r && c.G == g && c.B == b; }

/* ---- A TypedEventHandler<UISettings, IInspectable> ---------------------- */
typedef struct { void *vtbl; LONG refs; LONG calls; void *sender; HANDLE ev; } Handler;
static HRESULT STDMETHODCALLTYPE h_qi(Handler *h, REFIID iid, void **out) { *out = h; InterlockedIncrement(&h->refs); (void)iid; return S_OK; }
static ULONG STDMETHODCALLTYPE h_addref(Handler *h) { return (ULONG)InterlockedIncrement(&h->refs); }
static ULONG STDMETHODCALLTYPE h_release(Handler *h) { return (ULONG)InterlockedDecrement(&h->refs); }
static HRESULT STDMETHODCALLTYPE h_invoke(Handler *h, void *sender, void *args)
{
    (void)args;
    h->sender = sender;
    InterlockedIncrement(&h->calls);
    SetEvent(h->ev);
    return S_OK;
}
static void *g_handler_vtbl[] = { (void *)h_qi, (void *)h_addref, (void *)h_release, (void *)h_invoke };

/* ---- The registry ------------------------------------------------------- */
#define K_PERSONALIZE L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"
#define K_ACCENT      L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent"

typedef struct { BOOL had; DWORD type, n; BYTE data[64]; } Saved;
static void save(const WCHAR *key, const WCHAR *value, Saved *s)
{
    HKEY k;
    s->had = FALSE;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_READ, &k)) return;
    s->n = sizeof(s->data);
    s->had = !RegQueryValueExW(k, value, 0, &s->type, s->data, &s->n);
    RegCloseKey(k);
}
static void restore(const WCHAR *key, const WCHAR *value, const Saved *s)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, 0, 0, KEY_WRITE, 0, &k, 0)) return;
    if (s->had) RegSetValueExW(k, value, 0, s->type, s->data, s->n);
    else RegDeleteValueW(k, value);
    RegCloseKey(k);
}
static void set_dword(const WCHAR *key, const WCHAR *value, DWORD v)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, 0, 0, KEY_WRITE, 0, &k, 0)) return;
    RegSetValueExW(k, value, 0, REG_DWORD, (BYTE *)&v, sizeof(v));
    RegCloseKey(k);
}
static void del_value(const WCHAR *key, const WCHAR *value)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_WRITE, &k)) return;
    RegDeleteValueW(k, value);
    RegCloseKey(k);
}

static void uisettings(void)
{
    Saved light, transp, palette;
    save(K_PERSONALIZE, L"AppsUseLightTheme", &light);
    save(K_PERSONALIZE, L"EnableTransparency", &transp);
    save(K_ACCENT, L"AccentPalette", &palette);
    del_value(K_PERSONALIZE, L"AppsUseLightTheme");
    del_value(K_PERSONALIZE, L"EnableTransparency");
    del_value(K_ACCENT, L"AccentPalette");

    /* as C++/WinRT does it: the factory, then ActivateInstance */
    HSTRING name = hs(L"Windows.UI.ViewManagement.UISettings");
    void *f = 0, *agile = 0, *insp = 0;
    HRESULT hr = pGetFactory(name, &IID_IActivationFactory_t, &f);
    CHECK("UISettings has an activation factory", hr == S_OK && f);
    if (!f) return;
    CHECK("the factory is agile", qi(f, &IID_IAgileObject_t, &agile) == S_OK && agile);
    if (agile) RELEASE(agile);
    hr = ((HRESULT (STDMETHODCALLTYPE *)(void *, void **))SLOT(f, 6))(f, &insp);
    CHECK("ActivateInstance makes a UISettings", hr == S_OK && insp);
    RELEASE(f);
    if (!insp) return;

    HSTRING cls = 0;
    ((HRESULT (STDMETHODCALLTYPE *)(void *, HSTRING *))SLOT(insp, 4))(insp, &cls);
    CHECK("its runtime class name", cls && !wcscmp(pRawBuffer(cls, 0), L"Windows.UI.ViewManagement.UISettings"));
    pDeleteString(cls);
    ULONG niids = 0;
    GUID *iids = 0;
    ((HRESULT (STDMETHODCALLTYPE *)(void *, ULONG *, GUID **))SLOT(insp, 3))(insp, &niids, &iids);
    CHECK("GetIids lists IUISettings to IUISettings6", niids == 6 && iids && IsEqualGUID(&iids[0], &IID_IUISettings));
    CoTaskMemFree(iids);

    void *ui1 = 0, *ui2 = 0, *ui3 = 0, *ui4 = 0, *ui5 = 0, *ui6 = 0;
    CHECK("IUISettings", qi(insp, &IID_IUISettings, &ui1) == S_OK && ui1);
    CHECK("IUISettings2", qi(insp, &IID_IUISettings2, &ui2) == S_OK && ui2);
    CHECK("IUISettings3", qi(insp, &IID_IUISettings3, &ui3) == S_OK && ui3);
    CHECK("IUISettings4", qi(insp, &IID_IUISettings4, &ui4) == S_OK && ui4);
    CHECK("IUISettings5", qi(insp, &IID_IUISettings5, &ui5) == S_OK && ui5);
    CHECK("IUISettings6", qi(insp, &IID_IUISettings6, &ui6) == S_OK && ui6);
    RELEASE(insp);
    if (!ui1 || !ui2 || !ui3 || !ui4 || !ui5 || !ui6) return;

    /* the light app mode (no AppsUseLightTheme) and the default accent */
    WColor c;
    CHECK("Background is white in the light mode", get_color(ui3, 0, &c) == S_OK && is(c, 0xFF, 0xFF, 0xFF));
    CHECK("Foreground is black in the light mode", get_color(ui3, 1, &c) == S_OK && is(c, 0, 0, 0));
    CHECK("Accent is the default blue", get_color(ui3, 5, &c) == S_OK && is(c, 0x00, 0x78, 0xD7));
    CHECK("AccentDark1", get_color(ui3, 4, &c) == S_OK && is(c, 0x00, 0x5A, 0x9E));
    CHECK("AccentDark3", get_color(ui3, 2, &c) == S_OK && is(c, 0x00, 0x26, 0x42));
    CHECK("AccentLight1", get_color(ui3, 6, &c) == S_OK && is(c, 0x42, 0x9C, 0xE3));
    CHECK("AccentLight3", get_color(ui3, 8, &c) == S_OK && is(c, 0xA6, 0xD8, 0xFF));
    CHECK("an unknown UIColorType is refused", get_color(ui3, 42, &c) == E_INVALIDARG);

    COLORREF win = GetSysColor(COLOR_WINDOW);
    hr = ((HRESULT (STDMETHODCALLTYPE *)(void *, int, WColor *))SLOT(ui1, 18))(ui1, 11 /* Window */, &c);
    CHECK("UIElementColor(Window) is COLOR_WINDOW", hr == S_OK && is(c, GetRValue(win), GetGValue(win), GetBValue(win)));
    hr = ((HRESULT (STDMETHODCALLTYPE *)(void *, int, WColor *))SLOT(ui1, 18))(ui1, 1000 /* AccentColor */, &c);
    CHECK("UIElementColor(AccentColor) is the accent", hr == S_OK && is(c, 0x00, 0x78, 0xD7));
    WSize sz;
    hr = ((HRESULT (STDMETHODCALLTYPE *)(void *, WSize *))SLOT(ui1, 8))(ui1, &sz);
    CHECK("ScrollBarSize is SM_CXVSCROLL", hr == S_OK && sz.Width == (float)GetSystemMetrics(SM_CXVSCROLL));
    UINT32 u = 0;
    hr = ((HRESULT (STDMETHODCALLTYPE *)(void *, UINT32 *))SLOT(ui1, 16))(ui1, &u);
    CHECK("DoubleClickTime", hr == S_OK && u == GetDoubleClickTime());
    hr = ((HRESULT (STDMETHODCALLTYPE *)(void *, UINT32 *))SLOT(ui1, 15))(ui1, &u);
    CHECK("CaretWidth", hr == S_OK && u >= 1);
    double scale = 0;
    hr = ((HRESULT (STDMETHODCALLTYPE *)(void *, double *))SLOT(ui2, 6))(ui2, &scale);
    CHECK("TextScaleFactor is 1", hr == S_OK && scale == 1.0);
    BOOLEAN b = TRUE;
    hr = ((HRESULT (STDMETHODCALLTYPE *)(void *, BOOLEAN *))SLOT(ui4, 6))(ui4, &b);
    CHECK("AdvancedEffectsEnabled is off without transparency", hr == S_OK && !b);
    set_dword(K_PERSONALIZE, L"EnableTransparency", 1);
    hr = ((HRESULT (STDMETHODCALLTYPE *)(void *, BOOLEAN *))SLOT(ui4, 6))(ui4, &b);
    CHECK("AdvancedEffectsEnabled follows EnableTransparency", hr == S_OK && b);
    b = TRUE;
    hr = ((HRESULT (STDMETHODCALLTYPE *)(void *, BOOLEAN *))SLOT(ui5, 6))(ui5, &b);
    CHECK("AutoHideScrollBars is off", hr == S_OK && !b);

    /* ColorValuesChanged: raised when the app mode changes */
    Handler h = { g_handler_vtbl, 1, 0, 0, CreateEventW(0, FALSE, FALSE, 0) };
    EventToken tok = { 0 };
    hr = ((HRESULT (STDMETHODCALLTYPE *)(void *, void *, EventToken *))SLOT(ui3, 7))(ui3, &h, &tok);
    CHECK("add_ColorValuesChanged", hr == S_OK && tok.value && h.refs == 2);
    set_dword(K_PERSONALIZE, L"AppsUseLightTheme", 0);
    CHECK("ColorValuesChanged is raised for the dark mode", WaitForSingleObject(h.ev, 5000) == WAIT_OBJECT_0 && h.calls == 1);
    CHECK("its sender is the UISettings", h.sender == ui1);
    CHECK("Background is black in the dark mode", get_color(ui3, 0, &c) == S_OK && is(c, 0, 0, 0));
    CHECK("Foreground is white in the dark mode", get_color(ui3, 1, &c) == S_OK && is(c, 0xFF, 0xFF, 0xFF));

    static const BYTE green[32] = { 0x9B, 0xF0, 0x9B, 0, 0x6C, 0xD8, 0x6C, 0, 0x3C, 0xC0, 0x3C, 0, 0x10, 0x89, 0x3E, 0,
                                    0x0B, 0x6A, 0x0B, 0, 0x08, 0x4A, 0x08, 0, 0x04, 0x2A, 0x04, 0, 0x88, 0x17, 0x98, 0 };
    HKEY k;
    if (!RegCreateKeyExW(HKEY_CURRENT_USER, K_ACCENT, 0, 0, 0, KEY_WRITE, 0, &k, 0)) {
        RegSetValueExW(k, L"AccentPalette", 0, REG_BINARY, green, sizeof(green));
        RegCloseKey(k);
    }
    CHECK("ColorValuesChanged is raised for a new accent", WaitForSingleObject(h.ev, 5000) == WAIT_OBJECT_0 && h.calls == 2);
    CHECK("Accent comes from AccentPalette", get_color(ui3, 5, &c) == S_OK && is(c, 0x10, 0x89, 0x3E));
    CHECK("AccentDark2 too", get_color(ui3, 3, &c) == S_OK && is(c, 0x08, 0x4A, 0x08));

    hr = ((HRESULT (STDMETHODCALLTYPE *)(void *, EventToken))SLOT(ui3, 8))(ui3, tok);
    CHECK("remove_ColorValuesChanged lets the handler go", hr == S_OK && h.refs == 1);
    set_dword(K_PERSONALIZE, L"AppsUseLightTheme", 1);
    CHECK("no event after removal", WaitForSingleObject(h.ev, 1500) == WAIT_TIMEOUT && h.calls == 2);

    /* a handler left in place goes with the last reference */
    hr = ((HRESULT (STDMETHODCALLTYPE *)(void *, void *, EventToken *))SLOT(ui3, 7))(ui3, &h, &tok);
    RELEASE(ui1); RELEASE(ui2); RELEASE(ui3); RELEASE(ui4); RELEASE(ui5); RELEASE(ui6);
    CHECK("the last Release drops its handlers", hr == S_OK && h.refs == 1);
    CloseHandle(h.ev);

    /* RoActivateInstance */
    insp = 0;
    hr = pActivate(name, &insp);
    CHECK("RoActivateInstance(UISettings)", hr == S_OK && insp && qi(insp, &IID_IUISettings3, &ui3) == S_OK);
    if (insp) { RELEASE(ui3); RELEASE(insp); }
    pDeleteString(name);

    restore(K_PERSONALIZE, L"AppsUseLightTheme", &light);
    restore(K_PERSONALIZE, L"EnableTransparency", &transp);
    restore(K_ACCENT, L"AccentPalette", &palette);
}

static void uiviewsettings(void)
{
    HSTRING name = hs(L"Windows.UI.ViewManagement.UIViewSettings");
    void *interop = 0, *view = 0;
    HRESULT hr = pGetFactory(name, &IID_IUIViewSettingsInterop, &interop);
    CHECK("UIViewSettings has IUIViewSettingsInterop", hr == S_OK && interop);
    pDeleteString(name);
    if (!interop) return;
    HWND w = CreateWindowExW(0, L"STATIC", L"qtthemetest", WS_OVERLAPPEDWINDOW, 0, 0, 200, 100, 0, 0, 0, 0);
    hr = ((HRESULT (STDMETHODCALLTYPE *)(void *, HWND, REFIID, void **))SLOT(interop, 6))(interop, w, &IID_IUIViewSettings, &view);
    CHECK("GetForWindow gives a UIViewSettings", hr == S_OK && view);
    if (view) {
        int mode = -1;
        hr = ((HRESULT (STDMETHODCALLTYPE *)(void *, int *))SLOT(view, 6))(view, &mode);
        CHECK("UserInteractionMode is Mouse (no tablet mode)", hr == S_OK && mode == 0);
        RELEASE(view);
    }
    view = (void *)1;
    hr = ((HRESULT (STDMETHODCALLTYPE *)(void *, HWND, REFIID, void **))SLOT(interop, 6))(interop, (HWND)(ULONG_PTR)0xDEAD0, &IID_IUIViewSettings, &view);
    CHECK("GetForWindow refuses a window that is not there", FAILED(hr) && !view);
    DestroyWindow(w);
    RELEASE(interop);

    void *none = (void *)1;
    name = hs(L"Windows.UI.ViewManagement.NoSuchClass");
    hr = pGetFactory(name, &IID_IActivationFactory_t, &none);
    CHECK("an unknown class is REGDB_E_CLASSNOTREG", hr == REGDB_E_CLASSNOTREG && !none);
    pDeleteString(name);
}

static void ssl_suites(void)
{
    typedef struct { ULONG cFunctions; WCHAR **rgpszFunctions; } CtxFunctions;
    HMODULE bc = LoadLibraryW(L"bcrypt.dll");
    LONG (WINAPI *enum_ctx)(ULONG, LPCWSTR, ULONG, ULONG *, void **) =
        (LONG (WINAPI *)(ULONG, LPCWSTR, ULONG, ULONG *, void **))GetProcAddress(bc, "BCryptEnumContextFunctions");
    void (WINAPI *free_buf)(void *) = (void (WINAPI *)(void *))GetProcAddress(bc, "BCryptFreeBuffer");
    CHECK("bcrypt exports BCryptEnumContextFunctions", enum_ctx && free_buf);
    if (!enum_ctx || !free_buf) return;
    ULONG size = 0;
    CtxFunctions *f = 0;
    LONG s = enum_ctx(1 /* CRYPT_LOCAL */, L"SSL", 0x00010002 /* NCRYPT_SCHANNEL_INTERFACE */, &size, (void **)&f);
    CHECK("Schannel's cipher suites", s == 0 && f && f->cFunctions >= 10 && size > sizeof(*f));
    BOOL gcm = FALSE;
    for (ULONG i = 0; f && i < f->cFunctions; i++) gcm |= !wcscmp(f->rgpszFunctions[i], L"TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256");
    CHECK("TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256 is one", gcm);
    if (f) free_buf(f);
    char small[16];
    void *mine = small;
    size = sizeof(small);
    s = enum_ctx(1, L"SSL", 0x00010002, &size, &mine);
    CHECK("a caller's buffer that is too small", s == (LONG)0xC0000023 && size > sizeof(small));
    f = 0;
    s = enum_ctx(1, L"NoSuchContext", 0x00010002, &size, (void **)&f);
    CHECK("an unknown context has none", s == (LONG)0xC0000225 && !f);
}

int main(void)
{
    HMODULE ole = LoadLibraryW(L"ole32.dll");
    pCreateString = (void *)GetProcAddress(ole, "WindowsCreateString");
    pDeleteString = (void *)GetProcAddress(ole, "WindowsDeleteString");
    pRawBuffer = (void *)GetProcAddress(ole, "WindowsGetStringRawBuffer");
    pGetFactory = (void *)GetProcAddress(ole, "RoGetActivationFactory");
    pActivate = (void *)GetProcAddress(ole, "RoActivateInstance");
    CHECK("ole32 has the WinRT functions", pCreateString && pDeleteString && pRawBuffer && pGetFactory && pActivate);
    if (pCreateString && pDeleteString && pRawBuffer && pGetFactory && pActivate) {
        CoInitializeEx(0, COINIT_APARTMENTTHREADED);
        uisettings();
        uiviewsettings();
        CoUninitialize();
    }
    ssl_suites();
    printf("qtthemetest: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
