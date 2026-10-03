## DirectSound, XAudio2 and MIDI

Phase 19.5.  Three more ways for programs to make sound, all mixed by the
kernel mixer like `waveOut` and WASAPI.

- **DirectSound** (`dsound.dll`, NovaOS's own): `DirectSoundCreate`,
  `DirectSoundCreate8`, the enumerators and the COM classes.  The primary
  buffer is the kernel stream; each secondary buffer (any PCM or float
  format, static or streaming, looping, volume, pan and frequency, position
  notifications) is converted and mixed into it by a thread that stays a
  few milliseconds ahead of the card.  `DirectSoundCapture` records through
  a capture stream into the program's ring, with notifications.
- **XAudio2** 2.7, 2.8 and 2.9 and **X3DAudio** (`xaudio2_7.dll`,
  `xaudio2_8.dll`, `xaudio2_9.dll`, `x3daudio1_7.dll`) on FAudio 26.07
  (zlib).  NovaOS supplies FAudio's platform layer, which renders a quantum
  at a time into a kernel stream (folding surround to stereo), and a COM
  layer that gives each version its own vtables: voice and engine callbacks,
  sends, effect chains (the built-in reverb and volume meter, and a
  program's own XAPOs, adapted in both directions), and XAudio2 2.7's
  `CoCreateInstance` classes.
- **MIDI** in `winmm`: `midiOut` (short and system-exclusive messages, GM,
  GS and XG resets, volume), `midiStream` (tempo, time division, callbacks,
  position) and the MCI sequencer (`open`, `play`, `pause`, `seek`,
  `status` and the rest, by string or by `mciSendCommand`), playing through
  TinySoundFont (MIT).  Its instruments come from `gm.sf2`, a 75 KB General
  MIDI soundfont that `tools/make_gm_soundfont.py` builds from looped
  single-cycle waves, one per instrument family, plus a drum kit.

New self-tests play and record each one, on x64 and x86, and check that the
recording has the tones: `soundtest dsound` and `soundtest dscapture`,
`xa2test` (XAudio2 2.9 with callbacks and a volume meter, 2.7 through COM
with a submix voice, and X3DAudio panning) and `miditest`.

The first CI runs of this work never got past the build: with
`-fasync-exceptions`, clang 18 inlined the `__try` that checks a MIDI
handle into `midiOutClose` along with the synthesizer's cleanup, and its
x86-64 instruction selector never finished that function.  The check is now
kept out of line (`__declspec(noinline)`), and `midi.c` compiles in about
two seconds.
