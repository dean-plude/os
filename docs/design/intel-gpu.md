# The Intel GPU, the Windows 11 way

Status: design, decided 2026-10-13 (roadmap step 25.3).  Step 1 of the
steps at the end, the display-only WDDM host (`\Device\DxgKrnl`, tested
with NovaOS's `wddmdod.sys` on QEMU's standard VGA), is merged.  Step 3's
first part (the `D3DKMT*` calls, the Basic Render adapter's allocations
and CPU fences) is in; the rest of step 3, and steps 2, 4 and 5, are not
built.  Intel's driver has not been tried: it needs step 2
and the T14, and nothing has run on the T14 yet.

**The decision.**  NovaOS drives the ThinkPad T14 Gen 4's Intel Iris Xe
with **Intel's own Windows display driver**, unchanged, through a WDDM
host: a NovaOS stand-in for Microsoft's `dxgkrnl.sys` that a WDDM display
miniport plugs into, as Phase 30's NDIS and Storport hosts do for network
and storage miniports.  The package comes from the Microsoft Update
Catalog by hardware ID, is Microsoft-signed (WHQL), and installs through
the driver store like any other ([windows-drivers.md](windows-drivers.md)).
Programs reach the GPU through Intel's Vulkan and OpenGL drivers from the
same package; Direct3D 8 to 12 keep running through DXVK and VKD3D-Proton,
now on Intel's Vulkan driver instead of lavapipe.  NovaOS does not write
its own Intel kernel driver.

The alternative the roadmap step first named (a native NovaOS kernel
driver under Mesa's `iris` and `anv`) is kept as the fallback, with the
point at which it would be taken written down below.

## The hardware

| | ThinkPad T14 Gen 4 (Intel) |
|---|---|
| Processor | Core i5-1335U/1345U, i7-1355U/1365U (Raptor Lake-U) |
| GPU | Iris Xe, Xe-LP graphics (Gen12), 80 or 96 EUs; display version 13 (the Alder Lake-P display engine) |
| PCI ID | `8086:A7A1` expected; Linux's Raptor Lake-U list is `A721`, `A7A1`, `A7A9`, `A7AC`, `A7AD` (inferred from the processor model, to be confirmed with `devices` on the T14) |
| Memory | unified: the GPU uses system RAM through its own page tables (GGTT, per-process PPGTT); no VRAM |
| Panel | 14" 1920x1200 eDP; outputs on HDMI 2.0 and two USB-C/Thunderbolt 4 ports (step 25.5) |
| Firmware the GPU needs | GuC (scheduling), HuC (media), DMC (display power states); Linux's names `adlp_guc_70.bin`, `tgl_huc.bin`, `adlp_dmc.bin` ship in NovaOS's firmware set (#341); Intel's Windows package carries its own copies |
| Today on NovaOS | the UEFI GOP framebuffer in the firmware's mode: one mode, no flipping, no second monitor; 3D runs on the CPU (llvmpipe, lavapipe).  The display-only WDDM host (step 1) exists but no Intel driver runs on it yet |

## The choices

| | A. Intel's Windows driver through a WDDM host (chosen) | B. A native driver under Mesa (fallback) |
|---|---|---|
| Kernel side | Intel's `igdkmdn64.sys` (the KMD; name from Intel's DCH package, confirm from its INF), unchanged | Linux's `i915` or `xe`, ported, or written new |
| What NovaOS writes | the WDDM host: the `DXGKRNL_INTERFACE` callbacks, the VidPN (display topology and modes), the video memory manager (VidMm) and GPU scheduler, the `D3DKMT*` thunks behind `gdi32` | a Linux-like DRM layer (GEM objects, contexts, `execbuf`, sync files, KMS) for `i915`/`xe` to sit on, and a winsys for `iris` and `anv` |
| Code reused | Intel's binary driver (GPU, display, power, firmware loading), Microsoft's public WDDM DDI documentation | `i915`/`xe` and DRM core are mostly MIT-licensed (some DRM core files are GPL-only and would be rewritten); FreeBSD's BSD-licensed LinuxKPI shows the shim; Mesa `iris`/`anv` (MIT) |
| Size | the WDDM DDI is large, but documented field by field in the public WDK; NovaOS writes its own headers, as for NDIS | hundreds of thousands of lines of Linux driver plus a Linux kernel API shim (inferred from the `i915` tree's size) |
| Display | Intel's own: eDP link training, panel power, PSR, scaling, HDMI/Thunderbolt outputs, as tested on Windows | NovaOS ports or writes all of it |
| Games | the driver games are tested against on Windows; DXVK and VKD3D-Proton on Intel's Windows Vulkan driver are a known combination | Mesa on Linux, which Proton users run daily; `anv` is at least as mature as Intel's Windows Vulkan driver |
| Fits Dean's decisions | yes: a real vendor package, matched by hardware ID, from the Update Catalog, under Windows' signing policy ("Let's not re-invent the wheel") | partly: reuses MIT code, but is a NovaOS driver where Windows uses Intel's |
| Risk | Intel's KMD may lean on `dxgkrnl` behaviour the documentation leaves out; only the T14 can run it (QEMU has no Intel GPU) | the porting effort; every new Intel generation is another port |

A third option, a native driver written from scratch, was dropped: it is
B's work without B's code.

**Why A.**  It is Phase 30's decision applied to the GPU, as step 28.1
applied it to Wi-Fi (Intel's `Netwtw14.sys` through the NDIS host).  The
WDDM host is also what every other Windows GPU needs later (AMD's and
NVIDIA's drivers for step 28.5's AMD machine and for desktop PCs), whereas
B serves Intel only.

**When B is taken instead.**  After step 2 below, if Intel's KMD does not
start on the host and the cause is a `dxgkrnl` contract that cannot be
learned from the documentation and the driver's own behaviour, the GPU
falls back to B while the WDDM host stays for display-only and other
vendors' drivers.  The decision is recorded in this file either way.

## What runs where

```
 user mode   program ── d3d9/10/11 (DXVK) ── d3d12 (VKD3D-Proton) ── opengl32 ── vulkan-1 (NovaOS's loader)
                                                   │                      │             │
                                         Intel's OpenGL ICD     Intel's Vulkan ICD (igvk64.dll, from the package)
                                                   └──────────────┬───────┘
             gdi32 D3DKMT* thunks ── win32u/NtGdiDdDDI* system calls
 ─────────────────────────────────────────────────────────────────────────────────────────────
 kernel      WDDM host (kernel/nt/dxgk*.c, "\Device\DxgKrnl")
               ├ adapter + PnP start/stop/power (on Phase 30's PDOs, MSI interrupts, MmMapIoSpaceEx)
               ├ VidPN: sources, targets, mode sets ── the display layer (kernel/hal/display.c, a WDDM back end)
               ├ VidMm: allocations, GPU virtual address spaces, residency, paging buffers
               ├ scheduler: contexts, DMA buffers, monitored fences, preemption, TDR
               └ calls into ── Intel's igdkmdn64.sys (DriverEntry → DxgkInitialize)
             NovaOS kernel: mm, scheduler, interrupts, PnP tree, Phase 30 export layer
```

### Loading: `DxgkInitialize`

A display miniport's `DriverEntry` calls `DxgkInitialize` (or
`DxgkInitializeDisplayOnlyDriver`), from the WDK's static `displib.lib`
linked into the driver, which opens `\Device\DxgKrnl`, asks it for its
initialisation function with `IOCTL_VIDEO_DDI_FUNC_REGISTER`, and hands it
the driver's `DRIVER_INITIALIZATION_DATA` (the `DxgkDdi*` table).  The
host creates that device at boot, answers the IOCTL, and fills the
driver object's `AddDevice` and IRP dispatch the way `dxgkrnl` does, so
the PnP manager starts the GPU like any Phase 30 device: hardware ID
match, the package's service, `DxgkDdiAddDevice`, `DxgkDdiStartDevice`
with the `CM_RESOURCE_LIST` and the `DXGKRNL_INTERFACE` callback table.
The host reports the WDDM version Intel's driver asks for (WDDM 3.x for
current drivers; the driver's `DxgkDdiQueryAdapterInfo` caps say which
optional features it uses).

### Display: the VidPN and NovaOS's display layer

- **Taking over from the GOP framebuffer.**  The kernel draws on the
  firmware's framebuffer until the KMD starts; the host then hands it over
  with `DxgkCbAcquirePostDisplayOwnership` and the KMD's
  `DxgkDdiStopDeviceAndReleasePostDisplayOwnership` hands it back on
  failure or removal, so NovaOS always has a screen (Windows' Basic
  Display Driver plays this part; NovaOS's GOP back end does here).
- **A WDDM back end in `kernel/hal/display.c`** (built in step 1 as
  `DRV_WDDM`, for one source and one output), next to Bochs, Cirrus,
  virtio-gpu and GOP: heads are the KMD's VidPN targets (from
  `DxgkDdiQueryChildRelations` and `DxgkDdiQueryChildStatus`, with each
  monitor's EDID from `DxgkDdiQueryDeviceDescriptor`), and their modes
  are the source and target mode sets the KMD offers through the VidPN
  interface (`DxgkDdiEnumVidPnCofuncModality`), not a NovaOS list.
- **Changing modes** (`DisplaySetMode`, which Settings and
  `ChangeDisplaySettingsEx` already call) builds a VidPN with the new
  source mode, checks it with `DxgkDdiIsSupportedVidPn` and commits it
  with `DxgkDdiCommitVidPn`.  On the laptop panel the target timing stays
  the panel's native one and smaller modes are scaled by the GPU
  (`D3DKMDT_VPPS_STRETCHED` or centred), as on Windows.
- **Scan-out and flips.**  The desktop's surface becomes a primary
  allocation (`DxgkDdiCreateAllocation` with the standard primary type)
  that the GDI draws into through a CPU mapping, shown with
  `DxgkDdiSetVidPnSourceAddress`; flipping between two of them on the
  vertical blank (`DXGK_INTERRUPT_CRTC_VSYNC`) gives the display layer the
  `DisplayCanFlip` path the Bochs back end has, without tearing.
- **More monitors** (step 25.5) are more VidPN targets: the KMD reports
  HDMI and USB-C outputs, hot plug comes as `DxgkCbIndicateChildStatus`,
  and the display layer's `DisplayPoll` turns it into a new head as it
  does for virtio-gpu outputs.
- **Sleep**: `DxgkDdiSetPowerState` on the adapter and each child before
  S0ix/S3 and after wake, replacing `DisplayResume` for this back end.

### Memory: VidMm on unified memory

The Iris Xe has no VRAM, so every allocation lives in system memory and
the GPU reaches it through page tables.  The host implements VidMm for
WDDM 2.0's **GPU virtual addressing** in the model the KMD reports in its
`DXGK_DRIVERCAPS` (GpuMmu, where VidMm allocates the GPU's page tables and
the KMD writes their entries in paging buffers, is expected for Intel;
confirm from the driver's caps):

- the memory segments the KMD describes (`DXGKQAITYPE_QUERYSEGMENT4`): an
  aperture or system-memory segment backed by NovaOS pages pinned while
  resident, so nothing is copied between "video" and system memory;
- a GPU virtual address space per process (`DxgkDdiCreateProcess`), with
  `D3DKMTMapGpuVirtualAddress` placing allocations in it;
- residency (`MakeResident`, `Evict`) as pin counts on NovaOS pages, with
  a budget from the machine's RAM, as Windows reports shared memory to
  programs (`QueryVideoMemoryInfo`);
- paging operations sent to the KMD with `DxgkDdiBuildPagingBuffer`
  (fill, transfer, page table updates) and run on its paging queue;
- CPU access (`D3DKMTLock2`) mapping the same pages into the process,
  write-combined where the KMD asks.

### Work: contexts, submission and fences

- Devices and contexts per process (`DxgkDdiCreateDevice`,
  `DxgkDdiCreateContext`); the user-mode driver builds command buffers in
  its own memory and submits them (`D3DKMTSubmitCommand`), the host
  queues them per context and calls `DxgkDdiSubmitCommandVirtual` in
  order, with preemption (`DxgkDdiPreemptCommand`) for the desktop and
  for timeouts.
- Interrupts go through Phase 30's MSI path to the KMD's
  `DxgkDdiInterruptRoutine`; its `DxgkCbNotifyInterrupt` reports DMA
  completion and vsync, and its `DxgkCbQueueDpc` runs
  `DxgkDdiDpcRoutine` from a DPC.
- Monitored fences (`D3DKMTCreateSynchronizationObject2`, signalled by the
  GPU, waited on by the CPU and other contexts) are the synchronisation
  Vulkan and D3D12 use, and are what VKD3D-Proton's `d3dkmt.c` already
  asks for.
- Hardware scheduling (WDDM 2.7 HwSch, where the GuC schedules) is turned
  on only if the KMD requires it; host scheduling comes first.
- A hung GPU is recovered as Windows' TDR does it
  (`DxgkDdiResetFromTimeout`, `DxgkDdiRestartFromTimeout`), losing the
  offending contexts, never the desktop.

### User mode: `D3DKMT*` and the ICDs

- `gdi32`'s `D3DKMT*` functions (`userland/gdi32/d3dkmt.c`) are thunks to
  one NovaOS system call, `NtNovaDxgkCtl`, that takes each call's
  `D3DKMT_*` structure as Windows lays it out on x64
  (`kernel/um/um_d3dkmt.c`; 32-bit programs still get
  `STATUS_NOT_SUPPORTED`).  The kernel always lists a **Basic Render
  adapter**, the software render-only adapter Windows has on every machine
  (Microsoft Basic Render Driver): one shared system-memory segment with a
  budget of half the RAM, its allocations' size in its own private driver
  data (`NOVA_KMT_ALLOCATION`, `userland/include/d3dkmthk.h`), as each
  adapter's user- and kernel-mode drivers agree on theirs.  The adapter list (`D3DKMTEnumAdapters2`) shows the Intel adapter with its
  LUID, the one Vulkan reports for the device, so VKD3D-Proton can share
  resources and fences across APIs on it.
- Vulkan: NovaOS's own `vulkan-1.dll` finds the ICD the way the Khronos
  loader does on Windows 11, from the adapter's driver registry key
  (`D3DKMTQueryAdapterInfo` with `KMTQAITYPE_QUERYREGISTRY`,
  `VulkanDriverName`), so Intel's `igvk64.dll` loads from the driver
  store; lavapipe stays the fallback without a GPU.
- OpenGL: NovaOS's `opengl32.dll` loads the adapter's OpenGL ICD
  (`OpenGLDriverName`) the same way it picks virgl or llvmpipe today.
- Direct3D: DXVK and VKD3D-Proton run on Intel's Vulkan driver.  Intel's
  own Direct3D user-mode drivers are not used: they plug into Microsoft's
  `d3d9.dll`/`d3d11.dll`/`d3d12.dll` runtimes, which NovaOS cannot ship.
- Presenting: a Vulkan swapchain on a WDDM adapter presents through the
  `D3DKMT` present and flip calls (the ICD's own code decides which);
  NovaOS's desktop composes the result as DWM does, and a window alone on
  its monitor can be flipped straight to scan-out, as step 25.2 did for
  the virtio GPU.  Which calls Intel's ICD makes is learned in step 4.
- Video decode (Intel's Media Foundation transforms and oneVPL) and
  content protection (PAVP, HDCP) are out of scope; protected content is
  never forced or bypassed.

## Signing and packages

Intel's graphics packages in the Update Catalog are Microsoft-signed, so
they install under NovaOS's signing policy (step 30.7) unchanged; NovaOS
never patches, re-signs or test-signs them.  They are large (several
hundred megabytes; inferred from Intel's installers) and carry
extension and component INFs beside the base INF; the driver store
installs the base package and the components the GPU needs, and leaves
control-panel apps out.  Driver search (step 30.6) finds them with
`pnputil /scan-devices /search-online` on the T14.

## How it is tested

QEMU has no Intel GPU, so the host is tested on QEMU with NovaOS's own
test miniports, and Intel's driver only on the T14 (Dean's hand checks in
[hardware.md](../hardware.md)):

- **`wddmdod.sys`** (built, step 1; `tools/drivertest/wddmdod.c`), a
  display-only miniport for QEMU's standard VGA (the Bochs VBE adapter),
  modelled on Microsoft's MIT-licensed KMDOD sample
  (Windows-driver-samples, `video/KMDOD`).  The devices suite's `wddm`
  boot installs it with `pnputil`; it takes the display over from the
  built-in driver, offers the modes that fit the adapter's memory, has
  `disptest` change its mode as Settings does, gives the screen back when
  disabled, and starts again at boot.  It has one output and no
  interrupts, so flips, vertical blanks and more monitors are not tested
  by it.
- **`kmttest`** (built, step 3's first part; `userland/programs/kmttest.c`,
  the core suite): the `D3DKMT*` calls on the Basic Render adapter.
- **`wddmtest.sys`** (planned, step 3), a render miniport whose "GPU" is a kernel thread
  executing a tiny command format in software: exercises allocations,
  GPU virtual addresses, paging buffers, submission, fences, preemption
  and TDR on QEMU, with a test program driving it through `D3DKMT*`.
- On the T14: `devices` shows the GPU started with Intel's driver,
  Settings changes the panel's mode, then (step 25.4) `gltest` and
  `d3dtest` pass on it and beat llvmpipe and lavapipe.

## Steps

Each is one pull request off main.

1. **The WDDM host, display-only.**  `\Device\DxgKrnl`,
   `DxgkInitializeDisplayOnlyDriver`, the display-only DDIs and the
   VidPN, the display layer's WDDM back end.  Done when `wddmdod.sys`
   drives QEMU's Bochs adapter, Settings changes its mode, and removing
   the driver returns the screen to the built-in one.  **Done**:
   `kernel/nt/dxgkrnl.c` and `kernel/nt/dxgk.h`, the `DRV_WDDM` back end
   in `kernel/hal/display.c`, `tools/drivertest/wddmdod.c`, tested by the
   devices suite's "wddm" boot (`tests/selftest/devices/wddm`; see
   [HISTORY](../HISTORY.md#a-windows-display-driver-on-the-boot-display)).
   Not in step 1: the render DDIs, interrupts and vertical blanks, power
   states, more than one source, and monitor interfaces.
2. **Intel's driver starts on the T14 and changes the panel's mode.**  The
   full `DxgkInitialize` path, `DxgkDdiStartDevice` with the
   `DXGKRNL_INTERFACE` callbacks, adapter queries, VidMm's segments and
   the primary allocation, `CommitVidPn` and `SetVidPnSourceAddress`.
   Done when Intel's package, downloaded by hardware ID, starts on the
   T14, the desktop shows on the panel through it, and Settings changes
   the panel's mode.  **Roadmap step 25.3 ticks here**: its "Done when"
   is this design merged and modes changing on the T14's panel.
3. **Render on QEMU.**  VidMm's GPU virtual addressing and residency, the
   scheduler, monitored fences, TDR, and the `D3DKMT*` system calls,
   checked with `wddmtest.sys`.  In parts, one pull request each:
   1. **The `D3DKMT*` calls** (done): `NtNovaDxgkCtl` and gdi32's thunks;
      the Basic Render adapter (`D3DKMTEnumAdapters2`,
      `OpenAdapterFromLuid`, `QueryAdapterInfo`: type, WDDM version,
      segment sizes); devices; allocations in shared system memory,
      counted by `D3DKMTQueryVideoMemoryInfo` and mapped by
      `D3DKMTLock2`; fences and monitored fences (the value on a read-only
      page in the process) signalled from the CPU, waited on in place or
      through an event, `WaitAny`.  Checked by `kmttest` in the core suite.
   2. **Render miniports**: `DxgkInitialize`'s
      `DRIVER_INITIALIZATION_DATA` and `wddmtest.sys` registering with it;
      its adapter in the list beside Basic Render; allocations through
      `DxgkDdiCreateAllocation`/`DestroyAllocation` and its segments
      (`DXGKQAITYPE_QUERYSEGMENT4`).
   3. **GPU virtual addresses**: `DxgkDdiCreateProcess`, per-process
      address spaces, `D3DKMTMapGpuVirtualAddress`, page tables written
      through `DxgkDdiBuildPagingBuffer`, residency (`MakeResident`,
      `Evict`) against the budget.
   4. **Contexts and submission**: `D3DKMTCreateContextVirtual`,
      `D3DKMTSubmitCommand`, the scheduler calling
      `DxgkDdiSubmitCommandVirtual` in order, the miniport's interrupt and
      DPC (`DxgkCbNotifyInterrupt`), fences the GPU signals
      (`D3DKMTSignalSynchronizationObjectFromGpu2`, waits from the GPU).
   5. **Preemption and TDR**: `DxgkDdiPreemptCommand`, the timeout,
      `DxgkDdiResetFromTimeout`/`RestartFromTimeout`, the offending
      context lost and the rest going on.
   6. **Sharing and 32-bit programs**: shared allocations and fences by NT
      handle (`D3DKMTShareObjects`, `OpenResourceFromNtHandle`,
      `OpenSyncObjectFromNtHandle`, which VKD3D-Proton uses), and the x86
      structures translated for 32-bit programs.
4. **Intel's Vulkan and OpenGL drivers** (roadmap step 25.4).  The ICDs
   load through NovaOS's loaders from the adapter's registry key;
   `vulkaninfo`, then `gltest` and `d3dtest` (DXVK, VKD3D-Proton) pass on
   the T14 and outperform llvmpipe and lavapipe.
5. **External monitors** (roadmap step 25.5).  HDMI and USB-C outputs as
   heads, hot plug, windows moved between them and the panel.

Steps 1 and 3 run in CI; steps 2, 4 and 5 need the T14.
