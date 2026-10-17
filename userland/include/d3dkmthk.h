/*
 * d3dkmthk.h — the D3DKMT* interface: gdi32's door to the WDDM graphics
 * kernel (NovaOS's is kernel/um/um_d3dkmt.c)
 *
 * The structures are Windows', written from the WDK's public documentation
 * of d3dkmthk.h and d3dukmdt.h (learn.microsoft.com), only the ones NovaOS
 * implements.  On x64 the kernel reads them as they are (abitest-style
 * offsets are checked in the kernel's copy); 32-bit programs get
 * STATUS_NOT_SUPPORTED for now.
 */
#pragma once
#include <windows.h>

typedef UINT D3DKMT_HANDLE;
typedef UINT64 D3DGPU_VIRTUAL_ADDRESS;
typedef UINT D3DDDI_VIDEO_PRESENT_SOURCE_ID;

typedef struct _D3DKMT_ADAPTERINFO {
    D3DKMT_HANDLE hAdapter;
    LUID          AdapterLuid;
    ULONG         NumOfSources;
    BOOL          bPrecisePresentRegionsPreferred;
} D3DKMT_ADAPTERINFO;

typedef struct _D3DKMT_ENUMADAPTERS2 {
    ULONG               NumAdapters;
    D3DKMT_ADAPTERINFO *pAdapters;
} D3DKMT_ENUMADAPTERS2;

typedef struct _D3DKMT_OPENADAPTERFROMLUID {
    LUID          AdapterLuid;
    D3DKMT_HANDLE hAdapter;
} D3DKMT_OPENADAPTERFROMLUID;

typedef struct _D3DKMT_CLOSEADAPTER {
    D3DKMT_HANDLE hAdapter;
} D3DKMT_CLOSEADAPTER;

typedef enum _KMTQUERYADAPTERINFOTYPE {
    KMTQAITYPE_UMDRIVERPRIVATE = 0,
    KMTQAITYPE_UMDRIVERNAME = 1,
    KMTQAITYPE_GETSEGMENTSIZE = 3,
    KMTQAITYPE_ADAPTERGUID = 4,
    KMTQAITYPE_ADAPTERADDRESS = 6,
    KMTQAITYPE_DRIVERVERSION = 13,
    KMTQAITYPE_ADAPTERTYPE = 15,
    KMTQAITYPE_WDDM_2_0_CAPS = 24,
    KMTQAITYPE_QUERYREGISTRY = 48,
} KMTQUERYADAPTERINFOTYPE;

typedef struct _D3DKMT_QUERYADAPTERINFO {
    D3DKMT_HANDLE           hAdapter;
    KMTQUERYADAPTERINFOTYPE Type;
    VOID                   *pPrivateDriverData;
    UINT                    PrivateDriverDataSize;
} D3DKMT_QUERYADAPTERINFO;

/* KMTQAITYPE_ADAPTERTYPE: bit fields */
#define D3DKMT_ADAPTERTYPE_RENDER_SUPPORTED   0x1
#define D3DKMT_ADAPTERTYPE_DISPLAY_SUPPORTED  0x2
#define D3DKMT_ADAPTERTYPE_SOFTWARE_DEVICE    0x4
typedef struct _D3DKMT_ADAPTERTYPE { UINT Value; } D3DKMT_ADAPTERTYPE;

/* KMTQAITYPE_DRIVERVERSION */
typedef enum _D3DKMT_DRIVERVERSION {
    KMT_DRIVERVERSION_WDDM_1_0 = 1000,
    KMT_DRIVERVERSION_WDDM_2_0 = 2000,
    KMT_DRIVERVERSION_WDDM_2_7 = 2700,
    KMT_DRIVERVERSION_WDDM_3_0 = 3000,
} D3DKMT_DRIVERVERSION;

/* KMTQAITYPE_GETSEGMENTSIZE */
typedef struct _D3DKMT_SEGMENTSIZEINFO {
    ULONGLONG DedicatedVideoMemorySize;
    ULONGLONG DedicatedSystemMemorySize;
    ULONGLONG SharedSystemMemorySize;
} D3DKMT_SEGMENTSIZEINFO;

typedef struct _D3DKMT_CREATEDEVICE {
    union { D3DKMT_HANDLE hAdapter; VOID *pAdapter; };
    UINT          Flags;                          /* D3DKMT_CREATEDEVICEFLAGS */
    D3DKMT_HANDLE hDevice;
    VOID         *pCommandBuffer;                 /* (WDDM 1.x only; 0 here) */
    UINT          CommandBufferSize;
    VOID         *pAllocationList;
    UINT          AllocationListSize;
    VOID         *pPatchLocationList;
    UINT          PatchLocationListSize;
} D3DKMT_CREATEDEVICE;

typedef struct _D3DKMT_DESTROYDEVICE {
    D3DKMT_HANDLE hDevice;
} D3DKMT_DESTROYDEVICE;

typedef struct _D3DDDI_ALLOCATIONINFO2 {
    D3DKMT_HANDLE hAllocation;                    /* out */
    union { HANDLE hSection; const VOID *pSystemMem; };
    const VOID   *pPrivateDriverData;
    UINT          PrivateDriverDataSize;
    D3DDDI_VIDEO_PRESENT_SOURCE_ID VidPnSourceId;
    UINT          Flags;                          /* bit 0 Primary, bit 1 Stereo, bit 2 OverridePriority */
    D3DGPU_VIRTUAL_ADDRESS GpuVirtualAddress;     /* out */
    union { ULONG Priority; ULONG_PTR Unused; };
    ULONG_PTR     Reserved[5];
} D3DDDI_ALLOCATIONINFO2;

typedef struct _D3DKMT_CREATEALLOCATION {
    D3DKMT_HANDLE hDevice;
    D3DKMT_HANDLE hResource;
    D3DKMT_HANDLE hGlobalShare;
    const VOID   *pPrivateRuntimeData;
    UINT          PrivateRuntimeDataSize;
    union { VOID *pStandardAllocation; const VOID *pPrivateDriverData; };
    UINT          PrivateDriverDataSize;
    UINT          NumAllocations;
    union { VOID *pAllocationInfo; D3DDDI_ALLOCATIONINFO2 *pAllocationInfo2; };
    UINT          Flags;                          /* D3DKMT_CREATEALLOCATIONFLAGS */
    HANDLE        hPrivateRuntimeResourceHandle;
} D3DKMT_CREATEALLOCATION;

/* What an allocation on NovaOS's Basic Render adapter is: its private
 * driver data (each adapter's user- and kernel-mode drivers agree on
 * their own; this is the built-in adapter's) */
typedef struct _NOVA_KMT_ALLOCATION {
    UINT64 Size;                                  /* bytes */
    UINT   Flags;                                 /* none yet */
    UINT   Reserved;
} NOVA_KMT_ALLOCATION;

typedef struct _D3DKMT_DESTROYALLOCATION2 {
    D3DKMT_HANDLE        hDevice;
    D3DKMT_HANDLE        hResource;
    const D3DKMT_HANDLE *phAllocationList;
    UINT                 AllocationCount;
    UINT                 Flags;                   /* D3DDDICB_DESTROYALLOCATION2FLAGS */
} D3DKMT_DESTROYALLOCATION2;

typedef struct _D3DKMT_LOCK2 {
    D3DKMT_HANDLE hDevice;
    D3DKMT_HANDLE hAllocation;
    UINT          Flags;                          /* D3DDDICB_LOCK2FLAGS */
    VOID         *pData;                          /* out */
} D3DKMT_LOCK2;

typedef struct _D3DKMT_UNLOCK2 {
    D3DKMT_HANDLE hDevice;
    D3DKMT_HANDLE hAllocation;
} D3DKMT_UNLOCK2;

typedef enum _D3DDDI_SYNCHRONIZATIONOBJECT_TYPE {
    D3DDDI_SYNCHRONIZATION_MUTEX = 1,
    D3DDDI_SEMAPHORE = 2,
    D3DDDI_FENCE = 3,
    D3DDDI_CPU_NOTIFICATION = 4,
    D3DDDI_MONITORED_FENCE = 5,
    D3DDDI_PERIODIC_MONITORED_FENCE = 6,
} D3DDDI_SYNCHRONIZATIONOBJECT_TYPE;

typedef struct _D3DDDI_SYNCHRONIZATIONOBJECTINFO2 {
    D3DDDI_SYNCHRONIZATIONOBJECT_TYPE Type;
    UINT Flags;                                   /* D3DDDI_SYNCHRONIZATIONOBJECT_FLAGS */
    union {
        struct { BOOL InitialState; } SynchronizationMutex;
        struct { UINT MaxCount; UINT InitialCount; } Semaphore;
        struct { UINT64 FenceValue; } Fence;
        struct { HANDLE Event; } CPUNotification;
        struct {
            UINT64                 InitialFenceValue;
            VOID                  *FenceValueCPUVirtualAddress;   /* out: the value, read-only */
            D3DGPU_VIRTUAL_ADDRESS FenceValueGPUVirtualAddress;   /* out */
            UINT                   EngineAffinity;
        } MonitoredFence;
        struct { UINT64 Reserved[8]; } Reserved;
    };
    D3DKMT_HANDLE SharedHandle;
} D3DDDI_SYNCHRONIZATIONOBJECTINFO2;

typedef struct _D3DKMT_CREATESYNCHRONIZATIONOBJECT2 {
    D3DKMT_HANDLE                     hDevice;
    D3DDDI_SYNCHRONIZATIONOBJECTINFO2 Info;
    D3DKMT_HANDLE                     hSyncObject;    /* out */
} D3DKMT_CREATESYNCHRONIZATIONOBJECT2;

typedef struct _D3DKMT_DESTROYSYNCHRONIZATIONOBJECT {
    D3DKMT_HANDLE hSyncObject;
} D3DKMT_DESTROYSYNCHRONIZATIONOBJECT;

typedef struct _D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU {
    D3DKMT_HANDLE        hDevice;
    UINT                 ObjectCount;
    const D3DKMT_HANDLE *ObjectHandleArray;
    const UINT64        *FenceValueArray;
    UINT                 Flags;                   /* D3DDDICB_SIGNALFLAGS */
} D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU;

typedef struct _D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU {
    D3DKMT_HANDLE        hDevice;
    UINT                 ObjectCount;
    const D3DKMT_HANDLE *ObjectHandleArray;
    const UINT64        *FenceValueArray;
    HANDLE               hAsyncEvent;             /* NULL: wait here */
    UINT                 Flags;                   /* bit 0 WaitAny */
} D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU;

typedef enum _D3DKMT_MEMORY_SEGMENT_GROUP {
    D3DKMT_MEMORY_SEGMENT_GROUP_LOCAL = 0,
    D3DKMT_MEMORY_SEGMENT_GROUP_NON_LOCAL = 1,
} D3DKMT_MEMORY_SEGMENT_GROUP;

typedef struct _D3DKMT_QUERYVIDEOMEMORYINFO {
    HANDLE                      hProcess;         /* NULL: the caller */
    D3DKMT_HANDLE               hAdapter;
    D3DKMT_MEMORY_SEGMENT_GROUP MemorySegmentGroup;
    UINT64                      Budget;
    UINT64                      CurrentUsage;
    UINT64                      CurrentReservation;
    UINT64                      AvailableForReservation;
    UINT                        PhysicalAdapterIndex;
} D3DKMT_QUERYVIDEOMEMORYINFO;

#ifndef NOVA_BUILD_GDI32
#define KMTAPI __declspec(dllimport) LONG __stdcall
KMTAPI D3DKMTEnumAdapters2(D3DKMT_ENUMADAPTERS2 *);
KMTAPI D3DKMTOpenAdapterFromLuid(D3DKMT_OPENADAPTERFROMLUID *);
KMTAPI D3DKMTCloseAdapter(const D3DKMT_CLOSEADAPTER *);
KMTAPI D3DKMTQueryAdapterInfo(const D3DKMT_QUERYADAPTERINFO *);
KMTAPI D3DKMTCreateDevice(D3DKMT_CREATEDEVICE *);
KMTAPI D3DKMTDestroyDevice(const D3DKMT_DESTROYDEVICE *);
KMTAPI D3DKMTCreateAllocation2(D3DKMT_CREATEALLOCATION *);
KMTAPI D3DKMTDestroyAllocation2(const D3DKMT_DESTROYALLOCATION2 *);
KMTAPI D3DKMTLock2(D3DKMT_LOCK2 *);
KMTAPI D3DKMTUnlock2(const D3DKMT_UNLOCK2 *);
KMTAPI D3DKMTCreateSynchronizationObject2(D3DKMT_CREATESYNCHRONIZATIONOBJECT2 *);
KMTAPI D3DKMTDestroySynchronizationObject(const D3DKMT_DESTROYSYNCHRONIZATIONOBJECT *);
KMTAPI D3DKMTSignalSynchronizationObjectFromCpu(const D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU *);
KMTAPI D3DKMTWaitForSynchronizationObjectFromCpu(const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU *);
KMTAPI D3DKMTQueryVideoMemoryInfo(D3DKMT_QUERYVIDEOMEMORYINFO *);
#undef KMTAPI
#endif
