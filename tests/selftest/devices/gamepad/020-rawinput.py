# gamepad: Raw Input and hid.dll, after 010 (which unplugged the Xbox 360
# controller): rawpadtest checks what Raw Input lists and the preparsed
# data hid.dll reads, registers for game pads and at each "[PAD] step N"
# line the test sets a controller through its peer's control port (the
# peer port + 100); WM_INPUT, GetRawInputBuffer and a HID device handle's
# reads must carry the reports.  Step 11 unplugs the Xbox One controller.
# The 32-bit rawpadtest then checks the HID game pad left.
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
    r = nova.qmp.cmd('device_del', id='padone')
    if 'error' in r:
        print('device_del:', r['error'], flush=True)


TESTS = [
    Test('raw input', 'rawpadtest', [r'rawpadtest: \d+ passed, 0 failed'], timeout=300,
         acts=[(r'\[PAD\] step 7:', _pad(10712, 'buttons=0x8002 hat=4 x=10 y=128 z=128 rz=200')),
               (r'\[PAD\] step 8:', _pad(10711, 'buttons=0x5000 lt=255 rt=0 lx=32767 ly=-32768 rx=0 ry=0')),
               (r'\[PAD\] step 9:', _pad(10712, 'buttons=0x0010 hat=8')),
               (r'\[PAD\] step 10:', _pad(10712, 'buttons=0x0040')),
               (r'\[PAD\] step 11:', _unplug)]),
    Test('raw input x86', r'C:\Programs\x86\rawpadtest.exe still', [r'rawpadtest still: \d+ passed, 0 failed']),
]
