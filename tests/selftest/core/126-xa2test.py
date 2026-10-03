# xa2test: XAudio2 2.9 (callbacks, end of stream, a volume meter effect),
# 2.7 through COM (a looped buffer through a submix voice) and X3DAudio,
# on FAudio; tones() checks both tones reached the recording
DOC = '`xa2test` (XAudio2 2.9 and 2.7 voices, callbacks and effects, and X3DAudio panning)'
TESTS = [
    Test('xa2test', 'xa2test 660 2000', [r'all XAudio2 tests passed'], check=tones(660, 990)),
    Test('xa2test x86', r'C:\Programs\x86\xa2test.exe 550 1000', [r'all XAudio2 tests passed'], check=tones(550, 830)),
]
