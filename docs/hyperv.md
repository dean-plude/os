# NovaOS in Hyper-V

NovaOS starts only from UEFI, so in Hyper-V it needs a **Generation 2**
virtual machine (Generation 1 has BIOS firmware only).  A Generation 2 VM
has no PS/2 controller, no USB controller and no emulated PCI devices:
its keyboard, mouse, disks, network adapter and video are *synthetic
devices* on **VMBus**, Hyper-V's own bus.  NovaOS drives the keyboard and
the mouse; the rest is listed under [What does not work yet](#what-does-not-work-yet).

## Making the VM

In Hyper-V Manager: New, Virtual Machine, and

1. **Generation 2**.
2. At least 4096 MB of startup memory; turning off dynamic memory is
   simplest (NovaOS has no balloon driver).
3. No network connection (NovaOS has no driver for Hyper-V's network
   adapter yet).
4. "Install an operating system from a bootable image file": `nova.iso`.

Then, in the VM's Settings, before starting it:

- **Security**: untick **Enable Secure Boot** (NovaOS's boot loader is not
  signed).
- **Processor**: any number of virtual processors.

Or in PowerShell (as administrator):

```powershell
New-VM -Name NovaOS -Generation 2 -MemoryStartupBytes 4GB -NoVHD
Set-VMFirmware -VMName NovaOS -EnableSecureBoot Off
Set-VMMemory -VMName NovaOS -DynamicMemoryEnabled $false
Add-VMDvdDrive -VMName NovaOS -Path C:\path\to\nova.iso
Set-VMFirmware -VMName NovaOS -FirstBootDevice (Get-VMDvdDrive -VMName NovaOS)
Start-VM NovaOS
```

Use **Basic session** in the VM window (View, untick Enhanced session):
the enhanced session is a Remote Desktop connection, which needs a
service inside the guest that NovaOS does not have.

## The keyboard and mouse

`kernel/drivers/vmbus.c` finds Hyper-V (CPUID leaf 0x40000000 says
"Microsoft Hv"), names the hypercall page, sets up the synthetic interrupt
controller (SynIC) on CPU 0, connects to VMBus (protocol 5.3 down to 3.0,
whichever the host takes first), asks for the devices and opens the ones
NovaOS drives: each gets two rings in shared memory, described to the host
as a GPADL.  `kernel/drivers/hv_input.c` speaks the two devices' protocols:

- the **synthetic keyboard** sends set-1 scan codes, posted as the PS/2
  keyboard's keys are;
- the **synthetic mouse** is HID over VMBus: its report descriptor and
  input reports go through the HID decoding USB mice use, as an absolute
  pointer (the pointer follows the host's cursor, with no capture and no
  Ctrl+Alt+Left to release it).

The device poll thread reads both rings every 10 ms tick.

### What the log says

NovaOS writes its log to the first serial port.  To read it from a
Generation 2 VM, give the VM a named pipe for COM1 (with the VM off):

```powershell
Set-VMComPort -VMName NovaOS -Number 1 -Path \\.\pipe\novaos
```

and open `\\.\pipe\novaos` with PuTTY (Connection type "Serial", the pipe
as the serial line) after starting the VM.  A working start shows:

```
[VMBUS] Connected: VMBus 5.3 (CPU 0 is virtual processor 0)
[VMBUS] Device 1: keyboard {f912ad6d-2b17-48ea-bd65-f927a61c7684}
[VMBUS] Device 2: mouse {cfa8b69e-5b4a-4cc0-b98b-8ba1a1f3f95a}
...
[VMBUS] Hyper-V keyboard: channel 1 open
[VMBUS] Hyper-V mouse: channel 2 open
[HVINPUT] keyboard: protocol 1.0 accepted
[HVINPUT] mouse: protocol 2.0 approved
[HVINPUT] mouse: 045e:0621, 67-byte report descriptor: absolute pointer
```

(The device numbers, the version and the descriptor's size depend on the
host.)  Every device the host offers is listed, with a name for the common
ones.

## What does not work yet

| Device | State |
|---|---|
| Keyboard, mouse | Work (VMBus) |
| Display | The firmware's frame buffer, at the resolution the firmware chose; no driver for the synthetic video device, so no resolution changes |
| Disks | No driver for Hyper-V's SCSI controller (storvsc): NovaOS runs live from the ISO and cannot be installed |
| Network | No driver for Hyper-V's network adapter (netvsc) |
| Sound | Hyper-V has no virtual sound card (only through an enhanced session) |
| Shutdown, time sync, heartbeat | No integration-service drivers: the host's "Shut Down" does nothing; turn the VM off or shut down from NovaOS |

## Tests

`tests/host/hv_input.c` (run by `tests/tools/test_hv_input.py`) runs the
ring buffer (wrapping, a full ring, the signalling hints) and both
protocols on the host, without a VM.  QEMU's CI machines cannot offer the
Hyper-V devices, so the VMBus connection itself is checked by hand in
Hyper-V.
