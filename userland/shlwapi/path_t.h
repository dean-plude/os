/*
 * path_t.h — shlwapi's path and string functions, written once for both
 * character types.  Included by path.c with T (char or WCHAR), F(name)
 * (nameA or nameW) and a few helpers defined.
 */

static int F(len_)(const T *s) { int n = 0; if (s) while (s[n]) n++; return n; }
static T F(lower_)(T c) { return (T)((c >= 'A' && c <= 'Z') ? c + 32 : (sizeof(T) > 1 ? (T)CharLowerOne((WCHAR)c) : c)); }
static int F(is_sep_)(T c) { return c == '\\' || c == '/'; }
static void F(copy_)(T *d, const T *s) { while ((*d++ = *s++)) ; }
static void F(move_)(T *d, const T *s) { int n = F(len_)(s); for (int i = 0; i <= n; i++) d[i] = s[i]; }

/* ---- classification ---- */
LWSTDAPI_(BOOL) F(PathIsUNC)(const T *p) { return p && p[0] == '\\' && p[1] == '\\' && p[2] != '?'; }
/* A network path: a UNC one (NovaOS maps no network drives) */
LWSTDAPI_(BOOL) F(PathIsNetworkPath)(const T *p) { return F(PathIsUNC)(p); }

LWSTDAPI_(BOOL) F(PathIsRelative)(const T *p)
{
    if (!p || !*p) return TRUE;
    if (p[0] == '\\' || p[0] == '/') return FALSE;
    if (p[0] && p[1] == ':') return FALSE;
    return TRUE;
}

LWSTDAPI_(BOOL) F(PathIsRoot)(const T *p)
{
    if (!p || !*p) return FALSE;
    if ((p[0] == '\\' || p[0] == '/') && !p[1]) return TRUE;
    if (p[0] && p[1] == ':' && (p[2] == '\\' || p[2] == '/') && !p[3]) return TRUE;
    if (F(PathIsUNC)(p)) {                              /* "\\server\share" */
        int seps = 0;
        for (const T *c = p + 2; *c; c++) if (F(is_sep_)(*c) && c[1]) seps++;
        return seps <= 1;
    }
    return FALSE;
}

LWSTDAPI_(int) F(PathGetDriveNumber)(const T *p)
{
    if (!p || !p[0] || p[1] != ':') return -1;
    T c = F(lower_)(p[0]);
    return c >= 'a' && c <= 'z' ? c - 'a' : -1;
}

LWSTDAPI_(BOOL) F(PathIsFileSpec)(const T *p)
{
    for (; p && *p; p++) if (F(is_sep_)(*p) || *p == ':') return FALSE;
    return TRUE;
}

LWSTDAPI_(BOOL) F(PathIsURL)(const T *p)
{
    if (!p) return FALSE;
    int i = 0;
    while ((p[i] >= 'a' && p[i] <= 'z') || (p[i] >= 'A' && p[i] <= 'Z') || (p[i] >= '0' && p[i] <= '9') || p[i] == '+' || p[i] == '-' || p[i] == '.') i++;
    return i > 1 && p[i] == ':';
}

/* ---- components ---- */
LWSTDAPI_(T *) F(PathFindFileName)(const T *p)
{
    const T *last = p;
    for (const T *c = p; c && *c; c++)
        if ((F(is_sep_)(*c) || *c == ':') && c[1] && !F(is_sep_)(c[1])) last = c + 1;
    return (T *)last;
}

LWSTDAPI_(T *) F(PathFindExtension)(const T *p)
{
    const T *dot = 0, *c = p;
    for (; c && *c; c++) {
        if (F(is_sep_)(*c) || *c == ' ') dot = 0;
        else if (*c == '.') dot = c;
    }
    return (T *)(dot ? dot : c);
}

LWSTDAPI_(T *) F(PathSkipRoot)(const T *p)
{
    if (!p) return 0;
    if (F(PathIsUNC)(p)) {
        const T *c = p + 2;
        int seps = 0;
        while (*c && seps < 2) { if (F(is_sep_)(*c)) seps++; c++; }
        return (T *)c;
    }
    if (p[0] && p[1] == ':' && F(is_sep_)(p[2])) return (T *)(p + 3);
    if (F(is_sep_)(p[0])) return (T *)(p + 1);
    return 0;
}

LWSTDAPI_(T *) F(PathFindNextComponent)(const T *p)
{
    if (!p || !*p) return 0;
    while (*p && !F(is_sep_)(*p)) p++;
    if (*p) p++;
    return (T *)p;
}

LWSTDAPI_(T *) F(PathGetArgs)(const T *p)
{
    BOOL quoted = FALSE;
    for (; p && *p; p++) {
        if (*p == '"') quoted = !quoted;
        else if (*p == ' ' && !quoted) return (T *)(p + 1);
    }
    return (T *)p;
}

LWSTDAPI_(void) F(PathRemoveArgs)(T *p)
{
    T *a = F(PathGetArgs)(p);
    if (*a) { a--; }
    *a = 0;
    int n = F(len_)(p);
    while (n && p[n - 1] == ' ') p[--n] = 0;
}

/* ---- editing in place ---- */
LWSTDAPI_(BOOL) F(PathRemoveFileSpec)(T *p)
{
    if (!p || !*p) return FALSE;
    T *last = 0, *root_end = F(PathSkipRoot)(p);
    for (T *c = p; *c; c++) if (F(is_sep_)(*c)) last = c;
    if (!last) {                                        /* "file" or "C:file" */
        if (p[0] && p[1] == ':') { if (!p[2]) return FALSE; p[2] = 0; return TRUE; }
        *p = 0;
        return TRUE;
    }
    if (root_end && last < root_end) {                  /* keep the root's separator */
        if (!*root_end) return FALSE;
        *root_end = 0;
        return TRUE;
    }
    *last = 0;
    return TRUE;
}

LWSTDAPI_(void) F(PathStripPath)(T *p) { F(move_)(p, F(PathFindFileName)(p)); }

LWSTDAPI_(BOOL) F(PathStripToRoot)(T *p)
{
    T *r = F(PathSkipRoot)(p);
    if (!r) { *p = 0; return FALSE; }
    *r = 0;
    return TRUE;
}

LWSTDAPI_(void) F(PathRemoveExtension)(T *p) { *F(PathFindExtension)(p) = 0; }

LWSTDAPI_(BOOL) F(PathRenameExtension)(T *p, const T *ext)
{
    T *e = F(PathFindExtension)(p);
    if ((e - p) + F(len_)(ext) >= MAX_PATH) return FALSE;
    F(copy_)(e, ext);
    return TRUE;
}

LWSTDAPI_(BOOL) F(PathAddExtension)(T *p, const T *ext)
{
    if (*F(PathFindExtension)(p)) return FALSE;
    static const T exe[] = { '.', 'e', 'x', 'e', 0 };
    if (!ext) ext = exe;
    int n = F(len_)(p);
    if (n + F(len_)(ext) >= MAX_PATH) return FALSE;
    F(copy_)(p + n, ext);
    return TRUE;
}

LWSTDAPI_(T *) F(PathAddBackslash)(T *p)
{
    int n = F(len_)(p);
    if (n && !F(is_sep_)(p[n - 1])) {
        if (n >= MAX_PATH - 1) return 0;
        p[n++] = '\\';
        p[n] = 0;
    }
    return p + n;
}

LWSTDAPI_(T *) F(PathRemoveBackslash)(T *p)
{
    int n = F(len_)(p);
    if (n && p[n - 1] == '\\' && !F(PathIsRoot)(p)) { p[--n] = 0; return p + n; }
    return p + (n ? n - 1 : 0);
}

LWSTDAPI_(void) F(PathRemoveBlanks)(T *p)
{
    int s = 0, n = F(len_)(p);
    while (p[s] == ' ') s++;
    while (n > s && p[n - 1] == ' ') n--;
    for (int i = s; i < n; i++) p[i - s] = p[i];
    p[n - s] = 0;
}

LWSTDAPI_(BOOL) F(PathQuoteSpaces)(T *p)
{
    int n = F(len_)(p), sp = 0;
    for (int i = 0; i < n; i++) if (p[i] == ' ') sp = 1;
    if (!sp || n + 3 > MAX_PATH) return FALSE;
    for (int i = n; i >= 0; i--) p[i + 1] = p[i];
    p[0] = '"';
    p[n + 1] = '"';
    p[n + 2] = 0;
    return TRUE;
}

LWSTDAPI_(void) F(PathUnquoteSpaces)(T *p)
{
    int n = F(len_)(p);
    if (n >= 2 && p[0] == '"' && p[n - 1] == '"') {
        for (int i = 1; i < n - 1; i++) p[i - 1] = p[i];
        p[n - 2] = 0;
    }
}

/* Resolve "." and ".." (no file system access) */
LWSTDAPI_(BOOL) F(PathCanonicalize)(T *out, const T *in)
{
    if (!out || !in) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    T tmp[MAX_PATH * 2];
    int n = F(len_)(in);
    if (n >= MAX_PATH) return FALSE;
    const T *root = F(PathSkipRoot)(in);
    int rl = root ? (int)(root - in) : 0, o = 0;
    for (int i = 0; i < rl; i++) tmp[o++] = in[i] == '/' ? '\\' : in[i];
    int base = o;
    for (int i = rl; i < n; ) {
        int s = i;
        while (i < n && !F(is_sep_)(in[i])) i++;
        int len = i - s;
        if (i < n) i++;                                 /* the separator */
        if (!len || (len == 1 && in[s] == '.')) continue;
        if (len == 2 && in[s] == '.' && in[s + 1] == '.') {
            while (o > base && tmp[o - 1] != '\\') o--;   /* drop the last component */
            if (o > base) o--;                          /* and its separator */
            continue;
        }
        if (o > base) tmp[o++] = '\\';
        for (int k = 0; k < len; k++) tmp[o++] = in[s + k];
    }
    if (n && F(is_sep_)(in[n - 1]) && o > base) tmp[o++] = '\\';    /* keep a trailing separator */
    if (!o) tmp[o++] = '\\';
    tmp[o] = 0;
    F(copy_)(out, tmp);
    return TRUE;
}

LWSTDAPI_(T *) F(PathCombine)(T *out, const T *dir, const T *file)
{
    T tmp[MAX_PATH * 2];
    if (!out || (!dir && !file)) return 0;
    if (!file || !*file) {
        F(copy_)(tmp, dir ? dir : file);
    } else if (!dir || !*dir || !F(PathIsRelative)(file)) {
        if (dir && dir[0] && dir[1] == ':' && F(is_sep_)(file[0]) && !F(is_sep_)(file[1])) {
            tmp[0] = dir[0]; tmp[1] = ':';
            F(copy_)(tmp + 2, file);                    /* "\x" on dir's drive */
        } else {
            F(copy_)(tmp, file);
        }
    } else {
        int n = F(len_)(dir);
        if (n + F(len_)(file) + 2 >= MAX_PATH * 2) return 0;
        F(copy_)(tmp, dir);
        if (n && !F(is_sep_)(tmp[n - 1])) tmp[n++] = '\\';
        F(copy_)(tmp + n, file);
    }
    if (F(len_)(tmp) >= MAX_PATH) { *out = 0; return 0; }
    F(PathCanonicalize)(out, tmp);
    return out;
}

LWSTDAPI_(BOOL) F(PathAppend)(T *p, const T *more)
{
    if (!p || !more) return FALSE;
    while (F(is_sep_)(*more)) more++;
    T tmp[MAX_PATH];
    if (!F(PathCombine)(tmp, p, more)) return FALSE;
    F(copy_)(p, tmp);
    return TRUE;
}

static BOOL F(PathBuildRoot_)(T *out, int drive)
{
    if (drive < 0 || drive > 25) { *out = 0; return FALSE; }
    out[0] = (T)('A' + drive); out[1] = ':'; out[2] = '\\'; out[3] = 0;
    return TRUE;
}

LWSTDAPI_(int) F(PathCommonPrefix)(const T *a, const T *b, T *out)
{
    int i = 0, last = 0;
    while (a[i] && b[i] && F(lower_)(a[i]) == F(lower_)(b[i])) {
        if (F(is_sep_)(a[i])) last = i;
        i++;
    }
    if (!a[i] && (!b[i] || F(is_sep_)(b[i]))) last = i;
    else if (!b[i] && F(is_sep_)(a[i])) last = i;
    if (last == 2 && a[1] == ':') last = 3;             /* "C:\" */
    if (out) { for (int k = 0; k < last; k++) out[k] = a[k]; out[last] = 0; }
    return last;
}

LWSTDAPI_(BOOL) F(PathIsPrefix)(const T *prefix, const T *path)
{
    return F(PathCommonPrefix)(prefix, path, 0) == F(len_)(prefix);
}

LWSTDAPI_(BOOL) F(PathIsSameRoot)(const T *a, const T *b)
{
    T ra[MAX_PATH], rb[MAX_PATH];
    F(copy_)(ra, a); F(copy_)(rb, b);
    F(PathStripToRoot)(ra); F(PathStripToRoot)(rb);
    int i = 0;
    while (ra[i] && F(lower_)(ra[i]) == F(lower_)(rb[i])) i++;
    return !ra[i] && !rb[i];
}

/* ---- wildcards ---- */
static BOOL F(match_)(const T *pat, const T *s)
{
    for (;;) {
        if (!*pat || *pat == ';') return !*s;
        if (*pat == '*') {
            for (;; s++) { if (F(match_)(pat + 1, s)) return TRUE; if (!*s) return FALSE; }
        }
        if (!*s) return FALSE;
        if (*pat != '?' && F(lower_)(*pat) != F(lower_)(*s)) return FALSE;
        pat++; s++;
    }
}

LWSTDAPI_(BOOL) F(PathMatchSpec)(const T *file, const T *spec)
{
    static const T all[] = { '*', '.', '*', 0 };
    const T *name = F(PathFindFileName)(file);
    for (const T *p = spec; p && *p; ) {
        while (*p == ' ' || *p == ';') p++;
        if (!*p) break;
        int n = 0;
        while (p[n] && p[n] != ';') n++;
        if ((n == 3 && p[0] == all[0] && p[1] == all[1] && p[2] == all[2]) || F(match_)(p, name)) return TRUE;
        p += n;
    }
    return FALSE;
}

/* ---- the file system ---- */
LWSTDAPI_(BOOL) F(PathIsDirectoryEmpty)(const T *p)
{
    T pat[MAX_PATH + 4];
    int n = F(len_)(p);
    if (n > MAX_PATH - 3) return FALSE;
    F(copy_)(pat, p);
    if (n && !F(is_sep_)(pat[n - 1])) pat[n++] = '\\';
    pat[n++] = '*'; pat[n] = 0;
    FIND_T fd;
    HANDLE h = FIND_FIRST(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return TRUE;
    BOOL empty = TRUE;
    do {
        const T *c = fd.cFileName;
        if (!(c[0] == '.' && (!c[1] || (c[1] == '.' && !c[2])))) { empty = FALSE; break; }
    } while (FIND_NEXT(h, &fd));
    FindClose(h);
    return empty;
}

/* ---- strings ---- */
LWSTDAPI_(T *) F(StrChr)(const T *s, T c)       { for (; s && *s; s++) if (*s == c) return (T *)s; return 0; }
LWSTDAPI_(T *) F(StrChrI)(const T *s, T c)      { for (; s && *s; s++) if (F(lower_)(*s) == F(lower_)(c)) return (T *)s; return 0; }
LWSTDAPI_(T *) F(StrRChr)(const T *s, const T *end, T c)
{
    const T *r = 0;
    for (; s && *s && (!end || s < end); s++) if (*s == c) r = s;
    return (T *)r;
}
LWSTDAPI_(T *) F(StrRChrI)(const T *s, const T *end, T c)
{
    const T *r = 0;
    for (; s && *s && (!end || s < end); s++) if (F(lower_)(*s) == F(lower_)(c)) r = s;
    return (T *)r;
}

static int F(cmp_)(const T *a, const T *b, int n, BOOL fold)
{
    for (int i = 0; n < 0 || i < n; i++) {
        T x = fold ? F(lower_)(a[i]) : a[i], y = fold ? F(lower_)(b[i]) : b[i];
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
    return 0;
}

LWSTDAPI_(int) F(StrCmp)(const T *a, const T *b)             { return F(cmp_)(a, b, -1, FALSE); }
LWSTDAPI_(int) F(StrCmpI)(const T *a, const T *b)            { return F(cmp_)(a, b, -1, TRUE); }
LWSTDAPI_(int) F(StrCmpN)(const T *a, const T *b, int n)     { return F(cmp_)(a, b, n, FALSE); }
LWSTDAPI_(int) F(StrCmpNI)(const T *a, const T *b, int n)    { return F(cmp_)(a, b, n, TRUE); }
LWSTDAPI_(BOOL) F(StrIsIntlEqual)(BOOL cs, const T *a, const T *b, int n) { return !F(cmp_)(a, b, n, !cs); }
LWSTDAPI_(BOOL) F(ChrCmpI)(T a, T b)                         { return F(lower_)(a) != F(lower_)(b); }

LWSTDAPI_(T *) F(StrStr)(const T *h, const T *n)
{
    int nl = F(len_)(n);
    for (; h && *h; h++) if (!F(cmp_)(h, n, nl, FALSE)) return (T *)h;
    return nl ? 0 : (T *)h;
}

LWSTDAPI_(T *) F(StrStrI)(const T *h, const T *n)
{
    int nl = F(len_)(n);
    for (; h && *h; h++) if (!F(cmp_)(h, n, nl, TRUE)) return (T *)h;
    return nl ? 0 : (T *)h;
}

LWSTDAPI_(T *) F(StrPBrk)(const T *s, const T *set)
{
    for (; s && *s; s++) for (const T *k = set; *k; k++) if (*s == *k) return (T *)s;
    return 0;
}

LWSTDAPI_(int) F(StrSpn)(const T *s, const T *set)
{
    int n = 0;
    for (; s[n]; n++) { const T *k = set; while (*k && *k != s[n]) k++; if (!*k) break; }
    return n;
}

LWSTDAPI_(int) F(StrCSpn)(const T *s, const T *set)
{
    int n = 0;
    for (; s[n]; n++) { const T *k = set; while (*k && *k != s[n]) k++; if (*k) break; }
    return n;
}

LWSTDAPI_(T *) F(StrCpyN)(T *d, const T *s, int n)
{
    int i = 0;
    for (; i < n - 1 && s[i]; i++) d[i] = s[i];
    if (n > 0) d[i] = 0;
    return d;
}

LWSTDAPI_(T *) F(StrNCat)(T *d, const T *s, int n)
{
    int dl = F(len_)(d), i = 0;
    for (; i < n - 1 && s[i]; i++) d[dl + i] = s[i];
    d[dl + i] = 0;
    return d;
}

LWSTDAPI_(T *) F(StrCatBuff)(T *d, const T *s, int cap)
{
    int dl = F(len_)(d);
    if (dl < cap) F(StrCpyN)(d + dl, s, cap - dl);
    return d;
}

LWSTDAPI_(T *) F(StrDup)(const T *s)
{
    int n = F(len_)(s);
    T *d = LocalAlloc(0, sizeof(T) * ((SIZE_T)n + 1));
    if (d) F(copy_)(d, s);
    return d;
}

LWSTDAPI_(BOOL) F(StrToIntEx)(const T *s, DWORD flags, int *out)
{
    while (*s == ' ' || *s == '\t') s++;
    BOOL neg = FALSE;
    if (*s == '-' || *s == '+') neg = *s++ == '-';
    unsigned v = 0;
    if ((flags & 1 /* STIF_SUPPORT_HEX */) && s[0] == '0' && (s[1] | 0x20) == 'x') {
        s += 2;
        const T *st = s;
        for (;; s++) {
            T c = F(lower_)(*s);
            if (c >= '0' && c <= '9') v = v * 16 + (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v = v * 16 + (unsigned)(c - 'a' + 10);
            else break;
        }
        if (s == st) return FALSE;
    } else {
        const T *st = s;
        while (*s >= '0' && *s <= '9') v = v * 10 + (unsigned)(*s++ - '0');
        if (s == st) return FALSE;
    }
    *out = neg ? -(int)v : (int)v;
    return TRUE;
}

LWSTDAPI_(int) F(StrToInt)(const T *s) { int v = 0; F(StrToIntEx)(s, 0, &v); return v; }

LWSTDAPI_(BOOL) F(StrTrim)(T *s, const T *chars)
{
    int n = F(len_)(s), a = 0;
    while (a < n && F(StrChr)(chars, s[a])) a++;
    while (n > a && F(StrChr)(chars, s[n - 1])) n--;
    BOOL changed = a || s[n];
    for (int i = a; i < n; i++) s[i - a] = s[i];
    s[n - a] = 0;
    return changed;
}

/* "123 bytes", "1.20 KB", ... (three significant digits, like Explorer) */
static T *F(format_size_)(ULONGLONG v, T *buf, UINT cap)
{
    static const char *units[] = { "bytes", "KB", "MB", "GB", "TB", "PB", "EB" };
    char tmp[40];
    int o = 0;
    if (v < 1024) {
        char d[24]; int k = 0;
        ULONGLONG x = v;
        do { d[k++] = (char)('0' + x % 10); x /= 10; } while (x);
        while (k) tmp[o++] = d[--k];
        tmp[o++] = ' ';
        for (const char *u = units[0]; *u; u++) tmp[o++] = *u;
    } else {
        int u = 0;
        ULONGLONG scaled = v * 100;                     /* hundredths of the unit */
        while (scaled >= 1024 * 100 && u < 6) { scaled /= 1024; u++; }
        /* keep three significant digits */
        ULONGLONG whole = scaled / 100, frac = scaled % 100;
        char d[24]; int k = 0;
        ULONGLONG x = whole;
        do { d[k++] = (char)('0' + x % 10); x /= 10; } while (x);
        int digits = k;
        while (k) tmp[o++] = d[--k];
        if (digits < 3) {
            tmp[o++] = '.';
            tmp[o++] = (char)('0' + frac / 10);
            if (digits < 2) tmp[o++] = (char)('0' + frac % 10);
        }
        tmp[o++] = ' ';
        for (const char *s = units[u]; *s; s++) tmp[o++] = *s;
    }
    tmp[o] = 0;
    UINT i = 0;
    for (; i + 1 < cap && tmp[i]; i++) buf[i] = (T)tmp[i];
    if (cap) buf[i] = 0;
    return buf;
}
