# usbheadset, asynchronous endpoints: two speakers that run on their own
# clocks (tools/usbredirpeer.py --feedback) are plugged in after 045: a
# full-speed USB Audio 1.0 one on the xHCI controller whose synch endpoint
# (named by bSynchAddress) says, in 10.14 fixed point, that it plays
# 48,500 frames a second, and a high-speed USB Audio 2.0 one on the EHCI
# controller whose explicit feedback endpoint says, in 16.16, 47,600.  At
# a nominal 48 kHz NovaOS would send 48 frames a millisecond and 6 a
# microframe; it must follow the feedback instead: 48.5 a packet on the
# first and 5.95 on the second, on average (counted by the peer once the
# feedback has run for half a second), and each tone must sound at its
# pitch in its speaker's WAV with nothing else there.
import os
import re
import time


def _wait_log(nova, pattern, secs=60):
    """Wait until the serial log (read from the file: run() owns the stream) matches @pattern"""
    for _ in range(secs * 4):
        if re.search(pattern, open(nova.serial_path, 'rb').read().decode('latin-1')):
            return
        time.sleep(0.25)
    print(f'usbheadset: no "{pattern}" in the serial log', flush=True)


def plug(n, port, bus, product):
    def act(nova):
        r = nova.qmp.cmd('chardev-add', id=n, backend={'type': 'socket', 'data': {
            'addr': {'type': 'inet', 'data': {'host': '127.0.0.1', 'port': str(port)}}, 'server': False}})
        if 'error' in r:
            print('chardev-add:', r['error'], flush=True)
        r = nova.qmp.cmd('device_add', driver='usb-redir', id=n, chardev=n, bus=bus)
        if 'error' in r:
            print('device_add:', r['error'], flush=True)
        _wait_log(nova, r'Attached Speakers \(' + re.escape(product) + r'\)')
    return act


def rate(wav, hz, per_s, tone):
    """@wav.fb: the frames a packet averaged @hz / @per_s (within 0.2%); and
    @wav holds @tone alone"""
    tone_check = tones(tone, wav=wav, only=True)

    def check(nova):
        bad = tone_check(nova)
        if bad:
            return bad
        path = os.path.join(nova.work, wav + '.fb')
        if not os.path.exists(path):
            return f'the peer counted no packets for {wav}'
        packets, frames = (int(x) for x in open(path).read().split())
        if packets < 500:
            return f'only {packets} packets came after the feedback started'
        got, want = frames / packets, hz / per_s
        if abs(got - want) > want * 0.002:
            return f'{got:.3f} frames a packet, not the {want:.3f} the feedback asked for (nominal {48000 / per_s:g})'
        return None
    return check


TESTS = [
    Test('feedback full speed', 'soundtest tone 784 3000 "dev=Async Speaker"', [r'played \d+ samples'],
         before=plug('async1', 10707, 'xhci.0', 'Test Async Speaker'), check=rate('async1.wav', 48500, 1000, 784),
         boot_expect=[r'audio output is asynchronous: feedback every 1000 us from endpoint 81',
                      r'audio feedback: the device plays 48500\.000 frames a second \(nominal 48000\)']),
    Test('feedback high speed', 'soundtest tone 932 3000 "dev=Async Headset"', [r'played \d+ samples'],
         before=plug('async2', 10708, 'ehci.0', 'Test Async Headset'), check=rate('async2.wav', 47600, 8000, 932),
         boot_expect=[r'audio output, 48 kHz 24-bit stereo, 48-byte packets every 125 us \(USB Audio 2\.0\)\n'
                      r'[^\n]*audio output is asynchronous: feedback every 1000 us from endpoint 81',
                      r'audio feedback: the device plays 47(?:599|600)\.\d{3} frames a second \(nominal 48000\)']),
]
