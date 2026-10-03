# soundtest dsound/dscapture: DirectSound streams a tone through a looping
# buffer refilled at position notifications, then loops a static buffer an
# octave up (SetFrequency); DirectSoundCapture records the microphone's
# REC_HZ tone (novarun --rec) into a WAV that wavcheck must find it in.
DOC = ('`soundtest dsound` and `dscapture` (DirectSound playback with notifications, frequency and volume, '
       'and DirectSoundCapture recording the microphone)')
TESTS = [
    Test('soundtest dsound', 'soundtest dsound 880 2000', [r'streamed 44100 samples, [45] notifications',
                                                          r'static buffer at 1760 Hz'],
         check=tones(880, 1760)),
    Test('soundtest dscapture', r'soundtest dscapture C:\dsc.wav 3000', [r'captured 132300 samples'],
         check=recording(r'C:\dsc.wav', REC_HZ, 2500)),
]
