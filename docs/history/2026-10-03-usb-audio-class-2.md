## USB Audio Class 2.0

USB Audio 2.0 headsets, speakers and microphones now play and record
like the Audio 1.0 ones: plug one in and `waveOut`, `waveIn` and WASAPI
use it, and unplugging it hands sound back to the device before.  Most
current USB headsets and DACs are Audio 2.0 devices.

- **USB audio** (`kernel/drivers/usbaudio.c`): a control interface with
  protocol 0x20 is taken as Audio 2.0.  Its streaming interfaces are the
  ones its Interface Association groups (or else every Audio 2.0
  streaming interface of the device); a setting is used when its
  class-specific AS descriptor has Format Type I with PCM in
  `bmFormats` and two channels (playing) or one or two (recording).  The
  terminal the setting links to names a clock: selectors in front of the
  clock source are switched to their first input, 48 kHz is set on the
  source where the host may set it (`CS_SAM_FREQ_CONTROL`, 4 bytes) and
  read back, and a clock running at anything else is not used.  Feature
  units take Audio 2.0's two-bit controls (unmuted, 0 dB where the host
  may set them).  High-speed endpoints polled every microframe get 6
  frames a packet.
- **Wider samples**, for Audio 1.0 and 2.0 alike: samples of 16 to 32
  bits in 2-, 3- or 4-byte slots.  The mixer's 16-bit samples go into
  the top two bytes of each slot when playing and are taken from there
  when recording (many Audio 2.0 devices offer only 24-bit samples).
- **Tests**: `tools/usbredirpeer.py --uac2` is an Audio 2.0 headset (a
  programmable clock behind a clock selector, 24-bit samples in 4-byte
  slots out and 3-byte slots in, a packet every microframe both ways).
  The devices suite's `usbheadset` boot plugs one into the xHCI
  controller after its other tests (`uac2 tone`, `uac2 record`, `uac2
  capture`, `uac2 unplug`): the tone must sound in its WAV alone, its
  microphone's 988 Hz must be recorded through `waveIn` and WASAPI, and
  unplugging it must hand recording back to the OHCI microphone.
- Not yet: rates other than 48 kHz (the clock's rate ranges are not
  read), more than two channels, asynchronous endpoints' rate feedback,
  siTDs (full-speed isochronous behind a high-speed hub on EHCI), and
  choosing the playback or recording device in Settings.
