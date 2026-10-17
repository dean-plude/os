# Windows drivers, the Windows 11 way

Status: design, decided 2026-10-12 (Dean: "Let's do drivers like Windows 11
does it. Let's not re-invent the wheel.").  Built: all eleven steps of
roadmap Phase 30 (30.1 to 30.11, listed at the end) are merged, each
tested in QEMU in CI's devices suite.  Not built yet: the vendor feeds and
Device Manager's search button of 30.6, the desktop's test-mode mark of
30.7, and the later hosts (HID class, PortCls/AVStream, the rest of
WDDM).  Nothing has run on the reference ThinkPad T14 Gen 4 yet.  This
file is the design as decided, with notes where the build differs; the
user-facing description of what works is
[docs/drivers.md](../drivers.md).

**The decision.**  NovaOS will load real Windows driver packages (an `.inf`
with its `.sys`, `.cat` and firmware files) and run their kernel-mode
drivers through the same NT kernel-mode driver interface Windows 11 offers
them: WDM through `ntoskrnl.exe` and `hal.dll` exports, KMDF and UMDF through
Microsoft's own open-source Windows Driver Frameworks, and the class
"port" interfaces (NDIS, Storport, the USB bus interface, later WDDM) that
vendor miniports plug into.  A device gets its driver as on Windows: PnP
finds its hardware IDs, the driver store is searched for the best-ranked
`.inf` that names one, and if none is there the driver is downloaded by
hardware ID, the way Windows Update offers drivers.  NovaOS's own built-in
drivers (`kernel/drivers/`) stay and play the part of Windows' inbox
drivers.

## How Windows 11 does it

| Piece | Windows 11 | NovaOS before Phase 30 | NovaOS plan (built unless noted) |
|---|---|---|---|
| Device tree | PnP manager: device nodes with instance, hardware and compatible IDs | `kernel/io/pnp.c` (#300), same IDs, `CTL_PNP`, PlugPlay service (#301) | each node also gets a Windows PDO (`DEVICE_OBJECT`) that a driver stack attaches to |
| Driver store | `DriverStore\FileRepository`, `oemN.inf`, `HKLM\SYSTEM\DriverDatabase` | same layout; `pnputil`/`SetupCopyOEMInf` stage packages (#295, #302) | unchanged; staging now also installs on matching devices |
| Matching | hardware ID, then compatible ID, ranked by match and signature | `pnputil /enum-devices` shows matches but installs nothing | setupapi/newdev rank and install as Windows does, writing the service key and the device's `Driver` key |
| Inbox drivers | ~600 Microsoft-signed `.inf`s in `C:\Windows\INF` | NovaOS's built-in drivers | built-ins keep the role; a vendor package with a better rank can replace one on a device, and rolling back returns the built-in (not built: only the virtio-net and Bochs VBE drivers step aside, and only when a package is installed on their device explicitly) |
| Getting drivers | Windows Update offers drivers by hardware ID; Microsoft Update Catalog serves the same packages | none | a driver search service queries by hardware ID and stages the package (step 30.6; built as `pnputil /scan-devices /search-online`, Catalog only) |
| Kernel-mode drivers | PE32+ `.sys` images (subsystem native) importing from `ntoskrnl.exe`, `hal.dll`, `Wdf01000.sys` via `WdfLdr`, class drivers | NT-style I/O manager with `DRIVER_OBJECT`/`IRP` for NovaOS's own devices (`kernel/io/io.c`), but ELF, System V ABI, NovaOS layouts | a kernel PE loader and an NT export layer with Windows-exact layouts and the Microsoft x64 ABI |
| User-mode drivers | UMDF 2 drivers in `WUDFHost.exe` | none | UMDF 2 host from the same open-source WDF code (step 30.10) |
| Signing | kernel-mode drivers need a Microsoft-signed catalog (WHQL or attestation); test signing is a boot option | `.cat` files are copied, never checked | the same policy, written down below; never bypassed |

## What runs where

```
 user mode   Device Manager / pnputil / Settings ─ setupapi, newdev, cfgmgr32 ─ driver search service
                                                                              (hardware ID → package)
 ───────────────────────────────────────────────────────────────────────────────────────────────
 kernel      vendor.sys   Wdf01000.sys (MIT WDF source)   netkvm.sys   viostor.sys   ...
               │                │                              │            │
             ntoskrnl.exe / hal.dll export layer (kernel/nt/)  NDIS host   Storport host   USB bus interface
               │  Microsoft x64 ABI, Windows-exact DRIVER_OBJECT, DEVICE_OBJECT, IRP, IO_STACK_LOCATION, MDL, KEVENT ...
             NovaOS kernel: mm, scheduler, interrupts (IOAPIC, MSI), PnP tree, I/O manager, lwIP, file systems
```

A Windows driver is a guest in NovaOS's kernel, not a rewrite of it: the
export layer translates each call into NovaOS's own memory, scheduling,
interrupt and PnP code.  NovaOS's built-in drivers do not change.

## The kernel-mode driver interface

### Loading an image

- **Kernel PE loader** (`kernel/nt/ldr.c`, step 30.1): maps a PE32+
  image with subsystem native into a reserved 1 GiB kernel range
  (`0xFFFFFFFF00000000`, under the top page-map entry every process
  shares, so a driver is mapped in all of them), section by section with
  W^X protections, applies base relocations, binds imports, initialises
  the `/GS` cookie and calls `DriverEntry(DRIVER_OBJECT*, UNICODE_STRING
  *RegistryPath)`.  Registering its `.pdata` so kernel-mode exceptions
  unwind through it comes with kernel-mode SEH (30.2).  A driver whose
  import is missing does not start: the log names the image and the
  export ("`[NT] vioser.sys: ntoskrnl.exe!KeFoo not implemented`"),
  `NtLoadDriver` fails with `STATUS_PROCEDURE_NOT_FOUND`, and once drivers
  start on devices (30.3) the device shows problem code 39, as Windows
  shows for a driver that cannot load.
- **Services**: a kernel driver is a service key under
  `HKLM\SYSTEM\CurrentControlSet\Services\<name>` (`Type` 1, `Start`,
  `ImagePath`, `Group`), as written by an `.inf`'s `AddService` or by `sc
  create type= kernel`.  `NtLoadDriver`/`NtUnloadDriver` (which need
  `SeLoadDriverPrivilege`) and the service manager (advapi32's
  `StartService` and `ControlService`) start and stop them; a driver with
  open files unloads when the last one closes, and one without a
  `DriverUnload` cannot be stopped, as on Windows; boot-start drivers (`Start` 0) start after
  the file system is up, since NovaOS boots from its own built-in storage
  drivers.
- **Export layer** (`kernel/nt/`): `ntoskrnl.exe` and `hal.dll` are export
  tables built into the NovaOS kernel from one list, `kernel/nt/exports.h`
  (`NT_EXPORT(module, name)` lines, expanded by `exports.c` into the table
  the loader binds against, and read by `tools/drivertest/mksys.py` to
  make the import libraries test drivers link with).  Every export is compiled with `__attribute__((ms_abi))`,
  because drivers use the Microsoft x64 calling convention and the NovaOS
  kernel the System V one; callbacks a driver hands in (`DriverEntry`,
  dispatch routines, ISRs, DPCs, completion routines, thread start
  routines) are called through `ms_abi` function pointers.  Keeping the
  layer inside the ELF kernel lets it reach NovaOS internals directly; a
  separate PE `ntoskrnl.exe` would need a second bridge the other way.
- **Structures**: drivers read and write `DRIVER_OBJECT`,
  `DEVICE_OBJECT`, `IRP`, `IO_STACK_LOCATION`, `MDL`, `KEVENT`,
  `KSPIN_LOCK`, `KDPC`, `KTIMER`, `UNICODE_STRING`, `LIST_ENTRY` and the
  rest directly, so the layer uses Windows' exact x64 layouts
  (`kernel/nt/wdm.h`, written from Microsoft's public documentation and
  checked against the offsets drivers use).  NovaOS's own `io.h`
  structures keep their layouts; a NovaOS file handle that names a
  Windows device carries the Windows `FILE_OBJECT` beside it, and
  `NtCreateFile`, `NtReadFile`, `NtWriteFile`, `NtDeviceIoControlFile`
  and `NtClose` on it build a Windows `IRP` and call the driver's
  dispatch routine (`kernel/nt/io.c`).  Step 30.1 served buffered and
  neither I/O and completion routines; 30.2 added direct I/O (an MDL for
  the caller's buffer in `MdlAddress`, a control code's input still
  buffered), dispatch routines returning `STATUS_PENDING` (the requesting
  thread sleeps on the IRP's event until the driver completes it, from a
  DPC or another thread), the cancel spin lock and `IoCancelIrp`.  On a
  handle opened for overlapped I/O a pending IRP returns
  `STATUS_PENDING` to the program instead: an I/O completion thread
  finishes it into the caller's `OVERLAPPED` (buffered data copied back,
  the status block written, the event set) when the driver completes
  it, `NtCancelIoFile(Ex)` cancels it, and the handle's `IRP_MJ_CLOSE`
  waits for the last pending one, as on Windows.

### What a driver expects of the kernel

- **IRQL**: Windows x64 keeps IRQL in `CR8`, and so does NovaOS (step
  30.2, `kernel/ke/irql.h`): the WDK's `KeRaiseIrql` and `KeLowerIrql` are
  inline `CR8` writes on x64, so drivers set it themselves, and the
  exports (`KeRaiseIrqlToDpcLevel`, `KeAcquireSpinLockRaiseToDpc`, ...)
  do the same.  `CR8` is the local APIC's task priority: at
  `DISPATCH_LEVEL` the APIC holds back vectors below 0x30, where the DPC
  interrupt is (0x2F, Windows' own), while NovaOS's device interrupts and
  timer (0x30 up) still arrive.  At `DISPATCH_LEVEL` and above the NovaOS
  scheduler does not switch threads: it asks for the DPC interrupt, which
  the APIC delivers once the IRQL drops, and switches then; a thread
  that blocks at `APC_LEVEL` (holding a fast mutex) keeps its IRQL across
  the switch.  A dispatch routine that returns above `PASSIVE_LEVEL` is
  logged (`IRQL_GT_ZERO_AT_SYSTEM_SERVICE`) and its IRQL put back.  Device interrupts
  for a Windows driver run at the vector's IRQL (`vector / 16`, as on
  Windows) with interrupts off; once the routines are done the interrupt
  is ended and the IRQL goes back down with interrupts on, so whatever
  arrived meanwhile is taken then (lowering `CR8` below a waiting
  interrupt with interrupts off makes KVM nested on some hosts fail the
  VM entry).  So NovaOS's own interrupt vectors have to be placed so they
  are not masked by a raised IRQL they must still serve.
- **DPCs and work items**: a per-CPU DPC queue drained when IRQL drops
  below `DISPATCH_LEVEL` (step 30.2: by the DPC interrupt, at
  `DISPATCH_LEVEL` with interrupts on, the interrupted code's SSE
  registers kept since driver code may use them); `IoQueueWorkItem`/`ExQueueWorkItem` on a
  pool of system threads.
- **Synchronisation and time**: `KEVENT`, `KMUTEX`, `KSEMAPHORE`,
  `FAST_MUTEX`, `ERESOURCE`, push locks, `KTIMER` with DPCs,
  `KeWaitForSingleObject`/`MultipleObjects`, `KeQueryPerformanceCounter`,
  `KeStallExecutionProcessor`, all mapped onto NovaOS's wait queues and
  timers.  Step 30.2 has events, semaphores, timers (on the 100 Hz clock
  tick, with their DPCs), fast mutexes and `KeWaitForSingleObject`; the
  waiters sit in the object's `WaitListHead` under one dispatcher lock.
- **Memory**: `ExAllocatePool2`/`ExAllocatePoolWithTag`/`ExFreePool`
  (paged and non-paged, tags kept for the log), lookaside lists, MDLs
  (`IoAllocateMdl`, `MmProbeAndLockPages`, `MmGetSystemAddressForMdlSafe`),
  `MmMapIoSpaceEx`, `MmAllocateContiguousMemorySpecifyCache`,
  `MmGetPhysicalAddress`.  Step 30.2 has the pool (every type is the
  kernel heap, which is never paged; tag, size and owner recorded beside
  each block, a wrong tag or a double free logged as `BAD_POOL_CALLER`,
  blocks left at unload logged) and MDLs (`MmProbeAndLockPages` on a
  program's or a kernel buffer, `MmBuildMdlForNonPagedPool`,
  `MmMapLockedPagesSpecifyCache` mapping the frames at consecutive kernel
  addresses).  NovaOS keeps no reference counts on a program's frames, so
  a locked page is described, not pinned; and until kernel-mode SEH
  raises, `MmProbeAndLockPages` on a bad buffer logs it and describes the
  bad pages by a spare zeroed frame.  Kernel stacks of threads that call into a
  driver need Windows' 24 KB (`KERNEL_STACK_SIZE`); NovaOS's are still
  16 KB (`THREAD_STACK_SIZE`).
- **Threads and processes**: `PsCreateSystemThread`,
  `PsGetCurrentProcess`, `KeStackAttachProcess` (for drivers that touch a
  caller's buffers), `ObReferenceObjectByHandle`.
- **Registry and files**: `ZwOpenKey`, `ZwQueryValueKey`,
  `IoOpenDeviceRegistryKey`, `ZwCreateFile`/`ZwReadFile` (firmware
  loading reads from `C:\Windows\System32\drivers` or the driver store),
  over NovaOS's configuration manager and file systems.
- **PnP and power**: `AddDevice`, `IoAttachDeviceToDeviceStack`, the
  `IRP_MJ_PNP` minor codes (start with the device's raw and translated
  `CM_RESOURCE_LIST`, query and surprise removal, stop), `IRP_MJ_POWER`
  (D0/D3 for sleep, which NovaOS already does for its own drivers),
  device interfaces (`IoRegisterDeviceInterface`, so `SetupDi*` and
  `CM_Register_Notification` find them through the existing PnP code).
- **Hardware access**: `IoConnectInterruptEx` (line-based and MSI/MSI-X
  through NovaOS's IOAPIC and MSI code), `READ_REGISTER_*` and port I/O
  are inline in drivers already, `IoGetDmaAdapter` with common buffers and
  scatter/gather lists (identity physical mapping first, IOMMU later),
  `HalGetBusDataByOffset`/`BUS_INTERFACE_STANDARD` for PCI configuration
  space.
- **Exceptions and crashes**: kernel-mode SEH (`__C_specific_handler`,
  `RtlUnwindEx` over the image's `.pdata`), `__chkstk`, the `/GS`
  `__security_cookie`; `KeBugCheckEx` and any fault in a driver image stop
  NovaOS with the bug-check code and the driver's name on the crash screen
  and serial log, as Windows' blue screen does.
- **Tracing stubs**: WPP/ETW (`EtwRegister`, `WmiTraceMessage`) and WMI
  (`IoWMIRegistrationControl`) succeed and log nothing at first, since
  almost every driver imports them.

Which exports to write first is decided by data, not guesswork: a script
(`tools/sys_imports.py`) lists a `.sys` file's imports against
`kernel/nt/exports.h`, so each step implements what its target driver
actually calls.

### Frameworks: KMDF and UMDF

Most drivers written since Windows 8 are KMDF drivers: they import only
`WdfVersionBind` and friends from `Wdf01000.sys`, which then calls the
kernel.  Microsoft publishes the KMDF and UMDF 2 source under the MIT
licence ([Windows-Driver-Frameworks](https://github.com/microsoft/Windows-Driver-Frameworks)),
so NovaOS builds `Wdf01000.sys` and `WdfLdr.sys` from that source with the
same clang and lld-link it builds userland with, and runs it as any other
`.sys` on the export layer.  This is the "don't reinvent the wheel" part:
the framework drivers call is Microsoft's own code.  Microsoft's
NetAdapterCx (the network class extension, also MIT) can be reused the
same way when a target needs it.  The published source implements KMDF
1.33, the version Windows 11 ships; a driver built for a newer minor binds
as on Windows (`WdfClientVersionHigherThanFramework`: it sees the 1.33
function table and checks `WdfDriverIsVersionAvailable` before newer
calls), and one built for another major is refused.

How it is built (step 30.4, `tools/kmdf/build_kmdf.py`): the published
tree is the framework without the build around it, so NovaOS adds what
was left out and changes nothing else.  Five patches restore what the
published tree lost (a class head, a few return types, two enums, one
alignment; each in `tools/kmdf/patches` with its reason); mingw-w64's WDK
headers, completed by `tools/kmdf/inc` where they predate Windows 10,
stand in for the WDK's; `tools/kmdf/rt` stands in for the WDK libraries
the framework links (`wdmsec.lib`, the USB and `aux_klib` helpers, the
Verifier queries, WPP trace output as `DbgPrintEx`); and `WdfLdr.sys`,
which Microsoft did not publish, is NovaOS's own (`tools/kmdf/wdfldr.c`),
written to the loader interface the source's `fxldr.h` defines.  The
kernel loads `WdfLdr.sys` as Windows loads an export driver, for the
first driver that imports it.  Until NovaOS has a trust anchor for its own
system files, the framework and loader carry a test signature and KMDF
drivers need test signing on.

UMDF 2 drivers (sensors, many fingerprint readers, some HID and USB
devices) run in a host process (`WUDFHost.exe`) from the same source, with
a reflector driver forwarding I/O to it.

### Class and port drivers NovaOS must provide

Vendor drivers are usually miniports: they plug into a Microsoft class or
port driver that Windows ships inbox and NovaOS cannot redistribute.
NovaOS writes those hosts itself, connected to its own stacks, one per
step and only when a target driver needs it:

| Host | Microsoft binary it stands in for | Miniports it runs | Connected to |
|---|---|---|---|
| USB bus interface | `usbhub3.sys`, `ucx01000.sys` (URBs, `IOCTL_INTERNAL_USB_SUBMIT_URB`), and `usbccgp.sys` for composite devices (a PDO per function, `MI_nn`) | USB client drivers (USB serial, vendor devices, each function of a composite device) | NovaOS's USB core (`kernel/drivers/usb.c`) |
| NDIS | `ndis.sys` (NDIS 6.x miniports), then WDI/WiFiCx for Wi-Fi | Ethernet and Wi-Fi cards (`netkvm.sys`, Intel `Netwtw*.sys`) | lwIP and NovaOS's network interfaces |
| Storport | `storport.sys` | storage miniports (`viostor.sys`, `vioscsi.sys`, Intel VMD) | NovaOS's disk layer |
| HID class | `hidclass.sys` (`kernel/nt/hidclass.c`, built) | HID minidrivers (vendor touchpads, pens), and NovaOS's USB one (`hidusb.c`); KMDF HID drivers as lower filters under NovaOS's `mshidkmdf.sys` (`mshidkmdf.c`, Microsoft's `vhidmini2` sample checked in CI) | `hid.dll` and setupapi (one device per top-level collection); keyboard and mouse collections go to NovaOS's `kbdhid.sys` and `mouhid.sys` class drivers (`kernel/nt/kbdhid.c`), which feed `win32k` and Raw Input; touch screen, pen and touchpad collections to `hiddigi` (what win32k reads raw on Windows) |
| PortCls / AVStream | `portcls.sys`, `ks.sys` | audio and camera drivers | NovaOS's audio and capture paths |
| WDDM | `dxgkrnl.sys` | display miniports and their user-mode D3D/Vulkan drivers | `gdi32`'s `D3DKMT*` calls and the compositor |

WDDM is the largest by far and is not a Phase 30 step: roadmap step 25.3
chose Intel's own WDDM driver through a WDDM host for the T14's Iris Xe,
with a native driver under Mesa as the fallback; the host's design and
steps are in [intel-gpu.md](intel-gpu.md).

## Getting drivers: the driver store and on-demand download

- **Install on match**: `pnputil /add-driver x.inf /install`,
  `UpdateDriverForPlugAndPlayDevices`, `DiInstallDriver` and a setup
  program's `.inf` install now install on every present device the
  package matches, ranked as Windows ranks drivers (signature, then
  hardware ID before compatible ID, then date and version).  A device
  later plugged in whose best match is in the store gets it at once.
- **Built-in drivers first**: a device a NovaOS built-in driver runs keeps
  it unless the user picks the package in Device Manager ("Update
  driver"), or the package ranks better by the same rules a Windows inbox
  driver loses to a vendor driver; "Roll back driver" returns the built-in.
  As built, packages install only on devices no built-in driver claims
  (problem code 28), except where the built-in driver can step aside
  (NovaOS's virtio-net driver for an NDIS miniport, its Bochs VBE display
  driver for a WDDM one): there `pnputil /add-driver /install` installs
  the package and the built-in driver gives the device up, and disabling
  the Windows driver gives it back.  "Update driver" and "Roll back
  driver" are not built.
- **Download by hardware ID**: a device left with problem code 28 can be
  searched for ("Search automatically for drivers" in Device Manager,
  "Optional driver updates" in Settings, `pnputil /scan-devices`).  The
  driver search service asks, in order, for its hardware IDs, then its
  compatible IDs:
  1. the Microsoft Update Catalog, which serves the packages Windows
     Update offers, keyed by hardware ID, as `.cab` files that `cabinet.dll`
     expands;
  2. a small NovaOS list of vendor feeds (`userland/drivers/feeds.json`)
     for packages published outside the Catalog, such as virtio-win.
  It stages the package with `pnputil` and installs it.  Nothing is
  downloaded without the user asking, nothing Microsoft or a vendor ships
  is put in the NovaOS image, and the downloaded `.sys` runs unchanged.
  As built (step 30.6): the Catalog source, as `pnputil /scan-devices
  /search-online [/install]`, which stages (and with `/install` installs)
  the package and records the update it came from and its SHA-256
  ([drivers.md](../drivers.md#downloading-drivers-by-hardware-id)).  The
  vendor feeds, Device Manager (NovaOS has no Device Manager app yet) and
  Settings' "Optional driver updates" are not built.
- **Redistribution**: Microsoft's inbox `.sys` files and vendor drivers
  are never shipped with NovaOS; they come to the user's own machine from
  their publisher, as on Windows.  The "default set" stays NovaOS's
  built-in drivers plus the firmware set, the store and the downloader.

## Signing policy

Windows 11 x64 loads a kernel-mode driver only when its catalog is signed
by Microsoft (WHQL or attestation signing), or when test signing is turned
on.  NovaOS follows the same policy and does not bypass it:

- At install, setupapi verifies the package's `.cat` with `WinVerifyTrust`
  (the driver action) and checks each staged file's hash against it, as
  Windows does.  A file the catalog does not list refuses the package
  (`ERROR_FILE_HASH_NOT_IN_CATALOG`); an intact catalog is installed in
  `CatRoot` as `oemN.cat`; a package without a trusted signature is
  staged as unsigned, as after Windows' "can't verify the publisher"
  warning, and its drivers do not load (problem code 52 once Plug and
  Play installs drivers on devices, step 30.3).
- At load, the kernel (`kernel/nt/ci.c`, Mbed TLS for the X.509 and RSA
  work) recomputes the image's Authenticode digest and accepts an
  embedded signature or a `CatRoot` catalog listing it, so a `.sys`
  changed on disk after install does not start (the job Windows' code
  integrity does).  Without test signing the signature must chain to a
  Microsoft product root (`kernel/nt/ci_roots.c`: Root Certificate
  Authority 2010 and the 2001 root, the public certificates from
  `userland/crypt32/roots/`) through a signing certificate with a driver
  usage (WHQL, attestation, Windows component); certificate validity
  periods are not checked, as Windows does not for drivers.  That is
  Windows 11's policy with Secure Boot on, so cross-signed drivers (a
  vendor certificate under Microsoft's old cross-signing roots) are
  refused too.
- Test signing is a NovaOS boot option: the DWORD `TestSigning` = 1 under
  `HKLM\SYSTEM\CurrentControlSet\Control\NovaOS`, NovaOS's
  `bcdedit /set testsigning on`, read at boot (so it takes a restart) and
  shown in `Control\SystemStartOptions` as ` TESTSIGNING`, as Windows
  shows it.  It lets any intact signature load (a test certificate whose
  root nobody trusts, as `makecert`'s are), never an unsigned image.
  NovaOS's own test drivers (`tools/drivertest/mksys.py`) carry such test
  signatures and load only there; the devices suite turns it on.  So do
  NovaOS's own `Wdf01000.sys`, `WdfLdr.sys` and `WUDFRd.sys` for now, so
  every KMDF and UMDF driver needs test signing on until NovaOS has a
  trust anchor for its own system files.  The desktop's "Test Mode" mark
  is not drawn yet.
- What does not work, and stays so: drivers that need Secure Boot's
  measured state, HVCI/memory integrity guarantees or a TPM-backed
  attestation (anti-cheat and DRM drivers among them) are documented as
  failing in [compatibility.md](../compatibility.md), with the faithful fix
  written down, never patched around.

## Choices made, and the alternatives

- **In the kernel, not in a user-mode host**: Wine runs `.sys` files in a
  user process (`winedevice.exe`), which works for drivers that never touch
  hardware (copy protection, virtual devices).  Real device drivers map
  registers, take interrupts and do DMA, and the Windows contract for
  them is ring 0, so NovaOS hosts them in the kernel.  A crashing driver
  stops NovaOS, as it stops Windows.
- **No ReactOS code**: ReactOS's NT kernel is GPL and cannot be linked
  into NovaOS's MIT kernel.  NovaOS's export layer is written from
  Microsoft's public documentation; Microsoft's own MIT code (WDF,
  NetAdapterCx) is reused as is.
- **Microsoft's DDK headers are not needed**: the export layer and the
  test drivers use NovaOS's own headers, as userland does.

## Steps (roadmap Phase 30)

All eleven steps are merged.  Each was one or more pull requests off
main, ending in a check anyone can repeat on QEMU; the devices suite boot
that checks it is named after "Test:".  The "As built" notes say where
the result differs from the plan.  Test drivers are written
for NovaOS under `tests/selftest/devices/`, or, for third-party drivers,
downloaded by the test as NovaOS's other tests download apps.

1. **30.1 Load a kernel-mode driver.** The kernel PE loader, the
   `ntoskrnl.exe`/`hal.dll` export layer with Windows-exact I/O
   structures, `NtLoadDriver` and kernel services, and IRPs from
   `NtCreateFile`/`NtDeviceIoControlFile`/`NtReadFile`/`NtClose`.  Done
   when: a NovaOS test driver (`wdmtest.sys`, built with clang and
   lld-link `/driver`) installed with `sc create type= kernel` starts,
   creates `\Device\WdmTest` and its `\DosDevices` link, and a test
   program's `DeviceIoControl` and `ReadFile` round-trip through it, and
   `sc stop` unloads it.  **Done.** Test: `wdm`.
2. **30.2 The kernel API a driver lives on.** IRQL in `CR8`, spin locks,
   DPCs, timers, events, work items, system threads, pool, MDLs, `Zw*`
   registry and files, kernel-mode SEH, `/GS`, ETW/WMI stubs, bug checks
   naming the driver; `tools/sys_imports.py`.  Done when: `wdmtest.sys`
   exercises each group in a devices test, and `sys_imports.py` on
   virtio-win's `vioser.sys` and `Wdf01000.sys` lists what is still
   missing.  **Done.** Test: `wdm`.
3. **30.3 PnP drivers on real hardware resources.** A PDO per PnP node,
   `AddDevice`, start/stop/remove IRPs with `CM_RESOURCE_LIST`,
   `IoConnectInterruptEx` (INTx and MSI), `MmMapIoSpaceEx`, DMA adapters,
   device interfaces; `pnputil /add-driver /install` and newdev install on
   matching devices with no built-in driver.  Done when: a NovaOS WDM test
   driver installed from its `.inf` drives QEMU's `edu` PCI device
   (MMIO, an MSI interrupt and a DMA transfer) and shows as started in
   Device Manager.  **Done.** Test: `edu` (the device shows as started
   in `pnputil /enum-devices`; there is no Device Manager app).  As built:
   the PDOs were PCI devices only at first (the bus NovaOS's own drivers
   leave free); USB devices (30.8) and bus drivers' children (30.5) came
   later.  Interrupts are the device's line or,
   for a driver whose INF sets `MSISupported` (Windows' own opt-in) on a
   device with an MSI or MSI-X capability, a `CM_RESOURCE_INTERRUPT_MESSAGE`
   descriptor (the kernel programs the capability when the device starts, as
   Windows' PCI driver does) and a message-based `IoConnectInterruptEx` returns the
   `IO_INTERRUPT_MESSAGE_INFO` table (KMDF connects each vector fully specified);
   the connect falls back to the line when the device was not given messages, DMA goes
   through bounce buffers when memory is beyond the device's address
   width (the page allocator sets aside 2 MiB below 16 MiB for them), and removal is tested through `pnputil /disable-device` (query-
   remove, remove), since NovaOS has no PCI hot-unplug for QEMU's
   `device_del`: surprise removal is handled but untested.
4. **30.4 KMDF.** `Wdf01000.sys` and `WdfLdr.sys` built from Microsoft's
   MIT WDF source and loaded on the export layer.  Done when: a KMDF
   version of the `edu` test driver passes the same test.  **Done.** Tests:
   `wdm` (`kmdftest.sys`, a control device and a queue) and `kmdfedu`
   (`kmdfedu.sys` on the `edu` device: interrupt object on the line and on
   MSI, DMA transactions, a common buffer).  Class-extension drivers
   (`WdfVersionBindClass`) do not load yet.
5. **30.5 The first third-party driver.** virtio-win's serial driver
   (`vioser.sys`, KMDF; virtio-win is BSD-licensed), on a device NovaOS has
   no built-in driver for.  Done when: on QEMU with a virtio-serial port,
   the package downloaded from virtio-win's release installs, the driver
   starts, and a test program exchanges bytes with the host through
   `\\.\Global\<port name>`.  **Done.** Test: `vioser` (the package
   comes from virtio-win's signed guest-tools installer; the PnP manager
   enumerates the driver's child ports).
6. **30.6 Downloading drivers by hardware ID.** The driver search
   service, the Update Catalog and vendor-feed sources, Device Manager's
   "Search automatically for drivers", `pnputil /scan-devices`.  Done
   when: on QEMU a device with problem code 28 gets its package found,
   downloaded, staged and started without the test naming the package.
   **Done** for the Catalog source (tests: `driversearch`, `usbserial`);
   the vendor feeds and Device Manager's button are not built.
7. **30.7 Signing policy.** Catalog verification at install, the image
   hash check at load, the test-signing boot option and its desktop mark,
   problem code 52.  Done when: a Microsoft-signed package installs, a
   tampered copy of it is refused with problem code 52, and the test
   drivers load only with test signing on.  **Done**, except the desktop
   mark.  Test: `wdm`.
8. **30.8 USB client drivers.** The USB bus interface (URBs, pipes,
   selective suspend) over NovaOS's USB core.  Done when: QEMU's
   `usb-serial` device (an FTDI FT232) works through FTDI's own driver,
   downloaded by hardware ID, as a COM port a terminal program opens.
   **Done.** Test: `usbserial` (COM3).  Isochronous transfers are not
   built.
9. **30.9 Network miniports through NDIS.** An NDIS 6 host connected to
   lwIP.  Done when: virtio-win's `netkvm.sys` drives a QEMU virtio-net
   card (with the built-in driver set aside for that card) and a
   download over it succeeds.  Wi-Fi (WDI/WiFiCx, for Intel's AX211
   driver on the T14) follows as its own step once this works, and is
   where roadmap step 28.1 chooses between it and a native driver.
   **Done.** Test: `netkvm`.  Beyond it, Intel's `Netwtw14.sys` (a WDI
   miniport) installs, registers and has its adapter opened on a PCI
   stand-in for the AX211 (test: `ax211`).  The WDI command layer is
   28.1's: its first part (capabilities, the station port, scanning) is
   in `kernel/nt/wdi.c`, tested with NovaOS's own WDI miniport (test:
   `wifi`); connecting and the data path follow, and the real card needs
   the T14.
10. **30.10 UMDF 2.** `WUDFHost.exe` and the reflector from the same WDF
    source.  Done when: a NovaOS UMDF test driver on a QEMU device starts
    in its host process and serves a test program's I/O.  **Done.** Test:
    `umdf`.  As built, a driver must link NovaOS's UMDF stub (Microsoft's
    binds through an unpublished interface), I/O is buffered, and the host
    gives no hardware access yet.
11. **30.11 Storage miniports through Storport.** Done when: virtio-win's
    `viostor.sys` drives a second QEMU virtio-blk disk and NovaOS mounts
    a volume on it.  **Done.** Test: `viostor`.  As built: one request at a
    time through a bounce buffer, line interrupts, no adapter removal.

The HID class host is built (Dean's ask, 2026-10-14; see
[drivers.md](../drivers.md#hid-the-class-driver-and-minidrivers)), and
USB keyboards and mice reach the desktop through it, on `kbdhid` and
`mouhid` ([drivers.md](../drivers.md#keyboards-and-mice-kbdhid-and-mouhid)),
USB touch screens, pens and precision touchpads on `hiddigi`
([drivers.md](../drivers.md#touch-screens-pens-and-touchpads-hiddigi)),
and USB HID game pads and joysticks on `gamehid`
([drivers.md](../drivers.md#game-pads-and-joysticks-gamehid)).
Xbox controllers (not HID devices; `xpad.c`, as `xusb22.sys` on Windows)
and I2C-HID touchpads still skip it.
Later steps, each only when a target device needs it: PortCls
and AVStream (for the T14's audio and camera if their built-in drivers
fall short), and WDDM ([intel-gpu.md](intel-gpu.md), roadmap step 25.3,
whose first step, the display-only host with `wddmdod.sys`, is merged).
