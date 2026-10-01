/*
 * kernel32 — fibers: user-scheduled execution contexts on their own
 * stacks.  A fiber keeps its registers on its stack while it is not
 * running; SwitchToFiber saves the callee-saved registers of the running
 * one, switches stacks and restores the other's.  The TEB follows the
 * running fiber: its stack bounds, the fiber pointer (NtTib.FiberData,
 * which GetCurrentFiber and GetFiberData read) and, on x86, the SEH list.
 */
#include <windows.h>
#include <winternl.h>

#define K32 __declspec(dllexport)

typedef VOID (WINAPI *FIBER_START)(LPVOID);

typedef struct {
    LPVOID param;                   /* first: GetFiberData() reads it */
    ULONG_PTR sp;                   /* its saved stack pointer while not running */
    ULONG_PTR stack_base, stack_limit, dealloc;
    ULONG_PTR seh;                  /* x86: the exception registration list */
    FIBER_START start;
    BOOL converted;                 /* made from a thread (its stack is the thread's) */
    DWORD flags;
} Fiber;

#undef TEB_EXCEPTION_LIST
#undef TEB_STACK_BASE
#undef TEB_STACK_LIMIT
#undef TEB_FIBER_DATA
#ifdef _WIN64
#define TEB_EXCEPTION_LIST 0x0
#define TEB_STACK_BASE     0x8
#define TEB_STACK_LIMIT    0x10
#define TEB_FIBER_DATA     0x20
#define TEB_DEALLOC        0x1478
#define TEB_SAME_FLAGS     0x17EE
#else
#define TEB_EXCEPTION_LIST 0x0
#define TEB_STACK_BASE     0x4
#define TEB_STACK_LIMIT    0x8
#define TEB_FIBER_DATA     0x10
#define TEB_DEALLOC        0xE0C
#define TEB_SAME_FLAGS     0xFCA
#endif
#define HAS_FIBER_DATA     0x4       /* TEB SameTebFlags */

#define TEBP(off) ((ULONG_PTR *)(NtCurrentTebBytes() + (off)))

/* fiber_swap(ULONG_PTR *save_sp, ULONG_PTR new_sp) */
void fiber_swap(ULONG_PTR *save, ULONG_PTR sp);
void fiber_entry(void);
#ifdef _WIN64
__asm__(".text\n\t.globl fiber_swap\nfiber_swap:\n\t"
        "pushq %rbp\n\tpushq %rbx\n\tpushq %rdi\n\tpushq %rsi\n\t"
        "pushq %r12\n\tpushq %r13\n\tpushq %r14\n\tpushq %r15\n\t"
        "subq $168, %rsp\n\t"
        "movdqu %xmm6, 0(%rsp)\n\tmovdqu %xmm7, 16(%rsp)\n\tmovdqu %xmm8, 32(%rsp)\n\tmovdqu %xmm9, 48(%rsp)\n\t"
        "movdqu %xmm10, 64(%rsp)\n\tmovdqu %xmm11, 80(%rsp)\n\tmovdqu %xmm12, 96(%rsp)\n\tmovdqu %xmm13, 112(%rsp)\n\t"
        "movdqu %xmm14, 128(%rsp)\n\tmovdqu %xmm15, 144(%rsp)\n\t"
        "stmxcsr 160(%rsp)\n\tfnstcw 164(%rsp)\n\t"
        "movq %rsp, (%rcx)\n\t"
        "movq %rdx, %rsp\n\t"
        "movdqu 0(%rsp), %xmm6\n\tmovdqu 16(%rsp), %xmm7\n\tmovdqu 32(%rsp), %xmm8\n\tmovdqu 48(%rsp), %xmm9\n\t"
        "movdqu 64(%rsp), %xmm10\n\tmovdqu 80(%rsp), %xmm11\n\tmovdqu 96(%rsp), %xmm12\n\tmovdqu 112(%rsp), %xmm13\n\t"
        "movdqu 128(%rsp), %xmm14\n\tmovdqu 144(%rsp), %xmm15\n\t"
        "ldmxcsr 160(%rsp)\n\tfldcw 164(%rsp)\n\t"
        "addq $168, %rsp\n\t"
        "popq %r15\n\tpopq %r14\n\tpopq %r13\n\tpopq %r12\n\t"
        "popq %rsi\n\tpopq %rdi\n\tpopq %rbx\n\tpopq %rbp\n\t"
        "ret\n\t"
        ".globl fiber_entry\nfiber_entry:\n\t"
        "andq $-16, %rsp\n\tsubq $32, %rsp\n\tcall fiber_run\n\tint3\n");
#define SAVED_BYTES (168 + 8 * 8)
#else
__asm__(".text\n\t.globl _fiber_swap\n_fiber_swap:\n\t"
        "movl 4(%esp), %ecx\n\tmovl 8(%esp), %edx\n\t"
        "pushl %ebp\n\tpushl %ebx\n\tpushl %edi\n\tpushl %esi\n\t"
        "subl $8, %esp\n\tstmxcsr 0(%esp)\n\tfnstcw 4(%esp)\n\t"
        "movl %esp, (%ecx)\n\t"
        "movl %edx, %esp\n\t"
        "ldmxcsr 0(%esp)\n\tfldcw 4(%esp)\n\taddl $8, %esp\n\t"
        "popl %esi\n\tpopl %edi\n\tpopl %ebx\n\tpopl %ebp\n\t"
        "ret\n\t"
        ".globl _fiber_entry\n_fiber_entry:\n\t"
        "andl $-16, %esp\n\tcall _fiber_run\n\tint3\n");
#define SAVED_BYTES (8 + 4 * 4)
#endif

static Fiber *current(void) { return (Fiber *)*TEBP(TEB_FIBER_DATA); }

/* The first code a new fiber runs */
void fiber_run(void)
{
    Fiber *f = current();
    f->start(f->param);
    ExitThread(0);                                  /* a fiber routine that returns ends the thread */
}

K32 BOOL WINAPI IsThreadAFiber(void)
{
    return (*(WORD *)(NtCurrentTebBytes() + TEB_SAME_FLAGS) & HAS_FIBER_DATA) != 0;
}

K32 LPVOID WINAPI ConvertThreadToFiberEx(LPVOID param, DWORD flags)
{
    if (IsThreadAFiber()) { SetLastError(1280 /* ERROR_ALREADY_FIBER */); return 0; }
    Fiber *f = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Fiber));
    if (!f) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    f->param = param;
    f->flags = flags;
    f->converted = TRUE;
    *TEBP(TEB_FIBER_DATA) = (ULONG_PTR)f;
    *(WORD *)(NtCurrentTebBytes() + TEB_SAME_FLAGS) |= HAS_FIBER_DATA;
    return f;
}
K32 LPVOID WINAPI ConvertThreadToFiber(LPVOID param) { return ConvertThreadToFiberEx(param, 0); }

K32 BOOL WINAPI ConvertFiberToThread(void)
{
    if (!IsThreadAFiber()) { SetLastError(1281 /* ERROR_ALREADY_THREAD */); return FALSE; }
    Fiber *f = current();
    *TEBP(TEB_FIBER_DATA) = 0;
    *(WORD *)(NtCurrentTebBytes() + TEB_SAME_FLAGS) &= (WORD)~HAS_FIBER_DATA;
    if (f && f->converted) HeapFree(GetProcessHeap(), 0, f);
    return TRUE;
}

K32 LPVOID WINAPI CreateFiberEx(SIZE_T commit, SIZE_T reserve, DWORD flags, FIBER_START start, LPVOID param)
{
    (void)commit;
    if (!reserve) reserve = 1024 * 1024;
    reserve = (reserve + 0xFFFF) & ~(SIZE_T)0xFFFF;
    BYTE *stack = VirtualAlloc(0, reserve, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!stack) return 0;
    Fiber *f = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Fiber));
    if (!f) { VirtualFree(stack, 0, MEM_RELEASE); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    f->param = param;
    f->flags = flags;
    f->start = start;
    f->dealloc = (ULONG_PTR)stack;
    f->stack_limit = (ULONG_PTR)stack;
    f->stack_base = (ULONG_PTR)stack + reserve;
    /* the frame fiber_swap pops: saved registers (zero, with the default
     * MXCSR and x87 control word), then fiber_entry as the return address */
    ULONG_PTR *sp = (ULONG_PTR *)(f->stack_base - 64);
    *--sp = (ULONG_PTR)fiber_entry;
    BYTE *regs = (BYTE *)sp - SAVED_BYTES;
    for (int i = 0; i < SAVED_BYTES; i++) regs[i] = 0;
#ifdef _WIN64
    *(DWORD *)(regs + 160) = 0x1F80;
    *(WORD *)(regs + 164) = 0x027F;
#else
    *(DWORD *)(regs + 0) = 0x1F80;
    *(WORD *)(regs + 4) = 0x027F;
#endif
    f->sp = (ULONG_PTR)regs;
    f->seh = (ULONG_PTR)-1;                         /* x86: an empty SEH list */
    return f;
}
K32 LPVOID WINAPI CreateFiber(SIZE_T stack, FIBER_START start, LPVOID param)
{
    return CreateFiberEx(stack, stack, 0, start, param);
}

K32 VOID WINAPI SwitchToFiber(LPVOID to)
{
    Fiber *cur = current(), *next = to;
    if (!cur || !next || cur == next) return;
    /* the running fiber's TEB fields go with it */
    cur->stack_base = *TEBP(TEB_STACK_BASE);
    cur->stack_limit = *TEBP(TEB_STACK_LIMIT);
    cur->dealloc = *TEBP(TEB_DEALLOC);
    cur->seh = *TEBP(TEB_EXCEPTION_LIST);
    *TEBP(TEB_STACK_BASE) = next->stack_base;
    *TEBP(TEB_STACK_LIMIT) = next->stack_limit;
    *TEBP(TEB_DEALLOC) = next->dealloc;
#ifndef _WIN64
    *TEBP(TEB_EXCEPTION_LIST) = next->seh;
#endif
    *TEBP(TEB_FIBER_DATA) = (ULONG_PTR)next;
    fiber_swap(&cur->sp, next->sp);
}

K32 VOID WINAPI DeleteFiber(LPVOID fiber)
{
    Fiber *f = fiber;
    if (!f) return;
    if (f == current()) ExitThread(0);              /* deleting the running fiber ends the thread */
    if (!f->converted && f->dealloc) VirtualFree((LPVOID)f->dealloc, 0, MEM_RELEASE);
    HeapFree(GetProcessHeap(), 0, f);
}
