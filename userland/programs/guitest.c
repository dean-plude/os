/* guitest.exe — a Win32 GUI with the standard pieces: a menu bar and
 * accelerators, child controls, a dialog from a resource template (combo
 * boxes, radio buttons, check boxes, a list box), message boxes */
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <string.h>
#include "guitest.h"

static HINSTANCE g_inst;
static HWND g_log, g_edit;
static INT_PTR CALLBACK page_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);

static void logf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("[guitest] %s\n", buf);
    fflush(stdout);
    if (g_log) {
        WCHAR w[256];
        MultiByteToWideChar(CP_UTF8, 0, buf, -1, w, 256);
        int i = (int)SendMessageW(g_log, LB_ADDSTRING, 0, (LPARAM)w);
        SendMessageW(g_log, LB_SETCURSEL, (WPARAM)i, 0);
    }
}

static INT_PTR CALLBACK options_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG: {
        SetDlgItemTextW(h, IDC_NAME, L"C:\\Documents\\archive.7z");
        static const WCHAR *fmts[] = { L"7z", L"zip", L"tar", L"wim" };
        for (int i = 0; i < 4; i++) SendDlgItemMessageW(h, IDC_FORMAT, CB_ADDSTRING, 0, (LPARAM)fmts[i]);
        SendDlgItemMessageW(h, IDC_FORMAT, CB_SETCURSEL, 0, 0);
        static const WCHAR *lv[] = { L"Store", L"Fastest", L"Fast", L"Normal", L"Maximum", L"Ultra" };
        for (int i = 0; i < 6; i++) SendDlgItemMessageW(h, IDC_LEVEL, CB_ADDSTRING, 0, (LPARAM)lv[i]);
        SendDlgItemMessageW(h, IDC_LEVEL, CB_SETCURSEL, 3, 0);
        CheckRadioButton(h, IDC_MODE1, IDC_MODE3, IDC_MODE1);
        for (int i = 0; i < 12; i++) {
            WCHAR s[64];
            wsprintfW(s, L"file%02d.txt\t%d KB", i + 1, (i + 3) * 17);
            SendDlgItemMessageW(h, IDC_LIST, LB_ADDSTRING, 0, (LPARAM)s);
        }
        logf("options: WM_INITDIALOG");
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK: {
            WCHAR name[260];
            GetDlgItemTextW(h, IDC_NAME, name, 260);
            int fmt = (int)SendDlgItemMessageW(h, IDC_FORMAT, CB_GETCURSEL, 0, 0);
            int enc = IsDlgButtonChecked(h, IDC_ENCRYPT);
            char a[260];
            WideCharToMultiByte(CP_UTF8, 0, name, -1, a, 260, NULL, NULL);
            logf("options: OK name=%s format=%d encrypt=%d", a, fmt, enc);
            EndDialog(h, IDOK);
            return TRUE;
        }
        case IDCANCEL: logf("options: Cancel"); EndDialog(h, IDCANCEL); return TRUE;
        case IDC_FORMAT:
            if (HIWORD(wp) == CBN_SELCHANGE) logf("options: format -> %d", (int)SendDlgItemMessageW(h, IDC_FORMAT, CB_GETCURSEL, 0, 0));
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static LRESULT CALLBACK main_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HWND b = CreateWindowExW(0, L"Button", L"&Options...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 10, 10, 100, 26, h, (HMENU)IDC_BTN, g_inst, NULL);
        g_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"Edit", L"Type here", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 120, 11, 250, 24, h, (HMENU)IDC_EDIT, g_inst, NULL);
        g_log = CreateWindowExW(WS_EX_CLIENTEDGE, L"ListBox", NULL, WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY, 10, 46, 360, 200, h, (HMENU)IDC_LOG, g_inst, NULL);
        SendMessageW(b, WM_SETFONT, (WPARAM)f, 0);
        SendMessageW(g_edit, WM_SETFONT, (WPARAM)f, 0);
        SendMessageW(g_log, WM_SETFONT, (WPARAM)f, 0);
        logf("main: WM_CREATE");
        return 0;
    }
    case WM_SIZE: {
        int w = LOWORD(lp), hh = HIWORD(lp);
        if (g_log) MoveWindow(g_log, 10, 46, w - 20, hh - 56, TRUE);
        if (g_edit) MoveWindow(g_edit, 120, 11, w - 130, 24, TRUE);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_PROPS: {
            PROPSHEETPAGEW pg[2];
            memset(pg, 0, sizeof(pg));
            for (int i = 0; i < 2; i++) { pg[i].dwSize = sizeof(pg[i]); pg[i].hInstance = g_inst; pg[i].pszTemplate = MAKEINTRESOURCEW(i ? IDD_PAGE2 : IDD_PAGE1); pg[i].pfnDlgProc = page_proc; }
            PROPSHEETHEADERW ph;
            memset(&ph, 0, sizeof(ph));
            ph.dwSize = sizeof(ph);
            ph.dwFlags = PSH_PROPSHEETPAGE | PSH_PROPTITLE;
            ph.hwndParent = h;
            ph.hInstance = g_inst;
            ph.pszCaption = L"Guitest";
            ph.nPages = 2;
            ph.ppsp = pg;
            INT_PTR r = PropertySheetW(&ph);
            logf("main: property sheet returned %d", (int)r);
            return 0;
        }
        case IDC_BTN: case IDC_OPTIONS: {
            INT_PTR r = DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_OPTIONS), h, options_proc, 0);
            logf("main: dialog returned %d", (int)r);
            return 0;
        }
        case IDC_MSGBOX: {
            int r = MessageBoxW(h, L"Do you want to save the changes to the archive before closing?", L"Guitest", MB_YESNOCANCEL | MB_ICONQUESTION);
            logf("main: MessageBox returned %d", r);
            return 0;
        }
        case IDC_ABOUT: MessageBoxW(h, L"A test of NovaOS's user32: menus, dialogs and controls.", L"About Guitest", MB_OK | MB_ICONINFORMATION); return 0;
        case IDC_TOOLBAR: {
            HMENU m = GetMenu(h);
            UINT st = GetMenuState(m, IDC_TOOLBAR, MF_BYCOMMAND);
            CheckMenuItem(m, IDC_TOOLBAR, (st & MF_CHECKED) ? MF_UNCHECKED : MF_CHECKED);
            logf("main: toolbar %s", (st & MF_CHECKED) ? "off" : "on");
            return 0;
        }
        case IDC_SORTNAME: case IDC_SORTSIZE:
            CheckMenuRadioItem(GetMenu(h), IDC_SORTNAME, IDC_SORTSIZE, LOWORD(wp), MF_BYCOMMAND);
            logf("main: sort by %s", LOWORD(wp) == IDC_SORTNAME ? "name" : "size");
            return 0;
        case IDC_EXIT: DestroyWindow(h); return 0;
        case IDC_EDIT:
            if (HIWORD(wp) == EN_CHANGE) { char a[128]; GetWindowTextA(g_edit, a, 128); logf("edit: %s", a); }
            return 0;
        }
        break;
    case WM_CLOSE: logf("main: WM_CLOSE"); DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* the property sheet's pages: a form and a tree of folders */
static INT_PTR CALLBACK page_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        HWND tv = GetDlgItem(h, IDC_TREE);
        if (tv) {
            static const WCHAR *const roots[] = { L"Documents", L"Downloads", L"Pictures" };
            static const WCHAR *const subs[] = { L"Reports", L"Letters", L"Archive" };
            for (int i = 0; i < 3; i++) {
                TVINSERTSTRUCTW ins;
                memset(&ins, 0, sizeof(ins));
                ins.hParent = TVI_ROOT; ins.hInsertAfter = TVI_LAST;
                ins.item.mask = TVIF_TEXT | TVIF_PARAM; ins.item.pszText = (LPWSTR)roots[i]; ins.item.lParam = i;
                HTREEITEM r = TreeView_InsertItem(tv, &ins);
                for (int k = 0; k < 3; k++) {
                    ins.hParent = r;
                    ins.item.pszText = (LPWSTR)subs[k]; ins.item.lParam = 10 * (i + 1) + k;
                    HTREEITEM c = TreeView_InsertItem(tv, &ins);
                    if (k == 0) { ins.hParent = c; ins.item.pszText = L"2026"; TreeView_InsertItem(tv, &ins); }
                }
                if (i == 0) TreeView_Expand(tv, r, TVE_EXPAND);
            }
        } else SetDlgItemTextW(h, IDC_PAGETEXT, L"initial text");
        return TRUE;
    }
    case WM_COMMAND:
        if (HIWORD(wp) == EN_CHANGE || HIWORD(wp) == BN_CLICKED) { PropSheet_Changed(GetParent(h), h); logf("page: control %d changed", LOWORD(wp)); }
        return 0;
    case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)lp;
        if (nm->code == PSN_APPLY) { logf("page %s: PSN_APPLY", GetDlgItem(h, IDC_TREE) ? "folders" : "general"); SetWindowLongPtrW(h, DWLP_MSGRESULT, PSNRET_NOERROR); return TRUE; }
        if (nm->code == PSN_SETACTIVE) { logf("page %s: PSN_SETACTIVE", GetDlgItem(h, IDC_TREE) ? "folders" : "general"); return TRUE; }
        if (nm->code == TVN_SELCHANGEDW) {
            NMTREEVIEWW *tv = (NMTREEVIEWW *)lp;
            WCHAR t[64] = { 0 };
            TVITEMW it = { TVIF_TEXT, tv->itemNew.hItem, 0, 0, t, 64, 0, 0, 0, 0 };
            TreeView_GetItem(nm->hwndFrom, &it);
            logf("tree: selected %ls (%d)", t, (int)tv->itemNew.lParam);
        }
        if (nm->code == TVN_ITEMEXPANDEDW) logf("tree: %s", ((NMTREEVIEWW *)lp)->action == TVE_EXPAND ? "expanded" : "collapsed");
        return 0;
    }
    }
    return FALSE;
}

int main(int argc, char **argv)
{
    g_inst = GetModuleHandleW(NULL);
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = main_proc;
    wc.hInstance = g_inst;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    wc.lpszClassName = L"GuiTest";
    wc.lpszMenuName = MAKEINTRESOURCEW(IDM_MAIN);
    if (!RegisterClassExW(&wc)) { printf("RegisterClassEx failed\n"); return 1; }
    HWND h = CreateWindowExW(0, L"GuiTest", L"User32 Test", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 520, 380, NULL, NULL, g_inst, NULL);
    if (!h) { printf("CreateWindowEx failed: %lu\n", GetLastError()); return 1; }
    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);
    HACCEL acc = LoadAcceleratorsW(g_inst, MAKEINTRESOURCEW(IDA_MAIN));
    if (argc > 1 && !strcmp(argv[1], "dialog")) PostMessageW(h, WM_COMMAND, IDC_OPTIONS, 0);
    if (argc > 1 && !strcmp(argv[1], "msgbox")) PostMessageW(h, WM_COMMAND, IDC_MSGBOX, 0);
    if (argc > 1 && !strcmp(argv[1], "props")) PostMessageW(h, WM_COMMAND, IDC_PROPS, 0);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (TranslateAcceleratorW(h, acc, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    logf("main: exit");
    return 0;
}
