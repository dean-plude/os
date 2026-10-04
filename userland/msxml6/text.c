/*
 * msxml6.dll: strings (BSTR <-> libxml2's UTF-8), VARIANT to text, the
 * windows-1252 decoder libxml2 lacks without iconv, and reading a
 * document's bytes from a path, a file:// or http(s):// URL or a stream.
 */
#include "msxml_private.h"
#include <libxml/encoding.h>

void *mem_alloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n ? n : 1); }
void *mem_realloc(void *p, SIZE_T n)
{
    return p ? HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, p, n ? n : 1) : mem_alloc(n);
}
void mem_free(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

WCHAR *wide_utf8(const xmlChar *s, int n, int *outlen)
{
    if (!s) s = (const xmlChar *)"";
    if (n < 0) n = (int)strlen((const char *)s);
    int w = n ? MultiByteToWideChar(CP_UTF8, 0, (const char *)s, n, 0, 0) : 0;
    WCHAR *out = mem_alloc((SIZE_T)(w + 1) * sizeof(WCHAR));
    if (!out) return 0;
    if (w) MultiByteToWideChar(CP_UTF8, 0, (const char *)s, n, out, w);
    out[w] = 0;
    if (outlen) *outlen = w;
    return out;
}

BSTR bstr_utf8n(const xmlChar *s, int n)
{
    if (!s) s = (const xmlChar *)"";
    if (n < 0) n = (int)strlen((const char *)s);
    int w = n ? MultiByteToWideChar(CP_UTF8, 0, (const char *)s, n, 0, 0) : 0;
    BSTR b = SysAllocStringLen(0, (UINT)w);
    if (b && w) MultiByteToWideChar(CP_UTF8, 0, (const char *)s, n, b, w);
    return b;
}

BSTR bstr_utf8(const xmlChar *s) { return bstr_utf8n(s, -1); }

xmlChar *utf8_wide(const WCHAR *s, int n)
{
    if (!s) s = L"";
    if (n < 0) n = (int)wcslen(s);
    int u = n ? WideCharToMultiByte(CP_UTF8, 0, s, n, 0, 0, 0, 0) : 0;
    xmlChar *out = xmlMalloc((size_t)u + 1);
    if (!out) return 0;
    if (u) WideCharToMultiByte(CP_UTF8, 0, s, n, (char *)out, u, 0, 0);
    out[u] = 0;
    return out;
}

HRESULT ret_bstr(BSTR *out, const xmlChar *s)
{
    if (!out) return E_INVALIDARG;
    *out = bstr_utf8(s);
    return *out ? S_OK : E_OUTOFMEMORY;
}

HRESULT variant_string(const VARIANT *v, BSTR *out)
{
    *out = 0;
    const VARIANT *src = v;
    if (v->vt == (VT_BYREF | VT_VARIANT)) src = v->pvarVal;
    if (src->vt == VT_BSTR) {
        *out = SysAllocStringLen(src->bstrVal, SysStringLen(src->bstrVal));
        return *out ? S_OK : E_OUTOFMEMORY;
    }
    if (src->vt == VT_BOOL) {                  /* MSXML writes booleans as "true"/"false"... */
        *out = SysAllocString(src->boolVal ? L"true" : L"false");
        return *out ? S_OK : E_OUTOFMEMORY;
    }
    VARIANT t;
    VariantInit(&t);
    HRESULT hr = VariantChangeType(&t, (VARIANT *)src, 0, VT_BSTR);
    if (FAILED(hr)) return hr;
    *out = t.bstrVal;
    return S_OK;
}

/* ---- windows-1252 (libxml2 has UTF-8/16, ISO-8859-x and ASCII built in) ---- */
static const unsigned short cp1252_hi[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

static int cp1252_in(unsigned char *out, int *outlen, const unsigned char *in, int *inlen)
{
    int i = 0, o = 0;
    while (i < *inlen) {
        unsigned c = in[i];
        if (c >= 0x80 && c < 0xA0) c = cp1252_hi[c - 0x80];
        int need = c < 0x80 ? 1 : c < 0x800 ? 2 : 3;
        if (o + need > *outlen) break;
        if (need == 1) out[o++] = (unsigned char)c;
        else if (need == 2) { out[o++] = (unsigned char)(0xC0 | c >> 6); out[o++] = (unsigned char)(0x80 | (c & 0x3F)); }
        else { out[o++] = (unsigned char)(0xE0 | c >> 12); out[o++] = (unsigned char)(0x80 | (c >> 6 & 0x3F));
               out[o++] = (unsigned char)(0x80 | (c & 0x3F)); }
        i++;
    }
    *inlen = i;
    *outlen = o;
    return o;
}

void encodings_init(void)
{
    xmlNewCharEncodingHandler("windows-1252", cp1252_in, 0);
    xmlAddEncodingAlias("windows-1252", "cp1252");
}

/* ---- reading a document's bytes ---- */
static HRESULT read_file(const WCHAR *path, char **data, DWORD *len)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
    if (f == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    DWORD size = GetFileSize(f, 0), got = 0;
    char *buf = mem_alloc(size + 1);
    if (!buf) { CloseHandle(f); return E_OUTOFMEMORY; }
    BOOL ok = ReadFile(f, buf, size, &got, 0);
    CloseHandle(f);
    if (!ok) { mem_free(buf); return E_FAIL; }
    *data = buf;
    *len = got;
    return S_OK;
}

static int hexval(WCHAR c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

HRESULT read_url(const WCHAR *url, char **data, DWORD *len)
{
    if (!_wcsnicmp(url, L"http://", 7) || !_wcsnicmp(url, L"https://", 8)) return http_get(url, data, len);
    WCHAR path[MAX_PATH];
    int n = 0;
    if (!_wcsnicmp(url, L"file://", 7)) {           /* file:///C:/dir/a%20b.xml */
        const WCHAR *p = url + 7;
        if (*p == '/' && p[1] && p[2] == ':') p++;
        for (; *p && n < MAX_PATH - 1; p++) {
            if (*p == '%' && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) { path[n++] = (WCHAR)(hexval(p[1]) * 16 + hexval(p[2])); p += 2; }
            else path[n++] = *p == '/' ? '\\' : *p;
        }
    } else
        for (const WCHAR *p = url; *p && n < MAX_PATH - 1; p++) path[n++] = *p == '/' ? '\\' : *p;
    path[n] = 0;
    return read_file(path, data, len);
}

HRESULT read_stream(IUnknown *unk, char **data, DWORD *len)
{
    ISequentialStream *s = 0;
    if (FAILED(unk->lpVtbl->QueryInterface(unk, &IID_IStream, (void **)&s)) &&
        FAILED(unk->lpVtbl->QueryInterface(unk, &IID_ISequentialStream, (void **)&s)))
        return E_INVALIDARG;
    DWORD cap = 65536, n = 0;
    char *buf = mem_alloc(cap + 1);
    HRESULT hr = S_OK;
    for (;;) {
        if (!buf) { hr = E_OUTOFMEMORY; break; }
        if (cap - n < 16384) {
            char *b = mem_realloc(buf, (SIZE_T)cap * 2 + 1);
            if (!b) { hr = E_OUTOFMEMORY; break; }
            buf = b;
            cap *= 2;
        }
        ULONG got = 0;
        hr = s->lpVtbl->Read(s, buf + n, cap - n, &got);
        n += got;
        if (FAILED(hr) || !got) break;
    }
    s->lpVtbl->Release(s);
    if (FAILED(hr)) { mem_free(buf); return hr; }
    *data = buf;
    *len = n;
    return S_OK;
}
