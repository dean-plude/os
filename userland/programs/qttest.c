/*
 * qttest — what Qt programs (KeePassXC) need from NovaOS beyond the usual:
 *
 *   GetGlyphOutline   metrics, coverage bitmaps and outlines (Qt's GDI font
 *                     engine draws every widget's text through it)
 *   HSTRINGs          made by the caller in Windows' layout, as C++/WinRT
 *                     does, and read back by combase's functions
 *   KeyCredentialManager  activates; IsSupportedAsync completes with false
 *   SetSecurityInfo   on GetCurrentProcess() (KeePassXC's crash-dump DACL)
 */
#include <windows.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* (not in NovaOS's headers) */
typedef struct { WORD fract; short value; } FIXED_;
typedef struct { FIXED_ eM11, eM12, eM21, eM22; } MAT2_;
typedef struct { UINT gmBlackBoxX, gmBlackBoxY; POINT gmptGlyphOrigin; short gmCellIncX, gmCellIncY; } GLYPHMETRICS_;
typedef struct { DWORD cb, dwType; FIXED_ start[2]; } TTPOLYGONHEADER_;
typedef struct { WORD wType, cpfx; } TTPOLYCURVE_;
#define GGO_METRICS_ 0
#define GGO_NATIVE_ 2
#define GGO_GRAY8_BITMAP_ 6
#define GGO_GLYPH_INDEX_ 0x80
#define TT_POLYGON_TYPE_ 24
#define TT_PRIM_QSPLINE_ 2
__declspec(dllimport) DWORD WINAPI GetGlyphOutlineW(HDC, UINT, UINT, GLYPHMETRICS_ *, DWORD, void *, const MAT2_ *);
__declspec(dllimport) BOOL WINAPI CreateWellKnownSid(int type, PSID domain, PSID out, DWORD *n);
__declspec(dllimport) DWORD WINAPI SetSecurityInfo(HANDLE, SE_OBJECT_TYPE, SECURITY_INFORMATION, PSID, PSID, PACL, PACL);
#define WinCreatorOwnerRightsSid_ 71
__declspec(dllimport) DWORD WINAPI GetGlyphIndicesW(HDC, LPCWSTR, int, LPWORD, DWORD);

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

typedef struct HSTRING_ *HSTRING;
typedef HRESULT (WINAPI *GetFactory)(HSTRING, REFIID, void **);
typedef const WCHAR *(WINAPI *RawBuffer)(HSTRING, UINT32 *);
typedef HRESULT (WINAPI *Duplicate)(HSTRING, HSTRING *);
typedef HRESULT (WINAPI *Delete)(HSTRING);

static void glyphs(void)
{
    HDC dc = CreateCompatibleDC(0);
    HFONT f = CreateFontW(-24, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Segoe UI");
    SelectObject(dc, f);
    MAT2_ m = { { 0, 1 }, { 0, 0 }, { 0, 0 }, { 0, 1 } };
    GLYPHMETRICS_ gm;
    DWORD r = GetGlyphOutlineW(dc, 'H', GGO_METRICS_, &gm, 0, 0, &m);
    CHECK("GGO_METRICS", r != GDI_ERROR && gm.gmBlackBoxX > 4 && gm.gmBlackBoxY > 10 && gm.gmCellIncX > 8 &&
          gm.gmptGlyphOrigin.y >= (LONG)gm.gmBlackBoxY - 1);
    WORD gi = 0;
    GetGlyphIndicesW(dc, L"H", 1, &gi, 0);
    GLYPHMETRICS_ gi_gm;
    r = GetGlyphOutlineW(dc, gi, GGO_METRICS_ | GGO_GLYPH_INDEX_, &gi_gm, 0, 0, &m);
    CHECK("GGO_GLYPH_INDEX gives the same metrics", r != GDI_ERROR && gi && gi_gm.gmBlackBoxX == gm.gmBlackBoxX &&
          gi_gm.gmCellIncX == gm.gmCellIncX);
    DWORD need = GetGlyphOutlineW(dc, 'H', GGO_GRAY8_BITMAP_, &gm, 0, 0, &m);
    DWORD pitch = (gm.gmBlackBoxX + 3) & ~3u;
    CHECK("GGO_GRAY8_BITMAP size", need == pitch * gm.gmBlackBoxY);
    BYTE *bits = calloc(1, need ? need : 1);
    r = GetGlyphOutlineW(dc, 'H', GGO_GRAY8_BITMAP_, &gm, need, bits, &m);
    int full = 0, over = 0;
    for (DWORD i = 0; i < need; i++) { full += bits[i] == 64; over += bits[i] > 64; }
    CHECK("GGO_GRAY8_BITMAP levels 0..64", r == need && full > 10 && !over);
    free(bits);
    need = GetGlyphOutlineW(dc, 'o', GGO_NATIVE_, &gm, 0, 0, &m);
    BYTE *poly = calloc(1, need ? need : 1);
    r = GetGlyphOutlineW(dc, 'o', GGO_NATIVE_, &gm, need, poly, &m);
    int contours = 0, curves = 0;
    for (DWORD at = 0; r == need && at + sizeof(TTPOLYGONHEADER_) <= need;) {
        TTPOLYGONHEADER_ *h = (TTPOLYGONHEADER_ *)(poly + at);
        if (h->dwType != TT_POLYGON_TYPE_ || h->cb < sizeof(*h) || at + h->cb > need) { contours = -1; break; }
        contours++;
        for (DWORD c = sizeof(*h); c < h->cb;) {
            TTPOLYCURVE_ *pc = (TTPOLYCURVE_ *)(poly + at + c);
            curves += pc->wType == TT_PRIM_QSPLINE_;
            c += 4 + 8 * pc->cpfx;
        }
        at += h->cb;
    }
    CHECK("GGO_NATIVE: 'o' is two contours of splines", need > 0 && contours == 2 && curves > 2);
    free(poly);
    DWORD blank = GetGlyphOutlineW(dc, ' ', GGO_GRAY8_BITMAP_, &gm, 0, 0, &m);
    CHECK("a space has no bitmap but an advance", blank == 0 && gm.gmCellIncX > 0);
    DeleteDC(dc);
    DeleteObject(f);
}

/* C++/WinRT's own HSTRINGs: { flags, length, padding, padding, chars },
 * reference strings with flag 1, heap ones with a count after that */
struct hstring_header { UINT32 flags, length, pad1, pad2; const WCHAR *ptr; };
struct shared_hstring { struct hstring_header h; LONG count; WCHAR buf[64]; };

static void hstrings(HMODULE ole)
{
    RawBuffer raw = (RawBuffer)GetProcAddress(ole, "WindowsGetStringRawBuffer");
    Duplicate dup = (Duplicate)GetProcAddress(ole, "WindowsDuplicateString");
    Delete del = (Delete)GetProcAddress(ole, "WindowsDeleteString");
    static const WCHAR text[] = L"Windows.Foundation.Uri";
    struct hstring_header ref = { 1, (UINT32)wcslen(text), 0, 0, text };
    UINT32 len = 0;
    const WCHAR *got = raw((HSTRING)&ref, &len);
    CHECK("a reference HSTRING made by the caller", got == text && len == wcslen(text));
    struct shared_hstring *s = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*s));
    wcscpy(s->buf, L"heap string");
    s->h.length = (UINT32)wcslen(s->buf);
    s->h.ptr = s->buf;
    s->count = 1;
    HSTRING d = 0;
    CHECK("duplicating one shares it", dup((HSTRING)s, &d) == S_OK && d == (HSTRING)s && s->count == 2);
    del(d);
    CHECK("deleting the copy drops the count", s->count == 1);
    HeapFree(GetProcessHeap(), 0, s);
}

static void key_credentials(HMODULE ole)
{
    GetFactory get = (GetFactory)GetProcAddress(ole, "RoGetActivationFactory");
    static const WCHAR name[] = L"Windows.Security.Credentials.KeyCredentialManager";
    struct hstring_header ref = { 1, (UINT32)wcslen(name), 0, 0, name };
    static const GUID statics = { 0x6AAC468B, 0x0EF1, 0x4CE0, { 0x82, 0x90, 0x41, 0x06, 0xDA, 0x6A, 0x63, 0xB5 } };
    static const GUID async_info = { 0x00000036, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
    IUnknown *f = 0;
    HRESULT hr = get((HSTRING)&ref, &statics, (void **)&f);
    CHECK("KeyCredentialManager activates", hr == S_OK && f);
    if (!f) return;
    /* IKeyCredentialManagerStatics::IsSupportedAsync is the slot after IInspectable's six */
    typedef HRESULT (WINAPI *IsSupported)(IUnknown *, IUnknown **);
    IUnknown *op = 0;
    hr = ((IsSupported)((void **)f->lpVtbl)[6])(f, &op);
    CHECK("IsSupportedAsync", hr == S_OK && op);
    if (!op) return;
    IUnknown *info = 0;
    int status = -1;
    if (SUCCEEDED(op->lpVtbl->QueryInterface(op, &async_info, (void **)&info)) && info) {
        typedef HRESULT (WINAPI *GetStatus)(IUnknown *, int *);
        ((GetStatus)((void **)info->lpVtbl)[7])(info, &status);
        info->lpVtbl->Release(info);
    }
    CHECK("the operation has completed", status == 1);
    typedef HRESULT (WINAPI *GetResults)(IUnknown *, BOOLEAN *);
    BOOLEAN yes = TRUE;
    hr = ((GetResults)((void **)op->lpVtbl)[8])(op, &yes);
    CHECK("Windows Hello is not supported", hr == S_OK && !yes);
    op->lpVtbl->Release(op);
    f->lpVtbl->Release(f);
}

static void process_dacl(void)
{
    BYTE buf[256];
    PACL acl = (PACL)buf;
    InitializeAcl(acl, sizeof(buf), ACL_REVISION);
    BYTE sid[SECURITY_MAX_SID_SIZE];
    DWORD n = sizeof(sid);
    CreateWellKnownSid(WinCreatorOwnerRightsSid_, 0, sid, &n);
    AddAccessAllowedAce(acl, ACL_REVISION, 0x00020000 /* READ_CONTROL */, sid);
    DWORD e = SetSecurityInfo(GetCurrentProcess(), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, 0, 0, acl, 0);
    CHECK("SetSecurityInfo on GetCurrentProcess()", e == ERROR_SUCCESS);
}

int main(void)
{
    HMODULE ole = LoadLibraryW(L"combase.dll");
    if (!ole) ole = LoadLibraryW(L"ole32.dll");
    glyphs();
    hstrings(ole);
    key_credentials(ole);
    process_dacl();
    printf("qttest: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
