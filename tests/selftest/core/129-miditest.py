# miditest: winmm's MIDI synthesizer (TinySoundFont and NovaOS's generated
# General MIDI soundfont) through midiOut, midiStream and the MCI sequencer;
# tones() checks A4, E5 and A5 each reached the recording
DOC = '`miditest` (MIDI through `midiOut`, `midiStream` and the MCI sequencer)'
TESTS = [
    Test('miditest', 'miditest', [r'all MIDI tests passed'], check=tones(440, 659, 880)),
    Test('miditest x86', r'C:\Programs\x86\miditest.exe', [r'all MIDI tests passed'], check=tones(440, 659, 880)),
]
