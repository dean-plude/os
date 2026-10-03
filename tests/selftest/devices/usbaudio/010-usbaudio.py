# usbaudio: no HD Audio card, only USB speakers (QEMU usb-audio, each
# recorded to its own WAV): speaker 1 on the xHCI controller at boot, then
# speaker 2 plugged into the OHCI controller and speaker 3 into the UHCI
# one while NovaOS runs.  The newest speaker plays; unplugging speaker 3
# hands playback back to speaker 2.  Each tone must be in its speaker's WAV
# (isochronous OUT on all three controllers; QEMU has no isochronous IN
# device), and nothing else: a speaker another one took over from goes
# quiet once what was mixed for it has played.
import re
import time


def _wait_log(nova, pattern, secs=60):
    """Wait until the serial log (read from the file: run() owns the stream) matches @pattern"""
    for _ in range(secs * 4):
        if re.search(pattern, open(nova.serial_path, 'rb').read().decode('latin-1')):
            return
        time.sleep(0.25)
    print(f'usbaudio: no "{pattern}" in the serial log', flush=True)


def plug(n, bus, kind):
    def act(nova):
        r = nova.qmp.cmd('device_add', driver='usb-audio', id=f'spk{n}', bus=bus, audiodev=f'usbsnd{n}')
        if 'error' in r:
            print('device_add:', r['error'], flush=True)
        _wait_log(nova, r'full speed, ' + kind + r'[^\n]*\n(?:[^\n]*\n)*?\[AUDIO\] Playing on USB Audio Device')
    return act


def unplug(n):
    def act(nova):
        nova.qmp.cmd('device_del', id=f'spk{n}')
        _wait_log(nova, r'audio device unplugged')
    return act


TESTS = [
    Test('usbaudio xhci', 'soundtest tone 440 1000', [r'played \d+ samples'], check=tones(440, wav='usb1.wav', only=True),
         boot_expect=[r'\[USB\] [^\n]*: audio output, 48 kHz 16-bit stereo', r'\[AUDIO\] Playing on USB Audio Device']),
    Test('usbaudio ohci', 'soundtest tone 550 1000', [r'played \d+ samples'], before=plug(2, 'ohci.0', 'OHCI'),
         check=tones(550, wav='usb2.wav')),
    Test('usbaudio uhci', 'soundtest tone 660 1000', [r'played \d+ samples'], before=plug(3, 'uhci.0', 'UHCI'),
         check=tones(660, wav='usb3.wav', only=True)),
    Test('usbaudio unplug', 'soundtest tone 770 1000', [r'played \d+ samples'], before=unplug(3),
         check=tones(550, 770, wav='usb2.wav', only=True)),
]
