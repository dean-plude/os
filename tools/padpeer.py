#!/usr/bin/env python3
"""A USB game controller for testing NovaOS, as a QEMU usb-redir device.

    tools/padpeer.py --port 10710 --kind xbox360|xboxone|hid [--ctl PORT]

QEMU has no gamepad of its own, so this program is one, behind QEMU's
usb-redir device (the usbredir protocol, as tools/usbredirpeer.py speaks
it for its headsets):

    -chardev socket,id=pad,host=127.0.0.1,port=10710
    -device usb-redir,chardev=pad,bus=xhci.0

It listens on --port for QEMU and is, at full speed, one of:

  xbox360  a wired Xbox 360 controller (045e:028e): vendor-specific
           interface FF/5D/01, 20-byte input reports on interrupt IN
           endpoint 0x81, the motors and the ring of lights through
           interrupt OUT endpoint 0x01;
  xboxone  an Xbox One controller (045e:02ea): interface FF/47/D0, which
           says nothing until the host sends "power on" (05 20 ..) to
           endpoint 0x01; then input reports of type 0x20 (and 0x07 for
           the Guide button) on 0x81;
  hid      a HID game pad (1209:0007, "NovaOS Test Gamepad"): sixteen
           buttons, a hat switch with a null state, and X, Y, Z and Rz of
           8 bits.

The test drives it through --ctl (default --port + 100), a TCP port that
takes lines of name=value pairs and answers "ok" once the report is sent:

  xbox360, xboxone: buttons (XInput's XINPUT_GAMEPAD_* bits, 0x0400 the
           Guide button), lt, rt (0-255), lx, ly, rx, ry (-32768..32767)
  hid:     buttons (bit n: button n + 1), hat (0-7, 8 centred), x, y, z,
           rz (0-255)

What the host sends the controller is printed: "rumble LEFT RIGHT" (the
motor bytes as sent), "led N", "power on", "set_report HEX".
"""
import argparse, socket, struct, sys, threading

HELLO, DEVICE_CONNECT, RESET, INTERFACE_INFO, EP_INFO = 0, 1, 3, 4, 5
SET_CONFIGURATION, GET_CONFIGURATION, CONFIGURATION_STATUS = 6, 7, 8
SET_ALT_SETTING, GET_ALT_SETTING, ALT_SETTING_STATUS = 9, 10, 11
START_INTERRUPT_RECEIVING, STOP_INTERRUPT_RECEIVING, INTERRUPT_RECEIVING_STATUS = 15, 16, 17
CONTROL_PACKET, INTERRUPT_PACKET = 100, 103
CAPS = (1 << 1) | (1 << 4) | (1 << 5) | (1 << 6)
CAP_64BIT_IDS = 1 << 5
SPEED_FULL = 1
OK, STALL = 0, 4
EP_IN, EP_OUT = 0x81, 0x01

# The HID game pad's report descriptor: 16 buttons, a hat (0-7, null
# state), 4 bits of padding, X Y Z Rz (0-255)
HID_REPORT = bytes([
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x10, 0x81, 0x02,
    0x05, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x35, 0x00, 0x46, 0x3B, 0x01, 0x65, 0x14,
    0x75, 0x04, 0x95, 0x01, 0x81, 0x42,
    0x65, 0x00, 0x45, 0x00, 0x75, 0x04, 0x95, 0x01, 0x81, 0x03,
    0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x04, 0x81, 0x02,
    0xC0,
])


def le16(v):
    return struct.pack('<H', v)


class Pad:
    """One controller: its descriptors and state"""

    def __init__(self, kind):
        self.kind = kind
        self.config = 0
        self.powered = kind != 'xboxone'
        self.seq = 0
        if kind == 'hid':
            self.vid, self.pid, self.product = 0x1209, 0x0007, 'NovaOS Test Gamepad'
            self.state = {'buttons': 0, 'hat': 8, 'x': 128, 'y': 128, 'z': 128, 'rz': 128}
            self.ifaces = [(0, 3, 0, 0)]
            self.mps, self.interval = 8, 10
        else:
            self.vid, self.pid = (0x045E, 0x028E) if kind == 'xbox360' else (0x045E, 0x02EA)
            self.product = 'Controller'
            self.state = {'buttons': 0, 'lt': 0, 'rt': 0, 'lx': 0, 'ly': 0, 'rx': 0, 'ry': 0}
            self.ifaces = [(0, 0xFF, 0x5D, 0x01)] if kind == 'xbox360' else [(0, 0xFF, 0x47, 0xD0)]
            self.mps, self.interval = (32, 4) if kind == 'xbox360' else (64, 4)

    def device(self):
        cls = bytes([0, 0, 0]) if self.kind == 'hid' else bytes([0xFF, 0xFF, 0xFF])
        return bytes([18, 1]) + le16(0x0200) + cls + bytes([64 if self.kind == 'hid' else 8]) + le16(self.vid) + \
            le16(self.pid) + le16(0x0114) + bytes([1, 2, 3, 1])

    def config_desc(self):
        f = self.ifaces[0]
        if self.kind == 'hid':
            d = bytes([9, 4, 0, 0, 1, 3, 0, 0, 0])
            d += bytes([9, 0x21]) + le16(0x0111) + bytes([0, 1, 0x22]) + le16(len(HID_REPORT))
            d += bytes([7, 5, EP_IN, 3]) + le16(self.mps) + bytes([self.interval])
        else:
            d = bytes([9, 4, 0, 0, 2, f[1], f[2], f[3], 0])
            if self.kind == 'xbox360':     # (the controller's own class descriptor, which hosts skip)
                d += bytes([17, 0x21, 0x00, 0x01, 0x01, 0x25, EP_IN, 0x14, 0, 0, 0, 0, 0x13, EP_OUT, 0x08, 0, 0])
            d += bytes([7, 5, EP_IN, 3]) + le16(self.mps) + bytes([self.interval])
            d += bytes([7, 5, EP_OUT, 3]) + le16(self.mps) + bytes([8 if self.kind == 'xbox360' else 4])
        return bytes([9, 2]) + le16(9 + len(d)) + bytes([1, 1, 0, 0x80, 250]) + d

    def string(self, i):
        if i == 0:
            return bytes([4, 3]) + le16(0x0409)
        s = {1: 'NovaOS' if self.kind == 'hid' else 'Microsoft', 2: self.product, 3: 'NOVA0001'}.get(i)
        if s is None:
            return None
        u = s.encode('utf-16-le')
        return bytes([2 + len(u), 3]) + u

    def endpoints(self):
        eps = [(0x00, 0, 0, 0, 64), (0x80, 0, 0, 0, 64)]
        if self.config:
            eps.append((EP_IN, 3, self.interval, 0, self.mps))
            if self.kind != 'hid':
                eps.append((EP_OUT, 3, 8 if self.kind == 'xbox360' else 4, 0, self.mps))
        return eps

    def report(self):
        """The input report for the current state (None: nothing to send yet)"""
        s = self.state
        if self.kind == 'hid':
            return struct.pack('<HBBBBB', s['buttons'] & 0xFFFF, s['hat'] & 0x0F, s['x'], s['y'], s['z'], s['rz'])
        if self.kind == 'xbox360':
            return struct.pack('<BBHBBhhhh6x', 0x00, 0x14, s['buttons'] & 0xFFFF, s['lt'], s['rt'],
                               s['lx'], s['ly'], s['rx'], s['ry'])
        if not self.powered:
            return None
        b = s['buttons']
        byte4 = sum(1 << (2 + i) for i, bit in enumerate((0x0010, 0x0020, 0x1000, 0x2000, 0x4000, 0x8000)) if b & bit)
        byte5 = sum(1 << i for i, bit in enumerate((0x0001, 0x0002, 0x0004, 0x0008, 0x0100, 0x0200, 0x0040, 0x0080)) if b & bit)
        self.seq = (self.seq + 1) & 0xFF
        return struct.pack('<BBBBBBHHhhhh', 0x20, 0x00, self.seq, 0x0E, byte4, byte5, s['lt'] * 4 + (3 if s['lt'] else 0),
                           s['rt'] * 4 + (3 if s['rt'] else 0), s['lx'], s['ly'], s['rx'], s['ry'])

    def guide_report(self):
        """Xbox One: the Guide button's own report"""
        self.seq = (self.seq + 1) & 0xFF
        return bytes([0x07, 0x20, self.seq, 0x02, 1 if self.state['buttons'] & 0x0400 else 0, 0x5B])


class Conn:
    """One QEMU usb-redir connection"""

    def __init__(self, sock, pad):
        self.sock, self.pad = sock, pad
        self.wlock = threading.Lock()
        self.ids64 = False
        self.listening = False

    def send(self, ptype, hdr=b'', data=b'', pid=0):
        h = struct.pack('<II', ptype, len(hdr) + len(data)) + struct.pack('<Q' if self.ids64 else '<I', pid)
        with self.wlock:
            self.sock.sendall(h + hdr + data)

    def recv_exact(self, n):
        buf = b''
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise EOFError
            buf += chunk
        return buf

    def send_info(self):
        p = self.pad
        n = len(p.ifaces)
        cols = [[f[k] for f in p.ifaces] + [0] * (32 - n) for k in range(4)]
        self.send(INTERFACE_INFO, struct.pack('<I', n) + b''.join(bytes(c) for c in cols))
        types, intervals, ifs, mps = [255] * 32, [0] * 32, [0] * 32, [0] * 32
        for addr, t, iv, iface, m in p.endpoints():
            i = ((addr & 0x80) >> 3) | (addr & 0x0F)
            types[i], intervals[i], ifs[i], mps[i] = t, iv, iface, m
        self.send(EP_INFO, bytes(types) + bytes(intervals) + bytes(ifs) + struct.pack('<32H', *mps))

    def send_report(self, guide=False):
        """The current state on the IN endpoint (if the host listens)"""
        if not self.listening:
            return
        r = self.pad.guide_report() if guide else self.pad.report()
        if r is not None:
            self.send(INTERRUPT_PACKET, struct.pack('<BBH', EP_IN, OK, len(r)), r)

    def control(self, pid, hdr, data):
        ep, req, rtype, _, value, index, length = struct.unpack('<BBBBHHH', hdr)
        p = self.pad
        reply, status = b'', OK
        if rtype in (0x80, 0x81) and req == 6:             # GET_DESCRIPTOR
            kind, i = value >> 8, value & 0xFF
            if kind == 1:
                reply = p.device()
            elif kind == 2:
                reply = p.config_desc()
            elif kind == 3:
                reply = p.string(i)
            elif kind == 0x22 and p.kind == 'hid':
                reply = HID_REPORT
            else:
                reply = None
            if reply is None:
                reply, status = b'', STALL
        elif rtype & 0x80 and req == 0:                    # GET_STATUS
            reply = b'\0\0'
        elif rtype == 0xA1 and req == 1 and p.kind == 'hid':   # GET_REPORT
            reply = p.report()
        elif rtype & 0x80:
            reply = bytes(length)
        elif rtype == 0x21 and req == 9:                   # SET_REPORT
            print('padpeer: set_report ' + data.hex(), flush=True)
        if rtype & 0x80:
            reply = reply[:length]
        self.send(CONTROL_PACKET, struct.pack('<BBBBHHH', ep, req, rtype, status, value, index,
                                              len(reply) if rtype & 0x80 else length), reply, pid)

    def interrupt_out(self, pid, ep, data):
        p = self.pad
        if p.kind == 'xbox360' and len(data) >= 5 and data[0] == 0x00 and data[1] == 0x08:
            print(f'padpeer: rumble {data[3]} {data[4]}', flush=True)
        elif p.kind == 'xbox360' and len(data) >= 3 and data[0] == 0x01 and data[1] == 0x03:
            print(f'padpeer: led {data[2]}', flush=True)
        elif p.kind == 'xboxone' and len(data) >= 5 and data[0] == 0x05 and data[1] == 0x20:
            print('padpeer: power on', flush=True)
            p.powered = True
        elif p.kind == 'xboxone' and len(data) >= 10 and data[0] == 0x09:
            print(f'padpeer: rumble {data[8]} {data[9]}', flush=True)
        else:
            print('padpeer: out ' + data.hex(), flush=True)
        self.send(INTERRUPT_PACKET, struct.pack('<BBH', ep, OK, len(data)), pid=pid)
        if p.kind == 'xboxone' and data[:2] == b'\x05\x20':
            self.send_report()

    def run(self):
        self.send(HELLO, b'novaos padpeer'.ljust(64, b'\0') + struct.pack('<I', CAPS))
        connected = False
        while True:
            hlen = 16 if self.ids64 else 12
            h = self.recv_exact(hlen)
            ptype, length = struct.unpack('<II', h[:8])
            pid = struct.unpack('<Q' if self.ids64 else '<I', h[8:])[0]
            body = self.recv_exact(length)
            if ptype == HELLO:
                caps = struct.unpack('<I', body[64:68])[0] if len(body) >= 68 else 0
                self.ids64 = bool(caps & CAP_64BIT_IDS)
                if not connected:
                    connected = True
                    self.send_info()
                    d = self.pad.device()
                    self.send(DEVICE_CONNECT, struct.pack('<BBBBHHH', SPEED_FULL, d[4], d[5], d[6],
                                                          self.pad.vid, self.pad.pid, 0x0114))
            elif ptype == CONTROL_PACKET:
                self.control(pid, body[:10], body[10:])
            elif ptype == SET_CONFIGURATION:
                self.pad.config = body[0]
                self.send_info()
                self.send(CONFIGURATION_STATUS, bytes([OK, self.pad.config]), pid=pid)
            elif ptype == GET_CONFIGURATION:
                self.send(CONFIGURATION_STATUS, bytes([OK, self.pad.config]), pid=pid)
            elif ptype == SET_ALT_SETTING:
                self.send(ALT_SETTING_STATUS, bytes([OK if body[1] == 0 else STALL, body[0], body[1]]), pid=pid)
            elif ptype == GET_ALT_SETTING:
                self.send(ALT_SETTING_STATUS, bytes([OK, body[0], 0]), pid=pid)
            elif ptype == START_INTERRUPT_RECEIVING:
                self.send(INTERRUPT_RECEIVING_STATUS, bytes([OK, body[0]]), pid=pid)
                if body[0] == EP_IN and not self.listening:
                    self.listening = True
                    self.send_report()
            elif ptype == STOP_INTERRUPT_RECEIVING:
                if body[0] == EP_IN:
                    self.listening = False
                self.send(INTERRUPT_RECEIVING_STATUS, bytes([OK, body[0]]), pid=pid)
            elif ptype == INTERRUPT_PACKET:
                ep, st, n = struct.unpack('<BBH', body[:4])
                self.interrupt_out(pid, ep, body[4:4 + n])


def ctl_loop(port, pad, conns):
    """The test's control port: name=value lines; each sends a report"""
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('127.0.0.1', port))
    srv.listen(4)
    while True:
        c, _ = srv.accept()
        f = c.makefile('rw')
        for line in f:
            try:
                guide = False
                for kv in line.split():
                    k, v = kv.split('=')
                    if k == 'buttons' and pad.kind == 'xboxone' and (int(v, 0) ^ pad.state['buttons']) & 0x0400:
                        guide = True
                    pad.state[k] = int(v, 0)
                for conn in list(conns):
                    conn.send_report()
                    if guide:
                        conn.send_report(guide=True)
                f.write('ok\n')
            except (ValueError, KeyError, OSError) as e:
                f.write(f'error {e}\n')
            f.flush()
        c.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, required=True)
    ap.add_argument('--kind', choices=('xbox360', 'xboxone', 'hid'), required=True)
    ap.add_argument('--ctl', type=int)
    a = ap.parse_args()
    pad = Pad(a.kind)
    conns = []
    threading.Thread(target=ctl_loop, args=(a.ctl or a.port + 100, pad, conns), daemon=True).start()
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('127.0.0.1', a.port))
    srv.listen(1)
    print(f'padpeer: {a.kind} listening on {a.port}', flush=True)
    while True:
        s, _ = srv.accept()
        print('padpeer: QEMU connected', flush=True)
        conn = Conn(s, pad)
        conns.append(conn)
        try:
            conn.run()
        except (EOFError, OSError):
            pass
        conns.remove(conn)
        pad.config, pad.powered = 0, pad.kind != 'xboxone'
        print('padpeer: QEMU went away', flush=True)
        s.close()


if __name__ == '__main__':
    sys.exit(main())
