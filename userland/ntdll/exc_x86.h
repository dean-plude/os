/*
 * exc_x86.h — structured exception handling for 32-bit programs
 * (included by ntdll_exc.c in the SysWOW64 build)
 *
 * On x86 every function with an exception handler links a registration
 * record { Next, Handler } into the chain at fs:[0].  The kernel delivers
 * an exception as a 64-bit CONTEXT and EXCEPTION_RECORD (it is a 64-bit
 * kernel); KiUserExceptionDispatcher turns them into their x86 forms,
 * runs the vectored handlers, then calls each registered handler, newest
 * first.  A handler that takes the exception calls RtlUnwind (which calls
 * the handlers in between once more, with EXCEPTION_UNWINDING, and takes
 * them off the chain) and jumps into its function; one that fixes things
 * up returns ExceptionContinueExecution and the thread resumes at the
 * (possibly changed) CONTEXT through NtContinue.
 *
 * _except_handler3 and _except_handler4_common are the handlers of
 * MSVC's and clang's __try/__except/__finally (scope tables of filter and
 * handler code that runs with EBP pointing into the function's frame).
 */

void nova_context_from64(CONTEXT *c, const BYTE *x);           /* ntdll_wow.c */
void nova_record_from64(EXCEPTION_RECORD *r, const BYTE *x);

#define NOVA_X86_EXPORT(name) ".section .drectve,\"yn\"\n\t.ascii \" /EXPORT:" name "\"\n\t.text\n"

static EXCEPTION_REGISTRATION_RECORD *seh_head(void)
{
    EXCEPTION_REGISTRATION_RECORD *f;
    __asm__ volatile ("movl %%fs:0, %0" : "=r"(f));
    return f;
}

static void seh_set_head(EXCEPTION_REGISTRATION_RECORD *f)
{
    __asm__ volatile ("movl %0, %%fs:0" : : "r"(f) : "memory");
}

/* call a handler (cdecl, 4 arguments) keeping EBX/ESI/EDI/EBP and ESP
 * whatever it does to them */
EXCEPTION_DISPOSITION nova_call_handler(PEXCEPTION_RECORD rec, PVOID frame, PCONTEXT ctx, PVOID dc,
                                        PEXCEPTION_ROUTINE fn) __asm__("nova_call_handler");
__asm__(
    ".text\n"
    "nova_call_handler:\n\t"
    "pushl %ebp\n\t"
    "movl %esp, %ebp\n\t"
    "pushl %ebx\n\t" "pushl %esi\n\t" "pushl %edi\n\t"
    "pushl 20(%ebp)\n\t" "pushl 16(%ebp)\n\t" "pushl 12(%ebp)\n\t" "pushl 8(%ebp)\n\t"
    "calll *24(%ebp)\n\t"
    "leal -12(%ebp), %esp\n\t"
    "popl %edi\n\t" "popl %esi\n\t" "popl %ebx\n\t"
    "popl %ebp\n\t"
    "retl\n");

/* call filter or __finally code of a function whose frame is at @ebp */
LONG nova_call_ebp(PVOID fn, PVOID ebp) __asm__("nova_call_ebp");
__asm__(
    ".text\n"
    "nova_call_ebp:\n\t"
    "pushl %ebp\n\t" "pushl %ebx\n\t" "pushl %esi\n\t" "pushl %edi\n\t"
    "movl 20(%esp), %eax\n\t"
    "movl 24(%esp), %ebp\n\t"
    "movl %esp, %esi\n\t"
    "pushl %esi\n\t"                     /* ESP kept on the stack too (in case ESI is not preserved) */
    "calll *%eax\n\t"
    "popl %esi\n\t"
    "movl %esi, %esp\n\t"
    "popl %edi\n\t" "popl %esi\n\t" "popl %ebx\n\t" "popl %ebp\n\t"
    "retl\n");

/* continue in an __except block of the function whose frame is at @ebp
 * (the block reloads ESP from the frame itself) */
__declspec(noreturn) void nova_jump_ebp(PVOID fn, PVOID ebp) __asm__("nova_jump_ebp");
__asm__(
    ".text\n"
    "nova_jump_ebp:\n\t"
    "movl 4(%esp), %eax\n\t"
    "movl 8(%esp), %ebp\n\t"
    "jmpl *%eax\n");

/* call a thread's start routine: stdcall or not, the stack comes back */
DWORD nova_call_start(PVOID fn, PVOID arg) __asm__("nova_call_start");
__asm__(
    ".text\n"
    ".globl nova_call_start\n"
    "nova_call_start:\n\t"
    "pushl %ebp\n\t"
    "movl %esp, %ebp\n\t"
    "pushl 12(%ebp)\n\t"
    "calll *8(%ebp)\n\t"
    "movl %ebp, %esp\n\t"
    "popl %ebp\n\t"
    "retl\n");

static BOOL frame_ok(EXCEPTION_REGISTRATION_RECORD *f)
{
    BYTE *t = NtCurrentTebBytes();
    ULONG_PTR base = *(ULONG_PTR *)(t + TEB_STACK_BASE), low = *(ULONG_PTR *)(t + TEB_DEALLOCATION_STACK);
    ULONG_PTR a = (ULONG_PTR)f;
    if (a & 3) return FALSE;
    if (base && (a >= base || (low && a < low))) return FALSE;
    return TRUE;
}

PVOID NTAPI RtlPcToFileHeader(PVOID pc, PVOID *base)
{
    PLDR_DATA_TABLE_ENTRY e = LdrNovaFindEntry(pc);
    PVOID b = e ? e->DllBase : 0;
    if (base) *base = b;
    return b;
}

BOOLEAN NTAPI RtlDispatchException(PEXCEPTION_RECORD rec, PCONTEXT ctx)
{
    EXCEPTION_POINTERS ep = { rec, ctx };
    if (run_vectored(g_veh, &ep) == EXCEPTION_CONTINUE_EXECUTION) return TRUE;
    for (EXCEPTION_REGISTRATION_RECORD *f = seh_head(); f && f != EXCEPTION_CHAIN_END; f = f->Next) {
        if (!frame_ok(f)) { rec->ExceptionFlags |= EXCEPTION_STACK_INVALID; break; }
        PVOID dc = 0;
        EXCEPTION_DISPOSITION d = nova_call_handler(rec, f, ctx, &dc, f->Handler);
        if (d == ExceptionContinueExecution) {
            if (rec->ExceptionFlags & EXCEPTION_NONCONTINUABLE) return FALSE;
            return TRUE;
        }
        /* ExceptionContinueSearch (and the nested cases): the next frame */
    }
    return FALSE;
}

/* RtlUnwind: call the handlers above @target with EXCEPTION_UNWINDING and
 * take them off the chain (all of them if @target is NULL), then return */
VOID NTAPI RtlUnwind(PVOID target, PVOID target_ip, PEXCEPTION_RECORD rec, PVOID retval)
{
    (void)retval;
    EXCEPTION_RECORD local;
    if (!rec) {
        memset(&local, 0, sizeof(local));
        local.ExceptionCode = 0xC0000027;            /* STATUS_UNWIND */
        local.ExceptionAddress = target_ip;
        rec = &local;
    }
    rec->ExceptionFlags |= EXCEPTION_UNWINDING;
    if (!target) rec->ExceptionFlags |= EXCEPTION_EXIT_UNWIND;
    CONTEXT ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_FULL;
    EXCEPTION_REGISTRATION_RECORD *f = seh_head();
    while (f && f != EXCEPTION_CHAIN_END && f != (EXCEPTION_REGISTRATION_RECORD *)target) {
        if (!frame_ok(f)) break;
        PVOID dc = 0;
        nova_call_handler(rec, f, &ctx, &dc, f->Handler);
        f = f->Next;
        seh_set_head(f);
    }
}

/* RtlRaiseException(rec): the caller's registers into a CONTEXT, then
 * dispatch; a handled exception resumes after the call */
__declspec(noreturn) void nova_raise32(PEXCEPTION_RECORD rec, PCONTEXT ctx) __asm__("nova_raise32");
void nova_raise32(PEXCEPTION_RECORD rec, PCONTEXT ctx)
{
    rec->ExceptionAddress = (PVOID)(ULONG_PTR)ctx->Eip;
    if (RtlDispatchException(rec, ctx)) NtContinue(ctx, FALSE);
    else {
        run_vectored(g_vch, &(EXCEPTION_POINTERS){ rec, ctx });
        NtRaiseException(rec, ctx, FALSE);
    }
    for (;;) NtTerminateProcess(NtCurrentProcess(), rec->ExceptionCode);
}

/* CONTEXT (x86) offsets */
#define X86_CAPTURE(base)                                                          \
    "movl %eax, 0xB0" base "\n\t" "movl %ecx, 0xAC" base "\n\t"                    \
    "movl %edx, 0xA8" base "\n\t" "movl %ebx, 0xA4" base "\n\t"                    \
    "movl %esi, 0xA0" base "\n\t" "movl %edi, 0x9C" base "\n\t"                    \
    "xorl %eax, %eax\n\t"                                                          \
    "movw %cs, %ax\n\t" "movl %eax, 0xBC" base "\n\t"                              \
    "movw %ss, %ax\n\t" "movl %eax, 0xC8" base "\n\t"                              \
    "movw %ds, %ax\n\t" "movl %eax, 0x98" base "\n\t"                              \
    "movw %es, %ax\n\t" "movl %eax, 0x94" base "\n\t"                              \
    "movw %fs, %ax\n\t" "movl %eax, 0x90" base "\n\t"                              \
    "movw %gs, %ax\n\t" "movl %eax, 0x8C" base "\n\t"                              \
    "pushfl\n\t" "popl %eax\n\t" "movl %eax, 0xC0" base "\n\t"                     \
    "movl $0x10007, %eax\n\t" "movl %eax, 0" base "\n\t"

__asm__(
    ".text\n"
    ".globl _RtlRaiseException@4\n"
    "_RtlRaiseException@4:\n\t"
    "pushl %ebp\n\t"
    "movl %esp, %ebp\n\t"
    "subl $0x2D0, %esp\n\t"
    "andl $-16, %esp\n\t"
    X86_CAPTURE("(%esp)")
    "movl 0(%ebp), %eax\n\t" "movl %eax, 0xB4(%esp)\n\t"      /* the caller's EBP */
    "movl 4(%ebp), %eax\n\t" "movl %eax, 0xB8(%esp)\n\t"      /* EIP: the return address */
    "leal 12(%ebp), %eax\n\t" "movl %eax, 0xC4(%esp)\n\t"     /* ESP: after the return (ret 4) */
    "movl 0xB0(%esp), %eax\n\t"
    "movl %esp, %ecx\n\t"
    "pushl %ecx\n\t"
    "pushl 8(%ebp)\n\t"
    "calll nova_raise32\n\t"
    "int3\n"
    NOVA_X86_EXPORT("_RtlRaiseException@4"));

/* RtlCaptureContext(ctx): the caller's registers, as after this returns */
__asm__(
    ".text\n"
    ".globl _RtlCaptureContext@4\n"
    "_RtlCaptureContext@4:\n\t"
    "pushl %ebx\n\t"
    "movl 8(%esp), %ebx\n\t"
    "movl %eax, 0xB0(%ebx)\n\t"
    "movl (%esp), %eax\n\t"
    "movl %eax, 0xA4(%ebx)\n\t"                               /* EBX */
    "movl 0xB0(%ebx), %eax\n\t"
    "movl %ecx, 0xAC(%ebx)\n\t" "movl %edx, 0xA8(%ebx)\n\t"
    "movl %esi, 0xA0(%ebx)\n\t" "movl %edi, 0x9C(%ebx)\n\t"
    "movl %ebp, 0xB4(%ebx)\n\t"
    "movl 4(%esp), %eax\n\t" "movl %eax, 0xB8(%ebx)\n\t"      /* EIP */
    "leal 12(%esp), %eax\n\t" "movl %eax, 0xC4(%ebx)\n\t"     /* ESP after ret 4 */
    "xorl %eax, %eax\n\t"
    "movw %cs, %ax\n\t" "movl %eax, 0xBC(%ebx)\n\t"
    "movw %ss, %ax\n\t" "movl %eax, 0xC8(%ebx)\n\t"
    "movw %ds, %ax\n\t" "movl %eax, 0x98(%ebx)\n\t"
    "movw %es, %ax\n\t" "movl %eax, 0x94(%ebx)\n\t"
    "movw %fs, %ax\n\t" "movl %eax, 0x90(%ebx)\n\t"
    "movw %gs, %ax\n\t" "movl %eax, 0x8C(%ebx)\n\t"
    "pushfl\n\t" "popl %eax\n\t" "movl %eax, 0xC0(%ebx)\n\t"
    "movl $0x10007, 0(%ebx)\n\t"
    "movl 0xB0(%ebx), %eax\n\t"
    "popl %ebx\n\t"
    "retl $4\n"
    NOVA_X86_EXPORT("_RtlCaptureContext@4"));

VOID NTAPI RtlRestoreContext(PCONTEXT c, PEXCEPTION_RECORD rec)
{
    (void)rec;
    NtContinue(c, FALSE);
}

/* -----------------------------------------------------------------------
 * The kernel's way in (ECX = 64-bit CONTEXT, EDX = 64-bit EXCEPTION_RECORD)
 * ----------------------------------------------------------------------- */
__declspec(noreturn) void nova_dispatch32(const BYTE *c64, const BYTE *r64) __asm__("nova_dispatch32");
void nova_dispatch32(const BYTE *c64, const BYTE *r64)
{
    CONTEXT ctx;
    EXCEPTION_RECORD rec;
    nova_context_from64(&ctx, c64);
    nova_record_from64(&rec, r64);
    if (RtlDispatchException(&rec, &ctx)) {
        NtContinue(&ctx, FALSE);                     /* handled or fixed up: resume */
    } else {
        run_vectored(g_vch, &(EXCEPTION_POINTERS){ &rec, &ctx });
        NtRaiseException(&rec, &ctx, FALSE);         /* second chance → the kernel ends us */
    }
    for (;;) NtTerminateProcess(NtCurrentProcess(), rec.ExceptionCode);
}

__asm__(
    ".text\n"
    ".globl _KiUserExceptionDispatcher\n"
    "_KiUserExceptionDispatcher:\n\t"
    "cld\n\t"
    "pushl %edx\n\t"
    "pushl %ecx\n\t"
    "calll nova_dispatch32\n\t"
    "int3\n"
    NOVA_X86_EXPORT("_KiUserExceptionDispatcher"));

/* -----------------------------------------------------------------------
 * __try/__except/__finally: the language handlers (MSVC and clang)
 * ----------------------------------------------------------------------- */
typedef struct { LONG prev; PVOID filter, handler; } SCOPE_ENTRY;        /* filter NULL: __finally */
typedef struct EH3_FRAME {
    struct EH3_FRAME *next;
    PVOID handler;
    ULONG_PTR scope;                  /* SCOPE_ENTRY[] (EH4: xor the security cookie; after a 16-byte header) */
    LONG trylevel;                    /* the innermost active scope; -1 (EH4: -2): none */
} EH3_FRAME;                          /* ESP at [-8], EXCEPTION_POINTERS* at [-4]; the function's EBP at +16 */

#define FRAME_EBP(f) ((PVOID)((BYTE *)(f) + 16))

/* run __finally blocks from the current level up to (not including) @stop */
static void local_unwind(EH3_FRAME *f, const SCOPE_ENTRY *scope, LONG stop, LONG none)
{
    while (f->trylevel != none && f->trylevel != stop) {
        const SCOPE_ENTRY *e = &scope[f->trylevel];
        f->trylevel = e->prev;
        if (!e->filter && e->handler) nova_call_ebp(e->handler, FRAME_EBP(f));
    }
}

static EXCEPTION_DISPOSITION eh_common(PEXCEPTION_RECORD rec, EH3_FRAME *f, PCONTEXT ctx,
                                       const SCOPE_ENTRY *scope, LONG none)
{
    if (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) {
        local_unwind(f, scope, none, none);
        return ExceptionContinueSearch;
    }
    EXCEPTION_POINTERS ep = { rec, ctx };
    ((EXCEPTION_POINTERS **)f)[-1] = &ep;
    for (LONG lvl = f->trylevel; lvl != none && lvl >= 0; lvl = scope[lvl].prev) {
        const SCOPE_ENTRY *e = &scope[lvl];
        if (!e->filter) continue;
        LONG r = nova_call_ebp(e->filter, FRAME_EBP(f));
        if (r < 0) return ExceptionContinueExecution;
        if (r > 0) {
            RtlUnwind(f, 0, rec, 0);                 /* the frames above this one */
            local_unwind(f, scope, lvl, none);       /* this one's inner __finally blocks */
            f->trylevel = e->prev;
            nova_jump_ebp(e->handler, FRAME_EBP(f));
        }
    }
    return ExceptionContinueSearch;
}

__declspec(dllexport) EXCEPTION_DISPOSITION __cdecl _except_handler3(PEXCEPTION_RECORD rec, PVOID frame,
                                                                    PCONTEXT ctx, PVOID dc)
{
    (void)dc;
    EH3_FRAME *f = frame;
    return eh_common(rec, f, ctx, (const SCOPE_ENTRY *)f->scope, -1);
}

__declspec(dllexport) EXCEPTION_DISPOSITION __cdecl _except_handler2(PEXCEPTION_RECORD rec, PVOID frame,
                                                                    PCONTEXT ctx, PVOID dc)
{
    return _except_handler3(rec, frame, ctx, dc);
}

/* VS2005 and later: scope table pointer xor the security cookie, after a
 * header of GS/EH cookie offsets; "no scope" is -2 */
__declspec(dllexport) EXCEPTION_DISPOSITION __cdecl _except_handler4_common(ULONG_PTR *cookie, PVOID check,
                                                                           PEXCEPTION_RECORD rec, PVOID frame,
                                                                           PCONTEXT ctx, PVOID dc)
{
    (void)check; (void)dc;
    EH3_FRAME *f = frame;
    const BYTE *table = (const BYTE *)(f->scope ^ *cookie);
    return eh_common(rec, f, ctx, (const SCOPE_ENTRY *)(table + 16), -2);
}

/* _local_unwind2(frame, stop): the __finally blocks of one EH3 frame */
__declspec(dllexport) void __cdecl _local_unwind2(PVOID frame, LONG stop)
{
    EH3_FRAME *f = frame;
    local_unwind(f, (const SCOPE_ENTRY *)f->scope, stop, -1);
}

__declspec(dllexport) void __cdecl _local_unwind4(ULONG_PTR *cookie, PVOID frame, LONG stop)
{
    EH3_FRAME *f = frame;
    const BYTE *table = (const BYTE *)(f->scope ^ *cookie);
    local_unwind(f, (const SCOPE_ENTRY *)(table + 16), stop, -2);
}

/* _global_unwind2(frame): unwind the frames above @frame */
__declspec(dllexport) void __cdecl _global_unwind2(PVOID frame)
{
    RtlUnwind(frame, 0, 0, 0);
}
