# usbaudio, a device's own volume and the default kept across a restart:
# after 010 speakers 1 (xHCI) and 2 (OHCI) are attached and 2, the newer,
# is the default.  Speaker 1 is made the default (as Settings' Sound page
# does) and its endpoint volume set to a half (IAudioEndpointVolume on the
# default endpoint), which must leave speaker 2's at full.  After a restart
# (drive C: and the registry kept) speaker 1 must be the default again,
# although both speakers attach at boot, and still at half volume: the
# tones played on it before and after the restart must be half as loud as
# 010's tone at full volume, and usb1.wav must hold nothing but those.
import os
import re
import time
import wavcheck


def _wait_log(nova, pattern, secs=60):
    """Wait until the serial log after the restart matches @pattern"""
    for _ in range(secs * 4):
        log = open(nova.serial_path, 'rb').read().decode('latin-1')
        if re.search(pattern, log[log.rfind('Saved default output'):]):
            return
        time.sleep(0.25)
    print(f'usbaudio: no "{pattern}" in the serial log', flush=True)


def both_attached(nova):
    _wait_log(nova, r'Speakers \(QEMU USB Audio\)[\s\S]*Speakers \(2- QEMU USB Audio\)|'
                    r'Speakers \(2- QEMU USB Audio\)[\s\S]*Speakers \(QEMU USB Audio\)')


def half_as_loud(nova):
    """usb1.wav: 440 Hz at full volume, then 880 and 990 Hz at half"""
    bad = tones(440, 880, 990, wav='usb1.wav', only=True)(nova)
    if bad:
        return bad
    segs = wavcheck.segments(os.path.join(nova.work, 'usb1.wav'))
    level = {hz: max((s[2] for s in segs if abs(s[3] - hz) < hz * 0.05), default=0) for hz in (440, 880, 990)}
    for hz in (880, 990):
        ratio = level[hz] / level[440] if level[440] else 0
        if not 0.4 < ratio < 0.6:
            return f'the {hz} Hz tone played at {ratio:.2f} of full volume, not a half'
    return None


TESTS = [
    Test('usbaudio default', 'soundtest default out "(QEMU"', [r'default output: "Speakers \(QEMU USB Audio\)" chosen']),
    Test('usbaudio level', 'soundtest level out 0.5',
         [r'set "Speakers \(QEMU USB Audio\)" to 50%', r'"Speakers \(QEMU USB Audio\)" 50% \(default\)',
          r'"Speakers \(2- QEMU USB Audio\)" 100%(?! \()']),
    Test('usbaudio half', 'soundtest tone 880 1000', [r'played \d+ samples']),
    Test('usbaudio restart', 'shutdown /r', [r'\[AUDIO\] Saved default output: Speakers \(QEMU USB Audio\)'], reboot=True),
    Test('usbaudio kept', 'soundtest level out', [r'QEMU USB Audio\)" 50% \(default\)', r'QEMU USB Audio\)" 100%(?! \()'],
         before=both_attached),
    Test('usbaudio kept tone', 'soundtest tone 990 1000', [r'played \d+ samples'], check=half_as_loud),
]
