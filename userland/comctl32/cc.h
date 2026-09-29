/*
 * cc.h — what comctl32's sources share (not exported)
 */
#pragma once
#define NOVA_BUILD_COMCTL32
#include <windows.h>
#include <commctrl.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>

#define CC __declspec(dllexport)
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define ABS(x) ((x) < 0 ? -(x) : (x))

/* A control's state lives in its first extra bytes */
static inline void *ctl_get(HWND h) { return (void *)GetWindowLongPtrW(h, 0); }
static inline void ctl_set(HWND h, void *p) { SetWindowLongPtrW(h, 0, (LONG_PTR)p); }

static inline int wlen(const WCHAR *s) { int n = 0; if (s && (ULONG_PTR)s > 0xFFFF && s != LPSTR_TEXTCALLBACKW) while (s[n]) n++; return n; }
static inline WCHAR *wdup(const WCHAR *s)
{
    if (!s || s == LPSTR_TEXTCALLBACKW) return (WCHAR *)s;
    int n = wlen(s);
    WCHAR *d = malloc(2 * ((size_t)n + 1));
    if (d) { memcpy(d, s, 2 * (size_t)n); d[n] = 0; }
    return d;
}
static inline void wfree(WCHAR *s) { if (s && s != LPSTR_TEXTCALLBACKW) free(s); }

/* WM_NOTIFY to the control's parent */
static inline LRESULT cc_notify(HWND h, UINT code, NMHDR *nm)
{
    NMHDR tmp;
    if (!nm) nm = &tmp;
    nm->hwndFrom = h;
    nm->idFrom = (UINT_PTR)GetDlgCtrlID(h);
    nm->code = code;
    HWND p = GetParent(h);
    return p ? SendMessageW(p, WM_NOTIFY, nm->idFrom, (LPARAM)nm) : 0;
}

HFONT cc_font(void);                                /* the UI font */
void  cc_fill(HDC dc, const RECT *r, COLORREF c);
void  cc_frame(HDC dc, const RECT *r, COLORREF c);
void  cc_arrow(HDC dc, const RECT *r, int dir, COLORREF c);   /* 0 up 1 down 2 left 3 right */
int   cc_text_w(HDC dc, const WCHAR *s, int n);
int   cc_font_h(HFONT f);
ATOM  cc_register(LPCWSTR name, WNDPROC proc, UINT style, HBRUSH brush);

LRESULT CALLBACK ProgressProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK StatusProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK TooltipProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK UpDownProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK TrackbarProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK TabProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK HeaderProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK ListViewProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK ToolbarProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK RebarProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK ComboExProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK TreeViewProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK LinkProc(HWND, UINT, WPARAM, LPARAM);

/* image lists (imagelist.c) */
int   il_draw(HIMAGELIST h, int i, HDC dc, int x, int y, UINT style);
