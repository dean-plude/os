/*
 * Shared memory between Vulkan devices: VK_KHR_external_memory_win32 for a
 * driver without it.
 *
 * Direct3D 11 programs share textures between devices by handle
 * (D3D11_RESOURCE_MISC_SHARED_NTHANDLE, IDXGIResource1::CreateSharedHandle,
 * ID3D11Device1::OpenSharedResource1): Chromium draws a web page into one
 * and Qt WebEngine shows it from its own device, for instance.  DXVK does
 * that with VK_KHR_external_memory_win32, which Mesa's lavapipe (the App
 * Store's Mesa 3D, Vulkan on the CPU) has no Windows build of.  lavapipe's
 * memory is the computer's memory anyway, and it can use memory it is
 * handed (VK_EXT_external_memory_host), so the loader provides the
 * extension itself:
 *
 *  - memory allocated for export is a section (CreateFileMapping) mapped
 *    into the process and handed to the driver as host memory; its handle
 *    (vkGetMemoryWin32HandleKHR) is a new handle to that section;
 *  - memory imported from a handle maps the section and hands the driver
 *    the same pages, so both devices draw into and read the one texture,
 *    in this process or another one the handle was passed to;
 *  - images and buffers made for such memory, format queries and device
 *    creation are answered as a driver with the extension would.
 *
 * Fences shared the same way (ID3D11Fence::CreateSharedHandle, which
 * Chromium signals and Qt waits on) are timeline semaphores with
 * VK_KHR_external_semaphore_win32, provided here too: a shared semaphore
 * keeps its value in a section as well, and a thread of the loader copies
 * a new value between each device's semaphore and the section, so a wait
 * on one device ends when another device signals.
 *
 * Only NT handles (VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT, and for
 * semaphores also D3D12_FENCE_BIT, which D3D11 fences use) are provided; the older global (KMT) handles are not.  DXVK keeps a shared
 * texture's description with the handle (DeviceIoControl, answered for
 * sections by the kernel, kernel/um/um_thread.c).
 */
#include <windows.h>
#include <winternl.h>

typedef int VkResult;
typedef UINT32 VkFlags;
typedef UINT64 VkHandle64;                    /* a non-dispatchable handle */
typedef void (WINAPI *PFN_vkVoidFunction)(void);
typedef PFN_vkVoidFunction (WINAPI *PFN_gipa)(void *instance, const char *name);

#define VK_SUCCESS                          0
#define VK_INCOMPLETE                       5
#define VK_ERROR_OUT_OF_HOST_MEMORY         (-1)
#define VK_ERROR_EXTENSION_NOT_PRESENT      (-7)
#define VK_ERROR_INVALID_EXTERNAL_HANDLE    (-1000072003)

#define ST_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO  1000071000
#define ST_EXTERNAL_IMAGE_FORMAT_PROPERTIES            1000071001
#define ST_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO        1000071002
#define ST_EXTERNAL_MEMORY_BUFFER_CREATE_INFO          1000072000
#define ST_EXTERNAL_MEMORY_IMAGE_CREATE_INFO           1000072001
#define ST_EXPORT_MEMORY_ALLOCATE_INFO                 1000072002
#define ST_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR         1000073000
#define ST_EXPORT_MEMORY_WIN32_HANDLE_INFO_KHR         1000073001
#define ST_IMPORT_MEMORY_HOST_POINTER_INFO_EXT         1000178000
#define ST_MEMORY_HOST_POINTER_PROPERTIES_EXT          1000178001
#define ST_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO     1000076000
#define ST_EXPORT_SEMAPHORE_CREATE_INFO                1000077000
#define ST_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR      1000078000
#define ST_EXPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR      1000078001
#define ST_SEMAPHORE_TYPE_CREATE_INFO                  1000207002
#define ST_SEMAPHORE_SIGNAL_INFO                       1000207005

#define HT_OPAQUE_WIN32        0x00000002u
#define HT_OPAQUE_WIN32_KMT    0x00000004u
#define HT_HOST_ALLOCATION     0x00000080u
#define HT_WIN32               (HT_OPAQUE_WIN32 | HT_OPAQUE_WIN32_KMT)

#define EMF_DEDICATED_ONLY     0x1u
#define EMF_EXPORTABLE         0x2u
#define EMF_IMPORTABLE         0x4u

#define SHT_OPAQUE_WIN32       0x00000002u   /* semaphore handle types */
#define SHT_D3D12_FENCE        0x00000008u
#define SHT_OURS               (SHT_OPAQUE_WIN32 | SHT_D3D12_FENCE)
#define ESF_EXPORTABLE         0x1u
#define ESF_IMPORTABLE         0x2u
#define SEMAPHORE_TIMELINE     1

#define EXT_WIN32  "VK_KHR_external_memory_win32"
#define EXT_SEM    "VK_KHR_external_semaphore_win32"
#define EXT_HOST   "VK_EXT_external_memory_host"
#define PAGE       4096u

typedef struct Base { int sType; struct Base *pNext; } Base;
typedef struct { char name[256]; UINT32 spec; } ExtProps;          /* VkExtensionProperties */
typedef struct { VkFlags features, export_from, compatible; } ExtMemProps;   /* VkExternalMemoryProperties */
typedef struct { int sType; void *pNext; UINT32 handleType; } ExtImageInfo;  /* ...ExternalImageFormatInfo */
typedef struct { int sType; void *pNext; ExtMemProps props; } ExtFormatProps; /* VkExternalImageFormatProperties */
typedef struct { int sType; const void *pNext; VkFlags flags, usage; UINT32 handleType; } ExtBufferInfo;
typedef struct { int sType; void *pNext; ExtMemProps props; } ExtBufferProps;
typedef struct { int sType; const void *pNext; VkFlags handleTypes; } ExtCreateInfo; /* External...CreateInfo, Export...AllocateInfo */
typedef struct {                                                     /* VkDeviceCreateInfo */
    int sType; const void *pNext; VkFlags flags;
    UINT32 queues; const void *pQueues;
    UINT32 layers; const char *const *ppLayers;
    UINT32 exts; const char *const *ppExts;
    const void *features;
} DeviceInfo;
typedef struct { int sType; const void *pNext; UINT64 size; UINT32 type; } AllocInfo;   /* VkMemoryAllocateInfo */
typedef struct { int sType; const void *pNext; UINT32 handleType; HANDLE handle; LPCWSTR name; } ImportWin32;
typedef struct { int sType; const void *pNext; UINT32 handleType; void *ptr; } ImportHost;
typedef struct { int sType; const void *pNext; VkHandle64 memory; UINT32 handleType; } GetWin32Info;
typedef struct { int sType; void *pNext; UINT32 typeBits; } HandleProps;   /* VkMemoryWin32HandlePropertiesKHR, ...HostPointerPropertiesEXT */
typedef struct { int sType; const void *pNext; UINT32 handleType; } ExtSemInfo;     /* ...ExternalSemaphoreInfo */
typedef struct { int sType; void *pNext; VkFlags export_from, compatible, features; } ExtSemProps;
typedef struct { int sType; const void *pNext; UINT32 type; UINT64 initial; } SemTypeInfo;
typedef struct { int sType; const void *pNext; VkHandle64 semaphore; UINT64 value; } SemSignalInfo;
typedef struct {                                                     /* VkImportSemaphoreWin32HandleInfoKHR */
    int sType; const void *pNext; VkHandle64 semaphore; VkFlags flags; UINT32 handleType; HANDLE handle; LPCWSTR name;
} SemImport;
typedef struct { int sType; const void *pNext; VkHandle64 semaphore; UINT32 handleType; } SemGetInfo;

/* The driver's commands */
typedef VkResult (WINAPI *PFN_create_device)(void *, const DeviceInfo *, const void *, void **);
typedef VkResult (WINAPI *PFN_enum_dev_ext)(void *, const char *, UINT32 *, ExtProps *);
typedef VkResult (WINAPI *PFN_image_props2)(void *, const Base *, Base *);
typedef void     (WINAPI *PFN_buffer_props)(void *, const Base *, Base *);
typedef VkResult (WINAPI *PFN_create_object)(void *, const Base *, const void *, VkHandle64 *);
typedef VkResult (WINAPI *PFN_alloc)(void *, const AllocInfo *, const void *, VkHandle64 *);
typedef void     (WINAPI *PFN_free)(void *, VkHandle64, const void *);
typedef VkResult (WINAPI *PFN_host_props)(void *, UINT32, const void *, HandleProps *);
typedef void     (WINAPI *PFN_sem_props)(void *, const ExtSemInfo *, ExtSemProps *);
typedef VkResult (WINAPI *PFN_counter)(void *, VkHandle64, UINT64 *);
typedef VkResult (WINAPI *PFN_signal)(void *, const SemSignalInfo *);

static PFN_create_device r_create_device;
static PFN_enum_dev_ext  r_enum_dev_ext;
static PFN_image_props2  r_image_props2;
static PFN_buffer_props  r_buffer_props;
static PFN_create_object r_create_image, r_create_buffer;
static PFN_alloc         r_alloc;
static PFN_free          r_free;
static PFN_host_props    r_host_props;
static PFN_sem_props     r_sem_props;
static PFN_create_object r_create_semaphore;
static PFN_free          r_destroy_semaphore;
static PFN_counter       r_counter;
static PFN_signal        r_signal;

/* Whether the driver needs the loader's extension: -1 not known yet */
static volatile LONG g_emulate = -1;

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}

static int has_ext(const ExtProps *e, UINT32 n, const char *name)
{
    for (UINT32 i = 0; i < n; i++) if (streq(e[i].name, name)) return 1;
    return 0;
}

/* The driver's device extensions (heap block), or NULL */
static ExtProps *driver_exts(void *phys, UINT32 *n)
{
    *n = 0;
    if (!r_enum_dev_ext || r_enum_dev_ext(phys, NULL, n, NULL) != VK_SUCCESS) return NULL;
    ExtProps *e = HeapAlloc(GetProcessHeap(), 0, (*n + 2) * sizeof(ExtProps));
    if (e && r_enum_dev_ext(phys, NULL, n, e) < 0) { HeapFree(GetProcessHeap(), 0, e); e = NULL; }
    return e;
}

static int emulating(void *phys)
{
    LONG v = g_emulate;
    if (v >= 0 || !phys) return v > 0;
    UINT32 n;
    ExtProps *e = driver_exts(phys, &n);
    if (!e) return 0;
    v = has_ext(e, n, EXT_HOST) && !has_ext(e, n, EXT_WIN32) && r_alloc && r_free;
    HeapFree(GetProcessHeap(), 0, e);
    InterlockedExchange(&g_emulate, v);
    return v;
}

/* ---- memory the loader shares ------------------------------------------- */
typedef struct Shared {
    struct Shared *next;
    VkHandle64 memory;
    HANDLE section;
    void *view;
} Shared;

typedef struct { PVOID base; ULONG attributes; LARGE_INTEGER size; } SectionInfo;   /* SECTION_BASIC_INFORMATION */

static SRWLOCK g_lock = SRWLOCK_INIT;
static Shared *g_shared;

static HANDLE section_of(VkHandle64 memory)
{
    HANDLE h = NULL;
    AcquireSRWLockShared(&g_lock);
    for (Shared *s = g_shared; s; s = s->next) if (s->memory == memory) { h = s->section; break; }
    ReleaseSRWLockShared(&g_lock);
    return h;
}

/* Take @node out of a structure chain (put back with relink) */
typedef struct { Base *prev, *node; } Unlinked;

static int unlink_node(const void *head, int type, Unlinked *u)
{
    for (Base *p = (Base *)head; p && p->pNext; p = p->pNext)
        if (p->pNext->sType == type) {
            u->prev = p;
            u->node = p->pNext;
            p->pNext = u->node->pNext;
            return 1;
        }
    u->prev = u->node = NULL;
    return 0;
}

static void relink(Unlinked *u)
{
    if (u->prev) u->prev->pNext = u->node;
}

static Base *find(const void *head, int type)
{
    for (Base *p = ((const Base *)head)->pNext; p; p = p->pNext) if (p->sType == type) return p;
    return NULL;
}

static VkResult WINAPI x_allocate_memory(void *device, const AllocInfo *info, const void *alloc, VkHandle64 *mem)
{
    ExtCreateInfo *exp = (ExtCreateInfo *)find(info, ST_EXPORT_MEMORY_ALLOCATE_INFO);
    ImportWin32 *imp = (ImportWin32 *)find(info, ST_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR);
    int exporting = exp && (exp->handleTypes & HT_WIN32), importing = imp && (imp->handleType & HT_WIN32);
    if (g_emulate <= 0 || (!exporting && !importing)) return r_alloc(device, info, alloc, mem);
    if ((exporting && (exp->handleTypes & ~HT_OPAQUE_WIN32)) || (importing && imp->handleType != HT_OPAQUE_WIN32))
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;              /* (no KMT handles) */

    UINT64 size = (info->size + PAGE - 1) & ~(UINT64)(PAGE - 1);
    HANDLE section = NULL;
    if (importing && imp->handle) {
        if (!DuplicateHandle(GetCurrentProcess(), imp->handle, GetCurrentProcess(), &section, 0, FALSE,
                             DUPLICATE_SAME_ACCESS))
            section = NULL;
    } else if (importing && imp->name)
        section = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, imp->name);
    else if (importing)
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    else
        section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, (DWORD)(size >> 32), (DWORD)size, NULL);
    if (!section) return importing ? VK_ERROR_INVALID_EXTERNAL_HANDLE : VK_ERROR_OUT_OF_HOST_MEMORY;

    SectionInfo si = { 0 };
    void *view = NtQuerySection(section, 0, &si, sizeof(si), NULL) >= 0 && (UINT64)si.size.QuadPart >= info->size
               ? MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, 0) : NULL;
    Shared *s = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*s));
    if (!view || !s) {
        if (view) UnmapViewOfFile(view);
        CloseHandle(section);
        if (s) HeapFree(GetProcessHeap(), 0, s);
        return importing ? VK_ERROR_INVALID_EXTERNAL_HANDLE : VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    if (size > (UINT64)si.size.QuadPart) size = (UINT64)si.size.QuadPart;   /* (an imported one may be exactly the size) */

    /* The driver gets the view as host memory, without the Win32 parts */
    Unlinked u[3];
    unlink_node(info, ST_EXPORT_MEMORY_ALLOCATE_INFO, &u[0]);
    unlink_node(info, ST_EXPORT_MEMORY_WIN32_HANDLE_INFO_KHR, &u[1]);
    unlink_node(info, ST_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, &u[2]);
    ImportHost host = { ST_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, info->pNext, HT_HOST_ALLOCATION, view };
    AllocInfo a = *info;
    a.pNext = &host;
    a.size = size;
    VkResult r = r_alloc(device, &a, alloc, mem);
    for (int i = 2; i >= 0; i--) relink(&u[i]);
    if (r != VK_SUCCESS) {
        UnmapViewOfFile(view);
        CloseHandle(section);
        HeapFree(GetProcessHeap(), 0, s);
        return r;
    }
    s->memory = *mem;
    s->section = section;
    s->view = view;
    AcquireSRWLockExclusive(&g_lock);
    s->next = g_shared;
    g_shared = s;
    ReleaseSRWLockExclusive(&g_lock);
    return VK_SUCCESS;
}

static void WINAPI x_free_memory(void *device, VkHandle64 memory, const void *alloc)
{
    r_free(device, memory, alloc);
    if (!memory) return;
    Shared *s = NULL;
    AcquireSRWLockExclusive(&g_lock);
    for (Shared **pp = &g_shared; *pp; pp = &(*pp)->next)
        if ((*pp)->memory == memory) { s = *pp; *pp = s->next; break; }
    ReleaseSRWLockExclusive(&g_lock);
    if (!s) return;
    UnmapViewOfFile(s->view);
    CloseHandle(s->section);
    HeapFree(GetProcessHeap(), 0, s);
}

static VkResult WINAPI x_get_memory_win32_handle(void *device, const GetWin32Info *info, HANDLE *out)
{
    (void)device;
    HANDLE section = info->handleType == HT_OPAQUE_WIN32 ? section_of(info->memory) : NULL;
    if (!section) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    return DuplicateHandle(GetCurrentProcess(), section, GetCurrentProcess(), out, 0, FALSE, DUPLICATE_SAME_ACCESS)
         ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}

static VkResult WINAPI x_get_memory_win32_handle_props(void *device, UINT32 type, HANDLE handle, HandleProps *props)
{
    (void)handle;
    if (type != HT_OPAQUE_WIN32) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    /* Any memory type that takes host memory */
    static __declspec(align(4096)) char page[PAGE];
    HandleProps host = { ST_MEMORY_HOST_POINTER_PROPERTIES_EXT, NULL, 0 };
    VkResult r = r_host_props ? r_host_props(device, HT_HOST_ALLOCATION, page, &host) : VK_ERROR_INVALID_EXTERNAL_HANDLE;
    props->typeBits = host.typeBits;
    return r;
}

/* ---- semaphores the loader shares ----------------------------------------- */
/* A shared timeline semaphore: the device's own one, and its value in a
 * section every device holding the handle maps */
typedef struct SharedSem {
    struct SharedSem *next;
    void *device;
    VkHandle64 semaphore;
    HANDLE section;
    volatile UINT64 *value;
} SharedSem;

static SharedSem *g_sems;
static HANDLE g_pump_wake;

/* Copies new values both ways, while a shared semaphore exists: a device's
 * signal reaches the section, and the section's value the other devices */
static DWORD WINAPI sem_pump(void *arg)
{
    (void)arg;
    for (;;) {
        AcquireSRWLockShared(&g_lock);
        int any = g_sems != NULL;
        for (SharedSem *s = g_sems; s; s = s->next) {
            UINT64 local = 0, shared = *s->value;
            if (r_counter(s->device, s->semaphore, &local) != VK_SUCCESS) continue;
            if (local > shared)
                while (shared < local) {
                    UINT64 was = (UINT64)InterlockedCompareExchange64((volatile LONG64 *)s->value, (LONG64)local, (LONG64)shared);
                    if (was == shared) break;
                    shared = was;
                }
            else if (shared > local) {
                SemSignalInfo si = { ST_SEMAPHORE_SIGNAL_INFO, NULL, s->semaphore, shared };
                r_signal(s->device, &si);
            }
        }
        ReleaseSRWLockShared(&g_lock);
        if (any) Sleep(1);
        else WaitForSingleObject(g_pump_wake, INFINITE);
    }
    return 0;
}

/* Starts sharing @semaphore through @section (taken over) */
static VkResult link_semaphore(void *device, VkHandle64 semaphore, HANDLE section)
{
    SectionInfo si = { 0 };
    void *view = NtQuerySection(section, 0, &si, sizeof(si), NULL) >= 0 && si.size.QuadPart >= (LONGLONG)sizeof(UINT64)
               ? MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, 0) : NULL;
    SharedSem *s = view ? HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*s)) : NULL;
    if (!s) {
        if (view) UnmapViewOfFile(view);
        CloseHandle(section);
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    s->device = device;
    s->semaphore = semaphore;
    s->section = section;
    s->value = view;
    SharedSem *old = NULL;
    AcquireSRWLockExclusive(&g_lock);
    for (SharedSem **pp = &g_sems; *pp; pp = &(*pp)->next)          /* (imported again: the new handle) */
        if ((*pp)->semaphore == semaphore) { old = *pp; *pp = old->next; break; }
    s->next = g_sems;
    g_sems = s;
    if (!g_pump_wake) {
        g_pump_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
        HANDLE t = g_pump_wake ? CreateThread(NULL, 0, sem_pump, NULL, 0, NULL) : NULL;
        if (t) CloseHandle(t);
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (g_pump_wake) SetEvent(g_pump_wake);
    if (old) {
        UnmapViewOfFile((void *)old->value);
        CloseHandle(old->section);
        HeapFree(GetProcessHeap(), 0, old);
    }
    return VK_SUCCESS;
}

static int timeline(const void *info)
{
    SemTypeInfo *t = (SemTypeInfo *)find(info, ST_SEMAPHORE_TYPE_CREATE_INFO);
    return t && t->type == SEMAPHORE_TIMELINE;
}

static VkResult WINAPI x_create_semaphore(void *device, const Base *info, const void *alloc, VkHandle64 *out)
{
    ExtCreateInfo *exp = g_emulate > 0 ? (ExtCreateInfo *)find(info, ST_EXPORT_SEMAPHORE_CREATE_INFO) : NULL;
    if (!exp || !(exp->handleTypes & SHT_OURS)) return r_create_semaphore(device, info, alloc, out);
    if ((exp->handleTypes & ~SHT_OURS) || !timeline(info)) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, PAGE, NULL);
    if (!section) return VK_ERROR_OUT_OF_HOST_MEMORY;
    Unlinked u[2];
    unlink_node(info, ST_EXPORT_SEMAPHORE_CREATE_INFO, &u[0]);
    unlink_node(info, ST_EXPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR, &u[1]);
    VkResult r = r_create_semaphore(device, info, alloc, out);
    relink(&u[1]);
    relink(&u[0]);
    if (r != VK_SUCCESS) { CloseHandle(section); return r; }
    UINT64 now = 0;
    r_counter(device, *out, &now);
    r = link_semaphore(device, *out, section);
    if (r != VK_SUCCESS) { r_destroy_semaphore(device, *out, alloc); *out = 0; return r; }
    AcquireSRWLockShared(&g_lock);
    for (SharedSem *s = g_sems; s; s = s->next) if (s->semaphore == *out) *s->value = now;
    ReleaseSRWLockShared(&g_lock);
    return VK_SUCCESS;
}

static void WINAPI x_destroy_semaphore(void *device, VkHandle64 semaphore, const void *alloc)
{
    SharedSem *s = NULL;
    if (semaphore) {
        AcquireSRWLockExclusive(&g_lock);
        for (SharedSem **pp = &g_sems; *pp; pp = &(*pp)->next)
            if ((*pp)->semaphore == semaphore) { s = *pp; *pp = s->next; break; }
        ReleaseSRWLockExclusive(&g_lock);
    }
    r_destroy_semaphore(device, semaphore, alloc);
    if (!s) return;
    UnmapViewOfFile((void *)s->value);
    CloseHandle(s->section);
    HeapFree(GetProcessHeap(), 0, s);
}

static VkResult WINAPI x_get_semaphore_win32_handle(void *device, const SemGetInfo *info, HANDLE *out)
{
    (void)device;
    HANDLE section = NULL;
    if (info->handleType & SHT_OURS) {
        AcquireSRWLockShared(&g_lock);
        for (SharedSem *s = g_sems; s; s = s->next) if (s->semaphore == info->semaphore) { section = s->section; break; }
        ReleaseSRWLockShared(&g_lock);
    }
    if (!section) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    return DuplicateHandle(GetCurrentProcess(), section, GetCurrentProcess(), out, 0, FALSE, DUPLICATE_SAME_ACCESS)
         ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}

static VkResult WINAPI x_import_semaphore_win32_handle(void *device, const SemImport *info)
{
    if (!(info->handleType & SHT_OURS)) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    HANDLE section = NULL;
    if (info->handle) {
        if (!DuplicateHandle(GetCurrentProcess(), info->handle, GetCurrentProcess(), &section, 0, FALSE,
                             DUPLICATE_SAME_ACCESS))
            section = NULL;
    } else if (info->name)
        section = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, info->name);
    if (!section) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    return link_semaphore(device, info->semaphore, section);
}

/* Timeline semaphores share by NT handle; binary ones as the driver says */
static void WINAPI x_sem_props(void *phys, const ExtSemInfo *info, ExtSemProps *props)
{
    if (!emulating(phys) || !(info->handleType & SHT_OURS) || !timeline(info)) { r_sem_props(phys, info, props); return; }
    props->export_from = info->handleType;
    props->compatible = info->handleType;
    props->features = ESF_EXPORTABLE | ESF_IMPORTABLE;
}

/* ---- objects made for it ------------------------------------------------- */
/* An image or buffer for shared memory is one for host memory to the driver */
static VkResult create_object(PFN_create_object create, int type, void *device, const Base *info, const void *alloc,
                              VkHandle64 *out)
{
    ExtCreateInfo *e = g_emulate > 0 ? (ExtCreateInfo *)find(info, type) : NULL;
    VkFlags was = e ? e->handleTypes : 0;
    if (e && (was & HT_WIN32)) e->handleTypes = (was & ~HT_WIN32) | HT_HOST_ALLOCATION;
    VkResult r = create(device, info, alloc, out);
    if (e) e->handleTypes = was;
    return r;
}

static VkResult WINAPI x_create_image(void *device, const Base *info, const void *alloc, VkHandle64 *out)
{
    return create_object(r_create_image, ST_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, device, info, alloc, out);
}

static VkResult WINAPI x_create_buffer(void *device, const Base *info, const void *alloc, VkHandle64 *out)
{
    return create_object(r_create_buffer, ST_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, device, info, alloc, out);
}

/* What NT handles allow: export and import, one allocation per handle */
static void win32_props(ExtMemProps *p)
{
    p->features = EMF_DEDICATED_ONLY | EMF_EXPORTABLE | EMF_IMPORTABLE;
    p->export_from = HT_OPAQUE_WIN32;
    p->compatible = HT_OPAQUE_WIN32;
}

static VkResult WINAPI x_image_props2(void *phys, const Base *info, Base *props)
{
    ExtImageInfo *e = emulating(phys) ? (ExtImageInfo *)find(info, ST_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO) : NULL;
    if (!e || e->handleType != HT_OPAQUE_WIN32) return r_image_props2(phys, info, props);
    e->handleType = HT_HOST_ALLOCATION;                     /* (what the driver can do with any memory) */
    VkResult r = r_image_props2(phys, info, props);
    e->handleType = HT_OPAQUE_WIN32;
    ExtFormatProps *out = (ExtFormatProps *)find(props, ST_EXTERNAL_IMAGE_FORMAT_PROPERTIES);
    if (r == VK_SUCCESS && out) win32_props(&out->props);
    return r;
}

static void WINAPI x_buffer_props(void *phys, const Base *info, Base *props)
{
    ExtBufferInfo *e = (ExtBufferInfo *)info;
    if (!emulating(phys) || e->handleType != HT_OPAQUE_WIN32) { r_buffer_props(phys, info, props); return; }
    e->handleType = HT_HOST_ALLOCATION;
    r_buffer_props(phys, info, props);
    e->handleType = HT_OPAQUE_WIN32;
    win32_props(&((ExtBufferProps *)props)->props);
}

/* ---- the device and its extensions --------------------------------------- */
static VkResult WINAPI x_enum_dev_ext(void *phys, const char *layer, UINT32 *count, ExtProps *props)
{
    if (layer || !emulating(phys)) return r_enum_dev_ext(phys, layer, count, props);
    UINT32 n;
    ExtProps *e = driver_exts(phys, &n);
    if (!e) return VK_ERROR_OUT_OF_HOST_MEMORY;
    ExtProps *w = &e[n++];
    ZeroMemory(w, sizeof(*w));
    lstrcpyA(w->name, EXT_WIN32);
    w->spec = 1;
    if (r_sem_props && r_create_semaphore && r_destroy_semaphore && r_counter && r_signal) {
        w = &e[n++];
        ZeroMemory(w, sizeof(*w));
        lstrcpyA(w->name, EXT_SEM);
        w->spec = 1;
    }
    VkResult r = VK_SUCCESS;
    if (!props) *count = n;
    else {
        if (*count < n) { n = *count; r = VK_INCOMPLETE; }
        CopyMemory(props, e, n * sizeof(ExtProps));
        *count = n;
    }
    HeapFree(GetProcessHeap(), 0, e);
    return r;
}

static VkResult WINAPI x_create_device(void *phys, const DeviceInfo *info, const void *alloc, void **device)
{
    int want = 0, host = 0;
    for (UINT32 i = 0; i < info->exts; i++) {
        if (streq(info->ppExts[i], EXT_WIN32) || streq(info->ppExts[i], EXT_SEM)) want = 1;
        if (streq(info->ppExts[i], EXT_HOST)) host = 1;
    }
    if (!want || !emulating(phys)) return r_create_device(phys, info, alloc, device);
    /* The driver gets VK_EXT_external_memory_host in their place */
    const char **names = HeapAlloc(GetProcessHeap(), 0, (info->exts + 1) * sizeof(char *));
    if (!names) return VK_ERROR_OUT_OF_HOST_MEMORY;
    UINT32 n = 0;
    for (UINT32 i = 0; i < info->exts; i++)
        if (!streq(info->ppExts[i], EXT_WIN32) && !streq(info->ppExts[i], EXT_SEM)) names[n++] = info->ppExts[i];
    if (!host) names[n++] = EXT_HOST;
    DeviceInfo d = *info;
    d.exts = n;
    d.ppExts = names;
    VkResult r = r_create_device(phys, &d, alloc, device);
    HeapFree(GetProcessHeap(), 0, names);
    return r;
}

/* ---- the loader's side ---------------------------------------------------- */
extern void *p_vkCreateDevice, *p_vkEnumerateDeviceExtensionProperties, *p_vkGetPhysicalDeviceImageFormatProperties2,
            *p_vkGetPhysicalDeviceExternalBufferProperties, *p_vkAllocateMemory, *p_vkFreeMemory, *p_vkCreateImage,
            *p_vkCreateBuffer, *p_vkCreateSemaphore, *p_vkDestroySemaphore,
            *p_vkGetPhysicalDeviceExternalSemaphoreProperties;

/* flags: 1 = only while the loader provides the extensions, 2 = semaphores */
static const struct { const char *name; PFN_vkVoidFunction fn; int flags; } g_own[] = {
    { "vkCreateDevice",                                (PFN_vkVoidFunction)x_create_device, 0 },
    { "vkEnumerateDeviceExtensionProperties",          (PFN_vkVoidFunction)x_enum_dev_ext, 0 },
    { "vkGetPhysicalDeviceImageFormatProperties2",     (PFN_vkVoidFunction)x_image_props2, 0 },
    { "vkGetPhysicalDeviceImageFormatProperties2KHR",  (PFN_vkVoidFunction)x_image_props2, 0 },
    { "vkGetPhysicalDeviceExternalBufferProperties",   (PFN_vkVoidFunction)x_buffer_props, 0 },
    { "vkGetPhysicalDeviceExternalBufferPropertiesKHR",(PFN_vkVoidFunction)x_buffer_props, 0 },
    { "vkAllocateMemory",                              (PFN_vkVoidFunction)x_allocate_memory, 0 },
    { "vkFreeMemory",                                  (PFN_vkVoidFunction)x_free_memory, 0 },
    { "vkCreateImage",                                 (PFN_vkVoidFunction)x_create_image, 0 },
    { "vkCreateBuffer",                                (PFN_vkVoidFunction)x_create_buffer, 0 },
    { "vkCreateSemaphore",                             (PFN_vkVoidFunction)x_create_semaphore, 2 },
    { "vkDestroySemaphore",                            (PFN_vkVoidFunction)x_destroy_semaphore, 2 },
    { "vkGetPhysicalDeviceExternalSemaphoreProperties",   (PFN_vkVoidFunction)x_sem_props, 2 },
    { "vkGetPhysicalDeviceExternalSemaphorePropertiesKHR",(PFN_vkVoidFunction)x_sem_props, 2 },
    { "vkGetSemaphoreWin32HandleKHR",                  (PFN_vkVoidFunction)x_get_semaphore_win32_handle, 3 },
    { "vkImportSemaphoreWin32HandleKHR",               (PFN_vkVoidFunction)x_import_semaphore_win32_handle, 3 },
    { "vkGetMemoryWin32HandleKHR",                     (PFN_vkVoidFunction)x_get_memory_win32_handle, 1 },
    { "vkGetMemoryWin32HandlePropertiesKHR",           (PFN_vkVoidFunction)x_get_memory_win32_handle_props, 1 },
};

/* Called once, with the first instance the driver made: keeps the
 * driver's commands and points the loader's exports at the ones here */
void shared_init(void *instance, PFN_gipa gipa)
{
    r_create_device = (PFN_create_device)gipa(instance, "vkCreateDevice");
    r_enum_dev_ext = (PFN_enum_dev_ext)gipa(instance, "vkEnumerateDeviceExtensionProperties");
    r_image_props2 = (PFN_image_props2)gipa(instance, "vkGetPhysicalDeviceImageFormatProperties2");
    if (!r_image_props2) r_image_props2 = (PFN_image_props2)gipa(instance, "vkGetPhysicalDeviceImageFormatProperties2KHR");
    r_buffer_props = (PFN_buffer_props)gipa(instance, "vkGetPhysicalDeviceExternalBufferProperties");
    if (!r_buffer_props) r_buffer_props = (PFN_buffer_props)gipa(instance, "vkGetPhysicalDeviceExternalBufferPropertiesKHR");
    r_create_image = (PFN_create_object)gipa(instance, "vkCreateImage");
    r_create_buffer = (PFN_create_object)gipa(instance, "vkCreateBuffer");
    r_alloc = (PFN_alloc)gipa(instance, "vkAllocateMemory");
    r_free = (PFN_free)gipa(instance, "vkFreeMemory");
    r_host_props = (PFN_host_props)gipa(instance, "vkGetMemoryHostPointerPropertiesEXT");
    r_sem_props = (PFN_sem_props)gipa(instance, "vkGetPhysicalDeviceExternalSemaphoreProperties");
    if (!r_sem_props) r_sem_props = (PFN_sem_props)gipa(instance, "vkGetPhysicalDeviceExternalSemaphorePropertiesKHR");
    r_create_semaphore = (PFN_create_object)gipa(instance, "vkCreateSemaphore");
    r_destroy_semaphore = (PFN_free)gipa(instance, "vkDestroySemaphore");
    r_counter = (PFN_counter)gipa(instance, "vkGetSemaphoreCounterValue");
    if (!r_counter) r_counter = (PFN_counter)gipa(instance, "vkGetSemaphoreCounterValueKHR");
    r_signal = (PFN_signal)gipa(instance, "vkSignalSemaphore");
    if (!r_signal) r_signal = (PFN_signal)gipa(instance, "vkSignalSemaphoreKHR");
    if (!r_create_device || !r_enum_dev_ext || !r_image_props2 || !r_buffer_props || !r_create_image ||
        !r_create_buffer || !r_alloc || !r_free) {
        g_emulate = 0;                                      /* (an incomplete driver: left alone) */
        return;
    }
    p_vkCreateDevice = (void *)x_create_device;
    p_vkEnumerateDeviceExtensionProperties = (void *)x_enum_dev_ext;
    p_vkGetPhysicalDeviceImageFormatProperties2 = (void *)x_image_props2;
    p_vkGetPhysicalDeviceExternalBufferProperties = (void *)x_buffer_props;
    p_vkAllocateMemory = (void *)x_allocate_memory;
    p_vkFreeMemory = (void *)x_free_memory;
    p_vkCreateImage = (void *)x_create_image;
    p_vkCreateBuffer = (void *)x_create_buffer;
    if (r_sem_props && r_create_semaphore && r_destroy_semaphore && r_counter && r_signal) {
        p_vkCreateSemaphore = (void *)x_create_semaphore;
        p_vkDestroySemaphore = (void *)x_destroy_semaphore;
        p_vkGetPhysicalDeviceExternalSemaphoreProperties = (void *)x_sem_props;
    }
}

/* The command @name if the loader answers it for the driver, else NULL */
PFN_vkVoidFunction shared_proc(const char *name)
{
    if (g_emulate == 0 || !r_alloc) return NULL;
    int sems = r_sem_props && r_create_semaphore && r_destroy_semaphore && r_counter && r_signal;
    for (UINT i = 0; i < sizeof(g_own) / sizeof(g_own[0]); i++)
        if (streq(name, g_own[i].name))
            return ((g_own[i].flags & 1) && g_emulate <= 0) || ((g_own[i].flags & 2) && !sems) ? NULL : g_own[i].fn;
    return NULL;
}
