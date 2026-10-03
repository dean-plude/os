## Sound at each device's own rate, USB audio rate feedback, and waveOut device 0 as the default

The mixer now runs at each sound device's own rate, sound reaches a
device converted once at most, asynchronous USB audio devices are fed at
the rate they ask for, and `waveOut`/`waveIn` device 0 is the default
device, as on Windows.

- **The mixer at the device's rate** (`kernel/drivers/audio.c`,
  `kernel/drivers/usbaudio.c`): each output and input has a rate (the HD
  Audio card 48 kHz, a USB device the rate its clock was set to), its
  ring holds frames at that rate, and the mixer converts each stream to
  it as it mixes (linear interpolation; a stream already at that rate is
  copied).  Capture streams are converted from the input's rate the same
  way.  `usbaudio.c` no longer converts anything: packets carry the
  ring's frames as they are (spread over the device's channels and slot
  size), a whole number each that averages the rate.  The ring is sized
  for a third of a second at the device's rate.
- **Streams at their own rate**: a stream's frames can be at any rate
  from 8 to 384 kHz (`NtNovaAudioCtl` op 13; ops 5 and 7 now report the
  default device's rate).  `waveOut` and `PlaySound` hand the kernel the
  program's own rate and convert only the sample format, so a 44.1 kHz
  sound on a 44.1 kHz USB headset arrives sample for sample, and a 22.05
  kHz one is converted once instead of twice (to 48 kHz and then to the
  device's rate).  WASAPI keeps its 48 kHz mix format.
- **Asynchronous endpoints' feedback**: a USB speaker on its own clock
  has a feedback endpoint (the second endpoint of the setting, by its
  usage, or the one its `bSynchAddress` names) that says how many frames
  it really plays: at full speed in 10.14 fixed point frames a
  millisecond, at high speed in 16.16 frames a microframe.  NovaOS
  streams it and fills each packet with that many frames on average
  instead of the nominal rate's, so the device never runs dry or
  overflows however long it plays (a value further than an eighth from
  the nominal is ignored, after trying the other format, which some
  full-speed devices use).  The log says "audio output is asynchronous"
  and the rate the device first asks for.
- **`waveOut` device 0 is the default** (`userland/winmm`): device IDs
  list the default device first and then the others oldest first, as on
  Windows, where device 0 is the preferred device (it moves when the
  default does); `waveOutMessage` and `waveInMessage` answer
  `DRVM_MAPPER_PREFERRED_GET` and `DRVM_MAPPER_CONSOLEVOICECOM_GET` with
  device 0.  `WAVE_MAPPER` still follows the default wherever it moves.
- **Tests**: `soundtest tone ... rate=N` plays at N Hz, `soundtest info`
  prints the preferred device IDs, and `tools/usbredirpeer.py
  --feedback HZ` makes the test speaker asynchronous.  The devices
  suite's `usbheadset` boot checks a 44.1 kHz tone arrives unchanged on
  the 44.1 kHz surround headset (035), that device 0 is the default and
  follows it (045), and that a full-speed USB Audio 1.0 speaker saying
  48,500 frames a second gets 48.5 frames a packet and a high-speed USB
  Audio 2.0 one saying 47,600 gets 5.95 a microframe (050).
- Not yet: siTDs (full-speed isochronous behind a high-speed hub on
  EHCI).  QEMU skips active siTDs and has no high-speed hub, so they
  cannot be tested here, and Intel chipsets since 2015 (the reference
  ThinkPad's included) have xHCI only, which handles such devices itself.
