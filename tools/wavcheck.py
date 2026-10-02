#!/usr/bin/env python3
"""Summarise what a WAV recording holds: the sounding stretches, each one's
length, level and pitch (from zero crossings, so: sine-like tones).

    tools/wavcheck.py FILE.wav [--tone HZ MS]

Used with tools/novarun.py --wav to check NovaOS's sound output, and on
what NovaOS records (soundtest record/capture under novarun --rec).  With
--tone the exit status says whether the file holds a tone within 2% of HZ
lasting at least MS milliseconds (0: it does).
"""
import math, struct, sys, wave


def segments(path, show=False):
    """The sounding stretches: [(start s, length ms, rms, ~Hz, share of L=R samples)]"""
    w = wave.open(path)
    rate, ch, width, n = w.getframerate(), w.getnchannels(), w.getsampwidth(), w.getnframes()
    raw = w.readframes(n)
    if width != 2:
        sys.exit('16-bit recordings only')
    s = struct.unpack('<%dh' % (len(raw) // 2), raw)
    left = s[0::ch]
    if show:
        print(f'{path}: {rate} Hz, {ch} channels, {n / rate:.2f} s')
    found = []
    win = rate // 100                                 # 10 ms windows
    loud = [max(abs(x) for x in left[i:i + win]) > 300 for i in range(0, len(left) - win, win)]
    i = 0
    while i < len(loud):
        if not loud[i]:
            i += 1
            continue
        j = i
        while j < len(loud) and (loud[j] or any(loud[j:j + 5])):   # bridge gaps under 50 ms
            j += 1
        seg = left[i * win:j * win]
        zc = sum(1 for a, b in zip(seg, seg[1:]) if (a < 0) != (b < 0))
        rms = math.sqrt(sum(x * x for x in seg) / len(seg))
        right = s[1::ch][i * win:j * win] if ch > 1 else seg
        same = sum(1 for a, b in zip(seg, right) if abs(a - b) < 4) / len(seg)
        found.append((i * win / rate, len(seg) / rate * 1000, rms, zc / 2 / (len(seg) / rate), same))
        if show:
            print(f'  {i * win / rate:7.2f} s  {len(seg) / rate * 1000:7.0f} ms  rms {rms:7.0f}  '
                  f'~{zc / 2 / (len(seg) / rate):7.1f} Hz  L=R {same * 100:3.0f}%')
        i = j
    return found


def has_tone(path, hz, ms, show=False):
    """Whether @path holds a tone within 2% of @hz lasting @ms or longer"""
    return any(abs(s[3] - hz) <= hz * 0.02 and s[1] >= ms for s in segments(path, show))


def main(argv):
    if len(argv) == 4 and argv[1] == '--tone':
        ok = has_tone(argv[0], float(argv[2]), float(argv[3]), show=True)
        print(f'{"ok" if ok else "FAIL"}: {"a" if ok else "no"} {argv[2]} Hz tone of {argv[3]} ms or more')
        return 0 if ok else 1
    segments(argv[0], show=True)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
