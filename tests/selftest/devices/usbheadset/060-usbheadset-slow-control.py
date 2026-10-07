# usbheadset, a device that is slow to answer: after 050 a full-speed
# speaker (tools/usbredirpeer.py --slow-control 3000) is plugged into the
# xHCI controller whose first request, the read of its device descriptor,
# is answered three seconds late, as by a device (or a QEMU on a machine
# CI has loaded) that takes its time.  A control transfer may take up to
# five seconds (the USB specification's limit; EHCI has always waited that
# long), counted in time, not in polls of the event ring: NovaOS must wait
# for the answer, enumerate the speaker, and a tone played on it must
# sound at its pitch in slow.wav with nothing else there.  (A controller
# that timed out after about a second, as the xHCI driver's count of
# polls did on a fast machine, gave the device up with "control request
# 06 timed out" and it never attached: what became of the surround
# headset after 040's restart in one CI run, very likely for this reason.)
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
        _wait_log(nova, r'(?:Attached|Playing on) Speakers \(' + re.escape(product) + r'\)')
    return act


TESTS = [
    Test('slow descriptor', 'soundtest tone 988 1000 "dev=Slow Speaker"', [r'played \d+ samples'],
         before=plug('slow', 10709, 'xhci.0', 'Test Slow Speaker'), check=tones(988, wav='slow.wav', only=True)),
]
