- **Drivers**: AHCI SATA and NVMe disks (NovaOS installs to and boots from
  either), FAT16/FAT32, GPT, NTFS (read, write and format: drive C: with
  file ACLs and hard links, and other drives); Intel e1000/e1000e network cards (82540EM, 82574L
  and the I219 that Intel PCs have built in) and virtio-net; Intel High Definition Audio (playback
  and recording, laptop controllers with the audio DSP on included, the
  speakers turned off while headphones are plugged in) with a kernel mixer;
  PS/2 keyboards and mice; I2C-HID touchpads on Intel's LPSS I2C
  controllers (found through ACPI; tap to click, two-finger tap for the
  right button and two-finger scrolling as mouse-wheel input); USB (xHCI, EHCI, OHCI and UHCI controllers, any
  number of each) with hubs and HID keyboards (lock-key LEDs and media
  keys included), mice (five buttons and both wheels), tablets, pens
  (pressure, X/Y tilt, barrel rotation, barrel buttons and eraser, for
  Wintab and as `WM_POINTER` pen messages) and
  multi-touch screens (report protocol), USB sticks (FAT and NTFS, as
  the next drive letter, hot-plugged) and USB speakers, headsets and
  microphones (USB Audio Class 1 and 2 over isochronous transfers, at the
  device's own sampling rate and channel count, which the mixer runs at,
  asynchronous devices' rate feedback followed, played on and recorded
  from as soon as they are plugged in, or chosen in Settings' Sound page,
  each with its own volume; the choice and the levels are kept across
  restarts); virtio multi-touch screens,
  pens (pressure, tilt, rotation) and tablets; CMOS clock; a VBE display
  driver for QEMU's standard VGA, QXL, virtio-vga and VMware adapters,
  bochs-display and VirtualBox (resolutions switched at run time, page
  flipping, the mode set again after sleep and kept across restarts; more
  adapters, such as QEMU's secondary-vga, are more monitors of one
  desktop, arranged in Settings, each able to show DPI-aware programs
  its own DPI) and a Cirrus GD5446 one, with
  the UEFI framebuffer as the fallback; a virtio GPU driver for
  QEMU's virtio-vga and virtio-gpu-pci, whose outputs are several
  monitors on one card, plugged in and unplugged while NovaOS runs, and
  which on a 3D one (`virtio-vga-gl`) gives Mesa's Venus and virgl (App
  Store) their contexts, host-visible blobs, 3D resources, transfers and
  fences, so Vulkan, DXVK and OpenGL run on the host's GPU; ACPI power-off, reset, power buttons,
  sleep (S3, or low-power S0 idle on firmware without it), batteries and
  AC adapters, the lid, thermal zones, wake devices and PCI interrupt
  routing (AML interpreted by uACPI, with the SCI a real interrupt
  through the I/O APIC), and the embedded controller laptops keep their
  lid and battery behind.
