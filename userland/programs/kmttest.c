/*
 * kmttest.exe — the D3DKMT* calls on NovaOS's Basic Render adapter (step 3
 * of docs/design/intel-gpu.md, its first part: kernel/um/um_d3dkmt.c):
 *
 *   adapters      D3DKMTEnumAdapters2 lists the adapter with its LUID;
 *                 D3DKMTOpenAdapterFromLuid finds it again and refuses an
 *                 unknown LUID; D3DKMTQueryAdapterInfo says render-only,
 *                 software, WDDM 2.0, and its shared memory
 *   allocations   D3DKMTCreateAllocation2 gives memory the video memory
 *                 counts (D3DKMTQueryVideoMemoryInfo); D3DKMTLock2 maps it
 *                 read-write and a second lock sees what the first wrote;
 *                 too big is refused; D3DKMTDestroyAllocation2 gives it back
 *   fences        a monitored fence's value is on a read-only page;
 *                 D3DKMTSignalSynchronizationObjectFromCpu raises it (never
 *                 lowers it); D3DKMTWaitForSynchronizationObjectFromCpu
 *                 returns at once when reached, sets an event later when not,
 *                 and holds another thread until the value comes
 *   handles       a bad or closed handle is refused; destroying the device
 *                 takes its fences with it
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <d3dkmthk.h>

#define ST_INVALID_PARAMETER ((LONG)0xC000000D)

static int g_pass, g_fail;

static void check(int ok, const char *what)
{
    if (ok) g_pass++;
    else { g_fail++; printf("FAIL: %s\n", what); }
}

static D3DKMT_HANDLE g_dev, g_fence;

static DWORD WINAPI waiter(void *arg)
{
    UINT64 v = (UINT64)(ULONG_PTR)arg;
    D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU w = { g_dev, 1, &g_fence, &v, NULL, 0 };
    return (DWORD)D3DKMTWaitForSynchronizationObjectFromCpu(&w);
}

static D3DKMT_HANDLE allocate(D3DKMT_HANDLE dev, UINT64 size, LONG *st)
{
    NOVA_KMT_ALLOCATION spec = { size, 0, 0 };
    D3DDDI_ALLOCATIONINFO2 info;
    memset(&info, 0, sizeof(info));
    info.pPrivateDriverData = &spec;
    info.PrivateDriverDataSize = sizeof(spec);
    D3DKMT_CREATEALLOCATION ca;
    memset(&ca, 0, sizeof(ca));
    ca.hDevice = dev;
    ca.NumAllocations = 1;
    ca.pAllocationInfo2 = &info;
    *st = D3DKMTCreateAllocation2(&ca);
    return *st ? 0 : info.hAllocation;
}

static UINT64 usage(D3DKMT_HANDLE adapter)
{
    D3DKMT_QUERYVIDEOMEMORYINFO q;
    memset(&q, 0, sizeof(q));
    q.hAdapter = adapter;
    return D3DKMTQueryVideoMemoryInfo(&q) ? (UINT64)-1 : q.CurrentUsage;
}

int main(void)
{
    LONG st;

    /* ---- adapters ---- */
    D3DKMT_ENUMADAPTERS2 en = { 0, NULL };
    check(D3DKMTEnumAdapters2(&en) == 0 && en.NumAdapters >= 1, "EnumAdapters2 counts the adapters");
    D3DKMT_ADAPTERINFO info[4];
    en.NumAdapters = 4;
    en.pAdapters = info;
    st = D3DKMTEnumAdapters2(&en);
    check(st == 0 && en.NumAdapters >= 1 && info[0].hAdapter, "EnumAdapters2 lists and opens them");
    if (st || !en.NumAdapters) { printf("kmttest: no adapter (%08lx)\n", (unsigned long)st); return 1; }
    D3DKMT_HANDLE adapter = info[0].hAdapter;
    LUID luid = info[0].AdapterLuid;
    printf("kmttest: adapter LUID %08lx:%08lx\n", (unsigned long)luid.HighPart, (unsigned long)luid.LowPart);

    D3DKMT_ADAPTERTYPE type = { 0 };
    D3DKMT_QUERYADAPTERINFO qa = { adapter, KMTQAITYPE_ADAPTERTYPE, &type, sizeof(type) };
    check(D3DKMTQueryAdapterInfo(&qa) == 0 && (type.Value & D3DKMT_ADAPTERTYPE_RENDER_SUPPORTED) &&
          (type.Value & D3DKMT_ADAPTERTYPE_SOFTWARE_DEVICE) && !(type.Value & D3DKMT_ADAPTERTYPE_DISPLAY_SUPPORTED),
          "the adapter is a render-only software one (Basic Render)");
    D3DKMT_DRIVERVERSION ver = 0;
    qa.Type = KMTQAITYPE_DRIVERVERSION; qa.pPrivateDriverData = &ver; qa.PrivateDriverDataSize = sizeof(ver);
    check(D3DKMTQueryAdapterInfo(&qa) == 0 && ver >= KMT_DRIVERVERSION_WDDM_2_0, "its driver is WDDM 2.0 or later");
    D3DKMT_SEGMENTSIZEINFO seg;
    memset(&seg, 0, sizeof(seg));
    qa.Type = KMTQAITYPE_GETSEGMENTSIZE; qa.pPrivateDriverData = &seg; qa.PrivateDriverDataSize = sizeof(seg);
    check(D3DKMTQueryAdapterInfo(&qa) == 0 && seg.DedicatedVideoMemorySize == 0 && seg.SharedSystemMemorySize >= (16u << 20),
          "its memory is shared system memory, no VRAM");
    printf("kmttest: %llu MB of shared memory\n", (unsigned long long)(seg.SharedSystemMemorySize >> 20));
    qa.PrivateDriverDataSize = 4;
    check(D3DKMTQueryAdapterInfo(&qa) == ST_INVALID_PARAMETER, "a query with the wrong size is refused");

    D3DKMT_OPENADAPTERFROMLUID ol = { luid, 0 };
    check(D3DKMTOpenAdapterFromLuid(&ol) == 0 && ol.hAdapter && ol.hAdapter != adapter, "OpenAdapterFromLuid finds it");
    D3DKMT_CLOSEADAPTER close = { ol.hAdapter };
    check(D3DKMTCloseAdapter(&close) == 0, "CloseAdapter");
    check(D3DKMTCloseAdapter(&close) == ST_INVALID_PARAMETER, "a closed adapter handle is refused");
    D3DKMT_OPENADAPTERFROMLUID bad = { { luid.LowPart + 1, luid.HighPart }, 0 };
    check(D3DKMTOpenAdapterFromLuid(&bad) != 0, "an unknown LUID is refused");

    D3DKMT_CREATEDEVICE cd;
    memset(&cd, 0, sizeof(cd));
    cd.hAdapter = adapter;
    check(D3DKMTCreateDevice(&cd) == 0 && cd.hDevice, "CreateDevice");
    g_dev = cd.hDevice;

    /* ---- allocations ---- */
    UINT64 before = usage(adapter);
    D3DKMT_HANDLE a1 = allocate(g_dev, 65536, &st);
    check(a1 != 0, "CreateAllocation2, 64 KiB");
    D3DKMT_HANDLE a2 = allocate(g_dev, (1u << 20) + 1, &st);
    check(a2 != 0 && a2 != a1, "CreateAllocation2, 1 MiB and a byte");
    check(usage(adapter) == before + 65536 + (1u << 20) + 4096, "the video memory counts them (whole pages)");
    allocate(g_dev, 0x80000000ull, &st);
    check(st != 0, "an allocation past the largest is refused");

    D3DKMT_LOCK2 lk = { g_dev, a2, 0, NULL };
    check(D3DKMTLock2(&lk) == 0 && lk.pData, "Lock2 maps it");
    unsigned char *p = lk.pData;
    MEMORY_BASIC_INFORMATION mbi;
    check(p && VirtualQuery(p, &mbi, sizeof(mbi)) && mbi.Protect == PAGE_READWRITE && mbi.RegionSize >= (1u << 20),
          "the mapping is read-write and covers it");
    int zero = 1;
    for (int i = 0; p && i < (1 << 20); i += 4096) zero &= p[i] == 0;
    check(zero, "a new allocation is zeroed");
    if (p) { memset(p, 0x5A, 1u << 20); p[(1u << 20)] = 0xA5; }
    D3DKMT_LOCK2 lk2 = { g_dev, a2, 0, NULL };
    check(D3DKMTLock2(&lk2) == 0 && lk2.pData == lk.pData, "a second lock gets the same mapping");
    D3DKMT_UNLOCK2 ul = { g_dev, a2 };
    check(D3DKMTUnlock2(&ul) == 0 && D3DKMTUnlock2(&ul) == 0, "Unlock2, twice");
    check(D3DKMTUnlock2(&ul) == ST_INVALID_PARAMETER, "an unlocked allocation cannot be unlocked");
    lk.pData = NULL;
    check(D3DKMTLock2(&lk) == 0 && lk.pData, "Lock2 again");
    p = lk.pData;
    check(p && p[0] == 0x5A && p[(1u << 20) - 1] == 0x5A && p[1u << 20] == 0xA5, "it kept what was written");
    D3DKMT_DESTROYALLOCATION2 da = { g_dev, 0, &a2, 1, 0 };
    check(D3DKMTDestroyAllocation2(&da) == 0, "DestroyAllocation2 (while locked)");
    check(!p || !VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State == MEM_FREE, "its mapping went with it");
    check(D3DKMTDestroyAllocation2(&da) == ST_INVALID_PARAMETER, "a destroyed allocation's handle is refused");
    check(usage(adapter) == before + 65536, "the video memory gives it back");

    /* ---- fences ---- */
    D3DKMT_CREATESYNCHRONIZATIONOBJECT2 cs;
    memset(&cs, 0, sizeof(cs));
    cs.hDevice = g_dev;
    cs.Info.Type = D3DDDI_MONITORED_FENCE;
    cs.Info.MonitoredFence.InitialFenceValue = 5;
    check(D3DKMTCreateSynchronizationObject2(&cs) == 0 && cs.hSyncObject, "CreateSynchronizationObject2, a monitored fence");
    g_fence = cs.hSyncObject;
    volatile UINT64 *cpu = cs.Info.MonitoredFence.FenceValueCPUVirtualAddress;
    check(cpu && *cpu == 5, "its value is at FenceValueCPUVirtualAddress");
    check(cpu && VirtualQuery((void *)cpu, &mbi, sizeof(mbi)) && mbi.Protect == PAGE_READONLY, "read-only there");

    UINT64 v = 10;
    D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU sg = { g_dev, 1, &g_fence, &v, 0 };
    check(D3DKMTSignalSynchronizationObjectFromCpu(&sg) == 0 && cpu && *cpu == 10, "SignalFromCpu raises it");
    v = 3;
    check(D3DKMTSignalSynchronizationObjectFromCpu(&sg) == 0 && cpu && *cpu == 10, "a lower value leaves it");
    v = 10;
    D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wt = { g_dev, 1, &g_fence, &v, NULL, 0 };
    check(D3DKMTWaitForSynchronizationObjectFromCpu(&wt) == 0, "a wait for a reached value returns");

    HANDLE ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    v = 20;
    wt.hAsyncEvent = ev;
    check(D3DKMTWaitForSynchronizationObjectFromCpu(&wt) == 0, "a wait with an event returns at once");
    check(WaitForSingleObject(ev, 50) == WAIT_TIMEOUT, "its event waits for the value");
    v = 20;
    check(D3DKMTSignalSynchronizationObjectFromCpu(&sg) == 0 && WaitForSingleObject(ev, 1000) == WAIT_OBJECT_0,
          "signalling the value sets the event");
    CloseHandle(ev);

    HANDLE th = CreateThread(NULL, 0, waiter, (void *)(ULONG_PTR)30, 0, NULL);
    check(WaitForSingleObject(th, 100) == WAIT_TIMEOUT, "another thread waits for 30");
    v = 30;
    D3DKMTSignalSynchronizationObjectFromCpu(&sg);
    DWORD code = 1;
    check(WaitForSingleObject(th, 5000) == WAIT_OBJECT_0 && GetExitCodeThread(th, &code) && code == 0,
          "and goes on when it is signalled");
    CloseHandle(th);

    D3DKMT_CREATESYNCHRONIZATIONOBJECT2 cs2;
    memset(&cs2, 0, sizeof(cs2));
    cs2.hDevice = g_dev;
    cs2.Info.Type = D3DDDI_FENCE;
    cs2.Info.Fence.FenceValue = 1;
    check(D3DKMTCreateSynchronizationObject2(&cs2) == 0 && cs2.hSyncObject, "CreateSynchronizationObject2, a fence");
    D3DKMT_HANDLE two[2] = { g_fence, cs2.hSyncObject };
    UINT64 vals[2] = { 30, 2 };
    D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU any = { g_dev, 2, two, vals, NULL, 1 };
    check(D3DKMTWaitForSynchronizationObjectFromCpu(&any) == 0, "WaitAny returns when one is reached");
    ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU all = { g_dev, 2, two, vals, ev, 0 };
    check(D3DKMTWaitForSynchronizationObjectFromCpu(&all) == 0 && WaitForSingleObject(ev, 50) == WAIT_TIMEOUT,
          "waiting for all of them waits for the other");
    D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU sg2 = { g_dev, 1, &two[1], &vals[1], 0 };
    check(D3DKMTSignalSynchronizationObjectFromCpu(&sg2) == 0 && WaitForSingleObject(ev, 1000) == WAIT_OBJECT_0,
          "until it is reached too");
    CloseHandle(ev);

    D3DKMT_CREATESYNCHRONIZATIONOBJECT2 mx;
    memset(&mx, 0, sizeof(mx));
    mx.hDevice = g_dev;
    mx.Info.Type = D3DDDI_SYNCHRONIZATION_MUTEX;
    check(D3DKMTCreateSynchronizationObject2(&mx) != 0, "the kinds not done yet are refused");
    D3DKMT_HANDLE junk = 0x12345;
    D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU sj = { g_dev, 1, &junk, &v, 0 };
    check(D3DKMTSignalSynchronizationObjectFromCpu(&sj) == ST_INVALID_PARAMETER, "a bad fence handle is refused");

    D3DKMT_DESTROYSYNCHRONIZATIONOBJECT ds = { cs2.hSyncObject };
    check(D3DKMTDestroySynchronizationObject(&ds) == 0, "DestroySynchronizationObject");
    D3DKMT_DESTROYDEVICE dd = { g_dev };
    check(D3DKMTDestroyDevice(&dd) == 0, "DestroyDevice");
    check(cpu && (!VirtualQuery((void *)cpu, &mbi, sizeof(mbi)) || mbi.State == MEM_FREE),
          "the device's monitored fence went with it");
    check(D3DKMTSignalSynchronizationObjectFromCpu(&sg) == ST_INVALID_PARAMETER, "and so did its handles");
    check(usage(adapter) == before, "and its allocations");
    close.hAdapter = adapter;
    check(D3DKMTCloseAdapter(&close) == 0, "CloseAdapter (the listed one)");

    printf("kmttest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail != 0;
}
