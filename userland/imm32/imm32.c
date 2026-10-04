/*
 * imm32.dll — input method editors.  NovaOS has no IMEs: keyboard input
 * arrives as characters, so windows have no input context and the
 * composition calls have nothing to work on.
 */
#include <windows.h>

#define IMMAPI __declspec(dllexport)
typedef HANDLE HIMC;
typedef HANDLE HKL_;
#define VK_PROCESSKEY_ 0xE5

IMMAPI HIMC WINAPI ImmGetContext(HWND w) { (void)w; return 0; }
IMMAPI BOOL WINAPI ImmReleaseContext(HWND w, HIMC c) { (void)w; (void)c; return TRUE; }
IMMAPI HIMC WINAPI ImmCreateContext(void) { return 0; }
IMMAPI BOOL WINAPI ImmDestroyContext(HIMC c) { (void)c; return FALSE; }
IMMAPI HIMC WINAPI ImmAssociateContext(HWND w, HIMC c) { (void)w; (void)c; return 0; }
IMMAPI BOOL WINAPI ImmAssociateContextEx(HWND w, HIMC c, DWORD flags) { (void)w; (void)c; (void)flags; return TRUE; }
IMMAPI HWND WINAPI ImmGetDefaultIMEWnd(HWND w) { (void)w; return 0; }
IMMAPI BOOL WINAPI ImmIsIME(HKL_ kl) { (void)kl; return FALSE; }
IMMAPI BOOL WINAPI ImmDisableIME(DWORD tid) { (void)tid; return TRUE; }
IMMAPI BOOL WINAPI ImmDisableTextFrameService(DWORD tid) { (void)tid; return TRUE; }
IMMAPI UINT WINAPI ImmGetIMEFileNameA(HKL_ kl, LPSTR buf, UINT n) { (void)kl; if (buf && n) buf[0] = 0; return 0; }
IMMAPI BOOL WINAPI ImmGetOpenStatus(HIMC c) { (void)c; return FALSE; }
IMMAPI BOOL WINAPI ImmSetOpenStatus(HIMC c, BOOL open) { (void)c; (void)open; return FALSE; }
IMMAPI BOOL WINAPI ImmGetConversionStatus(HIMC c, LPDWORD conv, LPDWORD sent)
{ (void)c; if (conv) *conv = 0; if (sent) *sent = 0; return FALSE; }
IMMAPI BOOL WINAPI ImmSetConversionStatus(HIMC c, DWORD conv, DWORD sent) { (void)c; (void)conv; (void)sent; return FALSE; }
IMMAPI LONG WINAPI ImmGetCompositionStringW(HIMC c, DWORD index, LPVOID buf, DWORD n)
{ (void)c; (void)index; (void)buf; (void)n; return 0; }
IMMAPI LONG WINAPI ImmGetCompositionStringA(HIMC c, DWORD index, LPVOID buf, DWORD n)
{ (void)c; (void)index; (void)buf; (void)n; return 0; }
IMMAPI BOOL WINAPI ImmSetCompositionStringW(HIMC c, DWORD index, LPVOID comp, DWORD nc, LPVOID read, DWORD nr)
{ (void)c; (void)index; (void)comp; (void)nc; (void)read; (void)nr; return FALSE; }
IMMAPI BOOL WINAPI ImmSetCompositionStringA(HIMC c, DWORD index, LPVOID comp, DWORD nc, LPVOID read, DWORD nr)
{ (void)c; (void)index; (void)comp; (void)nc; (void)read; (void)nr; return FALSE; }
IMMAPI BOOL WINAPI ImmGetCompositionWindow(HIMC c, LPVOID form) { (void)c; (void)form; return FALSE; }
IMMAPI BOOL WINAPI ImmSetCompositionWindow(HIMC c, LPVOID form) { (void)c; (void)form; return FALSE; }
IMMAPI BOOL WINAPI ImmGetCandidateWindow(HIMC c, DWORD i, LPVOID form) { (void)c; (void)i; (void)form; return FALSE; }
IMMAPI BOOL WINAPI ImmSetCandidateWindow(HIMC c, LPVOID form) { (void)c; (void)form; return FALSE; }
IMMAPI BOOL WINAPI ImmGetCompositionFontW(HIMC c, LPLOGFONTW lf) { (void)c; (void)lf; return FALSE; }
IMMAPI BOOL WINAPI ImmSetCompositionFontW(HIMC c, LPLOGFONTW lf) { (void)c; (void)lf; return FALSE; }
IMMAPI BOOL WINAPI ImmSetCompositionFontA(HIMC c, LPVOID lf) { (void)c; (void)lf; return FALSE; }
IMMAPI BOOL WINAPI ImmNotifyIME(HIMC c, DWORD action, DWORD index, DWORD value)
{ (void)c; (void)action; (void)index; (void)value; return FALSE; }
IMMAPI LRESULT WINAPI ImmEscapeW(HKL_ kl, HIMC c, UINT esc, LPVOID data) { (void)kl; (void)c; (void)esc; (void)data; return 0; }
IMMAPI LRESULT WINAPI ImmEscapeA(HKL_ kl, HIMC c, UINT esc, LPVOID data) { (void)kl; (void)c; (void)esc; (void)data; return 0; }
IMMAPI DWORD WINAPI ImmGetProperty(HKL_ kl, DWORD index) { (void)kl; (void)index; return 0; }
IMMAPI UINT WINAPI ImmGetVirtualKey(HWND w) { (void)w; return VK_PROCESSKEY_; }
IMMAPI DWORD WINAPI ImmGetCandidateListW(HIMC c, DWORD i, LPVOID list, DWORD n) { (void)c; (void)i; (void)list; (void)n; return 0; }
IMMAPI DWORD WINAPI ImmGetCandidateListCountW(HIMC c, LPDWORD count) { (void)c; if (count) *count = 0; return 0; }
IMMAPI UINT WINAPI ImmGetDescriptionW(HKL_ kl, LPWSTR s, UINT n) { (void)kl; if (s && n) s[0] = 0; return 0; }
IMMAPI UINT WINAPI ImmGetIMEFileNameW(HKL_ kl, LPWSTR s, UINT n) { (void)kl; if (s && n) s[0] = 0; return 0; }
IMMAPI BOOL WINAPI ImmSimulateHotKey(HWND w, DWORD id) { (void)w; (void)id; return FALSE; }
IMMAPI DWORD WINAPI ImmGetIMCLockCount(HIMC c) { (void)c; return 0; }
IMMAPI LPVOID WINAPI ImmLockIMC(HIMC c) { (void)c; return 0; }
IMMAPI BOOL WINAPI ImmUnlockIMC(HIMC c) { (void)c; return FALSE; }
IMMAPI BOOL WINAPI ImmRequestMessageW(HIMC c, WPARAM wp, LPARAM lp) { (void)c; (void)wp; (void)lp; return FALSE; }
