/* xamlpagetest.exe — a XAML page in a Window (roadmap step 32.4, its first
 * part; docs/design/windows-ui-xaml.md): an app started as xamlapptest
 * starts it (Application.Start on the desktop path) makes a Window in
 * OnLaunched, as a desktop app does: the framework's DesktopWindowImpl with
 * its Win32 window, the window chrome, the DesktopWindowXamlSource and the
 * XAML island root under it, on the Windows App SDK's lifted content islands
 * (NovaOS's Microsoft.UI.Input.dll: ContentIsland, DesktopChildSiteBridge,
 * the island's input sources, AppWindow).  The window's content is a page
 * loaded with XamlReader.Load (a Grid holding a Border and a Button: text,
 * which needs DirectWrite's Windows 10 font model, comes in the next part);
 * Activate shows the window, and once the framework has laid the page out
 * the test checks the window's bounds and the elements' laid-out sizes,
 * then closes the window and exits.
 *
 *   xamlpagetest       the self-test runs it with no arguments
 */
#include <stdio.h>
#include <string.h>
#include <new>
#include <corep.h>
#include <CStaticLock.h>
#include <SystemThemingInterop.h>
#include <DXamlInstanceStorage.h>
#include <XamlReader.g.h>
#include <WindowsXamlManager.g.h>
#include <XamlSourceFocusNavigationRequest.g.h>
#include <Grid.g.h>
#include <Button.g.h>
#include <Window.g.h>
#include <roapi.h>
#include <FrameworkApplication_Partial.h>
#include <microsoft.ui.dispatching.h>

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d)\n", what, __LINE__); fflush(stdout); } } while (0)
#define CHECK_HR(what, expr) do { HRESULT h_ = (expr); if (SUCCEEDED(h_)) pass++; else { fail++; \
    printf("FAIL: %s: 0x%08lx (line %d)\n", what, static_cast<unsigned long>(h_), __LINE__); fflush(stdout); } } while (0)

#define XMLNS L"xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' "
#define XMLNS_X L"xmlns:x='http://schemas.microsoft.com/winfx/2006/xaml'"

static bool is(HSTRING h, const wchar_t *s)
{
    return wcscmp(h ? WindowsGetStringRawBuffer(h, nullptr) : L"", s) == 0;
}

/* what the app saw, checked after Application.Start returns */
static bool s_callback, s_launched, s_checked, s_exit_called;
static DWORD s_main_thread;
static int s_ticks;

/* the app's Application: its own object composed over the framework's (the
 * framework's is the inner object; this, the outer, answers
 * IApplicationOverrides itself and every other interface through the
 * inner), as a projection makes an app's App class */
class App : public xaml::IApplicationOverrides
{
public:
    IInspectable *m_inner = nullptr;   /* (the framework's, which this owns) */
    LONG m_refs = 1;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IInspectable) || iid == __uuidof(xaml::IApplicationOverrides)) {
            *out = static_cast<xaml::IApplicationOverrides *>(this);
            AddRef();
            return S_OK;
        }
        if (!m_inner) { *out = nullptr; return E_NOINTERFACE; }
        return m_inner->QueryInterface(iid, out);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&m_refs); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG r = InterlockedDecrement(&m_refs);
        if (!r) { if (m_inner) m_inner->Release(); delete this; }
        return (ULONG)r;
    }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG *n, IID **iids) override { *n = 0; *iids = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING *name) override
    {
        return WindowsCreateString(L"XamlPageTest.App", 16, name);
    }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel *t) override { *t = BaseTrust; return S_OK; }

    HRESULT STDMETHODCALLTYPE OnLaunched(xaml::ILaunchActivatedEventArgs *args) override;
};
static App *s_app;

/* the page's elements, kept for the checks the timer makes */
static ctl::ComPtr<xaml::IWindow> s_window;
static ctl::ComPtr<xaml::IFrameworkElement> s_panel, s_button, s_grid, s_label;
static int s_shown;     /* ticks since the checks, while the page stays on screen */
static ctl::ComPtr<msy::IDispatcherQueueTimer> s_timer;

static HWND find_window()
{
    HWND found = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND h, LPARAM p) -> BOOL {
        wchar_t cls[64];
        if (GetClassNameW(h, cls, 64) && !wcscmp(cls, L"WinUIDesktopWin32WindowClass")) {
            *reinterpret_cast<HWND *>(p) = h;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&found));
    return found;
}

/* once the page is laid out (or after ten seconds): the checks, then the
 * window closes and the app exits */
static HRESULT on_tick(msy::IDispatcherQueueTimer *, IInspectable *)
{
    double w = 0, h = 0;
    bool laid_out = s_panel && SUCCEEDED(s_panel->get_ActualWidth(&w)) && w > 0;
    if (s_checked) {
        /* the page stays on screen for three seconds (the self-test's
         * screenshot), then the window closes and the app exits */
        if (++s_shown < 12) return S_OK;
        s_timer->Stop();
        CHECK_HR("Window.Close", s_window->Close());
        CHECK("closes its Win32 window", !find_window());
        ctl::ComPtr<xaml::IApplication> app;
        CHECK("the app", s_app && SUCCEEDED(s_app->QueryInterface(__uuidof(xaml::IApplication), reinterpret_cast<void **>(app.GetAddressOf()))));
        CHECK_HR("Application.Exit", app ? app->Exit() : E_FAIL);
        s_exit_called = true;
        return S_OK;
    }
    if (!laid_out && ++s_ticks < 40) return S_OK;
    s_checked = true;
    printf("xamlpagetest: page laid out after %d ticks\n", s_ticks);
    HWND hwnd = find_window();
    CHECK("the window's Win32 window", hwnd != nullptr);
    CHECK("is visible", hwnd && IsWindowVisible(hwnd));
    RECT rc = {};
    if (hwnd) GetClientRect(hwnd, &rc);
    wf::Rect bounds = {};
    CHECK_HR("Window.Bounds", s_window->get_Bounds(&bounds));
    printf("xamlpagetest: client %ldx%ld, bounds %gx%g\n", rc.right, rc.bottom, bounds.Width, bounds.Height);
    CHECK("are the window's client area", bounds.Width == rc.right && bounds.Height == rc.bottom && bounds.Height > 0);
    double gw = 0, gh = 0;
    CHECK("the Grid fills the window", s_grid && SUCCEEDED(s_grid->get_ActualWidth(&gw)) && SUCCEEDED(s_grid->get_ActualHeight(&gh)) &&
          gw == bounds.Width && gh == bounds.Height);
    CHECK("the Border is laid out", laid_out);
    if (s_panel) s_panel->get_ActualHeight(&h);
    printf("xamlpagetest: grid %gx%g, border %gx%g\n", gw, gh, w, h);
    CHECK("at its size", w == 200 && h == 100);
    double bw = 0, bh = 0;
    CHECK("the Button is laid out at its size", s_button && SUCCEEDED(s_button->get_ActualWidth(&bw)) &&
          SUCCEEDED(s_button->get_ActualHeight(&bh)) && bw == 120 && bh == 40);
    printf("xamlpagetest: button %gx%g\n", bw, bh);
    double tw = 0, th = 0;
    CHECK("the TextBlock is laid out to its text", s_label && SUCCEEDED(s_label->get_ActualWidth(&tw)) &&
          SUCCEEDED(s_label->get_ActualHeight(&th)) && tw > 0 && th > 0);
    printf("xamlpagetest: text %gx%g\n", tw, th);
    printf("xamlpagetest: page on screen\n");
    fflush(stdout);
    return S_OK;
}

/* OnLaunched: as an app's: a window, its content, activated */
HRESULT App::OnLaunched(xaml::ILaunchActivatedEventArgs *)
{
    s_launched = true;
    CHECK("OnLaunched on the thread Application.Start was called on", GetCurrentThreadId() == s_main_thread);

    /* Application.Current is the app's own object */
    ctl::ComPtr<DirectUI::FrameworkApplicationFactory> appFactory;
    ctl::ComObject<DirectUI::FrameworkApplicationFactory>::CreateInstance(appFactory.ReleaseAndGetAddressOf());
    ctl::ComPtr<xaml::IApplicationStatics> appStatics;
    ctl::ComPtr<xaml::IApplication> current;
    CHECK("Application.Current", appFactory && SUCCEEDED(appFactory.As(&appStatics)) &&
          SUCCEEDED(appStatics->get_Current(&current)) && current);

    /* the page: a Grid with a Border and a Button */
    ctl::ComPtr<DirectUI::XamlReaderFactory> reader;
    ctl::ComObject<DirectUI::XamlReaderFactory>::CreateInstance(reader.ReleaseAndGetAddressOf());
    ctl::ComPtr<xaml_markup::IXamlReaderStatics> readerStatics;
    if (reader) reader.As(&readerStatics);
    ctl::ComPtr<IInspectable> page;
    CHECK_HR("XamlReader.Load of a page", readerStatics ? readerStatics->Load(wrl_wrappers::HStringReference(
        L"<Grid " XMLNS XMLNS_X L">"
        L"<Border x:Name='panel' Width='200' Height='100' Margin='24,24,0,0' HorizontalAlignment='Left' VerticalAlignment='Top' Background='SteelBlue'/>"
        L"<TextBlock x:Name='label' Text='Hello from NovaOS' FontSize='20' Margin='240,24,0,0' HorizontalAlignment='Left' VerticalAlignment='Top'/>"
        L"<Button x:Name='go' Content='Click me' Width='120' Height='40' Margin='24,148,0,0' HorizontalAlignment='Left' VerticalAlignment='Top'/>"
        L"</Grid>").Get(), page.ReleaseAndGetAddressOf()) : E_FAIL);
    if (page) page.As(&s_grid);
    ctl::ComPtr<IInspectable> found;
    wrl_wrappers::HStringReference panel(L"panel"), go(L"go"), label(L"label");
    if (s_grid && SUCCEEDED(s_grid->FindName(panel.Get(), &found)) && found) found.As(&s_panel);
    if (s_grid && SUCCEEDED(s_grid->FindName(go.Get(), found.ReleaseAndGetAddressOf())) && found) found.As(&s_button);
    if (s_grid && SUCCEEDED(s_grid->FindName(label.Get(), found.ReleaseAndGetAddressOf())) && found) found.As(&s_label);
    CHECK("the page's Border, TextBlock and Button", s_panel && s_button && s_label);

    /* a Window (made as a projection makes one: the factory, no outer object) */
    ctl::ComPtr<DirectUI::WindowFactory> windowFactory;
    ctl::ComObject<DirectUI::WindowFactory>::CreateInstance(windowFactory.ReleaseAndGetAddressOf());
    ctl::ComPtr<xaml::IWindowFactory> factory;
    if (windowFactory) windowFactory.As(&factory);
    ctl::ComPtr<IInspectable> inner;
    CHECK_HR("a Window", factory ? factory->CreateInstance(nullptr, &inner, s_window.ReleaseAndGetAddressOf()) : E_FAIL);
    if (!s_window) return S_OK;
    CHECK("with its Win32 window", find_window() != nullptr);
    ctl::ComPtr<xaml::IUIElement> content;
    if (s_grid) s_grid.As(&content);
    CHECK_HR("Window.Content", s_window->put_Content(content.Get()));
    CHECK_HR("Window.Activate", s_window->Activate());

    /* then a timer on the thread's DispatcherQueue, which the message loop runs */
    ctl::ComPtr<msy::IDispatcherQueueStatics> queueStatics;
    ctl::ComPtr<msy::IDispatcherQueue> queue;
    CHECK("the thread's DispatcherQueue", SUCCEEDED(wf::GetActivationFactory(
        wrl_wrappers::HStringReference(RuntimeClass_Microsoft_UI_Dispatching_DispatcherQueue).Get(), queueStatics.ReleaseAndGetAddressOf())) &&
        SUCCEEDED(queueStatics->GetForCurrentThread(&queue)) && queue);
    if (!queue) return S_OK;
    CHECK_HR("a timer on it", queue->CreateTimer(&s_timer));
    if (!s_timer) return S_OK;
    s_timer->put_Interval(wf::TimeSpan{ 2500000 });     /* (250 ms) */
    EventRegistrationToken token;
    s_timer->add_Tick(wrl::Callback<wf::ITypedEventHandler<msy::DispatcherQueueTimer *, IInspectable *>>(on_tick).Get(), &token);
    CHECK_HR("started", s_timer->Start());
    return S_OK;
}

/* Application.Start's callback: the app makes its Application */
static HRESULT on_start(xaml::IApplicationInitializationCallbackParams *params)
{
    s_callback = true;
    CHECK("the initialization callback, given its parameters", params != nullptr);
    CHECK("on the thread Application.Start was called on", GetCurrentThreadId() == s_main_thread);
    ctl::ComPtr<DirectUI::FrameworkApplicationFactory> appFactory;
    ctl::ComObject<DirectUI::FrameworkApplicationFactory>::CreateInstance(appFactory.ReleaseAndGetAddressOf());
    ctl::ComPtr<xaml::IApplicationFactory> factory;
    if (appFactory) appFactory.As(&factory);
    s_app = new App();
    ctl::ComPtr<xaml::IApplication> instance;
    CHECK_HR("the app's Application, composed over the framework's", factory ? factory->CreateInstance(
        static_cast<xaml::IApplicationOverrides *>(s_app), &s_app->m_inner,
        instance.ReleaseAndGetAddressOf()) : E_FAIL);
    return S_OK;
}

/* (a framework that never returns from Application.Start fails the test) */
static DWORD WINAPI watchdog(void *)
{
    Sleep(120000);
    printf("FAIL: Application.Start did not return\nxamlpagetest: %d passed, %d failed (%s)\n", pass, fail + 1,
           sizeof(void *) == 8 ? "64-bit" : "32-bit");
    fflush(stdout);
    ExitProcess(1);
}

/* The framework's own classes Application.Start activates by name (the
 * thread's WindowsXamlManager), served by this program as Windows.UI.Xaml.dll
 * serves them to a packaged app: registered for the process
 * (RoRegisterActivationFactories), as an executable serves its classes */
static HRESULT WINAPI make_factory(HSTRING cls, IActivationFactory **out)
{
    *out = nullptr;
    if (is(cls, RuntimeClass_Microsoft_UI_Xaml_Hosting_XamlSourceFocusNavigationRequest)) {
        ctl::ComPtr<DirectUI::XamlSourceFocusNavigationRequestFactory> f;
        IFC_RETURN(ctl::ComObject<DirectUI::XamlSourceFocusNavigationRequestFactory>::CreateInstance(f.ReleaseAndGetAddressOf()));
        return f.CopyTo(out);
    }
    if (!is(cls, RuntimeClass_Microsoft_UI_Xaml_Hosting_WindowsXamlManager)) return REGDB_E_CLASSNOTREG;
    ctl::ComPtr<DirectUI::WindowsXamlManagerFactory> f;
    IFC_RETURN(ctl::ComObject<DirectUI::WindowsXamlManagerFactory>::CreateInstance(f.ReleaseAndGetAddressOf()));
    return f.CopyTo(out);
}

int main()
{
    DirectUI::StaticLockGlobalInit();
    DXamlInstanceStorage::Initialize();     /* (as the framework's DllMain does: its per-thread core's slot, */
    DirectUI::FrameworkApplication::GlobalInit();   /* the application's lock, */
    IPlatformServices *pal = nullptr;                   /* and the platform layer's services) */
    CHECK_HR("ObtainPlatformServices", ObtainPlatformServices(&pal));
    SystemThemingInterop::OverrideSystemTheme(Theming::Theme::Dark);
    s_main_thread = GetCurrentThreadId();
    wrl_wrappers::HStringReference manager(RuntimeClass_Microsoft_UI_Xaml_Hosting_WindowsXamlManager),
        request(RuntimeClass_Microsoft_UI_Xaml_Hosting_XamlSourceFocusNavigationRequest);
    HSTRING classes[] = { manager.Get(), request.Get() };
    PFNGETACTIVATIONFACTORY makers[] = { make_factory, make_factory };
    RO_REGISTRATION_COOKIE cookie = nullptr;
    CHECK_HR("RoRegisterActivationFactories (the framework's classes)", RoRegisterActivationFactories(classes, makers, 2, &cookie));
    CloseHandle(CreateThread(nullptr, 0, watchdog, nullptr, 0, nullptr));

    ctl::ComPtr<DirectUI::FrameworkApplicationFactory> appFactory;
    ctl::ComObject<DirectUI::FrameworkApplicationFactory>::CreateInstance(appFactory.ReleaseAndGetAddressOf());
    ctl::ComPtr<xaml::IApplicationStatics> statics;
    CHECK("Application's statics", appFactory && SUCCEEDED(appFactory.As(&statics)));
    if (!statics) return 1;
    ctl::ComPtr<xaml::IApplication> none;
    CHECK("no Application.Current before Application.Start", FAILED(statics->get_Current(&none)) || !none);

    CHECK_HR("Application.Start", statics->Start(wrl::Callback<xaml::IApplicationInitializationCallback>(on_start).Get()));
    CHECK("the initialization callback ran", s_callback);
    CHECK("OnLaunched ran", s_launched);
    CHECK("the timer ran the checks", s_checked);
    CHECK("Application.Exit returned from Application.Start", s_exit_called);

    RoRevokeActivationFactories(cookie);
    printf("xamlpagetest: %d passed, %d failed (%s)\n", pass, fail, sizeof(void *) == 8 ? "64-bit" : "32-bit");
    fflush(stdout);
    ExitProcess(fail != 0);
}
