/*
 * Copyright 2026 NovaOS contributors
 * SPDX-License-Identifier: MIT
 *
 * The Venus renderer back end for NovaOS: the virtgpu back end's job
 * (vn_renderer_virtgpu.c) done through NovaOS's NtNovaGpuCtl service
 * instead of the Linux virtio-gpu DRM uAPI.  The kernel keeps one 3D
 * context per handle, its blobs (numbered from 1) and its timelines; a
 * mappable blob comes back as a section, mapped with NtMapViewOfSection.
 * There are no dma-bufs or sync files, so external memory and fences
 * stay inside the process.
 */

#include <windows.h>

#include "virtio/virtio-gpu/venus_hw.h"

#include "util/sparse_array.h"

#include "vn_renderer_internal.h"

#define NOVA_CAPSET_VENUS 4
#define NOVA_BLOB_MEM_HOST3D 2
#define NOVA_BLOB_FLAG_MAPPABLE 1
#define NOVA_BLOB_FLAG_SHAREABLE 2

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
   GPU_SYNC_DESTROY = 8,
   GPU_SYNC_OP = 9,
   GPU_WAIT = 10,
};
struct gpu_info { uint32_t present, pad; uint64_t host_visible; };
struct gpu_capset { uint32_t id, version, size, pad; uint64_t out; };
struct gpu_submit { uint64_t cs; uint32_t size, ring; uint32_t nsync, pad; uint64_t syncs, values; };
struct gpu_blob_create { uint32_t blob_mem, flags; uint64_t blob_id, size, cs; uint32_t cs_size, res; };
struct gpu_sync_op { uint32_t id, op; uint64_t value; };
struct gpu_wait { uint32_t n, any; uint64_t timeout_ns, ids, values; };

typedef LONG_PTR(NTAPI *PFN_NtNovaGpuCtl)(INT_PTR h, ULONG op, ULONG_PTR arg, void *ptr);
typedef LONG(NTAPI *PFN_NtMapViewOfSection)(HANDLE, HANDLE, PVOID *, ULONG_PTR, SIZE_T,
                                            PLARGE_INTEGER, PSIZE_T, DWORD, ULONG, ULONG);
typedef LONG(NTAPI *PFN_NtUnmapViewOfSection)(HANDLE, PVOID);
typedef LONG(NTAPI *PFN_NtClose)(HANDLE);

static PFN_NtNovaGpuCtl gpu_ctl;
static PFN_NtMapViewOfSection map_view;
static PFN_NtUnmapViewOfSection unmap_view;
static PFN_NtClose nt_close;

struct nova_shmem {
   struct vn_renderer_shmem base;
   uint32_t blob;
};

struct nova_bo {
   struct vn_renderer_bo base;
   uint32_t blob;
   uint32_t blob_flags;
};

struct nova {
   struct vn_renderer base;

   struct vn_instance *instance;
   INT_PTR h;

   struct virgl_renderer_capset_venus capset;

   /* indexed by blob number */
   struct util_sparse_array shmem_array;
   struct util_sparse_array bo_array;

   struct vn_renderer_shmem_cache shmem_cache;
};

static bool
nova_load(void)
{
   if (gpu_ctl)
      return true;
   HMODULE ntdll = GetModuleHandleA("ntdll.dll");
   if (!ntdll)
      return false;
   map_view = (PFN_NtMapViewOfSection)GetProcAddress(ntdll, "NtMapViewOfSection");
   unmap_view =
      (PFN_NtUnmapViewOfSection)GetProcAddress(ntdll, "NtUnmapViewOfSection");
   nt_close = (PFN_NtClose)GetProcAddress(ntdll, "NtClose");
   gpu_ctl = (PFN_NtNovaGpuCtl)GetProcAddress(ntdll, "NtNovaGpuCtl");
   return gpu_ctl && map_view && unmap_view && nt_close;
}

/* Map the section of blob @blob; its address, or NULL */
static void *
nova_map_blob(struct nova *gpu, uint32_t blob, size_t *size)
{
   uint64_t len = 0;
   INT_PTR sec = gpu_ctl(gpu->h, GPU_BLOB_MAP, blob, &len);
   if (!sec) {
      vn_log(gpu->instance, "mapping blob %u failed", blob);
      return NULL;
   }
   void *base = NULL;
   SIZE_T view = (SIZE_T)len;
   LONG st = map_view((HANDLE)sec, (HANDLE)(LONG_PTR)-1, &base, 0, 0, NULL,
                      &view, 2 /* ViewUnmap */, 0, PAGE_READWRITE);
   nt_close((HANDLE)sec); /* (the view keeps it) */
   if (st < 0) {
      vn_log(gpu->instance, "mapping blob %u's section failed: %lx", blob,
             (unsigned long)st);
      return NULL;
   }
   if (size)
      *size = (size_t)len;
   return base;
}

static uint32_t
nova_blob_create(struct nova *gpu,
                 const struct vn_renderer_submit_batch *batch,
                 uint32_t blob_flags,
                 size_t size,
                 uint64_t blob_id,
                 uint32_t *res_id)
{
   struct gpu_blob_create a = {
      .blob_mem = NOVA_BLOB_MEM_HOST3D,
      .flags = blob_flags,
      .blob_id = blob_id,
      .size = align64(size, 4096),
      .cs = batch ? (uint64_t)(uintptr_t)batch->cs_data : 0,
      .cs_size = batch ? (uint32_t)batch->cs_size : 0,
   };
   LONG_PTR n = gpu_ctl(gpu->h, GPU_BLOB_CREATE, 0, &a);
   if (n <= 0) {
      vn_log(gpu->instance,
             "RESOURCE_CREATE_BLOB failed: flags=%u, size=%zu, id=%" PRIu64,
             blob_flags, size, blob_id);
      return 0;
   }
   *res_id = a.res;
   return (uint32_t)n;
}

/* ---- timelines ---------------------------------------------------------- */

static VkResult
nova_sync_op(struct vn_renderer *renderer,
             struct vn_renderer_sync *sync,
             uint32_t op,
             uint64_t *val)
{
   struct nova *gpu = (struct nova *)renderer;
   struct gpu_sync_op a = { .id = sync->syncobj_handle, .op = op,
                            .value = val ? *val : 0 };
   if (gpu_ctl(gpu->h, GPU_SYNC_OP, 0, &a))
      return VK_ERROR_DEVICE_LOST;
   if (val)
      *val = a.value;
   return VK_SUCCESS;
}

static VkResult
nova_sync_write(struct vn_renderer *renderer,
                struct vn_renderer_sync *sync,
                uint64_t val)
{
   return nova_sync_op(renderer, sync, 1, &val);
}

static VkResult
nova_sync_read(struct vn_renderer *renderer,
               struct vn_renderer_sync *sync,
               uint64_t *val)
{
   return nova_sync_op(renderer, sync, 0, val);
}

static VkResult
nova_sync_reset(struct vn_renderer *renderer, struct vn_renderer_sync *sync)
{
   return nova_sync_op(renderer, sync, 2, NULL);
}

static int
nova_sync_export_syncobj(struct vn_renderer *renderer,
                         struct vn_renderer_sync *sync,
                         bool sync_file)
{
   return -1;
}

static void
nova_sync_destroy(struct vn_renderer *renderer, struct vn_renderer_sync *sync)
{
   struct nova *gpu = (struct nova *)renderer;
   gpu_ctl(gpu->h, GPU_SYNC_DESTROY, sync->syncobj_handle, NULL);
   free(sync);
}

static VkResult
nova_sync_create_from_syncobj(struct vn_renderer *renderer,
                              int fd,
                              bool sync_file,
                              struct vn_renderer_sync **out_sync)
{
   return VK_ERROR_INVALID_EXTERNAL_HANDLE;
}

static VkResult
nova_sync_create(struct vn_renderer *renderer,
                 uint64_t initial_val,
                 struct vn_renderer_sync **out_sync)
{
   struct nova *gpu = (struct nova *)renderer;
   LONG_PTR id = gpu_ctl(gpu->h, GPU_SYNC_CREATE, initial_val, NULL);
   if (id <= 0)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   struct vn_renderer_sync *sync = calloc(1, sizeof(*sync));
   if (!sync) {
      gpu_ctl(gpu->h, GPU_SYNC_DESTROY, id, NULL);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   sync->syncobj_handle = (uint32_t)id;
   *out_sync = sync;
   return VK_SUCCESS;
}

/* ---- buffer objects ----------------------------------------------------- */

static void
nova_bo_invalidate(struct vn_renderer *renderer,
                   struct vn_renderer_bo *bo,
                   VkDeviceSize offset,
                   VkDeviceSize size)
{
   /* nop: host-visible memory is coherent */
}

static void
nova_bo_flush(struct vn_renderer *renderer,
              struct vn_renderer_bo *bo,
              VkDeviceSize offset,
              VkDeviceSize size)
{
   /* nop: host-visible memory is coherent */
}

static void *
nova_bo_map(struct vn_renderer *renderer,
            struct vn_renderer_bo *_bo,
            void *placed_addr)
{
   struct nova *gpu = (struct nova *)renderer;
   struct nova_bo *bo = (struct nova_bo *)_bo;
   if (placed_addr)
      return NULL; /* (no placed maps: views go where the kernel puts them) */
   if (!bo->base.mmap_ptr && (bo->blob_flags & NOVA_BLOB_FLAG_MAPPABLE))
      bo->base.mmap_ptr = nova_map_blob(gpu, bo->blob, NULL);
   return bo->base.mmap_ptr;
}

static int
nova_bo_export_dma_buf(struct vn_renderer *renderer, struct vn_renderer_bo *bo)
{
   return -1;
}

static int
nova_bo_export_sync_file(struct vn_renderer *renderer,
                         struct vn_renderer_bo *bo)
{
   return -1;
}

static bool
nova_bo_destroy(struct vn_renderer *renderer, struct vn_renderer_bo *_bo)
{
   struct nova *gpu = (struct nova *)renderer;
   struct nova_bo *bo = (struct nova_bo *)_bo;
   if (bo->base.mmap_ptr)
      unmap_view((HANDLE)(LONG_PTR)-1, bo->base.mmap_ptr);
   const uint32_t blob = bo->blob;
   bo->blob = 0;
   gpu_ctl(gpu->h, GPU_BLOB_FREE, blob, NULL);
   return true;
}

static VkResult
nova_bo_create_from_dma_buf(struct vn_renderer *renderer,
                            VkDeviceSize size,
                            int fd,
                            VkMemoryPropertyFlags flags,
                            struct vn_renderer_bo **out_bo)
{
   return VK_ERROR_INVALID_EXTERNAL_HANDLE;
}

static VkResult
nova_bo_create_from_device_memory(
   struct vn_renderer *renderer,
   struct vn_renderer_submit_batch *batch,
   VkDeviceSize size,
   vn_object_id mem_id,
   VkMemoryPropertyFlags flags,
   VkExternalMemoryHandleTypeFlags external_handles,
   struct vn_renderer_bo **out_bo)
{
   struct nova *gpu = (struct nova *)renderer;
   uint32_t blob_flags = 0;
   if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
      blob_flags |= NOVA_BLOB_FLAG_MAPPABLE;
   if (external_handles)
      blob_flags |= NOVA_BLOB_FLAG_SHAREABLE;

   uint32_t res_id;
   uint32_t blob =
      nova_blob_create(gpu, batch, blob_flags, size, mem_id, &res_id);
   if (!blob)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   struct nova_bo *bo = util_sparse_array_get(&gpu->bo_array, blob);
   *bo = (struct nova_bo){
      .base = {
         .refcount = VN_REFCOUNT_INIT(1),
         .res_id = res_id,
         .mmap_size = size,
      },
      .blob = blob,
      .blob_flags = blob_flags,
   };
   *out_bo = &bo->base;
   return VK_SUCCESS;
}

/* ---- shared memory ------------------------------------------------------ */

static void
nova_shmem_destroy_now(struct vn_renderer *renderer,
                       struct vn_renderer_shmem *_shmem)
{
   struct nova *gpu = (struct nova *)renderer;
   struct nova_shmem *shmem = (struct nova_shmem *)_shmem;
   unmap_view((HANDLE)(LONG_PTR)-1, shmem->base.mmap_ptr);
   gpu_ctl(gpu->h, GPU_BLOB_FREE, shmem->blob, NULL);
}

static void
nova_shmem_destroy(struct vn_renderer *renderer,
                   struct vn_renderer_shmem *shmem)
{
   struct nova *gpu = (struct nova *)renderer;
   if (vn_renderer_shmem_cache_add(&gpu->shmem_cache, shmem))
      return;
   nova_shmem_destroy_now(&gpu->base, shmem);
}

static struct vn_renderer_shmem *
nova_shmem_create(struct vn_renderer *renderer, size_t size)
{
   struct nova *gpu = (struct nova *)renderer;

   struct vn_renderer_shmem *cached_shmem =
      vn_renderer_shmem_cache_get(&gpu->shmem_cache, size);
   if (cached_shmem) {
      cached_shmem->refcount = VN_REFCOUNT_INIT(1);
      return cached_shmem;
   }

   uint32_t res_id;
   uint32_t blob =
      nova_blob_create(gpu, NULL, NOVA_BLOB_FLAG_MAPPABLE, size, 0, &res_id);
   if (!blob)
      return NULL;
   void *ptr = nova_map_blob(gpu, blob, NULL);
   if (!ptr) {
      gpu_ctl(gpu->h, GPU_BLOB_FREE, blob, NULL);
      return NULL;
   }

   struct nova_shmem *shmem = util_sparse_array_get(&gpu->shmem_array, blob);
   *shmem = (struct nova_shmem){
      .base = {
         .refcount = VN_REFCOUNT_INIT(1),
         .res_id = res_id,
         .mmap_size = size,
         .mmap_ptr = ptr,
      },
      .blob = blob,
   };
   return &shmem->base;
}

/* ---- submissions and waits ---------------------------------------------- */

static VkResult
nova_wait(struct vn_renderer *renderer, const struct vn_renderer_wait *wait)
{
   struct nova *gpu = (struct nova *)renderer;

   STACK_ARRAY(uint32_t, ids, wait->sync_count);
   for (uint32_t i = 0; i < wait->sync_count; i++)
      ids[i] = wait->syncs[i]->syncobj_handle;

   struct gpu_wait a = {
      .n = wait->sync_count,
      .any = wait->wait_any,
      .timeout_ns = wait->timeout,
      .ids = (uint64_t)(uintptr_t)ids,
      .values = (uint64_t)(uintptr_t)wait->sync_values,
   };
   LONG_PTR r = gpu_ctl(gpu->h, GPU_WAIT, 0, &a);
   STACK_ARRAY_FINISH(ids);

   if (r < 0)
      return VK_ERROR_DEVICE_LOST;
   return r ? VK_TIMEOUT : VK_SUCCESS;
}

static VkResult
nova_submit(struct vn_renderer *renderer,
            const struct vn_renderer_submit_batch *batch)
{
   struct nova *gpu = (struct nova *)renderer;

   STACK_ARRAY(uint32_t, ids, batch->sync_count);
   for (uint32_t i = 0; i < batch->sync_count; i++)
      ids[i] = batch->syncs[i]->syncobj_handle;

   struct gpu_submit a = {
      .cs = (uint64_t)(uintptr_t)batch->cs_data,
      .size = (uint32_t)batch->cs_size,
      .ring = batch->ring_idx,
      .nsync = batch->sync_count,
      .syncs = (uint64_t)(uintptr_t)ids,
      .values = (uint64_t)(uintptr_t)batch->sync_values,
   };
   LONG_PTR r = gpu_ctl(gpu->h, GPU_SUBMIT, 0, &a);
   STACK_ARRAY_FINISH(ids);

   return r ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
}

/* ---- set-up ------------------------------------------------------------- */

static void
nova_init_renderer_info(struct nova *gpu)
{
   struct vn_renderer_info *info = &gpu->base.info;

   info->pci.vendor_id = 0x1af4;
   info->pci.device_id = 0x1050;

   info->has_dma_buf_import = false;
   info->has_external_sync = false;
   info->has_timeline_sync = true;
   info->has_implicit_fencing = false;

   const struct virgl_renderer_capset_venus *capset = &gpu->capset;
   info->wire_format_version = capset->wire_format_version;
   info->vk_xml_version = capset->vk_xml_version;
   info->vk_ext_command_serialization_spec_version =
      capset->vk_ext_command_serialization_spec_version;
   info->vk_mesa_venus_protocol_spec_version =
      capset->vk_mesa_venus_protocol_spec_version;

   STATIC_ASSERT(sizeof(info->vk_extension_mask) >=
                 sizeof(capset->vk_extension_mask1));
   memcpy(info->vk_extension_mask, capset->vk_extension_mask1,
          sizeof(capset->vk_extension_mask1));

   info->max_timeline_count = 64;

   if (capset->use_guest_vram)
      info->has_guest_vram = true;
}

static void
nova_destroy(struct vn_renderer *renderer, const VkAllocationCallbacks *alloc)
{
   struct nova *gpu = (struct nova *)renderer;

   vn_renderer_shmem_cache_fini(&gpu->shmem_cache);

   if (gpu->h)
      nt_close((HANDLE)gpu->h);

   util_sparse_array_finish(&gpu->shmem_array);
   util_sparse_array_finish(&gpu->bo_array);

   vk_free(alloc, gpu);
}

static VkResult
nova_init(struct nova *gpu)
{
   util_sparse_array_init(&gpu->shmem_array, sizeof(struct nova_shmem), 1024);
   util_sparse_array_init(&gpu->bo_array, sizeof(struct nova_bo), 1024);

   struct gpu_info gi = { 0 };
   if (!nova_load() || gpu_ctl(0, GPU_INFO, 0, &gi) || !gi.present) {
      if (VN_DEBUG(INIT))
         vn_log(gpu->instance, "no virtio GPU with 3D");
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   struct gpu_capset c = {
      .id = NOVA_CAPSET_VENUS,
      .version = 0,
      .size = sizeof(gpu->capset),
      .out = (uint64_t)(uintptr_t)&gpu->capset,
   };
   if (gpu_ctl(0, GPU_CAPSET, 0, &c) < 0 ||
       gpu->capset.wire_format_version == 0) {
      if (VN_DEBUG(INIT))
         vn_log(gpu->instance, "no venus capset");
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   gpu->h = gpu_ctl(0, GPU_OPEN, NOVA_CAPSET_VENUS, NULL);
   if (!gpu->h) {
      if (VN_DEBUG(INIT))
         vn_log(gpu->instance, "failed to initialize context");
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   vn_renderer_shmem_cache_init(&gpu->shmem_cache, &gpu->base,
                                nova_shmem_destroy_now);

   nova_init_renderer_info(gpu);

   gpu->base.ops.destroy = nova_destroy;
   gpu->base.ops.submit = nova_submit;
   gpu->base.ops.wait = nova_wait;

   gpu->base.shmem_ops.create = nova_shmem_create;
   gpu->base.shmem_ops.destroy = nova_shmem_destroy;

   gpu->base.bo_ops.create_from_device_memory =
      nova_bo_create_from_device_memory;
   gpu->base.bo_ops.create_from_dma_buf = nova_bo_create_from_dma_buf;
   gpu->base.bo_ops.destroy = nova_bo_destroy;
   gpu->base.bo_ops.export_dma_buf = nova_bo_export_dma_buf;
   gpu->base.bo_ops.export_sync_file = nova_bo_export_sync_file;
   gpu->base.bo_ops.map = nova_bo_map;
   gpu->base.bo_ops.flush = nova_bo_flush;
   gpu->base.bo_ops.invalidate = nova_bo_invalidate;

   gpu->base.sync_ops.create = nova_sync_create;
   gpu->base.sync_ops.create_from_syncobj = nova_sync_create_from_syncobj;
   gpu->base.sync_ops.destroy = nova_sync_destroy;
   gpu->base.sync_ops.export_syncobj = nova_sync_export_syncobj;
   gpu->base.sync_ops.reset = nova_sync_reset;
   gpu->base.sync_ops.read = nova_sync_read;
   gpu->base.sync_ops.write = nova_sync_write;

   return VK_SUCCESS;
}

VkResult
vn_renderer_create_nova(struct vn_instance *instance,
                        const VkAllocationCallbacks *alloc,
                        struct vn_renderer **renderer)
{
   struct nova *gpu = vk_zalloc(alloc, sizeof(*gpu), VN_DEFAULT_ALIGN,
                                VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!gpu)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   gpu->instance = instance;

   VkResult result = nova_init(gpu);
   if (result != VK_SUCCESS) {
      nova_destroy(&gpu->base, alloc);
      return result;
   }

   *renderer = &gpu->base;

   if (VN_DEBUG(INIT))
      vn_log(gpu->instance, "NovaOS backend initialized");

   return VK_SUCCESS;
}

/* The ICD is present only where it can work: with no virtio GPU with 3D,
 * a loader moves on to the next driver (lavapipe) */
bool vn_renderer_nova_present(void);
bool
vn_renderer_nova_present(void)
{
   struct gpu_info gi = { 0 };
   return nova_load() && !gpu_ctl(0, GPU_INFO, 0, &gi) && gi.present;
}

/* vk_util.c's vk_spec_info_to_nir_spirv refers to the SPIR-V front end,
 * which Venus (the host compiles shaders) never calls or links */
struct nir_spirv_specialization;
struct nir_spirv_specialization *vtn_alloc_specialization(uint32_t num_entries);
bool vtn_add_specialization_entry(struct nir_spirv_specialization *spec, uint32_t slot,
                                  uint32_t entry_id, uint32_t entry_size,
                                  const void *entry_data, bool defined_on_module);
void vtn_free_specialization(struct nir_spirv_specialization *spec);

struct nir_spirv_specialization *
vtn_alloc_specialization(uint32_t num_entries)
{
   return NULL;
}

bool
vtn_add_specialization_entry(struct nir_spirv_specialization *spec, uint32_t slot,
                             uint32_t entry_id, uint32_t entry_size,
                             const void *entry_data, bool defined_on_module)
{
   return false;
}

void
vtn_free_specialization(struct nir_spirv_specialization *spec)
{
}
