/*
 * eh.c — C++ exception handling for programs built with MSVC or clang-cl
 * (vcruntime140.dll): _CxxThrowException and __CxxFrameHandler3.
 *
 * A throw raises exception 0xE06D7363 with the object and its ThrowInfo.
 * Each C++ frame's handler reads the function's FuncInfo tables (the
 * current EH state comes from the IP-to-state map), and when a try block's
 * catch matches the thrown type it
 *   1. constructs the catch object in the catching frame,
 *   2. unwinds the frames in between (their destructors run), and the
 *      catching frame down to the try block's state,
 *   3. calls the catch funclet — still deep in the stack, so the thrown
 *      object (in a dead frame) stays intact — through nova_call_catch,
 *      whose unwind information leads straight back to the catching frame,
 *   4. destroys the exception object and resumes at the address the catch
 *      funclet returned.
 * While a catch runs, its frame's state is "outside the try" (a per-thread
 * record), so an exception thrown from the catch is not caught by it.
 */

#ifdef _WIN64                   /* 32-bit programs: eh_x86.h */
#include <winternl.h>
#include <winnt.h>

#define VCRT __declspec(dllexport)

#define CXX_EXCEPTION     0xE06D7363u
#define CXX_MAGIC_MIN     0x19930520u
#define CXX_MAGIC_MAX     0x19930522u
#define EXC_UNWIND_MASK   (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)
#define EXC_TARGET_UNWIND 0x20

/* HandlerType.adjectives */
#define HT_IsConst     0x01
#define HT_IsVolatile  0x02
#define HT_IsReference 0x08
/* CatchableType.properties */
#define CT_IsSimpleType    0x01
#define CT_ByReferenceOnly 0x02
#define CT_HasVirtualBase  0x04
/* ThrowInfo.attributes */
#define TI_IsConst    0x01
#define TI_IsVolatile 0x02

typedef struct { int mdisp, pdisp, vdisp; } PMD;
typedef struct { DWORD properties; int pType; PMD thisDisplacement; int sizeOrOffset; int copyFunction; } CatchableType;
typedef struct { int nCatchableTypes; int arrayOfCatchableTypes[1]; } CatchableTypeArray;
typedef struct { DWORD attributes; int pmfnUnwind; int pForwardCompat; int pCatchableTypeArray; } ThrowInfo;
typedef struct { const void *vftable; void *spare; char name[1]; } TypeDescriptor;
typedef struct {
    DWORD magic; int maxState; int dispUnwindMap; DWORD nTryBlocks; int dispTryBlockMap;
    DWORD nIPMapEntries; int dispIPtoStateMap; int dispUnwindHelp; int dispESTypeList; int EHFlags;
} FuncInfo;
typedef struct { int toState; int action; } UnwindMapEntry;
typedef struct { int tryLow, tryHigh, catchHigh, nCatches, dispHandlerArray; } TryBlockMapEntry;
typedef struct { DWORD adjectives; int dispType; int dispCatchObj; int dispOfHandler; int dispFrame; } HandlerType;
typedef struct { int ip; int state; } IPtoState;

void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
static int str_eq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

/* -----------------------------------------------------------------------
 * Per-thread state
 * ----------------------------------------------------------------------- */
typedef struct CatchRec {
    struct CatchRec *next;
    DWORD64 frame;                  /* the frame the catch belongs to (establisher) */
    int enclosing;                  /* that frame's EH state while the catch runs */
    void *object;                   /* the exception object */
    const ThrowInfo *ti;
    DWORD64 ti_base;
    EXCEPTION_RECORD rec;           /* for rethrow and __current_exception */
    CONTEXT *ctx;
} CatchRec;

typedef struct {
    CatchRec *catches;              /* innermost first */
    int uncaught;                   /* thrown and not yet caught */
    int target_state;               /* the state the catching frame unwinds to */
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
        if (!__atomic_compare_exchange_n(&g_tls, &expect, s, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            /* another thread won; the slot we got stays unused */
        }
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
WINBASEAPI VOID WINAPI ExitProcess(UINT code);

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
    ULONG_PTR args[4];
    if (!ti) {                              /* "throw;": the exception being handled */
        CatchRec *c = t ? t->catches : 0;
        if (!c) no_catch("vcruntime: throw; with no exception being handled\n");
        object = c->object;
        ti = c->ti;
        args[3] = c->ti_base;
    } else {
        PVOID base = 0;
        RtlPcToFileHeader((PVOID)ti, &base);
        args[3] = (ULONG_PTR)base;
    }
    args[0] = CXX_MAGIC_MIN;
    args[1] = (ULONG_PTR)object;
    args[2] = (ULONG_PTR)ti;
    if (t) t->uncaught++;
    RaiseException(CXX_EXCEPTION, EXCEPTION_NONCONTINUABLE, 4, args);
    no_catch("vcruntime: exception not handled\n");
}

/* -----------------------------------------------------------------------
 * Frame state
 * ----------------------------------------------------------------------- */
static int state_from_ip(DWORD64 base, const FuncInfo *fi, DWORD rva)
{
    const IPtoState *m = (const IPtoState *)(base + fi->dispIPtoStateMap);
    int state = -1;
    for (DWORD i = 0; i < fi->nIPMapEntries; i++) {
        if ((DWORD)m[i].ip > rva) break;
        state = m[i].state;
    }
    return state;
}

static int frame_state(EhThread *t, DWORD64 base, const FuncInfo *fi, DWORD64 pc, DWORD64 frame)
{
    for (CatchRec *c = t ? t->catches : 0; c; c = c->next)
        if (c->frame == frame) return c->enclosing;
    return state_from_ip(base, fi, (DWORD)(pc - base));
}

typedef void (*Funclet)(void *unused, DWORD64 frame);

/* Run the unwind actions (destructors) from @cur down to @to */
static void unwind_states(DWORD64 base, const FuncInfo *fi, DWORD64 parent, int cur, int to)
{
    const UnwindMapEntry *um = (const UnwindMapEntry *)(base + fi->dispUnwindMap);
    while (cur > to && cur < fi->maxState) {
        int next = um[cur].toState;
        if (um[cur].action) ((Funclet)(base + um[cur].action))(0, parent);
        cur = next;
    }
}

static void destroy_object(void *obj, const ThrowInfo *ti, DWORD64 ti_base)
{
    if (obj && ti && ti->pmfnUnwind) ((void (*)(void *))(ti_base + ti->pmfnUnwind))(obj);
}

/* Abandon catches that were running in @frame (an exception left them) */
static void drop_catches(EhThread *t, DWORD64 frame, void *in_flight)
{
    CatchRec **pp = &t->catches;
    while (*pp) {
        CatchRec *c = *pp;
        if (c->frame != frame) { pp = &c->next; continue; }
        *pp = c->next;
        if (c->object != in_flight) destroy_object(c->object, c->ti, c->ti_base);
        RtlFreeHeap(RtlGetProcessHeap(), 0, c);
    }
    t->cur_rec = t->catches ? &t->catches->rec : 0;
}

/* -----------------------------------------------------------------------
 * Type matching and the catch object
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

static const CatchableType *match(DWORD64 base, const HandlerType *h, const ThrowInfo *ti, DWORD64 ti_base)
{
    const CatchableTypeArray *cta = (const CatchableTypeArray *)(ti_base + ti->pCatchableTypeArray);
    if (!h->dispType) return (const CatchableType *)(ti_base + cta->arrayOfCatchableTypes[0]);   /* catch (...) */
    const TypeDescriptor *want = (const TypeDescriptor *)(base + h->dispType);
    if (!want->name[0]) return (const CatchableType *)(ti_base + cta->arrayOfCatchableTypes[0]);
    for (int i = 0; i < cta->nCatchableTypes; i++) {
        const CatchableType *ct = (const CatchableType *)(ti_base + cta->arrayOfCatchableTypes[i]);
        const TypeDescriptor *td = (const TypeDescriptor *)(ti_base + ct->pType);
        if (td != want && !str_eq(td->name, want->name)) continue;
        if ((ct->properties & CT_ByReferenceOnly) && !(h->adjectives & HT_IsReference)) continue;
        if ((ti->attributes & TI_IsConst) && !(h->adjectives & HT_IsConst)) continue;
        if ((ti->attributes & TI_IsVolatile) && !(h->adjectives & HT_IsVolatile)) continue;
        return ct;
    }
    return 0;
}

static void build_catch_object(DWORD64 base, const HandlerType *h, DWORD64 parent, void *obj,
                               const CatchableType *ct, DWORD64 ti_base)
{
    if (!h->dispType || !h->dispCatchObj) return;           /* catch (...) or unnamed */
    const TypeDescriptor *want = (const TypeDescriptor *)(base + h->dispType);
    if (!want->name[0]) return;
    void **slot = (void **)(parent + h->dispCatchObj);
    const TypeDescriptor *td = (const TypeDescriptor *)(ti_base + ct->pType);
    int is_ptr = td->name[0] == '.' && td->name[1] == 'P';
    if (h->adjectives & HT_IsReference) {
        *slot = adjust(obj, &ct->thisDisplacement);
    } else if (ct->properties & CT_IsSimpleType) {
        memcpy(slot, obj, (size_t)ct->sizeOrOffset);
        if (is_ptr && *slot) *slot = adjust(*slot, &ct->thisDisplacement);
    } else if (ct->copyFunction) {
        void *src = adjust(obj, &ct->thisDisplacement);
        if (ct->properties & CT_HasVirtualBase)
            ((void (*)(void *, void *, int))(ti_base + ct->copyFunction))(slot, src, 1);
        else
            ((void (*)(void *, void *))(ti_base + ct->copyFunction))(slot, src);
    } else {
        memcpy(slot, adjust(obj, &ct->thisDisplacement), (size_t)ct->sizeOrOffset);
    }
}

/* -----------------------------------------------------------------------
 * Unwinding and calling the catch
 * ----------------------------------------------------------------------- */
/* Unwind from @start up to the frame @target, running each frame's
 * termination handler; *out gets the target frame's registers. */
static BOOL unwind_to(DWORD64 target, EXCEPTION_RECORD *rec, const CONTEXT *start, CONTEXT *out)
{
    CONTEXT cur = *start;
    rec->ExceptionFlags |= EXCEPTION_UNWINDING;
    for (int guard = 0; guard < 100000; guard++) {
        DWORD64 base = 0, frame = 0;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(cur.Rip, &base, 0);
        if (!f) {
            if (!cur.Rsp || (cur.Rsp & 7)) return FALSE;
            cur.Rip = *(DWORD64 *)cur.Rsp;
            cur.Rsp += 8;
            if (!cur.Rip) return FALSE;
            continue;
        }
        CONTEXT before = cur;
        PVOID hdata = 0;
        PEXCEPTION_ROUTINE h = RtlVirtualUnwind(2 /* UNW_FLAG_UHANDLER */, base, before.Rip, f, &cur, &hdata, &frame, 0);
        BOOL is_target = frame == target;
        if (h) {
            DISPATCHER_CONTEXT dc;
            memset(&dc, 0, sizeof(dc));
            dc.ControlPc = before.Rip;
            dc.ImageBase = base;
            dc.FunctionEntry = f;
            dc.EstablisherFrame = frame;
            dc.ContextRecord = &before;
            dc.LanguageHandler = h;
            dc.HandlerData = hdata;
            DWORD saved = rec->ExceptionFlags;
            if (is_target) rec->ExceptionFlags |= EXC_TARGET_UNWIND;
            h(rec, (PVOID)frame, &before, &dc);
            rec->ExceptionFlags = saved;
        }
        if (is_target) { *out = before; return TRUE; }
        if (frame > target || (cur.Rip == before.Rip && cur.Rsp == before.Rsp)) return FALSE;
    }
    return FALSE;
}

/* The catching frame's registers, for nova_call_catch's unwind information */
typedef struct { DWORD64 rbx, rbp, rsi, rdi, r12, r13, r14, r15, rip, rsp; } FrameRegs;

/* DWORD64 nova_call_catch(funclet, parent frame, const FrameRegs *):
 * calls funclet(funclet, parent) and returns its continuation address.
 * Its frame unwinds (machine frame + saved registers) to the catching
 * frame, skipping the dead frames and the dispatcher below it. */
DWORD64 nova_call_catch(DWORD64 funclet, DWORD64 parent, const FrameRegs *r);
__asm__(
    ".text\n"
    ".globl nova_call_catch\n"
    ".def nova_call_catch; .scl 2; .type 32; .endef\n"
    ".seh_proc nova_call_catch\n"
    "nova_call_catch:\n\t"
    ".seh_pushframe\n\t"
    "subq $0x68, %rsp\n\t"
    ".seh_stackalloc 0x68\n\t"
    ".seh_savereg %rbx, 0x20\n\t"
    ".seh_savereg %rbp, 0x28\n\t"
    ".seh_savereg %rsi, 0x30\n\t"
    ".seh_savereg %rdi, 0x38\n\t"
    ".seh_savereg %r12, 0x40\n\t"
    ".seh_savereg %r13, 0x48\n\t"
    ".seh_savereg %r14, 0x50\n\t"
    ".seh_savereg %r15, 0x58\n\t"
    ".seh_endprologue\n\t"
    "movq 0x68(%rsp), %rax\n\t"          /* our return address, kept aside */
    "movq %rax, 0x60(%rsp)\n\t"
    "movq 0x00(%r8), %rax\n\t movq %rax, 0x20(%rsp)\n\t"
    "movq 0x08(%r8), %rax\n\t movq %rax, 0x28(%rsp)\n\t"
    "movq 0x10(%r8), %rax\n\t movq %rax, 0x30(%rsp)\n\t"
    "movq 0x18(%r8), %rax\n\t movq %rax, 0x38(%rsp)\n\t"
    "movq 0x20(%r8), %rax\n\t movq %rax, 0x40(%rsp)\n\t"
    "movq 0x28(%r8), %rax\n\t movq %rax, 0x48(%rsp)\n\t"
    "movq 0x30(%r8), %rax\n\t movq %rax, 0x50(%rsp)\n\t"
    "movq 0x38(%r8), %rax\n\t movq %rax, 0x58(%rsp)\n\t"
    "movq 0x40(%r8), %rax\n\t movq %rax, 0x68(%rsp)\n\t"     /* machine frame: RIP */
    "movq 0x48(%r8), %rax\n\t movq %rax, 0x80(%rsp)\n\t"     /* machine frame: RSP */
    "movq %rcx, %rax\n\t"
    "callq *%rax\n\t"                    /* funclet(RCX = funclet, RDX = parent frame) */
    "movq 0x60(%rsp), %rcx\n\t"
    "movq %rcx, 0x68(%rsp)\n\t"
    "addq $0x68, %rsp\n\t"
    "retq\n\t"
    ".seh_endproc\n");

static __declspec(noreturn) void catch_it(EXCEPTION_RECORD *rec, DWORD64 target, DWORD64 parent, CONTEXT *ctx,
                                          DWORD64 base, const FuncInfo *fi, const TryBlockMapEntry *tb,
                                          const HandlerType *h, const CatchableType *ct)
{
    EhThread *t = vcrt_thread();
    void *obj = (void *)rec->ExceptionInformation[1];
    const ThrowInfo *ti = (const ThrowInfo *)rec->ExceptionInformation[2];
    DWORD64 ti_base = rec->ExceptionInformation[3];

    build_catch_object(base, h, parent, obj, ct, ti_base);

    /* unwind the frames in between, and this one down to the try block */
    EXCEPTION_RECORD urec = *rec;
    CONTEXT tctx;
    t->target_state = tb->tryLow;
    if (!unwind_to(target, &urec, ctx, &tctx)) no_catch("vcruntime: unwinding to the catch failed\n");

    CatchRec *c = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*c));
    if (!c) no_catch("vcruntime: out of memory in catch\n");
    const UnwindMapEntry *um = (const UnwindMapEntry *)(base + fi->dispUnwindMap);
    c->frame = target;
    c->enclosing = tb->tryLow >= 0 && tb->tryLow < fi->maxState ? um[tb->tryLow].toState : -1;
    c->object = obj;
    c->ti = ti;
    c->ti_base = ti_base;
    c->rec = *rec;
    c->rec.ExceptionFlags &= ~EXC_UNWIND_MASK;
    c->ctx = ctx;
    c->next = t->catches;
    t->catches = c;
    t->cur_rec = &c->rec;
    t->cur_ctx = ctx;
    if (t->uncaught > 0) t->uncaught--;

    FrameRegs fr = { tctx.Rbx, tctx.Rbp, tctx.Rsi, tctx.Rdi, tctx.R12, tctx.R13, tctx.R14, tctx.R15, tctx.Rip, tctx.Rsp };
    DWORD64 cont = nova_call_catch(base + (DWORD)h->dispOfHandler, parent, &fr);

    /* the catch ended normally: the exception is over */
    t->catches = c->next;
    t->cur_rec = t->catches ? &t->catches->rec : 0;
    t->cur_ctx = t->catches ? t->catches->ctx : 0;
    destroy_object(obj, ti, ti_base);
    RtlFreeHeap(RtlGetProcessHeap(), 0, c);

    tctx.Rip = cont;
    NtContinue(&tctx, FALSE);
    no_catch("vcruntime: resuming after catch failed\n");
}

/* -----------------------------------------------------------------------
 * The frame handler
 * ----------------------------------------------------------------------- */
VCRT EXCEPTION_DISPOSITION __CxxFrameHandler3(EXCEPTION_RECORD *rec, PVOID establisher, CONTEXT *ctx, DISPATCHER_CONTEXT *dc)
{
    DWORD64 base = dc->ImageBase, frame = (DWORD64)establisher;
    const FuncInfo *fi = (const FuncInfo *)(base + *(DWORD *)dc->HandlerData);
    DWORD magic = fi->magic & 0x1FFFFFFF;
    if (magic < CXX_MAGIC_MIN || magic > CXX_MAGIC_MAX) return ExceptionContinueSearch;
    EhThread *t = vcrt_thread();
    const TryBlockMapEntry *tbm = (const TryBlockMapEntry *)(base + fi->dispTryBlockMap);

    /* A catch funclet?  Its locals live in the parent function's frame. */
    DWORD begin = dc->FunctionEntry->BeginAddress;
    int funclet_try = -1;
    DWORD64 parent = frame;
    for (DWORD i = 0; i < fi->nTryBlocks && funclet_try < 0; i++) {
        const HandlerType *ha = (const HandlerType *)(base + tbm[i].dispHandlerArray);
        for (int k = 0; k < tbm[i].nCatches; k++)
            if ((DWORD)ha[k].dispOfHandler == begin) {
                funclet_try = (int)i;
                parent = *(DWORD64 *)(frame + ha[k].dispFrame);
                break;
            }
    }
    int state = frame_state(t, base, fi, dc->ControlPc, frame);

    if (rec->ExceptionFlags & EXC_UNWIND_MASK) {
        void *in_flight = rec->ExceptionCode == CXX_EXCEPTION && rec->NumberParameters >= 3
                          ? (void *)rec->ExceptionInformation[1] : 0;
        drop_catches(t, frame, in_flight);
        if (rec->ExceptionFlags & EXC_TARGET_UNWIND) {
            unwind_states(base, fi, parent, state, t->target_state);
        } else if (funclet_try >= 0) {
            /* only the catch block's own objects; the parent frame does the rest */
            const TryBlockMapEntry *tb = &tbm[funclet_try];
            const UnwindMapEntry *um = (const UnwindMapEntry *)(base + fi->dispUnwindMap);
            while (state > tb->tryHigh && state <= tb->catchHigh && state < fi->maxState) {
                int next = um[state].toState;
                if (um[state].action) ((Funclet)(base + um[state].action))(0, parent);
                state = next;
            }
        } else {
            unwind_states(base, fi, parent, state, -1);
        }
        return ExceptionContinueSearch;
    }

    if (rec->ExceptionCode != CXX_EXCEPTION || rec->NumberParameters < 3) return ExceptionContinueSearch;
    DWORD m = (DWORD)rec->ExceptionInformation[0];
    if (m < CXX_MAGIC_MIN || m > CXX_MAGIC_MAX) return ExceptionContinueSearch;
    const ThrowInfo *ti = (const ThrowInfo *)rec->ExceptionInformation[2];
    DWORD64 ti_base = rec->NumberParameters >= 4 ? rec->ExceptionInformation[3] : 0;
    if (!ti) return ExceptionContinueSearch;

    for (DWORD i = 0; i < fi->nTryBlocks; i++) {
        const TryBlockMapEntry *tb = &tbm[i];
        if (state < tb->tryLow || state > tb->tryHigh) continue;
        /* a catch funclet owns only the try blocks inside its catch; the
         * ones around it belong to the parent frame, visited next */
        if (funclet_try >= 0 && (tb->tryLow <= tbm[funclet_try].tryHigh || tb->tryHigh > tbm[funclet_try].catchHigh))
            continue;
        const HandlerType *ha = (const HandlerType *)(base + tb->dispHandlerArray);
        for (int k = 0; k < tb->nCatches; k++) {
            const CatchableType *ct = match(base, &ha[k], ti, ti_base);
            if (ct) catch_it(rec, frame, parent, ctx, base, fi, tb, &ha[k], ct);
        }
    }
    return ExceptionContinueSearch;
}

VCRT EXCEPTION_DISPOSITION __CxxFrameHandler(EXCEPTION_RECORD *rec, PVOID frame, CONTEXT *ctx, DISPATCHER_CONTEXT *dc)
{
    return __CxxFrameHandler3(rec, frame, ctx, dc);
}

VCRT EXCEPTION_DISPOSITION __CxxFrameHandler2(EXCEPTION_RECORD *rec, PVOID frame, CONTEXT *ctx, DISPATCHER_CONTEXT *dc)
{
    return __CxxFrameHandler3(rec, frame, ctx, dc);
}

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
        destroy_object((void *)rec->ExceptionInformation[1], (const ThrowInfo *)rec->ExceptionInformation[2],
                       rec->NumberParameters >= 4 ? rec->ExceptionInformation[3] : 0);
}

VCRT void *__AdjustPointer(void *p, const PMD *pmd) { return adjust(p, pmd); }

VCRT int _is_exception_typeof(const TypeDescriptor *type, EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *rec = ep->ExceptionRecord;
    if (rec->ExceptionCode != CXX_EXCEPTION || rec->NumberParameters < 4) return 0;
    const ThrowInfo *ti = (const ThrowInfo *)rec->ExceptionInformation[2];
    DWORD64 b = rec->ExceptionInformation[3];
    const CatchableTypeArray *cta = (const CatchableTypeArray *)(b + ti->pCatchableTypeArray);
    for (int i = 0; i < cta->nCatchableTypes; i++) {
        const CatchableType *ct = (const CatchableType *)(b + cta->arrayOfCatchableTypes[i]);
        if (str_eq(((const TypeDescriptor *)(b + ct->pType))->name, type->name)) return 1;
    }
    return 0;
}

/* __try-style helpers some compilers call */
VCRT void _local_unwind(void *frame, void *target) { RtlUnwind(frame, target, 0, 0); }

#else
#include "eh_x86.h"
#endif
