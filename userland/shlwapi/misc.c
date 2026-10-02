/*
 * misc.c — the rest of shlwapi.dll: URLs, IsOS, hashing, colors, thread
 * helpers, QISearch and file associations (none are registered).
 */

#define NOVA_BUILD_SHLWAPI
#include <windows.h>

#define S_OK_         ((HRESULT)0)
#define E_POINTER_    ((HRESULT)0x80004003L)
#define E_INVALIDARG_ ((HRESULT)0x80070057L)
#define E_NOINTERFACE_ ((HRESULT)0x80004002L)
#define HR_WIN32(e)   ((HRESULT)(0x80070000L | (e)))

static int wlen(const WCHAR *s) { int n = 0; while (s[n]) n++; return n; }
static int hexval(WCHAR c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1; }

/* -----------------------------------------------------------------------
 * URLs
 * ----------------------------------------------------------------------- */
static BOOL url_scheme(LPCWSTR u, int *len)
{
    int i = 0;
    while ((u[i] >= 'a' && u[i] <= 'z') || (u[i] >= 'A' && u[i] <= 'Z') || (u[i] >= '0' && u[i] <= '9') || u[i] == '+' || u[i] == '-' || u[i] == '.') i++;
    *len = i;
    return i > 1 && u[i] == ':';
}

/* URLIS_URL 0, URLIS_OPAQUE 1, URLIS_NOHISTORY 2, URLIS_FILEURL 3, URLIS_APPLIABLE 4, URLIS_DIRECTORY 5, URLIS_HASQUERY 6 */
LWSTDAPI_(BOOL) UrlIsW(LPCWSTR u, int what)
{
    int n;
    BOOL url = url_scheme(u, &n);
    switch (what) {
    case 0: return url;
    case 1: return url && u[n + 1] != '/';
    case 3: return url && n == 4 && (u[0] | 32) == 'f' && (u[1] | 32) == 'i' && (u[2] | 32) == 'l' && (u[3] | 32) == 'e';
    case 5: { int l = wlen(u); return l && (u[l - 1] == '/' || u[l - 1] == '\\'); }
    case 6: for (; *u; u++) if (*u == '?') return TRUE; return FALSE;
    }
    return FALSE;
}

LWSTDAPI_(BOOL) UrlIsA(LPCSTR u, int what)
{
    WCHAR w[2048];
    MultiByteToWideChar(CP_UTF8, 0, u, -1, w, 2048);
    return UrlIsW(w, what);
}

/* file:///C:/dir/a%20b.txt -> C:\dir\a b.txt */
LWSTDAPI_(HRESULT) PathCreateFromUrlW(LPCWSTR url, LPWSTR out, LPDWORD n, DWORD flags)
{
    (void)flags;
    if (!url || !out || !n) return E_INVALIDARG_;
    if (!UrlIsW(url, 3)) return E_INVALIDARG_;
    const WCHAR *p = url + 5;
    while (*p == '/') p++;
    if (p[0] && p[1] == '|') {}                            /* file:///C|/x */
    WCHAR tmp[2048];
    int o = 0;
    for (; *p && o < 2047; p++) {
        if (*p == '%' && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) {
            tmp[o++] = (WCHAR)(hexval(p[1]) * 16 + hexval(p[2]));
            p += 2;
        } else tmp[o++] = *p == '/' ? '\\' : *p == '|' ? ':' : *p;
    }
    tmp[o] = 0;
    if ((DWORD)o >= *n) { *n = (DWORD)o + 1; return E_POINTER_; }
    for (int i = 0; i <= o; i++) out[i] = tmp[i];
    *n = (DWORD)o;
    return S_OK_;
}

LWSTDAPI_(HRESULT) UrlCreateFromPathW(LPCWSTR path, LPWSTR url, LPDWORD n, DWORD reserved)
{
    (void)reserved;
    if (UrlIsW(path, 0)) {                                  /* already a URL */
        int l = wlen(path);
        if ((DWORD)l >= *n) { *n = (DWORD)l + 1; return E_POINTER_; }
        for (int i = 0; i <= l; i++) url[i] = path[i];
        *n = (DWORD)l;
        return 1;                                           /* S_FALSE */
    }
    static const char hex[] = "0123456789ABCDEF";
    WCHAR tmp[4096];
    int o = 0;
    const char *pre = "file:///";
    while (*pre) tmp[o++] = (WCHAR)*pre++;
    for (const WCHAR *p = path; *p && o < 4090; p++) {
        WCHAR c = *p;
        if (c == '\\') tmp[o++] = '/';
        else if (c == ' ' || c == '%' || c == '#' || c == '?' || c < 32) {
            tmp[o++] = '%'; tmp[o++] = (WCHAR)hex[(c >> 4) & 15]; tmp[o++] = (WCHAR)hex[c & 15];
        } else tmp[o++] = c;
    }
    tmp[o] = 0;
    if ((DWORD)o >= *n) { *n = (DWORD)o + 1; return E_POINTER_; }
    for (int i = 0; i <= o; i++) url[i] = tmp[i];
    *n = (DWORD)o;
    return S_OK_;
}

#define URL_ESCAPE_SPACES_ONLY 0x04000000
#define URL_ESCAPE_PERCENT     0x00001000
#define URL_ESCAPE_SEGMENT_ONLY 0x00002000
#define URL_UNESCAPE_INPLACE   0x00100000

LWSTDAPI_(HRESULT) UrlEscapeW(LPCWSTR in, LPWSTR out, LPDWORD n, DWORD flags)
{
    static const char hex[] = "0123456789ABCDEF";
    WCHAR tmp[4096];
    int o = 0;
    BOOL in_query = FALSE;
    for (const WCHAR *p = in; *p && o < 4090; p++) {
        WCHAR c = *p;
        if (c == '?' || c == '#') in_query = TRUE;
        BOOL esc;
        if (flags & URL_ESCAPE_SPACES_ONLY) esc = c == ' ';
        else if (in_query && !(flags & URL_ESCAPE_SEGMENT_ONLY)) esc = FALSE;
        else esc = c <= 32 || c >= 127 || c == '"' || c == '<' || c == '>' || c == '{' || c == '}' || c == '|' ||
                   c == '\\' || c == '^' || c == '`' || c == '[' || c == ']' || (c == '%' && (flags & URL_ESCAPE_PERCENT)) ||
                   ((flags & URL_ESCAPE_SEGMENT_ONLY) && (c == '/' || c == '?' || c == '#'));
        if (esc && c < 256) { tmp[o++] = '%'; tmp[o++] = (WCHAR)hex[c >> 4]; tmp[o++] = (WCHAR)hex[c & 15]; }
        else tmp[o++] = c;
    }
    tmp[o] = 0;
    if ((DWORD)o >= *n) { *n = (DWORD)o + 1; return E_POINTER_; }
    for (int i = 0; i <= o; i++) out[i] = tmp[i];
    *n = (DWORD)o;
    return S_OK_;
}

LWSTDAPI_(HRESULT) UrlUnescapeW(LPWSTR in, LPWSTR out, LPDWORD n, DWORD flags)
{
    WCHAR tmp[4096];
    int o = 0;
    for (WCHAR *p = in; *p && o < 4095; p++) {
        if (*p == '%' && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) { tmp[o++] = (WCHAR)(hexval(p[1]) * 16 + hexval(p[2])); p += 2; }
        else tmp[o++] = *p;
    }
    tmp[o] = 0;
    if (flags & URL_UNESCAPE_INPLACE) { for (int i = 0; i <= o; i++) in[i] = tmp[i]; return S_OK_; }
    if (!out || !n) return E_INVALIDARG_;
    if ((DWORD)o >= *n) { *n = (DWORD)o + 1; return E_POINTER_; }
    for (int i = 0; i <= o; i++) out[i] = tmp[i];
    *n = (DWORD)o;
    return S_OK_;
}

LWSTDAPI_(HRESULT) UrlCanonicalizeW(LPCWSTR in, LPWSTR out, LPDWORD n, DWORD flags)
{
    (void)flags;
    int l = wlen(in);
    if ((DWORD)l >= *n) { *n = (DWORD)l + 1; return E_POINTER_; }
    for (int i = 0; i <= l; i++) out[i] = in[i];
    *n = (DWORD)l;
    return S_OK_;
}

/* URL_PART_SCHEME 1, HOSTNAME 2, USERNAME 3, PASSWORD 4, PORT 5, QUERY 6 */
LWSTDAPI_(HRESULT) UrlGetPartW(LPCWSTR u, LPWSTR out, LPDWORD n, DWORD part, DWORD flags)
{
    (void)flags;
    int sl;
    if (!url_scheme(u, &sl)) return E_INVALIDARG_;
    const WCHAR *s = u, *e = u + sl;
    if (part != 1) {
        const WCHAR *auth = u + sl + 1;
        if (auth[0] == '/' && auth[1] == '/') auth += 2;
        const WCHAR *ae = auth;
        while (*ae && *ae != '/' && *ae != '?' && *ae != '#') ae++;
        const WCHAR *at = 0;
        for (const WCHAR *c = auth; c < ae; c++) if (*c == '@') at = c;
        const WCHAR *host = at ? at + 1 : auth, *colon = 0;
        for (const WCHAR *c = host; c < ae; c++) if (*c == ':') colon = c;
        switch (part) {
        case 2: s = host; e = colon ? colon : ae; break;
        case 5: if (!colon) return E_INVALIDARG_; s = colon + 1; e = ae; break;
        case 3: case 4: {
            if (!at) return E_INVALIDARG_;
            const WCHAR *c = auth;
            while (c < at && *c != ':') c++;
            if (part == 3) { s = auth; e = c; } else { if (c == at) return E_INVALIDARG_; s = c + 1; e = at; }
            break;
        }
        case 6: {
            const WCHAR *q = ae;
            while (*q && *q != '?') q++;
            if (!*q) return E_INVALIDARG_;
            s = q + 1; e = s;
            while (*e && *e != '#') e++;
            break;
        }
        default: return E_INVALIDARG_;
        }
    }
    DWORD l = (DWORD)(e - s);
    if (l >= *n) { *n = l + 1; return E_POINTER_; }
    for (DWORD i = 0; i < l; i++) out[i] = s[i];
    out[l] = 0;
    *n = l;
    return S_OK_;
}

typedef struct { DWORD cbSize; LPCWSTR pszProtocol; UINT cchProtocol; LPCWSTR pszSuffix; UINT cchSuffix; UINT nScheme; } PARSEDURLW;

LWSTDAPI_(HRESULT) ParseURLW(LPCWSTR u, PARSEDURLW *p)
{
    int sl;
    if (!p || p->cbSize != sizeof(*p)) return E_INVALIDARG_;
    if (!url_scheme(u, &sl)) return 0x80041001L;            /* URL_E_INVALID_SYNTAX */
    p->pszProtocol = u;
    p->cchProtocol = (UINT)sl;
    p->pszSuffix = u + sl + 1;
    p->cchSuffix = (UINT)wlen(u + sl + 1);
    p->nScheme = 1;                                         /* URL_SCHEME_UNKNOWN-ish */
    return S_OK_;
}

/* -----------------------------------------------------------------------
 * Odds and ends
 * ----------------------------------------------------------------------- */
/* OS_* questions about "the Windows we are": a Windows 10 workstation */
LWSTDAPI_(BOOL) IsOS(DWORD what)
{
    switch (what) {
    case 1:  /* OS_NT */          case 13: /* OS_WIN2000ORGREATER */ case 18: /* OS_WIN2000PRO */
    case 19: /* OS_WIN2000... */  case 21: /* OS_PROFESSIONAL */     case 22: /* OS_DATACENTER-ish */
    case 25: /* OS_WHISTLERORGREATER */ case 26: /* OS_PERSONAL? */ case 30: /* OS_64BIT */
        return what != 22 && what != 26;
    case 36: /* OS_WOW6432 */ return FALSE;
    }
    return FALSE;
}

LWSTDAPI_(HRESULT) HashData(const BYTE *data, DWORD n, BYTE *out, DWORD outn)
{
    /* Pearson-style hash spread over @outn bytes, like shlwapi's */
    static BYTE table[256];
    if (!table[1]) for (int i = 0; i < 256; i++) table[i] = (BYTE)((i * 167 + 13) & 0xFF);
    for (DWORD i = 0; i < outn; i++) {
        BYTE h = table[(i + (n ? data[0] : 0)) & 0xFF];
        for (DWORD k = 0; k < n; k++) h = table[h ^ data[k]];
        out[i] = h;
    }
    return S_OK_;
}

LWSTDAPI_(HRESULT) SHAutoComplete(HWND h, DWORD flags) { (void)h; (void)flags; return S_OK_; }

LWSTDAPI_(HRESULT) AssocQueryStringW(DWORD flags, DWORD str, LPCWSTR assoc, LPCWSTR extra, LPWSTR out, DWORD *n)
{
    (void)flags; (void)str; (void)assoc; (void)extra; (void)out; (void)n;
    return HR_WIN32(1155);                                  /* ERROR_NO_ASSOCIATION */
}

LWSTDAPI_(HRESULT) AssocQueryStringA(DWORD flags, DWORD str, LPCSTR assoc, LPCSTR extra, LPSTR out, DWORD *n)
{
    (void)flags; (void)str; (void)assoc; (void)extra; (void)out; (void)n;
    return HR_WIN32(1155);
}

LWSTDAPI_(HRESULT) GetAcceptLanguagesW(LPWSTR buf, DWORD *n)
{
    static const WCHAR en[] = { 'e', 'n', '-', 'U', 'S', 0 };
    if (*n < 6) { *n = 6; return E_INVALIDARG_; }
    for (int i = 0; i < 6; i++) buf[i] = en[i];
    *n = 5;
    return S_OK_;
}

/* Only plain strings: "@dll,-id" resource references are not resolved */
LWSTDAPI_(HRESULT) SHLoadIndirectString(LPCWSTR src, LPWSTR out, UINT n, void **reserved)
{
    (void)reserved;
    if (src[0] == '@') return E_INVALIDARG_;
    UINT l = (UINT)wlen(src);
    if (l >= n) return E_POINTER_;
    for (UINT i = 0; i <= l; i++) out[i] = src[i];
    return S_OK_;
}

LWSTDAPI_(HRESULT) SHStrDupW(LPCWSTR s, LPWSTR *out)
{
    if (!s || !out) return E_INVALIDARG_;
    int l = wlen(s);
    *out = LocalAlloc(0, 2 * ((SIZE_T)l + 1));              /* the process heap, like CoTaskMemAlloc */
    if (!*out) return (HRESULT)0x8007000EL;
    for (int i = 0; i <= l; i++) (*out)[i] = s[i];
    return S_OK_;
}

LWSTDAPI_(HRESULT) SHStrDupA(LPCSTR s, LPWSTR *out)
{
    if (!s || !out) return E_INVALIDARG_;
    int l = MultiByteToWideChar(CP_UTF8, 0, s, -1, 0, 0);
    *out = LocalAlloc(0, 2 * (SIZE_T)l);
    if (!*out) return (HRESULT)0x8007000EL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, *out, l);
    return S_OK_;
}

/* ---- colors: hue/luminance/saturation on a 0..240 scale ---- */
LWSTDAPI_(void) ColorRGBToHLS(COLORREF rgb, WORD *h, WORD *l, WORD *s)
{
    int r = GetRValue(rgb), g = GetGValue(rgb), b = GetBValue(rgb);
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int L = ((mx + mn) * 240 + 255) / 510, H = 160, S = 0;
    if (mx != mn) {
        int d = mx - mn;
        S = L <= 120 ? (d * 240 + (mx + mn) / 2) / (mx + mn) : (d * 240 + (510 - mx - mn) / 2) / (510 - mx - mn);
        int rd = ((mx - r) * 40 + d / 2) / d, gd = ((mx - g) * 40 + d / 2) / d, bd = ((mx - b) * 40 + d / 2) / d;
        H = r == mx ? bd - gd : g == mx ? 80 + rd - bd : 160 + gd - rd;
        if (H < 0) H += 240;
        if (H >= 240) H -= 240;
    }
    *h = (WORD)H; *l = (WORD)L; *s = (WORD)S;
}

static int hue_part(int n1, int n2, int h)
{
    if (h < 0) h += 240;
    if (h > 240) h -= 240;
    if (h < 40) return n1 + ((n2 - n1) * h + 20) / 40;
    if (h < 120) return n2;
    if (h < 160) return n1 + ((n2 - n1) * (160 - h) + 20) / 40;
    return n1;
}

LWSTDAPI_(COLORREF) ColorHLSToRGB(WORD h, WORD l, WORD s)
{
    if (!s) { int v = l * 255 / 240; return RGB(v, v, v); }
    int m2 = l <= 120 ? (l * (240 + s) + 120) / 240 : l + s - (l * s + 120) / 240, m1 = 2 * l - m2;
    int r = hue_part(m1, m2, h + 80), g = hue_part(m1, m2, h), b = hue_part(m1, m2, h - 80);
    return RGB(r * 255 / 240, g * 255 / 240, b * 255 / 240);
}

LWSTDAPI_(COLORREF) ColorAdjustLuma(COLORREF c, int n, BOOL scale)
{
    WORD h, l, s;
    ColorRGBToHLS(c, &h, &l, &s);
    int L = scale ? l + l * n / 1000 : l + n;
    if (L < 0) L = 0;
    if (L > 240) L = 240;
    return ColorHLSToRGB(h, (WORD)L, s);
}

/* ---- threads ---- */
typedef struct { LPTHREAD_START_ROUTINE fn; void *arg; } ThreadStart;

static DWORD WINAPI sh_thread(LPVOID p)
{
    ThreadStart t = *(ThreadStart *)p;
    LocalFree(p);
    return t.fn(t.arg);
}

LWSTDAPI_(BOOL) SHCreateThread(LPTHREAD_START_ROUTINE fn, void *arg, DWORD flags, LPTHREAD_START_ROUTINE sync)
{
    (void)flags;
    ThreadStart *t = LocalAlloc(0, sizeof(*t));
    if (!t) return FALSE;
    t->fn = fn; t->arg = arg;
    if (sync) sync(arg);
    HANDLE h = CreateThread(0, 0, sh_thread, t, 0, 0);
    if (!h) { LocalFree(t); if (flags & 2 /* CTF_INSIST */) fn(arg); return (flags & 2) != 0; }
    CloseHandle(h);
    return TRUE;
}

static void *g_thread_ref;
LWSTDAPI_(HRESULT) SHSetThreadRef(void *p) { g_thread_ref = p; return S_OK_; }
LWSTDAPI_(HRESULT) SHGetThreadRef(void **p) { *p = g_thread_ref; return g_thread_ref ? S_OK_ : E_NOINTERFACE_; }

/* ---- COM helper: a QueryInterface from a table of (IID, offset) ---- */
typedef struct { const GUID *piid; DWORD dwOffset; } QITAB;
typedef struct Unk { struct UnkVtbl *v; } Unk;
struct UnkVtbl { HRESULT (WINAPI *QueryInterface)(Unk *, REFIID, void **); ULONG (WINAPI *AddRef)(Unk *); ULONG (WINAPI *Release)(Unk *); };

static BOOL guid_eq(const GUID *a, const GUID *b)
{
    const BYTE *x = (const BYTE *)a, *y = (const BYTE *)b;
    for (int i = 0; i < 16; i++) if (x[i] != y[i]) return FALSE;
    return TRUE;
}

LWSTDAPI_(HRESULT) QISearch(void *self, const QITAB *tab, REFIID riid, void **out)
{
    static const GUID iid_unknown = { 0, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
    if (!out) return E_POINTER_;
    for (const QITAB *t = tab; t->piid; t++)
        if (guid_eq(t->piid, riid)) {
            Unk *u = (Unk *)((BYTE *)self + t->dwOffset);
            u->v->AddRef(u);
            *out = u;
            return S_OK_;
        }
    if (guid_eq(riid, &iid_unknown)) {
        Unk *u = (Unk *)((BYTE *)self + tab[0].dwOffset);
        u->v->AddRef(u);
        *out = u;
        return S_OK_;
    }
    *out = 0;
    return E_NOINTERFACE_;
}

/* shcore's per-monitor DPI (api-ms-win-shcore-scaling): every monitor is
 * 96 DPI, 100% scale, as user32's GetDpiForWindow reports */
__declspec(dllexport) HRESULT __stdcall GetDpiForMonitor(HANDLE mon, int type, UINT *x, UINT *y)
{
    (void)mon; (void)type;
    if (!x || !y) return E_INVALIDARG_;
    *x = *y = 96;
    return S_OK_;
}
__declspec(dllexport) HRESULT __stdcall GetScaleFactorForMonitor(HANDLE mon, int *scale)
{
    (void)mon;
    if (!scale) return E_INVALIDARG_;
    *scale = 100;                                   /* SCALE_100_PERCENT */
    return S_OK_;
}
__declspec(dllexport) HRESULT __stdcall SetProcessDpiAwareness(int v) { (void)v; return S_OK_; }
__declspec(dllexport) HRESULT __stdcall GetProcessDpiAwareness(HANDLE p, int *v) { (void)p; if (!v) return E_INVALIDARG_; *v = 2; return S_OK_; }
