- **Drivers**: AHCI SATA and NVMe disks (NovaOS installs to and boots from
  either), FAT16/FAT32, GPT, NTFS (read, write and format: drive C: with
  file ACLs and hard links, and other drives); Intel e1000/e1000e and virtio-net network
  cards; Intel High Definition Audio (playback and recording) with a kernel
  mixer;
  PS/2 keyboards and mice; USB (xHCI, EHCI, OHCI and UHCI controllers, any
  number of each) with hubs and HID keyboards (lock-key LEDs and media
  keys included), mice (five buttons and both wheels), tablets and
  multi-touch screens (report protocol), USB sticks (FAT and NTFS, as
  the next drive letter, hot-plugged) and USB speakers, headsets and
  microphones (USB Audio Class 1 over isochronous transfers, played on and
  recorded from as soon as they are plugged in); virtio multi-touch screens; CMOS clock; a VBE display
  driver for QEMU's standard VGA, QXL, virtio-vga and VMware adapters,
  bochs-display and VirtualBox (resolutions switched at run time, page
  flipping, the mode set again after sleep and kept across restarts; more
  adapters, such as QEMU's secondary-vga, are more monitors of one
  desktop, arranged in Settings) and a Cirrus GD5446 one, with
  the UEFI framebuffer as the fallback; ACPI power-off, reset, power buttons,
  sleep (S3), batteries and AC adapters, the lid, thermal zones, wake
  devices and PCI interrupt routing (AML interpreted by uACPI, with the SCI
  a real interrupt through the I/O APIC).
