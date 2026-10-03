# Supported hardware

NovaOS is developed and tested in QEMU.  Phase 21 of the roadmap takes it
to one real PC, the **reference machine**: one widely available model
whose devices get drivers first, so "boots on real hardware" has a
concrete meaning and a machine anyone can buy to check it.

## Where NovaOS runs today

| Machine | Status |
|---|---|
| **QEMU** (Linux with or without KVM, macOS, Windows through WSL2) | Supported and tested on every pull request: a q35 machine with OVMF firmware, as [building.md](building.md#running-in-qemu) and [macos.md](macos.md) start it |
| **Other virtual machines** (VirtualBox, VMware, Hyper-V, UTM) | Untested; UTM is QEMU underneath ([macos.md](macos.md)).  NovaOS needs UEFI, an AHCI SATA or NVMe disk, and an Intel e1000/e1000e or virtio-net network card |
| **Real PCs** | **Not yet checked on any machine.**  NovaOS should start on a UEFI PC with Secure Boot off from a USB stick, on the firmware's framebuffer; the reference machine below is the first one to be checked, by hand ([install-and-power.md](install-and-power.md#checks-on-the-t14)) |
| **Macs** | Intel Macs from a USB stick, untested and with few drivers ([macos.md](macos.md#on-a-real-intel-mac--untested)); Apple Silicon Macs only in QEMU |

The tables below list, device by device, what NovaOS drives on the
reference machine and in general.

## The reference machine: Lenovo ThinkPad T14 Gen 4 (Intel)

The model with integrated graphics only (Intel Iris Xe; the GeForce MX550
option comes only with P-series processors).  Any
Core i5-1335U/1345U or i7-1355U/1365U configuration works; the vPro ones
(1345U, 1365U) have the I219-LM network controller and Intel AMT, whose
serial-over-LAN port is the only serial console the machine has (NovaOS
does not need it: started from a USB stick, it writes its log onto the
stick, step 21.2).

Why this one:

- **Wired Intel Ethernet** (I219, the `e1000e` family) on the machine
  itself, so step 21.3 does not depend on Wi-Fi or a USB adapter.  Many
  thin laptops dropped RJ-45; the T14 kept it.
- **Ordinary storage and USB**: one M.2 NVMe SSD and xHCI controllers,
  both of which NovaOS already drives by PCI class.
- **HD Audio** with a Realtek codec (ALC3287, the ALC257 family).
- **A lid, a battery, a TrackPoint and a touchpad**, so steps 21.4 and
  21.5 (input, sleep on lid close, battery) can be tested on it.
- **Common and documented**: a business laptop sold in large numbers
  since 2023 and easy to find refurbished; Lenovo publishes its full
  specification ([PSREF](https://psref.lenovo.com/Product/ThinkPad_T14_Gen_4_Intel)),
  and Linux supports all of it, which gives each driver a reference to
  compare against.
- **UEFI with Secure Boot that can be turned off**, as NovaOS's
  bootloader needs (step 21.2).

## Devices

Status, for each device the reference machine has:

- **supported**: NovaOS has a driver that takes this kind of device
  (matched by PCI class or by the same programming interface); not yet
  run on the machine itself.
- **partial**: a driver exists for the family but not for this model, or
  only part of the device works.
- **missing**: no driver.

The PCI IDs are read on the machine with the Terminal's `devices`
command (below) in step 21.2; the "Linux driver" column names the
driver that runs the same device there.

| Device | On the T14 Gen 4 (Intel) | Linux driver | NovaOS | Notes |
|---|---|---|---|---|
| **Firmware** | UEFI, Secure Boot | — | supported | NovaOS boots from UEFI with Secure Boot off (21.2) |
| **Display** | Intel Iris Xe (Raptor Lake-P), 14" 1920x1200 panel | `i915` | partial | the UEFI GOP framebuffer in the firmware's mode: one mode, no mode changes, no second monitor, no 3D on the GPU (3D runs on the CPU through llvmpipe and lavapipe) |
| **Storage** | M.2 2280 NVMe SSD, PCIe 4.0 x4 | `nvme` | supported | NVMe driver by class 01.08.02 (Phase 18.3); the Terminal's `install` copies NovaOS to it and adds a firmware boot entry (step 21.5, [install-and-power.md](install-and-power.md)); the BIOS's Intel VMD option must be off or the disk is hidden (NovaOS logs it); install checked in QEMU only |
| **Ethernet** | Intel I219-V or I219-LM (vPro) | `e1000e` | supported, unverified on hardware | the I219 is the chipset's built-in MAC with its PHY on a separate bus; NovaOS's `e1000e` drives it since step 21.3 (PR #136), tested only on QEMU's 82574L, not yet run on the machine |
| **Wi-Fi + Bluetooth** | Intel AX211 or Qualcomm NFA725A | `iwlwifi` / `ath11k` | missing | not needed for Phase 21: the gate goes online over Ethernet |
| **Audio** | Intel HD Audio controller (Raptor Lake-P, with an audio DSP), Realtek ALC3287 codec, two speakers, a headset jack, digital microphones | `snd_hda_intel` / `sof-audio-pci-intel-tgl` | partial | the HD Audio driver takes this controller with the DSP on (class 04.01) as well as off, plays through the speakers and the headphone jack, turns the speakers off while headphones are plugged in, and records from a headset's microphone (step 21.4); the digital microphones are reached only through the DSP and stay silent; checked on a modelled codec, not yet on the machine |
| **USB** | Raptor Lake-P xHCI (USB-A ports), Thunderbolt 4 xHCI (USB-C ports) | `xhci_hcd` | supported | xHCI driver by class 0C.03.30, with hubs, HID, mass storage and audio (Phases 18.1, 18.2) |
| **Thunderbolt 4 / USB4** | Intel Thunderbolt 4 controller | `thunderbolt` | missing | USB devices on the USB-C ports work through the xHCI controller; Thunderbolt docks and PCIe tunnelling do not |
| **Keyboard** | built-in keyboard on the i8042 controller | `atkbd` | supported | PS/2 keyboard driver |
| **TrackPoint** | PS/2 pointing stick | `psmouse` | supported | PS/2 mouse driver; the three buttons above the touchpad are its buttons |
| **Touchpad** | multi-touch, 61 x 115 mm, three buttons; in this generation an I2C-HID device on an Intel LPSS I2C controller (the boot log confirms it) | `i2c_hid_acpi` | partial | found through ACPI (PNP0C50), read over I2C-HID and switched to its touchpad mode, as Windows does: the pointer follows one finger, a tap is a left click and a two-finger tap a right click, pressing the pad clicks (right with two fingers on it), two fingers scroll up, down and sideways as mouse-wheel notches; a touchpad that refuses touchpad mode stays in mouse mode (pointer and clicks only); polled every 10 ms (no GPIO interrupt driver yet); checked on a modelled touchpad, not yet on the machine (step 21.4) |
| **ACPI** | ACPI tables, embedded controller | `acpi` | supported | uACPI interprets the AML (Phases 18.6, 18.4): power button, S3 where the firmware has it, else low-power S0 idle (the T14 Gen 4 has no S3), battery; the embedded controller (`ec.c`, step 21.5) that holds the lid, battery and AC events; checked on modelled tables, not yet on the machine |
| **Battery and AC** | 39.3 or 52.5 Wh, USB-C power | `acpi battery` | supported | `_BIF`/`_BIX`/`_BST` through uACPI and the embedded controller; checked on a modelled controller in QEMU, untested on the real tables |
| **Lid** | ACPI lid switch | `acpi button` | supported | lid device `PNP0C0D`, tested with a custom table in QEMU (Phase 18.6); closing it sleeps (step 21.5), which on the T14 is low-power S0 idle with the LPS0 calls, not S3; unverified on the machine |
| **Timers** | TSC with TSC-deadline, HPET (may be off in the firmware) | — | supported | TSC-deadline APIC timer, calibrated by the HPET or the 8254 PIT (Phase 18.7); if the firmware hides the HPET and gates the PIT, calibration uses the CPU's TSC frequency from CPUID leaf 0x15 (0x16 when the crystal is not reported), added in step 21.5; QEMU does not expose that leaf, so this path is unverified |
| **Serial console** | none on the machine; Intel AMT serial-over-LAN on vPro models (a PCI 16550 UART) | `8250_pci` | missing | not needed: NovaOS logs to COM1 at I/O port 0x3F8 only, which the machine lacks; started from a USB stick it writes its log into `EFI\NOVA\bootlog.txt` on the stick instead (step 21.2) |
| **Camera** | 720p or 1080p+IR, USB | `uvcvideo` | missing | not needed for Phase 21 |
| **Fingerprint reader** | in the power button, USB | `libfprint` | missing | not needed for Phase 21 |
| **Smart card reader, NFC, WWAN** | optional | — | missing | not needed for Phase 21 |
| **TPM** | discrete TPM 2.0 | `tpm_tis` | missing | not needed for Phase 21 |

### What Phase 21 needs

The gate (boots to the desktop from USB, goes online, plays sound) needs
the **Ethernet** and **audio** rows above, and the touchpad for 21.4:

1. 21.2 (done): boot from a USB stick on the GOP framebuffer, with the log
   written to `EFI\NOVA\bootlog.txt` on the stick.
2. 21.3 (done, PR #136): the I219 Ethernet controller.
3. 21.4 (done in QEMU): HD Audio on the DSP-class controller with the
   ALC257's speakers and headphone jack; the touchpad over I2C-HID.
4. 21.5 (done in QEMU): install to the NVMe disk, sleep (low-power S0 idle
   on the T14), battery and lid through the ACPI embedded controller.

Steps 21.2 to 21.5 have only been checked in QEMU (QEMU has no I219, so
21.3's driver was tested on its 82574L); each needs a hand check on the
machine ([install-and-power.md](install-and-power.md#checks-on-the-t14) for 21.2, 21.3 and 21.5).

## Checking audio and the touchpad on the T14 (step 21.4)

QEMU has no class 04.01 HD Audio controller, no Realtek codec and no I2C
controller, so the Terminal's `hwcheck` runs those code paths against
modelled devices (it is in the core self-tests).  On the machine, started
from the stick (Secure Boot off):

1. In the Terminal, `devices` lists the audio controller (`8086:51ca` or a
   neighbour, class "Audio") with the driver **HD Audio**, and one of the
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
5. Shut down and read `EFI\NOVA\bootlog.txt` on another computer: the
   lines starting `[HDA]`, `[ACPI] I2C-HID device` and `[I2C]` say what
   was found (the codec's vendor and subsystem IDs, the touchpad's ACPI
   name, address and controller), which is what a fix needs if a step
   fails.

## Listing a machine's devices

The Terminal's `devices` command (also `lspci`) lists every PCI function
NovaOS found at boot, with the driver that took it:

```
devices
```

Each line shows the function's bus address, vendor and device ID, the
vendor's name, its class and the driver (`(bridge)` for bridges); a
function without a driver is shown in red, and the last line counts
them.  Typed after `serial on`, the list also goes to the serial port.
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

## Devices NovaOS drives

For reference, every driver NovaOS has, by device:

| Kind | Devices |
|---|---|
| Display | UEFI GOP framebuffer (any PC); Bochs VBE (QEMU `std`, `bochs-display`, `secondary-vga`), QXL, VMware SVGA II, Cirrus CL-GD5446, virtio-gpu (2D, and 3D through Venus and virgl) |
| Storage | AHCI SATA (class 01.06.01), NVMe (class 01.08.02), USB mass storage; FAT and NTFS |
| Network | Intel 82540EM, 82544 and 82545EM (`e1000`), Intel 82574L and I219-LM/I219-V (`e1000e`), virtio-net |
| Audio | Intel HD Audio (class 04.03, and Intel's class 04.01 controllers with the audio DSP on), with headphone-jack sensing; USB Audio 1.0 and 2.0 |
| USB | xHCI, EHCI, OHCI, UHCI host controllers; hubs, HID keyboards, mice, tablets, touch screens and pens, mass storage, audio |
| Input | PS/2 keyboard and mouse, I2C-HID precision touchpads (tap to click, two-finger scrolling) on Intel LPSS I2C controllers, virtio-input tablets, touch screens and pens |
| Platform | ACPI through uACPI (power button, S3 and S5, low-power S0 idle, batteries, AC, lid, thermal zones, embedded controller), HPET or CPUID-calibrated TSC-deadline APIC timer, COM1 |
