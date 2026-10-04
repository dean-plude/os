/* unwindtest.exe — RtlUnwindEx the way LLVM's libunwind and GCC's
 * libgcc use it (MinGW-built C++ programs: Krita, its Qt and KDE
 * libraries).  Their throw raises STATUS_GCC_THROW; the frame's language
 * handler starts the unwind from the dispatch, and at the target frame
 * starts a second, "collided" unwind to the landing pad with a CONTEXT it
 * never filled in (only scratch space on Windows).  A landing pad that
 * ends in _Unwind_Resume calls RtlUnwindEx from no handler at all, again
 * with an empty CONTEXT.
 *
 * Also the consolidating unwind of Microsoft's own C++ runtime
 * (vcruntime140, which programs built with MSVC may carry or install):
 * its catch block runs from RtlUnwindEx's STATUS_UNWIND_CONSOLIDATE
 * callback, and a throw from there (a rethrow, a new exception) must reach
 * the frames above the catching one (GOG Galaxy's service and client). */
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <string.h>

#define STATUS_GCC_THROW  0x20474343
#define STATUS_GCC_UNWIND 0x21474343

static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)

static int handler_calls, target_calls, cleanups;
UINT64 landed_rax, landed_rdx, landed_flags, resumed_rax, resumed_flags;

/* Garbage, as an uninitialised CONTEXT may hold: the trap flag in EFlags
 * among it */
static void scratch(CONTEXT *c) { memset(c, 0xFF, sizeof(*c)); }

/* The language handler of catcher() (below), doing what
 * _GCC_specific_handler does for a frame whose personality catches */
EXCEPTION_DISPOSITION catch_handler(EXCEPTION_RECORD *rec, void *frame, CONTEXT *ctx, DISPATCHER_CONTEXT *dc)
{
    (void)ctx;
    handler_calls++;
    if (rec->ExceptionCode == STATUS_GCC_UNWIND) {
        /* the collided unwind reached its target: the second value for
         * the landing pad (RAX was RtlUnwindEx's return value) */
        if (rec->ExceptionFlags & EXCEPTION_TARGET_UNWIND) { target_calls++; dc->ContextRecord->Rdx = 0x5678; }
        return ExceptionContinueSearch;
    }
    if (rec->ExceptionCode != STATUS_GCC_THROW) return ExceptionContinueSearch;
    if (!(rec->ExceptionFlags & EXCEPTION_UNWINDING)) {
        /* search phase: found; start the unwind from the dispatch */
        RtlUnwindEx(frame, (void *)dc->ControlPc, rec, 0, dc->ContextRecord, dc->HistoryTable);
        return ExceptionContinueSearch;                  /* (not reached) */
    }
    if (rec->ExceptionFlags & EXCEPTION_TARGET_UNWIND) {
        /* install the landing pad: a collided unwind, empty CONTEXT */
        CONTEXT scratch_ctx;
        scratch(&scratch_ctx);
        rec->ExceptionCode = STATUS_GCC_UNWIND;
        RtlUnwindEx(frame, *(void **)dc->HandlerData, rec, (void *)0x1234, &scratch_ctx, dc->HistoryTable);
    }
    return ExceptionContinueSearch;
}

/* A frame between the thrower and the catcher with a cleanup */
EXCEPTION_DISPOSITION cleanup_handler(EXCEPTION_RECORD *rec, void *frame, CONTEXT *ctx, DISPATCHER_CONTEXT *dc)
{
    (void)frame; (void)ctx; (void)dc;
    if ((rec->ExceptionFlags & EXCEPTION_UNWINDING) && rec->ExceptionCode == STATUS_GCC_THROW) cleanups++;
    return ExceptionContinueSearch;
}

__declspec(noinline) void thrower(void)
{
    RaiseException(STATUS_GCC_THROW, 0, 0, 0);
}

/* _Unwind_Resume: unwind to @frame and continue at @ip, from no handler */
__declspec(noinline) void resume_to(void *frame, void *ip)
{
    CONTEXT c;
    scratch(&c);
    EXCEPTION_RECORD rec;
    memset(&rec, 0, sizeof(rec));
    rec.ExceptionCode = STATUS_GCC_THROW;
    rec.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    RtlUnwindEx(frame, ip, &rec, (void *)0x4321, &c, 0);
}

/* catcher(): calls middle() -> thrower(); its handler's data is the
 * landing pad's address.  middle() has a cleanup handler.  resumer():
 * passes its own frame to resume_to(), which unwinds back into it. */
__asm__(
    ".text\n"
    ".globl catcher\n"
    ".def catcher; .scl 2; .type 32; .endef\n"
    ".seh_proc catcher\n"
    "catcher:\n\t"
    "pushq %rbx\n\t" ".seh_pushreg %rbx\n\t"
    "subq $32, %rsp\n\t" ".seh_stackalloc 32\n\t"
    ".seh_endprologue\n\t"
    "movq $0x77, %rbx\n\t"                       /* a nonvolatile the landing pad must see */
    "callq middle\n\t"
    "nop\n\t"
    "xorl %eax, %eax\n\t"                        /* (not reached: thrower throws) */
    "jmp 1f\n"
    "catcher_pad:\n\t"
    "movq %rax, landed_rax(%rip)\n\t"
    "movq %rdx, landed_rdx(%rip)\n\t"
    "pushfq\n\t" "popq %rax\n\t" "movq %rax, landed_flags(%rip)\n\t"
    "movq %rbx, %rax\n"                          /* returns 0x77 when RBX was restored */
    "1:\n\t"
    "addq $32, %rsp\n\t"
    "popq %rbx\n\t"
    "retq\n\t"
    ".seh_handler catch_handler, @unwind, @except\n\t"
    ".seh_handlerdata\n\t"
    ".quad catcher_pad\n\t"
    ".text\n\t"
    ".seh_endproc\n"

    ".globl middle\n"
    ".def middle; .scl 2; .type 32; .endef\n"
    ".seh_proc middle\n"
    "middle:\n\t"
    "subq $40, %rsp\n\t" ".seh_stackalloc 40\n\t"
    ".seh_endprologue\n\t"
    "callq thrower\n\t"
    "nop\n\t"
    "addq $40, %rsp\n\t"
    "retq\n\t"
    ".seh_handler cleanup_handler, @unwind\n\t"
    ".text\n\t"
    ".seh_endproc\n"

    ".globl resumer\n"
    ".def resumer; .scl 2; .type 32; .endef\n"
    ".seh_proc resumer\n"
    "resumer:\n\t"
    "pushq %rbx\n\t" ".seh_pushreg %rbx\n\t"
    "subq $32, %rsp\n\t" ".seh_stackalloc 32\n\t"
    ".seh_endprologue\n\t"
    "movq $0x99, %rbx\n\t"
    "movq %rsp, %rcx\n\t"                        /* the establisher frame */
    "leaq resumer_pad(%rip), %rdx\n\t"
    "callq resume_to\n\t"
    "nop\n\t"
    "xorl %eax, %eax\n\t"
    "jmp 2f\n"
    "resumer_pad:\n\t"
    "movq %rax, resumed_rax(%rip)\n\t"
    "pushfq\n\t" "popq %rax\n\t" "movq %rax, resumed_flags(%rip)\n\t"
    "movq %rbx, %rax\n"
    "2:\n\t"
    "addq $32, %rsp\n\t"
    "popq %rbx\n\t"
    "retq\n\t"
    ".seh_endproc\n");

/* MSVC's C++ runtime: its frame handler finds a catch in the search phase
 * and unwinds to the frame with a consolidation record, whose callback
 * runs the catch block and returns where the frame continues */
#define CODE_FIRST  0xE0000001
#define CODE_SECOND 0xE0000002
static int catch_runs, raise_in_catch, second_seen_by_msvc, outer_caught;

static PVOID msvc_catch_block(EXCEPTION_RECORD *rec)
{
    catch_runs++;
    if (raise_in_catch) RaiseException(CODE_SECOND, 0, 0, 0);    /* (does not return) */
    return (PVOID)rec->ExceptionInformation[2];                  /* the continuation */
}

EXCEPTION_DISPOSITION msvc_handler(EXCEPTION_RECORD *rec, void *frame, CONTEXT *ctx, DISPATCHER_CONTEXT *dc)
{
    (void)ctx;
    if (rec->ExceptionCode == CODE_SECOND && !(rec->ExceptionFlags & EXCEPTION_UNWINDING)) second_seen_by_msvc++;
    if (rec->ExceptionCode != CODE_FIRST || (rec->ExceptionFlags & EXCEPTION_UNWINDING)) return ExceptionContinueSearch;
    EXCEPTION_RECORD c;
    memset(&c, 0, sizeof(c));
    c.ExceptionCode = STATUS_UNWIND_CONSOLIDATE;
    c.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    c.NumberParameters = 3;
    c.ExceptionInformation[0] = (ULONG_PTR)msvc_catch_block;
    c.ExceptionInformation[1] = (ULONG_PTR)frame;
    c.ExceptionInformation[2] = *(ULONG_PTR *)dc->HandlerData;
    RtlUnwindEx(frame, (void *)dc->ControlPc, &c, 0, dc->ContextRecord, dc->HistoryTable);
    return ExceptionContinueSearch;                      /* (not reached) */
}

__declspec(noinline) void thrower_first(void)
{
    RaiseException(CODE_FIRST, 0, 0, 0);
}

/* msvc_catcher(): calls thrower_first(); its handler's data is where the
 * frame continues after the catch block */
__asm__(
    ".text\n"
    ".globl msvc_catcher\n"
    ".def msvc_catcher; .scl 2; .type 32; .endef\n"
    ".seh_proc msvc_catcher\n"
    "msvc_catcher:\n\t"
    "pushq %rbx\n\t" ".seh_pushreg %rbx\n\t"
    "subq $32, %rsp\n\t" ".seh_stackalloc 32\n\t"
    ".seh_endprologue\n\t"
    "movq $0x55, %rbx\n\t"
    "callq thrower_first\n\t"
    "nop\n\t"
    "xorl %eax, %eax\n\t"
    "jmp 3f\n"
    "msvc_continue:\n\t"
    "movq %rbx, %rax\n"                          /* 0x55 when RBX was restored */
    "3:\n\t"
    "addq $32, %rsp\n\t"
    "popq %rbx\n\t"
    "retq\n\t"
    ".seh_handler msvc_handler, @unwind, @except\n\t"
    ".seh_handlerdata\n\t"
    ".quad msvc_continue\n\t"
    ".text\n\t"
    ".seh_endproc\n");

UINT64 catcher(void);
UINT64 resumer(void);
UINT64 msvc_catcher(void);

/* A frame above the catching one, with an __except for what its catch
 * block raises */
__declspec(noinline) static UINT64 outer_try(void)
{
    UINT64 r = 0;
    __try {
        r = msvc_catcher();
    } __except (GetExceptionCode() == CODE_SECOND ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        outer_caught++;
        r = 0xEE;
    }
    return r;
}

/* An __except inside a __try/__finally of the same frame: unwinding to
 * the __except body leaves the __finally in force (it runs once, when the
 * frame leaves its __try).  A raise from that __except body (MSVC's C++
 * runtime re-raises a rethrow there) runs it once, on the way out. */
static int finally_runs, except_runs;
__declspec(noinline) static void raise_code(DWORD code) { RaiseException(code, 0, 0, 0); }
__declspec(noinline) static int except_in_finally(int raise_again)
{
    __try {
        __try {
            raise_code(CODE_FIRST);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            except_runs++;
            if (raise_again) raise_code(CODE_SECOND);
        }
    } __finally {
        finally_runs++;
    }
    return 1;
}

__declspec(noinline) static int catch_raise_from_except(void)
{
    __try {
        except_in_finally(1);
    } __except (GetExceptionCode() == CODE_SECOND ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return 2;
    }
    return 0;
}

int main(void)
{
    /* a throw caught by a frame two levels up */
    UINT64 r = catcher();
    CHECK("landing pad reached with the frame's registers", r == 0x77);
    CHECK("landing pad's RAX is RtlUnwindEx's return value", landed_rax == 0x1234);
    CHECK("landing pad's RDX set by the handler at the target", landed_rdx == 0x5678 && target_calls == 1);
    CHECK("no trap flag from the empty CONTEXT", !(landed_flags & 0x100));
    CHECK("the frame in between cleaned up once", cleanups == 1);

    /* again: per-thread state was left clean */
    target_calls = cleanups = 0;
    landed_rax = landed_rdx = 0;
    r = catcher();
    CHECK("a second throw lands too", r == 0x77 && landed_rax == 0x1234 && landed_rdx == 0x5678 && cleanups == 1);

    /* _Unwind_Resume: RtlUnwindEx outside any handler */
    r = resumer();
    CHECK("unwind from no handler resumes in the target frame", r == 0x99);
    CHECK("its RAX is RtlUnwindEx's return value", resumed_rax == 0x4321);
    CHECK("no trap flag there either", !(resumed_flags & 0x100));

    /* MSVC-style catch block run by a consolidating unwind */
    r = outer_try();
    CHECK("consolidation callback's catch block ran once", catch_runs == 1);
    CHECK("the frame continues where the callback said, its registers restored", r == 0x55 && outer_caught == 0);

    /* ... and raising in that catch block: the frames above the catching
     * one are searched, the catching frame's own handler first */
    catch_runs = 0;
    raise_in_catch = 1;
    r = outer_try();
    CHECK("an exception raised in the catch block reaches the caller's __except", r == 0xEE && outer_caught == 1);
    CHECK("the catching frame's handler saw it on the way", second_seen_by_msvc == 1 && catch_runs == 1);

    /* again: the per-thread state was left clean */
    raise_in_catch = 0;
    catch_runs = outer_caught = 0;
    r = outer_try();
    CHECK("a plain catch works after that", r == 0x55 && catch_runs == 1 && outer_caught == 0);

    /* the target frame's own __finally around the __except */
    int k = except_in_finally(0);
    CHECK("an __except inside a __finally: the __finally runs once, after", k == 1 && except_runs == 1 && finally_runs == 1);
    except_runs = finally_runs = 0;
    k = catch_raise_from_except();
    CHECK("a raise from that __except body runs the __finally once", k == 2 && except_runs == 1 && finally_runs == 1);

    printf("unwindtest: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
