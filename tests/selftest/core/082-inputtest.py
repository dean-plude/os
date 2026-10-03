# inputtest: mouse side buttons and the horizontal wheel (a USB mouse
# plugged in for the test, then the PS/2 mouse) and the volume keys (the USB
# keyboard), as a program's window gets them; usbcheck runs the HID report
# parser on media-key and five-button-mouse report descriptors QEMU has no
# device for
import re, time

DOC = '`inputtest` (side buttons, horizontal wheel, volume keys), `usbcheck` (media keys, AC Pan)'


def _log(nova):
    return open(nova.serial_path, 'rb').read().decode('latin-1')


def _buttons(nova, *names):
    for n in names:
        for down in (True, False):
            nova.qmp.cmd('input-send-event', events=[{'type': 'btn', 'data': {'down': down, 'button': n}}])
            time.sleep(0.15)


def usb_mouse(nova):
    """Plug a USB mouse into the xHCI controller and press its buttons 4 and 5"""
    before = len(re.findall(r'\[USB\] [^\n]*: mouse', _log(nova)))
    nova.hmp('device_add usb-mouse,bus=xhci.0,id=xmouse')
    for _ in range(80):
        if len(re.findall(r'\[USB\] [^\n]*: mouse', _log(nova))) > before:
            break
        time.sleep(0.25)
    time.sleep(1)
    _buttons(nova, 'side', 'extra')


def ps2_mouse(nova):
    """Unplug it: the PS/2 mouse turns its horizontal wheel and presses button 4"""
    nova.hmp('device_del xmouse')
    for _ in range(80):
        if re.search(r'\[USB\] [^\n]*: pointer removed', _log(nova)):
            break
        time.sleep(0.25)
    time.sleep(1)
    _buttons(nova, 'wheel-right', 'wheel-left', 'side')


def volume_keys(nova):
    for k in ('volumedown', 'volumeup', 'audiomute', 'audiomute'):   # (the volume ends where it was)
        nova.qmp.key(k)
        time.sleep(0.3)


TESTS = [
    Test('usbcheck', 'usbcheck', [r'usbcheck: all passed, 0 failed'], builtin=True),
    Test('inputtest', 'inputtest', [r'inputtest: \d+ passed, 0 failed', r'\[SHELL\] Volume \d+%',
                                    r'\[SHELL\] Volume muted', r'\[SHELL\] Volume unmuted'],
         boot_expect=[r'\[PS2\] [^\n]*5-button wheel mouse'],
         acts=[(r'inputtest: plug in a USB mouse', usb_mouse), (r'inputtest: unplug it', ps2_mouse),
               (r'inputtest: press volume', volume_keys)]),
]
