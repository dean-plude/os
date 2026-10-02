# soundtest: the core suite's boot has an Intel HD Audio card recorded to a
# WAV; tones() checks the recording once QEMU has quit
DOC = '`soundtest` (the recorded WAV must hold the tones played)'
TESTS = [
    Test('soundtest tone', 'soundtest tone 440 1000', [r'played \d+ samples']),
    Test('soundtest wasapi', 'soundtest wasapi 660 1000', [r'played \d+ frames'], check=tones(440, 660)),
]
