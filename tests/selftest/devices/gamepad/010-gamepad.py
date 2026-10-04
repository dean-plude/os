# gamepad: three USB game controllers on the xHCI controller, each
# tools/padpeer.py behind a QEMU usb-redir device (QEMU has no gamepad of
# its own): a wired Xbox 360 controller (port 10710), an Xbox One
# controller (10711) and a HID game pad (10712).  padtest checks what
# XInput and DirectInput 8 list, then at each "[PAD] step N" line the test
# sets a controller's buttons and sticks through the peer's control port
# (the peer port + 100) and padtest checks both APIs see them; the motors
# it sets must reach the Xbox controllers (the peers' logs), and step 6
# unplugs the Xbox 360 controller.  The 32-bit padtest then checks the two
# controllers left.
import os
import re
import socket


def _pad(port, line):
    """Set a controller's state through its peer's control port"""
    def act(nova):
        try:
            with socket.create_connection(('127.0.0.1', port + 100), timeout=10) as s:
                s.sendall((line + '\n').encode())
                reply = s.makefile().readline().strip()
            if reply != 'ok':
                print(f'padpeer {port}: {reply}', flush=True)
        except OSError as e:
            print(f'padpeer {port}: {e}', flush=True)
    return act


def _unplug(nova):
    r = nova.qmp.cmd('device_del', id='pad360')
    if 'error' in r:
        print('device_del:', r['error'], flush=True)


def _motors(nova):
    """What the controllers were sent: the Xbox 360 one its player light
    and the motors (0x8000, 0x4000 as bytes), the Xbox One one "power on"
    and its motors (0xFFFF as 99 percent)"""
    want = {10710: [r'padpeer: led [67]\b', r'padpeer: rumble 128 64\b'],
            10711: [r'padpeer: power on', r'padpeer: rumble 99 0\b']}
    for port, pats in want.items():
        path = os.path.join(nova.work, f'padpeer-{port}.log')
        log = open(path).read() if os.path.exists(path) else ''
        for p in pats:
            if not re.search(p, log):
                return f'no "{p}" in padpeer-{port}.log'
    return None


TESTS = [
    Test('gamepad', 'padtest', [r'padtest: \d+ passed, 0 failed'], timeout=300, check=_motors,
         boot_expect=[r'Xbox 360 controller "Controller" \(XInput user [12]\)',
                      r'Xbox One controller "Controller" \(XInput user [12]\)',
                      r'gamepad "NovaOS Test Gamepad" \(16 buttons, 4 axes, a hat\)'],
         acts=[(r'\[PAD\] step 1:', _pad(10710, 'buttons=0x1109 lt=200 lx=20000 ly=-10000')),
               (r'\[PAD\] step 2:', _pad(10711, 'buttons=0xA610 rt=255 rx=-30000 ry=30000')),
               (r'\[PAD\] step 3:', _pad(10712, 'buttons=0x0805 hat=2 x=255 y=0 rz=64')),
               (r'\[PAD\] step 4:', _pad(10712, 'buttons=0x0806')),
               (r'\[PAD\] step 6:', _unplug)]),
    Test('gamepad x86', r'C:\Programs\x86\padtest.exe still', [r'padtest still: \d+ passed, 0 failed']),
]
