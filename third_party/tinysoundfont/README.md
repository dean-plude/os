# TinySoundFont

The SoundFont 2 synthesizer (`tsf.h`) and MIDI file loader (`tml.h`) under
NovaOS's MIDI support in `winmm.dll` (`userland/winmm/midi.c`): `midiOut`,
`midiStream` and the MCI sequencer.

Version: **TinySoundFont v0.9** (`tsf.h`) and **TinyMidiLoader v0.7**
(`tml.h`), by Bernhard Schelling, from
https://github.com/schellingb/TinySoundFont.  Licensed under the MIT licence
(see `LICENSE`).  Unaltered; NovaOS builds them with `TSF_NO_STDIO` and
`TML_NO_STDIO` and loads from memory.

The instruments come from `gm.sf2`, a small General MIDI soundfont that
`tools/make_gm_soundfont.py` generates at build time (NovaOS's own, MIT),
installed in `System32\drivers` and `SysWOW64\drivers`.
