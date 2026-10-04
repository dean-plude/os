#!/usr/bin/env python3
"""Summarise what a WAV recording holds: the sounding stretches, each one's
length, level and pitch (from zero crossings, so: sine-like tones).

    tools/wavcheck.py FILE.wav [--tone HZ MS] [--gaps HZ]

Used with tools/novarun.py --wav to check NovaOS's sound output, and on
what NovaOS records (soundtest record/capture under novarun --rec).  With
--tone the exit status says whether the file holds a tone within 2% of HZ
lasting at least MS milliseconds (0: it does).  With --gaps it lists the
places where a steady tone of HZ jumps (frames went missing between two
loud stretches) and the exit status says whether there were none.
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


def gaps(path, hz, show=False, settle=0.1):
    """Where a steady tone of @hz skips: [(time s, jump in periods)].  Each
    rising zero crossing must come one period after the last; one that
    does not, with sound on both sides, is frames lost (a recording that
    dropped some).  A jump next to silence is not counted: the microphone's
    WAV restarting on the host puts a little silence in, not a skip.  The
    first @settle seconds are not looked at: the first recording after a
    boot can skip in its first 50 ms, before any program could fall
    behind (the host's microphone stream starting up)."""
    w = wave.open(path)
    rate, ch, n = w.getframerate(), w.getnchannels(), w.getnframes()
    if w.getsampwidth() != 2:
        sys.exit('16-bit recordings only')
    raw = w.readframes(n)
    left = struct.unpack('<%dh' % (len(raw) // 2), raw)[0::ch]
    period = rate / hz
    ups = [i + left[i] / (left[i] - left[i + 1]) for i in range(len(left) - 1) if left[i] <= 0 < left[i + 1]]
    found = []
    for a, b in zip(ups, ups[1:]):
        if a < settle * rate:
            continue
        jump = (b - a) / period
        if abs(jump - 1) < 0.05:
            continue
        lo, hi = int(a) - int(period), int(b) + int(period)
        if lo < 0 or hi > len(left):
            continue
        quiet = any(max(abs(x) for x in left[i:i + int(period)]) < 300 for i in range(lo, hi - int(period), int(period) // 2))
        if not quiet and not (found and a / rate - found[-1][0] < 3 / hz):    # (one skip, counted once)
            found.append((a / rate, jump))
            if show:
                print(f'  {a / rate:7.3f} s  the tone jumps {jump:.2f} periods')
    return found


def main(argv):
    if len(argv) == 3 and argv[1] == '--gaps':
        g = gaps(argv[0], float(argv[2]), show=True)
        print(f'{"FAIL" if g else "ok"}: {len(g)} skip(s) in the {argv[2]} Hz tone')
        return 1 if g else 0
    if len(argv) == 4 and argv[1] == '--tone':
        ok = has_tone(argv[0], float(argv[2]), float(argv[3]), show=True)
        print(f'{"ok" if ok else "FAIL"}: {"a" if ok else "no"} {argv[2]} Hz tone of {argv[3]} ms or more')
        return 0 if ok else 1
    segments(argv[0], show=True)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
