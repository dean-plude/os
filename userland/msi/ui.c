/*
 * ui.c — the progress window ("Please wait while Windows Installer ...")
 *
 * The engine runs on the calling thread and pumps messages from its
 * progress callbacks, so the window stays live and Cancel works.
 */
#include "msi.h"
#include "ui.h"
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct MsiUi {
    HWND  wnd, text, status, bar, cancel;
    bool  cancelled;
    char  product[256];
    bool  removing;
};

#define ID_CANCEL 1

static LRESULT CALLBACK ui_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    MsiUi *ui = (MsiUi *)GetWindowLongPtrW(w, GWLP_USERDATA);
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == ID_CANCEL && ui) { ui->cancelled = true; EnableWindow(ui->cancel, FALSE); }
        return 0;
    case WM_CLOSE:
        if (ui) ui->cancelled = true;
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

static void pump(void)
{
    MSG m;
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

MsiUi *msiui_create(void)
{
    MsiUi *ui = calloc(1, sizeof(MsiUi));
    return ui;
}

static void set_text(HWND h, const char *u8)
{
    WCHAR w[512];
    MultiByteToWideChar(CP_UTF8, 0, u8, -1, w, 512);
    w[511] = 0;
    SetWindowTextW(h, w);
}

void msiui_begin(MsiUi *ui, const char *product, bool removing)
{
    if (!ui) return;
    snprintf(ui->product, sizeof(ui->product), "%s", product[0] ? product : "the program");
    ui->removing = removing;
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = ui_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"NovaMsiProgress";
    RegisterClassW(&wc);
    int w = 440, h = 170;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    WCHAR title[300];
    MultiByteToWideChar(CP_UTF8, 0, ui->product, -1, title, 300);
    ui->wnd = CreateWindowExW(0, L"NovaMsiProgress", title, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                              x, y, w, h, NULL, NULL, inst, NULL);
    SetWindowLongPtrW(ui->wnd, GWLP_USERDATA, (LONG_PTR)ui);
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    char line[400];
    snprintf(line, sizeof(line), "Please wait while Windows Installer %s %s. This may take several minutes.",
             removing ? "removes" : "installs", ui->product);
    ui->text = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 16, 14, w - 32, 36, ui->wnd, NULL, inst, NULL);
    set_text(ui->text, line);
    ui->status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 16, 56, w - 32, 18, ui->wnd, NULL, inst, NULL);
    set_text(ui->status, removing ? "Removing files" : "Preparing to install...");
    ui->bar = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 16, 80, w - 32, 18, ui->wnd, NULL, inst, NULL);
    SendMessageW(ui->bar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    ui->cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                 w - 16 - 88, h - 44 - 30, 88, 26, ui->wnd, (HMENU)(UINT_PTR)ID_CANCEL, inst, NULL);
    SendMessageW(ui->text, WM_SETFONT, (WPARAM)font, 0);
    SendMessageW(ui->status, WM_SETFONT, (WPARAM)font, 0);
    SendMessageW(ui->cancel, WM_SETFONT, (WPARAM)font, 0);
    ShowWindow(ui->wnd, SW_SHOW);
    UpdateWindow(ui->wnd);
    pump();
}

void msiui_status(MsiUi *ui, const char *text)
{
    if (!ui || !ui->status) return;
    set_text(ui->status, text);
    pump();
}

void msiui_progress(MsiUi *ui, int done, int total)
{
    if (!ui || !ui->bar) return;
    int pct = total > 0 ? done * 100 / total : 0;
    if (pct > 100) pct = 100;
    SendMessageW(ui->bar, PBM_SETPOS, (WPARAM)pct, 0);
    pump();
}

bool msiui_cancelled(MsiUi *ui)
{
    if (!ui) return false;
    pump();
    return ui->cancelled;
}

void msiui_end(MsiUi *ui, int result, bool message_box, const char *err)
{
    if (!ui) return;
    if (ui->bar && result == MSI_OK) { SendMessageW(ui->bar, PBM_SETPOS, 100, 0); pump(); }
    if (ui->wnd) DestroyWindow(ui->wnd);
    ui->wnd = NULL;
    pump();
    char msg[600];
    WCHAR wm[600], wt[300];
    MultiByteToWideChar(CP_UTF8, 0, ui->product, -1, wt, 300);
    if (result == MSI_OK) {
        if (message_box) {
            snprintf(msg, sizeof(msg), "%s was %s successfully.", ui->product, ui->removing ? "removed" : "installed");
            MultiByteToWideChar(CP_UTF8, 0, msg, -1, wm, 600);
            MessageBoxW(NULL, wm, wt, MB_OK | MB_ICONINFORMATION);
        }
    } else if (result != MSI_ERROR_USEREXIT) {
        snprintf(msg, sizeof(msg), "%s could not be %s.\n\n%s", ui->product, ui->removing ? "removed" : "installed",
                 err && err[0] ? err : "The installation failed.");
        MultiByteToWideChar(CP_UTF8, 0, msg, -1, wm, 600);
        MessageBoxW(NULL, wm, L"Windows Installer", MB_OK | MB_ICONERROR);
    }
    free(ui);
}
