/*
 * ps.c — NT Process and Thread Manager
 *
 * Key design points:
 *
 * 1. The scheduler's Thread object is EMBEDDED at the start of KTHREAD,
 *    which is embedded at the start of ETHREAD.  The scheduler only knows
 *    about Thread (Phase 1 type).  KTHREAD/ETHREAD extend it.
 *    We cast Thread* ↔ ETHREAD* freely since Thread is at offset 0.
 *
 * 2. System process (PsInitialSystemProcess) wraps the kernel's boot
 *    execution context.  It runs at ring 0 and never has a user VA space.
 *
 * 3. PID/TID allocation: simple monotonic counter.  Windows uses a different
 *    scheme (object table handle reuse) but a counter is fine for Phase 2.
 *
 * 4. Process list: doubly-linked EPROCESS.ActiveProcessLinks.
 *    The list head is PsActiveProcessHead.
 */

#include "ps.h"
#include "../ob/ob.h"
#include "../mm/vmm.h"
#include "../mm/vma.h"
#include "../ke/printf.h"
#include "../ke/scheduler.h"
#include "../lib/string.h"
#include "../include/types.h"
#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/gdt.h"
#include "../ldr/ldr.h"

/* -----------------------------------------------------------------------
 * Globals
 * ----------------------------------------------------------------------- */
PEPROCESS PsInitialSystemProcess;

static OBJECT_TYPE ps_process_type_storage;
static OBJECT_TYPE ps_thread_type_storage;

static volatile UINT64 ps_next_pid = 4;   /* PID 4 = System (like Windows) */
static volatile UINT64 ps_next_tid = 1;

/* Active process list (circular doubly-linked) */
static struct { void *Flink, *Blink; } PsActiveProcessHead;
static volatile UINT32 ps_proc_list_next, ps_proc_list_owner;

static void ps_list_lock(void) {
    UINT32 t = __atomic_fetch_add(&ps_proc_list_next, 1, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&ps_proc_list_owner, __ATOMIC_ACQUIRE) != t)
        pause_cpu();
}
static void ps_list_unlock(void) {
    __atomic_fetch_add(&ps_proc_list_owner, 1, __ATOMIC_RELEASE);
}

/* -----------------------------------------------------------------------
 * Object type callbacks
 * ----------------------------------------------------------------------- */
static void process_delete(void *obj)
{
    PEPROCESS p = (PEPROCESS)obj;
    if (p->ObjectTable) {
        ObDestroyHandleTable(p->ObjectTable);
        p->ObjectTable = NULL;
    }
    kprintf("[PS] Process '%s' PID=%lu deleted\n",
            p->ImageFileName, p->UniqueProcessId);
}

static void thread_delete(void *obj)
{
    PETHREAD t = (PETHREAD)obj;
    kprintf("[PS] Thread TID=%lu deleted\n", t->UniqueThread);
}

/* -----------------------------------------------------------------------
 * PsGetCurrentProcess / PsGetCurrentThread
 *
 * The scheduler's current_thread IS the KTHREAD (which is the first member
 * of ETHREAD, which starts with KTHREAD).  We cast directly.
 * ----------------------------------------------------------------------- */
PEPROCESS PsGetCurrentProcess(void)
{
    PETHREAD et = PsGetCurrentThread();
    return et ? et->Process : PsInitialSystemProcess;
}

PETHREAD PsGetCurrentThread(void)
{
    Thread *t = sched_current();
    if (!t) return NULL;
    /* Thread is embedded at offset 0 in KTHREAD, which is at offset 0 in ETHREAD */
    return (PETHREAD)t;
}

PHANDLE_TABLE PsGetCurrentProcessHandleTable(void)
{
    PEPROCESS p = PsGetCurrentProcess();
    return p ? p->ObjectTable : NULL;
}

/* -----------------------------------------------------------------------
 * PID / TID allocation
 * ----------------------------------------------------------------------- */
static UINT64 alloc_pid(void)
{
    return __atomic_fetch_add(&ps_next_pid, 4, __ATOMIC_SEQ_CST);
}

static UINT64 alloc_tid(void)
{
    return __atomic_fetch_add(&ps_next_tid, 1, __ATOMIC_SEQ_CST);
}

/* -----------------------------------------------------------------------
 * Process active list helpers
 * ----------------------------------------------------------------------- */
static void ps_add_process(PEPROCESS p)
{
    ps_list_lock();
    void *head = &PsActiveProcessHead;
    void *tail = PsActiveProcessHead.Blink;
    if (!tail) {
        PsActiveProcessHead.Flink = p;
        PsActiveProcessHead.Blink = p;
        p->ActiveProcessLinks.Flink = head;
        p->ActiveProcessLinks.Blink = head;
    } else {
        PEPROCESS prev = (PEPROCESS)((char *)tail - __builtin_offsetof(EPROCESS, ActiveProcessLinks));
        prev->ActiveProcessLinks.Flink = p;
        p->ActiveProcessLinks.Blink = tail;
        p->ActiveProcessLinks.Flink = head;
        PsActiveProcessHead.Blink = p;
    }
    ps_list_unlock();
}

/* -----------------------------------------------------------------------
 * PsCreateSystemProcess
 * ----------------------------------------------------------------------- */
NTSTATUS PsCreateSystemProcess(
    PEPROCESS  *ProcessOut,
    const char *ImageFileName,
    void       *Token)
{
    OBJECT_ATTRIBUTES attr = { sizeof(OBJECT_ATTRIBUTES),
                                .Attributes = OBJ_KERNEL_HANDLE | OBJ_PERMANENT };
    void *proc_obj;
    NTSTATUS s = ObCreateObject(ObpProcessType, &attr, 0, &proc_obj);
    if (!NT_SUCCESS(s)) return s;

    PEPROCESS p = (PEPROCESS)proc_obj;
    __builtin_memset(p, 0, sizeof(EPROCESS));

    p->UniqueProcessId = alloc_pid();
    strncpy(p->ImageFileName, ImageFileName,
            EPROCESS_IMAGE_NAME_MAX - 1);
    p->ExitStatus = (NTSTATUS)0x103;  /* STATUS_PENDING */
    p->HasExited  = false;
    p->Token      = Token;

    /* Create handle table */
    s = ObCreateHandleTable(&p->ObjectTable);
    if (!NT_SUCCESS(s)) {
        ObDereferenceObject(proc_obj);
        return s;
    }

    /* Initialize the user virtual address space */
    VmaInitSpace(&p->VmaSpace);

    ps_add_process(p);

    if (ProcessOut) *ProcessOut = p;
    kprintf("[PS] Created process '%s' PID=%lu\n",
            p->ImageFileName, p->UniqueProcessId);
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * Thread trampoline context
 * A kernel ETHREAD wraps the Phase 1 Thread scheduler object.
 * We create an ETHREAD, then ask the scheduler to create the Thread
 * using ETHREAD.Tcb.SchedulerThread (which is at offset 0).
 * ----------------------------------------------------------------------- */

NTSTATUS PsCreateSystemThread(
    HANDLE             *ThreadHandle,
    ACCESS_MASK         DesiredAccess,
    POBJECT_ATTRIBUTES  ObjectAttributes,
    HANDLE              ProcessHandle,
    void               *ClientId,
    ThreadEntry         StartRoutine,
    void               *StartContext)
{
    (void)DesiredAccess; (void)ObjectAttributes; (void)ClientId;

    /* Resolve the process */
    PEPROCESS proc = NULL;
    if (ProcessHandle == (HANDLE)(ULONG_PTR)-1 || !ProcessHandle) {
        proc = PsGetCurrentProcess();
    } else {
        NTSTATUS s = ObReferenceObjectByHandle(ProcessHandle, PROCESS_ALL_ACCESS,
                                               ObpProcessType, NULL,
                                               (void **)&proc, NULL);
        if (!NT_SUCCESS(s)) return s;
    }
    if (!proc) proc = PsInitialSystemProcess;

    /* Allocate ETHREAD as the object body */
    OBJECT_ATTRIBUTES attr = { sizeof(OBJECT_ATTRIBUTES),
                                .Attributes = OBJ_KERNEL_HANDLE };
    void *thread_obj;
    NTSTATUS s = ObCreateObject(ObpThreadType, &attr, 0, &thread_obj);
    if (!NT_SUCCESS(s)) return s;

    PETHREAD et = (PETHREAD)thread_obj;
    __builtin_memset(et, 0, sizeof(ETHREAD));

    et->UniqueThread         = alloc_tid();
    et->Process              = proc;
    et->Cid.UniqueProcess    = proc->UniqueProcessId;
    et->Cid.UniqueThread     = et->UniqueThread;
    et->ExitStatus           = (NTSTATUS)0x103;  /* STATUS_PENDING */

    /* The scheduler Thread object is at offset 0 within KTHREAD within ETHREAD.
     * We initialize it directly (bypassing sched_create_thread which allocates
     * a separate Thread struct — we ARE the Thread struct). */
    Thread *sched_t = &et->Tcb.SchedulerThread;
    sched_t->tid      = et->UniqueThread;
    sched_t->priority = 8;
    sched_t->state    = THREAD_READY;
    strncpy(sched_t->name, proc->ImageFileName, THREAD_NAME_MAX - 1);

    /* Allocate the kernel stack */
    sched_t->kernel_stack = kernel_alloc_pages(THREAD_STACK_SIZE / PAGE_SIZE);
    if (!sched_t->kernel_stack) {
        ObDereferenceObject(thread_obj);
        return STATUS_NO_MEMORY;
    }
    sched_t->stack_size = THREAD_STACK_SIZE;

    /* Set up initial register context (same logic as scheduler.c) */
    uintptr_t stack_top = (uintptr_t)sched_t->kernel_stack + THREAD_STACK_SIZE;
    uintptr_t *sp = (uintptr_t *)stack_top;

    /* We can't call the thread_trampoline in scheduler.c (it's static),
     * so we create a local trampoline that calls StartRoutine(StartContext)
     * and then calls PsTerminateSystemThread(STATUS_SUCCESS). */
    extern void PsThreadTrampoline(void);  /* defined below as naked asm */

    *--sp = (uintptr_t)PsThreadTrampoline;  /* "return address" for context_switch */

    sched_t->context.rsp    = (uint64_t)(uintptr_t)sp;
    sched_t->context.rbp    = 0;
    sched_t->context.rbx    = 0;
    sched_t->context.r12    = (uint64_t)(uintptr_t)StartRoutine;
    sched_t->context.r13    = (uint64_t)(uintptr_t)StartContext;
    sched_t->context.r14    = (uint64_t)(uintptr_t)et;   /* ETHREAD in r14 */
    sched_t->context.r15    = 0;
    sched_t->context.rflags = 0x202;

    /* Add to the scheduler's ready queue (the scheduler sees it as a Thread) */
    extern void sched_enqueue_thread(Thread *t);  /* defined in scheduler.c */
    sched_enqueue_thread(sched_t);

    /* Create a handle if requested */
    if (ThreadHandle) {
        HANDLE h = 0;
        ObInsertObject(thread_obj, proc, THREAD_ALL_ACCESS, 0, NULL, &h);
        *ThreadHandle = h;
    }

    kprintf("[PS] Created thread TID=%lu in process '%s'\n",
            et->UniqueThread, proc->ImageFileName);
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * Thread entry trampoline (naked — sets up ABI, calls StartRoutine)
 * ----------------------------------------------------------------------- */
void __attribute__((naked, noreturn)) PsThreadTrampoline(void)
{
    __asm__ volatile (
        /* r12 = StartRoutine, r13 = StartContext, r14 = ETHREAD* */
        "mov  %%r13, %%rdi\n\t"    /* arg = StartContext */
        "call *%%r12\n\t"          /* StartRoutine(StartContext) */
        /* Thread returned — terminate */
        "mov $0, %%edi\n\t"        /* ExitStatus = STATUS_SUCCESS */
        "call PsTerminateSystemThread\n\t"
        "1: cli; hlt; jmp 1b\n\t"
        ::: "memory"
    );
}

/* -----------------------------------------------------------------------
 * PsTerminateSystemThread
 * ----------------------------------------------------------------------- */
NTSTATUS PsTerminateSystemThread(NTSTATUS ExitStatus)
{
    PETHREAD et = PsGetCurrentThread();
    if (et) {
        et->ExitStatus = ExitStatus;
        et->HasExited  = true;
        et->Tcb.SchedulerThread.state = THREAD_DEAD;
    }

    /* Yield — the scheduler will not pick this thread again (THREAD_DEAD) */
    for (;;) sched_yield();
}

/* -----------------------------------------------------------------------
 * PsTerminateProcess
 * ----------------------------------------------------------------------- */
NTSTATUS PsTerminateProcess(PEPROCESS Process, NTSTATUS ExitStatus)
{
    Process->ExitStatus = ExitStatus;
    Process->HasExited  = true;
    /* TODO Phase 3: terminate all threads in the process */
    kprintf("[PS] Process '%s' PID=%lu terminated (status=0x%x)\n",
            Process->ImageFileName, Process->UniqueProcessId, (UINT32)ExitStatus);
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * PsLookupProcessByProcessId / PsLookupThreadByThreadId
 * ----------------------------------------------------------------------- */
NTSTATUS PsLookupProcessByProcessId(UINT64 Pid, PEPROCESS *ProcessOut)
{
    ps_list_lock();
    void *cur = PsActiveProcessHead.Flink;
    void *head = &PsActiveProcessHead;
    while (cur && cur != head) {
        PEPROCESS p = (PEPROCESS)((char *)cur -
            __builtin_offsetof(EPROCESS, ActiveProcessLinks));
        if (p->UniqueProcessId == Pid) {
            ObReferenceObject(p);
            *ProcessOut = p;
            ps_list_unlock();
            return STATUS_SUCCESS;
        }
        cur = p->ActiveProcessLinks.Flink;
    }
    ps_list_unlock();
    return STATUS_NOT_FOUND;
}

NTSTATUS PsLookupThreadByThreadId(UINT64 Tid, PETHREAD *ThreadOut)
{
    /* Walk the scheduler's thread list to find TID */
    (void)Tid; (void)ThreadOut;
    /* TODO: implement a global thread table in Phase 3 */
    return STATUS_NOT_FOUND;
}

/* -----------------------------------------------------------------------
 * User-mode thread context (passed through PsUserThreadEntry)
 * ----------------------------------------------------------------------- */
typedef struct _USER_THREAD_CONTEXT {
    UINT64  EntryPoint;   /* User VA of the PE entry point */
    UINT64  StackTop;     /* User VA of the initial stack top */
    UINT64  ImageBase;    /* User VA of the loaded image */
} USER_THREAD_CONTEXT;

/* -----------------------------------------------------------------------
 * PsUserThreadEntry — kernel-side trampoline that transitions to ring 3
 *
 * We build a fake IRET frame on the current kernel stack and execute IRETQ,
 * which atomically restores CS (user code selector), RIP, RFLAGS, SS, and RSP.
 *
 * Register state on ring-3 entry:
 *   RCX = PEB address (following the x64 ABI: arg1 for Windows EXE entry)
 *   RSP = user stack top (16-byte aligned per x64 ABI)
 *   CS  = GDT_USER_CODE | 3
 *   SS  = GDT_USER_DATA | 3
 *   RFLAGS = 0x202 (IF enabled, reserved bit 1)
 * ----------------------------------------------------------------------- */
void PsUserThreadEntry(void *arg)
{
    USER_THREAD_CONTEXT *ctx = (USER_THREAD_CONTEXT *)arg;

    /* Update the TSS RSP0 for this thread (kernel stack for syscall returns) */
    PETHREAD et = PsGetCurrentThread();
    if (et) {
        uintptr_t kstack_top = (uintptr_t)et->Tcb.SchedulerThread.kernel_stack
                             + et->Tcb.SchedulerThread.stack_size;
        gdt_set_rsp0(kstack_top);
    }

    UINT64 entry_point = ctx->EntryPoint;
    UINT64 stack_top   = ctx->StackTop;
    kfree(ctx);  /* Free the context struct before we can't return */

    kprintf("[PS] Entering user mode: RIP=0x%llx RSP=0x%llx\n",
            (unsigned long long)entry_point,
            (unsigned long long)stack_top);

    /* Build IRETQ frame and jump to user mode:
     *   User SS   (pushed last by IRETQ logic, so first on stack)
     *   User RSP
     *   RFLAGS
     *   User CS
     *   User RIP
     */
    UINT64 user_cs = GDT_USER_CODE | 3;
    UINT64 user_ss = GDT_USER_DATA | 3;
    UINT64 rflags  = 0x202;  /* IF=1 */

    __asm__ volatile (
        /* Align the user stack to 16 bytes (subtract 8 so that after call
         * instruction pushes a return address it's 16-byte aligned) */
        "sub $8, %[stk]\n\t"

        /* Build the IRETQ frame on the current kernel stack */
        "push %[ss]\n\t"        /* SS */
        "push %[stk]\n\t"       /* RSP */
        "push %[rfl]\n\t"       /* RFLAGS */
        "push %[cs]\n\t"        /* CS */
        "push %[rip]\n\t"       /* RIP */

        /* Clear all GPRs (don't leak kernel pointers) */
        "xor %%rax, %%rax\n\t"
        "xor %%rbx, %%rbx\n\t"
        "xor %%rcx, %%rcx\n\t"
        "xor %%rdx, %%rdx\n\t"
        "xor %%rsi, %%rsi\n\t"
        "xor %%rdi, %%rdi\n\t"
        "xor %%r8,  %%r8\n\t"
        "xor %%r9,  %%r9\n\t"
        "xor %%r10, %%r10\n\t"
        "xor %%r11, %%r11\n\t"
        "xor %%r12, %%r12\n\t"
        "xor %%r13, %%r13\n\t"
        "xor %%r14, %%r14\n\t"
        "xor %%r15, %%r15\n\t"
        "xor %%rbp, %%rbp\n\t"

        "iretq\n\t"
        :
        : [ss]  "r"(user_ss),
          [stk] "r"(stack_top),
          [rfl] "r"(rflags),
          [cs]  "r"(user_cs),
          [rip] "r"(entry_point)
        : "memory"
    );

    __builtin_unreachable();
}

/* -----------------------------------------------------------------------
 * PsCreateUserProcess
 * ----------------------------------------------------------------------- */
NTSTATUS PsCreateUserProcess(
    void           *PeBuffer,
    UINT64          PeSize,
    const char     *ImageName,
    PEPROCESS      *ProcessOut,
    HANDLE         *ThreadHandle)
{
    /* Create a new process */
    PEPROCESS proc = NULL;
    NTSTATUS s = PsCreateSystemProcess(&proc, ImageName, NULL);
    if (!NT_SUCCESS(s)) return s;

    /* Load the PE image */
    LOAD_IMAGE_RESULT ldr_result;
    __builtin_memset(&ldr_result, 0, sizeof(ldr_result));

    s = LdrLoadImage(PeBuffer, PeSize, proc, &ldr_result);
    if (!NT_SUCCESS(s)) {
        kprintf("[PS] LdrLoadImage failed: 0x%x\n", (UINT32)s);
        PsTerminateProcess(proc, s);
        ObDereferenceObject(proc);
        return s;
    }

    proc->SectionObject = NULL;  /* will be set by LdrLoadImage in Phase 4 */

    kprintf("[PS] Process '%s' loaded: base=0x%llx entry=0x%llx stack=0x%llx\n",
            ImageName,
            (unsigned long long)ldr_result.ImageBase,
            (unsigned long long)ldr_result.EntryPoint,
            (unsigned long long)(ldr_result.StackBase + ldr_result.StackSize));

    /* Build the user-mode thread context */
    USER_THREAD_CONTEXT *ctx = kmalloc(sizeof(USER_THREAD_CONTEXT));
    if (!ctx) {
        PsTerminateProcess(proc, STATUS_NO_MEMORY);
        ObDereferenceObject(proc);
        return STATUS_NO_MEMORY;
    }

    ctx->EntryPoint = ldr_result.EntryPoint;
    /* Stack grows down: initial RSP = base + size (top of stack) */
    ctx->StackTop   = ldr_result.StackBase + ldr_result.StackSize;
    ctx->ImageBase  = ldr_result.ImageBase;

    /* Create the user thread (kernel-side) that calls PsUserThreadEntry */
    s = PsCreateSystemThread(
        ThreadHandle,
        THREAD_ALL_ACCESS,
        NULL,
        (HANDLE)(ULONG_PTR)(-1),
        NULL,
        (ThreadEntry)PsUserThreadEntry,
        ctx);

    if (!NT_SUCCESS(s)) {
        kfree(ctx);
        PsTerminateProcess(proc, s);
        ObDereferenceObject(proc);
        return s;
    }

    if (ProcessOut) *ProcessOut = proc;
    else ObDereferenceObject(proc);

    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * PsInitialize — called from KiSystemStartup
 * ----------------------------------------------------------------------- */
void PsInitialize(void)
{
    /* Register PROCESS and THREAD object types */
    ps_process_type_storage = (OBJECT_TYPE){
        .Name            = "Process",
        .DefaultBodySize = sizeof(EPROCESS),
        .GenericAll      = PROCESS_ALL_ACCESS,
        .Operations      = { .Delete = process_delete },
    };
    ps_thread_type_storage = (OBJECT_TYPE){
        .Name            = "Thread",
        .DefaultBodySize = sizeof(ETHREAD),
        .GenericAll      = THREAD_ALL_ACCESS,
        .Operations      = { .Delete = thread_delete },
    };

    ObpProcessType = &ps_process_type_storage;
    ObpThreadType  = &ps_thread_type_storage;
    ObCreateObjectType(ObpProcessType);
    ObCreateObjectType(ObpThreadType);

    /* Initialize the active process list head */
    PsActiveProcessHead.Flink = &PsActiveProcessHead;
    PsActiveProcessHead.Blink = &PsActiveProcessHead;

    /* Create the System process (PID 4) */
    NTSTATUS s = PsCreateSystemProcess(&PsInitialSystemProcess, "System", NULL);
    if (!NT_SUCCESS(s)) {
        kprintf("[PS] FATAL: failed to create System process: 0x%x\n", (UINT32)s);
        for (;;) {}
    }

    /* The current boot thread becomes TID 4 (System:System) */
    PETHREAD idle_et = PsGetCurrentThread();
    if (idle_et) {
        idle_et->UniqueThread         = 4;
        idle_et->Process              = PsInitialSystemProcess;
        idle_et->Cid.UniqueProcess    = 4;
        idle_et->Cid.UniqueThread     = 4;
        ps_next_tid = 8;  /* next TID after 4 */
    }

    kprintf("[PS] Process manager initialized (System PID=%lu)\n",
            PsInitialSystemProcess->UniqueProcessId);
}
