# usbheadset, USB Audio Class 2.0: a high-speed UAC2 headset
# (tools/usbredirpeer.py --uac2: a clock source behind a clock selector,
# 24-bit samples in 4-byte slots out and 3-byte slots in, a packet every
# microframe both ways) plugged into the xHCI controller while NovaOS runs,
# after the tests in 010.  It becomes the newest output and input: its
# speaker writes uac2.wav, which must hold the tone and nothing else, and
# its microphone hears 988 Hz through waveIn and WASAPI (and the EHCI
# headset stays quiet meanwhile: 010 checks headset.wav holds only its
# tone).  Unplugging it hands recording back to the OHCI microphone.
import re
import time


def _wait_log(nova, pattern, secs=60):
    """Wait until the serial log (read from the file: run() owns the stream) matches @pattern"""
    for _ in range(secs * 4):
        if re.search(pattern, open(nova.serial_path, 'rb').read().decode('latin-1')):
            return
        time.sleep(0.25)
    print(f'usbheadset: no "{pattern}" in the serial log', flush=True)


def plug_uac2(nova):
    r = nova.qmp.cmd('chardev-add', id='uac2', backend={'type': 'socket', 'data': {
        'addr': {'type': 'inet', 'data': {'host': '127.0.0.1', 'port': '10704'}}, 'server': False}})
    if 'error' in r:
        print('chardev-add:', r['error'], flush=True)
    r = nova.qmp.cmd('device_add', driver='usb-redir', id='uac2', chardev='uac2', bus='xhci.0')
    if 'error' in r:
        print('device_add:', r['error'], flush=True)
    _wait_log(nova, r'high speed, xHCI[^\n]*\n(?:[^\n]*\n)*?\[AUDIO\] Recording from Microphone \(')


def unplug_uac2(nova):
    nova.qmp.cmd('device_del', id='uac2')
    _wait_log(nova, r'Audio 2\.0\)[\s\S]*audio device unplugged')


TESTS = [
    Test('uac2 tone', 'soundtest tone 660 1000', [r'played \d+ samples'],
         before=plug_uac2, check=tones(660, wav='uac2.wav', only=True),
         boot_expect=[r'audio clock 16 at 48 kHz',
                      r'audio output, 48 kHz 24-bit stereo, 48-byte packets every 125 us \(USB Audio 2\.0\)',
                      r'audio input, 48 kHz 24-bit mono, 18-byte packets every 125 us \(USB Audio 2\.0\)']),
    Test('uac2 record', r'soundtest record C:\u2rec.wav 2000', [r'recorded 88200 samples'],
         check=recording(r'C:\u2rec.wav', 988, 1500)),
    Test('uac2 capture', r'soundtest capture C:\u2cap.wav 1500', [r'captured 144000 frames .* at -12\.0 dB'],
         check=recording(r'C:\u2cap.wav', 988, 1000)),
    Test('uac2 unplug', r'soundtest record C:\u2back.wav 2000', [r'recorded 88200 samples'],
         before=unplug_uac2, check=recording(r'C:\u2back.wav', 659, 1500), settle=2),
]
