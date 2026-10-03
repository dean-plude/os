/*
 * vcruntime.c — the rest of vcruntime140.dll: RTTI (typeid, dynamic_cast),
 * std::type_info and std::exception helpers, pure-call and security-check
 * failures, and the string/setjmp functions it shares with the C runtime
 * (forwarded to ucrtbase.dll).
 */

#include <winternl.h>
#include <winnt.h>

#define VCRT __declspec(dllexport)

static void *vmalloc(SIZE_T n) { return RtlAllocateHeap(RtlGetProcessHeap(), 0, n); }
static void  vfree(void *p)    { if (p) RtlFreeHeap(RtlGetProcessHeap(), 0, p); }
static size_t slen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static int scmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }

static __declspec(noreturn) void fatal(const char *msg, UINT code)
{
    DWORD w;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), msg, (DWORD)slen(msg), &w, 0);
    NtNovaDebugPrint(msg, (ULONG)slen(msg));
    RtlExitUserProcess(code);
}

/* -----------------------------------------------------------------------
 * RTTI
 * ----------------------------------------------------------------------- */
typedef struct { const void *vftable; void *spare; char name[1]; } TypeDescriptor;
typedef struct { int mdisp, pdisp, vdisp; } PMD;
typedef struct { int pTypeDescriptor; DWORD numContainedBases; PMD where; DWORD attributes; int pClassDescriptor; } BaseClassDescriptor;
typedef struct { DWORD signature, attributes, numBaseClasses; int pBaseClassArray; } ClassHierarchyDescriptor;
typedef struct { DWORD signature, offset, cdOffset; int pTypeDescriptor, pClassDescriptor, pSelf; } CompleteObjectLocator;

/* The RTTI tables hold image-relative offsets (signature 1, x64) or
 * absolute addresses (signature 0, x86): @base makes both addresses */
static const CompleteObjectLocator *locator(void *obj, ULONG_PTR *base)
{
    const CompleteObjectLocator *col = ((const CompleteObjectLocator **)*(void **)obj)[-1];
    *base = col->signature ? (ULONG_PTR)col - (DWORD)col->pSelf : 0;
    return col;
}

static char *complete_object(void *obj, const CompleteObjectLocator *col)
{
    char *p = (char *)obj;
    if (col->cdOffset) p -= *(int *)(p - col->cdOffset);
    return p - col->offset;
}

VCRT void *__RTCastToVoid(void *obj)
{
    if (!obj) return 0;
    ULONG_PTR base;
    const CompleteObjectLocator *col = locator(obj, &base);
    return complete_object(obj, col);
}

VCRT void *__RTtypeid(void *obj)
{
    if (!obj) fatal("Access violation - no RTTI data!\n", 3);   /* would be std::bad_typeid */
    ULONG_PTR base;
    const CompleteObjectLocator *col = locator(obj, &base);
    return (void *)(base + col->pTypeDescriptor);
}

VCRT void *__RTDynamicCast(void *in, long vfdelta, void *src_type, void *target_type, int is_reference)
{
    (void)vfdelta; (void)src_type;
    if (!in) return 0;
    ULONG_PTR base;
    const CompleteObjectLocator *col = locator(in, &base);
    char *complete = complete_object(in, col);
    const ClassHierarchyDescriptor *chd = (const ClassHierarchyDescriptor *)(base + col->pClassDescriptor);
    const int *bases = (const int *)(base + chd->pBaseClassArray);
    const TypeDescriptor *want = target_type;
    for (DWORD i = 0; i < chd->numBaseClasses; i++) {
        const BaseClassDescriptor *bcd = (const BaseClassDescriptor *)(base + bases[i]);
        const TypeDescriptor *td = (const TypeDescriptor *)(base + bcd->pTypeDescriptor);
        if (td != want && scmp(td->name, want->name)) continue;
        char *r = complete + bcd->where.mdisp;
        if (bcd->where.pdisp >= 0) {
            char *vbtable = *(char **)(complete + bcd->where.pdisp);
            r += *(int *)(vbtable + bcd->where.vdisp) + bcd->where.pdisp;
        }
        return r;
    }
    if (is_reference) fatal("Bad dynamic_cast!\n", 3);           /* would be std::bad_cast */
    return 0;
}

/* -----------------------------------------------------------------------
 * std::type_info
 * ----------------------------------------------------------------------- */
typedef struct { const char *undecorated; char decorated[1]; } TypeInfoData;
typedef struct TypeInfoNode { void *next; } TypeInfoNode;

/* A readable name for the common decorated forms (".?AVFoo@ns@@" →
 * "class ns::Foo"); anything else keeps its decorated spelling. */
static char *undecorate(const char *d)
{
    static const struct { const char *code, *name; } prim[] = {
        { ".H", "int" }, { ".I", "unsigned int" }, { ".D", "char" }, { ".E", "unsigned char" }, { ".C", "signed char" },
        { ".F", "short" }, { ".G", "unsigned short" }, { ".J", "long" }, { ".K", "unsigned long" },
        { ".M", "float" }, { ".N", "double" }, { ".O", "long double" }, { ".X", "void" },
        { "._N", "bool" }, { "._J", "__int64" }, { "._K", "unsigned __int64" }, { "._W", "wchar_t" },
        { ".$$T", "std::nullptr_t" },
    };
    for (unsigned i = 0; i < sizeof(prim) / sizeof(prim[0]); i++)
        if (!scmp(d, prim[i].code)) {
            char *r = vmalloc(slen(prim[i].name) + 1);
            if (r) { size_t n = slen(prim[i].name); for (size_t k = 0; k <= n; k++) r[k] = prim[i].name[k]; }
            return r;
        }
    const char *kind = 0;
    if (d[0] == '.' && d[1] == '?' && d[2] == 'A') {
        if (d[3] == 'V') kind = "class ";
        else if (d[3] == 'U') kind = "struct ";
        else if (d[3] == 'T') kind = "union ";
        else if (d[3] == 'W' && d[4] == '4') kind = "enum ";
    }
    size_t n = slen(d);
    char *r = vmalloc(n + 32);
    if (!r) return 0;
    if (!kind) { for (size_t k = 0; k <= n; k++) r[k] = d[k]; return r; }
    const char *s = d + (kind[0] == 'e' ? 5 : 4);
    /* components "a@b@c@@" are innermost first; templates ("?$") keep their raw form */
    const char *comp[16];
    size_t len[16];
    int nc = 0;
    while (*s && *s != '@' && nc < 16) {
        const char *e = s;
        if (e[0] == '?' && e[1] == '$') { while (*e && !(e[0] == '@' && e[1] == '@')) e++; if (*e) e++; }
        else while (*e && *e != '@') e++;
        comp[nc] = s; len[nc] = (size_t)(e - s); nc++;
        s = *e ? e + 1 : e;
    }
    size_t o = 0;
    for (const char *k = kind; *k; k++) r[o++] = *k;
    for (int i = nc - 1; i >= 0; i--) {
        if (o + len[i] + 3 >= n + 32) break;
        for (size_t k = 0; k < len[i]; k++) r[o++] = comp[i][k];
        if (i) { r[o++] = ':'; r[o++] = ':'; }
    }
    r[o] = 0;
    return r;
}

VCRT const char *__std_type_info_name(TypeInfoData *data, TypeInfoNode *root)
{
    (void)root;
    if (!data->undecorated) {
        char *u = undecorate(data->decorated);
        const char *expect = 0;
        if (!u || !__atomic_compare_exchange_n(&data->undecorated, &expect, u, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            vfree(u);
    }
    return data->undecorated ? data->undecorated : data->decorated;
}

VCRT int __std_type_info_compare(const TypeInfoData *a, const TypeInfoData *b)
{
    return a == b ? 0 : scmp(a->decorated + 1, b->decorated + 1);
}

VCRT size_t __std_type_info_hash(const TypeInfoData *d)
{
#ifdef _WIN64
    size_t h = 14695981039346656037ULL;                 /* FNV-1a */
    for (const unsigned char *p = (const unsigned char *)d->decorated + 1; *p; p++) h = (h ^ *p) * 1099511628211ULL;
#else
    size_t h = 2166136261u;                             /* FNV-1a, 32-bit */
    for (const unsigned char *p = (const unsigned char *)d->decorated + 1; *p; p++) h = (h ^ *p) * 16777619u;
#endif
    return h;
}

VCRT void __std_type_info_destroy_list(TypeInfoNode *root) { (void)root; }   /* names are kept for the process */

/* -----------------------------------------------------------------------
 * std::exception's message
 * ----------------------------------------------------------------------- */
typedef struct { const char *what; char do_free; } StdExceptionData;

VCRT void __std_exception_copy(const StdExceptionData *from, StdExceptionData *to)
{
    if (from->do_free && from->what) {
        size_t n = slen(from->what);
        char *s = vmalloc(n + 1);
        if (s) {
            for (size_t k = 0; k <= n; k++) s[k] = from->what[k];
            to->what = s;
            to->do_free = 1;
            return;
        }
    }
    to->what = from->what;
    to->do_free = 0;
}

VCRT void __std_exception_destroy(StdExceptionData *d)
{
    if (d->do_free) vfree((void *)d->what);
    d->what = 0;
    d->do_free = 0;
}

/* -----------------------------------------------------------------------
 * Failures and handlers
 * ----------------------------------------------------------------------- */
typedef void (__cdecl *PurecallHandler)(void);
static PurecallHandler g_purecall;

VCRT PurecallHandler _set_purecall_handler(PurecallHandler h) { PurecallHandler o = g_purecall; g_purecall = h; return o; }
VCRT PurecallHandler _get_purecall_handler(void) { return g_purecall; }

VCRT int __cdecl _purecall(void)
{
    if (g_purecall) g_purecall();
    fatal("R6025 - pure virtual function call\n", 3);
}

VCRT __declspec(noreturn) void __report_gsfailure(ULONG_PTR cookie)
{
    (void)cookie;
    fatal("Stack buffer overrun detected; the program was stopped.\n", 0xC0000409);
}

VCRT __declspec(noreturn) void __report_rangecheckfailure(void)
{
    fatal("Range check failure; the program was stopped.\n", 0xC0000409);
}

typedef void (__cdecl *SeTranslator)(unsigned, EXCEPTION_POINTERS *);
static SeTranslator g_se;
VCRT SeTranslator _set_se_translator(SeTranslator f) { SeTranslator o = g_se; g_se = f; return o; }

typedef void (__cdecl *UnexpectedHandler)(void);
static UnexpectedHandler g_unexpected;
VCRT UnexpectedHandler set_unexpected(UnexpectedHandler f) { UnexpectedHandler o = g_unexpected; g_unexpected = f; return o; }
VCRT void unexpected(void) { if (g_unexpected) g_unexpected(); fatal("unexpected exception\n", 3); }

VCRT void __telemetry_main_invoke_trigger(void *h) { (void)h; }
VCRT void __telemetry_main_return_trigger(void *h) { (void)h; }
VCRT void __NLG_Dispatch2(void) { }
VCRT void __NLG_Return2(void) { }
VCRT BOOL __vcrt_InitializeCriticalSectionEx(LPCRITICAL_SECTION cs, DWORD spin, DWORD flags)
{
    (void)flags;
    return InitializeCriticalSectionAndSpinCount(cs, spin);
}

/* The C runtime's share of vcruntime140's exports.  (On x86 the linker
 * takes an /EXPORT name as a C-decorated symbol and drops one leading
 * underscore, so the names that start with one get another: XU.) */
#ifdef _WIN64
#define XU ""
#else
#define XU "_"
#endif
__asm__(".section .drectve,\"yn\"\n\t"
        ".ascii \" /EXPORT:memcpy=ucrtbase.memcpy /EXPORT:memmove=ucrtbase.memmove /EXPORT:memset=ucrtbase.memset\"\n\t"
        ".ascii \" /EXPORT:memcmp=ucrtbase.memcmp /EXPORT:memchr=ucrtbase.memchr\"\n\t"
        ".ascii \" /EXPORT:strchr=ucrtbase.strchr /EXPORT:strrchr=ucrtbase.strrchr /EXPORT:strstr=ucrtbase.strstr\"\n\t"
        ".ascii \" /EXPORT:wcschr=ucrtbase.wcschr /EXPORT:wcsrchr=ucrtbase.wcsrchr /EXPORT:wcsstr=ucrtbase.wcsstr\"\n\t"
        ".ascii \" /EXPORT:" XU "_setjmp=ucrtbase._setjmp /EXPORT:" XU "_setjmpex=ucrtbase._setjmpex\"\n\t"
        ".ascii \" /EXPORT:" XU "__intrinsic_setjmp=ucrtbase.__intrinsic_setjmp /EXPORT:" XU "__intrinsic_setjmpex=ucrtbase.__intrinsic_setjmpex\"\n\t"
        ".ascii \" /EXPORT:longjmp=ucrtbase.longjmp /EXPORT:" XU "__std_terminate=ucrtbase.terminate\"\n\t"
        ".ascii \" /EXPORT:" XU "__C_specific_handler=ntdll.__C_specific_handler\"\n\t"
        ".ascii \" /EXPORT:" XU "__vcrt_GetModuleFileNameW=kernel32.GetModuleFileNameW\"\n\t"
        ".ascii \" /EXPORT:" XU "__vcrt_GetModuleHandleW=kernel32.GetModuleHandleW\"\n\t"
        ".ascii \" /EXPORT:" XU "__vcrt_LoadLibraryExW=kernel32.LoadLibraryExW\"\n\t"
        ".text\n");

/* std::type_info's vtable: every RTTI/EH type descriptor points at it */
#ifdef _WIN64
#define VIRTUAL __cdecl
#define TYPE_INFO_DTOR "??1type_info@@UEAA@XZ"
#else                                           /* x86: member functions are thiscall */
#define VIRTUAL __thiscall
#define TYPE_INFO_DTOR "??1type_info@@UAE@XZ"
#endif
static void *VIRTUAL type_info_delete(void *self, unsigned flags)
{
    TypeInfoData *d = (TypeInfoData *)((char *)self + 8);
    vfree((void *)d->undecorated);
    if (flags & 1) vfree(self);
    return self;
}
static void __cdecl type_info_dtor(void *self) { type_info_delete(self, 0); }

__declspec(dllexport) void *const type_info_vtable[1] __asm__("??_7type_info@@6B@") = { (void *)type_info_delete };
VCRT void VIRTUAL type_info_destructor(void *self) __asm__(TYPE_INFO_DTOR);
VCRT void VIRTUAL type_info_destructor(void *self) { type_info_dtor(self); }
