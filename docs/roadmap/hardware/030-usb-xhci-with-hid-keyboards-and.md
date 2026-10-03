- ~~USB (xHCI) with HID keyboards and mice~~ Done: keyboards, mice,
  tablets and touch screens in report protocol, on root ports or behind
  hubs, with hot-plug; USB mass storage (FAT and NTFS sticks as the next
  drive letter; NTFS ones writable); the older EHCI, OHCI and UHCI
  controllers (EHCI passing full- and low-speed devices to its
  companions), any number of controllers, keyboard LEDs, media keys and
  mice's side buttons and horizontal wheel.
  ~~Isochronous transfers, USB audio~~ Done: isochronous streams on xHCI,
  EHCI (high-speed devices, iTDs), OHCI and UHCI (alternate settings, a
  ring of transfers per pipe), and USB Audio Class 1 and 2 speakers,
  headsets and microphones as the sound output and input the mixer
  switches to when they are plugged in.  Still to do: siTDs (full-speed
  isochronous behind a high-speed hub on EHCI), webcams.
