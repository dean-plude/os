# usbheadset, the mixer at the device's own rate: after 030 a program
# plays a 698 Hz tone through waveOut at 44.1 kHz (soundtest rate=44100)
# on the surround headset, whose clock runs at 44.1 kHz.  winmm hands the
# kernel the program's rate and the mixer runs at the device's, so nothing
# is converted: surround.wav must hold the program's samples exactly (at
# least 98% of half a second of them equal to the sine soundtest makes;
# converting 44.1 to 48 kHz and back, as before, matches almost none).
# 040 checks that the tone is in surround.wav among the others.
import math
import os
import struct
import wave

HZ, RATE = 698, 44100


def exact(nova):
    """surround.wav: half a second of the tone, sample for sample (the left
    channel; soundtest's samples are (short)(0.5 * sin(...) * 32767))"""
    path = os.path.join(nova.work, 'surround.wav')
    if not os.path.exists(path):
        return 'QEMU wrote no surround.wav'
    w = wave.open(path)
    raw = w.readframes(w.getnframes())
    left = struct.unpack('<%dh' % (len(raw) // 2), raw)[0::2]
    want = [int(0.5 * math.sin(2 * math.pi * HZ * n / RATE) * 32767) for n in range(RATE)]
    key = tuple(want[1:9])                         # (the tone starts at sample 0, which is 0)
    starts = [i - 1 for i in range(1, len(left) - 8) if tuple(left[i:i + 8]) == key]
    if not starts:
        return f'the {HZ} Hz tone\'s first samples are nowhere in surround.wav: it was converted'
    best = 0
    for p in starts:
        n = min(RATE // 2, len(left) - p)
        best = max(best, sum(1 for k in range(n) if left[p + k] == want[k]) / (RATE // 2))
    if best < 0.98:
        return f'only {best * 100:.1f}% of the tone\'s samples arrived unchanged (98% wanted)'
    return None


TESTS = [
    Test('native rate', 'soundtest tone 698 1000 rate=44100 dev=Surround',
         [r'waveOut device \d: "Speakers \(Test Surround', r'played 44100 samples'], check=exact),
]
