# Drivers and firmware

NovaOS gets drivers for devices the way Windows 11 does. A device has
hardware IDs; the driver store is searched for a Windows driver package
(an `.inf` with its `.sys` and `.cat` files) that names one of them; if
none is there, the package can be downloaded by hardware ID from the
Microsoft Update Catalog. The package's kernel-mode driver then runs
unchanged on NovaOS's NT kernel-mode driver interface (`kernel/nt/`).
This is roadmap Phase 30, whose steps 30.1 to 30.11 are all merged. The
design is in [design/windows-drivers.md](design/windows-drivers.md).

NovaOS also has its own built-in drivers (`kernel/drivers/`,
`kernel/hal/`). They play the part of Windows' inbox drivers: they are on
every image and run the disks, network cards, display, sound and USB of
QEMU and common PCs from the first boot, before any package is installed.
[hardware.md](hardware.md#devices-novaos-drives) lists them by device.

Firmware, the programs a driver loads into the device itself, is the
third part. Windows driver packages carry their own. For the built-in
drivers, NovaOS can ship a pinned set of files from linux-firmware and the
audio DSP's Sound Open Firmware ([below](#the-default-firmware-set)).

All of the Windows driver support has been tested only in QEMU, in CI's
devices suite. Nothing has run yet on the reference ThinkPad T14 Gen 4 or
on any other real PC.

## How a device gets its driver

| Situation | What happens |
|---|---|
| A built-in NovaOS driver takes the device at boot | The device runs on it. A Windows package that matches is listed by `pnputil /enum-devices /drivers` as outranked by the built-in driver, and is not installed on it |
| A built-in driver that can step aside (the Bochs VBE display driver, as on QEMU's standard VGA, and the virtio-net driver) | `pnputil /add-driver x.inf /install` installs the Windows driver on the device; the built-in driver steps aside (`[PCI] 00:03.0: virtio-net steps aside for an installed Windows driver`) and the device keeps the Windows driver at every boot after |
| No built-in driver (problem code 28) | `pnputil /add-driver x.inf /install`, a setup program's `UpdateDriverForPlugAndPlayDevices` or `DiInstallDriver` installs a matching package and starts its driver at once and at every boot after; `pnputil /scan-devices /search-online /install` finds, downloads and installs one by hardware ID |

Devices a Windows driver can be started on: PCI functions, USB devices
and functions, and the child devices a Windows bus driver reports
(`IoInvalidateDeviceRelations`). A non-Plug-and-Play kernel driver can
also be installed as a service and started by hand (below).

### Device Manager

Device Manager (`devmgmt.exe`, "Device Manager" in Start) shows the same
device tree the way Windows' does: devices by type or by connection,
phantoms with View > Show hidden devices, and for the selected device its
status and problem code, driver (provider, version, date, INF), hardware
and compatible IDs and resources.  It reads them only through the public
APIs: `SetupDiGetClassDevs`, `SetupDiGetClassDescription`,
`SetupDiGetDeviceRegistryProperty`, `SetupDiGetDeviceProperty`
(`DEVPKEY_Device_Driver*`, from the software key a device install
writes; a NovaOS driver shows NovaOS as its provider and the system
version, as Windows' inbox drivers do), `CM_Get_DevNode_Status`,
`CM_Get_Child`/`CM_Get_Sibling` and `CM_Get_First_Log_Conf`,
`CM_Get_Next_Res_Des`, `CM_Get_Res_Des_Data`.  A PCI device's resources
are its BARs, sized once at boot before any driver runs, and its
interrupt line as routed by ACPI.

### pnputil

`pnputil` is Windows' driver package tool, with one NovaOS addition
(`/search-online`). Installing, disabling and enabling need an elevated
administrator (`SeLoadDriverPrivilege`).

| Command | What it does |
|---|---|
| `pnputil /add-driver x.inf [/subdirs] [/install]` | stages the package in the driver store as `oemN.inf` (`*.inf` works); `/install` also installs it on every connected device one of its models names and that no NovaOS driver runs |
| `pnputil /delete-driver oemN.inf [/uninstall] [/force]` | removes a staged package |
| `pnputil /enum-drivers` | the staged packages, with their signer and, for downloaded ones, their source |
| `pnputil /enum-devices [/connected \| /disconnected] [/class C] [/instanceid ID] [/deviceid ID] [/ids] [/drivers]` | the Plug and Play device tree, with each device's status and driver, its hardware and compatible IDs (`/ids`) and the staged packages that match it (`/drivers`) |
| `pnputil /disable-device`, `/enable-device`, `/restart-device` (an instance ID, or `/deviceid ID`) | take a device's Windows driver away (query-remove, then remove; a program registered for a handle on the device (`RegisterDeviceNotification`) is asked first with `DBT_DEVICEQUERYREMOVE` and may refuse with `BROADCAST_QUERY_DENY`, the PnP manager waiting up to ten seconds for its answer; then a driver may refuse while a program has the device open), start it again, or both; a disabled device stays off across restarts |
| `pnputil /scan-devices [/instanceid ID] [/search-online [/source URL] [/install]]` | rescan; `/search-online` downloads packages by hardware ID ([below](#downloading-drivers-by-hardware-id)) |
| `pnputil /remove-device` (an instance ID, or `/deviceid ID`) | removes a device and its driver from the tree; NovaOS removes only root-enumerated devices (`ROOT\...`), the ones a program made |

Device Manager ([above](#device-manager)) shows the same tree;
programs that call setupapi, newdev and cfgmgr32 do the same work.

### Kernel drivers as services

A kernel driver that is not a Plug and Play driver is installed as a
service and started by hand, as on Windows:

```
sc create NAME type= kernel binPath= C:\path\driver.sys
sc start NAME
sc stop NAME
```

The kernel loads the `.sys` into kernel memory and calls its
`DriverEntry`. The devices and `\DosDevices` links it creates open from
programs as `\\.\NAME`, and their `ReadFile`, `WriteFile` and
`DeviceIoControl` calls reach the driver's dispatch routines as Windows
IRPs. A driver with files still open unloads when the last one closes;
one without a `DriverUnload` routine cannot be stopped.

## What a Windows driver can use

### The NT kernel API

NovaOS's kernel exports what drivers import from `ntoskrnl.exe`,
`hal.dll` and the port drivers below. The list is one file,
[`kernel/nt/exports.h`](../kernel/nt/exports.h) (about 480 exports:
`ntoskrnl.exe`, `hal.dll`, `ndis.sys`, `storport.sys`, `hidclass.sys`,
`usbd.sys`, `cng.sys`, `wmilib.sys`). The main groups:

- IRQL kept in `CR8`, as on Windows x64; spin locks, DPCs, timers,
  events, semaphores, mutexes, fast mutexes and executive resources;
  work items and system threads.
- The pool (`ExAllocatePool2`, `ExAllocatePoolWithTag`), with tags, and
  MDLs with direct I/O. Pool blocks a driver never frees are named in the
  log when it unloads (`[NT] x.sys: 2 pool blocks (96 bytes, tags 'Abcd')
  never freed`).
- IRPs that complete later: a dispatch routine may return
  `STATUS_PENDING` and complete the request from a DPC or another thread;
  cancellation works.
- `Zw*` registry and file calls, kernel-mode SEH, the `/GS` cookie, ETW
  and WMI registration stubs, and `KeBugCheckEx`.
- Hardware: `MmMapIoSpace`, `IoConnectInterruptEx` (the device's line,
  or MSI and MSI-X messages when the driver's INF sets `MSISupported` and
  the device has the capability), DMA adapters (`IoGetDmaAdapter`,
  scatter/gather, common buffers, the version 3 calls KMDF uses) and
  device interfaces that programs open.

A driver that imports something missing does not load, and the log names
what it missed (`[NT] x.sys: ntoskrnl.exe!KeFoo not implemented`). To see
in advance what a driver needs that NovaOS does not export yet:

```bash
python3 tools/sys_imports.py x.sys        # --all lists every import
```

### Plug and Play

When a device gets a driver, NovaOS's PnP manager (`kernel/nt/pnp.c`)
does what Windows does: it gives the device a physical device object
(PDO), loads the driver and calls its `AddDevice`, and sends
`IRP_MN_START_DEVICE` with the device's memory, I/O ports and interrupt
as raw and translated resource lists. A bus driver's children (asked for
with `IRP_MN_QUERY_DEVICE_RELATIONS`) get device nodes from their
`IRP_MN_QUERY_ID` answers and start with their own drivers, or raw when
they allow it; a child no longer reported is removed. Unplugging a USB
device sends `IRP_MN_SURPRISE_REMOVAL` and `IRP_MN_REMOVE_DEVICE`,
children first, and the USB device's node leaves only once those stacks
are down: a HID collection's keyboard, mouse or pen is gone before the
unplug is. A driver that fails to load shows problem code 39, one
whose signature fails problem code 52.

A root-enumerated device, one with no hardware under it (a virtual
device such as Microsoft's `vhidmini2` HID sample), is made as
`devcon install` makes it: `SetupDiCreateDeviceInfo` with
`DICD_GENERATE_ID` (`ROOT\HIDCLASS\0000`), its hardware ID with
`SetupDiSetDeviceRegistryProperty`, and `SetupDiCallClassInstaller
(DIF_REGISTERDEVICE)`. The PnP manager gives it a PDO under the root
that answers `IRP_MN_QUERY_ID` with those IDs; it shows problem code 28
until `pnputil /add-driver x.inf /install` installs a package on it, is
made again from its `Enum\ROOT` key at every boot, and goes with
`pnputil /remove-device` (`DIF_REMOVE`), which deletes it rather than
leaving a phantom. A child started raw (a HID collection, with no
function driver of its own) records no service in its Enum key, so it
starts raw again at the next boot.

Filter drivers are loaded as Windows loads them: a package's
`.Filters` section (`AddFilter = service,,section` with
`FilterPosition = Upper` or `Lower`) writes the device's `UpperFilters`
or `LowerFilters`, and the stack is built from the bottom: the lower
filters' `AddDevice`, then the function driver's, then the upper
filters'. The class key's filters (`Control\Class\{GUID}`) come after the
device's, as on Windows. An INF's `Include = x.inf` and `Needs =
section` pull another INF's sections in (from `C:\Windows\INF`, or beside
the INF).

`pnputil /enum-devices /deviceid ID` shows the device started with its
driver.

### KMDF

NovaOS ships Microsoft's Kernel-Mode Driver Framework, `Wdf01000.sys`,
built from Microsoft's MIT-licensed
[Windows-Driver-Frameworks](https://github.com/microsoft/Windows-Driver-Frameworks)
source, and its own KMDF loader, `WdfLdr.sys` (Microsoft did not publish
its own). Both are in `C:\Windows\System32\drivers`, as on Windows, built
by `tools/kmdf/build_kmdf.py`. A KMDF driver imports `WdfLdr.sys`, which
loads `Wdf01000.sys` and binds the driver to KMDF 1.33, the version
Windows 11 ships. The framework's messages go to the log as `[DbgPrint]
Wdf01000: ...` lines.

Limits today:

- The framework and the loader are test-signed by NovaOS's build, so
  every KMDF driver needs test signing on ([below](#driver-signing)),
  even one Microsoft signed, until NovaOS has a trust anchor for its own
  system files.
- Class-extension drivers (those that bind with `WdfVersionBindClass`) do
  not load yet.

### UMDF 2

User-mode drivers run too: a DLL whose package's INF has a `.Wdf` section
(`UmdfService`, `UmdfServiceOrder`, `UmdfLibraryVersion`) and installs
the reflector, `WUDFRd.sys`, as the device's function driver. NovaOS
ships Microsoft's user-mode framework, `Wudfx02000.dll` (from the same
MIT source), and its own driver host, `WUDFHost.exe`, and reflector
(`tools/umdf/build_umdf.py`). When the device starts, the UMDF service
(`wudfsvc`) starts a host for it, which loads the framework and the
driver and runs its `DriverEntry`, `EvtDriverDeviceAdd`,
`EvtDevicePrepareHardware` and `EvtDeviceD0Entry`. A program's
`ReadFile`, `WriteFile` and `DeviceIoControl` on the driver's interface
travel through the reflector to its queues in the host and back.
Disabling the device removes it in the host, which exits.

Limits today:

- UMDF drivers built with Microsoft's WDK link Microsoft's UMDF stub,
  which binds through an interface Microsoft does not publish, so only
  drivers built with NovaOS's stub (`tools/umdf/stub.c`) run.
- I/O is buffered, at most 64 KiB a request.
- The host gives drivers no direct hardware access, interrupts, USB or
  remote I/O targets, or impersonation; one UMDF driver per device stack.
- The reflector is test-signed, so UMDF drivers need test signing on.

### USB client drivers

A Windows USB client driver runs a USB device that no NovaOS driver
took. NovaOS's USB core (`kernel/drivers/usb.c`, on xHCI, EHCI, UHCI or
OHCI) gives the device a PDO that answers as Windows' USB hub driver does
(`kernel/nt/usbd.c`):

- URBs sent with `IOCTL_INTERNAL_USB_SUBMIT_URB`: selecting a
  configuration and interface, control, bulk and interrupt transfers,
  isochronous IN transfers (a camera's stream), aborting and resetting
  pipes. Isochronous OUT transfers are not there yet (USB audio devices
  run on NovaOS's own audio driver).
- The port requests, the USB bus interface (`USB_BUS_INTERFACE_USBDI`,
  versions 0 to 3) and `usbd.sys`'s exports.
- Selective-suspend requests are accepted and held until cancelled;
  NovaOS does not suspend the device.

Each pipe's transfers run in order on their own thread, so a read waiting
for data does not hold up a write. An isochronous IN pipe streams from its
first `URB_FUNCTION_ISOCH_TRANSFER` on: the controller receives a packet
every service interval into a ring of the pipe's, and each URB's packets
are filled from it in order, so a client that keeps a URB queued, as
Windows clients do, misses nothing. Aborting the pipe or selecting
another alternate setting stops the stream.

A composite device (several interfaces, device class 0, such as a
keyboard with a vendor-defined control interface or a headset with audio
and HID) is split the way Windows' `usbccgp.sys` splits it: under the
device's node (`USB\VID_v&PID_p`, compatible ID `USB\COMPOSITE`, "USB
Composite Device") each function is a node of its own,
`USB\VID_v&PID_p&MI_nn` with `USB\VID_v&PID_p&REV_r&MI_nn` first and
`USB\Class_cc&SubClass_ss&Prot_pp` compatible IDs. A function made of
several interfaces (an audio control interface and its streams, a
camera's video interfaces) is one node. NovaOS's own drivers take the
functions they know; each other one gets its own PDO, on which a Windows
driver (or a built-in one such as `HidUsb` or `usbvideo`) sees a
configuration descriptor with only its own interfaces, can select only
those, and has its own pipes, while the device's other functions keep
running under their drivers. A bus driver's children (FTDI's
`ftdibus.sys` reports a COM port) start with their own drivers once
installed.

A port (class Ports) is named when its driver is installed, as Windows'
ports installer does: the first free `COMn` from `COM3` (the `ComDB`
arbiter), stored in the device's `PortName` and friendly name. Programs
open it as `COM3` or `\\.\COM3` and use the communications functions
(`SetCommState`, `SetCommTimeouts`, `WaitCommEvent`, `PurgeComm` and the
rest), which send the driver the `IOCTL_SERIAL_*` requests Windows sends.
For example, with an FTDI USB serial converter plugged in,

```
pnputil /scan-devices /search-online /install
```

downloads, installs and starts FTDI's bus driver, then finds and installs
the port driver for the COM port it reports.

### HID: the class driver and minidrivers

NovaOS's HID class driver (`kernel/nt/hidclass.c`) is what Windows'
`hidclass.sys` is to a HID minidriver: the minidriver calls
`HidRegisterMinidriver` from its `DriverEntry`, and the class driver
takes over its `AddDevice` and dispatch table. It asks the minidriver
for the HID descriptor, report descriptor and attributes with the
internal `IOCTL_HID_*` requests, makes a child device for each top-level
collection (`HID\VID_1209&PID_0008`, `...&Col01` when there are several)
with the HID interface (`GUID_DEVINTERFACE_HID`) turned on (a composite
device's function adds its interface: `HID\VID_1209&PID_000A&MI_01`), and keeps an
`IOCTL_HID_READ_REPORT` going to queue each input report for every open
handle. Programs use the collection as on Windows: setupapi and cfgmgr32
list its interface, and `hid.dll`'s `HidD_*` send it the `IOCTL_HID_*`
requests (attributes, preparsed data, feature, input and output reports,
strings, the queue's size and flushing it); `ReadFile` takes input
reports and `WriteFile` sends an output report. Raw Input lists each
collection as a `RIM_TYPEHID` device named by its interface path, with
its attributes, usage and preparsed data, and a program that registers
for its usage gets every input report as `WM_INPUT` (and
`WM_INPUT_DEVICE_CHANGE` as it comes and goes), as win32k does on
Windows.

A USB HID device gets NovaOS's own USB minidriver, `HidUsb` (`kernel/nt/hidusb.c`,
built into the kernel in place of Windows' `hidusb.sys`): interrupt IN
for input, interrupt OUT or `SET_REPORT` for output,
`GET_REPORT`/`SET_REPORT` for feature and input reports. That covers
vendor-defined devices and every USB keyboard, mouse, tablet, media-key
device, touch screen, pen, precision touchpad, game pad and joystick (a
device that can only
speak the boot protocol keeps NovaOS's own `usbhid` driver). On a composite device each HID
function gets its own (see [USB client drivers](#usb-client-drivers)).

### Keyboards and mice: kbdhid and mouhid

USB keyboards and mice reach the desktop the way they do on Windows:
`HidUsb` under the HID class driver, then a class driver on each
top-level collection. Plug and Play installs NovaOS's keyboard class
driver, `kbdhid`, on a keyboard collection (`HID_DEVICE_SYSTEM_KEYBOARD`,
setup class Keyboard) and on consumer- and system-control collections
(media, browser, power and sleep keys), and its mouse class driver,
`mouhid`, on a mouse or pointer collection (`HID_DEVICE_SYSTEM_MOUSE`,
setup class Mouse), as Windows' `keyboard.inf` and `msmouse.inf` install
`kbdhid.sys` and `mouhid.sys`. Both are one driver built into the kernel
(`kernel/nt/kbdhid.c`). Each opens its collection like any program,
reads the device's report descriptor and its reports, and decodes them
with the same report parser the I2C touchpads use (`kernel/drivers/
usbhid.c`): keys become the scan codes every keyboard makes, motion,
buttons, wheels and absolute positions the pointer's events, so the
desktop, Raw Input and DirectInput see a USB keyboard or mouse as they
did before. The keyboard's Caps, Num and Scroll Lock lights follow the
lock keys as output reports, and a keyboard asks to wake the machine
(`IOCTL_HID_ENABLE_WAKE_ON_SX`: the USB bus driver arms its remote
wakeup). Raw Input lists a keyboard or mouse collection as the keyboard
and the mouse, not as a `RIM_TYPEHID` device, as on Windows; programs can
still open the collection (hidapi lists keyboards). Device Manager and
`pnputil` show the two as NovaOS drivers: a driver package that matches a
keyboard's hardware ID is listed as outranked by `kbdhid`.
A keyboard plugged in again keeps its setup class: the node is created as
Keyboard from the start even though its Enum key already names `kbdhid`
(the PlugPlay service rewrites `Class` from the node on every arrival, so a
node born as HIDClass would lose the class for good).

### Touch screens, pens and touchpads: hiddigi

A USB touch screen, pen or precision touchpad runs the same way. Its
digitizer collections, a touch screen (usage page `0x0D`, usage 4,
`HID_DEVICE_UP:000D_U:0004`), a pen (2), a touchpad's finger collection
(5) and a digitizer's configuration collection (`0x0E`), get NovaOS's
digitizer class driver, `hiddigi` (the third service of
`kernel/nt/kbdhid.c`), and keep the setup class HIDClass. On Windows
`input.inf` leaves those collections raw and win32k reads them itself;
NovaOS reads them in `hiddigi` and hands the contacts to the same paths
its other digitizers use: touch becomes `WM_POINTER` (`PT_TOUCH`) and
`WM_TOUCH`, the pen `WM_POINTER` (`PT_PEN`, pressure, barrel button,
eraser) and wintab32's packets, and the touchpad's fingers the pointer,
taps, clicks and two-finger scrolling. Each collection decodes only its
own fields, so one device that is a touch screen and a pen at once
works as both. On a precision touchpad's configuration collection
`hiddigi` does what Windows does when the touchpad starts: it sets the
Input Mode feature to touchpad mode (`IOCTL_HID_SET_FEATURE`), and the
device then sends finger reports on its Touch Pad collection rather than
mouse reports on its mouse collection, where `mouhid` runs (a touchpad
that refuses stays a mouse). Raw Input lists the digitizer collections as
`RIM_TYPEHID` devices, as Windows does. Touchpads on I2C (the T14's) keep
NovaOS's I2C-HID driver for now.

### Game pads and joysticks: gamehid

A USB HID game pad or joystick (a collection of Generic Desktop usage 5
or 4, `HID_DEVICE_SYSTEM_GAME`) runs the same way too: `HidUsb` under
the HID class driver, then NovaOS's game controller class driver,
`gamehid` (the fourth service of `kernel/nt/kbdhid.c`), with the setup
class HIDClass and the name "HID-compliant game controller". On Windows
`input.inf` leaves a game controller's collection raw and DirectInput
reads it through `hid.dll`; NovaOS's DirectInput, winmm's joystick API
and XInput read the kernel's controller slots instead
(`kernel/drivers/gamepad.h`), so `gamehid` gives the controller a slot
(named by its product string) and hands each report it reads from the
collection to `kernel/drivers/gamepad.c`, which decodes the buttons,
axes and hat from the report descriptor as before. Everything else sees
the collection itself, as on Windows: Raw Input lists it as a
`RIM_TYPEHID` device named by its interface path (`\\?\HID#VID_1209&
PID_0007#...`) and posts its reports as `WM_INPUT`; setupapi and cfgmgr32
list that interface; `hid.dll` opens it through the class driver
(attributes, product string, preparsed data, `ReadFile`,
`HidD_GetInputReport`); and DirectInput's `DIPROP_GUIDANDPATH` names it.
SDL reads such a pad through DirectInput or Raw Input as before. Xbox
360 and Xbox One controllers are not HID devices (vendor-specific
interfaces, `xusb22.sys` on Windows) and keep NovaOS's `xpad` driver,
their XInput user index and the HID side Windows gives them (`&IG_00`
in its path).

A handle opened with `FILE_FLAG_OVERLAPPED` does overlapped I/O, as
hidapi and most HID tools use it: `ReadFile` with no report waiting
fails with `ERROR_IO_PENDING`, the program waits on the `OVERLAPPED`'s
event with a timeout (`WaitForSingleObject`, `GetOverlappedResult`), and
the read completes when the next report comes; `CancelIo`,
`CancelIoEx` and closing the handle end it with
`ERROR_OPERATION_ABORTED`. This is the I/O manager's, not the HID class
driver's: an IRP any Windows driver leaves pending on an overlapped
handle (`ReadFile`, `WriteFile`, `DeviceIoControl`) lets the system
call return `STATUS_PENDING`, and the kernel's I/O completion thread
later copies the data back, writes the `OVERLAPPED`'s status and sets
its event (or posts to the handle's completion port), as Windows'
completion APC does (`kernel/nt/io.c`, `kernel/um/um_driver.c`).

hidapi (and SDL, which uses it) tells a USB HID device from a Bluetooth,
Bluetooth LE, I2C or SPI one by the device tree, as on Windows: the HID
interface's `DEVPKEY_Device_InstanceId`
(`CM_Get_Device_Interface_PropertyW`) names the collection's device
node, and the compatible IDs of that node's parent (`USB\Class_03...`
for a USB device) say which bus it is on; for a USB device it then reads
the parent's hardware IDs (`REV_`, `MI_`) and, when the device gives no
serial number string, takes it from the USB device's instance ID. NovaOS
answers these from its Plug and Play tree (`userland/cfgmgr32`): a device
interface path names its device, and its properties are the device's
instance ID and the interface's class, friendly name and state; a device
node also has `DEVPKEY_Device_BusTypeGuid` (from its enumerator: USB,
HID, PCI, ACPI) and `DEVPKEY_Device_ContainerId` (one per removable
device, shared by its functions and their HID collections). A USB
device's instance ID is its serial number when it has a usable one
(printable ASCII, no spaces or commas, and no other device with its
vendor and product IDs using it), as Windows makes it, so the device
keeps its ID on any port: `USB\VID_1209&PID_0008\NOVA0002`; one without
is named by where it is plugged in. hidapi 0.15.0's Windows backend,
built unchanged (`third_party/hidapi`), enumerates and talks to the
devices suite's vendor-defined device in `hidapitest` (the "hid" boot,
64-bit and 32-bit).

A KMDF HID driver cannot call `HidRegisterMinidriver` itself (the
framework owns its dispatch table), so on Windows it sits as a lower
filter under `mshidkmdf.sys`, a pass-through minidriver: its INF says
`Include = MsHidKmdf.inf`, `Needs = MsHidKmdf.NT.Services`, which makes
`mshidkmdf` the function driver, and adds its own driver as a lower
filter. NovaOS does the same: `mshidkmdf` is built into the kernel
(`kernel/nt/mshidkmdf.c`), registers with the HID class driver and
passes every request to the KMDF driver's queues, and NovaOS's own
`MsHidKmdf.inf` is in `C:\Windows\INF`. Microsoft's `vhidmini2` sample
(Windows-driver-samples `hid/vhidmini2`, KMDF), built unchanged, installs
this way on a root-enumerated device and runs through `hid.dll` (the
devices suite's `vhidmini` boot). The class driver hands the
minidriver its `HID_XFER_PACKET` with the buffer lengths Windows
gives: the input length for a set or write, the output length for a get.

### Cameras: USB Video Class

A USB camera (a device with a USB Video Class interface, `USB\Class_0E`)
gets NovaOS's UVC class driver, `usbvideo` (`kernel/nt/usbvideo.c`,
built into the kernel in place of Windows' `usbvideo.sys`). Its video
control and video streaming interfaces form one function, as Windows'
USB driver groups them. The driver reads the class descriptors, lists the
camera's modes (YUY2, NV12 and MJPEG frame sizes with their frame
rates), negotiates a mode with the UVC probe and commit requests, and
puts the frames together from the stream's payloads. A camera that
streams on a bulk endpoint is read with bulk transfers; one that streams
isochronously, as most webcams do (its streaming interface's first
setting has no endpoint and each other setting a wider isochronous one),
gets the smallest setting whose packets hold the payload size the camera
asked for, isochronous transfers while it streams, and the first setting
again when it stops. It turns on the
`KSCATEGORY_VIDEO_CAMERA` interface. Programs get the frames through
Media Foundation (`MFEnumDeviceSources`, the camera media source and the
source reader, in `mfplat.dll`, `mf.dll` and `mfreadwrite.dll`), which
reads the driver through private device controls
(`userland/include/novacam.h`) instead of Kernel Streaming. The source
reader converts YUY2 and NV12 to RGB32 and to each other, and decodes
MJPEG frames (stb_image, with the JPEG standard's typical Huffman tables
for frames that leave them out, as most cameras do) into RGB32, NV12 and
YUY2, taking an uncompressed mode of the size asked for first. The Camera app
(`camera.exe`) shows the first camera's picture; one program streams a
camera at a time, and a second gets `MF_E_HW_MFT_FAILED_START_STREAMING`.

A camera that is one function of a composite device (a webcam with a
microphone) gets a PDO of its own, as above. Not done yet: camera
controls (brightness, focus), still images, frame-based formats (H.264),
Media Foundation's media session and source events, and AVStream
(`ks.sys`) for a vendor's own camera driver.

### Network drivers (NDIS)

A Windows NDIS 6 miniport runs a network card on NovaOS's own NDIS host
(`kernel/nt/ndis.c`; Microsoft's `ndis.sys` is never shipped). The
miniport registers with `NdisMRegisterMiniportDriver`; NovaOS starts the
card as Windows does (its PnP handlers, `MiniportInitializeEx` with the
card's resources, `MiniportRestart`, the packet filter) and hands the
running adapter to its network stack. DHCP, DNS and every program's
sockets then go through the Windows driver: frames to send become
`NET_BUFFER_LIST`s for `MiniportSendNetBufferLists`, and frames the
miniport indicates are copied to the stack. Interrupts are line-based,
DMA is scatter/gather, and the link state, MAC address and OID requests
work. NDIS filter and protocol drivers are not hosted yet.

virtio-win's `netkvm.sys` on QEMU's virtio-net card is the test driver;
NovaOS's own virtio-net driver steps aside for it.

### Storage drivers (Storport)

A Windows Storport miniport runs a disk controller on NovaOS's own
Storport host (`kernel/nt/storport.c`; Microsoft's `storport.sys` is
never shipped). The miniport registers with `StorPortInitialize`; NovaOS
starts the adapter as Windows does (`HwFindAdapter` with the controller's
resources, `HwInitialize`, the passive initialization routine), sends it
SCSI requests as `STORAGE_REQUEST_BLOCK`s through `HwBuildIo` and
`HwStartIo` with a scatter/gather list, and takes completions from
`StorPortNotification(RequestComplete)` in the miniport's interrupt and
DPCs. Each disk it finds becomes a block device `storN`, whose volumes
mount like any other disk's (`[DRIVES] D: is NTFS volume "..." on stor0`).

virtio-win's `viostor.sys` on QEMU's virtio-blk card is the test driver.
Limits today: one request at a time through a 64 KiB bounce buffer,
line-based interrupts only (no MSI-X), and a started adapter cannot be
removed. NovaOS still boots from its own built-in storage drivers.

### Wi-Fi: Intel's AX211 driver (partly)

Intel's Windows Wi-Fi driver for the AX211 in the ThinkPad T14 Gen 4,
`Netwtw14.sys` (package `Netwtw6e.inf`, version 24.40.0.4, for
`PCI\VEN_8086&DEV_51F1&SUBSYS_00948086`), downloads from the Update
Catalog by hardware ID, passes the signing check (its catalog carries
Microsoft's WHQL signature) and installs unchanged. It is a WDI
miniport: it registers with `NdisMRegisterWdiMiniportDriver`, and NDIS
rather than the driver owns the Wi-Fi state machine.

What works: the NDIS host calls `MiniportWdiAllocateAdapter` and
`MiniportWdiOpenAdapter`. In the open, the driver reads its firmware
(`netwfw14.dat`, through `IoGetDriverDirectory`, `ZwOpenFile` and
`ZwReadFile`), checks its signature (ECDSA P-384 and SHA-384 through
NovaOS's `cng.sys` exports over Mbed TLS) and probes its hardware.

After the open, NovaOS's WDI command layer (`kernel/nt/wdi.c`, the part
of Windows' `wdiwifi.sys` that drives the driver) takes over. A WDI
command is an NDIS method request (`OID_WDI_*`, `NdisRequestMethod`) to
the driver's `MiniportOidRequest` whose buffer is a `WDI_MESSAGE_HEADER`
and TLVs (`kernel/nt/wdi.h`); a task is accepted by its request and
finished by an `NDIS_STATUS_WDI_INDICATION_*_COMPLETE` status indication
carrying its transaction ID. NovaOS asks the adapter's capabilities
(`OID_WDI_GET_ADAPTER_CAPABILITIES`: MAC address, firmware, radio state,
bands), creates its station port (`OID_WDI_TASK_CREATE_PORT`) and scans
on it (`OID_WDI_TASK_SCAN`): the BSS entries the driver indicates, each
with the access point's beacon or probe response, become the list
`netsh wlan show networks [mode=bssid]` prints (SSID, WPA2 or WPA3 from
the RSN element, cipher, signal, radio type, band, channel). `netsh wlan
show interfaces` lists the adapters. Disabling the device deletes the
port before the adapter closes.

What is missing: the data path (`MiniportWdiTalTxRxInitialize` and the
TAL transmit and receive handlers), `OID_WDI_SET_ADAPTER_CONFIGURATION`,
connecting (`OID_WDI_TASK_CONNECT`, the WPA2/WPA3 handshake and keys),
the adapter as the network stack's, `wlanapi.dll` for programs and
Settings' Wi-Fi page. That is the rest of roadmap step 28.1; scanning and
joining on the real card needs the T14.

QEMU has no Wi-Fi card of any kind, so the `wifi` self-test brings its
own: `wditest.sys` (`tools/drivertest/wditest.c`) is a WDI miniport on
QEMU's `edu` card whose radio hears three access points (WPA2-Personal on
2.4 GHz, WPA3-Personal on 5 GHz, an open network). The test installs it
with `pnputil`, checks the capabilities, port and scan messages from both
sides (the driver reports what it was sent), the two `netsh wlan`
listings, and the port deleted and the adapter closed on disable.

QEMU has no Intel Wi-Fi card, so the `ax211` self-test uses a PCI
stand-in: the QEMU firmware config file `opt/novaos/pci-standin`
(`VVVV:DDDD VVVV:DDDD SSSS:ssss CCSS`, here `1234:11e8 8086:51f1
8086:0094 0280`) makes `kernel/hal/pci.c` present QEMU's `edu` card with
the AX211's IDs (`[PCI] 00:03.0 1234:11e8 stands in for 8086:51f1,
subsystem 00948086 ...`). The test downloads Intel's real package through
a recorded Catalog (`tools/drivertest/ax211_catalog.py`), installs it with
test signing on (the driver is also a KMDF client), and checks that the
driver registers, reads its firmware, has its adapter allocated and
opened and reaches its hardware probe. The `edu` card is not an AX211, so
the probe fails there; that is where the stand-in stops. The probe on the
real card has not been tried.

### Display drivers (WDDM, display-only)

Windows display drivers are miniports of `dxgkrnl.sys`. NovaOS has the
display-only half of a stand-in (`kernel/nt/dxgkrnl.c`, step 1 of
[design/intel-gpu.md](design/intel-gpu.md)). A driver's
`DxgkInitializeDisplayOnlyDriver` opens `\Device\DxgKrnl` and registers
its `KMDDOD_INITIALIZATION_DATA`; the PnP manager starts it on its
adapter, and the host reads its outputs and modes through a VidPN. The
desktop then moves to the driver: what the desktop draws goes to its
`DxgkDdiPresentDisplayOnly`, and mode changes from Settings or
`ChangeDisplaySettings` become VidPNs the driver checks and commits. The
built-in Bochs VBE driver steps aside for it, and takes the screen back,
in the mode the driver left, when the driver is disabled or removed.

The test driver is `wddmdod.sys` (`tools/drivertest/wddmdod.c`), a
display-only driver for QEMU's standard VGA modelled on Microsoft's KMDOD
sample. Not there yet: render drivers (the full `DxgkInitialize`, which
Intel's GPU driver needs), interrupts and vertical blanks, power states,
and more than one source.

## Downloading drivers by hardware ID

Windows 11 finds a driver for a device that has none through Windows
Update, which offers the packages the
[Microsoft Update Catalog](https://www.catalog.update.microsoft.com)
serves, by hardware ID. NovaOS asks the same Catalog, when the user asks
it to:

    pnputil /scan-devices /search-online [/instanceid <device instance ID>] [/install]

For each device with problem code 28 (no driver), or the one named,
`pnputil`:

1. searches the Catalog (`Search.aspx?q=`) for the device's hardware IDs,
   most specific first, then its compatible IDs, until one has driver
   updates;
2. ranks them as a 64-bit Windows 11 PC would take them: Windows 11
   client before Windows 10, client before server, never ARM64, newest
   first;
3. asks the Catalog for the update's files (`DownloadDialog.aspx`, which
   gives each file's address, SHA-1 and SHA-256), downloads the cabinet to
   `C:\Windows\SoftwareDistribution\Download\Drivers\<update ID>\`, and
   refuses it unless its SHA-256 is the Catalog's;
4. expands it with `cabinet.dll` and looks for an INF with an x64 model
   (`[Models.NTamd64...]`) for one of the device's IDs, going on to the
   next update when there is none (up to three per ID);
5. stages the package in the driver store as `pnputil /add-driver` does
   (`oemN.inf`) and records where it came from under
   `HKLM\SYSTEM\DriverDatabase\DriverPackages\<package>`: `SourceName`
   ("Microsoft Update Catalog"), `SourceServer`, `SourceUpdateId`,
   `SourceTitle`, `SourceProducts`, `SourceVersion`, `SourceDate`,
   `SourceUrl`, `SourceSha256` (hex, checked), `SourceSha1` (base64, as
   the Catalog gives it), `SourceCabinet` (the kept cabinet) and
   `SourceHardwareId`.

A device whose package is already in the driver store is not searched
again. `pnputil /enum-drivers` shows a downloaded package's source, and
`pnputil /enum-devices /drivers` lists it as matching the device.

Without `/install` the package is only staged, and the device keeps
problem code 28 until `pnputil /add-driver x.inf /install` installs it.
With `/install`, each package found is installed on its device at once,
as Windows Update installs it, and the search goes round again for the
devices the new drivers bring (a USB serial converter's COM port), until
a round installs nothing new.

Nothing Microsoft or a vendor publishes is put in the NovaOS image or the
repository: the package comes from its publisher to the user's own
machine, as on Windows. `/source <url>` points the search at another
server that answers as the Catalog does. The self-tests use that so CI
needs no access to the Catalog:

| Self-test | Server | What it serves |
|---|---|---|
| `driversearch` | [`tools/drivertest/catalog_fixture.py`](../tools/drivertest/catalog_fixture.py) | pages recorded from the Catalog (`tests/selftest/devices/driversearch/catalog/`) |
| `usbserial` | [`tools/drivertest/ftdi_catalog.py`](../tools/drivertest/ftdi_catalog.py) | recorded pages and FTDI's real cabinet |
| `ax211` | [`tools/drivertest/ax211_catalog.py`](../tools/drivertest/ax211_catalog.py) | recorded pages and Intel's real cabinet |

The real cabinets are downloaded once from the Catalog's own address,
checked against the Catalog's SHA-256, and cached (`NOVA_DRIVER_CACHE`
names a folder to keep them in). Still to come: vendor feeds for packages
published outside the Catalog, and a Device Manager "Search automatically
for drivers" button.

### Driver signing

A kernel driver loads only if it is signed the way Windows 11 requires
(`kernel/nt/ci.c`), checked every time it starts:

- **Signed by Microsoft**: an Authenticode signature, embedded in the
  `.sys` or in the package's catalog (`.cat`) that `pnputil` installed,
  whose certificates chain to a Microsoft product root (Microsoft Root
  Certificate Authority 2010, or the 2001 root for older WHQL packages)
  and whose signing certificate is one for drivers: WHQL, attestation or
  Windows component signing. Packages on the Microsoft Update Catalog
  carry this, so they load as they come. Certificate expiry does not
  count, as on Windows. Cross-signed drivers (a vendor certificate under
  Microsoft's old cross-signing roots) are refused, as on Windows 11 with
  Secure Boot on.
- **Test-signed**: any other intact signature (a developer's or a test
  certificate) loads only with **test signing** on, NovaOS's equivalent
  of `bcdedit /set testsigning on`. Turn it on as an administrator and
  restart:

      reg add HKLM\SYSTEM\CurrentControlSet\Control\NovaOS /v TestSigning /t REG_DWORD /d 1 /f

  After the restart the log says `[NT] Test signing on` and
  `Control\SystemStartOptions` contains ` TESTSIGNING`, as on Windows.
  No "Test Mode" mark is drawn on the desktop yet.
- **Unsigned**, or changed after it was signed or installed: never, test
  signing or not.

A driver that fails the check does not start (Windows' error 577,
`ERROR_INVALID_IMAGE_HASH`; problem code 52 on a device), and the kernel
log says why: `[NT] x.sys: not loaded: it is not signed ...`, `... signed
by NAME (oem3.cat), which is not a Microsoft driver signature; it loads
only with test signing on`, or `... the image does not match its
signature`. Nothing is bypassed: a driver that needs more than this
(Secure Boot's measured state, HVCI, TPM attestation, as anti-cheat and
DRM drivers do) does not load.

When a package is staged (`pnputil /add-driver`, `DiInstallDriver`, a
setup program or a download from the Catalog), setupapi checks every file
against the package's catalog with `WinVerifyTrust`, as Windows does. A
file whose hash the catalog does not list is refused ("The hash for the
file is not present in the specified catalog file",
`ERROR_FILE_HASH_NOT_IN_CATALOG`) and nothing is staged. An intact
catalog is installed in `C:\Windows\System32\CatRoot` as `oemN.cat`,
where the kernel looks for it, and `pnputil /enum-drivers` shows its
signer. A package with no signed catalog, or one signed under a root
nobody trusts, is staged as unsigned (as Windows stages one after its
"Windows can't verify the publisher" warning; NovaOS does not ask), and
its drivers do not load.

Because NovaOS's own KMDF framework, KMDF loader and UMDF reflector are
test-signed, any KMDF or UMDF driver needs test signing on for now, even
a Microsoft-signed one such as `vioser.sys` or `Netwtw14.sys`. A
Microsoft-signed driver that does not use the frameworks, such as
`viostor.sys` (Storport) or `netkvm.sys` (NDIS), loads without it.

## Drivers checked in CI

Every one of these runs in the devices suite (`python3 tools/selftest.py
--suite devices`), on QEMU, in CI on every pull request that changes
code. Third-party packages are downloaded by the test (checked against pinned SHA-256s) and never
kept in the repository or the image.

| Boot | Driver | Device | What it shows |
|---|---|---|---|
| `wdm` | `wdmtest.sys`, `kmdftest.sys` (NovaOS's, test-signed) | none | the signing policy (unsigned, tampered and test-signed copies refused), `sc start`/`sc stop`, IRPs, the kernel API, KMDF on a control device |
| `edu` | `edutest.sys` (NovaOS's WDM driver) | QEMU's `edu` PCI device | `pnputil /add-driver /install`, memory BAR, interrupt, ISR and DPC, DMA, removal |
| `kmdfedu` | `kmdfedu.sys` (NovaOS's KMDF driver) | `edu` | KMDF resources, interrupt object (line, then MSI), DMA transactions, common buffer |
| `umdf` | `umdftest.dll` (NovaOS's UMDF 2 driver) | `edu` | the reflector and driver host, reads, writes and device controls |
| `vioser` | virtio-win's `vioser.sys` (KMDF) | virtio-serial | child devices, bytes exchanged with the host through `\\.\Global\<port name>` |
| `viostor` | virtio-win's `viostor.sys` (Storport) | virtio-blk | an NTFS volume mounted as D:, read and written, again after a restart |
| `netkvm` | virtio-win's `netkvm.sys` (NDIS) | virtio-net | the built-in driver stepping aside, DHCP, ping, a 32 MB download |
| `driversearch` | a NovaOS test package (`novaser.inf`) behind recorded Catalog pages | virtio-serial, with problem code 28 | `pnputil /scan-devices /search-online`: ranking, a bad SHA-256 refused, staging, no second download |
| `usbserial` | FTDI's `ftdibus.sys` and `ftser2k.sys` | QEMU's `usb-serial` (FTDI FT232) | download by hardware ID, the USB bus interface, COM3 |
| `ax211` | Intel's `Netwtw14.sys` (WDI) | `edu` standing in for the AX211 | download, signing, registration, firmware read, adapter open |
| `wifi` | `wditest.sys` (NovaOS's WDI miniport) | `edu` | the WDI command layer: capabilities, the station port, a scan and its BSS entries, `netsh wlan` |
| `wddm` | `wddmdod.sys` (NovaOS's WDDM display-only driver) | QEMU's standard VGA | the dxgkrnl host, mode changes, the screen handed back on disable |
| `hid` | NovaOS's HID class driver and `HidUsb` minidriver | a vendor-defined USB HID device (`tools/padpeer.py --kind vendor`) | `hid.dll` and setupapi on a class-driver collection: strings, preparsed data, feature, input and output reports, two handles, Raw Input, unplugging |
| `composite` | the HID class driver with `HidUsb` on each function, `kbdhid` on the keyboard's collection | a composite USB device: a keyboard and two vendor-defined HID functions (`tools/compositepeer.py`) | a node and a PDO per interface (`MI_nn` IDs), each function's own reports and feature report, Device Manager, unplugging |
| `touch` | `HidUsb`, `hiddigi` and `mouhid` on a USB digitizer (and virtio-input for QEMU's own) | a virtio multi-touch screen and tablet, and a USB touch screen, pen and precision touchpad in one device (`tools/padpeer.py --kind digitizer`) | `touchtest` on the virtio screen; `digitest`: each collection on its class driver, Raw Input's digitizer devices, a touch, the pen with its pressure, the touchpad moving the pointer, a tap and two-finger scrolling, unplugging |
| `gamepad` | `xpad` on two Xbox controllers; `HidUsb` and `gamehid` on a HID game pad | a wired Xbox 360, an Xbox One and a HID game pad (`tools/padpeer.py`) | `padtest`: XInput and DirectInput 8 on all three, motors, unplugging; `rawpadtest`: Raw Input, setupapi and `hid.dll` on the Xbox One's HID side and the game pad's class collection |
| `devmgr` | `HidUsb`, `kbdhid` and `mouhid` | QEMU's USB keyboard and tablet, and a second pair the test plugs in | Device Manager on the machine's devices; `kbdmoutest`: the class drivers on each collection, none of them a Raw Input HID device, the pointer following the new tablet, a line typed on the new keyboard, unplugging |
| `vhidmini` | Microsoft's `vhidmini2` sample (`vhidmini.sys`, KMDF 1.33, built unchanged) under NovaOS's `mshidkmdf` | none: a root-enumerated device (`ROOT\HIDCLASS\0000`, hardware ID `root\vhidmini`) | `SetupDiCreateDeviceInfo` and `DIF_REGISTERDEVICE`, `Include`/`Needs` and a lower filter, `hid.dll` on it: attributes, strings, caps, feature, input and output reports, `ReadFile`/`WriteFile`, Raw Input, Device Manager, start at boot, `pnputil /remove-device` |
| `camera` | NovaOS's UVC class driver (`usbvideo`) | a fake UVC 1.1 camera (`tools/campeer.py`) | Media Foundation on it: `MFEnumDeviceSources`, the source reader in each mode and as RGB32, the Camera app |
| `isocam` | NovaOS's UVC class driver (`usbvideo`) on isochronous URBs | a fake UVC 1.1 camera streaming isochronously in MJPEG and YUY2 (`tools/campeer.py --iso`) | the alternate setting chosen by payload size, every mode read, MJPEG decoded into RGB32, NV12 and YUY2, the Camera app |

To run one boot's tests, pass their names to `--only` (`python3
tools/selftest.py --suite devices --list` prints them).

## The default firmware set

The built-in drivers' firmware comes from two places, both downloaded at
build time and never committed:

- [`tools/firmware.json`](../tools/firmware.json) names files from
  linux-firmware, grouped in sets by device, pinned to one linux-firmware
  tag (`20260916`) and each file's SHA-256. `tools/fetch_firmware.py`
  downloads them into `third_party/firmware`.
- `tools/fetch_sof_firmware.py` downloads the audio DSP's Sound Open
  Firmware (sof-bin v2026.09.1) into `third_party/sof-bin`.

`tools/build_userland.py` puts whatever was fetched into
`C:\Windows\Firmware`, keeping linux-firmware's own layout
(`i915/adlp_dmc.bin` becomes `C:\Windows\Firmware\i915\adlp_dmc.bin`),
with each set's licence file under `C:\Windows\Firmware\LICENSES`.

| Set | Device | Linux driver | Files | Used by NovaOS |
|---|---|---|---|---|
| `intel-graphics` | Intel Iris Xe and UHD graphics, Alder Lake-P and Raptor Lake-P (the T14 Gen 4) | `i915` | DMC `adlp_dmc.bin`, GuC `adlp_guc_70.bin`, HuC `tgl_huc.bin`; 1 MB | no. The chosen GPU design runs Intel's Windows driver, which carries its own firmware ([design/intel-gpu.md](design/intel-gpu.md)); the files are kept for the native fallback |
| `intel-wifi` | Intel Wi-Fi 6E AX211 | `iwlwifi` | `intel/iwlwifi/iwlwifi-so-a0-gf-a0-89.ucode` and its `.pnvm`; 1.7 MB | no. Intel's Windows driver uses its own `netwfw14.dat` from its package |
| `intel-bluetooth` | Intel AX211 Bluetooth | `btintel` | `intel/ibt-0040-0041.sfi` and `.ddc`; 0.7 MB | no (no Bluetooth driver) |
| `qualcomm-wifi` | Qualcomm WCN6855 Wi-Fi 6E (NFA725A, the T14 Gen 4's other card option) | `ath11k` | `ath11k/WCN6855/hw2.0/` `amss.bin`, `m3.bin`, `board-2.bin`, `regdb.bin`; 12 MB | no |
| `qualcomm-bluetooth` | Qualcomm WCN6855 Bluetooth | `btqca` | `qca/wcnhpbtfw21.tlv`, `qca/wcnhpnv21.bin`; 0.2 MB | no |
| Sound Open Firmware | Intel's audio DSP, Tiger Lake to Raptor Lake | `sof-audio-pci-intel-tgl` | `C:\Windows\Firmware\Intel\sof-ipc4\<platform>\sof-<platform>.ri` | yes: `kernel/drivers/sof.c` ([hardware.md](hardware.md#the-digital-microphones-behind-the-audio-dsp)) |

So today only the DSP firmware is loaded by anything. The five
linux-firmware sets date from before the Windows driver model was chosen,
when NovaOS planned native drivers for the reference laptop's GPU, Wi-Fi
and Bluetooth; they are still fetched for release images but no NovaOS
driver reads them. QEMU's, VMware's and Hyper-V's virtual devices run no
firmware, so a virtual machine needs none of it.

Every file's licence allows redistributing it unchanged: Intel's
graphics, Wi-Fi and Bluetooth licences (`LICENSE.i915`,
`LICENCE.iwlwifi_firmware`, `LICENCE.ibt_firmware`), Qualcomm's
(`LICENSE.QualcommAtheros_ath10k` with `NOTICE.qca`, which also limits
use to Qualcomm chipsets) and the SOF project's (BSD-3-Clause with
Intel's firmware licence, `LICENCE.Intel`). None may be modified or
reverse engineered, and NovaOS does neither.

### Building with it

```bash
python3 tools/fetch_firmware.py            # every set, and the DSP firmware
python3 tools/fetch_firmware.py --list     # the sets and their sizes
python3 tools/fetch_firmware.py --set intel-graphics --no-sof
python3 tools/fetch_firmware.py --from ~/linux-firmware   # a local checkout, no network
python3 tools/fetch_sof_firmware.py        # only the DSP firmware (Raptor Lake; --all for every platform)
```

then build as usual ([building.md](building.md)). `fetch_firmware.py`
downloads from git.kernel.org, falls back to the GitLab mirror, retries
with the pauses `tools/download_cache.py` uses, checks every SHA-256 and
keeps files it already has. Without it NovaOS builds and runs; the image
just carries no firmware, and the DSP driver logs that its file is
missing.

CI fetches firmware only for release builds (the build workflow's
"Fetch release firmware" step runs when `release` is set, which the tag
workflow does; see [releasing.md](releasing.md)). Pull requests and the
rolling "latest" build carry no firmware.

### Adding firmware

1. Find the files the device's Linux driver loads (its `MODULE_FIRMWARE`
   lines, or `WHENCE` in linux-firmware) and check that `WHENCE` says the
   licence allows redistribution. A file whose licence does not is not
   added.
2. Add a set to `tools/firmware.json` with each file's `path`, `sha256`
   and `size` at the pinned tag, and its licence file under `licences`.
3. Run `tools/fetch_firmware.py --set <id>` and list the set in the table
   above.

Moving to a newer linux-firmware tag means changing `tag` and every hash
together.
