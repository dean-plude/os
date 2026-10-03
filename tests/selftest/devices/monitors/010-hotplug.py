# monitors: one card with three outputs (a QEMU virtio-vga, max_outputs=3)
# and a monitor only on the first at boot.  The test plugs monitors into
# the second and third outputs while NovaOS runs, then unplugs them; QEMU
# connects or disconnects an output when a VNC client on it asks for a
# desktop size (RFB SetDesktopSize; 0 x 0 disconnects).  montest checks
# the monitors, WM_DISPLAYCHANGE and that a window on a monitor that goes
# moves to one that is left, and the test pushes the pointer across onto
# the third monitor before it goes; the screenshot shows the three outputs.
import os
import socket
import struct
import time


def _vnc_size(nova, output, w, h):
    """Ask QEMU, through the VNC server on @output (work/vncN.sock), for a w x h monitor there"""
    s = socket.socket(socket.AF_UNIX)
    s.settimeout(10)
    s.connect(os.path.join(nova.work, f'vnc{output}.sock'))

    def rd(n):
        b = b''
        while len(b) < n:
            c = s.recv(n - len(b))
            if not c:
                raise EOFError('the VNC server hung up')
            b += c
        return b
    try:
        rd(12)
        s.sendall(b'RFB 003.008\n')
        rd(rd(1)[0])
        s.sendall(b'\x01')                                  # security: none
        if struct.unpack('>I', rd(4))[0]:
            raise EOFError('the VNC server refused')
        s.sendall(b'\x01')                                  # ClientInit: shared
        rd(20)
        rd(struct.unpack('>I', rd(4))[0])                   # ServerInit's name
        s.sendall(struct.pack('>BBHi', 2, 0, 1, -308))      # SetEncodings: ExtendedDesktopSize
        s.sendall(struct.pack('>BBHHBB', 251, 0, w, h, 1, 0) + struct.pack('>IHHHHI', 0, 0, 0, w, h, 0))
        time.sleep(1)
    except (OSError, EOFError) as e:
        print(f'monitors: VNC on output {output + 1}: {e}', flush=True)
    finally:
        s.close()


def plug(output, w, h):
    return lambda nova: _vnc_size(nova, output, w, h)


def push_right(nova):
    for _ in range(150):
        nova.hmp('mouse_move 40 0')
        time.sleep(0.02)


TESTS = [
    Test('montest hotplug', 'montest hotplug', [r'montest: \d+ passed, 0 failed',
                                                r'\[DISPLAY\] Head 1: QEMU virtio-vga output 2, 1024x768',
                                                r'\[DISPLAY\] Head 2: QEMU virtio-vga output 3, 800x600',
                                                r'\[DISPLAY\] Head 2: QEMU virtio-vga output 3 unplugged',
                                                r'\[DISPLAY\] Head 1: QEMU virtio-vga output 2 unplugged'],
         shot=r'window on display 3',
         boot_expect=[r'\[DISPLAY\] QEMU virtio-vga has 3 outputs'],
         acts=[(r'plug in display 2', plug(1, 1024, 768)), (r'plug in display 3', plug(2, 800, 600)),
               (r'move the pointer right', push_right), (r'unplug display 3', plug(2, 0, 0)), (r'unplug display 2', plug(1, 0, 0))]),
]
