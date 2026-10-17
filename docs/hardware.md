# Supported hardware

NovaOS is developed and tested in QEMU. Its first real PC is the
**reference machine**, a Lenovo ThinkPad T14 Gen 4 (Intel): one widely
available model whose devices get drivers first, so "runs on real
hardware" has a concrete meaning and a machine anyone can buy to check
it.

**Nothing has run on the reference machine yet.** Every driver below for
its devices has been tested in QEMU, against QEMU's own devices or
against modelled ones; the checks on the machine itself are written down
and wait for someone to do them by hand (roadmap step 23.1).

## Where NovaOS runs today

| Machine | Status |
|---|---|
| **QEMU** (Linux with or without KVM, macOS, Windows through WSL2) | Supported, and tested in CI on every pull request that changes code: a q35 machine with OVMF firmware, started as [building.md](building.md#running-in-qemu) and [macos.md](macos.md) describe |
| **Hyper-V** | A Generation 2 VM with Secure Boot off. NovaOS has drivers for the VMBus keyboard and mouse, its SCSI disks (so it installs to a VHDX), its synthetic video (resolution changes), its network adapter and the shutdown, heartbeat, time sync and data exchange services (the VM's addresses in Hyper-V Manager), but **has not been run on real Hyper-V yet** (roadmap step 23.4) ([hyperv.md](hyperv.md)) |
| **Other virtual machines** (VirtualBox, VMware, UTM) | Untested. UTM is QEMU underneath ([macos.md](macos.md)). NovaOS needs UEFI, an AHCI SATA or NVMe disk for its built-in drivers, and an Intel e1000/e1000e or virtio-net network card |
| **Real PCs** | **Not yet run on any machine.** NovaOS should start from a USB stick on a UEFI PC with Secure Boot off, on the firmware's framebuffer. The reference machine below is the first one to be checked ([install-and-power.md](install-and-power.md#checks-on-the-t14)) |
| **Macs** | Intel Macs from a USB stick, untested and with few drivers ([macos.md](macos.md)); Apple Silicon Macs only in QEMU |

A device NovaOS has no built-in driver for can also be run by its vendor's
Windows driver package, installed with `pnputil` or downloaded by
hardware ID ([drivers.md](drivers.md)). That works today for some network,
storage, USB and display-only drivers, all tested only in QEMU.

## The reference machine: Lenovo ThinkPad T14 Gen 4 (Intel)

The model with integrated graphics only (Intel Iris Xe; the GeForce MX550
option comes only with P-series processors). Any Core i5-1335U/1345U or
i7-1355U/1365U configuration will do. The vPro ones (1345U, 1365U) have
the I219-LM network controller and Intel AMT, whose serial-over-LAN port
is the only serial console the machine has. NovaOS does not need it:
started from a USB stick, it writes its log onto the stick.

Why this one:

- **Wired Intel Ethernet** (I219, the `e1000e` family) on the machine
  itself, so going online does not depend on Wi-Fi or a USB adapter.
- **Ordinary storage and USB**: one M.2 NVMe SSD and xHCI controllers,
  which NovaOS drives by PCI class.
- **HD Audio** with a Realtek codec (ALC3287, the ALC257 family).
- **A lid, a battery, a TrackPoint and a touchpad**, to test input,
  sleep on lid close and battery reporting.
- **Common and documented**: a business laptop sold in large numbers
  since 2023 and easy to find refurbished. Lenovo publishes its full
  specification ([PSREF](https://psref.lenovo.com/Product/ThinkPad_T14_Gen_4_Intel)),
  and Linux supports all of it, which gives each driver a reference to
  compare against.
- **UEFI with Secure Boot that can be turned off**, which NovaOS's
  unsigned boot loader needs.

## Devices

Status, for each device the reference machine has:

- **supported**: NovaOS has a driver that takes this kind of device
  (matched by PCI class or by the same programming interface). Not yet
  run on the machine itself.
- **partial**: a driver exists for the family but not for this model, or
  only part of the device works.
- **missing**: no driver.

The PCI IDs are to be read on the machine with the Terminal's `devices`
command ([below](#listing-a-machines-devices)). The "Linux driver" column
names the driver that runs the same device on Linux.

| Device | On the T14 Gen 4 (Intel) | Linux driver | NovaOS | Notes |
|---|---|---|---|---|
| **Firmware** | UEFI, Secure Boot | — | supported | NovaOS boots from UEFI with Secure Boot off |
| **Display** | Intel Iris Xe (Raptor Lake-P), 14" 1920x1200 panel | `i915` | partial | the UEFI GOP framebuffer in the firmware's mode: one mode, no mode changes, no second monitor, no 3D on the GPU (3D runs on the CPU through llvmpipe and lavapipe). The plan is Intel's own Windows driver through a WDDM host ([design/intel-gpu.md](design/intel-gpu.md)); its first step, the display-only host, is merged and tested in QEMU with a test driver; Intel's driver needs the next step and the machine |
| **Storage** | M.2 2280 NVMe SSD, PCIe 4.0 x4 | `nvme` | supported | NVMe driver by class 01.08.02; the Terminal's `install` copies NovaOS to it and adds a firmware boot entry ([install-and-power.md](install-and-power.md)); the firmware's Intel VMD option must be off or the disk is hidden (NovaOS logs it); install tested in QEMU only |
| **Ethernet** | Intel I219-V or I219-LM (vPro) | `e1000e` | supported | the I219 is the chipset's built-in MAC with its PHY on a separate bus; NovaOS's `e1000e` driver brings it up, tested only on QEMU's 82574L ([ethernet.md](ethernet.md)) |
| **Wi-Fi** | Intel AX211 or Qualcomm NFA725A | `iwlwifi` / `ath11k` | missing | Intel's Windows driver (`Netwtw14.sys`) downloads, installs and opens its adapter on a stand-in device in QEMU, and NovaOS's WDI command layer can scan with a WDI driver (tested with its own in QEMU), but connecting is not written yet and the real card has not been tried (roadmap step 28.1, [drivers.md](drivers.md#wi-fi-intels-ax211-driver-partly)). Nothing for the Qualcomm card |
| **Bluetooth** | on the same card | `btintel` / `btqca` | missing | roadmap step 28.2 |
| **Audio** | Intel HD Audio controller (Raptor Lake-P, with an audio DSP), Realtek ALC3287 codec, two speakers, a headset jack, digital microphones | `snd_hda_intel` / `sof-audio-pci-intel-tgl` | partial | the HD Audio driver takes this controller with the DSP on (class 04.01) as well as off, plays through the speakers and the headphone jack, turns the speakers off while headphones are plugged in, and records from a headset's microphone. The digital microphones are reached only through the DSP: NovaOS reads them from the ACPI NHLT table, boots the DSP with Sound Open Firmware and runs a capture pipeline, offered to programs as "Microphone Array (DSP)" ([below](#the-digital-microphones-behind-the-audio-dsp)). Tested on a modelled codec and DSP |
| **USB** | Raptor Lake-P xHCI (USB-A ports), Thunderbolt 4 xHCI (USB-C ports) | `xhci_hcd` | supported | xHCI driver by class 0C.03.30, with hubs, HID, mass storage, audio and game controllers |
| **Thunderbolt 4 / USB4** | Intel Thunderbolt 4 controller | `thunderbolt` | missing | USB devices on the USB-C ports work through the xHCI controller; Thunderbolt docks and PCIe tunnelling do not |
| **Keyboard** | built-in keyboard on the i8042 controller | `atkbd` | supported | PS/2 keyboard driver |
| **TrackPoint** | PS/2 pointing stick | `psmouse` | supported | PS/2 mouse driver; the three buttons above the touchpad are its buttons |
| **Touchpad** | multi-touch, 61 x 115 mm, three buttons; an I2C-HID device on an Intel LPSS I2C controller | `i2c_hid_acpi` | partial | found through ACPI (PNP0C50), read over I2C-HID and switched to its touchpad mode, as Windows does: the pointer follows one finger, a tap is a left click and a two-finger tap a right click, pressing the pad clicks (right with two fingers on it), a tap then a touch drags (the left button held until the finger lifts), two fingers scroll up, down and sideways as mouse-wheel notches and the scrolling coasts on after a flick until a touch stops it. A touchpad that refuses touchpad mode stays in mouse mode (pointer and clicks only). Read when its interrupt pin fires (its ACPI GpioInt, on the chipset's GPIO controller, `INTC1055` on this generation as far as Linux and OpenBSD know), polled every 10 ms only if no interrupt can be had. Tested on a modelled touchpad and GPIO controller |
| **ACPI** | ACPI tables, embedded controller | `acpi` | supported | uACPI interprets the AML: power button, S3 where the firmware has it, else low-power S0 idle (the T14 Gen 4 has no S3), battery, lid, thermal zones; the embedded controller driver (`kernel/hal/ec.c`) holds the lid, battery and AC events. Tested on modelled tables |
| **Battery and AC** | 39.3 or 52.5 Wh, USB-C power | `acpi battery` | supported | `_BIF`/`_BIX`/`_BST` through uACPI and the embedded controller; tested on a modelled controller in QEMU |
| **Lid** | ACPI lid switch | `acpi button` | supported | lid device `PNP0C0D`; closing it sleeps, which on the T14 is low-power S0 idle with the LPS0 calls, not S3. Tested with a custom ACPI table in QEMU |
| **Timers** | TSC with TSC-deadline, HPET (may be off in the firmware) | — | supported | TSC-deadline APIC timer, calibrated by the HPET; without one, from CPUID leaf 0x15 (0x16 when the crystal is not reported), then the 8254 PIT ([install-and-power.md](install-and-power.md#timers-without-an-hpet)); QEMU's usual CPU models do not report leaf 0x15, so that path has had little testing |
| **Serial console** | none on the machine; Intel AMT serial-over-LAN on vPro models (a PCI 16550 UART) | `8250_pci` | missing | not needed: NovaOS logs to COM1 at I/O port 0x3F8 only, which the machine lacks; started from a USB stick it writes its log into `EFI\NOVA\bootlog.txt` on the stick instead |
| **Camera** | 720p or 1080p+IR, USB | `uvcvideo` | partial | the UVC class driver and Media Foundation capture run a camera with a bulk streaming endpoint (tested with a fake camera in QEMU); the T14's webcam streams isochronously, which NovaOS's Windows USB interface lacks yet (roadmap step 28.3). The IR camera (Windows Hello) is not supported |
| **Fingerprint reader** | in the power button, USB | `libfprint` | missing | |
| **Smart card reader, NFC, WWAN** | optional | — | missing | |
| **TPM** | discrete TPM 2.0 | `tpm_tis` | missing | |

### What has been checked, and where

| Area | Tested in QEMU by | On the T14 |
|---|---|---|
| Starting from a USB stick, log on the stick | devices suite: `usbboot`, `cdboot` | not yet |
| I219 Ethernet | network suite's e1000e boot, on QEMU's 82574L ([ethernet.md](ethernet.md#testing-it)) | not yet |
| HD Audio on the DSP-class controller, the touchpad over I2C-HID and its GPIO interrupt, the DSP microphones | core suite: `hwcheck`, against modelled devices | not yet ([below](#checking-audio-and-the-touchpad-on-the-t14-step-214)) |
| Install to NVMe, sleep in S0 idle, battery and lid through the embedded controller, timers without an HPET | devices suite: `laptop` | not yet ([install-and-power.md](install-and-power.md#checks-on-the-t14)) |

## Checking audio and the touchpad on the T14 (step 21.4)

QEMU has no class 04.01 HD Audio controller, no Realtek codec, no I2C
controller and no Intel GPIO controller, so the Terminal's `hwcheck` runs
those code paths against modelled devices (it is in the core self-tests).
On the machine, started from the stick (Secure Boot off), the checks are
these. None of them has been done yet.

1. In the Terminal, `devices` lists the audio controller (`8086:51ca` or a
   neighbour, class "Audio (DSP)") with the driver **HD Audio**, and one of the
   I2C controllers (`8086:51e8`-`51eb` or `51c5`/`51c6`) with **I2C
   (touchpad)**.
2. `soundtest tone 440 1000` plays a tone through the speakers.  Plug
   headphones in: within half a second the speakers go quiet and the
   tone plays in the headphones; unplug them and the speakers come back.
3. Move a finger on the touchpad: the pointer follows; pressing the pad
   down clicks, and pressing it with two fingers on it right-clicks.  The
   TrackPoint and its three buttons work as a PS/2 mouse as before.
4. Gestures: tap the pad lightly with one finger (a short touch that
   hardly moves): a left click, so tapping an icon on the desktop twice
   opens it.  Tap with two fingers at once: a right click (a context
   menu).  Open something long (the Terminal after `help`, or a web page
   in NetSurf) and slide two fingers down the pad: the text follows the
   fingers, a wheel notch for every 3 mm; slide them up to go back, and
   sideways in a window with a horizontal scroll bar to scroll left and
   right.  A palm resting on the pad while typing moves nothing.  If
   none of this works but the pointer moves, the touchpad refused its
   touchpad mode: the boot log's `[I2C]` line then ends in `touchpad
   (mouse mode)` instead of `precision touchpad (tap to click,
   two-finger scrolling)`.
5. The touchpad's interrupt: in the Terminal, `hwcheck` prints
   `touchpad interrupt: ... is the machine's own GPIO controller` (it
   leaves real pins alone).  The boot log's `[GPIO]` lines name the GPIO
   controller (`\_SB_.GPI0 (INTC1055)`, a layout and `IRQ 14`) and the
   touchpad's pin (`pin N = pad GPP_xNN`), and its `[I2C]` line ends in
   `interrupt on GPIO pin N`.  The pointer must move as smoothly as in
   step 3 and stop at once when the finger lifts.  An `[I2C]` line ending
   in `polled (...)`, or a later one saying `reports without its
   interrupt ... polled from now on`, means the touchpad still works but
   the interrupt didn't: the reason it gives and the `[GPIO]` lines are
   what a fix needs.  Repeat after closing and opening the lid (sleep).
6. Shut down and read `EFI\NOVA\bootlog.txt` on another computer: the
   lines starting `[HDA]`, `[ACPI] I2C-HID device`, `[GPIO]` and `[I2C]`
   say what was found (the codec's vendor and subsystem IDs, the
   touchpad's ACPI name, address, controller and interrupt pin), which
   is what a fix needs if a step fails.

## Listing a machine's devices

The Terminal's `devices` command (also `lspci`) lists every PCI function
NovaOS found at boot, with the driver that took it:

```
devices
```

Each line shows the function's bus address, vendor and device ID, the
vendor's name, its class and the driver (`(bridge)` for bridges); a
function without a driver is shown in red, and the last line counts
them.  A device run by a Windows driver package shows the package's
service name as its driver ([drivers.md](drivers.md)).  Typed after
`serial on`, the list also goes to the serial port.
In QEMU's q35 machine it looks like this:

```
Slot     ID         Vendor      Class                Driver
00:00.0  8086:29c0  Intel       Host bridge          (bridge)
00:01.0  1234:1111  QEMU        VGA display          Bochs VBE
00:02.0  8086:2668  Intel       HD Audio             HD Audio
00:05.0  1b36:000d  QEMU        USB (xHCI)           xHCI
00:1f.0  8086:2918  Intel       ISA bridge           (bridge)
00:1f.2  8086:2922  Intel       SATA (AHCI)          AHCI
00:1f.3  8086:2930  Intel       SMBus                no driver
devices: 7 PCI functions, 4 with a driver, 2 bridges, 1 without a driver
```

On another PC, the same list shows which of its devices NovaOS runs and
which it does not; reports from other machines are welcome as issues.

## The digital microphones, behind the audio DSP

The T14's built-in microphones are digital microphones (DMICs) wired to
the audio DSP inside the HD Audio controller, not to the Realtek codec,
so the codec path that plays sound and records from a headset cannot
hear them.  The DSP has to run firmware that clocks the microphones and
passes their samples to NovaOS over an HD Audio stream.  NovaOS uses
Intel's signed build of Sound Open Firmware (SOF), which the SOF project
publishes at <https://github.com/thesofproject/sof-bin/releases>
(BSD-3-Clause with Intel's firmware licence, which allows
redistribution).  The firmware is not in the repository: building it in
takes one command before the build, which downloads the pinned release
(v2026.09.1), checks its SHA-256 and puts the Raptor Lake firmware in
`third_party/sof-bin` (`--all` takes every Tiger Lake to Raptor Lake
one):

```bash
python3 tools/fetch_sof_firmware.py
```

The build then puts it at
`C:\Windows\Firmware\Intel\sof-ipc4\rpl\sof-rpl.ri` (with
`LICENCE.Intel` beside it).  Without it NovaOS builds and runs as before
and the boot log says the firmware is missing.

Where it stands (`kernel/drivers/sof.c`):

1. Done: at boot, on an Intel controller with its DSP on (class 04.01) of
   the Tiger Lake to Raptor Lake generation (cAVS 2.5), NovaOS reads the
   ACPI NHLT table for digital microphones (how many, their formats and
   the configuration the firmware needs for them), checks the firmware
   file, powers the DSP's first core, has its ROM load the firmware from
   a host DMA stream, takes the firmware's FW_READY message and asks it
   for its version over IPC4.  It happens in the background and leaves
   the speakers, headphones and headset microphone alone; if a step
   fails, the DSP is switched off again and the boot log says which.
2. Done: the capture pipeline.  Once the firmware runs, NovaOS asks it
   over IPC4 for a pipeline with two copier modules: one reading the
   DMIC gateway, configured with NHLT's blob for the first format, bound
   to one writing through a host input gateway into the last HD Audio
   input stream, decoupled from the link (the codec's recording keeps
   the first).  The pipeline is paused, the stream's DMA started and the
   pipeline set running, so the microphones' samples (48 kHz or 16 kHz
   as NHLT says, 16-bit, one channel per microphone) flow into a 128 KiB
   ring in memory.  If the firmware refuses a step, the pipeline is
   deleted and the stream given back; the firmware stays running.
3. Done: the recording device.  The ring is an input of the mixer,
   "Microphone Array (DSP)", the default input from then on (as on
   Windows; Settings > Sound can choose the headset microphone instead),
   listed by Settings > Sound, `waveIn`, WASAPI and DirectSound, so
   Audacity and any other program can record from it.  The mixer turns
   the microphones' rate and channel count (two or four; the even ones
   left, the odd ones right) into what the program asked for.  The
   pipeline runs only while a program records: when the last one stops
   the DSP's thread pauses it (SET_PIPELINE_STATE PAUSED, the stream's
   DMA stopped), and runs it again when one starts.  Sleep takes the
   DSP's power and with it the firmware and the pipeline, so after a
   wake the thread boots the DSP again from the same firmware file and
   builds the pipeline again; the device stays, and a program recording
   across the sleep goes on recording.

QEMU has no audio DSP, so the Terminal's `hwcheck` runs the NHLT reader,
the firmware's manifest, the whole boot, the capture pipeline (pausing
and running it again too) and a boot again after a modelled sleep against
a modelled DSP that checks each IPC4 message and fills the ring with a
test tone (core self-test `hwcheck dsp`).  `hwcheck mic` boots such a
model live, whose DMA writes a 1 kHz tone, and attaches it as
"Microphone Array (DSP model)", so programs can record from it in QEMU;
`hwcheck mic sleep` takes its power as S3 does and has the resume path
boot it again (core self-test `hwcheck mic`: `waveIn`, WASAPI and
DirectSound list it, and `soundtest record` and `capture` hear the tone
before and after the sleep).  On the T14, from a stick built after
`tools/fetch_sof_firmware.py` (unverified on hardware: nothing has run on
a real T14 yet):

1. In the Terminal, `hwcheck`: its line `audio DSP on this machine:`
   should read `firmware 2.12.0.1 running, 2 digital microphones (48000
   Hz, 2 channels), paused while no program records` (the version is
   the firmware's; the count, rate and channels are NHLT's).
2. Settings > Sound lists "Microphone Array (DSP)" under the recording
   devices, chosen as the default.  In the Terminal, `soundtest record
   C:\mic.wav 5000` while talking near the laptop, then `soundtest play
   C:\mic.wav`: the speakers play back what was said.  While a recording
   runs, `hwcheck` in a second Terminal says `recording, level N%`, the
   level rising above the quiet room's when someone talks.
3. Audacity: pick "Microphone Array (DSP)" as the recording device,
   record a few seconds of speech and play it back.
4. Close the lid to sleep, open it to wake the laptop, and repeat step 2:
   the boot log gets a `[DSP] After sleep: firmware ... running` line and
   the recording works as before.
5. If step 1 names a failed step instead, shut down and read
   `EFI\NOVA\bootlog.txt`: the `[DSP]` lines give the NHLT contents, the
   firmware's version and size, the stream the code loader used, the
   ROM's status and error codes, and the capture pipeline's step the
   firmware refused (with its status), which is what a fix needs.
6. Speakers, headphones and the headset microphone work as in the check
   above, whatever the DSP line says.

## Devices NovaOS drives

For reference, every built-in driver NovaOS has, by device. They are all
built into the kernel. The firmware some of them need is described in
[drivers.md](drivers.md#the-default-firmware-set).

| Kind | Devices |
|---|---|
| Display | UEFI GOP framebuffer (any PC); Bochs VBE "DISPI" adapters: QEMU's standard VGA, `bochs-display` and `secondary-vga`, QXL, virtio-vga, VMware SVGA II and VirtualBox VGA; Cirrus CL-GD5446; virtio-gpu (2D with several monitors, and 3D through Venus and virgl); Hyper-V's synthetic video ([hyperv.md](hyperv.md)) |
| Storage | AHCI SATA (class 01.06.01), NVMe (class 01.08.02), USB mass storage, Hyper-V's synthetic SCSI disks ([hyperv.md](hyperv.md)); FAT and NTFS volumes |
| Network | Intel 82540EM, 82544 and 82545EM (`e1000`), Intel 82574L and I219-LM/I219-V (`e1000e`), virtio-net, Hyper-V's synthetic network adapter ([hyperv.md](hyperv.md#network)); one adapter at a time |
| Audio | Intel HD Audio (class 04.03, and Intel's class 04.01 controllers with the audio DSP on), with headphone-jack sensing; the audio DSP of Tiger Lake to Raptor Lake (boots Sound Open Firmware and records the digital microphones through it); USB Audio 1.0 and 2.0 |
| USB | xHCI, EHCI, OHCI and UHCI host controllers; hubs, HID keyboards, mice, tablets, touch screens and pens, mass storage, audio |
| Game controllers | wired Xbox 360 and Xbox One controllers on USB, and HID game pads and joysticks |
| Input | PS/2 keyboard and mouse; I2C-HID precision touchpads on Intel LPSS I2C controllers, interrupt-driven through Intel's GPIO controllers (Tiger Lake to Meteor Lake); virtio-input tablets, touch screens and pens; Hyper-V's synthetic keyboard and mouse over VMBus ([hyperv.md](hyperv.md)) |
| Platform | ACPI through uACPI (power button, S3 and S5, low-power S0 idle, batteries, AC, lid, thermal zones, embedded controller); HPET, or a TSC-deadline APIC timer calibrated without one; COM1 |

Devices with no built-in driver can be run by Windows driver packages:
NovaOS hosts KMDF, UMDF 2, USB client, NDIS, Storport and WDDM
display-only drivers ([drivers.md](drivers.md)).
