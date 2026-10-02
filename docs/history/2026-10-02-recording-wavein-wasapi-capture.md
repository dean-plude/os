## Recording: waveIn, WASAPI capture and endpoint volume

Phase 19.4.  The HD Audio driver now also programs an input stream: it
picks the codec's first input pin, preferring a microphone, then line in,
aux and CD, routes it through the mixer and selector widgets to an ADC,
and records 48 kHz 16-bit stereo into a ring the mixer thread reads every
tick.  The kernel mixer gained capture streams: each running one gets a
copy of what was recorded (the oldest frames dropped and counted when a
program falls behind), and the card records only while one runs.  Each
direction has a master volume, applied as the mixer mixes or copies.
`NtNovaAudioOpen` with its top bit set opens a capture stream, and
`NtNovaAudioCtl` gained four operations: read frames, describe the
recording device, and set and get the endpoint volume.

On top of that:
- winmm: `waveIn` (devices, capabilities, open with every callback kind,
  prepare and add buffers, start, stop, reset, position).  A thread
  converts the mixer's frames into each buffer in the program's format and
  hands it back full (`WIM_DATA`); stop hands back a partly filled one.
  The converter learned the reverse direction (any rate, 8 to 32-bit or
  float, mono as the average of both sides).
- mmdevapi: a second endpoint, the recording one ("Microphone (High
  Definition Audio)", or "Line in"), in the enumerator's lists and as the
  default `eCapture` device; its `IAudioClient` fills its buffer from a
  capture stream and `IAudioCaptureClient` hands it out ten milliseconds
  at a time, flagging a discontinuity when frames were dropped.  Both
  endpoints have `IAudioEndpointVolume` (scalar and decibel levels from
  -65.25 to 0 dB, per channel, mute, steps), kept in the kernel so every
  program sees the same volume.

To test it, `tools/novarun.py --rec FILE.wav` gives QEMU an `hda-micro`
card on a private PulseAudio server: the microphone hears FILE played over
and over into a null sink, and the speakers go to a second null sink
(saved with `--wav`).  Null sinks run on a clock, so the guest records in
real time; QEMU's ALSA backend with the file plugin delivered audio about
ten times too fast.  The core self-tests now record 3 s through `waveIn`
at 44.1 kHz mono and 3 s through WASAPI in the mix format, and
`tools/wavcheck.py --tone 523 2500` must find the 523 Hz tone in each
recording; the WASAPI test turns the recording volume down to a quarter
half way and must measure the level a quarter as loud, and `soundtest
volume` checks the volume controls on both endpoints.
