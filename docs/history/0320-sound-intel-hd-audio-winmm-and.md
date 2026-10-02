## Sound: Intel HD Audio, winmm and WASAPI

- **Driver** (`kernel/drivers/hda.c`): finds the HD Audio controller on
  PCI (class 04/03), resets it, talks to the codecs through the CORB/RIRB
  rings and routes every output pin with something attached (line out,
  speaker, headphones) through mixers and selectors to a DAC, unmuting
  the path at 0 dB.  One output stream plays a 64 KiB ring of 48 kHz
  16-bit stereo.  It polls: no interrupt routing or shared PCI code
  changed.  Tested with QEMU's `intel-hda` + `hda-output`/`hda-duplex`.
- **Mixer** (`kernel/drivers/audio.c`): up to 32 streams of 48 kHz s16
  stereo frames, each with its own queue and left/right volume.  A
  kernel thread wakes every tick, reads the hardware's position and mixes
  the running streams into the ring 80 ms ahead of it.  A stream reports
  frames written, mixed and actually played.
- **System calls** (`kernel/um/um_audio.c`, NovaOS-private like the
  socket ones): `NtNovaAudioOpen`, `NtNovaAudioWrite`, `NtNovaAudioCtl`.
  A stream is a handle object, so it stops when the program closes it or
  exits.
- **winmm**: `waveOut*` (open/write/pause/restart/reset/close, positions,
  volume, device caps; function, window, thread and event callbacks)
  converts any 8/16/24/32-bit PCM or float format, 1 to 8 channels, any
  rate, to the mixer's format.  `PlaySound`/`sndPlaySound` play files,
  `WAVE` resources, memory images and system sound aliases
  (`HKCU\AppEvents\Schemes`); NovaOS ships no `.wav` files, so the
  default sound is a synthesized two-note chime.  `waveIn`, MIDI and the
  mixer API still report no devices.
- **kernel32 `Beep`** plays its tone on the sound card; **`MessageBeep`**
  (and so every message box) plays the system sound.
- **mmdevapi** (WASAPI): `MMDeviceEnumerator` lists one render endpoint
  with its property store (friendly name, device format); `IAudioClient`
  (and `IAudioClient2`/`3`) in shared mode with event or polling
  clients, `IAudioRenderClient`, `IAudioClock`, `ISimpleAudioVolume`,
  `IAudioStreamVolume`, `IAudioSessionControl`.  The mix format is 48 kHz
  float stereo, and other PCM/float formats are converted.  **avrt**
  (`AvSetMmThreadCharacteristics`) raises the thread's priority.
- **Testing**: `soundtest` plays tones through each path;
  `tools/novarun.py --wav out.wav` gives QEMU a sound card that records to
  a file, and `tools/wavcheck.py` lists each tone's start, length, level
  and pitch.  Checked 64- and 32-bit: `waveOut` at 22.05 kHz mono and 48 kHz
  float, WASAPI, both at once (the mixer sums them at full level),
  `PlaySound` of an 8-bit 22.05 kHz file, the default sound and `Beep`;
  no discontinuities inside any tone.
- Not yet: recording, DirectSound (`dsound.dll`), XAudio2, MIDI,
  `IAudioEndpointVolume`, exclusive mode, and real programs (VLC,
  Audacity, SDL games) on it.
