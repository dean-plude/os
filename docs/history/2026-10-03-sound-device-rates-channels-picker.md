## Sampling rates, surround devices and a sound device picker

USB speakers, headsets and microphones now run at the sampling rate and
channel count they offer, not only 48 kHz stereo, and the sound device is
a choice: Settings has a Sound page listing every output and input, and
programs can play on or record from a device of their own choosing, as on
Windows.

- **Rates** (`kernel/drivers/usbaudio.c`): an Audio 2.0 clock source's
  rates are read from its `RANGE` (subranges of minimum, maximum and
  step; the usual rates from 8 to 192 kHz that fall in them), and an
  Audio 1.0 format's from its rate list or range.  48 kHz, the mixer's
  own, is taken where offered, else the lowest rate above it, else the
  highest below it; the clock is set to it and read back, and the
  stream runs at whatever the clock reports.  Playback and recording
  convert between the mixer's 48 kHz and the device's rate by linear
  interpolation with an exact rational step, so packets carry a varying
  whole number of frames that averages the device's rate (5 or 6 a
  microframe at 44.1 kHz).
- **Channels**: one to eight channels each way.  A surround speaker gets
  the mixer's left and right in its first two channels (front left and
  right, as Windows plays stereo on surround speakers) and silence in
  the rest, a mono one their average; a microphone with more than two
  channels is recorded from its first two.
- **Names**: devices are named like Windows endpoints after their
  product string, "Speakers (Product)" and "Microphone (Product)", with
  "2- " before the product when another device already has the name.
- **Device picker** (`kernel/drivers/audio.c`): the mixer mixes every
  attached output, each with its own position and silence, instead of
  only the newest one.  Each direction has a default, the newest device
  until another is chosen (`AudioSetDefault`); a stream plays on the
  default or on the device its program chose (`AudioRoute`), and goes to
  the default if that device is unplugged.  Inputs record only while a
  stream records from them.  `NtNovaAudioCtl` gains ops 10 (list the
  devices), 11 (choose the default) and 12 (route a stream).
- **Settings > Sound** lists the outputs and inputs, the default one's
  circle filled; clicking one makes it the default, and the page
  refreshes when a device is plugged in or out.
- **winmm**: `waveOutGetNumDevs`/`waveInGetNumDevs` count the devices
  (oldest first, the order Settings shows), `GetDevCaps` names them, and
  `waveOutOpen`/`waveInOpen` with a device ID use that device;
  `WAVE_MAPPER` follows the default.  Device presence is asked each time
  instead of once per process, so a USB device plugged in later is seen.
- **mmdevapi**: an endpoint for each device; `EnumAudioEndpoints` lists
  them, `GetDefaultAudioEndpoint` returns the default, `GetDevice` finds
  one by ID (the sound card's speakers and microphone keep their IDs), a
  device's friendly name, description and adapter come from its name,
  and an `IAudioClient` activated on an endpoint plays on (records from)
  that device.
- **Tests**: `tools/usbredirpeer.py` takes `--rates`, `--channels`,
  `--mic-channels` and `--product`.  The devices suite's `usbheadset`
  boot plugs a 44.1 kHz Audio 2.0 surround headset (six speaker channels,
  four microphone channels) and then a full-speed speaker: the tone must
  sound at its pitch in the headset's front channels with the other four
  silent, its microphone must be recorded at its pitch, `soundtest ...
  dev=NAME` must play on and record from named devices through
  `waveOut`, `waveIn` and WASAPI, and `soundtest default out|in NAME`
  (Settings' operation) must move the default.  `soundtest endpoints`
  lists the WASAPI endpoints.
- Not yet: asynchronous endpoints' rate feedback, siTDs (full-speed
  isochronous behind a high-speed hub on EHCI), a mixer running at the
  device's rate (it stays 48 kHz and converts), DirectSound and XAudio2
  device enumeration (they use the default), a volume per device (the
  endpoint volume is still one per direction), and remembering the
  chosen default across reboots.
