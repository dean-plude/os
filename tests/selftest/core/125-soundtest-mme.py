# soundtest mme: waveIn recorded the way PortAudio's MME host (Audacity)
# takes it, 8 buffers of 100 ms in all, with the program's thread held up
# 250 ms once a second as a busy machine holds it up.  No buffer may be
# skipped as an overflow, and the recorded REC_HZ tone must never jump
# (wavcheck.gaps): the frames recorded while the program was late wait for
# it instead of being dropped.
DOC = ('`soundtest mme` (`waveIn` recorded as PortAudio\'s MME host does, the program held up 250 ms a second: '
       'no input overflow, and no frame missing from the recorded tone)')
TESTS = [
    Test('soundtest mme', r'soundtest mme C:\mme.wav 5000 250', [r'recorded 220500 samples .* 0 overflows'],
         check=recording(r'C:\mme.wav', REC_HZ, 4000, gapless=True), settle=3),
]
