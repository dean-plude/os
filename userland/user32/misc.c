/*
 * misc.c — the rest of USER: rectangles, the keyboard, characters,
 * wsprintf, system metrics and parameters, DPI, monitors, the clipboard,
 * window stations and desktops
 */
#include "u32.h"

static int u8_to_w(const char *s, WCHAR *w, int cap) { int n = MultiByteToWideChar(CP_UTF8, 0, s ? s : "", -1, w, cap); if (n <= 0) { w[0] = 0; return 0; } return n - 1; }
static DWORD g_msg_time;

/* -----------------------------------------------------------------------
 * Rectangles
 * ----------------------------------------------------------------------- */
USERAPI BOOL SetRect(LPRECT r, int l, int t, int rr, int b) { if (!r) return FALSE; r->left = l; r->top = t; r->right = rr; r->bottom = b; return TRUE; }
USERAPI BOOL SetRectEmpty(LPRECT r) { return SetRect(r, 0, 0, 0, 0); }
USERAPI BOOL CopyRect(LPRECT d, const RECT *s) { if (!d || !s) return FALSE; *d = *s; return TRUE; }
USERAPI BOOL IsRectEmpty(const RECT *r) { return !r || r->right <= r->left || r->bottom <= r->top; }
USERAPI BOOL PtInRect(const RECT *r, POINT p) { return r && p.x >= r->left && p.x < r->right && p.y >= r->top && p.y < r->bottom; }
USERAPI BOOL OffsetRect(LPRECT r, int dx, int dy) { if (!r) return FALSE; r->left += dx; r->right += dx; r->top += dy; r->bottom += dy; return TRUE; }
USERAPI BOOL InflateRect(LPRECT r, int dx, int dy) { if (!r) return FALSE; r->left -= dx; r->right += dx; r->top -= dy; r->bottom += dy; return TRUE; }
USERAPI BOOL EqualRect(const RECT *a, const RECT *b) { return a && b && a->left == b->left && a->top == b->top && a->right == b->right && a->bottom == b->bottom; }
USERAPI BOOL IntersectRect(LPRECT d, const RECT *a, const RECT *b)
{
    RECT r = { a->left > b->left ? a->left : b->left, a->top > b->top ? a->top : b->top,
               a->right < b->right ? a->right : b->right, a->bottom < b->bottom ? a->bottom : b->bottom };
    if (IsRectEmpty(&r)) { SetRectEmpty(d); return FALSE; }
    *d = r;
    return TRUE;
}
USERAPI BOOL UnionRect(LPRECT d, const RECT *a, const RECT *b)
{
    if (IsRectEmpty(a)) { if (IsRectEmpty(b)) { SetRectEmpty(d); return FALSE; } *d = *b; return TRUE; }
    if (IsRectEmpty(b)) { *d = *a; return TRUE; }
    RECT r = { a->left < b->left ? a->left : b->left, a->top < b->top ? a->top : b->top,
               a->right > b->right ? a->right : b->right, a->bottom > b->bottom ? a->bottom : b->bottom };
    *d = r;
    return TRUE;
}
USERAPI BOOL SubtractRect(LPRECT d, const RECT *a, const RECT *b)
{
    RECT i;
    *d = *a;
    if (!IntersectRect(&i, a, b)) return !IsRectEmpty(d);
    if (i.top == a->top && i.bottom == a->bottom) {
        if (i.left == a->left) d->left = i.right; else if (i.right == a->right) d->right = i.left;
    } else if (i.left == a->left && i.right == a->right) {
        if (i.top == a->top) d->top = i.bottom; else if (i.bottom == a->bottom) d->bottom = i.top;
    }
    return !IsRectEmpty(d);
}


/* -----------------------------------------------------------------------
 * Keyboard
 * ----------------------------------------------------------------------- */
USERAPI SHORT GetKeyState(int vk) { BYTE s = g_keys[vk & 0xFF]; return (SHORT)((s & 0x80 ? 0x8000 : 0) | (s & 1)); }
extern BYTE g_async[256];
USERAPI SHORT GetAsyncKeyState(int vk) { return (SHORT)(g_async[vk & 0xFF] & 0x80 ? 0x8000 : 0); }
USERAPI BOOL GetKeyboardState(PBYTE keys) { memcpy(keys, g_keys, 256); return TRUE; }
USERAPI BOOL SetKeyboardState(PBYTE keys) { memcpy(g_keys, keys, 256); return TRUE; }
USERAPI HANDLE GetKeyboardLayout(DWORD tid) { (void)tid; return (HANDLE)(ULONG_PTR)0x04090409; }
USERAPI int GetKeyboardLayoutList(int n, HANDLE *list) { if (n >= 1 && list) list[0] = (HANDLE)(ULONG_PTR)0x04090409; return 1; }
USERAPI HANDLE LoadKeyboardLayoutW(LPCWSTR id, UINT f) { (void)id; (void)f; return (HANDLE)(ULONG_PTR)0x04090409; }
USERAPI HANDLE ActivateKeyboardLayout(HANDLE h, UINT f) { (void)f; return h; }
USERAPI BOOL GetKeyboardLayoutNameW(LPWSTR name) { const char *s = "00000409"; for (int i = 0; i < 9; i++) name[i] = (WCHAR)s[i]; return TRUE; }
USERAPI int GetKeyboardType(int what) { return what == 0 ? 4 : what == 2 ? 12 : 0; }

/* US layout: VK <-> scan code, and the character a key makes */
static const BYTE g_vk_to_sc[256] = {
    [0x1B] = 0x01, ['1'] = 0x02, ['2'] = 0x03, ['3'] = 0x04, ['4'] = 0x05, ['5'] = 0x06, ['6'] = 0x07, ['7'] = 0x08,
    ['8'] = 0x09, ['9'] = 0x0A, ['0'] = 0x0B, [0xBD] = 0x0C, [0xBB] = 0x0D, [0x08] = 0x0E, [0x09] = 0x0F,
    ['Q'] = 0x10, ['W'] = 0x11, ['E'] = 0x12, ['R'] = 0x13, ['T'] = 0x14, ['Y'] = 0x15, ['U'] = 0x16, ['I'] = 0x17,
    ['O'] = 0x18, ['P'] = 0x19, [0xDB] = 0x1A, [0xDD] = 0x1B, [0x0D] = 0x1C, [0x11] = 0x1D, ['A'] = 0x1E, ['S'] = 0x1F,
    ['D'] = 0x20, ['F'] = 0x21, ['G'] = 0x22, ['H'] = 0x23, ['J'] = 0x24, ['K'] = 0x25, ['L'] = 0x26, [0xBA] = 0x27,
    [0xDE] = 0x28, [0xC0] = 0x29, [0x10] = 0x2A, [0xDC] = 0x2B, ['Z'] = 0x2C, ['X'] = 0x2D, ['C'] = 0x2E, ['V'] = 0x2F,
    ['B'] = 0x30, ['N'] = 0x31, ['M'] = 0x32, [0xBC] = 0x33, [0xBE] = 0x34, [0xBF] = 0x35, [0x6A] = 0x37, [0x12] = 0x38,
    [0x20] = 0x39, [0x14] = 0x3A, [0x70] = 0x3B, [0x71] = 0x3C, [0x72] = 0x3D, [0x73] = 0x3E, [0x74] = 0x3F, [0x75] = 0x40,
    [0x76] = 0x41, [0x77] = 0x42, [0x78] = 0x43, [0x79] = 0x44, [0x90] = 0x45, [0x91] = 0x46, [0x24] = 0x47, [0x26] = 0x48,
    [0x21] = 0x49, [0x6D] = 0x4A, [0x25] = 0x4B, [0x0C] = 0x4C, [0x27] = 0x4D, [0x6B] = 0x4E, [0x23] = 0x4F, [0x28] = 0x50,
    [0x22] = 0x51, [0x2D] = 0x52, [0x2E] = 0x53, [0x7A] = 0x57, [0x7B] = 0x58, [0xA0] = 0x2A, [0xA1] = 0x36, [0xA2] = 0x1D,
    [0xA3] = 0x1D, [0xA4] = 0x38, [0xA5] = 0x38, [0x5B] = 0x5B, [0x5C] = 0x5C, [0x5D] = 0x5D,
};

static WCHAR vk_char(UINT vk, BOOL shift, BOOL caps)
{
    static const char plain[] = "0123456789", shifted[] = ")!@#$%^&*(";
    if (vk >= 'A' && vk <= 'Z') return (WCHAR)((shift ^ caps) ? vk : vk + 32);
    if (vk >= '0' && vk <= '9') return (WCHAR)(shift ? shifted[vk - '0'] : plain[vk - '0']);
    switch (vk) {
    case 0x20: return ' ';
    case 0x0D: return '\r';
    case 0x09: return '\t';
    case 0x08: return '\b';
    case 0x1B: return 0x1B;
    case 0xBA: return shift ? ':' : ';';
    case 0xBB: return shift ? '+' : '=';
    case 0xBC: return shift ? '<' : ',';
    case 0xBD: return shift ? '_' : '-';
    case 0xBE: return shift ? '>' : '.';
    case 0xBF: return shift ? '?' : '/';
    case 0xC0: return shift ? '~' : '`';
    case 0xDB: return shift ? '{' : '[';
    case 0xDC: return shift ? '|' : '\\';
    case 0xDD: return shift ? '}' : ']';
    case 0xDE: return shift ? '"' : '\'';
    case 0x6A: return '*';
    case 0x6B: return '+';
    case 0x6D: return '-';
    case 0x6F: return '/';
    }
    if (vk >= 0x60 && vk <= 0x69) return (WCHAR)('0' + vk - 0x60);
    return 0;
}

USERAPI UINT MapVirtualKeyW(UINT code, UINT type)
{
    switch (type) {
    case 0: return g_vk_to_sc[code & 0xFF];                 /* MAPVK_VK_TO_VSC */
    case 1: case 3:                                         /* MAPVK_VSC_TO_VK(_EX) */
        for (UINT vk = 1; vk < 256; vk++) if (g_vk_to_sc[vk] == (code & 0xFF)) return vk;
        return 0;
    case 2: { WCHAR c = vk_char(code, FALSE, FALSE); return c >= 'a' && c <= 'z' ? (UINT)c - 32 : c; }   /* MAPVK_VK_TO_CHAR */
    }
    return 0;
}
USERAPI UINT MapVirtualKeyA(UINT code, UINT type) { return MapVirtualKeyW(code, type); }
USERAPI UINT MapVirtualKeyExW(UINT code, UINT type, HANDLE hkl) { (void)hkl; return MapVirtualKeyW(code, type); }

USERAPI int ToUnicodeEx(UINT vk, UINT sc, const BYTE *keys, LPWSTR out, int n, UINT flags, HANDLE hkl)
{
    (void)sc; (void)flags; (void)hkl;
    if (n < 1) return 0;
    BOOL shift = keys && (keys[0x10] & 0x80), caps = keys && (keys[0x14] & 1), ctrl = keys && (keys[0x11] & 0x80);
    WCHAR c = vk_char(vk, shift, caps);
    if (ctrl && vk >= 'A' && vk <= 'Z') c = (WCHAR)(vk - 'A' + 1);
    if (!c) return 0;
    out[0] = c;
    if (n > 1) out[1] = 0;
    return 1;
}
USERAPI int ToUnicode(UINT vk, UINT sc, const BYTE *keys, LPWSTR out, int n, UINT flags) { return ToUnicodeEx(vk, sc, keys, out, n, flags, 0); }
USERAPI int ToAscii(UINT vk, UINT sc, const BYTE *keys, LPWORD out, UINT flags)
{
    WCHAR w[2];
    int r = ToUnicodeEx(vk, sc, keys, w, 2, flags, 0);
    if (r) *out = w[0];
    return r;
}
USERAPI SHORT VkKeyScanW(WCHAR c)
{
    for (UINT vk = 1; vk < 256; vk++) {
        if (vk_char(vk, FALSE, FALSE) == c) return (SHORT)vk;
        if (vk_char(vk, TRUE, FALSE) == c) return (SHORT)(vk | 0x100);
    }
    return -1;
}
USERAPI SHORT VkKeyScanA(CHAR c) { return VkKeyScanW((WCHAR)(BYTE)c); }
USERAPI SHORT VkKeyScanExW(WCHAR c, HANDLE hkl) { (void)hkl; return VkKeyScanW(c); }
USERAPI int GetKeyNameTextW(LONG lp, LPWSTR buf, int n)
{
    UINT sc = (UINT)(lp >> 16) & 0xFF;
    UINT vk = MapVirtualKeyW(sc, 1);
    char name[16];
    WCHAR c = vk_char(vk, TRUE, FALSE);
    if (vk == 0x20) memcpy(name, "Space", 6);
    else if (vk == 0x0D) memcpy(name, "Enter", 6);
    else if (vk == 0x1B) memcpy(name, "Esc", 4);
    else if (vk == 0x10) memcpy(name, "Shift", 6);
    else if (vk == 0x11) memcpy(name, "Ctrl", 5);
    else if (vk == 0x12) memcpy(name, "Alt", 4);
    else if (vk >= 0x70 && vk <= 0x7B) { name[0] = 'F'; int f = (int)vk - 0x6F; if (f >= 10) { name[1] = '1'; name[2] = (char)('0' + f - 10); name[3] = 0; } else { name[1] = (char)('0' + f); name[2] = 0; } }
    else if (c > ' ' && c < 0x7F) { name[0] = (char)(c >= 'a' && c <= 'z' ? c - 32 : c); name[1] = 0; }
    else return 0;
    return u8_to_w(name, buf, n);
}

USERAPI UINT SendInput(UINT n, void *inputs, int size) { (void)n; (void)inputs; (void)size; SetLastError(ERROR_ACCESS_DENIED); return 0; }
USERAPI void keybd_event(BYTE vk, BYTE sc, DWORD flags, ULONG_PTR extra) { (void)vk; (void)sc; (void)flags; (void)extra; }
USERAPI void mouse_event(DWORD flags, DWORD dx, DWORD dy, DWORD data, ULONG_PTR extra) { (void)flags; (void)dx; (void)dy; (void)data; (void)extra; }
USERAPI BOOL BlockInput(BOOL block) { (void)block; return FALSE; }
USERAPI BOOL GetLastInputInfo(void *lii) { DWORD *p = lii; p[1] = g_msg_time ? g_msg_time : GetTickCount(); return TRUE; }
USERAPI BOOL RegisterHotKey(HWND h, int id, UINT mods, UINT vk) { (void)h; (void)id; (void)mods; (void)vk; SetLastError(1409 /* ERROR_HOTKEY_ALREADY_REGISTERED */); return FALSE; }
USERAPI BOOL UnregisterHotKey(HWND h, int id) { (void)h; (void)id; return TRUE; }
USERAPI UINT GetDoubleClickTime(void) { return 500; }
USERAPI BOOL SetDoubleClickTime(UINT ms) { (void)ms; return TRUE; }
USERAPI BOOL SwapMouseButton(BOOL swap) { (void)swap; return FALSE; }

/* -----------------------------------------------------------------------
 * Characters (UTF-16 case mapping: ASCII, Latin-1, Greek, Cyrillic)
 * ----------------------------------------------------------------------- */
static WCHAR up(WCHAR c)
{
    if (c >= 'a' && c <= 'z') return c - 32;
    if ((c >= 0xE0 && c <= 0xFE && c != 0xF7) || (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) || (c >= 0x430 && c <= 0x44F)) return c - 32;
    if (c >= 0x450 && c <= 0x45F) return c - 80;
    return c;
}
static WCHAR low(WCHAR c)
{
    if (c >= 'A' && c <= 'Z') return c + 32;
    if ((c >= 0xC0 && c <= 0xDE && c != 0xD7) || (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) || (c >= 0x410 && c <= 0x42F)) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    return c;
}

USERAPI LPWSTR CharUpperW(LPWSTR s)
{
    if ((ULONG_PTR)s < 0x10000) return (LPWSTR)(ULONG_PTR)up((WCHAR)(ULONG_PTR)s);
    for (WCHAR *p = s; *p; p++) *p = up(*p);
    return s;
}
USERAPI LPWSTR CharLowerW(LPWSTR s)
{
    if ((ULONG_PTR)s < 0x10000) return (LPWSTR)(ULONG_PTR)low((WCHAR)(ULONG_PTR)s);
    for (WCHAR *p = s; *p; p++) *p = low(*p);
    return s;
}
USERAPI LPSTR CharUpperA(LPSTR s)
{
    if ((ULONG_PTR)s < 0x10000) { char c = (char)(ULONG_PTR)s; return (LPSTR)(ULONG_PTR)(BYTE)(c >= 'a' && c <= 'z' ? c - 32 : c); }
    for (char *p = s; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
    return s;
}
USERAPI LPSTR CharLowerA(LPSTR s)
{
    if ((ULONG_PTR)s < 0x10000) { char c = (char)(ULONG_PTR)s; return (LPSTR)(ULONG_PTR)(BYTE)(c >= 'A' && c <= 'Z' ? c + 32 : c); }
    for (char *p = s; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
    return s;
}
USERAPI DWORD CharUpperBuffW(LPWSTR s, DWORD n) { for (DWORD i = 0; i < n; i++) s[i] = up(s[i]); return n; }
USERAPI DWORD CharLowerBuffW(LPWSTR s, DWORD n) { for (DWORD i = 0; i < n; i++) s[i] = low(s[i]); return n; }
USERAPI DWORD CharUpperBuffA(LPSTR s, DWORD n) { for (DWORD i = 0; i < n; i++) if (s[i] >= 'a' && s[i] <= 'z') s[i] -= 32; return n; }
USERAPI DWORD CharLowerBuffA(LPSTR s, DWORD n) { for (DWORD i = 0; i < n; i++) if (s[i] >= 'A' && s[i] <= 'Z') s[i] += 32; return n; }
USERAPI LPWSTR CharNextW(LPCWSTR s) { return (LPWSTR)(*s ? s + 1 : s); }
USERAPI LPWSTR CharPrevW(LPCWSTR start, LPCWSTR s) { return (LPWSTR)(s > start ? s - 1 : start); }
USERAPI LPSTR CharNextA(LPCSTR s)
{
    if (!*s) return (LPSTR)s;
    s++;
    while ((*s & 0xC0) == 0x80) s++;                        /* the ANSI code page is UTF-8 */
    return (LPSTR)s;
}
USERAPI LPSTR CharPrevA(LPCSTR start, LPCSTR s)
{
    if (s <= start) return (LPSTR)start;
    s--;
    while (s > start && (*s & 0xC0) == 0x80) s--;
    return (LPSTR)s;
}
/* The Ex forms take a code page; every ANSI page is UTF-8 here */
USERAPI LPSTR CharPrevExA(WORD cp, LPCSTR start, LPCSTR s, DWORD flags) { (void)cp; (void)flags; return CharPrevA(start, s); }
USERAPI LPSTR CharNextExA(WORD cp, LPCSTR s, DWORD flags) { (void)cp; (void)flags; return CharNextA(s); }
USERAPI BOOL IsCharAlphaW(WCHAR c) { return up(c) != low(c) || (c >= 0x4E00 && c <= 0x9FFF); }
USERAPI BOOL IsCharAlphaNumericW(WCHAR c) { return IsCharAlphaW(c) || (c >= '0' && c <= '9'); }
USERAPI BOOL IsCharUpperW(WCHAR c) { return low(c) != c; }
USERAPI BOOL IsCharLowerW(WCHAR c) { return up(c) != c; }
USERAPI BOOL IsCharAlphaA(CHAR c) { return IsCharAlphaW((WCHAR)(BYTE)c); }
USERAPI BOOL IsCharAlphaNumericA(CHAR c) { return IsCharAlphaNumericW((WCHAR)(BYTE)c); }
USERAPI BOOL IsCharUpperA(CHAR c) { return c >= 'A' && c <= 'Z'; }
USERAPI BOOL IsCharLowerA(CHAR c) { return c >= 'a' && c <= 'z'; }
USERAPI BOOL CharToOemA(LPCSTR s, LPSTR d) { if (s != d) while ((*d++ = *s++)) ; return TRUE; }   /* OEM is UTF-8 too */
USERAPI BOOL OemToCharA(LPCSTR s, LPSTR d) { if (s != d) while ((*d++ = *s++)) ; return TRUE; }
USERAPI BOOL CharToOemBuffA(LPCSTR s, LPSTR d, DWORD n) { if (s != d) for (DWORD i = 0; i < n; i++) d[i] = s[i]; return TRUE; }
USERAPI BOOL OemToCharBuffA(LPCSTR s, LPSTR d, DWORD n) { if (s != d) for (DWORD i = 0; i < n; i++) d[i] = s[i]; return TRUE; }
USERAPI BOOL CharToOemW(LPCWSTR s, LPSTR d) { WideCharToMultiByte(CP_UTF8, 0, s, -1, d, 0x7FFFFFFF, 0, 0); return TRUE; }
USERAPI BOOL OemToCharW(LPCSTR s, LPWSTR d) { MultiByteToWideChar(CP_UTF8, 0, s, -1, d, 0x7FFFFFFF); return TRUE; }

/* wsprintf: the C runtime's formatting, capped at 1024 characters as on Windows */
__declspec(dllimport) int _vsnprintf(char *s, size_t n, const char *fmt, va_list ap);
__declspec(dllimport) int _vsnwprintf(wchar_t *s, size_t n, const wchar_t *fmt, va_list ap);
USERAPI int wvsprintfA(LPSTR buf, LPCSTR fmt, va_list ap) { int r = _vsnprintf(buf, 1024, fmt, ap); if (r < 0 || r >= 1024) { buf[1023] = 0; r = 1023; } return r; }
USERAPI int wvsprintfW(LPWSTR buf, LPCWSTR fmt, va_list ap) { int r = _vsnwprintf((wchar_t *)buf, 1024, (const wchar_t *)fmt, ap); if (r < 0 || r >= 1024) { buf[1023] = 0; r = 1023; } return r; }
__declspec(dllexport) int __cdecl wsprintfA(LPSTR buf, LPCSTR fmt, ...) { va_list a; va_start(a, fmt); int r = wvsprintfA(buf, fmt, a); va_end(a); return r; }
__declspec(dllexport) int __cdecl wsprintfW(LPWSTR buf, LPCWSTR fmt, ...) { va_list a; va_start(a, fmt); int r = wvsprintfW(buf, fmt, a); va_end(a); return r; }


USERAPI int GetSystemMetrics(int index)
{
    ULONG w = 0, h = 0;
    NtNovaGuiScreenSize(&w, &h);
    switch (index) {
    case 0: case 16: case 78: case 61: return (int)w;       /* CXSCREEN, CXFULLSCREEN, CXVIRTUALSCREEN, CXMAXIMIZED */
    case 1: case 17: case 79: case 62: return (int)h;
    case 76: case 77: return 0;                             /* XVIRTUALSCREEN, YVIRTUALSCREEN */
    case 2: case 3: case 20: case 21: return 17;            /* scroll bars */
    case 4: return FRAME_TITLE - 1;                         /* CYCAPTION (the desktop's title bar) */
    case 5: case 6: return 1;                               /* CXBORDER, CYBORDER */
    case 7: case 8: case 32: case 33: return 1;             /* frames: one pixel */
    case 9: case 10: return 8;                              /* CYVTHUMB, CXHTHUMB */
    case 11: case 12: case 13: case 14: return 32;          /* icons, cursors */
    case 15: return 20;                                     /* CYMENU */
    case 19: return 1;                                      /* MOUSEPRESENT */
    case 23: return 0;                                      /* SWAPBUTTON */
    case 28: case 29: return 136;                           /* CXMIN, CYMIN */
    case 30: case 31: return 22;                            /* CXSIZE, CYSIZE */
    case 36: case 37: return 4;                             /* double-click rectangle */
    case 43: return 3;                                      /* CMOUSEBUTTONS */
    case 45: case 46: return 2;                             /* CXEDGE, CYEDGE */
    case 49: case 50: return 16;                            /* small icons */
    case 67: return 0;                                      /* CLEANBOOT */
    case 68: case 69: return 4;                             /* CXDRAG, CYDRAG */
    case 75: return 1;                                      /* MOUSEWHEELPRESENT */
    case 80: return 1;                                      /* CMONITORS */
    case 81: return 1;                                      /* SAMEDISPLAYFORMAT */
    case 0x1000: return 0;                                  /* REMOTESESSION */
    case 34: return 136; case 35: return 40;                /* CXMINTRACK, CYMINTRACK */
    case 59: case 60: return (int)(index == 59 ? w : h) + 12;   /* CXMAXTRACK, CYMAXTRACK */
    case 51: return 22;                                     /* CYSMCAPTION */
    case 52: case 53: return 22;                            /* CXSMSIZE, CYSMSIZE */
    case 54: case 55: return 19;                            /* CXMENUSIZE, CYMENUSIZE */
    case 71: case 72: return 15;                            /* CXMENUCHECK, CYMENUCHECK */
    case 83: case 84: return 1;                             /* CXFOCUSBORDER, CYFOCUSBORDER */
    case 92: return 0;                                      /* CXPADDEDBORDER */
    case 47: case 48: return 160;                           /* CXMINSPACING, CYMINSPACING */
    case 57: case 58: return 160;                           /* CXMINIMIZED */
    case 38: case 39: return 75;                            /* CXICONSPACING */
    case 40: return 0;                                      /* MENUDROPALIGNMENT */
    case 42: return 0;                                      /* DBCSENABLED */
    case 44: return 0;                                      /* SECURE */
    case 63: return 1;                                      /* NETWORK */
    case 70: return 0;                                      /* SHOWSOUNDS */
    case 73: return 0;                                      /* SLOWMACHINE */
    case 74: return 0;                                      /* MIDEASTENABLED */
    case 86: return 0;                                      /* TABLETPC */
    case 87: return 0;                                      /* MEDIACENTER */
    case 89: return 0;                                      /* SERVERR2 */
    }
    return 0;
}
USERAPI int GetSystemMetricsForDpi(int index, UINT dpi) { (void)dpi; return GetSystemMetrics(index); }

typedef struct { UINT cbSize; int iBorderWidth, iScrollWidth, iScrollHeight, iCaptionWidth, iCaptionHeight; LOGFONTW lfCaptionFont;
                 int iSmCaptionWidth, iSmCaptionHeight; LOGFONTW lfSmCaptionFont; int iMenuWidth, iMenuHeight;
                 LOGFONTW lfMenuFont, lfStatusFont, lfMessageFont; int iPaddedBorderWidth; } NONCLIENTMETRICSW_;

static void ui_font(LOGFONTW *lf)
{
    memset(lf, 0, sizeof(*lf));
    lf->lfHeight = -12;
    lf->lfWeight = 400;
    u8_to_w("Segoe UI", lf->lfFaceName, 32);
}

USERAPI BOOL SystemParametersInfoW(UINT action, UINT uparam, PVOID p, UINT winini)
{
    (void)uparam; (void)winini;
    switch (action) {
    case 0x0030: {                                          /* SPI_GETWORKAREA */
        RECT *r = p;
        r->left = r->top = 0; r->right = GetSystemMetrics(0); r->bottom = GetSystemMetrics(1);
        return TRUE;
    }
    case 0x0029: {                                          /* SPI_GETNONCLIENTMETRICS */
        NONCLIENTMETRICSW_ *m = p;
        UINT size = m->cbSize;
        memset(m, 0, size < sizeof(*m) ? size : sizeof(*m));
        m->cbSize = size;
        m->iBorderWidth = 1; m->iScrollWidth = m->iScrollHeight = 17; m->iCaptionWidth = m->iCaptionHeight = 22;
        m->iSmCaptionWidth = m->iSmCaptionHeight = 22; m->iMenuWidth = m->iMenuHeight = 19;
        ui_font(&m->lfCaptionFont); ui_font(&m->lfSmCaptionFont); ui_font(&m->lfMenuFont);
        ui_font(&m->lfStatusFont); ui_font(&m->lfMessageFont);
        return TRUE;
    }
    case 0x001F: ui_font(p); return TRUE;                   /* SPI_GETICONTITLELOGFONT */
    case 0x004A: case 0x000A: case 0x0044: case 0x0046: case 0x0048: case 0x001B: case 0x005F:
        *(BOOL *)p = FALSE; return TRUE;                    /* screen reader, beep, key prefs, ... */
    case 0x004A + 0x1000: *(BOOL *)p = TRUE; return TRUE;
    case 0x0068: *(UINT *)p = 3; return TRUE;               /* SPI_GETWHEELSCROLLLINES */
    case 0x006C: *(UINT *)p = 3; return TRUE;               /* SPI_GETWHEELSCROLLCHARS */
    case 0x0016: *(int *)p = 500; return TRUE;              /* SPI_GETKEYBOARDDELAY-ish */
    case 0x004B: *(BOOL *)p = TRUE; return TRUE;            /* SPI_GETFONTSMOOTHING */
    case 0x200C: *(UINT *)p = 2; return TRUE;               /* SPI_GETFONTSMOOTHINGTYPE: ClearType */
    case 0x1042: *(BOOL *)p = TRUE; return TRUE;            /* SPI_GETCLIENTAREAANIMATION */
    case 0x1012: *(BOOL *)p = TRUE; return TRUE;            /* SPI_GETGRADIENTCAPTIONS */
    case 0x0070: *(UINT *)p = 10; return TRUE;              /* SPI_GETMOUSESPEED */
    case 0x0008: *(UINT *)p = 1; return TRUE;               /* SPI_GETBORDER */
    case 0x2014: *(UINT *)p = 0; return TRUE;               /* SPI_GETCARETWIDTH-ish */
    case 0x1024: *(BOOL *)p = TRUE; return TRUE;            /* SPI_GETDROPSHADOW */
    case 0x1002: *(BOOL *)p = TRUE; return TRUE;            /* SPI_GETMENUANIMATION */
    }
    if (p && action >= 0x1000 && action < 0x2000 && !(action & 1)) { *(BOOL *)p = FALSE; return TRUE; }   /* other SPI_GET* booleans */
    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
}

USERAPI BOOL SystemParametersInfoA(UINT action, UINT uparam, PVOID p, UINT winini)
{
    if (action == 0x0029) {                                 /* the A metrics hold LOGFONTA */
        BYTE *m = p;
        UINT size = *(UINT *)m;
        memset(m, 0, size);
        *(UINT *)m = size;
        return TRUE;
    }
    return SystemParametersInfoW(action, uparam, p, winini);
}
USERAPI BOOL SystemParametersInfoForDpi(UINT action, UINT uparam, PVOID p, UINT winini, UINT dpi) { (void)dpi; return SystemParametersInfoW(action, uparam, p, winini); }

/* DPI: everything is at 96 (the desktop scales logical pixels itself) */
USERAPI UINT GetDpiForWindow(HWND h) { (void)h; return 96; }
USERAPI UINT GetDpiForSystem(void) { return 96; }
USERAPI BOOL SetProcessDPIAware(void) { return TRUE; }
USERAPI BOOL IsProcessDPIAware(void) { return TRUE; }
USERAPI BOOL SetProcessDpiAwarenessContext(HANDLE ctx) { (void)ctx; return TRUE; }
USERAPI HANDLE SetThreadDpiAwarenessContext(HANDLE ctx) { (void)ctx; return (HANDLE)(LONG_PTR)-4; }
USERAPI HANDLE GetThreadDpiAwarenessContext(void) { return (HANDLE)(LONG_PTR)-4; }
USERAPI HANDLE GetWindowDpiAwarenessContext(HWND h) { (void)h; return (HANDLE)(LONG_PTR)-4; }
USERAPI int GetAwarenessFromDpiAwarenessContext(HANDLE ctx) { (void)ctx; return 2; }
USERAPI BOOL AreDpiAwarenessContextsEqual(HANDLE a, HANDLE b) { return a == b; }
USERAPI BOOL IsValidDpiAwarenessContext(HANDLE ctx) { return ctx != 0; }
USERAPI BOOL EnableNonClientDpiScaling(HWND h) { (void)h; return TRUE; }

/* Monitors: one */
#define THE_MONITOR ((HANDLE)(ULONG_PTR)0x10001)
USERAPI HANDLE MonitorFromWindow(HWND h, DWORD f) { (void)h; (void)f; return THE_MONITOR; }
USERAPI HANDLE MonitorFromPoint(POINT p, DWORD f) { (void)p; (void)f; return THE_MONITOR; }
USERAPI HANDLE MonitorFromRect(const RECT *r, DWORD f) { (void)r; (void)f; return THE_MONITOR; }
USERAPI BOOL GetMonitorInfoW(HANDLE m, void *mi)
{
    if (m != THE_MONITOR) return FALSE;
    DWORD *p = mi;                                          /* cbSize, rcMonitor, rcWork, dwFlags, [szDevice] */
    RECT *mon = (RECT *)(p + 1), *work = (RECT *)(p + 5);
    SetRect(mon, 0, 0, GetSystemMetrics(0), GetSystemMetrics(1));
    *work = *mon;
    p[9] = 1;                                               /* MONITORINFOF_PRIMARY */
    if (p[0] >= 104) u8_to_w("\\\\.\\DISPLAY1", (WCHAR *)(p + 10), 32);
    return TRUE;
}
USERAPI BOOL GetMonitorInfoA(HANDLE m, void *mi)
{
    DWORD *p = mi;
    DWORD size = p[0];
    p[0] = 40;
    BOOL ok = GetMonitorInfoW(m, mi);
    p[0] = size;
    if (ok && size >= 72) memcpy(p + 10, "\\\\.\\DISPLAY1", 13);
    return ok;
}
typedef BOOL (CALLBACK *MONITORENUMPROC)(HANDLE, HDC, LPRECT, LPARAM);
USERAPI BOOL EnumDisplayMonitors(HDC dc, const RECT *clip, MONITORENUMPROC fn, LPARAM lp)
{
    (void)clip;
    RECT r;
    SetRect(&r, 0, 0, GetSystemMetrics(0), GetSystemMetrics(1));
    fn(THE_MONITOR, dc, &r, lp);
    return TRUE;
}
USERAPI BOOL EnumDisplaySettingsW(LPCWSTR dev, DWORD mode, void *dm)
{
    (void)dev;
    if (mode != (DWORD)-1 && mode != (DWORD)-2 && mode != 0) return FALSE;
    BYTE *b = dm;                                           /* DEVMODEW: dmSize at 68; fields after the name */
    WORD size = *(WORD *)(b + 68), extra = *(WORD *)(b + 70);
    memset(b, 0, (size_t)size + extra);
    *(WORD *)(b + 68) = size;
    u8_to_w("NovaOS Display", (WCHAR *)b, 32);
    *(DWORD *)(b + 72) = 0x00580000;                        /* dmFields: BITSPERPEL | PELSWIDTH | PELSHEIGHT | DISPLAYFREQUENCY */
    *(DWORD *)(b + 168) = 32;                               /* dmBitsPerPel */
    *(DWORD *)(b + 172) = (DWORD)GetSystemMetrics(0);       /* dmPelsWidth */
    *(DWORD *)(b + 176) = (DWORD)GetSystemMetrics(1);       /* dmPelsHeight */
    *(DWORD *)(b + 184) = 60;                               /* dmDisplayFrequency */
    return TRUE;
}
USERAPI BOOL EnumDisplayDevicesW(LPCWSTR dev, DWORD i, void *dd, DWORD flags)
{
    (void)dev; (void)flags;
    if (i) return FALSE;
    BYTE *b = dd;                                           /* cb, DeviceName[32], DeviceString[128], StateFlags, ... */
    DWORD cb = *(DWORD *)b;
    memset(b + 4, 0, cb - 4);
    u8_to_w("\\\\.\\DISPLAY1", (WCHAR *)(b + 4), 32);
    u8_to_w("NovaOS Display Adapter", (WCHAR *)(b + 68), 128);
    *(DWORD *)(b + 324) = 0x5;                              /* ATTACHED_TO_DESKTOP | PRIMARY_DEVICE */
    return TRUE;
}
USERAPI LONG ChangeDisplaySettingsW(void *dm, DWORD f) { (void)dm; (void)f; return -2; /* DISP_CHANGE_BADMODE */ }
USERAPI LONG ChangeDisplaySettingsExW(LPCWSTR d, void *dm, HWND h, DWORD f, void *p) { (void)d; (void)dm; (void)h; (void)f; (void)p; return -2; }

/* -----------------------------------------------------------------------
 * Clipboard: the system's (NtNovaClipboard), shared by every program
 *
 * SetClipboardData hands the kernel a copy of the bytes; GetClipboardData
 * returns a global memory block this process keeps until the clipboard
 * changes.  Formats a program registers go by name.  A bitmap (CF_BITMAP)
 * travels as CF_DIB; a format set with a NULL handle is rendered by its
 * owner (WM_RENDERFORMAT) when the clipboard is closed.
 * ----------------------------------------------------------------------- */
enum { CB_EMPTY, CB_SET, CB_GET, CB_LIST, CB_SEQ, CB_OWNER };
#define CF_BITMAP_ 2
#define CF_DIB_    8
#define WM_RENDERFORMAT_ 0x0305

typedef struct { UINT fmt; HANDLE data; DWORD seq; } ClipItem;
static ClipItem g_clip[32];
static int g_clip_n, g_clip_open;
static HWND g_clip_hwnd;
static UINT g_delayed[16];
static int g_ndelayed;
int registered_name(UINT id, WCHAR *buf, int n);

/* The name the kernel knows a registered format by (UTF-8), or NULL */
static const char *fmt_name(UINT fmt, char *buf)
{
    if (fmt < 0xC000) return NULL;
    WCHAR w[64];
    int k = registered_name(fmt, w, 64);
    if (!k) return NULL;
    int m = WideCharToMultiByte(CP_UTF8, 0, w, k, buf, 59, NULL, NULL);
    buf[m > 0 ? m : 0] = 0;
    return buf;
}

static DWORD clip_seq(void) { return (DWORD)NtNovaClipboard(CB_SEQ, 0, 0, 0, 0); }

/* Forget what this process fetched from an older clipboard */
static void drop_cache(BOOL all)
{
    DWORD seq = clip_seq();
    int o = 0;
    for (int i = 0; i < g_clip_n; i++) {
        if (all || g_clip[i].seq != seq) {
            if (g_clip[i].fmt == CF_BITMAP_) DeleteObject(g_clip[i].data);
            else if (g_clip[i].data) GlobalFree(g_clip[i].data);
        } else g_clip[o++] = g_clip[i];
    }
    g_clip_n = o;
}

static void cache(UINT fmt, HANDLE h)
{
    for (int i = 0; i < g_clip_n; i++) {
        if (g_clip[i].fmt != fmt) continue;
        if (g_clip[i].data != h) { if (fmt == CF_BITMAP_) DeleteObject(g_clip[i].data); else GlobalFree(g_clip[i].data); }
        g_clip[i].data = h;
        g_clip[i].seq = clip_seq();
        return;
    }
    if (g_clip_n == 32) drop_cache(TRUE);
    g_clip[g_clip_n].fmt = fmt;
    g_clip[g_clip_n].data = h;
    g_clip[g_clip_n++].seq = clip_seq();
}

/* A bitmap as a packed DIB (BITMAPINFOHEADER + 32-bit pixels) */
static HGLOBAL bitmap_to_dib(HBITMAP bmp)
{
    BITMAP bm;
    if (!GetObjectW(bmp, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0) return 0;
    SIZE_T px = (SIZE_T)bm.bmWidth * (SIZE_T)bm.bmHeight * 4;
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + px);
    if (!g) return 0;
    BITMAPINFO *bi = GlobalLock(g);
    memset(bi, 0, sizeof(BITMAPINFOHEADER));
    bi->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi->bmiHeader.biWidth = bm.bmWidth;
    bi->bmiHeader.biHeight = bm.bmHeight;
    bi->bmiHeader.biPlanes = 1;
    bi->bmiHeader.biBitCount = 32;
    bi->bmiHeader.biCompression = BI_RGB;
    bi->bmiHeader.biSizeImage = (DWORD)px;
    HDC dc = GetDC(0);
    int ok = GetDIBits(dc, bmp, 0, (UINT)bm.bmHeight, (BYTE *)bi + sizeof(BITMAPINFOHEADER), bi, DIB_RGB_COLORS);
    ReleaseDC(0, dc);
    GlobalUnlock(g);
    if (!ok) { GlobalFree(g); return 0; }
    return g;
}

static HBITMAP dib_to_bitmap(HGLOBAL g)
{
    BITMAPINFO *bi = GlobalLock(g);
    if (!bi) return 0;
    const BITMAPINFOHEADER *h = &bi->bmiHeader;
    DWORD colors = h->biClrUsed ? h->biClrUsed : (h->biBitCount <= 8 ? 1u << h->biBitCount : 0);
    const BYTE *bits = (const BYTE *)bi + h->biSize + colors * 4 + (h->biCompression == BI_BITFIELDS ? 12 : 0);
    HDC dc = GetDC(0);
    HBITMAP b = CreateDIBitmap(dc, h, CBM_INIT, bits, bi, DIB_RGB_COLORS);
    ReleaseDC(0, dc);
    GlobalUnlock(g);
    return b;
}

static BOOL upload(UINT fmt, HANDLE data)
{
    char nb[64];
    if (fmt == CF_BITMAP_) {                              /* goes as a DIB */
        HGLOBAL dib = bitmap_to_dib((HBITMAP)data);
        if (!dib) return FALSE;
        BOOL ok = upload(CF_DIB_, dib);
        GlobalFree(dib);
        return ok;
    }
    SIZE_T n = GlobalSize(data);
    const void *p = GlobalLock(data);
    if (!p && n) return FALSE;
    LONG_PTR r = NtNovaClipboard(CB_SET, fmt, (PVOID)p, n, fmt_name(fmt, nb));
    GlobalUnlock(data);
    return r == 0;
}

USERAPI BOOL OpenClipboard(HWND h)
{
    if (g_clip_open) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    g_clip_open = 1;
    g_clip_hwnd = h;
    return TRUE;
}

USERAPI BOOL CloseClipboard(void)
{
    if (!g_clip_open) { SetLastError(1418 /* ERROR_CLIPBOARD_NOT_OPEN */); return FALSE; }
    /* delayed formats: their owner renders them now (it calls SetClipboardData) */
    int n = g_ndelayed;
    UINT fmts[16];
    memcpy(fmts, g_delayed, sizeof(UINT) * (size_t)n);
    g_ndelayed = 0;
    for (int i = 0; i < n; i++) if (g_clip_hwnd) SendMessageW(g_clip_hwnd, WM_RENDERFORMAT_, fmts[i], 0);
    g_clip_open = 0;
    return TRUE;
}

USERAPI BOOL EmptyClipboard(void)
{
    if (!g_clip_open) { SetLastError(1418); return FALSE; }
    NtNovaClipboard(CB_EMPTY, (ULONG_PTR)g_clip_hwnd, 0, 0, 0);
    drop_cache(TRUE);
    g_ndelayed = 0;
    return TRUE;
}

USERAPI HANDLE SetClipboardData(UINT fmt, HANDLE data)
{
    if (!data) {                                          /* rendered later */
        if (g_ndelayed < 16) g_delayed[g_ndelayed++] = fmt;
        return 0;
    }
    if (!upload(fmt, data)) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    cache(fmt, data);                                     /* the system owns it now */
    return data;
}

USERAPI HANDLE GetClipboardData(UINT fmt)
{
    drop_cache(FALSE);
    for (int i = 0; i < g_clip_n; i++) if (g_clip[i].fmt == fmt) return g_clip[i].data;
    if (fmt == CF_BITMAP_) {
        HANDLE dib = GetClipboardData(CF_DIB_);
        HBITMAP b = dib ? dib_to_bitmap(dib) : 0;
        if (b) cache(fmt, b);
        return b;
    }
    char nb[64];
    const char *name = fmt_name(fmt, nb);
    LONG_PTR size = NtNovaClipboard(CB_GET, fmt, 0, 0, name);
    if (size < 0) { SetLastError(1168 /* ERROR_NOT_FOUND */); return 0; }
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)size ? (SIZE_T)size : 1);
    if (!g) return 0;
    void *p = GlobalLock(g);
    NtNovaClipboard(CB_GET, fmt, p, (ULONG_PTR)size, name);
    GlobalUnlock(g);
    cache(fmt, g);
    return g;
}

/* The formats on the clipboard, as this process numbers them */
static int list_formats(UINT *out, int max)
{
    NOVA_CLIP_ENTRY e[32];
    LONG_PTR n = NtNovaClipboard(CB_LIST, 0, e, 32, 0);
    int k = 0;
    BOOL dib = FALSE;
    for (int i = 0; i < n && k < max; i++) {
        UINT f = e[i].Format;
        if (f >= 0xC000) {
            WCHAR w[64];
            MultiByteToWideChar(CP_UTF8, 0, e[i].Name, -1, w, 64);
            f = RegisterClipboardFormatW(w);
        }
        if (f == CF_DIB_) dib = TRUE;
        out[k++] = f;
    }
    if (dib && k < max) out[k++] = CF_BITMAP_;
    return k;
}

USERAPI BOOL IsClipboardFormatAvailable(UINT fmt)
{
    UINT f[40];
    int n = list_formats(f, 40);
    for (int i = 0; i < n; i++) if (f[i] == fmt) return TRUE;
    return FALSE;
}
USERAPI int CountClipboardFormats(void) { UINT f[40]; return list_formats(f, 40); }
USERAPI UINT EnumClipboardFormats(UINT fmt)
{
    UINT f[40];
    int n = list_formats(f, 40);
    if (!fmt) return n ? f[0] : 0;
    for (int i = 0; i + 1 < n; i++) if (f[i] == fmt) return f[i + 1];
    SetLastError(0);
    return 0;
}
USERAPI int GetPriorityClipboardFormat(UINT *list, int n)
{
    UINT f[40];
    int k = list_formats(f, 40);
    if (!k) return 0;
    for (int i = 0; i < n; i++) for (int j = 0; j < k; j++) if (list[i] == f[j]) return (int)list[i];
    return -1;
}
USERAPI HWND GetClipboardOwner(void) { return (HWND)(ULONG_PTR)NtNovaClipboard(CB_OWNER, 0, 0, 0, 0); }
USERAPI HWND GetOpenClipboardWindow(void) { return g_clip_open ? g_clip_hwnd : 0; }
USERAPI DWORD GetClipboardSequenceNumber(void) { return clip_seq(); }
USERAPI BOOL AddClipboardFormatListener(HWND h) { (void)h; return TRUE; }
USERAPI HWND SetClipboardViewer(HWND h) { (void)h; return 0; }             /* (no viewer chain) */
USERAPI BOOL ChangeClipboardChain(HWND h, HWND next) { (void)h; (void)next; return TRUE; }
USERAPI HWND GetClipboardViewer(void) { return 0; }
USERAPI BOOL RemoveClipboardFormatListener(HWND h) { (void)h; return TRUE; }
USERAPI int GetClipboardFormatNameW(UINT fmt, LPWSTR buf, int n) { return registered_name(fmt, buf, n); }
USERAPI int GetClipboardFormatNameA(UINT fmt, LPSTR buf, int n)
{
    WCHAR w[256];
    int k = registered_name(fmt, w, 256);
    if (!k || n <= 0) return 0;
    int m = WideCharToMultiByte(CP_ACP, 0, w, k, buf, n - 1, NULL, NULL);
    buf[m] = 0;
    return m;
}

/* -----------------------------------------------------------------------
 * Window stations, desktops, session
 * ----------------------------------------------------------------------- */
#define WINSTA ((HANDLE)(ULONG_PTR)0x57530001)
#define DESKTOP ((HANDLE)(ULONG_PTR)0x44540001)
USERAPI HANDLE GetProcessWindowStation(void) { return WINSTA; }
USERAPI HANDLE GetThreadDesktop(DWORD tid) { (void)tid; return DESKTOP; }
USERAPI HANDLE OpenInputDesktop(DWORD f, BOOL inherit, DWORD access) { (void)f; (void)inherit; (void)access; return DESKTOP; }
USERAPI HANDLE OpenDesktopW(LPCWSTR name, DWORD f, BOOL inherit, DWORD access) { (void)name; (void)f; (void)inherit; (void)access; return DESKTOP; }
USERAPI BOOL CloseDesktop(HANDLE h) { (void)h; return TRUE; }
USERAPI BOOL SwitchDesktop(HANDLE h) { (void)h; return TRUE; }
USERAPI BOOL SetThreadDesktop(HANDLE h) { (void)h; return TRUE; }
USERAPI BOOL CloseWindowStation(HANDLE h) { (void)h; return TRUE; }
/* (one window station and desktop: creating another gives the same one) */
USERAPI HANDLE CreateWindowStationW(LPCWSTR name, DWORD f, DWORD access, LPSECURITY_ATTRIBUTES sa) { (void)name; (void)f; (void)access; (void)sa; return WINSTA; }
USERAPI HANDLE OpenWindowStationW(LPCWSTR name, BOOL inherit, DWORD access) { (void)name; (void)inherit; (void)access; return WINSTA; }
USERAPI BOOL SetProcessWindowStation(HANDLE h) { (void)h; return TRUE; }
USERAPI HANDLE CreateDesktopW(LPCWSTR name, LPCWSTR dev, PVOID mode, DWORD f, DWORD access, LPSECURITY_ATTRIBUTES sa)
{ (void)name; (void)dev; (void)mode; (void)f; (void)access; (void)sa; return DESKTOP; }
USERAPI BOOL GetUserObjectInformationW(HANDLE h, int index, PVOID p, DWORD n, LPDWORD need)
{
    if (index == 1) {                                       /* UOI_FLAGS: fInherit, fReserved, dwFlags */
        if (need) *need = 12;
        if (n < 12) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
        DWORD *f = p;
        f[0] = 0; f[1] = 0; f[2] = 1;                       /* WSF_VISIBLE */
        return TRUE;
    }
    if (index == 2) {                                       /* UOI_NAME */
        const char *name = h == WINSTA ? "WinSta0" : "Default";
        WCHAR w[16];
        int k = u8_to_w(name, w, 16);
        DWORD bytes = 2 * ((DWORD)k + 1);
        if (need) *need = bytes;
        if (n < bytes) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
        memcpy(p, w, bytes);
        return TRUE;
    }
    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
}
USERAPI BOOL ExitWindowsEx(UINT flags, DWORD reason) { (void)flags; (void)reason; SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
USERAPI BOOL LockWorkStation(void) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }

USERAPI BOOL ChangeWindowMessageFilterEx(HWND h, UINT msg, DWORD action, void *cf) { (void)h; (void)msg; (void)action; (void)cf; return TRUE; }
USERAPI BOOL ChangeWindowMessageFilter(UINT msg, DWORD f) { (void)msg; (void)f; return TRUE; }
USERAPI BOOL RegisterTouchWindow(HWND h, ULONG f) { (void)h; (void)f; return FALSE; }
USERAPI HANDLE RegisterDeviceNotificationW(HANDLE r, LPVOID filter, DWORD f) { (void)r; (void)filter; (void)f; return (HANDLE)(ULONG_PTR)0xDE01; }
USERAPI BOOL UnregisterDeviceNotification(HANDLE h) { (void)h; return TRUE; }
USERAPI BOOL RegisterRawInputDevices(const void *d, UINT n, UINT cb) { (void)d; (void)n; (void)cb; return TRUE; }
USERAPI HANDLE RegisterPowerSettingNotification(HANDLE r, const GUID *g, DWORD f) { (void)r; (void)g; (void)f; return (HANDLE)(ULONG_PTR)0xDE02; }
USERAPI BOOL UnregisterPowerSettingNotification(HANDLE h) { (void)h; return TRUE; }

/* -----------------------------------------------------------------------
 * The pointer
 * ----------------------------------------------------------------------- */
USERAPI BOOL GetCursorPos(LPPOINT p)
{
    if (!p) return FALSE;
    INT32 c[3];
    if (NtNovaGuiCtl(0, CTL_CURSOR, 0, c)) { p->x = c[0]; p->y = c[1]; g_cursor = *p; }
    else *p = g_cursor;
    return TRUE;
}
USERAPI BOOL GetPhysicalCursorPos(LPPOINT p) { return GetCursorPos(p); }
USERAPI BOOL SetCursorPos(int x, int y) { (void)x; (void)y; return FALSE; }   /* the pointer is the user's */
USERAPI BOOL SetPhysicalCursorPos(int x, int y) { (void)x; (void)y; return FALSE; }
USERAPI BOOL ClipCursor(const RECT *r) { (void)r; return TRUE; }
USERAPI BOOL GetClipCursor(LPRECT r) { if (r) SetRect(r, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)); return TRUE; }
static HCURSOR g_cur;
static int g_cursor_count;
USERAPI HCURSOR SetCursor(HCURSOR c) { HCURSOR o = g_cur; g_cur = c; return o; }
USERAPI HCURSOR GetCursor(void) { return g_cur; }
USERAPI int ShowCursor(BOOL show) { return show ? ++g_cursor_count : --g_cursor_count; }
USERAPI BOOL GetCursorInfo(PCURSORINFO ci)
{
    if (!ci) return FALSE;
    ci->flags = CURSOR_SHOWING;
    ci->hCursor = g_cur;
    GetCursorPos(&ci->ptScreenPos);
    return TRUE;
}
USERAPI BOOL SetSystemCursor(HCURSOR c, DWORD id) { (void)c; (void)id; return TRUE; }
