#define NOVA_BUILD_NTDLL
/*
 * ntdll_exc.c — structured exception handling in user mode
 *
 * The kernel hands an exception to KiUserExceptionDispatcher with a
 * CONTEXT and an EXCEPTION_RECORD on the stack.  RtlDispatchException runs
 * the vectored handlers and then walks the stack with the x64 unwinder
 * (RtlLookupFunctionEntry + RtlVirtualUnwind over the .pdata/.xdata the
 * compiler emits), calling each frame's language handler
 * (__C_specific_handler for __try/__except/__finally).  A handler that
 * resolves the exception resumes through NtContinue; nothing handling it
 * goes back to the kernel as a second-chance NtRaiseException, which ends
 * the process with a crash report.
 */

#include <winternl.h>
#include <winnt.h>

void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);

/* -----------------------------------------------------------------------
 * Vectored / top-level handlers
 * ----------------------------------------------------------------------- */
typedef struct VEH { struct VEH *next; PVECTORED_EXCEPTION_HANDLER fn; } VEH;
static VEH *g_veh, *g_vch;
static volatile long g_veh_lock;
static PTOP_LEVEL_EXCEPTION_FILTER g_top_filter;

static void vlock(void)   { while (__atomic_exchange_n(&g_veh_lock, 1, __ATOMIC_ACQUIRE)) __builtin_ia32_pause(); }
static void vunlock(void) { __atomic_store_n(&g_veh_lock, 0, __ATOMIC_RELEASE); }

static PVOID veh_add(VEH **list, ULONG first, PVECTORED_EXCEPTION_HANDLER fn)
{
    VEH *v = RtlAllocateHeap(RtlGetProcessHeap(), 0, sizeof(VEH));
    if (!v) return 0;
    v->fn = fn;
    vlock();
    if (first || !*list) { v->next = *list; *list = v; }
    else { VEH *t = *list; while (t->next) t = t->next; t->next = 0; v->next = 0; t->next = v; }
    vunlock();
    return v;
}

static ULONG veh_remove(VEH **list, PVOID h)
{
    vlock();
    for (VEH **pp = list; *pp; pp = &(*pp)->next)
        if (*pp == h) { VEH *v = *pp; *pp = v->next; vunlock(); RtlFreeHeap(RtlGetProcessHeap(), 0, v); return 1; }
    vunlock();
    return 0;
}

PVOID NTAPI RtlAddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER h) { return veh_add(&g_veh, first, h); }
ULONG NTAPI RtlRemoveVectoredExceptionHandler(PVOID h) { return veh_remove(&g_veh, h); }
PVOID NTAPI RtlAddVectoredContinueHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER h) { return veh_add(&g_vch, first, h); }
ULONG NTAPI RtlRemoveVectoredContinueHandler(PVOID h) { return veh_remove(&g_vch, h); }
VOID  NTAPI RtlSetUnhandledExceptionFilter(PTOP_LEVEL_EXCEPTION_FILTER f) { g_top_filter = f; }

static LONG run_vectored(VEH *list, PEXCEPTION_POINTERS info)
{
    for (VEH *v = list; v; v = v->next) {
        LONG r = v->fn(info);
        if (r == EXCEPTION_CONTINUE_EXECUTION) return r;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Called by the loader's top-level __except filter (last resort) */
LONG nova_top_level_filter(PEXCEPTION_POINTERS info)
{
    LONG r = g_top_filter ? g_top_filter(info) : EXCEPTION_EXECUTE_HANDLER;
    /* ending the process: the kernel says where it crashed (second chance) */
    if (r == EXCEPTION_EXECUTE_HANDLER && info && info->ExceptionRecord && info->ContextRecord)
        NtRaiseException(info->ExceptionRecord, info->ContextRecord, FALSE);
    return r;
}

#ifdef _WIN64
/* -----------------------------------------------------------------------
 * x64 unwind data
 * ----------------------------------------------------------------------- */
typedef union { struct { BYTE CodeOffset, UnwindOp_OpInfo; }; USHORT FrameOffset; } UNWIND_CODE;
typedef struct {
    BYTE VersionFlags;              /* Version:3, Flags:5 */
    BYTE SizeOfProlog;
    BYTE CountOfCodes;
    BYTE FrameRegOff;               /* FrameRegister:4, FrameOffset:4 */
    UNWIND_CODE UnwindCode[1];
} UNWIND_INFO;

enum { UWOP_PUSH_NONVOL, UWOP_ALLOC_LARGE, UWOP_ALLOC_SMALL, UWOP_SET_FPREG,
       UWOP_SAVE_NONVOL, UWOP_SAVE_NONVOL_FAR, UWOP_EPILOG, UWOP_SPARE,
       UWOP_SAVE_XMM128, UWOP_SAVE_XMM128_FAR, UWOP_PUSH_MACHFRAME };

/* &CONTEXT.Rax as an array indexed the way unwind codes number registers */
static DWORD64 *int_reg(CONTEXT *c, int i) { return &c->Rax + i; }
static M128A   *xmm_reg(CONTEXT *c, int i) { return &c->Xmm0 + i; }

static IMAGE_NT_HEADERS *nt_of(void *base)
{
    IMAGE_DOS_HEADER *dos = base;
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((BYTE *)base + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE ? nt : 0;
}

PVOID NTAPI RtlPcToFileHeader(PVOID pc, PVOID *base)
{
    PLDR_DATA_TABLE_ENTRY e = LdrNovaFindEntry(pc);
    *base = e ? e->DllBase : 0;
    return *base;
}

/* Function tables registered at run time for generated code (JIT
 * compilers): RtlAddFunctionTable's sorted arrays and
 * RtlInstallFunctionTableCallback's ranges with a callback */
typedef PRUNTIME_FUNCTION (NTAPI *FT_CALLBACK)(DWORD64 pc, PVOID ctx);
typedef struct { PRUNTIME_FUNCTION tab; DWORD n; DWORD64 base, lo, hi; FT_CALLBACK cb; PVOID ctx; DWORD64 id; } DynTable;
#define MAX_DYN_TABLES 256
static DynTable g_dyn[MAX_DYN_TABLES];
static volatile LONG g_dyn_lock;
static void dyn_lock(void)   { while (__atomic_exchange_n(&g_dyn_lock, 1, __ATOMIC_ACQUIRE)) __builtin_ia32_pause(); }
static void dyn_unlock(void) { __atomic_store_n(&g_dyn_lock, 0, __ATOMIC_RELEASE); }

static BOOLEAN dyn_add(DynTable t)
{
    dyn_lock();
    for (int i = 0; i < MAX_DYN_TABLES; i++)
        if (!g_dyn[i].id) { g_dyn[i] = t; dyn_unlock(); return TRUE; }
    dyn_unlock();
    return FALSE;
}

BOOLEAN NTAPI RtlAddFunctionTable(PRUNTIME_FUNCTION tab, DWORD n, DWORD64 base)
{
    if (!tab || !n) return FALSE;
    DynTable t = { tab, n, base, base + tab[0].BeginAddress, base + tab[n - 1].EndAddress, 0, 0, (DWORD64)tab };
    for (DWORD i = 0; i < n; i++) {                  /* (not necessarily sorted by the caller) */
        if (base + tab[i].BeginAddress < t.lo) t.lo = base + tab[i].BeginAddress;
        if (base + tab[i].EndAddress > t.hi) t.hi = base + tab[i].EndAddress;
    }
    return dyn_add(t);
}

/* @id: the table's identifier with its low 2 bits set (3), as Windows requires */
BOOLEAN NTAPI RtlInstallFunctionTableCallback(DWORD64 id, DWORD64 base, DWORD len, FT_CALLBACK cb, PVOID ctx, PCWSTR dll)
{
    (void)dll;
    if ((id & 3) != 3 || !cb) return FALSE;
    DynTable t = { 0, 0, base, base, base + len, cb, ctx, id };
    return dyn_add(t);
}

BOOLEAN NTAPI RtlDeleteFunctionTable(PRUNTIME_FUNCTION tab)
{
    dyn_lock();
    for (int i = 0; i < MAX_DYN_TABLES; i++)
        if (g_dyn[i].id == (DWORD64)tab) { g_dyn[i].id = 0; dyn_unlock(); return TRUE; }
    dyn_unlock();
    return FALSE;
}

/* NTSTATUS RtlAddGrowableFunctionTable(PVOID *handle, PRUNTIME_FUNCTION, DWORD count, DWORD max, ULONG_PTR base, ULONG_PTR end) */
NTSTATUS NTAPI RtlAddGrowableFunctionTable(PVOID *h, PRUNTIME_FUNCTION tab, DWORD n, DWORD max, ULONG_PTR base, ULONG_PTR end)
{
    (void)max;
    DynTable t = { tab, n, base, base, end, 0, 0, (DWORD64)tab };
    if (!dyn_add(t)) return 0xC0000017;
    *h = tab;
    return 0;
}
void NTAPI RtlGrowFunctionTable(PVOID h, DWORD n)
{
    dyn_lock();
    for (int i = 0; i < MAX_DYN_TABLES; i++) if (g_dyn[i].id == (DWORD64)h) g_dyn[i].n = n;
    dyn_unlock();
}
void NTAPI RtlDeleteGrowableFunctionTable(PVOID h) { RtlDeleteFunctionTable(h); }

static PRUNTIME_FUNCTION dyn_lookup(DWORD64 pc, PDWORD64 base_out)
{
    DynTable t;
    int found = 0;
    dyn_lock();
    for (int i = 0; i < MAX_DYN_TABLES && !found; i++)
        if (g_dyn[i].id && pc >= g_dyn[i].lo && pc < g_dyn[i].hi) { t = g_dyn[i]; found = 1; }
    dyn_unlock();
    if (!found) return 0;
    if (base_out) *base_out = t.base;
    if (t.cb) return t.cb(pc, t.ctx);
    DWORD rva = (DWORD)(pc - t.base);
    for (DWORD i = 0; i < t.n; i++)
        if (rva >= t.tab[i].BeginAddress && rva < t.tab[i].EndAddress) return &t.tab[i];
    return 0;
}

PRUNTIME_FUNCTION NTAPI RtlLookupFunctionEntry(DWORD64 pc, PDWORD64 base_out, PUNWIND_HISTORY_TABLE hist)
{
    (void)hist;
    PLDR_DATA_TABLE_ENTRY e = LdrNovaFindEntry((PVOID)pc);
    if (!e) return dyn_lookup(pc, base_out);
    BYTE *base = e->DllBase;
    if (base_out) *base_out = (DWORD64)base;
    IMAGE_NT_HEADERS *nt = nt_of(base);
    if (!nt || nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXCEPTION) return 0;
    IMAGE_DATA_DIRECTORY *d = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!d->VirtualAddress || !d->Size) return 0;
    PRUNTIME_FUNCTION fn = (PRUNTIME_FUNCTION)(base + d->VirtualAddress);
    DWORD rva = (DWORD)(pc - (DWORD64)base);
    int lo = 0, hi = (int)(d->Size / sizeof(RUNTIME_FUNCTION)) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (rva < fn[mid].BeginAddress) hi = mid - 1;
        else if (rva >= fn[mid].EndAddress) lo = mid + 1;
        else {
            PRUNTIME_FUNCTION r = &fn[mid];
            while (r->UnwindData & 1) {              /* chained info: follow to the primary */
                r = (PRUNTIME_FUNCTION)(base + (r->UnwindData & ~1u));
            }
            return &fn[mid];
        }
    }
    return 0;
}

/* Record where a register was restored from (the caller's save slot), for
 * callers that need the address, such as a GC updating object references */
static void set_int(CONTEXT *ctx, PKNONVOLATILE_CONTEXT_POINTERS ptrs, int reg, DWORD64 *slot)
{
    *int_reg(ctx, reg) = *slot;
    if (ptrs) ptrs->IntegerContext[reg] = slot;
}
static void set_xmm(CONTEXT *ctx, PKNONVOLATILE_CONTEXT_POINTERS ptrs, int reg, M128A *slot)
{
    *xmm_reg(ctx, reg) = *slot;
    if (ptrs) ptrs->FloatingContext[reg] = slot;
}

/* Is @pc in an epilogue: [add $n,%rsp | lea n(%reg),%rsp] pop* ret, or a
 * jump within the function to such a sequence, or a tail jump */
static int in_epilogue(const BYTE *pc, DWORD64 base, PRUNTIME_FUNCTION f)
{
    if ((pc[0] & 0xF8) == 0x48) {
        switch (pc[1]) {
        case 0x81: if (pc[0] == 0x48 && pc[2] == 0xC4) { pc += 7; break; } return 0;
        case 0x83: if (pc[0] == 0x48 && pc[2] == 0xC4) { pc += 4; break; } return 0;
        case 0x8D:
            if (pc[0] & 0x06) return 0;                      /* rex.RX clear */
            if (((pc[2] >> 3) & 7) != 4) return 0;           /* into %rsp */
            if ((pc[2] & 7) == 4) return 0;                  /* no SIB */
            if ((pc[2] >> 6) == 1) { pc += 4; break; }
            if ((pc[2] >> 6) == 2) { pc += 7; break; }
            return 0;
        }
    }
    for (int steps = 0; steps < 32; steps++) {
        BYTE rex = 0;
        if ((*pc & 0xF0) == 0x40) rex = *pc++;
        switch (*pc) {
        case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            pc++;
            continue;
        case 0xC2: case 0xC3:
            return 1;
        case 0xF3:
            return pc[1] == 0xC3;
        case 0xFF:                                           /* rex.W jmp *ea: a tail call */
            return (rex & 8) && ((pc[1] >> 3) & 7) == 4;
        case 0xE9: {
            const BYTE *t = pc + 5 + *(const LONG *)(pc + 1);
            DWORD64 r = (DWORD64)t - base;
            if (r >= f->BeginAddress && r < f->EndAddress) { pc = t; continue; }
            return 0;
        }
        case 0xEB: {
            const BYTE *t = pc + 2 + (signed char)pc[1];
            DWORD64 r = (DWORD64)t - base;
            if (r >= f->BeginAddress && r < f->EndAddress) { pc = t; continue; }
            return 0;
        }
        }
        return 0;
    }
    return 0;
}

/* Run the rest of an epilogue on @ctx */
static void run_epilogue(const BYTE *pc, CONTEXT *ctx, PKNONVOLATILE_CONTEXT_POINTERS ptrs)
{
    for (int steps = 0; steps < 64; steps++) {
        BYTE rex = 0;
        if ((*pc & 0xF0) == 0x40) rex = *pc++ & 0x0F;
        switch (*pc) {
        case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            set_int(ctx, ptrs, *pc - 0x58 + (rex & 1) * 8, (DWORD64 *)ctx->Rsp);
            ctx->Rsp += 8;
            pc++;
            continue;
        case 0x81: ctx->Rsp += *(const LONG *)(pc + 2); pc += 6; continue;
        case 0x83: ctx->Rsp += (signed char)pc[2]; pc += 3; continue;
        case 0x8D:
            if ((pc[1] >> 6) == 1) {
                ctx->Rsp = *int_reg(ctx, (pc[1] & 7) + (rex & 1) * 8) + (signed char)pc[2];
                pc += 3;
            } else {
                ctx->Rsp = *int_reg(ctx, (pc[1] & 7) + (rex & 1) * 8) + *(const LONG *)(pc + 2);
                pc += 6;
            }
            continue;
        case 0xC2:
            ctx->Rip = *(DWORD64 *)ctx->Rsp;
            ctx->Rsp += 8 + *(const WORD *)(pc + 1);
            return;
        case 0xC3: case 0xF3: case 0xFF:                     /* ret, rep ret, tail jump */
            ctx->Rip = *(DWORD64 *)ctx->Rsp;
            ctx->Rsp += 8;
            return;
        case 0xE9: pc += 5 + *(const LONG *)(pc + 1); continue;
        case 0xEB: pc += 2 + (signed char)pc[1]; continue;
        }
        return;
    }
}

/* Unwind one frame: undo the prologue instructions already executed at @pc
 * (or finish the epilogue @pc is in), updating @ctx to the caller's
 * register state.  @ptrs, if given, receives where each restored
 * register was saved.  Returns the frame's handler of kind @type
 * (UNW_FLAG_EHANDLER or UNW_FLAG_UHANDLER), if any, and its data. */
PEXCEPTION_ROUTINE NTAPI RtlVirtualUnwind(ULONG type, DWORD64 base, DWORD64 pc, PRUNTIME_FUNCTION f,
                                          PCONTEXT ctx, PVOID *handler_data, PDWORD64 frame_out,
                                          PKNONVOLATILE_CONTEXT_POINTERS ptrs)
{
    while (f->UnwindData & 1) f = (PRUNTIME_FUNCTION)(base + (f->UnwindData & ~1u));   /* indirect entry */
    if (handler_data) *handler_data = 0;
    UNWIND_INFO *ui;
    DWORD64 frame_base = ctx->Rsp;
    int machframe = 0, in_prolog = 0, first = 1;
    for (;;) {
        ui = (UNWIND_INFO *)(base + f->UnwindData);
        int n = ui->CountOfCodes;
        BYTE framereg = ui->FrameRegOff & 0xF;
        DWORD off = (DWORD)(pc - (base + f->BeginAddress));
        int prolog = pc >= base + f->BeginAddress && off < ui->SizeOfProlog;
        if (first) in_prolog = prolog;

        /* the frame register holds the frame base once it is set */
        frame_base = ctx->Rsp;
        if (framereg) {
            int set = !prolog;
            for (int i = 0; prolog && i < n; ) {
                UNWIND_CODE *u = &ui->UnwindCode[i];
                int op = u->UnwindOp_OpInfo & 0xF, info = u->UnwindOp_OpInfo >> 4;
                if (op == UWOP_SET_FPREG && u->CodeOffset <= off) set = 1;
                i += op == UWOP_ALLOC_LARGE ? (info ? 3 : 2)
                   : (op == UWOP_SAVE_NONVOL || op == UWOP_SAVE_XMM128) ? 2
                   : (op == UWOP_SAVE_NONVOL_FAR || op == UWOP_SAVE_XMM128_FAR) ? 3 : 1;
            }
            if (set) frame_base = *int_reg(ctx, framereg) - (ui->FrameRegOff >> 4) * 16;
        }

        if (first && !prolog && in_epilogue((const BYTE *)pc, base, f)) {
            run_epilogue((const BYTE *)pc, ctx, ptrs);
            if (frame_out) *frame_out = frame_base;
            return 0;
        }
        first = 0;

        for (int i = 0; i < n; ) {
            UNWIND_CODE *u = &ui->UnwindCode[i];
            int op = u->UnwindOp_OpInfo & 0xF, info = u->UnwindOp_OpInfo >> 4;
            int slots = 1;
            int applied = !prolog || u->CodeOffset <= off;
            switch (op) {
            case UWOP_PUSH_NONVOL:
                if (applied) { set_int(ctx, ptrs, info, (DWORD64 *)ctx->Rsp); ctx->Rsp += 8; }
                break;
            case UWOP_ALLOC_LARGE:
                if (info == 0) { slots = 2; if (applied) ctx->Rsp += ui->UnwindCode[i + 1].FrameOffset * 8ULL; }
                else { slots = 3; if (applied) ctx->Rsp += ui->UnwindCode[i + 1].FrameOffset | ((DWORD)ui->UnwindCode[i + 2].FrameOffset << 16); }
                break;
            case UWOP_ALLOC_SMALL:
                if (applied) ctx->Rsp += info * 8ULL + 8;
                break;
            case UWOP_SET_FPREG:
                if (applied) ctx->Rsp = frame_base;
                break;
            case UWOP_SAVE_NONVOL:
                slots = 2;
                if (applied) set_int(ctx, ptrs, info, (DWORD64 *)(frame_base + ui->UnwindCode[i + 1].FrameOffset * 8ULL));
                break;
            case UWOP_SAVE_NONVOL_FAR:
                slots = 3;
                if (applied) set_int(ctx, ptrs, info, (DWORD64 *)(frame_base + (ui->UnwindCode[i + 1].FrameOffset |
                                                                   ((DWORD)ui->UnwindCode[i + 2].FrameOffset << 16))));
                break;
            case UWOP_SAVE_XMM128:
                slots = 2;
                if (applied) set_xmm(ctx, ptrs, info, (M128A *)(frame_base + ui->UnwindCode[i + 1].FrameOffset * 16ULL));
                break;
            case UWOP_SAVE_XMM128_FAR:
                slots = 3;
                if (applied) set_xmm(ctx, ptrs, info, (M128A *)(frame_base + (ui->UnwindCode[i + 1].FrameOffset |
                                                                ((DWORD)ui->UnwindCode[i + 2].FrameOffset << 16))));
                break;
            case UWOP_PUSH_MACHFRAME:
                if (applied) {
                    DWORD64 sp = ctx->Rsp + (info ? 8 : 0);
                    ctx->Rip = *(DWORD64 *)(sp + 0);
                    ctx->Rsp = *(DWORD64 *)(sp + 24);
                    machframe = 1;
                }
                break;
            default: break;
            }
            i += slots;
        }
        if (!((ui->VersionFlags >> 3) & UNW_FLAG_CHAININFO)) break;
        /* chained: the parent's prologue (all of it has run) unwinds next */
        f = (PRUNTIME_FUNCTION)&ui->UnwindCode[(n + 1) & ~1];
        while (f->UnwindData & 1) f = (PRUNTIME_FUNCTION)(base + (f->UnwindData & ~1u));
        pc = base + f->EndAddress;                           /* past its prologue */
    }
    if (frame_out) *frame_out = frame_base;
    /* Caller's RIP is at [RSP]; pop it (a machine frame already gave it). */
    if (!machframe) {
        ctx->Rip = *(DWORD64 *)ctx->Rsp;
        ctx->Rsp += 8;
    }

    BYTE flags = ui->VersionFlags >> 3;
    if ((flags & type & (UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER)) && !in_prolog) {
        DWORD *p = (DWORD *)&ui->UnwindCode[(ui->CountOfCodes + 1) & ~1];    /* codes padded to even count */
        if (handler_data) *handler_data = p + 1;
        return (PEXCEPTION_ROUTINE)(base + *p);
    }
    return 0;
}

/* -----------------------------------------------------------------------
 * Context capture / restore (for RtlUnwindEx and NtContinue)
 * ----------------------------------------------------------------------- */
/* RtlCaptureContext(RCX = CONTEXT*): save this call site's register state */
__asm__(
    ".globl RtlCaptureContext\n"
    ".section .text$RtlCaptureContext,\"xr\"\n"
    "RtlCaptureContext:\n\t"
    "movl $0x10001F, 0x30(%rcx)\n\t"
    "movq %rax, 0x78(%rcx)\n\t" "movq %rcx, 0x80(%rcx)\n\t" "movq %rdx, 0x88(%rcx)\n\t"
    "movq %rbx, 0x90(%rcx)\n\t"
    "leaq 8(%rsp), %rax\n\t" "movq %rax, 0x98(%rcx)\n\t"
    "movq %rbp, 0xA0(%rcx)\n\t" "movq %rsi, 0xA8(%rcx)\n\t" "movq %rdi, 0xB0(%rcx)\n\t"
    "movq %r8, 0xB8(%rcx)\n\t" "movq %r9, 0xC0(%rcx)\n\t" "movq %r10, 0xC8(%rcx)\n\t"
    "movq %r11, 0xD0(%rcx)\n\t" "movq %r12, 0xD8(%rcx)\n\t" "movq %r13, 0xE0(%rcx)\n\t"
    "movq %r14, 0xE8(%rcx)\n\t" "movq %r15, 0xF0(%rcx)\n\t"
    "movq (%rsp), %rax\n\t" "movq %rax, 0xF8(%rcx)\n\t"
    "pushfq\n\t" "popq %rax\n\t" "movl %eax, 0x44(%rcx)\n\t"
    "movq 0x78(%rcx), %rax\n\t"
    "stmxcsr 0x34(%rcx)\n\t"
    "movdqa %xmm0, 0x1A0(%rcx)\n\t" "movdqa %xmm1, 0x1B0(%rcx)\n\t"
    "movdqa %xmm2, 0x1C0(%rcx)\n\t" "movdqa %xmm3, 0x1D0(%rcx)\n\t"
    "movdqa %xmm4, 0x1E0(%rcx)\n\t" "movdqa %xmm5, 0x1F0(%rcx)\n\t"
    "movdqa %xmm6, 0x200(%rcx)\n\t" "movdqa %xmm7, 0x210(%rcx)\n\t"
    "movdqa %xmm8, 0x220(%rcx)\n\t" "movdqa %xmm9, 0x230(%rcx)\n\t"
    "movdqa %xmm10, 0x240(%rcx)\n\t" "movdqa %xmm11, 0x250(%rcx)\n\t"
    "movdqa %xmm12, 0x260(%rcx)\n\t" "movdqa %xmm13, 0x270(%rcx)\n\t"
    "movdqa %xmm14, 0x280(%rcx)\n\t" "movdqa %xmm15, 0x290(%rcx)\n\t"
    "retq\n\t"
    ".section .drectve,\"yn\"\n\t"
    ".ascii \" /EXPORT:RtlCaptureContext\"\n\t"
    ".text\n");

VOID NTAPI RtlRestoreContext(PCONTEXT c, PEXCEPTION_RECORD rec)
{
    (void)rec;
    NtContinue(c, FALSE);                            /* the kernel reloads and IRETs */
}

/* -----------------------------------------------------------------------
 * Dispatch and unwind
 * ----------------------------------------------------------------------- */
static void set_handler_ctx(DISPATCHER_CONTEXT *dc, DWORD64 control_pc, DWORD64 base, PRUNTIME_FUNCTION f,
                            PEXCEPTION_ROUTINE handler, PVOID hdata, DWORD64 frame, PCONTEXT ctx)
{
    dc->ControlPc = control_pc;
    dc->ImageBase = base;
    dc->FunctionEntry = f;
    dc->EstablisherFrame = frame;
    dc->ContextRecord = ctx;
    dc->LanguageHandler = handler;
    dc->HandlerData = hdata;
    dc->ScopeIndex = 0;
}

BOOLEAN NTAPI RtlDispatchException(PEXCEPTION_RECORD rec, PCONTEXT ctx)
{
    EXCEPTION_POINTERS ep = { rec, ctx };
    if (run_vectored(g_veh, &ep) == EXCEPTION_CONTINUE_EXECUTION) return TRUE;

    CONTEXT cur = *ctx;                              /* walked; the original stays for resume */
    for (;;) {
        DWORD64 base = 0, frame = 0;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(cur.Rip, &base, 0);
        if (!f) {
            /* Leaf function: the return address is on top of the stack. */
            if (cur.Rsp == 0 || (cur.Rsp & 7)) break;
            cur.Rip = *(DWORD64 *)cur.Rsp;
            cur.Rsp += 8;
            if (!cur.Rip) break;
            continue;
        }
        CONTEXT before = cur;
        PVOID hdata = 0;
        PEXCEPTION_ROUTINE handler = RtlVirtualUnwind(UNW_FLAG_EHANDLER, base, before.Rip, f, &cur, &hdata, &frame, 0);
        if (handler) {
            DISPATCHER_CONTEXT dc;
            memset(&dc, 0, sizeof(dc));
            set_handler_ctx(&dc, before.Rip, base, f, handler, hdata, frame, ctx);
            EXCEPTION_DISPOSITION disp = handler(rec, (PVOID)frame, ctx, &dc);
            if (disp == ExceptionContinueExecution) {
                if (rec->ExceptionFlags & EXCEPTION_NONCONTINUABLE) return FALSE;
                return TRUE;
            }
            if (disp == ExceptionNestedException || disp == ExceptionCollidedUnwind) {
                /* Follow where the handler redirected us. */
                continue;
            }
            /* ExceptionContinueSearch: keep walking */
        }
        if (cur.Rip == before.Rip && cur.Rsp == before.Rsp) break;   /* no progress */
        if (!cur.Rip) break;
    }
    return FALSE;
}

/* RtlUnwindEx: run termination handlers from the current frame down to
 * @target_frame, then continue at @target_ip. */
VOID NTAPI RtlUnwindEx(PVOID target_frame, PVOID target_ip, PEXCEPTION_RECORD rec, PVOID retval,
                       PCONTEXT ctx, PUNWIND_HISTORY_TABLE hist)
{
    (void)hist;
    static EXCEPTION_RECORD local;
    if (!rec) { memset(&local, 0, sizeof(local)); local.ExceptionCode = STATUS_UNWIND_CONSOLIDATE; rec = &local; }
    rec->ExceptionFlags |= EXCEPTION_UNWINDING;
    if (!target_frame) rec->ExceptionFlags |= EXCEPTION_EXIT_UNWIND;

    CONTEXT cur = *ctx;
    for (;;) {
        DWORD64 base = 0, frame = 0;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(cur.Rip, &base, 0);
        if (!f) {
            if (cur.Rsp == 0 || (cur.Rsp & 7)) break;
            cur.Rip = *(DWORD64 *)cur.Rsp;
            cur.Rsp += 8;
            if (!cur.Rip) break;
            continue;
        }
        CONTEXT before = cur;
        PVOID hdata = 0;
        PEXCEPTION_ROUTINE handler = RtlVirtualUnwind(UNW_FLAG_UHANDLER, base, before.Rip, f, &cur, &hdata, &frame, 0);
        int is_target = target_frame && frame == (DWORD64)target_frame;
        if (handler) {
            DISPATCHER_CONTEXT dc;
            memset(&dc, 0, sizeof(dc));
            set_handler_ctx(&dc, before.Rip, base, f, handler, hdata, frame, &before);
            DWORD saved = rec->ExceptionFlags;
            if (is_target) rec->ExceptionFlags |= EXCEPTION_TARGET_UNWIND;
            handler(rec, (PVOID)frame, &before, &dc);    /* runs __finally / __except cleanup */
            rec->ExceptionFlags = saved;
        }
        if (is_target) {
            /* Resume in the target frame with its own register state (the
             * nonvolatile registers restored while unwinding the frames
             * below it), at the handler's continuation address. */
            before.Rip = (DWORD64)target_ip;
            before.Rax = (DWORD64)retval;         /* Rsp: the frame's own (not the establisher frame) */
            NtContinue(&before, FALSE);
        }
        if (target_frame && frame > (DWORD64)target_frame) break;   /* passed it */
        if (cur.Rip == before.Rip && cur.Rsp == before.Rsp) break;
        if (!cur.Rip) break;
    }
    /* No target frame (exit unwind), or the target was never found. */
    ctx->Rip = (DWORD64)target_ip;
    ctx->Rsp = (DWORD64)target_frame;
    ctx->Rax = (DWORD64)retval;
    NtContinue(ctx, FALSE);
}

VOID NTAPI RtlUnwind(PVOID frame, PVOID target_ip, PEXCEPTION_RECORD rec, PVOID retval)
{
    CONTEXT c;
    RtlCaptureContext(&c);
    RtlUnwindEx(frame, target_ip, rec, retval, &c, 0);
}

/* -----------------------------------------------------------------------
 * Raising exceptions
 * ----------------------------------------------------------------------- */
VOID NTAPI RtlRaiseException(PEXCEPTION_RECORD rec)
{
    CONTEXT c;
    RtlCaptureContext(&c);
    rec->ExceptionAddress = (PVOID)c.Rip;
    NtRaiseException(rec, &c, TRUE);                 /* first chance: back through the dispatcher */
}

/* -----------------------------------------------------------------------
 * The C/C++ language handler for __try/__except/__finally (clang & MSVC)
 * ----------------------------------------------------------------------- */
typedef LONG (__cdecl *FilterFn)(PEXCEPTION_POINTERS, PVOID frame);
typedef void (__cdecl *FinallyFn)(BOOLEAN abnormal, PVOID frame);  /* clang x64: frame in RDX */

__declspec(dllexport) EXCEPTION_DISPOSITION __C_specific_handler(
    PEXCEPTION_RECORD rec, PVOID frame, PCONTEXT ctx, PDISPATCHER_CONTEXT dc)
{
    PSCOPE_TABLE_AMD64 scope = dc->HandlerData;
    DWORD64 base = dc->ImageBase;
    DWORD control = (DWORD)(dc->ControlPc - base);
    BOOLEAN unwinding = (rec->ExceptionFlags & EXCEPTION_UNWIND) != 0;

    for (DWORD i = dc->ScopeIndex; i < scope->Count; i++) {
        DWORD begin = scope->ScopeRecord[i].BeginAddress;
        DWORD end = scope->ScopeRecord[i].EndAddress;
        DWORD handler = scope->ScopeRecord[i].HandlerAddress;
        DWORD target = scope->ScopeRecord[i].JumpTarget;
        if (control < begin || control >= end) continue;
        if (!unwinding) {
            if (target == 0) continue;              /* __finally: nothing on a first pass */
            /* __except: run the filter (HandlerAddress==1: always EXECUTE_HANDLER) */
            LONG r;
            if (handler == 1) {
                r = EXCEPTION_EXECUTE_HANDLER;
            } else {
                EXCEPTION_POINTERS ep = { rec, ctx };
                FilterFn filter = (FilterFn)(base + handler);
                r = filter(&ep, frame);
            }
            if (r == EXCEPTION_CONTINUE_EXECUTION) return ExceptionContinueExecution;
            if (r == EXCEPTION_CONTINUE_SEARCH) continue;
            /* EXCEPTION_EXECUTE_HANDLER: unwind to this __except body */
            RtlUnwindEx(frame, (PVOID)(base + target), rec, (PVOID)(ULONG_PTR)rec->ExceptionCode, ctx, 0);
            /* RtlUnwindEx does not return */
            return ExceptionContinueExecution;
        } else {
            /* Unwinding: run __finally blocks in this scope */
            if (target == 0 && handler) {
                FinallyFn fin = (FinallyFn)(base + handler);
                fin(TRUE, (PVOID)frame);
            }
        }
    }
    return ExceptionContinueSearch;
}

/* GS handler used by MSVC-compiled code with /GS security cookies */
__declspec(dllexport) EXCEPTION_DISPOSITION __GSHandlerCheck(
    PEXCEPTION_RECORD rec, PVOID frame, PCONTEXT ctx, PDISPATCHER_CONTEXT dc)
{
    (void)rec; (void)frame; (void)ctx; (void)dc;
    return ExceptionContinueSearch;
}

/* -----------------------------------------------------------------------
 * Kernel entry: an exception was raised in this thread
 * ----------------------------------------------------------------------- */
/* The kernel entered here with RCX = CONTEXT*, RDX = EXCEPTION_RECORD*
 * (already on the user stack, below the interrupted frame). */
void nova_dispatch_from_kernel(PCONTEXT ctx, PEXCEPTION_RECORD rec)
{
    if (RtlDispatchException(rec, ctx)) {
        NtContinue(ctx, FALSE);                      /* handled or fixed up: resume */
    } else {
        run_vectored(g_vch, &(EXCEPTION_POINTERS){ rec, ctx });
        NtRaiseException(rec, ctx, FALSE);           /* second chance → the kernel ends us */
    }
    for (;;) NtTerminateProcess(NtCurrentProcess(), rec->ExceptionCode);
}

/* KiUserExceptionDispatcher: kernel entry (RCX=CONTEXT*, RDX=EXCEPTION_RECORD*) */
__asm__(
    ".globl KiUserExceptionDispatcher\n"
    ".section .text$KiUserExceptionDispatcher,\"xr\"\n"
    "KiUserExceptionDispatcher:\n\t"
    "movq %rcx, %rdi\n\t"
    "movq %rdx, %rsi\n\t"
    "andq $-16, %rsp\n\t"
    "subq $32, %rsp\n\t"
    "call nova_dispatch_from_kernel\n\t"
    "int3\n\t"
    ".section .drectve,\"yn\"\n\t"
    ".ascii \" /EXPORT:KiUserExceptionDispatcher\"\n\t"
    ".text\n");

#else
#include "exc_x86.h"          /* 32-bit programs: frames chained from fs:[0] */
#endif

void RtlNovaInitExceptions(void)
{
    g_veh = g_vch = 0;
    g_top_filter = 0;
}
