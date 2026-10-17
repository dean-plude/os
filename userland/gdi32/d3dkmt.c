/*
 * d3dkmt.c — gdi32's D3DKMT* thunks, the user-mode door to the WDDM
 * graphics kernel (adapters, devices, allocations, shared resources and
 * sync objects).  On Windows they call win32u's NtGdiDdDDI* system calls;
 * here each passes its D3DKMT_* structure, as Windows lays it out, to the
 * kernel's NtNovaDxgkCtl (kernel/um/um_d3dkmt.c, step 3 of
 * docs/design/intel-gpu.md).  The kernel lists NovaOS's Basic Render
 * adapter, the software adapter Windows always has, and gives its
 * allocations memory, maps them for the CPU, and keeps its fences.
 * 32-bit programs, whose structures the kernel does not translate yet,
 * get STATUS_NOT_SUPPORTED, as Windows answers where the kernel graphics
 * subsystem is absent; so do the calls not implemented yet.  VKD3D-Proton
 * 3.0 imports them (libs/vkd3d/d3dkmt.c: it opens the adapter by LUID to
 * share resources and fences across APIs, and carries on without when
 * that fails).
 */
#define NOVA_BUILD_GDI32
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>

#define STATUS_NOT_SUPPORTED ((LONG)0xC00000BB)

#define KMT(name) GDIAPI LONG name(void *args) { (void)args; return STATUS_NOT_SUPPORTED; }

KMT(D3DKMTOpenAdapterFromHdc)
KMT(D3DKMTOpenAdapterFromGdiDisplayName)
KMT(D3DKMTDestroyKeyedMutex)
KMT(D3DKMTEscape)
KMT(D3DKMTOpenResource2)
KMT(D3DKMTOpenResourceFromNtHandle)
KMT(D3DKMTOpenSyncObjectFromNtHandle)
KMT(D3DKMTQueryResourceInfo)
KMT(D3DKMTQueryResourceInfoFromNtHandle)
KMT(D3DKMTShareObjects)

/* NtNovaDxgkCtl's operations, in this order */
enum {
    OP_ENUM_ADAPTERS2, OP_OPEN_ADAPTER_FROM_LUID, OP_CLOSE_ADAPTER, OP_QUERY_ADAPTER_INFO,
    OP_CREATE_DEVICE, OP_DESTROY_DEVICE, OP_CREATE_ALLOCATION2, OP_DESTROY_ALLOCATION2,
    OP_LOCK2, OP_UNLOCK2, OP_CREATE_SYNC2, OP_DESTROY_SYNC, OP_SIGNAL_FROM_CPU, OP_WAIT_FROM_CPU,
    OP_QUERY_VIDEO_MEMORY_INFO,
};

#ifdef _WIN64
#define CALL(op, args) return args ? NtNovaDxgkCtl(op, (void *)(args)) : (LONG)0xC000000D   /* INVALID_PARAMETER */
#else
#define CALL(op, args) do { (void)(args); return STATUS_NOT_SUPPORTED; } while (0)
#endif

GDIAPI LONG D3DKMTEnumAdapters2(D3DKMT_ENUMADAPTERS2 *a)                 { CALL(OP_ENUM_ADAPTERS2, a); }
GDIAPI LONG D3DKMTOpenAdapterFromLuid(D3DKMT_OPENADAPTERFROMLUID *a)     { CALL(OP_OPEN_ADAPTER_FROM_LUID, a); }
GDIAPI LONG D3DKMTCloseAdapter(const D3DKMT_CLOSEADAPTER *a)             { CALL(OP_CLOSE_ADAPTER, a); }
GDIAPI LONG D3DKMTQueryAdapterInfo(const D3DKMT_QUERYADAPTERINFO *a)     { CALL(OP_QUERY_ADAPTER_INFO, a); }
GDIAPI LONG D3DKMTCreateDevice(D3DKMT_CREATEDEVICE *a)                   { CALL(OP_CREATE_DEVICE, a); }
GDIAPI LONG D3DKMTDestroyDevice(const D3DKMT_DESTROYDEVICE *a)           { CALL(OP_DESTROY_DEVICE, a); }
GDIAPI LONG D3DKMTCreateAllocation2(D3DKMT_CREATEALLOCATION *a)          { CALL(OP_CREATE_ALLOCATION2, a); }
GDIAPI LONG D3DKMTDestroyAllocation2(const D3DKMT_DESTROYALLOCATION2 *a) { CALL(OP_DESTROY_ALLOCATION2, a); }
GDIAPI LONG D3DKMTLock2(D3DKMT_LOCK2 *a)                                 { CALL(OP_LOCK2, a); }
GDIAPI LONG D3DKMTUnlock2(const D3DKMT_UNLOCK2 *a)                       { CALL(OP_UNLOCK2, a); }
GDIAPI LONG D3DKMTDestroySynchronizationObject(const D3DKMT_DESTROYSYNCHRONIZATIONOBJECT *a) { CALL(OP_DESTROY_SYNC, a); }
GDIAPI LONG D3DKMTQueryVideoMemoryInfo(D3DKMT_QUERYVIDEOMEMORYINFO *a)   { CALL(OP_QUERY_VIDEO_MEMORY_INFO, a); }

GDIAPI LONG D3DKMTCreateSynchronizationObject2(D3DKMT_CREATESYNCHRONIZATIONOBJECT2 *a)
{
    CALL(OP_CREATE_SYNC2, a);
}

GDIAPI LONG D3DKMTSignalSynchronizationObjectFromCpu(const D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU *a)
{
    CALL(OP_SIGNAL_FROM_CPU, a);
}

GDIAPI LONG D3DKMTWaitForSynchronizationObjectFromCpu(const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU *a)
{
    CALL(OP_WAIT_FROM_CPU, a);
}

/* The first version: the same structure without Flags */
typedef struct {
    D3DKMT_HANDLE hDevice, hResource;
    const D3DKMT_HANDLE *phAllocationList;
    UINT AllocationCount;
} D3DKMT_DESTROYALLOCATION;

GDIAPI LONG D3DKMTDestroyAllocation(const D3DKMT_DESTROYALLOCATION *a)
{
    if (!a) return (LONG)0xC000000D;
    D3DKMT_DESTROYALLOCATION2 v2 = { a->hDevice, a->hResource, a->phAllocationList, a->AllocationCount, 0 };
    return D3DKMTDestroyAllocation2(&v2);
}

/* A process's GPU scheduling priority (OBS raises its own to keep encoding
 * while a game loads the GPU): with no GPU scheduler there is nothing to
 * reorder, so the class is kept and reported back and the call succeeds */
static LONG g_sched_class;

GDIAPI LONG D3DKMTSetProcessSchedulingPriorityClass(HANDLE process, LONG cls)
{
    (void)process;
    if (cls < 0 || cls > 5) return (LONG)0xC000000D;    /* STATUS_INVALID_PARAMETER */
    g_sched_class = cls;
    return 0;
}

GDIAPI LONG D3DKMTGetProcessSchedulingPriorityClass(HANDLE process, LONG *cls)
{
    (void)process;
    if (!cls) return (LONG)0xC000000D;
    *cls = g_sched_class;
    return 0;
}
