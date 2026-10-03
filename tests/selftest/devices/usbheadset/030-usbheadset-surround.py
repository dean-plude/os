# usbheadset, sampling rates, channels and the device picker: after 020, a
# high-speed USB Audio 2.0 surround headset whose clock offers 44.1 kHz
# only (tools/usbredirpeer.py --rates 44100: six speaker channels, four
# microphone channels) is plugged into the xHCI controller, then a
# full-speed USB Audio 1.0 speaker ("Test Speaker", spk.wav) after it.
# NovaOS must run the clock at 44.1 kHz and convert: the tone must sound at
# its pitch in surround.wav (the front two channels, written at the rate
# the host chose) with the other four channels silent, and the microphone's
# 1175 Hz (on its first two channels; the others hear 300 Hz) must be
# recorded at its pitch.  The picker: with the speaker newest (the
# default), waveOut and WASAPI play on the surround headset when a program
# names it (dev=), waveIn and WASAPI capture record from a named
# microphone, and "soundtest default" makes a device the default the way
# Settings' Sound page does.  spk.wav must hold only the tone played after
# it was made the default again.
import re
import struct
import time


def _wait_log(nova, pattern, secs=60):
    """Wait until the serial log (read from the file: run() owns the stream) matches @pattern"""
    for _ in range(secs * 4):
        if re.search(pattern, open(nova.serial_path, 'rb').read().decode('latin-1')):
            return
        time.sleep(0.25)
    print(f'usbheadset: no "{pattern}" in the serial log', flush=True)


def plug(n, port, bus, wait):
    def act(nova):
        r = nova.qmp.cmd('chardev-add', id=n, backend={'type': 'socket', 'data': {
            'addr': {'type': 'inet', 'data': {'host': '127.0.0.1', 'port': str(port)}}, 'server': False}})
        if 'error' in r:
            print('chardev-add:', r['error'], flush=True)
        r = nova.qmp.cmd('device_add', driver='usb-redir', id=n, chardev=n, bus=bus)
        if 'error' in r:
            print('device_add:', r['error'], flush=True)
        _wait_log(nova, wait)
    return act


def surround(*hz):
    """surround.wav: the tones @hz in order and nothing else, written at
    44.1 kHz, and nothing on the four channels beyond the front two"""
    inner = tones(*hz, wav='surround.wav', only=True)

    def check(nova):
        bad = inner(nova)
        if bad:
            return bad
        path = nova.work + '/surround.wav'
        rate = struct.unpack('<I', open(path, 'rb').read(28)[24:28])[0]
        if rate != 44100:
            return f'surround.wav was written at {rate} Hz, not 44100'
        extra = int(open(path + '.extra').read() or 0)
        if extra:
            return f'{extra} samples sounded on the surround channels (only the front two should)'
        return None
    return check


TESTS = [
    Test('surround tone', 'soundtest tone 523 1000', [r'played \d+ samples'],
         before=plug('surround', 10705, 'xhci.0', r'Test Surround Headset[\s\S]*\[AUDIO\] Recording from Microphone \(Test Surround'),
         boot_expect=[r'audio clock 16 at 44\.1 kHz',
                      r'audio output, 44\.1 kHz 24-bit 6-channel, 168-byte packets every 125 us \(USB Audio 2\.0\)',
                      r'audio input, 44\.1 kHz 24-bit 4-channel, 84-byte packets every 125 us \(USB Audio 2\.0\)',
                      r'\[AUDIO\] Playing on Speakers \(Test Surround Headset\)']),
    Test('surround record', r'soundtest record C:\srec.wav 2000', [r'recorded 88200 samples'],
         check=recording(r'C:\srec.wav', 1175, 1500)),
    Test('picker devices', 'soundtest info', [r'waveOut devices: 3', r'"Speakers \(Test Speaker\)"',
                                              r'waveIn devices: 4'],
         before=plug('spk', 10706, 'xhci.0', r'\[AUDIO\] Playing on Speakers \(Test Speaker\)')),
    Test('picker endpoints', 'soundtest endpoints',
         [r'render endpoints: 3', r'"Speakers \(Test Speaker\)" \{0\.0\.0\.00000000\}\.\{[0-9a-f-]+\} \(default\)',
          r'capture endpoints: 4', r'"Microphone \(Test Surround Headset\)" [^\n]* \(default\)']),
    Test('picker waveout', 'soundtest tone 392 1000 dev=Surround', [r'waveOut device 1: "Speakers \(Test Surround', r'played \d+ samples']),
    Test('picker wasapi', 'soundtest wasapi 330 1000 dev=Surround', [r'endpoint 1: "Speakers \(Test Surround', r'played \d+ frames']),
    Test('picker wavein', r'soundtest record C:\pick.wav 2000 dev=2-', [r'waveIn device \d: "Microphone \(2- Test Microphone',
                                                                         r'recorded 88200 samples'],
         check=recording(r'C:\pick.wav', 659, 1500)),
    Test('picker capture', r'soundtest capture C:\pickc.wav 1500 "dev=Test Headset"',
         [r'endpoint \d: "Microphone \(Test Headset\)', r'captured 144000 frames .* at -12\.0 dB'],
         check=recording(r'C:\pickc.wav', REC_HZ, 1000)),
    Test('default output', 'soundtest default out Surround', [r'default output: "Speakers \(Test Surround Headset\)" chosen']),
    Test('default tone', 'soundtest tone 294 1000', [r'played \d+ samples']),
    Test('default input', r'soundtest default in "Test Headset"', [r'default input: "Microphone \(Test Headset\)" chosen']),
    Test('default record', r'soundtest record C:\defin.wav 2000', [r'recorded 88200 samples'],
         check=recording(r'C:\defin.wav', REC_HZ, 1500)),
    Test('default back', 'soundtest default out "Test Speaker"', [r'default output: "Speakers \(Test Speaker\)" chosen']),
    Test('default speaker', 'soundtest tone 262 1000', [r'played \d+ samples'],
         check=lambda nova: surround(523, 392, 330, 294)(nova) or tones(262, wav='spk.wav', only=True)(nova)),
]
