/*
 * um_d3dkmt.c — the WDDM graphics kernel's user-mode door: the D3DKMT*
 * calls (gdi32's d3dkmt.c; win32u's NtGdiDdDDI* system calls on Windows)
 *
 * Step 3 of docs/design/intel-gpu.md, its first part: adapters, devices,
 * video memory (VidMm) allocations with CPU access, and fences signalled
 * and waited on by the CPU.  Each D3DKMT_* structure arrives as Windows
 * lays it out on x64, through one NovaOS service, NtNovaDxgkCtl(op, args).
 *
 * The one adapter listed so far is NovaOS's Basic Render adapter, the
 * software render-only adapter every Windows machine has ("Microsoft Basic
 * Render Driver"): its memory is one segment of system memory, as the Iris
 * Xe's is (no VRAM), with a budget of half the machine's RAM, as Windows
 * gives shared GPU memory.  Its allocations are pages the kernel gives on
 * D3DKMTCreateAllocation2 (their size in the adapter's private driver
 * data, NOVA_KMT_ALLOCATION in userland/include/d3dkmthk.h) and maps into
 * the process for D3DKMTLock2; they stay resident until destroyed.  Fences
 * (D3DDDI_FENCE, D3DDDI_MONITORED_FENCE) hold a 64-bit value that only
 * rises; a monitored fence's value is also on a read-only page mapped into
 * the process (FenceValueCPUVirtualAddress), as Windows maps it.  A wait
 * returns when every fence (or, with WaitAny, one) reached its value, or,
 * given an event, sets the event then and returns at once.
 *
 * Not yet (the next parts of step 3, with wddmtest.sys, a render miniport
 * whose "GPU" runs in software): render miniports' adapters and their DDIs
 * (DxgkDdiCreateAllocation, BuildPagingBuffer, SubmitCommandVirtual), GPU
 * virtual addresses and residency, contexts and submission, the scheduler,
 * fences the GPU signals, preemption and TDR, shared resources and 32-bit
 * programs' structures.
 *
 * Handles (D3DKMT_HANDLE) are the process's own, as Windows' are; a
 * process's objects go with it (um_d3dkmt_process_gone).  A view the
 * process still has keeps its pages until it is unmapped.
 */
#include "um_internal.h"
#include "../ke/printf.h"
#include "../ke/probe.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../mm/pmm.h"

#define ST_OK               0x00000000u
#define ST_ACCESS_VIOLATION 0xC0000005u
#define ST_INVALID_PARAM    0xC000000Du
#define ST_NO_MEMORY        0xC0000017u
#define ST_BUFFER_TOO_SMALL 0xC0000023u
#define ST_NOT_SUPPORTED    0xC00000BBu
#define ST_TERMINATING      0xC000004Bu

#define MAX_OBJECTS   4096            /* D3DKMT handles one process may hold */
#define MAX_LIST      64              /* allocations or fences one call may name */
#define MAX_ALLOC     (1ull << 30)    /* the largest allocation */
#define HANDLE_BASE   0x40000000u     /* (handle = base | index << 6, as Windows' look) */

/* The Basic Render adapter */
#define BASIC_LUID_LO 0x00001000u

/* -----------------------------------------------------------------------
 * The structures, as Windows lays them out on x64 (d3dkmthk.h, d3dukmdt.h)
 * ----------------------------------------------------------------------- */
typedef struct { UINT32 lo; INT32 hi; } Luid;
typedef struct { UINT32 h; Luid luid; UINT32 sources, precise; } AdapterInfo;
typedef struct { UINT32 n, pad; UINT64 adapters; } EnumAdapters2;
typedef struct { Luid luid; UINT32 h; } OpenFromLuid;
typedef struct { UINT32 h; } HandleOnly;
typedef struct { UINT32 h, type; UINT64 data; UINT32 size, pad; } QueryAdapterInfo;
typedef struct { UINT64 adapter; UINT32 flags, device; UINT64 cmd; UINT32 cmd_size, pad1; UINT64 alist;
                 UINT32 alist_size, pad2; UINT64 plist; UINT32 plist_size, pad3; } CreateDevice;
typedef struct { UINT32 alloc, pad; UINT64 section, priv; UINT32 priv_size, source, flags, pad2; UINT64 gpu_va,
                 priority, reserved[5]; } AllocInfo2;
typedef struct { UINT32 device, resource, global_share, pad; UINT64 rt_data; UINT32 rt_size, pad2; UINT64 priv;
                 UINT32 priv_size, num; UINT64 info; UINT32 flags, pad3; UINT64 rt_handle; } CreateAllocation;
typedef struct { UINT64 size; UINT32 flags, reserved; } NovaAllocation;      /* NOVA_KMT_ALLOCATION */
typedef struct { UINT32 device, resource; UINT64 list; UINT32 count, flags; } DestroyAllocation2;
typedef struct { UINT32 device, alloc, flags, pad; UINT64 data; } Lock2;
typedef struct { UINT32 device, alloc; } Unlock2;
typedef struct {
    UINT32 device, pad;
    UINT32 type, flags;
    union {
        UINT64 fence_value;
        struct { UINT64 initial, cpu_va, gpu_va; UINT32 affinity, pad; } mf;
        UINT64 reserved[8];
    };
    UINT32 shared, pad2;
    UINT32 sync, pad3;
} CreateSync2;
typedef struct { UINT32 device, count; UINT64 handles, values; UINT32 flags, pad; } SignalFromCpu;
typedef struct { UINT32 device, count; UINT64 handles, values, event; UINT32 flags, pad; } WaitFromCpu;
typedef struct { UINT64 process; UINT32 adapter, group; UINT64 budget, usage, reservation, available;
                 UINT32 index, pad; } VideoMemoryInfo;

#define OFF(t, m) __builtin_offsetof(t, m)
_Static_assert(sizeof(AdapterInfo) == 20 && sizeof(EnumAdapters2) == 16 && sizeof(OpenFromLuid) == 12 &&
               sizeof(QueryAdapterInfo) == 24, "adapter structures");
_Static_assert(sizeof(CreateDevice) == 64 && OFF(CreateDevice, device) == 12, "D3DKMT_CREATEDEVICE");
_Static_assert(sizeof(AllocInfo2) == 96 && OFF(AllocInfo2, gpu_va) == 40, "D3DDDI_ALLOCATIONINFO2");
_Static_assert(sizeof(CreateAllocation) == 72 && OFF(CreateAllocation, info) == 48 && OFF(CreateAllocation, flags) == 56,
               "D3DKMT_CREATEALLOCATION");
_Static_assert(sizeof(DestroyAllocation2) == 24 && sizeof(Lock2) == 24, "D3DKMT_DESTROYALLOCATION2, D3DKMT_LOCK2");
_Static_assert(sizeof(CreateSync2) == 96 && OFF(CreateSync2, mf.cpu_va) == 24 && OFF(CreateSync2, shared) == 80 &&
               OFF(CreateSync2, sync) == 88, "D3DKMT_CREATESYNCHRONIZATIONOBJECT2");
_Static_assert(sizeof(SignalFromCpu) == 32 && sizeof(WaitFromCpu) == 40 && sizeof(VideoMemoryInfo) == 56,
               "fence and memory structures");

enum {
    OP_ENUM_ADAPTERS2, OP_OPEN_ADAPTER_FROM_LUID, OP_CLOSE_ADAPTER, OP_QUERY_ADAPTER_INFO,
    OP_CREATE_DEVICE, OP_DESTROY_DEVICE, OP_CREATE_ALLOCATION2, OP_DESTROY_ALLOCATION2,
    OP_LOCK2, OP_UNLOCK2, OP_CREATE_SYNC2, OP_DESTROY_SYNC, OP_SIGNAL_FROM_CPU, OP_WAIT_FROM_CPU,
    OP_QUERY_VIDEO_MEMORY_INFO,
};

#define QAI_GETSEGMENTSIZE 3
#define QAI_DRIVERVERSION  13
#define QAI_ADAPTERTYPE    15
#define ADAPTERTYPE_RENDER 0x1
#define ADAPTERTYPE_SOFTWARE 0x4
#define DRIVERVERSION_WDDM_2_0 2000

#define SYNC_FENCE           3
#define SYNC_MONITORED_FENCE 5

/* D3DKMT_CREATEALLOCATIONFLAGS this adapter cannot do yet: CreateResource,
 * CreateShared, ExistingSysMem, StandardAllocation, ExistingSection */
#define ALLOC_FLAGS_UNSUPPORTED (0x1u | 0x2u | 0x20u | 0x10000u | 0x20000u)

/* -----------------------------------------------------------------------
 * Objects
 * ----------------------------------------------------------------------- */
/* An allocation's pages, shared by its handle and its CPU view */
typedef struct {
    volatile int refs;
    PADDR  *f;
    UINT64  n, size;
} Memory;

/* A fence: its value, and a monitored fence's page holding it */
typedef struct {
    volatile int refs;
    int      type;
    volatile UINT64 value;
    PADDR    page;                    /* (monitored: 0 otherwise) */
} Fence;

typedef enum { K_FREE, K_ADAPTER, K_DEVICE, K_ALLOC, K_SYNC } Kind;

typedef struct {
    Kind     kind;
    UINT32   device;                  /* an allocation's or fence's device (its handle) */
    Memory  *mem;
    Fence   *fence;
    UINT64   va;                      /* an allocation's view while locked; a monitored fence's value page */
    int      locks;
} Object;

typedef struct Proc {
    struct Proc *next;
    UmProcess   *p;
    Object      *obj;
    int          nobj;
    UINT64       usage;               /* bytes of allocations (all resident) */
} Proc;

/* A wait that sets an event when its fences are reached */
typedef struct Waiter {
    struct Waiter *next;
    UmProcess     *p;
    UmObject      *event;
    bool           any;
    int            n;
    Fence         *fence[MAX_LIST];
    UINT64         value[MAX_LIST];
} Waiter;

static UmLock  g_lock;                /* everything below (never held across user memory) */
static Proc   *g_procs;
static Waiter *g_waiters;
static UINT64  g_budget;

static void mem_unref(void *m_)
{
    Memory *m = m_;
    if (__atomic_sub_fetch(&m->refs, 1, __ATOMIC_ACQ_REL)) return;
    um_free_frames(m->f, m->n);
    kfree(m);
}

static void fence_unref(void *f_)
{
    Fence *f = f_;
    if (__atomic_sub_fetch(&f->refs, 1, __ATOMIC_ACQ_REL)) return;
    if (f->page) um_free_frames(&f->page, 1);
    kfree(f);
}

static Proc *proc_of(UmProcess *p, bool make)
{
    for (Proc *q = g_procs; q; q = q->next)
        if (q->p == p) return q;
    if (!make) return NULL;
    Proc *q = kzalloc(sizeof(*q));
    if (!q) return NULL;
    q->p = p;
    q->next = g_procs;
    g_procs = q;
    return q;
}

static UINT32 handle_new(Proc *q, Kind kind)
{
    int i = 0;
    while (i < q->nobj && q->obj[i].kind != K_FREE) i++;
    if (i == q->nobj) {
        int n = q->nobj ? q->nobj * 2 : 64;
        if (n > MAX_OBJECTS) return 0;
        Object *t = kzalloc(sizeof(*t) * (size_t)n);
        if (!t) return 0;
        if (q->nobj) memcpy(t, q->obj, sizeof(*t) * (size_t)q->nobj);
        kfree(q->obj);
        q->obj = t;
        q->nobj = n;
    }
    memset(&q->obj[i], 0, sizeof(q->obj[i]));
    q->obj[i].kind = kind;
    return HANDLE_BASE | ((UINT32)(i + 1) << 6);
}

static Object *object(Proc *q, UINT32 h, Kind kind)
{
    if (!q || (h & ~0x3FFFFFC0u) != HANDLE_BASE) return NULL;
    UINT32 i = ((h & 0x3FFFFFC0u) >> 6) - 1;
    return i < (UINT32)q->nobj && q->obj[i].kind == kind ? &q->obj[i] : NULL;
}

static void fence_write(Fence *f, UINT64 v)
{
    f->value = v;
    if (f->page) *(volatile UINT64 *)um_frame_ptr(f->page) = v;
}

static bool reached(Fence *const *f, const UINT64 *v, int n, bool any)
{
    int done = 0;
    for (int i = 0; i < n; i++) done += f[i]->value >= v[i];
    return any ? done > 0 || !n : done == n;
}

static void set_event(UmObject *o)
{
    IrqState s = ob_lock();
    o->signaled = 1;
    um_ob_wake(o);
    ob_unlock(s);
}

static void waiter_free(Waiter *w)
{
    for (int i = 0; i < w->n; i++) fence_unref(w->fence[i]);
    um_ob_unref(w->event);
    kfree(w);
}

/* Fences rose: set the events of the waits they finished (under g_lock) */
static void waiters_check(void)
{
    for (Waiter **pp = &g_waiters; *pp;) {
        Waiter *w = *pp;
        if (!reached(w->fence, w->value, w->n, w->any)) { pp = &w->next; continue; }
        *pp = w->next;
        set_event(w->event);
        waiter_free(w);
    }
}

/* Drop object @o; @p: its process, still mapped (NULL when the process is going) */
static void object_free(Proc *q, Object *o, UmProcess *p)
{
    if (o->kind == K_ALLOC) {
        if (o->va && p) um_unmap_view_at(p, o->va, o->mem);
        q->usage -= o->mem->size;
        mem_unref(o->mem);
    } else if (o->kind == K_SYNC) {
        if (o->va && p) um_unmap_view_at(p, o->va, o->fence);
        fence_unref(o->fence);
    }
    memset(o, 0, sizeof(*o));
}

/* -----------------------------------------------------------------------
 * The calls (each with its structure copied in; g_lock held)
 * ----------------------------------------------------------------------- */
static UINT32 enum_adapters2(Proc *q, EnumAdapters2 *a, AdapterInfo *out)
{
    if (!a->adapters) { a->n = 1; return ST_OK; }
    if (a->n < 1) { a->n = 1; return ST_BUFFER_TOO_SMALL; }
    UINT32 h = handle_new(q, K_ADAPTER);
    if (!h) return ST_NO_MEMORY;
    *out = (AdapterInfo){ h, { BASIC_LUID_LO, 0 }, 0, 0 };
    a->n = 1;
    return ST_OK;
}

static UINT32 open_from_luid(Proc *q, OpenFromLuid *a)
{
    if (a->luid.lo != BASIC_LUID_LO || a->luid.hi != 0) return ST_INVALID_PARAM;
    return (a->h = handle_new(q, K_ADAPTER)) ? ST_OK : ST_NO_MEMORY;
}

static UINT32 query_adapter_info(Proc *q, const QueryAdapterInfo *a, UINT64 *out, UINT32 *len)
{
    if (!object(q, a->h, K_ADAPTER)) return ST_INVALID_PARAM;
    switch (a->type) {
    case QAI_ADAPTERTYPE: out[0] = ADAPTERTYPE_RENDER | ADAPTERTYPE_SOFTWARE; *len = 4; break;
    case QAI_DRIVERVERSION: out[0] = DRIVERVERSION_WDDM_2_0; *len = 4; break;
    case QAI_GETSEGMENTSIZE: out[0] = 0; out[1] = 0; out[2] = g_budget; *len = 24; break;
    default: return ST_NOT_SUPPORTED;
    }
    return a->size == *len ? ST_OK : ST_INVALID_PARAM;
}

static UINT32 create_device(Proc *q, CreateDevice *a)
{
    if (!object(q, (UINT32)a->adapter, K_ADAPTER)) return ST_INVALID_PARAM;
    return (a->device = handle_new(q, K_DEVICE)) ? ST_OK : ST_NO_MEMORY;
}

/* A device goes with its allocations and fences */
static UINT32 destroy_device(Proc *q, UINT32 dev, UmProcess *p)
{
    Object *d = object(q, dev, K_DEVICE);
    if (!d) return ST_INVALID_PARAM;
    for (int i = 0; i < q->nobj; i++)
        if ((q->obj[i].kind == K_ALLOC || q->obj[i].kind == K_SYNC) && q->obj[i].device == dev)
            object_free(q, &q->obj[i], p);
    memset(d, 0, sizeof(*d));
    return ST_OK;
}

/* Each allocation of @a (@info: its D3DDDI_ALLOCATIONINFO2s, filled in) */
static UINT32 create_allocation2(Proc *q, const CreateAllocation *a, AllocInfo2 *info, const NovaAllocation *spec)
{
    if (!object(q, a->device, K_DEVICE)) return ST_INVALID_PARAM;
    if (a->flags & ALLOC_FLAGS_UNSUPPORTED) return ST_NOT_SUPPORTED;
    UINT32 made[MAX_LIST];
    UINT32 i;
    UINT32 st = ST_OK;
    for (i = 0; i < a->num && st == ST_OK; i++) {
        UINT64 n = (spec[i].size + PAGE_SIZE - 1) / PAGE_SIZE;
        if (!spec[i].size || spec[i].size > MAX_ALLOC || q->usage + n * PAGE_SIZE > g_budget) { st = ST_INVALID_PARAM; break; }
        Memory *m = kzalloc(sizeof(*m));
        if (m && !(m->f = um_alloc_frames(n))) { kfree(m); m = NULL; }
        UINT32 h = m ? handle_new(q, K_ALLOC) : 0;
        if (!h) {
            if (m) { um_free_frames(m->f, n); kfree(m); }
            st = ST_NO_MEMORY;
            break;
        }
        m->refs = 1;
        m->n = n;
        m->size = n * PAGE_SIZE;
        Object *o = object(q, h, K_ALLOC);
        o->device = a->device;
        o->mem = m;
        q->usage += m->size;
        info[i].alloc = made[i] = h;
        info[i].gpu_va = 0;                     /* (no GPU virtual addresses on this adapter yet) */
    }
    if (st != ST_OK)                            /* all or nothing */
        while (i--) object_free(q, object(q, made[i], K_ALLOC), q->p);
    return st;
}

static UINT32 destroy_allocation2(Proc *q, const DestroyAllocation2 *a, const UINT32 *list, UmProcess *p)
{
    if (!object(q, a->device, K_DEVICE) || a->resource) return ST_INVALID_PARAM;
    for (UINT32 i = 0; i < a->count; i++) {
        Object *o = object(q, list[i], K_ALLOC);
        if (!o || o->device != a->device) return ST_INVALID_PARAM;
    }
    for (UINT32 i = 0; i < a->count; i++) object_free(q, object(q, list[i], K_ALLOC), p);
    return ST_OK;
}

static UINT32 lock2(Proc *q, Lock2 *a, UmProcess *p)
{
    Object *o = object(q, a->alloc, K_ALLOC);
    if (!o || o->device != a->device) return ST_INVALID_PARAM;
    if (!o->locks) {
        __atomic_add_fetch(&o->mem->refs, 1, __ATOMIC_ACQ_REL);     /* (the view's) */
        o->va = um_map_frames_view(p, o->mem->f, o->mem->n, 0x04 /* PAGE_READWRITE */, mem_unref, o->mem);
        if (!o->va) return ST_NO_MEMORY;                            /* (the view's reference went with it) */
    }
    o->locks++;
    a->data = o->va;
    return ST_OK;
}

static UINT32 unlock2(Proc *q, const Unlock2 *a, UmProcess *p)
{
    Object *o = object(q, a->alloc, K_ALLOC);
    if (!o || o->device != a->device || !o->locks) return ST_INVALID_PARAM;
    if (!--o->locks) {
        um_unmap_view_at(p, o->va, o->mem);
        o->va = 0;
    }
    return ST_OK;
}

static UINT32 create_sync2(Proc *q, CreateSync2 *a, UmProcess *p)
{
    if (!object(q, a->device, K_DEVICE)) return ST_INVALID_PARAM;
    if (a->type != SYNC_FENCE && a->type != SYNC_MONITORED_FENCE) return ST_NOT_SUPPORTED;
    if (a->flags & 1) return ST_NOT_SUPPORTED;                     /* Shared: not yet */
    Fence *f = kzalloc(sizeof(*f));
    if (!f) return ST_NO_MEMORY;
    f->refs = 1;
    f->type = (int)a->type;
    UINT64 va = 0;
    if (a->type == SYNC_MONITORED_FENCE) {
        PADDR *pg = um_alloc_frames(1);
        if (!pg) { kfree(f); return ST_NO_MEMORY; }
        f->page = pg[0];
        kfree(pg);
        fence_write(f, a->mf.initial);
        f->refs++;                                                  /* (the view's) */
        va = um_map_frames_view(p, &f->page, 1, 0x02 /* PAGE_READONLY */, fence_unref, f);
        if (!va) { fence_unref(f); return ST_NO_MEMORY; }
    } else
        fence_write(f, a->fence_value);
    UINT32 h = handle_new(q, K_SYNC);
    if (!h) {
        if (va) um_unmap_view_at(p, va, f);
        fence_unref(f);
        return ST_NO_MEMORY;
    }
    Object *o = object(q, h, K_SYNC);
    o->device = a->device;
    o->fence = f;
    o->va = va;
    if (va) { a->mf.cpu_va = va; a->mf.gpu_va = 0; }
    a->sync = h;
    return ST_OK;
}

/* The fences of @list on device @dev, referenced into @f; false if one is not */
static bool fences(Proc *q, UINT32 dev, const UINT32 *list, UINT32 n, Fence **f)
{
    if (!object(q, dev, K_DEVICE)) return false;
    for (UINT32 i = 0; i < n; i++) {
        Object *o = object(q, list[i], K_SYNC);
        if (!o || o->device != dev) return false;
        f[i] = o->fence;
    }
    for (UINT32 i = 0; i < n; i++) __atomic_add_fetch(&f[i]->refs, 1, __ATOMIC_ACQ_REL);
    return true;
}

/* -----------------------------------------------------------------------
 * NtNovaDxgkCtl(op, args): NTSTATUS
 * ----------------------------------------------------------------------- */
static bool in(void *k, UINT64 u, UINT64 n) { return u && NT_SUCCESS(CopyFromUser(k, (const void *)(uintptr_t)u, n)); }
static bool out(UINT64 u, const void *k, UINT64 n) { return NT_SUCCESS(CopyToUser((void *)(uintptr_t)u, k, n)); }

/* (calls on one structure: copied in, done under the lock, copied back) */
#define SIMPLE(T, call)                                                         \
    do {                                                                        \
        T a_;                                                                   \
        if (!in(&a_, args, sizeof(a_))) return ST_ACCESS_VIOLATION;             \
        um_lock(&g_lock);                                                       \
        Proc *q_ = proc_of(p, true);                                            \
        UINT32 st_ = q_ ? call : ST_NO_MEMORY;                                  \
        um_unlock(&g_lock);                                                     \
        if (st_ == ST_OK && !out(args, &a_, sizeof(a_))) return ST_ACCESS_VIOLATION; \
        return st_;                                                             \
    } while (0)

static UINT64 wait_from_cpu(UmProcess *p, UINT64 args)
{
    WaitFromCpu a;
    UINT32 list[MAX_LIST];
    UINT64 vals[MAX_LIST];
    if (!in(&a, args, sizeof(a))) return ST_ACCESS_VIOLATION;
    if (!a.count || a.count > MAX_LIST || !in(list, a.handles, a.count * 4ull) || !in(vals, a.values, a.count * 8ull))
        return ST_INVALID_PARAM;
    Waiter *w = kzalloc(sizeof(*w));
    if (!w) return ST_NO_MEMORY;
    w->p = p;
    w->n = (int)a.count;
    w->any = a.flags & 1;
    memcpy(w->value, vals, sizeof(vals[0]) * a.count);
    if (a.event && !(w->event = um_handle_object(p, a.event, UO_EVENT))) { kfree(w); return ST_INVALID_PARAM; }
    um_lock(&g_lock);
    bool ok = fences(proc_of(p, false), a.device, list, a.count, w->fence);
    if (ok && w->event) {                       /* set the event when reached; return now */
        if (reached(w->fence, w->value, w->n, w->any)) { set_event(w->event); waiter_free(w); }
        else { w->next = g_waiters; g_waiters = w; }
        w = NULL;
    }
    um_unlock(&g_lock);
    if (!ok) { w->n = 0; if (w->event) um_ob_unref(w->event); kfree(w); return ST_INVALID_PARAM; }
    if (!w) return ST_OK;
    /* Here: the fences only the CPU signals so far, so a short nap between looks */
    UINT32 st = ST_OK;
    for (int iter = 0; !reached(w->fence, w->value, w->n, w->any); iter++) {
        if (um_stopping()) { st = ST_TERMINATING; break; }
        if (iter < 50) sched_yield();
        else {
            static UINT32 never;
            sched_sleep_until_tsc(&never, sched_tsc_after(iter < 500 ? 1000 : 10000));   /* 0.1 ms, then 1 ms */
        }
    }
    waiter_free(w);
    return st;
}

static UINT64 sys_dxgk(UINT64 op, UINT64 args, UINT64 a3, UINT64 a4)
{
    (void)a3; (void)a4;
    UmProcess *p = UmCurrent();
    if (!p) return ST_INVALID_PARAM;
    switch (op) {
    case OP_ENUM_ADAPTERS2: {
        EnumAdapters2 a;
        AdapterInfo info;
        if (!in(&a, args, sizeof(a))) return ST_ACCESS_VIOLATION;
        um_lock(&g_lock);
        Proc *q = proc_of(p, true);
        UINT32 st = q ? enum_adapters2(q, &a, &info) : ST_NO_MEMORY;
        um_unlock(&g_lock);
        if ((st == ST_OK && a.adapters && !out(a.adapters, &info, sizeof(info))) || !out(args, &a, sizeof(a)))
            return ST_ACCESS_VIOLATION;
        return st;
    }
    case OP_OPEN_ADAPTER_FROM_LUID: SIMPLE(OpenFromLuid, open_from_luid(q_, &a_));
    case OP_CLOSE_ADAPTER: {
        HandleOnly a;
        if (!in(&a, args, sizeof(a))) return ST_ACCESS_VIOLATION;
        um_lock(&g_lock);
        Object *o = object(proc_of(p, false), a.h, K_ADAPTER);
        if (o) o->kind = K_FREE;
        um_unlock(&g_lock);
        return o ? ST_OK : ST_INVALID_PARAM;
    }
    case OP_QUERY_ADAPTER_INFO: {
        QueryAdapterInfo a;
        UINT64 res[3];
        UINT32 len = 0;
        if (!in(&a, args, sizeof(a))) return ST_ACCESS_VIOLATION;
        um_lock(&g_lock);
        UINT32 st = query_adapter_info(proc_of(p, false), &a, res, &len);
        um_unlock(&g_lock);
        if (st == ST_OK && !out(a.data, res, len)) return ST_ACCESS_VIOLATION;
        return st;
    }
    case OP_CREATE_DEVICE: SIMPLE(CreateDevice, create_device(q_, &a_));
    case OP_DESTROY_DEVICE: {
        HandleOnly a;
        if (!in(&a, args, sizeof(a))) return ST_ACCESS_VIOLATION;
        um_lock(&g_lock);
        Proc *q = proc_of(p, false);
        UINT32 st = q ? destroy_device(q, a.h, p) : ST_INVALID_PARAM;
        waiters_check();
        um_unlock(&g_lock);
        return st;
    }
    case OP_CREATE_ALLOCATION2: {
        CreateAllocation a;
        AllocInfo2 info[MAX_LIST];
        NovaAllocation spec[MAX_LIST];
        if (!in(&a, args, sizeof(a))) return ST_ACCESS_VIOLATION;
        if (!a.num || a.num > MAX_LIST || !in(info, a.info, sizeof(info[0]) * a.num)) return ST_INVALID_PARAM;
        for (UINT32 i = 0; i < a.num; i++)
            if (info[i].priv_size != sizeof(spec[i]) || !in(&spec[i], info[i].priv, sizeof(spec[i])))
                return ST_INVALID_PARAM;
        um_lock(&g_lock);
        Proc *q = proc_of(p, true);
        UINT32 st = q ? create_allocation2(q, &a, info, spec) : ST_NO_MEMORY;
        um_unlock(&g_lock);
        if (st == ST_OK && !out(a.info, info, sizeof(info[0]) * a.num)) return ST_ACCESS_VIOLATION;
        return st;
    }
    case OP_DESTROY_ALLOCATION2: {
        DestroyAllocation2 a;
        UINT32 list[MAX_LIST];
        if (!in(&a, args, sizeof(a))) return ST_ACCESS_VIOLATION;
        if (a.count > MAX_LIST || (a.count && !in(list, a.list, a.count * 4ull))) return ST_INVALID_PARAM;
        um_lock(&g_lock);
        Proc *q = proc_of(p, false);
        UINT32 st = q ? destroy_allocation2(q, &a, list, p) : ST_INVALID_PARAM;
        um_unlock(&g_lock);
        return st;
    }
    case OP_LOCK2: SIMPLE(Lock2, lock2(q_, &a_, p));
    case OP_UNLOCK2: SIMPLE(Unlock2, unlock2(q_, &a_, p));
    case OP_CREATE_SYNC2: SIMPLE(CreateSync2, create_sync2(q_, &a_, p));
    case OP_DESTROY_SYNC: {
        HandleOnly a;
        if (!in(&a, args, sizeof(a))) return ST_ACCESS_VIOLATION;
        um_lock(&g_lock);
        Proc *q = proc_of(p, false);
        Object *o = object(q, a.h, K_SYNC);
        if (o) object_free(q, o, p);
        um_unlock(&g_lock);
        return o ? ST_OK : ST_INVALID_PARAM;
    }
    case OP_SIGNAL_FROM_CPU: {
        SignalFromCpu a;
        UINT32 list[MAX_LIST];
        UINT64 vals[MAX_LIST];
        Fence *f[MAX_LIST];
        if (!in(&a, args, sizeof(a))) return ST_ACCESS_VIOLATION;
        if (!a.count || a.count > MAX_LIST || !in(list, a.handles, a.count * 4ull) ||
            !in(vals, a.values, a.count * 8ull))
            return ST_INVALID_PARAM;
        um_lock(&g_lock);
        bool ok = fences(proc_of(p, false), a.device, list, a.count, f);
        if (ok) {
            for (UINT32 i = 0; i < a.count; i++) {
                if (vals[i] > f[i]->value) fence_write(f[i], vals[i]);   /* (a fence never goes back) */
                fence_unref(f[i]);
            }
            waiters_check();
        }
        um_unlock(&g_lock);
        return ok ? ST_OK : ST_INVALID_PARAM;
    }
    case OP_WAIT_FROM_CPU: return wait_from_cpu(p, args);
    case OP_QUERY_VIDEO_MEMORY_INFO: {
        VideoMemoryInfo a;
        if (!in(&a, args, sizeof(a))) return ST_ACCESS_VIOLATION;
        if (a.process && a.process != (UINT64)-1) return ST_NOT_SUPPORTED;   /* (another process's: not yet) */
        um_lock(&g_lock);
        Proc *q = proc_of(p, false);
        bool ok = object(q, a.adapter, K_ADAPTER) && a.group <= 1 && !a.index;
        if (ok) {
            bool local = a.group == 0;          /* (an integrated GPU's "local" memory is its shared system memory) */
            a.budget = local ? g_budget : 0;
            a.usage = local ? q->usage : 0;
            a.reservation = 0;
            a.available = local ? g_budget : 0;
        }
        um_unlock(&g_lock);
        if (ok && !out(args, &a, sizeof(a))) return ST_ACCESS_VIOLATION;
        return ok ? ST_OK : ST_INVALID_PARAM;
    }
    }
    return ST_NOT_SUPPORTED;
}

void um_d3dkmt_process_gone(UmProcess *p)
{
    um_lock(&g_lock);
    for (Waiter **pp = &g_waiters; *pp;) {
        Waiter *w = *pp;
        if (w->p != p) { pp = &w->next; continue; }
        *pp = w->next;
        waiter_free(w);
    }
    for (Proc **pp = &g_procs; *pp; pp = &(*pp)->next) {
        Proc *q = *pp;
        if (q->p != p) continue;
        *pp = q->next;
        for (int i = 0; i < q->nobj; i++) object_free(q, &q->obj[i], NULL);   /* (the views go with the process) */
        kfree(q->obj);
        kfree(q);
        break;
    }
    um_unlock(&g_lock);
}

void um_d3dkmt_syscalls_init(void)
{
    g_budget = (UINT64)pmm_ram_pages() * PAGE_SIZE / 2;
    kprintf("[DXGK] Basic Render adapter (software, render only): LUID 0x%x, %llu MB of shared memory\n",
            BASIC_LUID_LO, (unsigned long long)(g_budget >> 20));
    um_install(SYSCALL_NtNovaDxgkCtl, sys_dxgk);
}
