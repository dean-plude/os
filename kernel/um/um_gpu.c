/*
 * um_gpu.c — the kernel half of the Vulkan and OpenGL drivers for virtio GPUs
 *
 * Mesa's Venus (vulkan_virtio.dll) encodes a program's Vulkan calls, and
 * Mesa's virgl (opengl32_virgl.dll) its OpenGL calls, and sends them to
 * the host's renderer through the GPU; this is what Linux's
 * virtio-gpu DRM driver gives it there (DRM_IOCTL_VIRTGPU_*), as one
 * NovaOS-private service on a UO_GPU handle: one 3D context per handle,
 * its blobs (numbered from 1 within the handle) and its timelines.  A
 * mappable blob is mapped by making it a section (um_section_foreign) that
 * the program maps with NtMapViewOfSection, so its views go away with the
 * program like any other.  virgl's resources are the classic kind: the
 * kernel gives each one pages of guest memory (mapped the same way, as a
 * section over those pages) and copies between them and the host's copy
 * with fenced transfers.  Every structure keeps pointers as 64-bit fields,
 * so 32-bit programs pass the same ones.
 */

#include "um_internal.h"
#include "../ke/probe.h"
#include "../ke/spinlock.h"
#include "../mm/vmm.h"
#include "../lib/string.h"
#include "../drivers/virtio_gpu.h"

#define MAX_STREAM  (8u << 20)        /* largest command stream a program may send */
#define MAX_WAIT    256               /* timelines one wait may name */

typedef struct {
    VgpuCtx   *ctx;
    KSpinLock  lock;                  /* the blob table */
    VgpuBlob **blob;                  /* blob N at blob[N - 1] */
    int        nblob;
} Gpu;

static void gpu_destroy(UmObject *o)
{
    Gpu *g = o->ptr;
    if (!g) return;
    for (int i = 0; i < g->nblob; i++)
        if (g->blob[i]) VgpuBlobUnref(g->blob[i]);
    kfree(g->blob);
    VgpuCtxDestroy(g->ctx);
    kfree(g);
    o->ptr = NULL;
}

static Gpu *handle_gpu(UINT64 h, UmObject **ob)
{
    UmObject *o = um_handle_object(UmCurrent(), h, UO_GPU);
    if (!o) return NULL;
    *ob = o;
    return o->ptr;
}

static UINT64 add_blob(Gpu *g, VgpuBlob *b)
{
    IrqState s = spin_lock_irqsave(&g->lock);
    int k = 0;
    while (k < g->nblob && g->blob[k]) k++;
    if (k == g->nblob) {
        int n = g->nblob ? g->nblob * 2 : 256;
        VgpuBlob **t = kzalloc(sizeof(*t) * (size_t)n);
        if (!t) { spin_unlock_irqrestore(&g->lock, s); return 0; }
        if (g->nblob) memcpy(t, g->blob, sizeof(*t) * (size_t)g->nblob);
        kfree(g->blob);
        g->blob = t;
        g->nblob = n;
    }
    g->blob[k] = b;
    spin_unlock_irqrestore(&g->lock, s);
    return (UINT64)k + 1;
}

/* Blob @n of @g with a reference of its own (or NULL); @take: removed from the table */
static VgpuBlob *get_blob(Gpu *g, UINT64 n, bool take)
{
    IrqState s = spin_lock_irqsave(&g->lock);
    VgpuBlob *b = n && n <= (UINT64)g->nblob ? g->blob[n - 1] : NULL;
    if (b && take) g->blob[n - 1] = NULL;
    else if (b) VgpuBlobRef(b);
    spin_unlock_irqrestore(&g->lock, s);
    return b;
}

static void *copy_in(UINT64 ptr, UINT64 size)
{
    void *k = kmalloc(size ? size : 1);
    if (k && size && !NT_SUCCESS(CopyFromUser(k, (const void *)(uintptr_t)ptr, size))) { kfree(k); return NULL; }
    return k;
}

#define FAIL ((UINT64)(INT64)-1)

typedef struct { UINT32 present, pad; UINT64 host_visible; } GpuInfo;
typedef struct { UINT32 id, version, size, pad; UINT64 out; } GpuCapset;
typedef struct { UINT64 cs; UINT32 size, ring; UINT32 nsync, pad; UINT64 syncs, values; } GpuSubmit;
typedef struct { UINT32 blob_mem, flags; UINT64 blob_id, size, cs; UINT32 cs_size, res; } GpuBlobCreate;
typedef struct { UINT32 id, op; UINT64 value; } GpuSyncOp;
typedef struct { UINT32 p[10]; UINT32 res; UINT32 pad; UINT64 size; } GpuRes3d;
typedef struct { UINT64 blob, offset; UINT32 to_host, level, stride, layer_stride; UINT32 box[6]; UINT32 sync, pad; UINT64 value; } GpuTransfer;
typedef struct { UINT32 n, any; UINT64 timeout_ns, ids, values; } GpuWaitArgs;

static UINT64 do_submit(Gpu *g, UINT64 ptr)
{
    GpuSubmit a;
    if (!NT_SUCCESS(CopyFromUser(&a, (const void *)(uintptr_t)ptr, sizeof(a)))) return FAIL;
    if (a.size > MAX_STREAM || a.nsync > MAX_WAIT) return FAIL;
    void *cs = copy_in(a.cs, a.size);
    UINT32 *ids = cs ? copy_in(a.syncs, (UINT64)a.nsync * 4) : NULL;
    UINT64 *vals = ids ? copy_in(a.values, (UINT64)a.nsync * 8) : NULL;
    bool ok = vals && VgpuCtxSubmit(g->ctx, cs, a.size, a.ring, (int)a.nsync, ids, vals);
    kfree(cs); kfree(ids); kfree(vals);
    return ok ? 0 : FAIL;
}

static UINT64 do_blob_create(Gpu *g, UINT64 ptr)
{
    GpuBlobCreate a;
    if (!NT_SUCCESS(CopyFromUser(&a, (const void *)(uintptr_t)ptr, sizeof(a)))) return FAIL;
    if (a.cs_size > MAX_STREAM) return FAIL;
    void *cs = copy_in(a.cs, a.cs_size);
    if (!cs) return FAIL;
    VgpuBlob *b = VgpuBlobCreate(g->ctx, a.blob_mem, a.flags, a.blob_id, a.size, cs, a.cs_size);
    kfree(cs);
    if (!b) return 0;
    UINT64 n = add_blob(g, b);
    if (!n) { VgpuBlobUnref(b); return 0; }
    a.res = VgpuBlobId(b);
    CopyToUser((void *)(uintptr_t)ptr, &a, sizeof(a));
    return n;
}

static UINT64 do_res_create(Gpu *g, UINT64 ptr)
{
    GpuRes3d a;
    if (!NT_SUCCESS(CopyFromUser(&a, (const void *)(uintptr_t)ptr, sizeof(a)))) return FAIL;
    UINT64 n = (a.size + PAGE_SIZE - 1) / PAGE_SIZE;
    PADDR *f = NULL;
    if (n && !(f = um_alloc_frames(n))) return 0;
    VgpuBlob *b = VgpuRes3dCreate(g->ctx, a.p, f, n);
    if (!b) { um_free_frames(f, n); return 0; }
    UINT64 k = add_blob(g, b);
    if (!k) { VgpuBlobUnref(b); return 0; }
    a.res = VgpuBlobId(b);
    CopyToUser((void *)(uintptr_t)ptr, &a, sizeof(a));
    return k;
}

static UINT64 do_transfer(Gpu *g, UINT64 ptr)
{
    GpuTransfer a;
    if (!NT_SUCCESS(CopyFromUser(&a, (const void *)(uintptr_t)ptr, sizeof(a)))) return FAIL;
    VgpuBlob *b = get_blob(g, a.blob, false);
    if (!b) return FAIL;
    bool ok = VgpuTransfer3d(g->ctx, b, a.to_host != 0, a.box, a.offset, a.level, a.stride, a.layer_stride,
                             a.sync, a.value);
    VgpuBlobUnref(b);
    return ok ? 0 : FAIL;
}

static void blob_release(void *b) { VgpuBlobUnref(b); }

static UINT64 do_blob_map(Gpu *g, UINT64 n, UINT64 size_ptr)
{
    VgpuBlob *b = get_blob(g, n, false);
    if (!b) return 0;
    UINT64 pa, size, h;
    const PADDR *f;
    UINT64 nf;
    if (VgpuBlobFrames(b, &f, &nf)) {                 /* a virgl resource: its guest pages */
        size = nf * PAGE_SIZE;
        h = um_section_frames(UmCurrent(), f, nf, size, blob_release, b);
    } else {
        if (!VgpuBlobMap(b, &pa, &size)) { VgpuBlobUnref(b); return 0; }
        h = um_section_foreign(UmCurrent(), pa, size, blob_release, b);   /* (the section owns our reference) */
    }
    if (!h) { VgpuBlobUnref(b); return 0; }
    if (size_ptr) CopyToUser((void *)(uintptr_t)size_ptr, &size, sizeof(size));
    return h;
}

static UINT64 do_wait(Gpu *g, UINT64 ptr)
{
    GpuWaitArgs a;
    if (!NT_SUCCESS(CopyFromUser(&a, (const void *)(uintptr_t)ptr, sizeof(a)))) return FAIL;
    if (a.n > MAX_WAIT) return FAIL;
    UINT32 *ids = copy_in(a.ids, (UINT64)a.n * 4);
    UINT64 *vals = ids ? copy_in(a.values, (UINT64)a.n * 8) : NULL;
    int r = vals ? VgpuWait(g->ctx, (int)a.n, ids, vals, a.any != 0, a.timeout_ns) : -1;
    kfree(ids); kfree(vals);
    return r < 0 ? FAIL : (UINT64)r;
}

/* NtNovaGpuCtl(h, op, arg, ptr):
 *   0 info (h unused) -> GpuInfo at @ptr                     1 capability set: GpuCapset at @ptr; its size
 *   2 open a context for capability set @arg: a handle (0: no 3D GPU)
 *   3 submit: GpuSubmit at @ptr                              4 create a blob: GpuBlobCreate at @ptr (res
 *                                                              filled in); the blob's number (0: refused)
 *   5 map blob @arg: a section handle (0: failed), its size at @ptr
 *   6 free blob @arg (mapped views keep it until unmapped)   7 a timeline starting at @arg: its number (0: none)
 *   8 destroy timeline @arg                                  9 timeline op: GpuSyncOp at @ptr (0 read,
 *                                                              1 write, 2 reset)
 *  10 wait: GpuWaitArgs at @ptr: 0 reached, 1 timed out
 *  11 create a virgl resource: GpuRes3d at @ptr (res filled in); its number, like a blob's (0: refused)
 *  12 transfer: GpuTransfer at @ptr
 * Returns 0 (or as said), or -1 for a bad handle or argument. */
static UINT64 sys_gpu(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    if (a2 == 0) {
        GpuInfo i = { Vgpu3dPresent(), 0, Vgpu3dHostVisibleSize() };
        return NT_SUCCESS(CopyToUser((void *)(uintptr_t)a4, &i, sizeof(i))) ? 0 : FAIL;
    }
    if (a2 == 1) {
        GpuCapset c;
        if (!NT_SUCCESS(CopyFromUser(&c, (const void *)(uintptr_t)a4, sizeof(c))) || c.size > 4096) return FAIL;
        UINT8 *buf = kmalloc(c.size ? c.size : 1);
        int n = buf ? Vgpu3dCapset(c.id, c.version, buf, c.size) : -1;
        bool ok = n >= 0 && NT_SUCCESS(CopyToUser((void *)(uintptr_t)c.out, buf, (size_t)n));
        kfree(buf);
        return ok ? (UINT64)n : FAIL;
    }
    if (a2 == 2) {
        Gpu *g = kzalloc(sizeof(*g));
        UmObject *o = g ? kzalloc(sizeof(*o)) : NULL;
        g = o ? g : (kfree(g), NULL);
        if (!g) return 0;
        g->lock = (KSpinLock)KSPINLOCK_INIT;
        g->ctx = VgpuCtxCreate((UINT32)a3, UmCurrent()->name);
        if (!g->ctx) { kfree(g); kfree(o); return 0; }
        o->type = UO_GPU;
        o->refs = 1;
        o->ptr = g;
        o->destroy = gpu_destroy;
        o->free_unlocked = true;
        UINT64 h = um_handle_new_object(UmCurrent(), o);
        um_ob_unref(o);
        return h;
    }
    UmObject *ob;
    Gpu *g = handle_gpu(a1, &ob);
    if (!g) return FAIL;
    UINT64 r = FAIL;
    switch (a2) {
    case 3: r = do_submit(g, a4); break;
    case 4: r = do_blob_create(g, a4); break;
    case 5: r = do_blob_map(g, a3, a4); break;
    case 6: {
        VgpuBlob *b = get_blob(g, a3, true);
        if (b) { VgpuBlobUnref(b); r = 0; }
        break;
    }
    case 7: r = VgpuSyncCreate(g->ctx, a3); break;
    case 8: VgpuSyncDestroy(g->ctx, (UINT32)a3); r = 0; break;
    case 9: {
        GpuSyncOp op;
        if (!NT_SUCCESS(CopyFromUser(&op, (const void *)(uintptr_t)a4, sizeof(op)))) break;
        if (!VgpuSyncAccess(g->ctx, op.id, (int)op.op, &op.value)) break;
        r = NT_SUCCESS(CopyToUser((void *)(uintptr_t)a4, &op, sizeof(op))) ? 0 : FAIL;
        break;
    }
    case 10: r = do_wait(g, a4); break;
    case 11: r = do_res_create(g, a4); break;
    case 12: r = do_transfer(g, a4); break;
    }
    um_ob_unref(ob);
    return r;
}

void um_gpu_syscalls_init(void)
{
    um_install(SYSCALL_NtNovaGpuCtl, sys_gpu);
}
