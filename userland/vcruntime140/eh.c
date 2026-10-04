/*
 * eh.c — C++ exception handling for programs built with MSVC or clang-cl
 * (vcruntime140.dll): _CxxThrowException, __CxxFrameHandler3 and, for
 * vcruntime140_1.dll, __CxxFrameHandler4.
 *
 * A throw raises exception 0xE06D7363 with the object and its ThrowInfo.
 * Each C++ frame's handler reads the function's tables: the FuncInfo of
 * __CxxFrameHandler3, or the compressed FuncInfo4 MSVC 2019 and later
 * emit for __CxxFrameHandler4 (both are decoded into one EhFunc, below).
 * The current EH state comes from the IP-to-state map, and when a try
 * block's catch matches the thrown type it
 *   1. constructs the catch object in the catching frame,
 *   2. unwinds the frames in between (their destructors run), and the
 *      catching frame down to the try block's state,
 *   3. calls the catch funclet — still deep in the stack, so the thrown
 *      object (in a dead frame) stays intact — through nova_call_catch,
 *      whose unwind information leads straight back to the catching frame,
 *   4. destroys the exception object and resumes at the address the catch
 *      funclet returned (FH4: or at the continuation address whose index
 *      it returned).
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
 * A function's EH tables, decoded
 *
 * FH4 (FuncInfo4) packs the same information as FH3's FuncInfo: a header
 * byte of flags, then RVAs of the unwind map, try block map and IP-to-state
 * map, all with variable-length integers.  Unwind map entries link to the
 * state they unwind to by a backwards byte offset, and each catch funclet
 * has tables of its own (its states are its own, its objects live in the
 * parent function's frame, found at a fixed offset in the funclet's frame).
 * ----------------------------------------------------------------------- */
enum { UW_NONE, UW_DTOR_OBJ, UW_DTOR_PTR, UW_FUNCLET };
typedef struct { int to; int kind; DWORD action; DWORD object; } UwEntry;
typedef struct {
    DWORD adjectives, type;         /* type: TypeDescriptor RVA, 0 for catch (...) */
    int catch_obj;                  /* frame offset of the catch object, 0: none */
    DWORD handler;                  /* the catch funclet */
    int frame;                      /* FH3: where the funclet keeps the parent frame */
    int ncont; DWORD cont[2];       /* FH4: continuation RVAs the funclet returns an index into */
} Catch;
typedef struct { int lo, hi, catch_high, ncatches; Catch *catches; } TryBlock;
typedef struct {
    DWORD64 base;
    int fh4;
    int nstates; UwEntry *uw;
    int ntry; TryBlock *tb;
    int state;                      /* at the dispatcher's ControlPc */
    DWORD64 parent;                 /* the frame the locals are in */
    int funclet_try;                /* FH3: a catch funclet of this try block, else -1 */
    BOOL noexcept;                  /* FH4: an exception may not leave the function */
} EhFunc;

#define FI4_IS_CATCH     0x01
#define FI4_IS_SEPARATED 0x02
#define FI4_BBT          0x04
#define FI4_UNWIND_MAP   0x08
#define FI4_TRYBLOCK_MAP 0x10
#define FI4_NOEXCEPT     0x40
#define CB4_ADJECTIVES   0x01
#define CB4_TYPE         0x02
#define CB4_CATCH_OBJ    0x04
#define CB4_SEPARATED    0x08
#define CB4_CONT_SHIFT   4

static void *eh_alloc(SIZE_T n) { return RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, n ? n : 1); }
static void eh_free(EhFunc *f)
{
    for (int i = 0; i < f->ntry; i++) RtlFreeHeap(RtlGetProcessHeap(), 0, f->tb[i].catches);
    RtlFreeHeap(RtlGetProcessHeap(), 0, f->tb);
    RtlFreeHeap(RtlGetProcessHeap(), 0, f->uw);
}

/* FH4's compressed unsigned integers: the low bits of the first byte say
 * how many bytes follow */
static DWORD fh4_uint(const BYTE **pp)
{
    const BYTE *p = *pp;
    DWORD v;
    if (!(p[0] & 1))             { v = p[0] >> 1; p += 1; }
    else if ((p[0] & 3) == 1)    { v = (p[0] >> 2) | (p[1] << 6); p += 2; }
    else if ((p[0] & 7) == 3)    { v = (p[0] >> 3) | (p[1] << 5) | ((DWORD)p[2] << 13); p += 3; }
    else if ((p[0] & 15) == 7)   { v = (p[0] >> 4) | (p[1] << 4) | ((DWORD)p[2] << 12) | ((DWORD)p[3] << 20); p += 4; }
    else                         { v = p[1] | (p[2] << 8) | ((DWORD)p[3] << 16) | ((DWORD)p[4] << 24); p += 5; }
    *pp = p;
    return v;
}
static DWORD fh4_rva(const BYTE **pp) { DWORD v; memcpy(&v, *pp, 4); *pp += 4; return v; }

static int fh3_state_from_ip(DWORD64 base, const FuncInfo *fi, DWORD rva)
{
    const IPtoState *m = (const IPtoState *)(base + fi->dispIPtoStateMap);
    int state = -1;
    for (DWORD i = 0; i < fi->nIPMapEntries; i++) {
        if ((DWORD)m[i].ip > rva) break;
        state = m[i].state;
    }
    return state;
}

static BOOL fh3_decode(EhFunc *f, DWORD64 base, const FuncInfo *fi, DWORD64 frame, DISPATCHER_CONTEXT *dc)
{
    DWORD magic = fi->magic & 0x1FFFFFFF;
    if (magic < CXX_MAGIC_MIN || magic > CXX_MAGIC_MAX) return FALSE;
    f->nstates = fi->maxState > 0 ? fi->maxState : 0;
    f->uw = eh_alloc(sizeof(UwEntry) * f->nstates);
    f->tb = eh_alloc(sizeof(TryBlock) * fi->nTryBlocks);
    if (!f->uw || !f->tb) return FALSE;
    const UnwindMapEntry *um = (const UnwindMapEntry *)(base + fi->dispUnwindMap);
    for (int i = 0; i < f->nstates; i++) {
        f->uw[i].to = um[i].toState;
        f->uw[i].kind = um[i].action ? UW_FUNCLET : UW_NONE;
        f->uw[i].action = (DWORD)um[i].action;
    }
    const TryBlockMapEntry *tbm = (const TryBlockMapEntry *)(base + fi->dispTryBlockMap);
    DWORD begin = dc->FunctionEntry->BeginAddress;
    for (DWORD i = 0; i < fi->nTryBlocks; i++, f->ntry++) {
        TryBlock *tb = &f->tb[i];
        tb->lo = tbm[i].tryLow; tb->hi = tbm[i].tryHigh; tb->catch_high = tbm[i].catchHigh;
        tb->catches = eh_alloc(sizeof(Catch) * (tbm[i].nCatches > 0 ? tbm[i].nCatches : 0));
        if (!tb->catches) return FALSE;
        const HandlerType *ha = (const HandlerType *)(base + tbm[i].dispHandlerArray);
        for (int k = 0; k < tbm[i].nCatches; k++, tb->ncatches++) {
            Catch *c = &tb->catches[k];
            c->adjectives = ha[k].adjectives;
            c->type = (DWORD)ha[k].dispType;
            c->catch_obj = ha[k].dispCatchObj;
            c->handler = (DWORD)ha[k].dispOfHandler;
            c->frame = ha[k].dispFrame;
            /* a catch funclet?  Its locals live in the parent function's frame. */
            if (c->handler == begin && f->funclet_try < 0) {
                f->funclet_try = (int)i;
                f->parent = *(DWORD64 *)(frame + c->frame);
            }
        }
    }
    f->state = fh3_state_from_ip(base, fi, (DWORD)(dc->ControlPc - base));
    return TRUE;
}

static BOOL fh4_decode(EhFunc *f, DWORD64 base, const BYTE *p, DWORD64 frame, DISPATCHER_CONTEXT *dc)
{
    DWORD begin = dc->FunctionEntry->BeginAddress, um = 0, tbm = 0;
    BYTE flags = *p++;
    if (flags & FI4_BBT) fh4_uint(&p);
    if (flags & FI4_UNWIND_MAP) um = fh4_rva(&p);
    if (flags & FI4_TRYBLOCK_MAP) tbm = fh4_rva(&p);
    DWORD ipm = fh4_rva(&p);
    if (flags & FI4_IS_CATCH) f->parent = *(DWORD64 *)(frame + fh4_uint(&p));
    f->noexcept = (flags & FI4_NOEXCEPT) != 0;

    if (um) {                       /* entries in state order; "to" by backwards offset */
        const BYTE *q = (const BYTE *)(base + um);
        f->nstates = (int)fh4_uint(&q);
        f->uw = eh_alloc(sizeof(UwEntry) * f->nstates);
        DWORD *start = eh_alloc(sizeof(DWORD) * f->nstates);
        if (!f->uw || !start) { RtlFreeHeap(RtlGetProcessHeap(), 0, start); return FALSE; }
        for (int i = 0; i < f->nstates; i++) {
            start[i] = (DWORD)(q - (const BYTE *)base);
            DWORD v = fh4_uint(&q);
            UwEntry *e = &f->uw[i];
            e->kind = v & 3;
            DWORD prev = start[i] - (v >> 2);
            e->to = -1;
            for (int k = i - 1; k >= 0 && (v >> 2); k--)
                if (start[k] == prev) { e->to = k; break; }
            if (e->kind != UW_NONE) e->action = fh4_rva(&q);
            if (e->kind == UW_DTOR_OBJ || e->kind == UW_DTOR_PTR) e->object = fh4_uint(&q);
        }
        RtlFreeHeap(RtlGetProcessHeap(), 0, start);
    }
    if (tbm) {
        const BYTE *q = (const BYTE *)(base + tbm);
        int n = (int)fh4_uint(&q);
        f->tb = eh_alloc(sizeof(TryBlock) * n);
        if (!f->tb) return FALSE;
        for (int i = 0; i < n; i++, f->ntry++) {
            TryBlock *tb = &f->tb[i];
            tb->lo = (int)fh4_uint(&q);
            tb->hi = (int)fh4_uint(&q);
            tb->catch_high = (int)fh4_uint(&q);
            const BYTE *h = (const BYTE *)(base + fh4_rva(&q));
            int nc = (int)fh4_uint(&h);
            tb->catches = eh_alloc(sizeof(Catch) * nc);
            if (!tb->catches) return FALSE;
            for (int k = 0; k < nc; k++, tb->ncatches++) {
                Catch *c = &tb->catches[k];
                BYTE cf = *h++;
                if (cf & CB4_ADJECTIVES) c->adjectives = fh4_uint(&h);
                if (cf & CB4_TYPE) c->type = fh4_rva(&h);
                if (cf & CB4_CATCH_OBJ) c->catch_obj = (int)fh4_uint(&h);
                c->handler = fh4_rva(&h);
                c->ncont = (cf >> CB4_CONT_SHIFT) & 3;
                if (c->ncont > 2) return FALSE;
                for (int j = 0; j < c->ncont; j++)
                    c->cont[j] = cf & CB4_SEPARATED ? fh4_rva(&h) : begin + fh4_uint(&h);
            }
        }
    }

    /* IP-to-state: (IP delta, state + 1) pairs from the function's start;
     * a function split into pieces has one map per piece */
    const BYTE *q = (const BYTE *)(base + ipm);
    if (flags & FI4_IS_SEPARATED) {
        int n = (int)fh4_uint(&q);
        const BYTE *found = 0;
        for (int i = 0; i < n; i++) {
            DWORD seg = fh4_rva(&q), map = fh4_rva(&q);
            if (seg == begin) found = (const BYTE *)(base + map);
        }
        q = found;
    }
    f->state = -1;
    if (q) {
        DWORD pc = (DWORD)(dc->ControlPc - base), ip = begin;
        int n = (int)fh4_uint(&q);
        for (int i = 0; i < n; i++) {
            ip += fh4_uint(&q);
            int st = (int)fh4_uint(&q) - 1;
            if (ip > pc) break;
            f->state = st;
        }
    }
    return TRUE;
}

static int frame_state(EhThread *t, DWORD64 frame, int state)
{
    for (CatchRec *c = t ? t->catches : 0; c; c = c->next)
        if (c->frame == frame) return c->enclosing;
    return state;
}

static BOOL eh_decode(EhFunc *f, EhThread *t, DWORD64 frame, DISPATCHER_CONTEXT *dc, int fh4)
{
    memset(f, 0, sizeof(*f));
    f->base = dc->ImageBase;
    f->fh4 = fh4;
    f->parent = frame;
    f->funclet_try = -1;
    const BYTE *data = (const BYTE *)(f->base + *(DWORD *)dc->HandlerData);
    BOOL ok = fh4 ? fh4_decode(f, f->base, data, frame, dc)
                  : fh3_decode(f, f->base, (const FuncInfo *)data, frame, dc);
    if (!ok) { eh_free(f); return FALSE; }
    f->state = frame_state(t, frame, f->state);
    return TRUE;
}

typedef void (*Funclet)(void *unused, DWORD64 frame);

static void run_unwind_action(const EhFunc *f, const UwEntry *e)
{
    void *fn = (void *)(f->base + e->action);
    switch (e->kind) {
    case UW_FUNCLET:  ((Funclet)fn)(0, f->parent); break;
    case UW_DTOR_OBJ: ((void (*)(void *))fn)((void *)(f->parent + e->object)); break;
    case UW_DTOR_PTR: ((void (*)(void *))fn)(*(void **)(f->parent + e->object)); break;
    }
}

/* Run the unwind actions (destructors) from @cur down to @to */
static void unwind_states(const EhFunc *f, int cur, int to)
{
    while (cur > to && cur >= 0 && cur < f->nstates) {
        const UwEntry *e = &f->uw[cur];
        cur = e->to;
        run_unwind_action(f, e);
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

static const CatchableType *match(DWORD64 base, const Catch *h, const ThrowInfo *ti, DWORD64 ti_base)
{
    const CatchableTypeArray *cta = (const CatchableTypeArray *)(ti_base + ti->pCatchableTypeArray);
    if (!h->type) return (const CatchableType *)(ti_base + cta->arrayOfCatchableTypes[0]);   /* catch (...) */
    const TypeDescriptor *want = (const TypeDescriptor *)(base + h->type);
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

static void build_catch_object(DWORD64 base, const Catch *h, DWORD64 parent, void *obj,
                               const CatchableType *ct, DWORD64 ti_base)
{
    if (!h->type || !h->catch_obj) return;                  /* catch (...) or unnamed */
    const TypeDescriptor *want = (const TypeDescriptor *)(base + h->type);
    if (!want->name[0]) return;
    void **slot = (void **)(parent + h->catch_obj);
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

/* @f is freed here (before the catching frame resumes) */
static __declspec(noreturn) void catch_it(EXCEPTION_RECORD *rec, DWORD64 target, CONTEXT *ctx, EhFunc *f,
                                          const TryBlock *tb, const Catch *h, const CatchableType *ct)
{
    EhThread *t = vcrt_thread();
    void *obj = (void *)rec->ExceptionInformation[1];
    const ThrowInfo *ti = (const ThrowInfo *)rec->ExceptionInformation[2];
    DWORD64 ti_base = rec->ExceptionInformation[3];
    DWORD64 base = f->base, parent = f->parent, handler = base + h->handler;
    DWORD64 cont_addr[2] = { base + h->cont[0], base + h->cont[1] };
    int ncont = h->ncont, try_low = tb->lo;
    int enclosing = try_low >= 0 && try_low < f->nstates ? f->uw[try_low].to : -1;

    build_catch_object(base, h, parent, obj, ct, ti_base);
    eh_free(f);

    /* unwind the frames in between, and this one down to the try block */
    EXCEPTION_RECORD urec = *rec;
    CONTEXT tctx;
    t->target_state = try_low;
    if (!unwind_to(target, &urec, ctx, &tctx)) no_catch("vcruntime: unwinding to the catch failed\n");

    CatchRec *c = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*c));
    if (!c) no_catch("vcruntime: out of memory in catch\n");
    c->frame = target;
    c->enclosing = enclosing;
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
    DWORD64 cont = nova_call_catch(handler, parent, &fr);
    if (cont < (DWORD64)ncont) cont = cont_addr[cont];     /* FH4: an index */

    /* the catch ended normally: the exception is over */
    t->catches = c->next;
    t->cur_rec = t->catches ? &t->catches->rec : 0;
    t->cur_ctx = t->catches ? t->catches->ctx : 0;
    destroy_object(obj, ti, ti_base);
    RtlFreeHeap(RtlGetProcessHeap(), 0, c);

    tctx.Rip = cont;
    RtlRestoreContext(&tctx, 0);                         /* (ntdll forgets the dispatch below) */
    no_catch("vcruntime: resuming after catch failed\n");
}

/* -----------------------------------------------------------------------
 * The frame handlers
 * ----------------------------------------------------------------------- */
static EXCEPTION_DISPOSITION frame_handler(EXCEPTION_RECORD *rec, PVOID establisher, DISPATCHER_CONTEXT *dc,
                                           CONTEXT *ctx, int fh4)
{
    DWORD64 frame = (DWORD64)establisher;
    EhThread *t = vcrt_thread();
    EhFunc f;
    if (!eh_decode(&f, t, frame, dc, fh4)) return ExceptionContinueSearch;
    int state = f.state;

    if (rec->ExceptionFlags & EXC_UNWIND_MASK) {
        void *in_flight = rec->ExceptionCode == CXX_EXCEPTION && rec->NumberParameters >= 3
                          ? (void *)rec->ExceptionInformation[1] : 0;
        drop_catches(t, frame, in_flight);
        if (rec->ExceptionFlags & EXC_TARGET_UNWIND) {
            unwind_states(&f, state, t->target_state);
        } else if (f.funclet_try >= 0) {
            /* FH3: only the catch block's own objects; the parent frame does the rest */
            const TryBlock *tb = &f.tb[f.funclet_try];
            while (state > tb->hi && state <= tb->catch_high && state < f.nstates) {
                const UwEntry *e = &f.uw[state];
                state = e->to;
                run_unwind_action(&f, e);
            }
        } else {
            unwind_states(&f, state, -1);
        }
        eh_free(&f);
        return ExceptionContinueSearch;
    }

    const ThrowInfo *ti = 0;
    DWORD64 ti_base = 0;
    if (rec->ExceptionCode == CXX_EXCEPTION && rec->NumberParameters >= 3) {
        DWORD m = (DWORD)rec->ExceptionInformation[0];
        if (m >= CXX_MAGIC_MIN && m <= CXX_MAGIC_MAX) {
            ti = (const ThrowInfo *)rec->ExceptionInformation[2];
            ti_base = rec->NumberParameters >= 4 ? rec->ExceptionInformation[3] : 0;
        }
    }
    if (!ti) { eh_free(&f); return ExceptionContinueSearch; }

    for (int i = 0; i < f.ntry; i++) {
        const TryBlock *tb = &f.tb[i];
        if (state < tb->lo || state > tb->hi) continue;
        /* an FH3 catch funclet owns only the try blocks inside its catch;
         * the ones around it belong to the parent frame, visited next */
        if (f.funclet_try >= 0 && (tb->lo <= f.tb[f.funclet_try].hi || tb->hi > f.tb[f.funclet_try].catch_high))
            continue;
        for (int k = 0; k < tb->ncatches; k++) {
            const CatchableType *ct = match(f.base, &tb->catches[k], ti, ti_base);
            if (ct) catch_it(rec, frame, ctx, &f, tb, &tb->catches[k], ct);
        }
    }
    BOOL noexcept_fn = f.noexcept;
    eh_free(&f);
    if (noexcept_fn) no_catch("vcruntime: an exception left a noexcept function (terminate)\n");
    return ExceptionContinueSearch;
}

VCRT EXCEPTION_DISPOSITION __CxxFrameHandler3(EXCEPTION_RECORD *rec, PVOID frame, CONTEXT *ctx, DISPATCHER_CONTEXT *dc)
{
    return frame_handler(rec, frame, dc, ctx, 0);
}

VCRT EXCEPTION_DISPOSITION __CxxFrameHandler(EXCEPTION_RECORD *rec, PVOID frame, CONTEXT *ctx, DISPATCHER_CONTEXT *dc)
{
    return frame_handler(rec, frame, dc, ctx, 0);
}

VCRT EXCEPTION_DISPOSITION __CxxFrameHandler2(EXCEPTION_RECORD *rec, PVOID frame, CONTEXT *ctx, DISPATCHER_CONTEXT *dc)
{
    return frame_handler(rec, frame, dc, ctx, 0);
}

/* vcruntime140_1.dll forwards here */
VCRT EXCEPTION_DISPOSITION __CxxFrameHandler4(EXCEPTION_RECORD *rec, PVOID frame, CONTEXT *ctx, DISPATCHER_CONTEXT *dc)
{
    return frame_handler(rec, frame, dc, ctx, 1);
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
