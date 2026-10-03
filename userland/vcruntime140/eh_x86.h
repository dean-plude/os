/*
 * eh_x86.h — C++ exception handling for 32-bit programs (included by
 * eh.c in the SysWOW64 build of vcruntime140.dll)
 *
 * x86 C++ code (MSVC, clang-cl) links a registration node into the fs:[0]
 * chain in every function with objects to destroy or try blocks:
 *   [node-4] saved ESP   [node] next   [node+4] handler   [node+8] state
 * The handler is a stub "mov eax, FuncInfo; jmp __CxxFrameHandler3", and
 * the function's EBP is node+12: catch blocks, destructors ("unwind
 * actions") and the continuation after a catch all run with that EBP.
 * The tables hold absolute addresses (no image-relative offsets as on x64).
 *
 * A throw raises 0xE06D7363 { magic, object, ThrowInfo }.  The frame
 * handler of the function whose try block matches (by the current state)
 *   1. constructs the catch object in the frame,
 *   2. unwinds the frames above it (RtlUnwind: their destructors run) and
 *      its own states down to the try block,
 *   3. calls the catch block with EBP set, still deep in the stack, so the
 *      thrown object (in a dead frame below) stays intact,
 *   4. destroys the exception object and continues at the address the
 *      catch block returned, with the frame's EBP and saved ESP.
 */
#include <winternl.h>
#include <winnt.h>

#define VCRT __declspec(dllexport)

#define CXX_EXCEPTION     0xE06D7363u
#define CXX_MAGIC_MIN     0x19930520u
#define CXX_MAGIC_MAX     0x19930522u
#define EXC_UNWIND_MASK   (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)

#define HT_IsConst     0x01
#define HT_IsVolatile  0x02
#define HT_IsReference 0x08
#define CT_IsSimpleType    0x01
#define CT_ByReferenceOnly 0x02
#define CT_HasVirtualBase  0x04
#define TI_IsConst    0x01
#define TI_IsVolatile 0x02

typedef struct { int mdisp, pdisp, vdisp; } PMD;
typedef struct TypeDescriptor { const void *vftable; void *spare; char name[1]; } TypeDescriptor;
typedef struct {
    DWORD properties; const TypeDescriptor *pType; PMD thisDisplacement; int sizeOrOffset; void *copyFunction;
} CatchableType;
typedef struct { int nCatchableTypes; const CatchableType *arrayOfCatchableTypes[1]; } CatchableTypeArray;
typedef struct {
    DWORD attributes; void *pmfnUnwind; void *pForwardCompat; const CatchableTypeArray *pCatchableTypeArray;
} ThrowInfo;
typedef struct { int toState; void *action; } UnwindMapEntry;
typedef struct { DWORD adjectives; const TypeDescriptor *pType; int dispCatchObj; void *addressOfHandler; } HandlerType;
typedef struct { int tryLow, tryHigh, catchHigh, nCatches; const HandlerType *pHandlerArray; } TryBlockMapEntry;
typedef struct {
    DWORD magic; int maxState; const UnwindMapEntry *pUnwindMap; DWORD nTryBlocks; const TryBlockMapEntry *pTryBlockMap;
    DWORD nIPMapEntries; void *pIPtoStateMap; void *pESTypeList; int EHFlags;
} FuncInfo;
typedef struct EHNode { struct EHNode *next; void *handler; int state; } EHNode;

void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
static int str_eq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

#define NODE_EBP(n)  ((BYTE *)(n) + 12)
#define NODE_ESP(n)  (*(ULONG_PTR *)((BYTE *)(n) - 4))

/* call code of the function whose frame is at @ebp (a catch block, an
 * unwind action); returns EAX (a catch block's continuation address) */
ULONG_PTR cxx_call_ebp(void *fn, void *ebp) __asm__("cxx_call_ebp");
__asm__(
    ".text\n"
    "cxx_call_ebp:\n\t"
    "pushl %ebp\n\t" "pushl %ebx\n\t" "pushl %esi\n\t" "pushl %edi\n\t"
    "movl 20(%esp), %eax\n\t"
    "movl 24(%esp), %ebp\n\t"
    "pushl %esp\n\t"
    "calll *%eax\n\t"
    "popl %esp\n\t"
    "popl %edi\n\t" "popl %esi\n\t" "popl %ebx\n\t" "popl %ebp\n\t"
    "retl\n");

/* continue at @ip in the frame of @node (EBP, and ESP as saved there) */
__declspec(noreturn) void cxx_jump(void *ip, void *ebp, ULONG_PTR esp) __asm__("cxx_jump");
__asm__(
    ".text\n"
    "cxx_jump:\n\t"
    "movl 4(%esp), %eax\n\t"
    "movl 8(%esp), %ebp\n\t"
    "movl 12(%esp), %esp\n\t"
    "jmpl *%eax\n");

/* member functions of the thrown type: thiscall */
typedef void (__thiscall *Dtor)(void *self);
typedef void (__thiscall *CopyCtor)(void *self, void *src);
typedef void (__thiscall *CopyCtorVb)(void *self, void *src, int most_derived);

/* -----------------------------------------------------------------------
 * Per-thread state
 * ----------------------------------------------------------------------- */
typedef struct CatchRec {
    struct CatchRec *next;
    EHNode *frame;                  /* the frame running the catch */
    void *object;                   /* the exception object */
    const ThrowInfo *ti;
    EXCEPTION_RECORD rec;           /* for rethrow and __current_exception */
    CONTEXT *ctx;
} CatchRec;

typedef struct {
    CatchRec *catches;              /* innermost first */
    int uncaught;                   /* thrown and not yet caught */
    EXCEPTION_RECORD *cur_rec;      /* __current_exception */
    CONTEXT *cur_ctx;
} EhThread;

WINBASEAPI DWORD  WINAPI TlsAlloc(void);
WINBASEAPI LPVOID WINAPI TlsGetValue(DWORD i);
WINBASEAPI BOOL   WINAPI TlsSetValue(DWORD i, LPVOID v);

static volatile DWORD g_tls = 0xFFFFFFFF;

EhThread *vcrt_thread(void)
{
    DWORD slot = g_tls;
    if (slot == 0xFFFFFFFF) {
        DWORD s = TlsAlloc(), expect = 0xFFFFFFFF;
        __atomic_compare_exchange_n(&g_tls, &expect, s, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        slot = g_tls;
    }
    EhThread *t = TlsGetValue(slot);
    if (!t) {
        t = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*t));
        TlsSetValue(slot, t);
    }
    return t;
}

/* -----------------------------------------------------------------------
 * Throwing
 * ----------------------------------------------------------------------- */
WINBASEAPI VOID WINAPI RaiseException(DWORD code, DWORD flags, DWORD nargs, const ULONG_PTR *args);

static __declspec(noreturn) void no_catch(const char *why)
{
    ULONG n = 0;
    while (why[n]) n++;
    NtNovaDebugPrint(why, n);
    DWORD w;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), why, n, &w, 0);
    RtlExitUserProcess(3);                  /* as abort() */
}

VCRT __declspec(noreturn) void __stdcall _CxxThrowException(void *object, const ThrowInfo *ti)
{
    EhThread *t = vcrt_thread();
    ULONG_PTR args[3];
    if (!ti) {                              /* "throw;": the exception being handled */
        CatchRec *c = t ? t->catches : 0;
        if (!c) no_catch("vcruntime: throw; with no exception being handled\n");
        object = c->object;
        ti = c->ti;
    }
    args[0] = CXX_MAGIC_MIN;
    args[1] = (ULONG_PTR)object;
    args[2] = (ULONG_PTR)ti;
    if (t) t->uncaught++;
    RaiseException(CXX_EXCEPTION, EXCEPTION_NONCONTINUABLE, 3, args);
    no_catch("vcruntime: exception not handled\n");
}

/* -----------------------------------------------------------------------
 * Objects
 * ----------------------------------------------------------------------- */
static void *adjust(void *p, const PMD *pmd)
{
    char *r = (char *)p + pmd->mdisp;
    if (pmd->pdisp >= 0) {
        char *vbtable = *(char **)((char *)p + pmd->pdisp);
        r += *(int *)(vbtable + pmd->vdisp) + pmd->pdisp;
    }
    return r;
}

static void destroy_object(void *obj, const ThrowInfo *ti)
{
    if (obj && ti && ti->pmfnUnwind) ((Dtor)ti->pmfnUnwind)(obj);
}

/* Abandon catches that were running in @frame (an exception left them) */
static void drop_catches(EhThread *t, EHNode *frame, void *in_flight)
{
    CatchRec **pp = &t->catches;
    while (*pp) {
        CatchRec *c = *pp;
        if (c->frame != frame) { pp = &c->next; continue; }
        *pp = c->next;
        if (c->object != in_flight) destroy_object(c->object, c->ti);
        RtlFreeHeap(RtlGetProcessHeap(), 0, c);
    }
    t->cur_rec = t->catches ? &t->catches->rec : 0;
}

static const CatchableType *match(const HandlerType *h, const ThrowInfo *ti)
{
    const CatchableTypeArray *cta = ti->pCatchableTypeArray;
    if (!h->pType || !h->pType->name[0]) return cta->arrayOfCatchableTypes[0];      /* catch (...) */
    for (int i = 0; i < cta->nCatchableTypes; i++) {
        const CatchableType *ct = cta->arrayOfCatchableTypes[i];
        if (ct->pType != h->pType && !str_eq(ct->pType->name, h->pType->name)) continue;
        if ((ct->properties & CT_ByReferenceOnly) && !(h->adjectives & HT_IsReference)) continue;
        if ((ti->attributes & TI_IsConst) && !(h->adjectives & HT_IsConst)) continue;
        if ((ti->attributes & TI_IsVolatile) && !(h->adjectives & HT_IsVolatile)) continue;
        return ct;
    }
    return 0;
}

static void build_catch_object(const HandlerType *h, BYTE *ebp, void *obj, const CatchableType *ct)
{
    if (!h->pType || !h->pType->name[0] || !h->dispCatchObj) return;   /* catch (...) or unnamed */
    void **slot = (void **)(ebp + h->dispCatchObj);
    int is_ptr = ct->pType->name[0] == '.' && ct->pType->name[1] == 'P';
    if (h->adjectives & HT_IsReference) {
        *slot = adjust(obj, &ct->thisDisplacement);
    } else if (ct->properties & CT_IsSimpleType) {
        memcpy(slot, obj, (size_t)ct->sizeOrOffset);
        if (is_ptr && *slot) *slot = adjust(*slot, &ct->thisDisplacement);
    } else if (ct->copyFunction) {
        void *src = adjust(obj, &ct->thisDisplacement);
        if (ct->properties & CT_HasVirtualBase) ((CopyCtorVb)ct->copyFunction)(slot, src, 1);
        else ((CopyCtor)ct->copyFunction)(slot, src);
    } else {
        memcpy(slot, adjust(obj, &ct->thisDisplacement), (size_t)ct->sizeOrOffset);
    }
}

/* Run the unwind actions (destructors) of @node's frame from its current
 * state down to @to */
static void unwind_frame(EHNode *node, const FuncInfo *fi, int to)
{
    while (node->state > to && node->state < fi->maxState) {
        const UnwindMapEntry *e = &fi->pUnwindMap[node->state];
        node->state = e->toState;
        if (e->action) cxx_call_ebp(e->action, NODE_EBP(node));
    }
}

/* -----------------------------------------------------------------------
 * The frame handler
 * ----------------------------------------------------------------------- */
static __declspec(noreturn) void catch_it(EXCEPTION_RECORD *rec, EHNode *node, CONTEXT *ctx, const FuncInfo *fi,
                                          const TryBlockMapEntry *tb, const HandlerType *h,
                                          const CatchableType *ct)
{
    EhThread *t = vcrt_thread();
    void *obj = (void *)rec->ExceptionInformation[1];
    const ThrowInfo *ti = (const ThrowInfo *)rec->ExceptionInformation[2];

    build_catch_object(h, NODE_EBP(node), obj, ct);
    RtlUnwind(node, 0, rec, 0);                      /* the frames above this one */
    rec->ExceptionFlags &= ~EXC_UNWIND_MASK;
    unwind_frame(node, fi, tb->tryLow);              /* this one, down to the try block */

    CatchRec *c = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*c));
    if (!c) no_catch("vcruntime: out of memory in catch\n");
    c->frame = node;
    c->object = obj;
    c->ti = ti;
    c->rec = *rec;
    c->ctx = ctx;
    c->next = t->catches;
    t->catches = c;
    t->cur_rec = &c->rec;
    t->cur_ctx = ctx;
    if (t->uncaught > 0) t->uncaught--;

    node->state = tb->tryHigh + 1;                   /* in the catch block */
    ULONG_PTR esp = NODE_ESP(node);                  /* the catch block stores its own ESP there */
    void *cont = (void *)cxx_call_ebp(h->addressOfHandler, NODE_EBP(node));
    NODE_ESP(node) = esp;

    /* the catch ended normally: the exception is over */
    t->catches = c->next;
    t->cur_rec = t->catches ? &t->catches->rec : 0;
    t->cur_ctx = t->catches ? t->catches->ctx : 0;
    destroy_object(obj, ti);
    RtlFreeHeap(RtlGetProcessHeap(), 0, c);
    node->state = tb->tryLow >= 0 && tb->tryLow < fi->maxState ? fi->pUnwindMap[tb->tryLow].toState : -1;
    cxx_jump(cont, NODE_EBP(node), NODE_ESP(node));
}

EXCEPTION_DISPOSITION cxx_frame_handler(EXCEPTION_RECORD *rec, EHNode *node, CONTEXT *ctx, void *dc,
                                               const FuncInfo *fi) __asm__("cxx_frame_handler");
EXCEPTION_DISPOSITION cxx_frame_handler(EXCEPTION_RECORD *rec, EHNode *node, CONTEXT *ctx, void *dc,
                                               const FuncInfo *fi)
{
    (void)dc;
    DWORD magic = fi->magic & 0x1FFFFFFF;
    if (magic < CXX_MAGIC_MIN || magic > CXX_MAGIC_MAX) return ExceptionContinueSearch;
    EhThread *t = vcrt_thread();

    if (rec->ExceptionFlags & EXC_UNWIND_MASK) {
        void *in_flight = rec->ExceptionCode == CXX_EXCEPTION && rec->NumberParameters >= 3
                          ? (void *)rec->ExceptionInformation[1] : 0;
        drop_catches(t, node, in_flight);
        unwind_frame(node, fi, -1);
        return ExceptionContinueSearch;
    }

    if (rec->ExceptionCode != CXX_EXCEPTION || rec->NumberParameters < 3) return ExceptionContinueSearch;
    DWORD m = (DWORD)rec->ExceptionInformation[0];
    if (m < CXX_MAGIC_MIN || m > CXX_MAGIC_MAX) return ExceptionContinueSearch;
    const ThrowInfo *ti = (const ThrowInfo *)rec->ExceptionInformation[2];
    if (!ti) return ExceptionContinueSearch;

    int state = node->state;
    for (DWORD i = 0; i < fi->nTryBlocks; i++) {
        const TryBlockMapEntry *tb = &fi->pTryBlockMap[i];
        if (state < tb->tryLow || state > tb->tryHigh) continue;
        for (int k = 0; k < tb->nCatches; k++) {
            const CatchableType *ct = match(&tb->pHandlerArray[k], ti);
            if (ct) catch_it(rec, node, ctx, fi, tb, &tb->pHandlerArray[k], ct);
        }
    }
    return ExceptionContinueSearch;
}

/* __CxxFrameHandler3(rec, node, ctx, dc) with EAX = FuncInfo */
__asm__(
    ".text\n"
    ".globl ___CxxFrameHandler3\n.globl ___CxxFrameHandler2\n.globl ___CxxFrameHandler\n"
    "___CxxFrameHandler3:\n___CxxFrameHandler2:\n___CxxFrameHandler:\n\t"
    "pushl %eax\n\t"
    "pushl 20(%esp)\n\t" "pushl 20(%esp)\n\t" "pushl 20(%esp)\n\t" "pushl 20(%esp)\n\t"
    "calll cxx_frame_handler\n\t"
    "addl $20, %esp\n\t"
    "retl\n"
    ".section .drectve,\"yn\"\n\t"
    ".ascii \" /EXPORT:___CxxFrameHandler3 /EXPORT:___CxxFrameHandler2 /EXPORT:___CxxFrameHandler\"\n\t"
    ".text\n");

/* -----------------------------------------------------------------------
 * Queries about the current exception
 * ----------------------------------------------------------------------- */
VCRT void **__current_exception(void)         { return (void **)&vcrt_thread()->cur_rec; }
VCRT void **__current_exception_context(void) { return (void **)&vcrt_thread()->cur_ctx; }
VCRT int *__processing_throw(void)            { return &vcrt_thread()->uncaught; }
VCRT int __uncaught_exceptions(void)          { return vcrt_thread()->uncaught; }
VCRT BOOL __uncaught_exception(void)          { return vcrt_thread()->uncaught > 0; }

VCRT void __DestructExceptionObject(EXCEPTION_RECORD *rec)
{
    if (rec && rec->ExceptionCode == CXX_EXCEPTION && rec->NumberParameters >= 3)
        destroy_object((void *)rec->ExceptionInformation[1], (const ThrowInfo *)rec->ExceptionInformation[2]);
}

VCRT void *__AdjustPointer(void *p, const PMD *pmd) { return adjust(p, pmd); }

VCRT int _is_exception_typeof(const TypeDescriptor *type, EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *rec = ep->ExceptionRecord;
    if (rec->ExceptionCode != CXX_EXCEPTION || rec->NumberParameters < 3) return 0;
    const ThrowInfo *ti = (const ThrowInfo *)rec->ExceptionInformation[2];
    const CatchableTypeArray *cta = ti->pCatchableTypeArray;
    for (int i = 0; i < cta->nCatchableTypes; i++)
        if (str_eq(cta->arrayOfCatchableTypes[i]->pType->name, type->name)) return 1;
    return 0;
}

/* SEH helpers vcruntime140 carries on x86 (ntdll has them).  The linker
 * drops one leading underscore from each /EXPORT name, hence two. */
__asm__(".section .drectve,\"yn\"\n\t"
        ".ascii \" /EXPORT:__except_handler4_common=ntdll._except_handler4_common\"\n\t"
        ".ascii \" /EXPORT:__except_handler2=ntdll._except_handler2 /EXPORT:__except_handler3=ntdll._except_handler3\"\n\t"
        ".ascii \" /EXPORT:__global_unwind2=ntdll._global_unwind2 /EXPORT:__local_unwind2=ntdll._local_unwind2\"\n\t"
        ".ascii \" /EXPORT:__local_unwind4=ntdll._local_unwind4 /EXPORT:__setjmp3=ucrtbase._setjmp3\"\n\t"
        ".text\n");
