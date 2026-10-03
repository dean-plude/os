# usbheadset, each device's own volume, DirectSound and XAudio2 devices,
# and the default kept across a restart: after 030 the outputs are the EHCI
# headset, the surround headset and the full-speed speaker (the default),
# and "Microphone (Test Headset)" is the chosen input.  The surround
# headset's endpoint volume is set to a quarter (IAudioEndpointVolume on
# its endpoint), which must leave the other outputs at full, and a waveOut
# tone on it must sound a quarter as loud as 030's at full; a program's
# waveOutSetVolume on one device ID must leave the others alone.
# DirectSoundEnumerate and DirectSoundCaptureEnumerate must list every
# device with its name and GUID, and DirectSound must play on and
# DirectSoundCapture record from the surround headset named by that GUID;
# XAudio2 2.7 must list every output and 2.9 play on the surround headset
# given its device ID.  The surround headset is then made the default and
# NovaOS restarted: although the speaker attaches again too, the surround
# headset must be the default output, "Microphone (Test Headset)" the
# default input and the surround headset still at a quarter, so a tone
# played on the default after the restart sounds in surround.wav, quietly.
# surround.wav must hold exactly these tones, at 44.1 kHz, with the four
# channels beyond the front two silent.
import os
import re
import struct
import time
import wavcheck

SURROUND = (523, 392, 330, 294, 698, 349, 440, 880, 587, 466)   # in the order played (030, 035, then here)


def _wait_log(nova, pattern, secs=90):
    """Wait until the serial log after the restart matches @pattern"""
    for _ in range(secs * 4):
        log = open(nova.serial_path, 'rb').read().decode('latin-1')
        if re.search(pattern, log[log.rfind('Saved default output'):]):
            return
        time.sleep(0.25)
    print(f'usbheadset: no "{pattern}" in the serial log', flush=True)


def all_back(nova):
    for name in ('Test Headset', 'Test Surround Headset', 'Test Speaker'):
        _wait_log(nova, r'(?:Playing on|Attached) Speakers \(' + re.escape(name) + r'\)')


def surround(nova):
    """surround.wav: SURROUND and nothing else, at 44.1 kHz on the front two
    channels; 349 and 466 Hz (the device at a quarter) a quarter as loud as
    392 Hz (030, at full volume)"""
    bad = tones(*SURROUND, wav='surround.wav', only=True)(nova)
    if bad:
        return bad
    path = os.path.join(nova.work, 'surround.wav')
    rate = struct.unpack('<I', open(path, 'rb').read(28)[24:28])[0]
    if rate != 44100:
        return f'surround.wav was written at {rate} Hz, not 44100'
    extra = int(open(path + '.extra').read() or 0)
    if extra:
        return f'{extra} samples sounded on the surround channels (only the front two should)'
    segs = wavcheck.segments(path)
    level = {hz: max((s[2] for s in segs if abs(s[3] - hz) < hz * 0.05), default=0) for hz in (392, 349, 466)}
    for hz in (349, 466):
        ratio = level[hz] / level[392] if level[392] else 0
        if not 0.18 < ratio < 0.32:
            return f'the {hz} Hz tone played at {ratio:.2f} of full volume, not a quarter'
    return None


TESTS = [
    Test('device volume', 'soundtest level out 0.25 dev=Surround',
         [r'set "Speakers \(Test Surround Headset\)" to 25%', r'"Speakers \(Test Surround Headset\)" 25%(?! \()',
          r'"Speakers \(Test Speaker\)" 100% \(default\)', r'"Speakers \(Test Headset\)" 100%(?! \()']),
    Test('device volume in', 'soundtest level in 0.5 "dev=Microphone (Test Surround"',
         [r'"Microphone \(Test Surround Headset\)" 50%(?! \()', r'"Microphone \(Test Headset\)" 100% \(default\)']),
    Test('device volume tone', 'soundtest tone 349 1000 dev=Surround', [r'played \d+ samples']),
    Test('waveout device volume', 'soundtest wovolume dev=Surround', [r'waveOut volume set on device \d only']),
    Test('dsound devices', 'soundtest dsenum',
         [r'\(null\) "Primary Sound Driver"', r'\{6e6f7661-6864-6100-0000-[0-9a-f]{12}\} "Speakers \(Test Surround Headset\)"',
          r'"Speakers \(Test Speaker\)" \{0\.0\.0\.00000000\}\.\{6e6f7661', r'\(null\) "Primary Sound Capture Driver"',
          r'"Microphone \(Test Surround Headset\)" \{0\.0\.1\.00000000\}']),
    Test('dsound named', 'soundtest dsound 440 1000 dev=Surround',
         [r'dsound device: "Speakers \(Test Surround Headset\)"', r'streamed \d+ samples', r'static buffer']),
    Test('dscapture named', r'soundtest dscapture C:\dscs.wav 2000 "dev=Microphone (Test Surround"',
         [r'dsound device: "Microphone \(Test Surround Headset\)"', r'captured 88200 samples'],
         check=recording(r'C:\dscs.wav', 1175, 1500)),
    Test('xaudio2 devices', 'xa2test devices Surround 587 1000',
         [r'2\.7: 4 device\(s\)', r'0: "Speakers \(Test Speaker\)" [^\n]* role 15',
          r'"Speakers \(Test Surround Headset\)" \{0\.0\.0\.00000000\}\.\{6e6f7661[^\n]* role 0',
          r'2\.9 on "Speakers \(Test Surround Headset\)"', r'XAudio2 devices passed']),
    Test('default before restart', 'soundtest default out Surround',
         [r'default output: "Speakers \(Test Surround Headset\)" chosen']),
    Test('restart', 'shutdown /r', [r'\[AUDIO\] Saved default output: Speakers \(Test Surround Headset\)',
                                    r'\[AUDIO\] Saved default input: Microphone \(Test Headset\)'], reboot=True),
    Test('default after restart', 'soundtest endpoints',
         [r'"Speakers \(Test Surround Headset\)" [^\n]* \(default\)', r'(?m)"Speakers \(Test Speaker\)" \S+\s*$',
          r'"Microphone \(Test Headset\)" [^\n]* \(default\)'], before=all_back),
    Test('volume after restart', 'soundtest level out',
         [r'"Speakers \(Test Surround Headset\)" 25% \(default\)', r'"Speakers \(Test Speaker\)" 100%(?! \()']),
    Test('tone after restart', 'soundtest tone 466 1000', [r'played \d+ samples'], check=surround),
]
