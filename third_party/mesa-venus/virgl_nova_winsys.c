/*
 * Copyright 2014, 2015 Red Hat.
 * Copyright 2026 NovaOS contributors
 * SPDX-License-Identifier: MIT
 *
 * The virgl winsys for NovaOS: what virgl_drm_winsys.c does through the
 * Linux virtio-gpu DRM uAPI, done through NovaOS's NtNovaGpuCtl service
 * (kernel/um/um_gpu.c), with the shape of virgl_vtest_winsys.c (whose
 * resource cache, command buffers and display targets this follows).
 *
 * A handle is a 3D context on the VIRGL2 capability set.  A resource is
 * a classic one (RESOURCE_CREATE_3D) whose guest pages the kernel gives it
 * and maps as a section, or, for persistent and coherent maps, a mappable
 * host blob.  Every command stream and transfer carries a fence that
 * advances one timeline of the context, numbered in submission order
 * (under the winsys's lock, so the numbers reach the kernel in order):
 * a resource remembers the number of the last stream or transfer that
 * used it, and is busy while the timeline is below it.  Fences are such
 * numbers too.  Frames reach the window through the GDI: the front buffer
 * is read back into its guest pages and copied to the WGL display target
 * (gdi_sw_winsys), as vtest does.
 */

#include <windows.h>
#include <stdio.h>

#include "util/macros.h"
#include "util/u_surface.h"
#include "util/u_memory.h"
#include "util/format/u_format.h"
#include "util/u_inlines.h"
#include "util/os_time.h"
#include "util/u_thread.h"
#include "frontend/sw_winsys.h"

#include "virgl/virgl_winsys.h"
#include "virgl_resource_cache.h"
#include "virtio-gpu/virgl_hw.h"
#include "virtio-gpu/virgl_protocol.h"
#include "virgl_nova_public.h"

#define NOVA_CAPSET_VIRGL 1
#define NOVA_CAPSET_VIRGL2 2
#define NOVA_BLOB_MEM_HOST3D 2
#define NOVA_BLOB_FLAG_MAPPABLE 1

/* NtNovaGpuCtl's operations and structures (kernel/um/um_gpu.c) */
enum {
   GPU_INFO = 0,
   GPU_CAPSET = 1,
   GPU_OPEN = 2,
   GPU_SUBMIT = 3,
   GPU_BLOB_CREATE = 4,
   GPU_BLOB_MAP = 5,
   GPU_BLOB_FREE = 6,
   GPU_SYNC_CREATE = 7,
   GPU_SYNC_OP = 9,
   GPU_WAIT = 10,
   GPU_RES_CREATE = 11,
   GPU_TRANSFER = 12,
};
struct gpu_info { uint32_t present, pad; uint64_t host_visible; };
struct gpu_capset { uint32_t id, version, size, pad; uint64_t out; };
struct gpu_submit { uint64_t cs; uint32_t size, ring; uint32_t nsync, pad; uint64_t syncs, values; };
struct gpu_blob_create { uint32_t blob_mem, flags; uint64_t blob_id, size, cs; uint32_t cs_size, res; };
struct gpu_sync_op { uint32_t id, op; uint64_t value; };
struct gpu_wait { uint32_t n, any; uint64_t timeout_ns, ids, values; };
struct gpu_res_create { uint32_t p[10]; uint32_t res, pad; uint64_t size; };
struct gpu_transfer {
   uint64_t blob, offset;
   uint32_t to_host, level, stride, layer_stride;
   uint32_t box[6];
   uint32_t sync, pad;
   uint64_t value;
};

typedef LONG_PTR(NTAPI *PFN_NtNovaGpuCtl)(INT_PTR h, ULONG op, ULONG_PTR arg, void *ptr);
typedef LONG(NTAPI *PFN_NtMapViewOfSection)(HANDLE, HANDLE, PVOID *, ULONG_PTR, SIZE_T,
                                            PLARGE_INTEGER, PSIZE_T, DWORD, ULONG, ULONG);
typedef LONG(NTAPI *PFN_NtUnmapViewOfSection)(HANDLE, PVOID);
typedef LONG(NTAPI *PFN_NtClose)(HANDLE);

static PFN_NtNovaGpuCtl gpu_ctl;
static PFN_NtMapViewOfSection map_view;
static PFN_NtUnmapViewOfSection unmap_view;
static PFN_NtClose nt_close;

static bool
nova_load(void)
{
   if (gpu_ctl)
      return true;
   HMODULE ntdll = GetModuleHandleA("ntdll.dll");
   if (!ntdll)
      return false;
   map_view = (PFN_NtMapViewOfSection)GetProcAddress(ntdll, "NtMapViewOfSection");
   unmap_view = (PFN_NtUnmapViewOfSection)GetProcAddress(ntdll, "NtUnmapViewOfSection");
   nt_close = (PFN_NtClose)GetProcAddress(ntdll, "NtClose");
   gpu_ctl = (PFN_NtNovaGpuCtl)GetProcAddress(ntdll, "NtNovaGpuCtl");
   return gpu_ctl && map_view && unmap_view && nt_close;
}

struct nova_winsys {
   struct virgl_winsys base;
   struct sw_winsys *sws;
   INT_PTR h;
   uint32_t capset;
   uint32_t timeline;

   mtx_t mutex;            /* the cache, and numbering + sending submissions */
   uint64_t seq;           /* the last number given */
   uint64_t done;          /* the timeline's value when last read */
   int32_t blob_id;
   struct virgl_resource_cache cache;
};

struct virgl_hw_res {
   struct pipe_reference reference;
   uint32_t res_handle;    /* the host's */
   uint32_t blob;          /* the kernel's number */
   int num_cs_references;

   void *ptr;              /* its memory, mapped (NULL: not yet) */
   uint32_t size;

   uint32_t format;
   uint32_t stride;        /* the display target's */
   uint32_t width;
   uint32_t height;
   uint32_t bind;
   uint32_t flags;
   struct sw_displaytarget *dt;

   uint64_t seq;           /* the last submission or transfer that used it */
   struct virgl_resource_cache_entry cache_entry;
};

struct nova_cmd_buf {
   struct virgl_cmd_buf base;
   uint32_t *buf;
   unsigned nres;
   unsigned cres;
   struct virgl_winsys *ws;
   struct virgl_hw_res **res_bo;
};

struct nova_fence {
   struct pipe_reference reference;
   uint64_t seq;
};

static inline struct nova_winsys *
nova_winsys(struct virgl_winsys *vws)
{
   return (struct nova_winsys *)vws;
}

#define cache_entry_container_res(ptr) \
   (struct virgl_hw_res *)((char *)ptr - offsetof(struct virgl_hw_res, cache_entry))

/* -- the timeline -- */

static bool
nova_reached(struct nova_winsys *nws, uint64_t seq)
{
   if (seq <= p_atomic_read(&nws->done))
      return true;
   struct gpu_sync_op op = { .id = nws->timeline, .op = 0 };
   if (gpu_ctl(nws->h, GPU_SYNC_OP, 0, &op))
      return true;            /* (a lost context: nothing will ever finish) */
   uint64_t old = p_atomic_read(&nws->done);
   while (op.value > old) {
      uint64_t prev = p_atomic_cmpxchg(&nws->done, old, op.value);
      if (prev == old)
         break;
      old = prev;
   }
   return seq <= op.value;
}

/* Wait for submission @seq; false when @timeout_ns passed first */
static bool
nova_wait_seq(struct nova_winsys *nws, uint64_t seq, uint64_t timeout_ns)
{
   if (nova_reached(nws, seq))
      return true;
   if (!timeout_ns)
      return false;
   uint32_t id = nws->timeline;
   struct gpu_wait w = {
      .n = 1,
      .timeout_ns = timeout_ns == OS_TIMEOUT_INFINITE ? UINT64_MAX : timeout_ns,
      .ids = (uint64_t)(uintptr_t)&id,
      .values = (uint64_t)(uintptr_t)&seq,
   };
   LONG_PTR r = gpu_ctl(nws->h, GPU_WAIT, 0, &w);
   if (r == 0) {
      nova_reached(nws, seq);
      return true;
   }
   return r != 1;
}

/* -- resources -- */

static void *
nova_map(struct nova_winsys *nws, struct virgl_hw_res *res)
{
   if (res->ptr || !res->size)
      return res->ptr;
   uint64_t len = 0;
   INT_PTR sec = gpu_ctl(nws->h, GPU_BLOB_MAP, res->blob, &len);
   if (!sec)
      return NULL;
   void *base = NULL;
   SIZE_T view = (SIZE_T)len;
   LONG st = map_view((HANDLE)sec, (HANDLE)(LONG_PTR)-1, &base, 0, 0, NULL, &view,
                      2 /* ViewUnmap */, 0, PAGE_READWRITE);
   nt_close((HANDLE)sec); /* (the view keeps it) */
   if (st < 0)
      return NULL;
   res->ptr = base;
   return base;
}

static void
nova_res_destroy(struct nova_winsys *nws, struct virgl_hw_res *res)
{
   if (res->ptr)
      unmap_view((HANDLE)(LONG_PTR)-1, res->ptr);
   gpu_ctl(nws->h, GPU_BLOB_FREE, res->blob, NULL);
   if (res->dt)
      nws->sws->displaytarget_destroy(nws->sws, res->dt);
   FREE(res);
}

static bool
nova_resource_is_busy(struct virgl_winsys *vws, struct virgl_hw_res *res)
{
   return !nova_reached(nova_winsys(vws), p_atomic_read(&res->seq));
}

static void
nova_resource_wait(struct virgl_winsys *vws, struct virgl_hw_res *res)
{
   nova_wait_seq(nova_winsys(vws), p_atomic_read(&res->seq), OS_TIMEOUT_INFINITE);
}

static inline bool
can_cache_resource(uint32_t bind)
{
   return bind == VIRGL_BIND_CONSTANT_BUFFER || bind == VIRGL_BIND_INDEX_BUFFER ||
          bind == VIRGL_BIND_VERTEX_BUFFER || bind == VIRGL_BIND_CUSTOM ||
          bind == VIRGL_BIND_STAGING;
}

static void
nova_resource_reference(struct virgl_winsys *vws, struct virgl_hw_res **dres,
                        struct virgl_hw_res *sres)
{
   struct nova_winsys *nws = nova_winsys(vws);
   struct virgl_hw_res *old = *dres;

   if (pipe_reference(&(*dres)->reference, &sres->reference)) {
      if (!can_cache_resource(old->bind)) {
         nova_res_destroy(nws, old);
      } else {
         mtx_lock(&nws->mutex);
         virgl_resource_cache_add(&nws->cache, &old->cache_entry);
         mtx_unlock(&nws->mutex);
      }
   }
   *dres = sres;
}

static struct virgl_hw_res *
nova_resource_create(struct virgl_winsys *vws, enum pipe_texture_target target,
                     const void *map_front_private, uint32_t format, uint32_t bind,
                     uint32_t width, uint32_t height, uint32_t depth, uint32_t array_size,
                     uint32_t last_level, uint32_t nr_samples, uint32_t flags, uint32_t size)
{
   struct nova_winsys *nws = nova_winsys(vws);
   struct virgl_resource_params params = { .size = size, .bind = bind, .format = format,
                                           .flags = flags, .nr_samples = nr_samples,
                                           .width = width, .height = height, .depth = depth,
                                           .array_size = array_size, .last_level = last_level,
                                           .target = target };
   struct virgl_hw_res *res = CALLOC_STRUCT(virgl_hw_res);
   if (!res)
      return NULL;
   uint64_t n;

   if (flags & (VIRGL_RESOURCE_FLAG_MAP_PERSISTENT | VIRGL_RESOURCE_FLAG_MAP_COHERENT)) {
      /* host memory the program maps: a blob the stream creates */
      uint32_t cmd[VIRGL_PIPE_RES_CREATE_SIZE + 1] = { 0 };
      int32_t blob_id = p_atomic_inc_return(&nws->blob_id);
      size = align(size, 4096);
      cmd[0] = VIRGL_CMD0(VIRGL_CCMD_PIPE_RESOURCE_CREATE, 0, VIRGL_PIPE_RES_CREATE_SIZE);
      cmd[VIRGL_PIPE_RES_CREATE_FORMAT] = pipe_to_virgl_format(format);
      cmd[VIRGL_PIPE_RES_CREATE_BIND] = bind;
      cmd[VIRGL_PIPE_RES_CREATE_TARGET] = target;
      cmd[VIRGL_PIPE_RES_CREATE_WIDTH] = align(width, 4096);
      cmd[VIRGL_PIPE_RES_CREATE_HEIGHT] = height;
      cmd[VIRGL_PIPE_RES_CREATE_DEPTH] = depth;
      cmd[VIRGL_PIPE_RES_CREATE_ARRAY_SIZE] = array_size;
      cmd[VIRGL_PIPE_RES_CREATE_LAST_LEVEL] = last_level;
      cmd[VIRGL_PIPE_RES_CREATE_NR_SAMPLES] = nr_samples;
      cmd[VIRGL_PIPE_RES_CREATE_FLAGS] = flags;
      cmd[VIRGL_PIPE_RES_CREATE_BLOB_ID] = blob_id;
      struct gpu_blob_create a = {
         .blob_mem = NOVA_BLOB_MEM_HOST3D,
         .flags = NOVA_BLOB_FLAG_MAPPABLE,
         .blob_id = (uint64_t)blob_id,
         .size = size,
         .cs = (uint64_t)(uintptr_t)cmd,
         .cs_size = sizeof(cmd),
      };
      mtx_lock(&nws->mutex); /* (the stream goes in order with the others) */
      n = (uint64_t)gpu_ctl(nws->h, GPU_BLOB_CREATE, 0, &a);
      mtx_unlock(&nws->mutex);
      res->res_handle = a.res;
   } else {
      struct gpu_res_create a = {
         .p = { target, pipe_to_virgl_format(format), bind, width, height, depth, array_size,
                last_level, nr_samples, 0 },
         .size = size,
      };
      n = (uint64_t)gpu_ctl(nws->h, GPU_RES_CREATE, 0, &a);
      res->res_handle = a.res;
   }
   if (!n || n == (uint64_t)-1) {
      FREE(res);
      return NULL;
   }
   res->blob = (uint32_t)n;

   if (bind & (VIRGL_BIND_DISPLAY_TARGET | VIRGL_BIND_SCANOUT)) {
      res->dt = nws->sws->displaytarget_create(nws->sws, bind, format, width, height, 64,
                                               map_front_private, &res->stride);
      if (!res->dt) {
         gpu_ctl(nws->h, GPU_BLOB_FREE, res->blob, NULL);
         FREE(res);
         return NULL;
      }
   }

   res->bind = bind;
   res->flags = flags;
   res->format = format;
   res->width = width;
   res->height = height;
   res->size = size;
   virgl_resource_cache_entry_init(&res->cache_entry, params);
   pipe_reference_init(&res->reference, 1);
   p_atomic_set(&res->num_cs_references, 0);
   return res;
}

static struct virgl_hw_res *
nova_resource_cache_create(struct virgl_winsys *vws, enum pipe_texture_target target,
                           const void *map_front_private, uint32_t format, uint32_t bind,
                           uint32_t width, uint32_t height, uint32_t depth,
                           uint32_t array_size, uint32_t last_level, uint32_t nr_samples,
                           uint32_t flags, uint32_t size)
{
   struct nova_winsys *nws = nova_winsys(vws);
   struct virgl_resource_params params = { .size = size, .bind = bind, .format = format,
                                           .flags = flags, .nr_samples = nr_samples,
                                           .width = width, .height = height, .depth = depth,
                                           .array_size = array_size, .last_level = last_level,
                                           .target = target };

   if (can_cache_resource(bind)) {
      mtx_lock(&nws->mutex);
      struct virgl_resource_cache_entry *entry =
         virgl_resource_cache_remove_compatible(&nws->cache, params);
      mtx_unlock(&nws->mutex);
      if (entry) {
         struct virgl_hw_res *res = cache_entry_container_res(entry);
         pipe_reference_init(&res->reference, 1);
         return res;
      }
   }
   return nova_resource_create(vws, target, map_front_private, format, bind, width, height,
                               depth, array_size, last_level, nr_samples, flags, size);
}

static void *
nova_resource_map(struct virgl_winsys *vws, struct virgl_hw_res *res)
{
   return nova_map(nova_winsys(vws), res);
}

static uint32_t
nova_resource_get_storage_size(struct virgl_winsys *vws, struct virgl_hw_res *res)
{
   return res->size;
}

/* -- transfers -- */

static int
nova_transfer(struct virgl_winsys *vws, struct virgl_hw_res *res, const struct pipe_box *box,
              uint32_t buf_offset, uint32_t level, bool to_host)
{
   struct nova_winsys *nws = nova_winsys(vws);
   struct gpu_transfer t = {
      .blob = res->blob,
      .offset = buf_offset,
      .to_host = to_host,
      .level = level,
      .box = { box->x, box->y, box->z, box->width, box->height, box->depth },
      .sync = nws->timeline,
   };
   mtx_lock(&nws->mutex);
   t.value = ++nws->seq;
   LONG_PTR r = gpu_ctl(nws->h, GPU_TRANSFER, 0, &t);
   p_atomic_set(&res->seq, t.value);
   mtx_unlock(&nws->mutex);
   return r ? -1 : 0;
}

/* (strides: the host takes the level's own, as Linux leaves them 0 for
 * resources with guest memory, which virgl lays out that way) */
static int
nova_transfer_put(struct virgl_winsys *vws, struct virgl_hw_res *res, const struct pipe_box *box,
                  uint32_t stride, uint32_t layer_stride, uint32_t buf_offset, uint32_t level)
{
   return nova_transfer(vws, res, box, buf_offset, level, true);
}

static int
nova_transfer_get(struct virgl_winsys *vws, struct virgl_hw_res *res, const struct pipe_box *box,
                  uint32_t stride, uint32_t layer_stride, uint32_t buf_offset, uint32_t level)
{
   return nova_transfer(vws, res, box, buf_offset, level, false);
}

/* -- command buffers -- */

static bool
nova_res_is_added(struct nova_cmd_buf *cbuf, struct virgl_hw_res *res)
{
   for (unsigned i = 0; i < cbuf->cres; i++)
      if (cbuf->res_bo[i] == res)
         return true;
   return false;
}

static void
nova_release_all_res(struct nova_winsys *nws, struct nova_cmd_buf *cbuf)
{
   for (unsigned i = 0; i < cbuf->cres; i++) {
      p_atomic_dec(&cbuf->res_bo[i]->num_cs_references);
      nova_resource_reference(&nws->base, &cbuf->res_bo[i], NULL);
   }
   cbuf->cres = 0;
}

static void
nova_add_res(struct nova_winsys *nws, struct nova_cmd_buf *cbuf, struct virgl_hw_res *res)
{
   if (unlikely(nova_res_is_added(cbuf, res)))
      return;
   if (cbuf->cres >= cbuf->nres) {
      unsigned new_nres = cbuf->nres + 256;
      struct virgl_hw_res **bo = REALLOC(cbuf->res_bo, cbuf->nres * sizeof(*bo),
                                         new_nres * sizeof(*bo));
      if (!bo)
         return;
      cbuf->res_bo = bo;
      cbuf->nres = new_nres;
   }
   cbuf->res_bo[cbuf->cres] = NULL;
   nova_resource_reference(&nws->base, &cbuf->res_bo[cbuf->cres], res);
   p_atomic_inc(&res->num_cs_references);
   cbuf->cres++;
}

static struct virgl_cmd_buf *
nova_cmd_buf_create(struct virgl_winsys *vws, uint32_t size)
{
   struct nova_cmd_buf *cbuf = CALLOC_STRUCT(nova_cmd_buf);
   if (!cbuf)
      return NULL;
   cbuf->nres = 512;
   cbuf->res_bo = CALLOC(cbuf->nres, sizeof(struct virgl_hw_res *));
   cbuf->buf = CALLOC(size, sizeof(uint32_t));
   if (!cbuf->res_bo || !cbuf->buf) {
      FREE(cbuf->res_bo);
      FREE(cbuf->buf);
      FREE(cbuf);
      return NULL;
   }
   cbuf->ws = vws;
   cbuf->base.buf = cbuf->buf;
   return &cbuf->base;
}

static void
nova_cmd_buf_destroy(struct virgl_cmd_buf *_cbuf)
{
   struct nova_cmd_buf *cbuf = (struct nova_cmd_buf *)_cbuf;
   nova_release_all_res(nova_winsys(cbuf->ws), cbuf);
   FREE(cbuf->res_bo);
   FREE(cbuf->buf);
   FREE(cbuf);
}

static struct pipe_fence_handle *
nova_fence_new(uint64_t seq)
{
   struct nova_fence *f = CALLOC_STRUCT(nova_fence);
   if (!f)
      return NULL;
   pipe_reference_init(&f->reference, 1);
   f->seq = seq;
   return (struct pipe_fence_handle *)f;
}

static int
nova_submit_cmd(struct virgl_winsys *vws, struct virgl_cmd_buf *_cbuf,
                struct pipe_fence_handle **fence)
{
   struct nova_winsys *nws = nova_winsys(vws);
   struct nova_cmd_buf *cbuf = (struct nova_cmd_buf *)_cbuf;
   if (cbuf->base.cdw == 0)
      return 0;

   uint32_t id = nws->timeline;
   uint64_t seq;
   struct gpu_submit s = {
      .cs = (uint64_t)(uintptr_t)cbuf->base.buf,
      .size = cbuf->base.cdw * 4,
      .nsync = 1,
      .syncs = (uint64_t)(uintptr_t)&id,
      .values = (uint64_t)(uintptr_t)&seq,
   };
   mtx_lock(&nws->mutex);
   seq = ++nws->seq;
   LONG_PTR r = gpu_ctl(nws->h, GPU_SUBMIT, 0, &s);
   for (unsigned i = 0; i < cbuf->cres; i++)
      p_atomic_set(&cbuf->res_bo[i]->seq, seq);
   mtx_unlock(&nws->mutex);

   if (fence && r == 0)
      *fence = nova_fence_new(seq);
   nova_release_all_res(nws, cbuf);
   cbuf->base.cdw = 0;
   return r ? -1 : 0;
}

static void
nova_emit_res(struct virgl_winsys *vws, struct virgl_cmd_buf *_cbuf, struct virgl_hw_res *res,
              bool write_buf)
{
   struct nova_cmd_buf *cbuf = (struct nova_cmd_buf *)_cbuf;
   if (write_buf)
      cbuf->base.buf[cbuf->base.cdw++] = res->res_handle;
   nova_add_res(nova_winsys(vws), cbuf, res);
}

static bool
nova_res_is_ref(struct virgl_winsys *vws, struct virgl_cmd_buf *_cbuf, struct virgl_hw_res *res)
{
   return p_atomic_read(&res->num_cs_references) != 0;
}

static int
nova_get_caps(struct virgl_winsys *vws, struct virgl_drm_caps *caps)
{
   struct nova_winsys *nws = nova_winsys(vws);
   virgl_ws_fill_new_caps_defaults(caps);
   struct gpu_capset c = {
      .id = nws->capset,
      .version = 0,
      .size = nws->capset == NOVA_CAPSET_VIRGL2 ? sizeof(union virgl_caps)
                                                : sizeof(struct virgl_caps_v1),
      .out = (uint64_t)(uintptr_t)&caps->caps,
   };
   if (gpu_ctl(0, GPU_CAPSET, 0, &c) < 0)
      return -1;
   /* Read-backs come back into the resource's own guest pages (the front
    * buffer every frame), so every texture has them: no host-to-guest copy
    * transfers, which would leave textures a one-page stand-in (as vtest) */
   caps->caps.v2.capability_bits_v2 &= ~VIRGL_CAP_V2_COPY_TRANSFER_BOTH_DIRECTIONS;
   return 0;
}

/* -- fences -- */

static struct pipe_fence_handle *
nova_cs_create_fence(struct virgl_winsys *vws, int fd)
{
   struct nova_winsys *nws = nova_winsys(vws);
   mtx_lock(&nws->mutex);
   uint64_t seq = nws->seq;
   mtx_unlock(&nws->mutex);
   return nova_fence_new(seq);
}

static bool
nova_fence_wait(struct virgl_winsys *vws, struct pipe_fence_handle *fence, uint64_t timeout)
{
   return nova_wait_seq(nova_winsys(vws), ((struct nova_fence *)fence)->seq, timeout);
}

static void
nova_fence_reference(struct virgl_winsys *vws, struct pipe_fence_handle **dst,
                     struct pipe_fence_handle *src)
{
   struct nova_fence *d = (struct nova_fence *)*dst, *s = (struct nova_fence *)src;
   if (pipe_reference(d ? &d->reference : NULL, s ? &s->reference : NULL))
      FREE(d);
   *dst = src;
}

/* (one timeline, in submission order: nothing to wait for on the host) */
static void
nova_fence_server_sync(struct virgl_winsys *vws, struct virgl_cmd_buf *cbuf,
                       struct pipe_fence_handle *fence)
{
}

static int
nova_fence_get_fd(struct virgl_winsys *vws, struct pipe_fence_handle *fence)
{
   return -1;
}

static int
nova_get_fd(struct virgl_winsys *vws)
{
   return -1;
}

/* -- the window -- */

static void
nova_flush_frontbuffer(struct virgl_winsys *vws, struct virgl_cmd_buf *cmdbuf,
                       struct virgl_hw_res *res, unsigned level, unsigned layer,
                       void *winsys_drawable_handle, struct pipe_box *sub_box)
{
   struct nova_winsys *nws = nova_winsys(vws);
   if (!res->dt)
      return;

   struct pipe_box box;
   uint32_t shm_stride = util_format_get_stride(res->format, res->width);
   uint32_t offset = 0;
   if (sub_box) {
      box = *sub_box;
      offset = box.y / util_format_get_blockheight(res->format) * shm_stride +
               box.x / util_format_get_blockwidth(res->format) *
                  util_format_get_blocksize(res->format);
   } else {
      memset(&box, 0, sizeof(box));
      box.z = layer;
      box.width = res->width;
      box.height = res->height;
      box.depth = 1;
   }

   void *ptr = nova_map(nws, res);
   if (!ptr || nova_transfer(vws, res, &box, offset, level, false))
      return;
   nova_resource_wait(vws, res);

   void *dt_map = nws->sws->displaytarget_map(nws->sws, res->dt, 0);
   if (dt_map) {
      util_copy_rect(dt_map, res->format, res->stride, box.x, box.y, box.width, box.height, ptr,
                     shm_stride, box.x, box.y);
      nws->sws->displaytarget_unmap(nws->sws, res->dt);
   }
   nws->sws->displaytarget_display(nws->sws, res->dt, winsys_drawable_handle, !!sub_box,
                                   sub_box);
}

/* -- the winsys -- */

static bool
nova_cache_entry_is_busy(struct virgl_resource_cache_entry *entry, void *user_data)
{
   struct nova_winsys *nws = user_data;
   return nova_resource_is_busy(&nws->base, cache_entry_container_res(entry));
}

static void
nova_cache_entry_release(struct virgl_resource_cache_entry *entry, void *user_data)
{
   nova_res_destroy(user_data, cache_entry_container_res(entry));
}

static void
nova_winsys_destroy(struct virgl_winsys *vws)
{
   struct nova_winsys *nws = nova_winsys(vws);
   virgl_resource_cache_flush(&nws->cache);
   mtx_destroy(&nws->mutex);
   nt_close((HANDLE)nws->h);
   FREE(nws);
}

bool
virgl_nova_present(void)
{
   struct gpu_info gi;
   return nova_load() && !gpu_ctl(0, GPU_INFO, 0, &gi) && gi.present;
}

struct virgl_winsys *
virgl_nova_winsys_wrap(struct sw_winsys *sws)
{
   static const unsigned CACHE_TIMEOUT_USEC = 1000000;
   struct gpu_info gi;
   if (!nova_load() || gpu_ctl(0, GPU_INFO, 0, &gi) || !gi.present)
      return NULL;

   struct nova_winsys *nws = CALLOC_STRUCT(nova_winsys);
   if (!nws)
      return NULL;
   nws->sws = sws;
   nws->capset = NOVA_CAPSET_VIRGL2;
   nws->h = gpu_ctl(0, GPU_OPEN, NOVA_CAPSET_VIRGL2, NULL);
   if (!nws->h) {
      nws->capset = NOVA_CAPSET_VIRGL;
      nws->h = gpu_ctl(0, GPU_OPEN, NOVA_CAPSET_VIRGL, NULL);
   }
   if (!nws->h) {
      FREE(nws);
      return NULL;
   }
   nws->timeline = (uint32_t)gpu_ctl(nws->h, GPU_SYNC_CREATE, 0, NULL);
   if (!nws->timeline || nws->timeline == (uint32_t)-1) {
      nt_close((HANDLE)nws->h);
      FREE(nws);
      return NULL;
   }

   virgl_resource_cache_init(&nws->cache, CACHE_TIMEOUT_USEC, nova_cache_entry_is_busy,
                             nova_cache_entry_release, nws);
   (void)mtx_init(&nws->mutex, mtx_plain);

   nws->base.destroy = nova_winsys_destroy;
   nws->base.get_fd = nova_get_fd;
   nws->base.transfer_put = nova_transfer_put;
   nws->base.transfer_get = nova_transfer_get;
   nws->base.resource_create = nova_resource_cache_create;
   nws->base.resource_reference = nova_resource_reference;
   nws->base.resource_map = nova_resource_map;
   nws->base.resource_wait = nova_resource_wait;
   nws->base.resource_is_busy = nova_resource_is_busy;
   nws->base.resource_get_storage_size = nova_resource_get_storage_size;
   nws->base.cmd_buf_create = nova_cmd_buf_create;
   nws->base.cmd_buf_destroy = nova_cmd_buf_destroy;
   nws->base.submit_cmd = nova_submit_cmd;
   nws->base.emit_res = nova_emit_res;
   nws->base.res_is_referenced = nova_res_is_ref;
   nws->base.get_caps = nova_get_caps;
   nws->base.cs_create_fence = nova_cs_create_fence;
   nws->base.fence_wait = nova_fence_wait;
   nws->base.fence_reference = nova_fence_reference;
   nws->base.fence_server_sync = nova_fence_server_sync;
   nws->base.fence_get_fd = nova_fence_get_fd;
   nws->base.flush_frontbuffer = nova_flush_frontbuffer;
   nws->base.supports_fences = 0;
   nws->base.supports_encoded_transfers = 1;
   nws->base.supports_coherent = gi.host_visible != 0;
   return &nws->base;
}
