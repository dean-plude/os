## USB isochronous transfers and USB speakers

USB speakers and headsets now play: plug one in and NovaOS's sound moves
to it, as Windows does, and back to the sound card when it is unplugged.
They need isochronous transfers, which no USB controller driver had.

- **USB core** (`kernel/drivers/usb.c`, `usb.h`, `usb_hc.h`): pipes for
  isochronous endpoints; `UsbSetInterface` switches an interface to
  another alternate setting (audio devices keep their streaming endpoint
  out of setting 0), closing the old setting's pipes and opening the new
  one's, which xHCI learns of in one Configure Endpoint that drops and
  adds endpoints; `UsbDevConfig` hands a driver the whole configuration
  descriptor.  `UsbIsoStart` keeps a ring of transfers of one packet per
  service interval scheduled back to back; each finished one goes to the
  driver's callback (to be refilled, or read) and is scheduled again.
- **Controllers**: xHCI queues an Isoch TRB per packet ("as soon as
  possible" after the one before, each with an event); OHCI an
  isochronous ED at the end of the interrupt list with TDs of up to eight
  packets; UHCI TDs placed straight in the frame list and taken out once
  their frame has passed.  EHCI refuses isochronous pipes (no iTDs or
  siTDs yet), so a full-speed audio device works on its companion
  controller.
- **USB audio** (`kernel/drivers/usbaudio.c`): a USB Audio Class 1 driver.
  It picks the streaming setting that carries 48 kHz, 16-bit stereo PCM
  (the mixer's format), sets the sampling rate where the endpoint has
  that control, unmutes the feature unit at 0 dB, and streams the mixer's
  ring: 48 frames a millisecond, eight transfers of 8 ms in flight.
- **The mixer** (`kernel/drivers/audio.c`): outputs are attached by their
  drivers (`AudioOutputAttach`/`Detach`: a ring and a position) instead of
  being the HD Audio card; the newest plays, and unplugging it hands
  playback back to the one before.  The mixer starts without an HD Audio
  card, so a machine with only USB speakers has sound; recording still
  comes from the HD Audio card.
- **Tests**: the devices suite has a second boot, `usbaudio`, with no HD
  Audio card and QEMU `usb-audio` speakers, each recorded to its own WAV:
  one on xHCI at boot, one plugged into an OHCI and one into a UHCI
  controller while NovaOS runs, then the UHCI one unplugged.  `soundtest
  tone` plays after each step, and each speaker's WAV must hold its tones
  (the OHCI one's two: before and after the UHCI speaker came and went).
  `tools/selftest.py` tests can now run a step before their command
  (`before=`, here plugging a speaker in).
- Not yet: isochronous IN (USB microphones and webcams: QEMU 8.2 has no
  device to test them with, so the IN paths are untested), EHCI
  isochronous transfers, asynchronous endpoints' rate feedback, USB Audio
  2.0, and choosing the playback device in Settings.
