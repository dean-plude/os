# soundtest record/capture/volume: the core boot's sound card has a
# microphone that hears a REC_HZ tone (novarun --rec, through PulseAudio);
# recording() copies each WAV off the data disk and wavcheck must find the
# tone in it.  capture turns the recording volume to a quarter half way.
DOC = ('`soundtest record`, `capture` and `volume` (`waveIn` and WASAPI capture must record the tone the '
       'microphone hears, and a quarter of the endpoint volume must sound 12 dB quieter)')
TESTS = [
    Test('soundtest record', r'soundtest record C:\rec.wav 3000', [r'recorded 132300 samples'],
         check=recording(r'C:\rec.wav', REC_HZ, 2500)),
    Test('soundtest capture', r'soundtest capture C:\cap.wav 1500', [r'captured 144000 frames .* at -12\.0 dB'],
         check=recording(r'C:\cap.wav', REC_HZ, 2500)),
    Test('soundtest volume', 'soundtest volume', [r'ok   render endpoint volume', r'ok   capture endpoint volume']),
]
