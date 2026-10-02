/* msvcrt / ucrtbase: what compiler-generated startup code and the UCRT's
 * header inlines call
 *
 * Programs built by MSVC link a static startup (vcruntime's mainCRTStartup)
 * that calls the UCRT's _configure_narrow_argv, _initterm, __p___argc, ...;
 * MinGW programs call msvcrt's __getmainargs, _initterm, __set_app_type,
 * ...; and the UCRT's printf family is a set of __stdio_common_* functions
 * that take an options word.  Both DLLs are built from the same sources, so
 * each program finds what its toolchain expects.
 */
#define NOVA_BUILD_MSVCRT
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>
#include <windows.h>
#include "msvcrt_internal.h"

#define CRTEXP __declspec(dllexport)

/* -----------------------------------------------------------------------
 * Command line and environment
 * ----------------------------------------------------------------------- */
CRTEXP int       __argc;
CRTEXP char    **__argv;
CRTEXP wchar_t **__wargv;
CRTEXP char    **_environ;
CRTEXP wchar_t **_wenviron;
CRTEXP char    **__initenv;
CRTEXP wchar_t **__winitenv;
CRTEXP char     *_acmdln;
CRTEXP wchar_t  *_wcmdln;
CRTEXP char     *_pgmptr;
CRTEXP wchar_t  *_wpgmptr;
CRTEXP int       _fmode;
CRTEXP int       _commode;
CRTEXP unsigned  __mb_cur_max = 4;

/* Split a command line the way Microsoft's CRT does: whitespace separates
 * arguments; "..." groups; 2n backslashes + " give n backslashes and a
 * quote toggle, 2n+1 give n and a literal quote. */
#define SPLIT(name, CH)                                                        \
static CH **name(const CH *cl, int *argc)                                      \
{                                                                              \
    size_t len = 0;                                                            \
    while (cl[len]) len++;                                                     \
    CH *buf = malloc((len + 1) * sizeof(CH));                                  \
    CH **av = malloc(sizeof(CH *) * (len / 2 + 2));                            \
    if (!buf || !av) { free(buf); free(av); *argc = 0; return NULL; }          \
    int n = 0;                                                                 \
    CH *o = buf;                                                               \
    const CH *p = cl;                                                          \
    while (*p) {                                                               \
        while (*p == ' ' || *p == '\t') p++;                                   \
        if (!*p) break;                                                        \
        av[n++] = o;                                                           \
        int quoted = 0;                                                        \
        while (*p && (quoted || (*p != ' ' && *p != '\t'))) {                  \
            int bs = 0;                                                        \
            while (*p == '\\') { bs++; p++; }                                  \
            if (*p == '"') {                                                   \
                for (int i = 0; i < bs / 2; i++) *o++ = '\\';                  \
                if (bs % 2) *o++ = '"';                                        \
                else if (quoted && p[1] == '"') { *o++ = '"'; p++; }           \
                else quoted = !quoted;                                         \
                p++;                                                           \
            } else {                                                           \
                for (int i = 0; i < bs; i++) *o++ = '\\';                      \
                if (*p && (quoted || (*p != ' ' && *p != '\t'))) *o++ = *p++;  \
            }                                                                  \
        }                                                                      \
        *o++ = 0;                                                              \
    }                                                                          \
    av[n] = NULL;                                                              \
    *argc = n;                                                                 \
    return av;                                                                 \
}
SPLIT(split_a, char)
SPLIT(split_w, wchar_t)

static void init_args(void)
{
    if (__argv) return;
    _acmdln = GetCommandLineA();
    _wcmdln = GetCommandLineW();
    __argv = split_a(_acmdln, &__argc);
    int wc;
    __wargv = split_w(_wcmdln, &wc);
    static char path[MAX_PATH];
    static wchar_t wpath[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    GetModuleFileNameW(NULL, wpath, MAX_PATH);
    _pgmptr = path;
    _wpgmptr = wpath;
}

/* The environment as the program sees it: built once from the process's
 * environment block (UTF-16), in both encodings */
static void init_env(void)
{
    if (_environ) return;
    wchar_t *blk = GetEnvironmentStringsW();
    int n = 0;
    for (wchar_t *p = blk; p && *p; p += wcslen(p) + 1) n++;
    _environ = calloc((size_t)n + 1, sizeof(char *));
    _wenviron = calloc((size_t)n + 1, sizeof(wchar_t *));
    if (!_environ || !_wenviron) return;
    int i = 0;
    for (wchar_t *p = blk; p && *p && i < n; p += wcslen(p) + 1, i++) {
        _wenviron[i] = _wcsdup(p);
        size_t len = wcstombs(NULL, p, 0);
        _environ[i] = len == (size_t)-1 ? NULL : malloc(len + 1);
        if (_environ[i]) wcstombs(_environ[i], p, len + 1);
        else _environ[i] = _strdup("");
    }
    if (blk) FreeEnvironmentStringsW(blk);
    __initenv = _environ;
    __winitenv = _wenviron;
}

CRTEXP int __wgetmainargs(int *argc, wchar_t ***argv, wchar_t ***envp, int glob, void *si)
{
    __iob_func();
    (void)glob; (void)si;
    init_args();
    init_env();
    int n;
    *argv = split_w(GetCommandLineW(), &n);
    *argc = n;
    if (envp) *envp = _wenviron;
    return 0;
}

/* __getmainargs lives in stdlib.c; it also fills these */
void __nova_note_args(void) { init_args(); init_env(); }

CRTEXP int      *__p___argc(void)    { init_args(); return &__argc; }
CRTEXP char   ***__p___argv(void)    { init_args(); return &__argv; }
CRTEXP wchar_t ***__p___wargv(void)  { init_args(); return &__wargv; }
CRTEXP char   ***__p__environ(void)  { init_env(); return &_environ; }
CRTEXP wchar_t ***__p__wenviron(void) { init_env(); return &_wenviron; }
CRTEXP char   ***__p___initenv(void) { init_env(); return &__initenv; }
CRTEXP wchar_t ***__p___winitenv(void) { init_env(); return &__winitenv; }
CRTEXP char    **__p__acmdln(void)   { init_args(); return &_acmdln; }
CRTEXP wchar_t **__p__wcmdln(void)   { init_args(); return &_wcmdln; }
CRTEXP char    **__p__pgmptr(void)   { init_args(); return &_pgmptr; }
CRTEXP wchar_t **__p__wpgmptr(void)  { init_args(); return &_wpgmptr; }
CRTEXP int       _get_pgmptr(char **p)     { init_args(); *p = _pgmptr; return 0; }
CRTEXP int       _get_wpgmptr(wchar_t **p) { init_args(); *p = _wpgmptr; return 0; }
CRTEXP int      *__p__fmode(void)    { return &_fmode; }
CRTEXP int      *__p__commode(void)  { return &_commode; }
CRTEXP int       _set_fmode(int m)   { _fmode = m; return 0; }
CRTEXP int       _get_fmode(int *m)  { *m = _fmode; return 0; }
CRTEXP int       _configure_narrow_argv(int mode) { (void)mode; init_args(); return 0; }
CRTEXP int       _configure_wide_argv(int mode)   { (void)mode; init_args(); return 0; }
CRTEXP int       _initialize_narrow_environment(void) { init_env(); return 0; }
CRTEXP int       _initialize_wide_environment(void)   { init_env(); return 0; }
CRTEXP char    **_get_initial_narrow_environment(void) { init_env(); return _environ; }
CRTEXP wchar_t **_get_initial_wide_environment(void)   { init_env(); return _wenviron; }
CRTEXP char     *_get_narrow_winmain_command_line(void)
{
    init_args();
    char *p = _acmdln;                       /* skip the program name */
    int q = 0;
    while (*p && (q || (*p != ' ' && *p != '\t'))) { if (*p == '"') q = !q; p++; }
    while (*p == ' ' || *p == '\t') p++;
    return p;
}
CRTEXP wchar_t  *_get_wide_winmain_command_line(void)
{
    init_args();
    wchar_t *p = _wcmdln;
    int q = 0;
    while (*p && (q || (*p != ' ' && *p != '\t'))) { if (*p == '"') q = !q; p++; }
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

CRTEXP unsigned *__p___mb_cur_max(void) { return &__mb_cur_max; }
CRTEXP int ___mb_cur_max_func(void) { return (int)__mb_cur_max; }
CRTEXP unsigned ___lc_codepage_func(void) { return 65001; }           /* UTF-8 */
CRTEXP unsigned ___lc_collate_cp_func(void) { return 65001; }
CRTEXP wchar_t **___lc_locale_name_func(void) { static wchar_t *names[6]; return names; }

/* -----------------------------------------------------------------------
 * Initializers and exit handlers
 * ----------------------------------------------------------------------- */
typedef void (__cdecl *PVFV)(void);
typedef int  (__cdecl *PIFV)(void);

CRTEXP void _initterm(PVFV *a, PVFV *b)
{
    for (; a < b; a++) if (*a) (*a)();
}
CRTEXP int _initterm_e(PIFV *a, PIFV *b)
{
    for (; a < b; a++) if (*a) { int r = (*a)(); if (r) return r; }
    return 0;
}

typedef struct { PVFV *first, *last, *end; } onexit_table_t;

CRTEXP int _initialize_onexit_table(onexit_table_t *t)
{
    if (!t) return -1;
    t->first = t->last = t->end = NULL;
    return 0;
}
CRTEXP int _register_onexit_function(onexit_table_t *t, PVFV fn)
{
    if (!t) return -1;
    if (t->last == t->end) {
        size_t n = (size_t)(t->end - t->first), cap = n ? n * 2 : 32;
        PVFV *p = realloc(t->first, cap * sizeof(PVFV));
        if (!p) return -1;
        t->first = p; t->last = p + n; t->end = p + cap;
    }
    *t->last++ = fn;
    return 0;
}
CRTEXP int _execute_onexit_table(onexit_table_t *t)
{
    if (!t || !t->first) return 0;
    PVFV *first = t->first, *last = t->last;
    t->first = t->last = t->end = NULL;
    while (last > first) { PVFV f = *--last; if (f) f(); }
    free(first);
    return 0;
}

/* The process's own table (atexit, _onexit, _crt_atexit), run by exit */
static onexit_table_t g_exit_table, g_quick_table;
static int g_exit_lock;
void __nova_run_atexit(void)
{
    if (__atomic_exchange_n(&g_exit_lock, 1, __ATOMIC_ACQ_REL)) return;
    _execute_onexit_table(&g_exit_table);
}
CRTEXP int _crt_atexit(PVFV fn) { return _register_onexit_function(&g_exit_table, fn); }
CRTEXP int _crt_at_quick_exit(PVFV fn) { return _register_onexit_function(&g_quick_table, fn); }
CRTEXP int at_quick_exit(PVFV fn) { return _crt_at_quick_exit(fn); }
typedef int (__cdecl *onexit_t)(void);
CRTEXP onexit_t _onexit(onexit_t fn) { return _register_onexit_function(&g_exit_table, (PVFV)fn) ? NULL : fn; }
CRTEXP onexit_t __dllonexit(onexit_t fn, PVFV **begin, PVFV **end)
{
    onexit_table_t t = { *begin, *end, *end };
    if (_register_onexit_function(&t, (PVFV)fn)) return NULL;
    *begin = t.first; *end = t.last;
    return fn;
}
CRTEXP __declspec(noreturn) void quick_exit(int code)
{
    _execute_onexit_table(&g_quick_table);
    ExitProcess((UINT)code);
}
CRTEXP void _cexit(void)  { __nova_run_atexit(); fflush(NULL); }
CRTEXP void _c_exit(void) { fflush(NULL); }
CRTEXP __declspec(noreturn) void _amsg_exit(int code)
{
    fprintf(stderr, "runtime error R60%02d\n", code);
    ExitProcess(255);
}
CRTEXP void _register_thread_local_exe_atexit_callback(void *cb) { (void)cb; }

/* -----------------------------------------------------------------------
 * Handlers and modes
 * ----------------------------------------------------------------------- */
static int g_app_type;
CRTEXP void _set_app_type(int t) { g_app_type = t; }
FILE *__iob_func(void);
/* Programs built against the old msvcrt.dll reach stdout as &_iob[1]
 * without calling __iob_func: set the streams up at startup */
CRTEXP void __set_app_type(int t) { g_app_type = t; __iob_func(); }
CRTEXP int  _query_app_type(void) { return g_app_type; }
CRTEXP void __setusermatherr(void *fn) { (void)fn; }
CRTEXP int  _configthreadlocale(int t) { (void)t; return 1; }       /* _DISABLE_PER_THREAD_LOCALE */
CRTEXP int  _seh_filter_exe(unsigned long code, void *ep) { (void)code; (void)ep; return 0; }  /* CONTINUE_SEARCH */
CRTEXP int  _seh_filter_dll(unsigned long code, void *ep) { (void)code; (void)ep; return 0; }
CRTEXP unsigned int _set_abort_behavior(unsigned int flags, unsigned int mask) { (void)flags; (void)mask; return 0; }
CRTEXP int  _set_error_mode(int m) { (void)m; return 0; }

typedef int (__cdecl *new_handler_t)(size_t);
static new_handler_t g_new_handler;
static int g_new_mode;
CRTEXP int _set_new_mode(int m) { int o = g_new_mode; g_new_mode = m; return o; }
CRTEXP int _query_new_mode(void) { return g_new_mode; }
CRTEXP new_handler_t _set_new_handler(new_handler_t h) { new_handler_t o = g_new_handler; g_new_handler = h; return o; }
CRTEXP new_handler_t _query_new_handler(void) { return g_new_handler; }
CRTEXP int _callnewh(size_t n) { return g_new_handler ? g_new_handler(n) : 0; }

typedef void (__cdecl *invalid_parameter_t)(const wchar_t *, const wchar_t *, const wchar_t *, unsigned, uintptr_t);
static invalid_parameter_t g_invalid;
CRTEXP invalid_parameter_t _set_invalid_parameter_handler(invalid_parameter_t h) { invalid_parameter_t o = g_invalid; g_invalid = h; return o; }
CRTEXP invalid_parameter_t _get_invalid_parameter_handler(void) { return g_invalid; }
CRTEXP invalid_parameter_t _set_thread_local_invalid_parameter_handler(invalid_parameter_t h) { return _set_invalid_parameter_handler(h); }
CRTEXP invalid_parameter_t _get_thread_local_invalid_parameter_handler(void) { return g_invalid; }
CRTEXP __declspec(noreturn) void _invoke_watson(const wchar_t *e, const wchar_t *f, const wchar_t *file, unsigned line, uintptr_t r)
{
    (void)e; (void)f; (void)file; (void)line; (void)r;
    fputs("The program passed an invalid parameter to the C runtime and was ended.\n", stderr);
    ExitProcess(0xC0000417u);
}
CRTEXP void _invalid_parameter(const wchar_t *e, const wchar_t *f, const wchar_t *file, unsigned line, uintptr_t r)
{
    if (g_invalid) { g_invalid(e, f, file, line, r); return; }
    _invoke_watson(e, f, file, line, r);
}
CRTEXP void _invalid_parameter_noinfo(void) { _invalid_parameter(NULL, NULL, NULL, 0, 0); }
CRTEXP __declspec(noreturn) void _invalid_parameter_noinfo_noreturn(void)
{
    _invalid_parameter(NULL, NULL, NULL, 0, 0);
    ExitProcess(0xC0000417u);
}

typedef void (__cdecl *terminate_t)(void);
static terminate_t g_terminate;
CRTEXP terminate_t set_terminate(terminate_t f) { terminate_t o = g_terminate; g_terminate = f; return o; }
CRTEXP terminate_t _get_terminate(void) { return g_terminate; }
CRTEXP __declspec(noreturn) void terminate(void)
{
    if (g_terminate) g_terminate();
    abort();
}

/* The old CRT's filter around main: let the exception reach the
 * unhandled-exception filter (which ends the program) */
CRTEXP int _XcptFilter(unsigned long code, void *pointers) { (void)code; (void)pointers; return 0; /* EXCEPTION_CONTINUE_SEARCH */ }

CRTEXP int *__doserrno(void) { static int e; return &e; }
CRTEXP int _get_errno(int *v) { *v = errno; return 0; }
CRTEXP int _set_errno(int v) { errno = v; return 0; }
CRTEXP int _get_doserrno(unsigned long *v) { *v = (unsigned long)*__doserrno(); return 0; }
CRTEXP int _set_doserrno(unsigned long v) { *__doserrno() = (int)v; return 0; }

/* The C runtime's per-thread locks: one lock is enough here */
static CRITICAL_SECTION g_crt_lock;
static volatile long g_crt_lock_ready;
static void crt_lock_init(void)
{
    if (__atomic_load_n(&g_crt_lock_ready, __ATOMIC_ACQUIRE) == 2) return;
    if (!__atomic_exchange_n(&g_crt_lock_ready, 1, __ATOMIC_ACQ_REL)) {
        InitializeCriticalSection(&g_crt_lock);
        __atomic_store_n(&g_crt_lock_ready, 2, __ATOMIC_RELEASE);
    }
    while (__atomic_load_n(&g_crt_lock_ready, __ATOMIC_ACQUIRE) != 2) Sleep(0);
}
CRTEXP void _lock(int n)   { (void)n; crt_lock_init(); EnterCriticalSection(&g_crt_lock); }
CRTEXP void _unlock(int n) { (void)n; LeaveCriticalSection(&g_crt_lock); }
CRTEXP void _lock_file(FILE *f)   { (void)f; _lock(0); }
CRTEXP void _unlock_file(FILE *f) { (void)f; _unlock(0); }
CRTEXP void _lock_locales(void)   { }
CRTEXP void _unlock_locales(void) { }

/* -----------------------------------------------------------------------
 * The UCRT's stdio entry points
 * ----------------------------------------------------------------------- */
#define OPT_LEGACY_NULL_TERMINATION 0x01
#define OPT_STANDARD_SNPRINTF       0x02
#define OPT_LEGACY_WIDE_SPECIFIERS  0x04

CRTEXP FILE *__acrt_iob_func(unsigned i) { return &__iob_func()[i < 3 ? i : 0]; }

static int pf_flags(unsigned long long opt, int wide)
{
    /* wide printf: %s is wide unless the program asked for ISO wide specifiers */
    return wide && (opt & OPT_LEGACY_WIDE_SPECIFIERS) ? PF_WIDE : 0;
}

/* snprintf family: count = buffer size; the options pick the return value */
static int finish_sprintf(unsigned long long opt, int r, size_t count, int truncated)
{
    if (opt & OPT_STANDARD_SNPRINTF) return r;          /* C99: the full length */
    if (truncated) return count == 0 ? r : -1;          /* legacy: -1 when it did not fit */
    return r;
}

CRTEXP int __stdio_common_vfprintf(unsigned long long opt, FILE *f, const char *fmt, void *loc, va_list ap)
{
    (void)loc;
    return __nova_vfprintf(f, fmt, ap, pf_flags(opt, 0));
}
CRTEXP int __stdio_common_vfprintf_s(unsigned long long opt, FILE *f, const char *fmt, void *loc, va_list ap)
{ return __stdio_common_vfprintf(opt, f, fmt, loc, ap); }
CRTEXP int __stdio_common_vfprintf_p(unsigned long long opt, FILE *f, const char *fmt, void *loc, va_list ap)
{ return __stdio_common_vfprintf(opt, f, fmt, loc, ap); }

CRTEXP int __stdio_common_vsprintf(unsigned long long opt, char *buf, size_t count, const char *fmt, void *loc, va_list ap)
{
    (void)loc;
    int r = __nova_vsnprintf(buf, buf ? count : 0, fmt, ap, 0);
    if (r < 0) return -1;
    return finish_sprintf(opt, r, count, (size_t)r >= count);
}
CRTEXP int __stdio_common_vsprintf_s(unsigned long long opt, char *buf, size_t count, const char *fmt, void *loc, va_list ap)
{
    int r = __stdio_common_vsprintf(opt | OPT_STANDARD_SNPRINTF, buf, count, fmt, loc, ap);
    if (r >= 0 && (size_t)r >= count) { if (buf && count) buf[0] = 0; errno = ERANGE; return -1; }
    return r;
}
CRTEXP int __stdio_common_vsnprintf_s(unsigned long long opt, char *buf, size_t count, size_t max, const char *fmt, void *loc, va_list ap)
{
    size_t lim = max < count ? max + 1 : count;
    int r = __stdio_common_vsprintf(opt | OPT_STANDARD_SNPRINTF, buf, lim, fmt, loc, ap);
    if (r >= 0 && (size_t)r >= lim) return max < count ? -1 : (buf && count ? (buf[0] = 0, -1) : -1);
    return r;
}
CRTEXP int __stdio_common_vsprintf_p(unsigned long long opt, char *buf, size_t count, const char *fmt, void *loc, va_list ap)
{ return __stdio_common_vsprintf(opt | OPT_STANDARD_SNPRINTF, buf, count, fmt, loc, ap); }

CRTEXP int __stdio_common_vfwprintf(unsigned long long opt, FILE *f, const wchar_t *fmt, void *loc, va_list ap)
{
    (void)loc;
    return __nova_vfwprintf(f, fmt, ap, pf_flags(opt, 1));
}
CRTEXP int __stdio_common_vfwprintf_s(unsigned long long opt, FILE *f, const wchar_t *fmt, void *loc, va_list ap)
{ return __stdio_common_vfwprintf(opt, f, fmt, loc, ap); }
CRTEXP int __stdio_common_vfwprintf_p(unsigned long long opt, FILE *f, const wchar_t *fmt, void *loc, va_list ap)
{ return __stdio_common_vfwprintf(opt, f, fmt, loc, ap); }

CRTEXP int __stdio_common_vswprintf(unsigned long long opt, wchar_t *buf, size_t count, const wchar_t *fmt, void *loc, va_list ap)
{
    (void)loc;
    int r = __nova_vsnwprintf(buf, buf ? count : 0, fmt, ap, pf_flags(opt, 1));
    if (r < 0) return -1;
    return finish_sprintf(opt, r, count, (size_t)r >= count);
}
CRTEXP int __stdio_common_vswprintf_s(unsigned long long opt, wchar_t *buf, size_t count, const wchar_t *fmt, void *loc, va_list ap)
{
    int r = __stdio_common_vswprintf(opt | OPT_STANDARD_SNPRINTF, buf, count, fmt, loc, ap);
    if (r >= 0 && (size_t)r >= count) { if (buf && count) buf[0] = 0; errno = ERANGE; return -1; }
    return r;
}
CRTEXP int __stdio_common_vsnwprintf_s(unsigned long long opt, wchar_t *buf, size_t count, size_t max, const wchar_t *fmt, void *loc, va_list ap)
{
    size_t lim = max < count ? max + 1 : count;
    int r = __stdio_common_vswprintf(opt | OPT_STANDARD_SNPRINTF, buf, lim, fmt, loc, ap);
    if (r >= 0 && (size_t)r >= lim) return -1;
    return r;
}
CRTEXP int __stdio_common_vswprintf_p(unsigned long long opt, wchar_t *buf, size_t count, const wchar_t *fmt, void *loc, va_list ap)
{ return __stdio_common_vswprintf(opt | OPT_STANDARD_SNPRINTF, buf, count, fmt, loc, ap); }

CRTEXP int __stdio_common_vsscanf(unsigned long long opt, const char *buf, size_t count, const char *fmt, void *loc, va_list ap)
{
    (void)opt; (void)loc;
    if (count != (size_t)-1 && buf) {
        char *tmp = malloc(count + 1);
        if (!tmp) return EOF;
        memcpy(tmp, buf, count);
        tmp[count] = 0;
        int r = vsscanf(tmp, fmt, ap);
        free(tmp);
        return r;
    }
    return vsscanf(buf, fmt, ap);
}
CRTEXP int __stdio_common_vfscanf(unsigned long long opt, FILE *f, const char *fmt, void *loc, va_list ap)
{
    (void)opt; (void)loc;
    return vfscanf(f, fmt, ap);
}
/* Wide scanf: numbers and wide/narrow strings, via the narrow scanner on a
 * UTF-8 copy (targets of %s/%c/%[ in wide mode are narrowed back) */
CRTEXP int __stdio_common_vswscanf(unsigned long long opt, const wchar_t *buf, size_t count, const wchar_t *fmt, void *loc, va_list ap)
{
    (void)opt; (void)loc; (void)count;
    size_t nb = wcstombs(NULL, buf, 0), nf = wcstombs(NULL, fmt, 0);
    if (nb == (size_t)-1 || nf == (size_t)-1) return EOF;
    char *b = malloc(nb + 1), *f = malloc(nf + 1);
    if (!b || !f) { free(b); free(f); return EOF; }
    wcstombs(b, buf, nb + 1);
    wcstombs(f, fmt, nf + 1);
    int r = vsscanf(b, f, ap);
    free(b); free(f);
    return r;
}
int vswscanf(const wchar_t *buf, const wchar_t *fmt, va_list ap) { return __stdio_common_vswscanf(0, buf, (size_t)-1, fmt, NULL, ap); }
int swscanf(const wchar_t *buf, const wchar_t *fmt, ...) { va_list a; va_start(a, fmt); int r = vswscanf(buf, fmt, a); va_end(a); return r; }
int sscanf_s(const char *buf, const char *fmt, ...) { va_list a; va_start(a, fmt); int r = vsscanf(buf, fmt, a); va_end(a); return r; }

/* -----------------------------------------------------------------------
 * Threads
 * ----------------------------------------------------------------------- */
typedef unsigned (__stdcall *thread_ex_t)(void *);
typedef void (__cdecl *thread_t)(void *);
CRTEXP uintptr_t _beginthreadex(void *sec, unsigned stack, thread_ex_t fn, void *arg, unsigned flags, unsigned *tid)
{
    DWORD id = 0;
    HANDLE h = CreateThread(sec, stack, (LPTHREAD_START_ROUTINE)fn, arg, flags, &id);
    if (tid) *tid = id;
    return (uintptr_t)h;
}
CRTEXP void _endthreadex(unsigned code) { ExitThread(code); }
typedef struct { thread_t fn; void *arg; } BtArgs;
static DWORD WINAPI bt_start(LPVOID p)
{
    BtArgs a = *(BtArgs *)p;
    free(p);
    a.fn(a.arg);
    return 0;
}
CRTEXP uintptr_t _beginthread(thread_t fn, unsigned stack, void *arg)
{
    BtArgs *a = malloc(sizeof(*a));
    if (!a) return (uintptr_t)-1;
    a->fn = fn; a->arg = arg;
    HANDLE h = CreateThread(NULL, stack, bt_start, a, 0, NULL);
    if (!h) { free(a); return (uintptr_t)-1; }
    CloseHandle(h);                             /* _beginthread handles close themselves */
    return (uintptr_t)h;
}
CRTEXP void _endthread(void) { ExitThread(0); }
CRTEXP unsigned long __threadid(void) { return GetCurrentThreadId(); }
CRTEXP uintptr_t __threadhandle(void) { return (uintptr_t)GetCurrentThread(); }

/* -----------------------------------------------------------------------
 * setjmp / longjmp (Microsoft's names; the frame argument is ignored:
 * longjmp restores registers without unwinding)
 * ----------------------------------------------------------------------- */
#ifdef _WIN64                   /* (32-bit: msvcrt/misc.c) */
__asm__(".globl _setjmp\n.globl _setjmpex\n.globl __intrinsic_setjmp\n.globl __intrinsic_setjmpex\n"
        ".section .text$setjmp2,\"xr\"\n"
        "_setjmp:\n_setjmpex:\n__intrinsic_setjmp:\n__intrinsic_setjmpex:\n\t"
        "jmp _setjmp_nova\n");
__asm__(".section .drectve\n"
        ".ascii \" -export:_setjmp -export:_setjmpex -export:__intrinsic_setjmp -export:__intrinsic_setjmpex\"\n"
        ".text\n");
#endif

/* -----------------------------------------------------------------------
 * Secure string functions
 * ----------------------------------------------------------------------- */
CRTEXP int strcpy_s(char *d, size_t n, const char *s)
{
    if (!d || !n) return EINVAL;
    if (!s) { d[0] = 0; return EINVAL; }
    size_t l = strlen(s);
    if (l >= n) { d[0] = 0; return ERANGE; }
    memcpy(d, s, l + 1);
    return 0;
}
CRTEXP int strcat_s(char *d, size_t n, const char *s)
{
    if (!d || !n) return EINVAL;
    size_t dl = strnlen(d, n);
    if (dl == n) return EINVAL;
    return strcpy_s(d + dl, n - dl, s);
}
CRTEXP int strncpy_s(char *d, size_t n, const char *s, size_t cnt)
{
    if (!d || !n) return EINVAL;
    if (!s) { d[0] = 0; return cnt ? EINVAL : 0; }
    size_t l = strnlen(s, cnt == (size_t)-1 ? n : cnt);
    if (l >= n) { if (cnt == (size_t)-1) { memcpy(d, s, n - 1); d[n - 1] = 0; return 80; } d[0] = 0; return ERANGE; }
    memcpy(d, s, l);
    d[l] = 0;
    return 0;
}
CRTEXP int strncat_s(char *d, size_t n, const char *s, size_t cnt)
{
    if (!d || !n) return EINVAL;
    size_t dl = strnlen(d, n);
    if (dl == n) return EINVAL;
    return strncpy_s(d + dl, n - dl, s, cnt);
}
CRTEXP int wcscpy_s(wchar_t *d, size_t n, const wchar_t *s)
{
    if (!d || !n) return EINVAL;
    if (!s) { d[0] = 0; return EINVAL; }
    size_t l = wcslen(s);
    if (l >= n) { d[0] = 0; return ERANGE; }
    memcpy(d, s, (l + 1) * sizeof(wchar_t));
    return 0;
}
CRTEXP int wcscat_s(wchar_t *d, size_t n, const wchar_t *s)
{
    if (!d || !n) return EINVAL;
    size_t dl = wcsnlen(d, n);
    if (dl == n) return EINVAL;
    return wcscpy_s(d + dl, n - dl, s);
}
CRTEXP int wcsncpy_s(wchar_t *d, size_t n, const wchar_t *s, size_t cnt)
{
    if (!d || !n) return EINVAL;
    if (!s) { d[0] = 0; return cnt ? EINVAL : 0; }
    size_t l = wcsnlen(s, cnt == (size_t)-1 ? n : cnt);
    if (l >= n) { if (cnt == (size_t)-1) { memcpy(d, s, (n - 1) * sizeof(wchar_t)); d[n - 1] = 0; return 80; } d[0] = 0; return ERANGE; }
    memcpy(d, s, l * sizeof(wchar_t));
    d[l] = 0;
    return 0;
}
CRTEXP int wcsncat_s(wchar_t *d, size_t n, const wchar_t *s, size_t cnt)
{
    if (!d || !n) return EINVAL;
    size_t dl = wcsnlen(d, n);
    if (dl == n) return EINVAL;
    return wcsncpy_s(d + dl, n - dl, s, cnt);
}
CRTEXP wchar_t *wcstok_s(wchar_t *s, const wchar_t *delim, wchar_t **ctx) { return wcstok(s, delim, ctx); }
CRTEXP int memcpy_s(void *d, size_t dn, const void *s, size_t n)
{
    if (!n) return 0;
    if (!d) return EINVAL;
    if (!s || dn < n) { memset(d, 0, dn); return !s ? EINVAL : ERANGE; }
    memcpy(d, s, n);
    return 0;
}
CRTEXP int memmove_s(void *d, size_t dn, const void *s, size_t n)
{
    if (!n) return 0;
    if (!d || !s) return EINVAL;
    if (dn < n) return ERANGE;
    memmove(d, s, n);
    return 0;
}
CRTEXP int strerror_s(char *buf, size_t n, int e)
{
    return strncpy_s(buf, n, strerror(e), (size_t)-1) == 80 ? 0 : 0;
}
CRTEXP int _strerror_s(char *buf, size_t n, const char *msg)
{
    if (msg && *msg) snprintf(buf, n, "%s: %s\n", msg, strerror(errno));
    else snprintf(buf, n, "%s\n", strerror(errno));
    return 0;
}
CRTEXP wchar_t *_wcserror(int e)
{
    static wchar_t buf[128];
    mbstowcs(buf, strerror(e), 127);
    return buf;
}
CRTEXP int _wcserror_s(wchar_t *buf, size_t n, int e)
{
    size_t r = mbstowcs(buf, strerror(e), n - 1);
    if (r != (size_t)-1) buf[r < n ? r : n - 1] = 0;
    return 0;
}
CRTEXP int _itoa_s(int v, char *b, size_t n, int radix) { char t[40]; _itoa(v, t, radix); return strcpy_s(b, n, t); }
CRTEXP int _ltoa_s(long v, char *b, size_t n, int radix) { char t[40]; _ltoa(v, t, radix); return strcpy_s(b, n, t); }
CRTEXP int _ultoa_s(unsigned long v, char *b, size_t n, int radix) { char t[40]; _ultoa(v, t, radix); return strcpy_s(b, n, t); }
CRTEXP char *_i64toa(long long v, char *b, int radix)
{
    unsigned long long u = v < 0 && radix == 10 ? 0ull - (unsigned long long)v : (unsigned long long)v;
    char t[72];
    int n = 0;
    do { int d = (int)(u % (unsigned)radix); t[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); u /= (unsigned)radix; } while (u);
    int o = 0;
    if (v < 0 && radix == 10) b[o++] = '-';
    while (n) b[o++] = t[--n];
    b[o] = 0;
    return b;
}
CRTEXP char *_ui64toa(unsigned long long u, char *b, int radix)
{
    char t[72];
    int n = 0;
    do { int d = (int)(u % (unsigned)radix); t[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); u /= (unsigned)radix; } while (u);
    int o = 0;
    while (n) b[o++] = t[--n];
    b[o] = 0;
    return b;
}
CRTEXP int _i64toa_s(long long v, char *b, size_t n, int radix) { char t[72]; _i64toa(v, t, radix); return strcpy_s(b, n, t); }
CRTEXP int _ui64toa_s(unsigned long long v, char *b, size_t n, int radix) { char t[72]; _ui64toa(v, t, radix); return strcpy_s(b, n, t); }
CRTEXP long long _strtoi64(const char *s, char **e, int b) { return strtoll(s, e, b); }
CRTEXP unsigned long long _strtoui64(const char *s, char **e, int b) { return strtoull(s, e, b); }
CRTEXP long long _atoi64(const char *s) { return strtoll(s, NULL, 10); }
CRTEXP long double strtold(const char *s, char **e) { return strtod(s, e); }
CRTEXP intmax_t strtoimax(const char *s, char **e, int b) { return strtoll(s, e, b); }
CRTEXP uintmax_t strtoumax(const char *s, char **e, int b) { return strtoull(s, e, b); }
CRTEXP char *_strrev(char *s)
{
    size_t n = strlen(s);
    for (size_t i = 0; i < n / 2; i++) { char t = s[i]; s[i] = s[n - 1 - i]; s[n - 1 - i] = t; }
    return s;
}
CRTEXP wchar_t *_wcsrev(wchar_t *s)
{
    size_t n = wcslen(s);
    for (size_t i = 0; i < n / 2; i++) { wchar_t t = s[i]; s[i] = s[n - 1 - i]; s[n - 1 - i] = t; }
    return s;
}
CRTEXP char *_strset(char *s, int c) { for (char *p = s; *p; p++) *p = (char)c; return s; }
CRTEXP char *_strnset(char *s, int c, size_t n) { for (size_t i = 0; i < n && s[i]; i++) s[i] = (char)c; return s; }
CRTEXP size_t strxfrm(char *d, const char *s, size_t n)
{
    size_t len = strlen(s);
    if (n) { strncpy(d, s, n); if (len >= n) d[n - 1] = 0; }
    return len;
}
CRTEXP int _stricoll(const char *a, const char *b) { return _stricmp(a, b); }
CRTEXP int _strcoll_l(const char *a, const char *b, void *l) { (void)l; return strcoll(a, b); }
CRTEXP int _stricmp_l(const char *a, const char *b, void *l) { (void)l; return _stricmp(a, b); }
CRTEXP int _strnicmp_l(const char *a, const char *b, size_t n, void *l) { (void)l; return _strnicmp(a, b, n); }
CRTEXP int _memicmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; i++) {
        int c = x[i] >= 'A' && x[i] <= 'Z' ? x[i] + 32 : x[i];
        int d = y[i] >= 'A' && y[i] <= 'Z' ? y[i] + 32 : y[i];
        if (c != d) return c - d;
    }
    return 0;
}
CRTEXP char *strtok_r(char *s, const char *d, char **ctx) { return strtok_s(s, d, ctx); }

/* -----------------------------------------------------------------------
 * Heap extras
 * ----------------------------------------------------------------------- */
CRTEXP void *_aligned_malloc(size_t n, size_t align)
{
    if (align < sizeof(void *)) align = sizeof(void *);
    if (align & (align - 1)) { errno = EINVAL; return NULL; }
    char *raw = malloc(n + align + sizeof(void *));
    if (!raw) return NULL;
    uintptr_t p = ((uintptr_t)raw + sizeof(void *) + align - 1) & ~(uintptr_t)(align - 1);
    ((void **)p)[-1] = raw;
    return (void *)p;
}
CRTEXP void _aligned_free(void *p) { if (p) free(((void **)p)[-1]); }
CRTEXP size_t _aligned_msize(void *p, size_t align, size_t off)
{
    (void)off;
    if (!p) return 0;
    char *raw = ((char **)p)[-1];
    return _msize(raw) - (size_t)((char *)p - raw) - 0 * align;
}
CRTEXP void *_aligned_realloc(void *p, size_t n, size_t align)
{
    if (!p) return _aligned_malloc(n, align);
    if (!n) { _aligned_free(p); return NULL; }
    void *q = _aligned_malloc(n, align);
    if (!q) return NULL;
    size_t old = _aligned_msize(p, align, 0);
    memcpy(q, p, old < n ? old : n);
    _aligned_free(p);
    return q;
}
CRTEXP void *_aligned_offset_malloc(size_t n, size_t align, size_t off) { (void)off; return _aligned_malloc(n, align); }
CRTEXP void *_recalloc(void *p, size_t n, size_t size)
{
    size_t total = n * size, old = p ? _msize(p) : 0;
    void *q = realloc(p, total);
    if (q && total > old) memset((char *)q + old, 0, total - old);
    return q;
}
CRTEXP void *_expand(void *p, size_t n) { (void)p; (void)n; return NULL; }
CRTEXP int _heapchk(void) { return -2; }                  /* _HEAPOK */
CRTEXP int _heapmin(void) { return 0; }
CRTEXP intptr_t _get_heap_handle(void) { return (intptr_t)GetProcessHeap(); }
CRTEXP void *_malloc_base(size_t n) { return malloc(n); }
CRTEXP void *_calloc_base(size_t n, size_t s) { return calloc(n, s); }
CRTEXP void *_realloc_base(void *p, size_t n) { return realloc(p, n); }
CRTEXP void _free_base(void *p) { free(p); }

CRTEXP int rand_s(unsigned int *v)
{
    HMODULE adv = LoadLibraryA("advapi32.dll");
    BOOLEAN (__stdcall *gen)(void *, ULONG) = adv ? (BOOLEAN (__stdcall *)(void *, ULONG))GetProcAddress(adv, "SystemFunction036") : NULL;
    if (gen && gen(v, sizeof(*v))) return 0;
    *v = (unsigned)rand() << 17 ^ (unsigned)rand() << 2 ^ (unsigned)GetTickCount();
    return 0;
}
