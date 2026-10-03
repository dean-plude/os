## A volume for each sound device, DirectSound and XAudio2 device lists, and the default kept across restarts

Each sound device now has its own volume, DirectSound and XAudio2 list
every device and open the one a program names, and the output and input
chosen in Settings are still the defaults after a restart, as on Windows.

- **A volume per device** (`kernel/drivers/audio.c`): the endpoint
  volume and mute that were one per direction are now each device's own,
  applied to what is mixed for it or recorded from it.
  `NtNovaAudioCtl` ops 8 and 9 take the device's id with the direction
  (`arg = capture | id << 1`, 0 for the default), so
  `IAudioEndpointVolume` acts on its endpoint's device; the volume keys
  act on the default output.  Settings' Sound page has a slider on each
  device's row (clicking it sets that device's level; clicking elsewhere
  on the row still makes it the default).  `waveOutSetVolume` on a device
  ID is now this program's volume on that device only (its handles there,
  the mapper's while that device is the default, and those it opens there
  later) instead of on all its handles.
- **Kept across restarts**: the chosen default and every level set are
  written to the registry (`HKLM\SOFTWARE\NovaOS\Audio\Render` and
  `Capture`, saved on drive C:), each device under a key that stays the
  same when it comes back: a USB device's vendor, product and port
  (`VID_46F4&PID_0002 at usb1 port 5`), the sound card's "HDA".  At boot
  (`AudioLoadSettings`, once the registry is loaded) and whenever a
  device attaches, it gets its saved level.  While the chosen default is
  attached, a device attached after it no longer takes over (it is
  ranked just below it; the log says "Attached ... (the chosen default
  stays)"); when the chosen device is absent the newest one is the
  default as before, so an unplugged favourite falls back to the
  built-in device, and plugging it in again makes it the default again.
- **DirectSound**: `DirectSoundEnumerate` and
  `DirectSoundCaptureEnumerate` list the "Primary Sound Driver" and then
  every device by its Windows name, with its endpoint's GUID
  (`PKEY_AudioEndpoint_GUID`, `{6e6f7661-6864-6100-0000-...}`) and its
  endpoint ID as the module; `DirectSoundCreate`, `Initialize` and the
  capture equivalents with one of those GUIDs play on (record from) that
  device, and `GetDeviceID` resolves the default GUIDs to the current
  default's.  The sound card's speakers and microphone keep the GUIDs
  they had.
- **XAudio2**: XAudio2 2.7's `GetDeviceCount` and `GetDeviceDetails`
  list the default (index 0, `GlobalDefaultDevice`) and then every
  output with its name and endpoint ID; `CreateMasteringVoice` with an
  index (2.7) or an endpoint ID (2.8 and 2.9) plays on that device.  The
  ID, GUID and name helpers are shared in `userland/winmm/audiodev.h`.
- **Tests**: `soundtest level out|in [LEVEL]`, `wovolume` and `dsenum`,
  `dev=NAME` for `dsound` and `dscapture`, and `xa2test devices [NAME HZ
  MS]`.  The devices suite's `usbheadset` boot (040) sets the surround
  headset to a quarter (its tone must sound a quarter as loud, the other
  devices stay at full), plays and records through DirectSound and plays
  through XAudio2 on the named surround headset, then makes it the
  default and restarts: it must be the default and at a quarter again.
  The `usbaudio` boot (020) does the same with QEMU's speakers at half
  volume.
- Not yet: siTDs (full-speed isochronous behind a high-speed hub on
  EHCI), asynchronous endpoints' rate feedback, and a mixer running at
  the device's own rate (it stays 48 kHz and converts).
