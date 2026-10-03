## USB microphones, and USB audio on EHCI

USB microphones and the microphones of USB headsets now record: plug one
in and `waveIn` and WASAPI capture record from it, as Windows does, and
from the HD Audio card's microphone again when it is unplugged.
High-speed USB audio devices now work on EHCI controllers too.

- **EHCI** (`kernel/drivers/ehci.c`): isochronous transfers for
  high-speed devices, in iTDs.  Like UHCI's isochronous TDs they go
  straight into the frame list, in front of the interrupt list, and come
  out once their frame has passed; an iTD carries a packet for each
  microframe the endpoint is polled in (eight for one polled every
  microframe), and a pipe polled every 2^n frames gets one every 2^n
  frames.  Full-speed isochronous endpoints behind a high-speed hub would
  need siTDs, which are not written (their pipes are refused; on a root
  port such a device goes to the companion controller, which streams).
- **The mixer** (`kernel/drivers/audio.c`): inputs are attached by their
  drivers (`AudioInputAttach`/`Detach`: a ring, a position and a start
  and stop) instead of being the HD Audio card; the newest records, and
  unplugging it goes back to the one before.  The recording device's
  name (`waveInGetDevCaps`, WASAPI) is the input's own, e.g. "USB
  Microphone (port 1)".  Playback: behind what it has mixed, the mixer
  now keeps the rest of the playing output's ring silent.  When the mixer
  thread was held up for longer than its 80 ms lead (here while a USB
  speaker was being set up on a busy host), the device played what the
  ring held a lap earlier: 30-40 ms of the previous tone's end, which
  failed `usbaudio unplug` about one run in two in this container, on
  `main` too.  Now such a hold-up leaves a gap.
- **USB audio** (`kernel/drivers/usbaudio.c`): besides the first
  streaming interface it can play on, the driver takes the first it can
  record from: a setting whose IN endpoint carries 48 kHz 16-bit PCM,
  mono or stereo.  The IN stream runs from the moment the microphone is
  plugged in; each packet is copied into a 64 KiB ring (a mono
  microphone's samples twice, as stereo), which the mixer reads from
  while something records.
- **Tests**: QEMU has no USB microphone, no high-speed audio device and
  nothing for EHCI's iTDs to talk to, so `tools/usbredirpeer.py` is one:
  a USB Audio Class 1 headset or microphone behind QEMU's `usb-redir`
  device, speaking the usbredir protocol (its speaker's packets go to a
  WAV, its microphone sends a sine in real time).  The devices suite has
  a third boot, `usbheadset`, with no HD Audio card and a high-speed
  headset on an EHCI controller: `soundtest tone` must sound in the
  headset's WAV alone (eight packets an iTD), and `soundtest record` and
  `capture` must record its microphone's tone.  Then full-speed
  microphones, each hearing its own tone, are plugged into xHCI, OHCI and
  UHCI controllers, which must each record the newest one's tone (the
  first isochronous IN on all four controllers), and unplugging the UHCI
  one must hand recording back to the OHCI one.
- Not yet: USB Audio 2.0, siTDs, sampling rates other than 48 kHz,
  asynchronous endpoints' rate feedback, webcams, and choosing the
  playback or recording device in Settings.
