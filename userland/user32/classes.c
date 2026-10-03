/*
 * classes.c — the system's window classes
 */
#include "u32.h"

void register_builtin_classes(void)
{
    HCURSOR arrow = LoadCursorW(NULL, MAKEINTRESOURCEW(32512)), ibeam = LoadCursorW(NULL, IDC_IBEAM);
    register_system_class(L"Button", ButtonProc, CS_DBLCLKS | CS_PARENTDC | CS_HREDRAW | CS_VREDRAW, 16, 0, arrow);
    register_system_class(L"Static", StaticProc, CS_DBLCLKS | CS_PARENTDC, 16, 0, arrow);
    register_system_class(L"Edit", EditProc, CS_DBLCLKS | CS_PARENTDC, 16, 0, ibeam);
    register_system_class(L"ListBox", ListBoxProc, CS_DBLCLKS | CS_PARENTDC, 16, 0, arrow);
    register_system_class(L"ComboBox", ComboProc, CS_DBLCLKS | CS_PARENTDC | CS_HREDRAW | CS_VREDRAW, 16, 0, arrow);
    register_system_class(L"ComboLBox", ComboLBoxProc, CS_DBLCLKS | CS_SAVEBITS, 16, 0, arrow);
    register_system_class(L"ScrollBar", ScrollBarProc, CS_DBLCLKS | CS_PARENTDC | CS_HREDRAW | CS_VREDRAW, 16, 0, arrow);
    register_system_class(L"#32770", DefDlgProcW, CS_DBLCLKS | CS_SAVEBITS, DLGWINDOWEXTRA, (HBRUSH)(COLOR_3DFACE + 1), arrow);
    register_system_class(L"#32768", MenuWndProc, CS_DBLCLKS | CS_SAVEBITS | CS_DROPSHADOW, 16, 0, arrow);
    register_system_class(L"Message", DefWindowProcW, 0, 0, 0, arrow);
    register_system_class(L"MDIClient", MDIClientProc, 0, 16, (HBRUSH)(COLOR_APPWORKSPACE + 1), arrow);
}
