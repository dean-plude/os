# touchtest: a virtio multi-touch screen (QEMU's input-send-event "mtt"
# events) touched where touchtest asks; usbcheck runs the USB HID parser on
# a multi-touch report descriptor (QEMU has no USB touch screen)
import time

MAX = 0x7FFF                   # QEMU's absolute axis range


def _mtt(nova, *events):
    """Send a frame of multi-touch events: (begin|update|end, slot, tracking id, x, y), x and y as
    fractions of the screen.  Each picks its slot (ABS_MT_SLOT, ABS_MT_TRACKING_ID) and then moves it"""
    ev = []
    for kind, slot, tid, x, y in events:
        ev.append({'type': 'mtt', 'data': {'type': kind, 'slot': slot, 'tracking-id': tid, 'axis': 'x', 'value': 0}})
        if kind != 'end':
            for axis, v in (('x', x), ('y', y)):
                ev.append({'type': 'mtt', 'data': {'type': 'data', 'slot': slot, 'tracking-id': tid, 'axis': axis,
                                                    'value': int(v * MAX)}})
    r = nova.qmp.cmd('input-send-event', events=ev)
    if 'error' in r:
        print('input-send-event:', r['error'], flush=True)
    time.sleep(0.3)


def two_fingers(nova):
    """Two contacts down on the left half, moved, lifted"""
    _mtt(nova, ('begin', 0, 10, 0.20, 0.40), ('begin', 1, 11, 0.30, 0.60))
    _mtt(nova, ('update', 0, 10, 0.22, 0.42), ('update', 1, 11, 0.32, 0.58))
    _mtt(nova, ('end', 0, -1, 0, 0), ('end', 1, -1, 0, 0))


def tap(nova):
    """One contact down and up on the right half"""
    _mtt(nova, ('begin', 0, 12, 0.75, 0.50))
    _mtt(nova, ('end', 0, -1, 0, 0))


TESTS = [
    Test('usbcheck', 'usbcheck', [r'ok   multi-touch \(hybrid reports\)', r'usbcheck: all passed, 0 failed'], builtin=True,
         boot_expect=[r'\[VIRTIO\] Multi-touch screen [^\n]*: 10 contacts']),
    Test('touchtest', 'touchtest', [r'touchtest: \d+ passed, 0 failed'],
         acts=[(r'touchtest: two fingers on the left', two_fingers), (r'touchtest: tap on the right', tap)]),
]
