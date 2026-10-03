# Supported hardware

NovaOS is developed and tested in QEMU.  Phase 21 of the roadmap takes it
to one real PC, the **reference machine**: one widely available model
whose devices get drivers first, so "boots on real hardware" has a
concrete meaning and a machine anyone can buy to check it.

## The reference machine: Lenovo ThinkPad T14 Gen 4 (Intel)

The model with integrated graphics only (Intel Iris Xe; the GeForce MX550
option comes only with P-series processors).  Any
Core i5-1335U/1345U or i7-1355U/1365U configuration works; the vPro ones
(1345U, 1365U) have the I219-LM network controller and Intel AMT, whose
serial-over-LAN port is the only serial console the machine has.

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
| **Storage** | M.2 2280 NVMe SSD, PCIe 4.0 x4 | `nvme` | supported | NVMe driver by class 01.08.02 (Phase 18.3); install to it is step 21.5 |
| **Ethernet** | Intel I219-V or I219-LM (vPro) | `e1000e` | missing | NovaOS's `e1000e` takes only the 82574L QEMU emulates; the I219 is the chipset's built-in MAC with its PHY on a separate bus, and needs its own setup (step 21.3) |
| **Wi-Fi + Bluetooth** | Intel AX211 or Qualcomm NFA725A | `iwlwifi` / `ath11k` | missing | not needed for Phase 21: the gate goes online over Ethernet |
| **Audio** | Intel HD Audio controller (Raptor Lake-P, with an audio DSP), Realtek ALC3287 codec, two speakers, digital microphones | `snd_hda_intel` / `sof-audio-pci-intel-tgl` | missing | NovaOS's HD Audio driver takes class 04.03; with the DSP on, this controller reports class 04.01 and is otherwise HD Audio compatible; the codec's speaker pins need Realtek quirks; the digital microphones are reached only through the DSP (step 21.4) |
| **USB** | Raptor Lake-P xHCI (USB-A ports), Thunderbolt 4 xHCI (USB-C ports) | `xhci_hcd` | supported | xHCI driver by class 0C.03.30, with hubs, HID, mass storage and audio (Phases 18.1, 18.2) |
| **Thunderbolt 4 / USB4** | Intel Thunderbolt 4 controller | `thunderbolt` | missing | USB devices on the USB-C ports work through the xHCI controller; Thunderbolt docks and PCIe tunnelling do not |
| **Keyboard** | built-in keyboard on the i8042 controller | `atkbd` | supported | PS/2 keyboard driver |
| **TrackPoint** | PS/2 pointing stick | `psmouse` | supported | PS/2 mouse driver; the three buttons above the touchpad are its buttons |
| **Touchpad** | multi-touch, 61 x 115 mm, three buttons | `psmouse` / `i2c_hid` / `rmi_smbus` | partial | moves the pointer only if the firmware presents it as a PS/2 mouse; multi-touch needs I2C-HID or Synaptics RMI4 over SMBus (step 21.4) |
| **ACPI** | ACPI tables, embedded controller | `acpi` | supported | uACPI interprets the AML (Phases 18.6, 18.4): power button, S3, battery |
| **Battery and AC** | 39.3 or 52.5 Wh, USB-C power | `acpi battery` | supported | `_BIF`/`_BIX`/`_BST` through uACPI; untested on the real tables (step 21.5) |
| **Lid** | ACPI lid switch | `acpi button` | supported | lid device `PNP0C0D`, tested with a custom table in QEMU (Phase 18.6); sleeping on lid close is step 21.5 |
| **Timers** | TSC with TSC-deadline, HPET (may be off in the firmware) | — | supported | TSC-deadline APIC timer, calibrated by the HPET or the 8254 PIT (Phase 18.7); if the firmware hides the HPET and gates the PIT, calibration needs the CPU's TSC frequency (CPUID 0x15) |
| **Serial console** | none on the machine; Intel AMT serial-over-LAN on vPro models (a PCI 16550 UART) | `8250_pci` | missing | NovaOS logs to COM1 at I/O port 0x3F8 only; step 21.2 needs the AMT serial port or a USB serial adapter |
| **Camera** | 720p or 1080p+IR, USB | `uvcvideo` | missing | not needed for Phase 21 |
| **Fingerprint reader** | in the power button, USB | `libfprint` | missing | not needed for Phase 21 |
| **Smart card reader, NFC, WWAN** | optional | — | missing | not needed for Phase 21 |
| **TPM** | discrete TPM 2.0 | `tpm_tis` | missing | not needed for Phase 21 |

### What Phase 21 needs

The gate (boots to the desktop from USB, goes online, plays sound) needs
the rows marked missing or partial above for **Ethernet**, **audio** and
the **serial console**, and the touchpad for 21.4:

1. 21.2: boot from a USB stick on the GOP framebuffer, with the log on the
   AMT serial port or a USB serial adapter.
2. 21.3: the I219 Ethernet controller.
3. 21.4: HD Audio on the DSP-class controller with the ALC257 speaker
   setup; the touchpad over I2C-HID or SMBus.
4. 21.5: install to the NVMe disk, S3, battery and lid on the real ACPI
   tables.

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
| Network | Intel 82540EM, 82544 and 82545EM (`e1000`), Intel 82574L (`e1000e`), virtio-net |
| Audio | Intel HD Audio (class 04.03), USB Audio 1.0 and 2.0 |
| USB | xHCI, EHCI, OHCI, UHCI host controllers; hubs, HID keyboards, mice, tablets, touch screens and pens, mass storage, audio |
| Input | PS/2 keyboard and mouse, virtio-input tablets, touch screens and pens |
| Platform | ACPI through uACPI (power button, S3 and S5, batteries, AC, lid, thermal zones), HPET, TSC-deadline APIC timer, COM1 |
