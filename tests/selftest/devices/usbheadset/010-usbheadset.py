# usbheadset: no HD Audio card.  A high-speed USB headset (speaker and
# microphone) on an EHCI controller at boot, and full-speed USB
# microphones plugged into the xHCI, OHCI and UHCI controllers while
# NovaOS runs: each is tools/usbredirpeer.py behind a QEMU usb-redir
# device (QEMU has no isochronous IN device and no high-speed audio
# device of its own).  The headset's speaker writes headset.wav, which
# must hold the tone and nothing else (isochronous OUT through EHCI iTDs,
# eight packets each); each microphone hears its own tone, and the newest
# microphone records (isochronous IN on all four controllers); unplugging
# the UHCI one hands recording back to the OHCI one.
import re
import time


def _wait_log(nova, pattern, secs=60):
    """Wait until the serial log (read from the file: run() owns the stream) matches @pattern"""
    for _ in range(secs * 4):
        if re.search(pattern, open(nova.serial_path, 'rb').read().decode('latin-1')):
            return
        time.sleep(0.25)
    print(f'usbheadset: no "{pattern}" in the serial log', flush=True)


def plug_mic(n, bus, kind):
    """Plug microphone @n (tools/usbredirpeer.py on port 10700 + n) into @bus"""
    def act(nova):
        r = nova.qmp.cmd('chardev-add', id=f'mic{n}', backend={'type': 'socket', 'data': {
            'addr': {'type': 'inet', 'data': {'host': '127.0.0.1', 'port': str(10700 + n)}}, 'server': False}})
        if 'error' in r:
            print('chardev-add:', r['error'], flush=True)
        r = nova.qmp.cmd('device_add', driver='usb-redir', id=f'mic{n}', chardev=f'mic{n}', bus=bus)
        if 'error' in r:
            print('device_add:', r['error'], flush=True)
        _wait_log(nova, r'full speed, ' + kind + r'[^\n]*\n(?:[^\n]*\n)*?\[AUDIO\] Recording from USB Microphone')
    return act


def unplug_mic(n):
    def act(nova):
        nova.qmp.cmd('device_del', id=f'mic{n}')
        _wait_log(nova, r'audio device unplugged')
    return act


TESTS = [
    Test('usbheadset tone', 'soundtest tone 440 1000', [r'played \d+ samples'],
         check=tones(440, wav='headset.wav', only=True),
         boot_expect=[r'high speed, EHCI', r'audio output, 48 kHz 16-bit stereo, 24-byte packets every 125 us',
                      r'audio input, 48 kHz 16-bit mono, 96-byte packets every 1000 us',
                      r'\[AUDIO\] Recording from USB Microphone']),
    Test('usbheadset record', r'soundtest record C:\hs.wav 3000', [r'recorded 132300 samples'],
         check=recording(r'C:\hs.wav', REC_HZ, 2500)),
    Test('usbheadset capture', r'soundtest capture C:\hscap.wav 1500', [r'captured 144000 frames .* at -12\.0 dB'],
         check=recording(r'C:\hscap.wav', REC_HZ, 2500)),
    Test('usbmic xhci', r'soundtest record C:\micx.wav 2000', [r'recorded 88200 samples'],
         before=plug_mic(1, 'xhci.0', 'xHCI'), check=recording(r'C:\micx.wav', 784, 1500)),
    Test('usbmic ohci', r'soundtest record C:\mico.wav 2000', [r'recorded 88200 samples'],
         before=plug_mic(2, 'ohci.0', 'OHCI'), check=recording(r'C:\mico.wav', 659, 1500)),
    Test('usbmic uhci', r'soundtest record C:\micu.wav 2000', [r'recorded 88200 samples'],
         before=plug_mic(3, 'uhci.0', 'UHCI'), check=recording(r'C:\micu.wav', 880, 1500)),
    Test('usbmic unplug', r'soundtest record C:\mic2.wav 2000', [r'recorded 88200 samples'],
         before=unplug_mic(3), check=recording(r'C:\mic2.wav', 659, 1500), settle=2),
]
