/*
 * riched20.dll — the Rich Edit control (classes RichEdit20W and
 * RichEdit20A; msftedit.dll builds this file again for RICHEDIT50W).
 *
 * Installers show their licence and read-me text in one (Inno Setup's
 * wizard, NSIS's licence page), as RTF streamed in with EM_STREAMIN.  This
 * one is the system EDIT control underneath (a superclass of it): RTF that
 * comes in, by EM_STREAMIN, EM_SETTEXTEX or WM_SETTEXT, becomes its plain
 * text (paragraphs, tabs, \'hh and \u characters, field results; font,
 * colour and picture tables and other destinations left out), and the
 * Rich Edit messages programs send are answered: selections as CHARRANGEs,
 * text ranges, finding text, streaming out, event masks and options.
 * Character and paragraph formatting is accepted and not drawn: the text
 * shows in the control's one font.  Written from the Rich Edit
 * documentation; the RTF reader follows the RTF 1.9 specification.
 */
#include <windows.h>

#ifndef RICHEDIT_CLASS_W                          /* (msftedit.c defines RICHEDIT50W alone) */
#define RICHEDIT_CLASS_W L"RichEdit20W"
#define RICHEDIT_CLASS_A L"RichEdit20A"
#endif

void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);

/* ---- the documentation's messages and structures ------------------ */
#define EM_EXGETSEL         (WM_USER + 52)
#define EM_EXLIMITTEXT      (WM_USER + 53)
#define EM_EXLINEFROMCHAR   (WM_USER + 54)
#define EM_EXSETSEL         (WM_USER + 55)
#define EM_FINDTEXT         (WM_USER + 56)
#define EM_GETCHARFORMAT    (WM_USER + 58)
#define EM_GETEVENTMASK     (WM_USER + 59)
#define EM_GETOLEINTERFACE  (WM_USER + 60)
#define EM_GETPARAFORMAT    (WM_USER + 61)
#define EM_GETSELTEXT       (WM_USER + 62)
#define EM_HIDESELECTION    (WM_USER + 63)
#define EM_REQUESTRESIZE    (WM_USER + 65)
#define EM_SELECTIONTYPE    (WM_USER + 66)
#define EM_SETBKGNDCOLOR    (WM_USER + 67)
#define EM_SETCHARFORMAT    (WM_USER + 68)
#define EM_SETEVENTMASK     (WM_USER + 69)
#define EM_SETOLECALLBACK   (WM_USER + 70)
#define EM_SETPARAFORMAT    (WM_USER + 71)
#define EM_SETTARGETDEVICE  (WM_USER + 72)
#define EM_STREAMIN         (WM_USER + 73)
#define EM_STREAMOUT        (WM_USER + 74)
#define EM_GETTEXTRANGE     (WM_USER + 75)
#define EM_SETOPTIONS       (WM_USER + 77)
#define EM_GETOPTIONS       (WM_USER + 78)
#define EM_FINDTEXTEX       (WM_USER + 79)
#define EM_SETUNDOLIMIT     (WM_USER + 82)
#define EM_REDO             (WM_USER + 84)
#define EM_CANREDO          (WM_USER + 85)
#define EM_STOPGROUPTYPING  (WM_USER + 88)
#define EM_SETTEXTMODE      (WM_USER + 89)
#define EM_GETTEXTMODE      (WM_USER + 90)
#define EM_AUTOURLDETECT    (WM_USER + 91)
#define EM_GETAUTOURLDETECT (WM_USER + 92)
#define EM_GETTEXTEX        (WM_USER + 94)
#define EM_GETTEXTLENGTHEX  (WM_USER + 95)
#define EM_SHOWSCROLLBAR    (WM_USER + 96)
#define EM_SETTEXTEX        (WM_USER + 97)
#define EM_SETLANGOPTIONS   (WM_USER + 120)
#define EM_FINDTEXTW        (WM_USER + 123)
#define EM_FINDTEXTEXW      (WM_USER + 124)
#define EM_GETLANGOPTIONS   (WM_USER + 121)
#define EM_SETEDITSTYLE     (WM_USER + 204)
#define EM_GETEDITSTYLE     (WM_USER + 205)
#define EM_GETSCROLLPOS     (WM_USER + 221)
#define EM_SETSCROLLPOS     (WM_USER + 222)
#define EM_SETFONTSIZE      (WM_USER + 223)
#define EM_GETZOOM          (WM_USER + 224)
#define EM_SETZOOM          (WM_USER + 225)

#define SF_TEXT        0x0001
#define SF_RTF         0x0002
#define SF_UNICODE     0x0010
#define SFF_SELECTION  0x8000
#define SF_USECODEPAGE 0x0020

#define ST_SELECTION   2
#define GT_USECRLF     1
#define GT_SELECTION   2
#define FR_DOWN        1
#define FR_WHOLEWORD   2
#define FR_MATCHCASE   4
#define SEL_EMPTY      0
#define SEL_TEXT       1
#define SEL_MULTICHAR  4

#pragma pack(push, 4)                             /* richedit.h packs its structures to 4 bytes (EDITSTREAM's callback is at 12 in 64-bit code) */
typedef DWORD (CALLBACK *EDITSTREAMCALLBACK)(DWORD_PTR cookie, LPBYTE buf, LONG cb, LONG *done);
typedef struct { DWORD_PTR dwCookie; DWORD dwError; EDITSTREAMCALLBACK pfnCallback; } EDITSTREAM;
typedef struct { LONG cpMin, cpMax; } CHARRANGE;
typedef struct { CHARRANGE chrg; LPWSTR lpstrText; } TEXTRANGEW;
typedef struct { CHARRANGE chrg; LPCWSTR lpstrText; } FINDTEXTW;
typedef struct { CHARRANGE chrg; LPCWSTR lpstrText; CHARRANGE chrgText; } FINDTEXTEXW;
typedef struct { DWORD flags; UINT codepage; } SETTEXTEX;
typedef struct { DWORD cb, flags; UINT codepage; LPCSTR lpDefaultChar; LPBOOL lpUsedDefChar; } GETTEXTEX;
typedef struct { DWORD flags; UINT codepage; } GETTEXTLENGTHEX;
#pragma pack(pop)

/* ---- per window ------------------------------------------------------ */
typedef struct {
    BOOL     ansi;              /* a RichEdit20A window: text messages carry ANSI strings */
    DWORD    events, options, langopts, editstyle;
    COLORREF bk;
    LONG     textmode, urls;
} Rich;

static WNDPROC g_edit;
static const WCHAR PROP[] = L"NovaRichEdit";

static Rich *rich(HWND h) { return (Rich *)GetPropW(h, PROP); }

static void *xalloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }
static void *xrealloc(void *p, SIZE_T n)
{
    return p ? HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, p, n) : xalloc(n);
}
static void xfree(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

static LRESULT edit(HWND h, UINT m, WPARAM wp, LPARAM lp) { return CallWindowProcW(g_edit, h, m, wp, lp); }

/* ---- a growing UTF-16 string ----------------------------------------- */
typedef struct { WCHAR *s; int n, cap; } Str;

static void put(Str *o, WCHAR c)
{
    if (o->n + 2 > o->cap) {
        int cap = o->cap ? o->cap * 2 : 256;
        WCHAR *s = xrealloc(o->s, (SIZE_T)cap * sizeof(WCHAR));
        if (!s) return;
        o->s = s;
        o->cap = cap;
    }
    o->s[o->n++] = c;
    o->s[o->n] = 0;
}

/* ---- RTF to plain text ---------------------------------------------- */
/* Destinations whose text is not part of the document */
static const char *const g_skip[] = {
    "fonttbl", "colortbl", "stylesheet", "info", "pict", "header", "headerl", "headerr", "headerf",
    "footer", "footerl", "footerr", "footerf", "listtable", "listoverridetable", "rsidtbl", "generator",
    "xmlnstbl", "themedata", "colorschememapping", "datastore", "latentstyles", "pgdsctbl", "object",
    "fldinst", "filetbl", "revtbl", "bkmkstart", "bkmkend", "nonshppict", "footnote", "annotation",
    "private", "mmathPr", "wgrffmtfilter", "panose", "falt", "objdata", "shppict", "template", 0
};

static int is_skip(const char *w)
{
    for (int i = 0; g_skip[i]; i++) if (!lstrcmpA(g_skip[i], w)) return 1;
    return 0;
}

/* Appends the RTF document @rtf (@n bytes) to @o as text, lines ending "\r\n" */
static void rtf_to_text(const BYTE *rtf, int n, Str *o)
{
    enum { DEPTH = 64 };
    int skip[DEPTH], uc[DEPTH], d = 0;            /* per group: skipping its text, \ucN */
    UINT cp = CP_ACP;
    int pending = 0;                              /* characters still to drop after a \u */
    skip[0] = 0;
    uc[0] = 1;
    BYTE mb[2];
    int nmb = 0;                                  /* a DBCS lead byte waiting for its trail */
    for (int i = 0; i < n; ) {
        BYTE c = rtf[i];
        if (c == '{') {
            if (d + 1 < DEPTH) { d++; skip[d] = skip[d - 1]; uc[d] = uc[d - 1]; }
            i++;
            continue;
        }
        if (c == '}') {
            if (d > 0) d--;
            i++;
            continue;
        }
        if (c == '\r' || c == '\n') { i++; continue; }
        if (c != '\\') {
            i++;
            if (pending) { pending--; continue; }
            if (skip[d]) continue;
            if (nmb || (cp != CP_ACP && cp != 1252 && IsDBCSLeadByteEx(cp, c))) {
                mb[nmb++] = c;
                if (nmb < 2) continue;
                WCHAR w;
                if (MultiByteToWideChar(cp, 0, (const char *)mb, 2, &w, 1) == 1) put(o, w);
                nmb = 0;
                continue;
            }
            WCHAR w = c;
            if (c >= 0x80) MultiByteToWideChar(cp, 0, (const char *)&c, 1, &w, 1);
            put(o, w);
            continue;
        }
        /* a control symbol or word */
        i++;
        if (i >= n) break;
        c = rtf[i];
        if (c == '\\' || c == '{' || c == '}') {
            i++;
            if (pending) { pending--; continue; }
            if (!skip[d]) put(o, c);
            continue;
        }
        if (c == '\'') {                          /* \'hh: a byte in the document's code page */
            int v = 0;
            for (int k = 1; k <= 2 && i + k < n; k++) {
                BYTE h = rtf[i + k];
                v = v * 16 + (h >= '0' && h <= '9' ? h - '0' : (h | 32) >= 'a' && (h | 32) <= 'f' ? (h | 32) - 'a' + 10 : 0);
            }
            i += 3;
            if (pending) { pending--; continue; }
            if (skip[d]) continue;
            BYTE b = (BYTE)v;
            if (nmb || (cp != CP_ACP && cp != 1252 && IsDBCSLeadByteEx(cp, b))) {
                mb[nmb++] = b;
                if (nmb < 2) continue;
                WCHAR w;
                if (MultiByteToWideChar(cp, 0, (const char *)mb, 2, &w, 1) == 1) put(o, w);
                nmb = 0;
                continue;
            }
            WCHAR w = b;
            if (b >= 0x80) MultiByteToWideChar(cp, 0, (const char *)&b, 1, &w, 1);
            put(o, w);
            continue;
        }
        if (c == '*') { skip[d] = 1; i++; continue; }      /* {\* ...}: an optional destination */
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
            i++;
            if (skip[d]) continue;
            if (c == '~') put(o, 0xA0);
            else if (c == '_') put(o, 0x2011);
            else if (c == '\r' || c == '\n') { put(o, '\r'); put(o, '\n'); }   /* "\<newline>" is \par */
            continue;                             /* \- (optional hyphen), \:, \| ... */
        }
        char word[32];
        int wl = 0;
        while (i < n && ((rtf[i] >= 'a' && rtf[i] <= 'z') || (rtf[i] >= 'A' && rtf[i] <= 'Z'))) {
            if (wl < 31) word[wl++] = (char)rtf[i];
            i++;
        }
        word[wl] = 0;
        int has = 0, neg = 0;
        long arg = 0;
        if (i < n && rtf[i] == '-') { neg = 1; i++; }
        while (i < n && rtf[i] >= '0' && rtf[i] <= '9') { has = 1; arg = arg * 10 + (rtf[i] - '0'); i++; }
        if (neg) arg = -arg;
        if (i < n && rtf[i] == ' ') i++;          /* the delimiter belongs to the word */
        if (is_skip(word)) { skip[d] = 1; continue; }
        if (!lstrcmpA(word, "ansicpg") && has) { cp = (UINT)arg; continue; }
        if (!lstrcmpA(word, "uc") && has) { uc[d] = (int)arg; continue; }
        if (!lstrcmpA(word, "u") && has) {
            if (!skip[d]) put(o, (WCHAR)(arg < 0 ? arg + 65536 : arg));
            pending = uc[d];
            continue;
        }
        if (skip[d]) continue;
        if (pending) pending = 0;                 /* a control word ends the \u's fallback */
        if (!lstrcmpA(word, "par") || !lstrcmpA(word, "line") || !lstrcmpA(word, "row") || !lstrcmpA(word, "sect") ||
            !lstrcmpA(word, "page")) { put(o, '\r'); put(o, '\n'); }
        else if (!lstrcmpA(word, "tab") || !lstrcmpA(word, "cell")) put(o, '\t');
        else if (!lstrcmpA(word, "emdash")) put(o, 0x2014);
        else if (!lstrcmpA(word, "endash")) put(o, 0x2013);
        else if (!lstrcmpA(word, "bullet")) put(o, 0x2022);
        else if (!lstrcmpA(word, "lquote")) put(o, 0x2018);
        else if (!lstrcmpA(word, "rquote")) put(o, 0x2019);
        else if (!lstrcmpA(word, "ldblquote")) put(o, 0x201C);
        else if (!lstrcmpA(word, "rdblquote")) put(o, 0x201D);
        else if (!lstrcmpA(word, "emspace") || !lstrcmpA(word, "enspace") || !lstrcmpA(word, "qmspace")) put(o, ' ');
    }
    /* a document ends without a final paragraph mark: drop a trailing one */
    if (o->n >= 2 && o->s[o->n - 2] == '\r' && o->s[o->n - 1] == '\n') o->s[o->n -= 2] = 0;
}

static int is_rtf(const BYTE *b, int n) { return n >= 5 && b[0] == '{' && b[1] == '\\' && b[2] == 'r' && b[3] == 't' && b[4] == 'f'; }

/* Bytes (RTF, or text in @cp, or UTF-16 when @unicode) to the control's text */
static Str to_text(const BYTE *b, int n, BOOL unicode, UINT cp)
{
    Str o = { 0 };
    if (!unicode && is_rtf(b, n)) { rtf_to_text(b, n, &o); return o; }
    if (unicode) {
        const WCHAR *w = (const WCHAR *)b;
        int wn = n / 2;
        if (wn >= 5 && w[0] == '{' && w[1] == '\\' && w[2] == 'r' && w[3] == 't' && w[4] == 'f') {   /* RTF as UTF-16 */
            BYTE *a = xalloc((SIZE_T)wn + 1);
            if (a) {
                for (int i = 0; i < wn; i++) a[i] = w[i] < 0x80 ? (BYTE)w[i] : '?';
                rtf_to_text(a, wn, &o);
                xfree(a);
            }
            return o;
        }
        for (int i = 0; i < wn; i++) put(&o, w[i]);
        return o;
    }
    int wn = n ? MultiByteToWideChar(cp, 0, (const char *)b, n, 0, 0) : 0;
    o.s = xalloc(((SIZE_T)wn + 1) * sizeof(WCHAR));
    if (o.s) {
        if (wn) MultiByteToWideChar(cp, 0, (const char *)b, n, o.s, wn);
        o.n = o.cap = wn;
    }
    return o;
}

/* Lone "\n" or "\r" line ends as "\r\n", as the EDIT control shows them */
static void crlf(Str *s)
{
    Str o = { 0 };
    for (int i = 0; i < s->n; i++) {
        WCHAR c = s->s[i];
        if (c == '\r' && i + 1 < s->n && s->s[i + 1] == '\n') { put(&o, '\r'); put(&o, '\n'); i++; }
        else if (c == '\r' || c == '\n') { put(&o, '\r'); put(&o, '\n'); }
        else put(&o, c);
    }
    xfree(s->s);
    *s = o;
}

/* Replaces the whole text, or the selection */
static void set_text(HWND h, Str *t, BOOL selection)
{
    crlf(t);
    const WCHAR *s = t->s ? t->s : L"";
    if (selection) edit(h, EM_REPLACESEL, FALSE, (LPARAM)s);
    else {
        edit(h, WM_SETTEXT, 0, (LPARAM)s);
        edit(h, EM_SETSEL, 0, 0);
    }
    xfree(t->s);
    t->s = 0;
}

/* The control's text (heap, UTF-16) and its length */
static WCHAR *get_all(HWND h, int *len)
{
    int n = (int)edit(h, WM_GETTEXTLENGTH, 0, 0);
    WCHAR *s = xalloc(((SIZE_T)n + 1) * sizeof(WCHAR));
    if (!s) { *len = 0; return 0; }
    edit(h, WM_GETTEXT, (WPARAM)n + 1, (LPARAM)s);
    *len = lstrlenW(s);
    return s;
}

static LRESULT stream_in(HWND h, WPARAM fmt, EDITSTREAM *es)
{
    if (!es || !es->pfnCallback) return 0;
    BYTE *b = 0;
    int n = 0, cap = 0;
    es->dwError = 0;
    for (;;) {
        if (n + 4096 > cap) {
            BYTE *nb = xrealloc(b, (SIZE_T)(cap = cap ? cap * 2 : 8192));
            if (!nb) break;
            b = nb;
        }
        LONG got = 0;
        DWORD e = es->pfnCallback(es->dwCookie, b + n, 4096, &got);
        if (e) { es->dwError = e; break; }
        if (got <= 0) break;
        n += got;
    }
    UINT cp = (fmt & SF_USECODEPAGE) ? HIWORD(fmt) : CP_ACP;
    Str t = to_text(b ? b : (BYTE *)"", n, (fmt & SF_UNICODE) && !(fmt & SF_RTF), cp);
    int len = t.n;
    set_text(h, &t, (fmt & SFF_SELECTION) != 0);
    xfree(b);
    return len;
}

/* Text as RTF: the characters escaped, one \par per line */
static void rtf_out(const WCHAR *s, int n, Str *o)
{
    const char *head = "{\\rtf1\\ansi\\ansicpg1252\\deff0{\\fonttbl{\\f0 Segoe UI;}}\\uc1\\pard ";
    for (const char *c = head; *c; c++) put(o, (WCHAR)*c);
    for (int i = 0; i < n; i++) {
        WCHAR c = s[i];
        if (c == '\r') continue;
        if (c == '\n') { const char *p = "\\par\r\n"; while (*p) put(o, (WCHAR)*p++); continue; }
        if (c == '\\' || c == '{' || c == '}') { put(o, '\\'); put(o, c); continue; }
        if (c == '\t') { const char *p = "\\tab "; while (*p) put(o, (WCHAR)*p++); continue; }
        if (c < 0x80) { put(o, c); continue; }
        char num[16];
        wsprintfA(num, "\\u%d?", (int)(short)c);
        for (char *p = num; *p; p++) put(o, (WCHAR)*p);
    }
    put(o, '}');
}

static LRESULT stream_out(HWND h, WPARAM fmt, EDITSTREAM *es)
{
    if (!es || !es->pfnCallback) return 0;
    int n;
    WCHAR *all = get_all(h, &n);
    if (!all) return 0;
    const WCHAR *s = all;
    if (fmt & SFF_SELECTION) {
        DWORD lo = 0, hi = 0;
        edit(h, EM_GETSEL, (WPARAM)&lo, (LPARAM)&hi);
        if ((int)hi > n) hi = (DWORD)n;
        if (lo > hi) lo = hi;
        s = all + lo;
        n = (int)(hi - lo);
    }
    Str r = { 0 };
    BYTE *bytes;
    int nb;
    if (fmt & SF_RTF) { rtf_out(s, n, &r); s = r.s; n = r.n; }
    if ((fmt & SF_UNICODE) && !(fmt & SF_RTF)) { bytes = (BYTE *)s; nb = n * 2; }
    else {
        UINT cp = (fmt & SF_USECODEPAGE) ? HIWORD(fmt) : CP_ACP;
        nb = n ? WideCharToMultiByte(cp, 0, s, n, 0, 0, 0, 0) : 0;
        bytes = xalloc((SIZE_T)nb + 1);
        if (bytes && nb) WideCharToMultiByte(cp, 0, s, n, (char *)bytes, nb, 0, 0);
    }
    es->dwError = 0;
    int done = 0;
    while (bytes && done < nb) {
        LONG put_n = 0;
        DWORD e = es->pfnCallback(es->dwCookie, bytes + done, nb - done, &put_n);
        if (e) { es->dwError = e; break; }
        if (put_n <= 0) break;
        done += put_n;
    }
    if (bytes != (BYTE *)s) xfree(bytes);
    xfree(r.s);
    xfree(all);
    return done;
}

static int wieq(WCHAR a, WCHAR b, BOOL match_case)
{
    if (match_case) return a == b;
    return CharLowerW((LPWSTR)(ULONG_PTR)a) == CharLowerW((LPWSTR)(ULONG_PTR)b);
}

static int word_char(WCHAR c) { return IsCharAlphaNumericW(c) || c == '_'; }

/* EM_FINDTEXT(EX): the first match of @what in @cr (cpMax -1: to the end) */
static LONG find(HWND h, WPARAM flags, CHARRANGE cr, const WCHAR *what, CHARRANGE *found)
{
    int n, wl = what ? lstrlenW(what) : 0;
    WCHAR *s = get_all(h, &n);
    LONG r = -1;
    if (s && wl) {
        BOOL down = (flags & FR_DOWN) != 0;
        LONG lo = cr.cpMin, hi = cr.cpMax < 0 ? n : cr.cpMax;
        if (!down) { LONG t = lo; lo = hi < 0 ? 0 : hi; hi = t; }   /* searching up: cpMin is the start, cpMax the stop */
        if (lo < 0) lo = 0;
        if (hi > n) hi = n;
        for (LONG i = down ? lo : hi - wl; down ? i + wl <= hi : i >= lo; i += down ? 1 : -1) {
            int k = 0;
            while (k < wl && wieq(s[i + k], what[k], (flags & FR_MATCHCASE) != 0)) k++;
            if (k < wl) continue;
            if ((flags & FR_WHOLEWORD) && ((i > 0 && word_char(s[i - 1])) || (i + wl < n && word_char(s[i + wl])))) continue;
            r = i;
            break;
        }
    }
    if (found) { found->cpMin = r; found->cpMax = r < 0 ? -1 : r + wl; }
    xfree(s);
    return r;
}

static WCHAR *a2w(const char *a)
{
    int n = MultiByteToWideChar(CP_ACP, 0, a, -1, 0, 0);
    WCHAR *w = xalloc((SIZE_T)(n > 0 ? n : 1) * sizeof(WCHAR));
    if (w && n > 0) MultiByteToWideChar(CP_ACP, 0, a, -1, w, n);
    return w;
}

static LRESULT CALLBACK RichProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    Rich *r = rich(h);
    switch (m) {
    case WM_NCCREATE: {
        r = xalloc(sizeof(Rich));
        if (!r) return FALSE;
#ifdef RICHEDIT_CLASS_A
        WCHAR cls[32];
        GetClassNameW(h, cls, 32);
        r->ansi = !lstrcmpiW(cls, RICHEDIT_CLASS_A);
#endif
        r->bk = GetSysColor(COLOR_WINDOW);
        r->textmode = 1 | 4 | 32;                 /* TM_RICHTEXT | TM_MULTILEVELUNDO | TM_MULTICODEPAGE */
        SetPropW(h, PROP, r);
        return edit(h, m, wp, lp);
    }
    case WM_NCDESTROY:
        RemovePropW(h, PROP);
        xfree(r);
        return edit(h, m, wp, lp);
    case WM_CREATE: {                             /* the text it was created with may be RTF */
        LRESULT res = edit(h, m, wp, lp);
        int n;
        WCHAR *s = get_all(h, &n);
        if (s && n >= 5 && !memcmp(s, L"{\\rtf", 10)) {
            BYTE *a = xalloc((SIZE_T)n + 1);
            if (a) {
                for (int i = 0; i < n; i++) a[i] = s[i] < 0x80 ? (BYTE)s[i] : '?';
                Str t = to_text(a, n, FALSE, CP_ACP);
                set_text(h, &t, FALSE);
                xfree(a);
            }
        }
        xfree(s);
        return res;
    }
    case WM_SETTEXT: {
        const WCHAR *s = (const WCHAR *)lp;
        if (s && s[0] == '{' && s[1] == '\\' && s[2] == 'r' && s[3] == 't' && s[4] == 'f') {
            int n = lstrlenW(s);
            BYTE *a = xalloc((SIZE_T)n + 1);
            if (!a) return FALSE;
            for (int i = 0; i < n; i++) a[i] = s[i] < 0x100 ? (BYTE)s[i] : '?';
            Str t = to_text(a, n, FALSE, CP_ACP);
            set_text(h, &t, FALSE);
            xfree(a);
            return TRUE;
        }
        return edit(h, m, wp, lp);
    }
    case EM_STREAMIN: return stream_in(h, wp, (EDITSTREAM *)lp);
    case EM_STREAMOUT: return stream_out(h, wp, (EDITSTREAM *)lp);
    case EM_SETTEXTEX: {
        SETTEXTEX *st = (SETTEXTEX *)wp;
        DWORD flags = st ? st->flags : 0;
        UINT cp = st ? st->codepage : CP_ACP;
        Str t = { 0 };
        if (!lp) t.s = 0;
        else if (cp == 1200) t = to_text((const BYTE *)lp, lstrlenW((LPCWSTR)lp) * 2, TRUE, 0);
        else t = to_text((const BYTE *)lp, lstrlenA((LPCSTR)lp), FALSE, cp);
        set_text(h, &t, (flags & ST_SELECTION) != 0);
        return 1;
    }
    case EM_GETTEXTEX: {
        GETTEXTEX *gt = (GETTEXTEX *)wp;
        if (!gt || !lp || !gt->cb) return 0;
        int n;
        WCHAR *s = get_all(h, &n);
        if (!s) return 0;
        const WCHAR *from = s;
        if (gt->flags & GT_SELECTION) {
            DWORD lo = 0, hi = 0;
            edit(h, EM_GETSEL, (WPARAM)&lo, (LPARAM)&hi);
            from = s + lo;
            n = (int)(hi - lo);
        }
        /* without GT_USECRLF a Rich Edit control's lines end in "\r" alone */
        Str o = { 0 };
        for (int i = 0; i < n; i++) {
            if (from[i] == '\r' && i + 1 < n && from[i + 1] == '\n' && !(gt->flags & GT_USECRLF)) { put(&o, '\r'); i++; }
            else put(&o, from[i]);
        }
        LRESULT res;
        if (gt->codepage == 1200) {
            int max = (int)(gt->cb / sizeof(WCHAR)) - 1;
            int c = o.n < max ? o.n : max;
            if (c > 0) memcpy((void *)lp, o.s, (SIZE_T)c * sizeof(WCHAR));
            ((WCHAR *)lp)[c < 0 ? 0 : c] = 0;
            res = c < 0 ? 0 : c;
        } else {
            int c = o.n ? WideCharToMultiByte(gt->codepage, 0, o.s, o.n, (char *)lp, (int)gt->cb - 1,
                                              gt->lpDefaultChar, gt->lpUsedDefChar) : 0;
            ((char *)lp)[c] = 0;
            res = c;
        }
        xfree(o.s);
        xfree(s);
        return res;
    }
    case EM_GETTEXTLENGTHEX: {
        GETTEXTLENGTHEX *gl = (GETTEXTLENGTHEX *)wp;
        int n;
        WCHAR *s = get_all(h, &n);
        if (!s) return 0;
        int len = n;
        if (!gl || !(gl->flags & GT_USECRLF))                   /* lines counted with one character */
            for (int i = 0; i + 1 < n; i++) if (s[i] == '\r' && s[i + 1] == '\n') len--;
        xfree(s);
        return len;
    }
    case EM_EXGETSEL: {
        DWORD lo = 0, hi = 0;
        edit(h, EM_GETSEL, (WPARAM)&lo, (LPARAM)&hi);
        CHARRANGE *cr = (CHARRANGE *)lp;
        if (cr) { cr->cpMin = (LONG)lo; cr->cpMax = (LONG)hi; }
        return 0;
    }
    case EM_EXSETSEL: {
        CHARRANGE *cr = (CHARRANGE *)lp;
        if (!cr) return 0;
        edit(h, EM_SETSEL, (WPARAM)cr->cpMin, (LPARAM)cr->cpMax);
        DWORD lo = 0, hi = 0;
        edit(h, EM_GETSEL, (WPARAM)&lo, (LPARAM)&hi);
        return (LRESULT)hi;
    }
    case EM_EXLIMITTEXT: return edit(h, EM_LIMITTEXT, lp ? (WPARAM)lp : 0x7FFFFFFE, 0);
    case EM_EXLINEFROMCHAR: return edit(h, EM_LINEFROMCHAR, (WPARAM)lp, 0);
    case EM_GETTEXTRANGE: {
        TEXTRANGEW *tr = (TEXTRANGEW *)lp;
        if (!tr || !tr->lpstrText) return 0;
        int n;
        WCHAR *s = get_all(h, &n);
        if (!s) return 0;
        LONG lo = tr->chrg.cpMin < 0 ? 0 : tr->chrg.cpMin, hi = tr->chrg.cpMax < 0 || tr->chrg.cpMax > n ? n : tr->chrg.cpMax;
        int c = hi > lo ? (int)(hi - lo) : 0;
        if (r && r->ansi) {
            int b = c ? WideCharToMultiByte(CP_ACP, 0, s + lo, c, (char *)tr->lpstrText, c * 2 + 1, 0, 0) : 0;
            ((char *)tr->lpstrText)[b] = 0;
            c = b;
        } else {
            if (c) memcpy(tr->lpstrText, s + lo, (SIZE_T)c * sizeof(WCHAR));
            tr->lpstrText[c] = 0;
        }
        xfree(s);
        return c;
    }
    case EM_GETSELTEXT: {
        if (!lp) return 0;
        DWORD lo = 0, hi = 0;
        edit(h, EM_GETSEL, (WPARAM)&lo, (LPARAM)&hi);
        TEXTRANGEW tr = { { (LONG)lo, (LONG)hi }, (LPWSTR)lp };
        return RichProc(h, EM_GETTEXTRANGE, 0, (LPARAM)&tr);
    }
    case EM_SELECTIONTYPE: {
        DWORD lo = 0, hi = 0;
        edit(h, EM_GETSEL, (WPARAM)&lo, (LPARAM)&hi);
        return hi == lo ? SEL_EMPTY : hi - lo > 1 ? SEL_TEXT | SEL_MULTICHAR : SEL_TEXT;
    }
    case EM_FINDTEXT: case EM_FINDTEXTEX: case EM_FINDTEXTW: case EM_FINDTEXTEXW: {
        if (!lp) return -1;                       /* EM_FINDTEXT(EX): an ANSI string; the ...W ones UTF-16 */
        FINDTEXTEXW *ft = (FINDTEXTEXW *)lp;
        WCHAR *w = m == EM_FINDTEXT || m == EM_FINDTEXTEX ? a2w((const char *)ft->lpstrText) : 0;
        LONG res = find(h, wp, ft->chrg, w ? w : ft->lpstrText,
                        m == EM_FINDTEXTEX || m == EM_FINDTEXTEXW ? &ft->chrgText : 0);
        xfree(w);
        return res;
    }
    case EM_SETEVENTMASK: { DWORD old = r ? r->events : 0; if (r) r->events = (DWORD)lp; return old; }
    case EM_GETEVENTMASK: return r ? r->events : 0;
    case EM_SETOPTIONS: {
        if (!r) return 0;
        if (wp == 1) r->options = (DWORD)lp;      /* ECOOP_SET */
        else if (wp == 2) r->options |= (DWORD)lp;   /* ECOOP_OR */
        else if (wp == 3) r->options &= (DWORD)lp;   /* ECOOP_AND */
        else if (wp == 4) r->options ^= (DWORD)lp;   /* ECOOP_XOR */
        if (lp & 0x800) edit(h, EM_SETREADONLY, (r->options & 0x800) != 0, 0);   /* ECO_READONLY */
        return r->options;
    }
    case EM_GETOPTIONS: return r ? r->options : 0;
    case EM_SETBKGNDCOLOR: {
        COLORREF old = r ? r->bk : 0;
        if (r) r->bk = wp ? GetSysColor(COLOR_WINDOW) : (COLORREF)lp;
        InvalidateRect(h, 0, TRUE);
        return old;
    }
    case EM_SETTEXTMODE: if (r) r->textmode = (LONG)wp; return 0;
    case EM_GETTEXTMODE: return r ? r->textmode : 1;
    case EM_AUTOURLDETECT: if (r) r->urls = (LONG)wp; return 0;
    case EM_GETAUTOURLDETECT: return r ? r->urls : 0;
    case EM_SETLANGOPTIONS: if (r) r->langopts = (DWORD)lp; return 1;
    case EM_GETLANGOPTIONS: return r ? r->langopts : 0;
    case EM_SETEDITSTYLE: if (r) r->editstyle = (r->editstyle & ~(DWORD)lp) | ((DWORD)wp & (DWORD)lp); return r ? r->editstyle : 0;
    case EM_GETEDITSTYLE: return r ? r->editstyle : 0;
    case EM_SETCHARFORMAT: case EM_SETPARAFORMAT: case EM_SETTARGETDEVICE: case EM_SETFONTSIZE:
        return 1;                                 /* accepted; drawn in the control's one font */
    case EM_GETCHARFORMAT: case EM_GETPARAFORMAT:
        return 0;
    case EM_GETOLEINTERFACE: if (lp) *(void **)lp = 0; return 0;
    case EM_SETOLECALLBACK: return 1;
    case EM_HIDESELECTION: return 0;
    case EM_REQUESTRESIZE: case EM_STOPGROUPTYPING: case EM_SHOWSCROLLBAR: return 0;
    case EM_SETUNDOLIMIT: return (LRESULT)wp;
    case EM_REDO: case EM_CANREDO: return 0;
    case EM_GETZOOM: if (wp) *(int *)wp = 0; if (lp) *(int *)lp = 0; return TRUE;
    case EM_SETZOOM: return TRUE;
    case EM_GETSCROLLPOS: {
        POINT *pt = (POINT *)lp;
        if (pt) { pt->x = 0; pt->y = (LONG)edit(h, EM_GETFIRSTVISIBLELINE, 0, 0); }
        return 1;
    }
    case EM_SETSCROLLPOS: {
        POINT *pt = (POINT *)lp;
        if (pt) edit(h, EM_LINESCROLL, 0, pt->y - (LONG)edit(h, EM_GETFIRSTVISIBLELINE, 0, 0));
        return 1;
    }
    }
    return edit(h, m, wp, lp);
}

static void register_classes(HINSTANCE inst)
{
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    if (!GetClassInfoExW(0, L"EDIT", &wc)) return;
    g_edit = wc.lpfnWndProc;
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = RichProc;
    wc.hInstance = inst;
    wc.style |= CS_GLOBALCLASS;
    wc.lpszClassName = RICHEDIT_CLASS_W;
    RegisterClassExW(&wc);
#ifdef RICHEDIT_CLASS_A
    wc.lpszClassName = RICHEDIT_CLASS_A;
    RegisterClassExW(&wc);
#endif
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD why, LPVOID reserved)
{
    (void)reserved;
    if (why == DLL_PROCESS_ATTACH) register_classes(inst);
    return TRUE;
}

/* What riched20.dll exports besides its classes */
__declspec(dllexport) HRESULT WINAPI CreateTextServices(void *outer, void *host, void **out)
{
    (void)outer; (void)host;
    if (out) *out = 0;
    return E_NOTIMPL;                             /* windowless text services: not provided */
}

__declspec(dllexport) HRESULT WINAPI DllGetVersion(DWORD *info)
{
    if (!info || info[0] < 20) return E_INVALIDARG;   /* DLLVERSIONINFO: cbSize, major, minor, build, platform */
    info[1] = 5; info[2] = 31; info[3] = 23; info[4] = 2;
    return S_OK;
}
