#!/usr/bin/env python3
"""A USB Audio Class 1 or 2 headset or microphone for testing NovaOS, as a
QEMU usb-redir device.

    tools/usbredirpeer.py --port 10700 [--speed high|full] [--uac2] [--speaker OUT.wav] [--mic HZ]

QEMU's usb-redir device forwards everything the guest sends a USB device
over a chardev, in the usbredir protocol, to a program that is the device:

    -chardev socket,id=ur,host=127.0.0.1,port=10700
    -device usb-ehci,id=ehci -device usb-redir,chardev=ur,bus=ehci.0

This program listens on --port and is that device for each connection: a
high-speed (or --speed full) USB Audio Class 1.0 function with a speaker
(--speaker: a 48 kHz 16-bit stereo isochronous OUT endpoint, whose packets
are written to the WAV file) and a microphone (--mic: a 48 kHz 16-bit mono
isochronous IN endpoint that hears a sine of HZ), either or both.  At high
speed the speaker's endpoint is polled every microframe (6 frames a
packet) and the microphone's every millisecond, so an EHCI controller's
iTDs carry eight packets and one; at full speed both are polled every
frame.  QEMU itself has no isochronous IN device, no high-speed audio
device and nothing for EHCI's iTDs to talk to, hence this.

With --uac2 the function is a USB Audio Class 2.0 one instead: an
interface association, a programmable clock source behind a clock
selector (the host must set 48 kHz on it), 24-bit samples in 4-byte
slots for the speaker (the WAV gets their top 16 bits) and in 3-byte
slots for the microphone, and at high speed both endpoints polled every
microframe (6 frames a packet).

The protocol is usbredir's (usbredirproto.h in spice/usbredir): packets of
a header (type, length, id) and a type-specific header and data.  QEMU
answers SET_ADDRESS itself and turns SET_CONFIGURATION and SET_INTERFACE
into packets of their own; every other control request arrives as a
control packet.  Isochronous OUT packets arrive one per guest packet;
for an IN endpoint QEMU asks once (start_iso_stream) and the device sends
packets at its own pace, which QEMU hands to the guest as it asks for
them.  Used by tools/selftest.py --suite devices.
"""
import argparse, math, os, signal, socket, struct, sys, threading, time

# usbredir packet types
HELLO, DEVICE_CONNECT, RESET, INTERFACE_INFO, EP_INFO = 0, 1, 3, 4, 5
SET_CONFIGURATION, GET_CONFIGURATION, CONFIGURATION_STATUS = 6, 7, 8
SET_ALT_SETTING, GET_ALT_SETTING, ALT_SETTING_STATUS = 9, 10, 11
START_ISO_STREAM, STOP_ISO_STREAM, ISO_STREAM_STATUS = 12, 13, 14
CONTROL_PACKET, ISO_PACKET = 100, 102
# capabilities: connect_device_version, ep_info_max_packet_size, 64bits_ids, 32bits_bulk_length
CAPS = (1 << 1) | (1 << 4) | (1 << 5) | (1 << 6)
CAP_64BIT_IDS = 1 << 5
SPEED_FULL, SPEED_HIGH = 1, 2
OK, STALL = 0, 4
RATE = 48000

EP_SPK, EP_MIC = 0x01, 0x82
CLOCK, SELECTOR = 0x10, 0x11        # Audio 2.0 clock source and selector IDs


def le16(v):
    return struct.pack('<H', v)


def rate3(v):
    return struct.pack('<I', v)[:3]


class Headset:
    """The descriptors and state of one device"""

    def __init__(self, speed, speaker, mic, uac2=False):
        self.speed, self.speaker, self.mic, self.uac2 = speed, speaker, mic, uac2
        high = speed == SPEED_HIGH
        # (bInterval, microframes or frames between packets, bytes a packet)
        self.spk_int = 1                                   # every microframe / every frame
        self.mic_int = 1 if uac2 or not high else 4        # every microframe (2.0) or millisecond
        self.spk_per_s = 8000 if high else 1000
        self.mic_per_s = 8000 if high and uac2 else 1000
        self.spk_sub, self.mic_sub, self.bits = (4, 3, 24) if uac2 else (2, 2, 16)   # bytes a sample, its bits
        self.spk_mps = RATE // self.spk_per_s * 2 * self.spk_sub
        self.mic_mps = RATE // self.mic_per_s * self.mic_sub
        self.rate = 0                                      # what the host set the clock to (2.0)
        self.config, self.alt = 0, {}
        proto = 0x20 if uac2 else 0
        self.ifaces = [(0, 1, 1, proto)]                   # (number, class, subclass, protocol)
        streaming = []
        if speaker:
            streaming.append(1)
        if mic:
            streaming.append(2)
        for n in streaming:
            self.ifaces.append((n, 1, 2, proto))
        self.cfg = self._config2(streaming) if uac2 else self._config(streaming)

    def _config(self, streaming):
        ac = b''
        if self.speaker:   # USB streaming in (1) -> feature unit (2) -> speaker (3)
            ac += bytes([12, 0x24, 2, 1]) + le16(0x0101) + bytes([0, 2]) + le16(3) + bytes([0, 0])
            ac += bytes([10, 0x24, 6, 2, 1, 1, 0x03, 0, 0, 0])
            ac += bytes([9, 0x24, 3, 3]) + le16(0x0301) + bytes([0, 2, 0])
        if self.mic:       # microphone (4) -> feature unit (5) -> USB streaming out (6)
            ac += bytes([12, 0x24, 2, 4]) + le16(0x0201) + bytes([0, 1]) + le16(0) + bytes([0, 0])
            ac += bytes([9, 0x24, 6, 5, 4, 1, 0x03, 0, 0])
            ac += bytes([9, 0x24, 3, 6]) + le16(0x0101) + bytes([0, 5, 0])
        hdr_len = 8 + len(streaming)
        ac = bytes([hdr_len, 0x24, 1]) + le16(0x0100) + le16(hdr_len + len(ac)) + bytes([len(streaming)] + streaming) + ac
        d = bytes([9, 4, 0, 0, 0, 1, 1, 0, 0]) + ac
        for n in streaming:
            spk = n == 1
            d += bytes([9, 4, n, 0, 0, 1, 2, 0, 0])                         # alt 0: no bandwidth
            d += bytes([9, 4, n, 1, 1, 1, 2, 0, 0])
            d += bytes([7, 0x24, 1, 1 if spk else 6, 1]) + le16(1)          # PCM
            d += bytes([11, 0x24, 2, 1, 2 if spk else 1, 2, 16, 1]) + rate3(RATE)
            ep, attrs = (EP_SPK, 0x09) if spk else (EP_MIC, 0x05)            # adaptive OUT, asynchronous IN
            d += bytes([9, 5, ep, attrs]) + le16(self.spk_mps if spk else self.mic_mps) + \
                bytes([self.spk_int if spk else self.mic_int, 0, 0])
            d += bytes([7, 0x25, 1, 0x01, 0]) + le16(0)                     # sampling frequency control
        return bytes([9, 2]) + le16(9 + len(d)) + bytes([len(self.ifaces), 1, 0, 0x80, 50]) + d

    def _config2(self, streaming):
        """The Audio 2.0 configuration: clock source CLOCK behind selector
        SELECTOR, which every terminal names as its clock"""
        ac = bytes([8, 0x24, 0x0A, CLOCK, 0x03, 0x07, 0, 0])            # internal programmable; rate rw, valid r
        ac += bytes([8, 0x24, 0x0B, SELECTOR, 1, CLOCK, 0x03, 0])       # one input; selector rw
        if self.speaker:   # USB streaming in (1) -> feature unit (2) -> speaker (3)
            ac += bytes([17, 0x24, 2, 1]) + le16(0x0101) + bytes([0, SELECTOR, 2]) + struct.pack('<I', 3) + \
                bytes([0]) + le16(0) + bytes([0])
            ac += bytes([18, 0x24, 6, 2, 1]) + struct.pack('<III', 0x0F, 0, 0) + bytes([0])   # master mute+volume rw
            ac += bytes([12, 0x24, 3, 3]) + le16(0x0301) + bytes([0, 2, SELECTOR]) + le16(0) + bytes([0])
        if self.mic:       # microphone (4) -> feature unit (5) -> USB streaming out (6)
            ac += bytes([17, 0x24, 2, 4]) + le16(0x0201) + bytes([0, SELECTOR, 1]) + struct.pack('<I', 0) + \
                bytes([0]) + le16(0) + bytes([0])
            ac += bytes([14, 0x24, 6, 5, 4]) + struct.pack('<II', 0x0F, 0) + bytes([0])
            ac += bytes([12, 0x24, 3, 6]) + le16(0x0101) + bytes([0, 5, SELECTOR]) + le16(0) + bytes([0])
        ac = bytes([9, 0x24, 1]) + le16(0x0200) + bytes([0x04]) + le16(9 + len(ac)) + bytes([0]) + ac   # headset
        d = bytes([8, 0x0B, 0, len(self.ifaces), 1, 0, 0x20, 0])                                  # association
        d += bytes([9, 4, 0, 0, 0, 1, 1, 0x20, 0]) + ac
        for n in streaming:
            spk = n == 1
            d += bytes([9, 4, n, 0, 0, 1, 2, 0x20, 0])                      # alt 0: no bandwidth
            d += bytes([9, 4, n, 1, 1, 1, 2, 0x20, 0])
            d += bytes([16, 0x24, 1, 1 if spk else 6, 0, 1]) + struct.pack('<I', 1) + \
                bytes([2 if spk else 1]) + struct.pack('<I', 3 if spk else 0) + bytes([0])   # PCM
            d += bytes([6, 0x24, 2, 1, self.spk_sub if spk else self.mic_sub, self.bits])
            ep, attrs = (EP_SPK, 0x09) if spk else (EP_MIC, 0x05)            # adaptive OUT, asynchronous IN
            d += bytes([7, 5, ep, attrs]) + le16(self.spk_mps if spk else self.mic_mps) + \
                bytes([self.spk_int if spk else self.mic_int])
            d += bytes([8, 0x25, 1, 0, 0, 0]) + le16(0)
        return bytes([9, 2]) + le16(9 + len(d)) + bytes([len(self.ifaces), 1, 0, 0x80, 50]) + d

    def product(self):
        return 0x0A0E if self.uac2 else 0x0A0D

    def device(self):
        cls = bytes([0xEF, 2, 1]) if self.uac2 else bytes([0, 0, 0])      # (2.0: interface association)
        return bytes([18, 1]) + le16(0x0200) + cls + bytes([64]) + le16(0x1209) + le16(self.product()) + \
            le16(0x0100) + bytes([1, 2, 0, 1])

    def string(self, i):
        if i == 0:
            return bytes([4, 3]) + le16(0x0409)
        s = {1: 'NovaOS', 2: ('Test Headset' if self.speaker else 'Test Microphone') +
             (' (USB Audio 2.0)' if self.uac2 else '')}.get(i)
        if s is None:
            return None
        u = s.encode('utf-16-le')
        return bytes([2 + len(u), 3]) + u

    def endpoints(self):
        """The endpoints of the current settings: [(address, type, interval, interface, max packet)]"""
        eps = [(0x00, 0, 0, 0, 64), (0x80, 0, 0, 0, 64)]
        if self.config:
            if self.speaker and self.alt.get(1):
                eps.append((EP_SPK, 1, self.spk_int if self.speed == SPEED_HIGH else 1, 1, self.spk_mps))
            if self.mic and self.alt.get(2):
                eps.append((EP_MIC, 1, (1 << (self.mic_int - 1)) if self.speed == SPEED_HIGH else 1, 2, self.mic_mps))
        return eps


class Wav:
    """A 48 kHz 16-bit stereo WAV file, its header kept current"""

    def __init__(self, path):
        self.f = open(path, 'wb')
        self.n = 0
        self.lock = threading.Lock()
        self._header()

    def _header(self):
        self.f.seek(0)
        self.f.write(b'RIFF' + struct.pack('<I', 36 + self.n) + b'WAVEfmt ' +
                     struct.pack('<IHHIIHH', 16, 1, 2, RATE, RATE * 4, 4, 16) + b'data' + struct.pack('<I', self.n))
        self.f.seek(0, 2)

    def write(self, data):
        with self.lock:
            self.f.write(data)
            self.n += len(data)

    def sync(self):
        with self.lock:
            self._header()
            self.f.flush()


class Conn:
    """One QEMU usb-redir connection"""

    def __init__(self, sock, dev, wav, mic_hz):
        self.sock, self.dev, self.wav, self.mic_hz = sock, dev, wav, mic_hz
        self.wlock = threading.Lock()
        self.ids64 = False
        self.mic_on = False
        self.mic_phase = 0
        # a second of the microphone's sine (a whole number of cycles for a whole HZ),
        # 16-bit samples in the top bytes of the device's sample slots
        sub = dev.mic_sub
        self.mic_wave = b''.join(struct.pack('<i', int(12000 * math.sin(2 * math.pi * mic_hz * i / RATE)) << 16)[4 - sub:]
                                 for i in range(RATE))

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
        d = self.dev
        n = len(d.ifaces)
        cols = [[f[k] for f in d.ifaces] + [0] * (32 - n) for k in range(4)]
        self.send(INTERFACE_INFO, struct.pack('<I', n) + b''.join(bytes(c) for c in cols))
        types, intervals, ifs, mps = [255] * 32, [0] * 32, [0] * 32, [0] * 32
        for addr, t, iv, iface, m in d.endpoints():
            i = ((addr & 0x80) >> 3) | (addr & 0x0F)
            types[i], intervals[i], ifs[i], mps[i] = t, iv, iface, m
        self.send(EP_INFO, bytes(types) + bytes(intervals) + bytes(ifs) + struct.pack('<32H', *mps))

    def mic_packets(self, n):
        """The next @n packets of the microphone's sine"""
        out = []
        frames, sub = RATE // self.dev.mic_per_s, self.dev.mic_sub
        for _ in range(n):
            out.append(self.mic_wave[self.mic_phase * sub:(self.mic_phase + frames) * sub])
            self.mic_phase = (self.mic_phase + frames) % RATE
        return out

    def mic_loop(self):
        """Send the microphone's packets in real time while its stream runs"""
        t0, sent = time.monotonic(), 0
        while self.mic_on:
            due = int((time.monotonic() - t0) * self.dev.mic_per_s)
            if due - sent > self.dev.mic_per_s // 5:    # (stalled: don't flood)
                sent = due - self.dev.mic_per_s // 50
            try:
                for pkt in self.mic_packets(due - sent):
                    self.send(ISO_PACKET, struct.pack('<BBH', EP_MIC, OK, len(pkt)), pkt)
            except OSError:                            # (QEMU went away)
                return
            sent = due
            time.sleep(0.002)

    def control(self, pid, hdr, data):
        ep, req, rtype, _, value, index, length = struct.unpack('<BBBBHHH', hdr)
        d = self.dev
        reply, status = b'', OK
        if rtype == 0x80 and req == 6:                 # GET_DESCRIPTOR
            kind, i = value >> 8, value & 0xFF
            reply = {1: d.device(), 2: d.cfg}.get(kind) if kind != 3 else d.string(i)
            if reply is None:
                reply, status = b'', STALL
        elif rtype == 0x80 and req == 0:               # GET_STATUS
            reply = b'\0\0'
        elif d.uac2 and rtype == 0xA1 and value == 0x0100 and index >> 8 == CLOCK:   # clock rate CUR / RANGE
            reply = struct.pack('<I', d.rate or RATE) if req == 1 else struct.pack('<HIII', 1, RATE, RATE, 0)
        elif d.uac2 and rtype == 0xA1 and req == 1 and index >> 8 == SELECTOR:
            reply = bytes([1])
        elif rtype & 0x80:                             # class GET_CUR/MIN/MAX/RES: zeros
            reply = bytes(length)
        elif d.uac2 and rtype == 0x21 and req == 1 and value == 0x0100 and index >> 8 == CLOCK:
            rate = struct.unpack('<I', data[:4])[0] if len(data) >= 4 else 0
            if rate != RATE:
                status = STALL
            else:
                d.rate = rate
                print(f'usbredirpeer: clock set to {rate} Hz', flush=True)
        elif d.uac2 and rtype == 0x21 and req == 1 and index >> 8 == SELECTOR and data[:1] != b'\x01':
            status = STALL
        elif rtype == 0x22 and req == 1 and value >> 8 == 1 and len(data) >= 3:      # SET_CUR sampling frequency
            rate = data[0] | data[1] << 8 | data[2] << 16
            if rate != RATE:
                status = STALL
        if rtype & 0x80:
            reply = reply[:length]
        self.send(CONTROL_PACKET, struct.pack('<BBBBHHH', ep, req, rtype, status, value, index,
                                              len(reply) if rtype & 0x80 else length), reply, pid)

    def run(self):
        self.send(HELLO, b'novaos usbredirpeer'.ljust(64, b'\0') + struct.pack('<I', CAPS))
        connected = False
        try:
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
                        self.send(DEVICE_CONNECT, struct.pack('<BBBBHHH', self.dev.speed, *self.dev.device()[4:7], 0x1209,
                                                              self.dev.product(), 0x0100))
                elif ptype == CONTROL_PACKET:
                    self.control(pid, body[:10], body[10:])
                elif ptype == SET_CONFIGURATION:
                    self.dev.config, self.dev.alt = body[0], {}
                    self.send_info()
                    self.send(CONFIGURATION_STATUS, bytes([OK, self.dev.config]), pid=pid)
                elif ptype == GET_CONFIGURATION:
                    self.send(CONFIGURATION_STATUS, bytes([OK, self.dev.config]), pid=pid)
                elif ptype == SET_ALT_SETTING:
                    iface, alt = body[0], body[1]
                    ok = any(f[0] == iface for f in self.dev.ifaces) and alt <= (1 if iface else 0)
                    if ok:
                        self.dev.alt[iface] = alt
                        self.send_info()
                    self.send(ALT_SETTING_STATUS, bytes([OK if ok else STALL, iface, alt]), pid=pid)
                elif ptype == GET_ALT_SETTING:
                    self.send(ALT_SETTING_STATUS, bytes([OK, body[0], self.dev.alt.get(body[0], 0)]), pid=pid)
                elif ptype == START_ISO_STREAM:
                    self.send(ISO_STREAM_STATUS, bytes([OK, body[0]]), pid=pid)
                    if body[0] == EP_MIC and not self.mic_on:
                        self.mic_on = True
                        threading.Thread(target=self.mic_loop, daemon=True).start()
                elif ptype == STOP_ISO_STREAM:
                    if body[0] == EP_MIC:
                        self.mic_on = False
                    self.send(ISO_STREAM_STATUS, bytes([OK, body[0]]), pid=pid)
                elif ptype == ISO_PACKET:
                    ep, st, n = struct.unpack('<BBH', body[:4])
                    if ep == EP_SPK and self.wav:
                        pcm = body[4:4 + n]
                        sub = self.dev.spk_sub
                        if sub != 2:                   # the top 16 bits of each sample
                            pcm = b''.join(pcm[i + sub - 2:i + sub] for i in range(0, len(pcm) - sub + 1, sub))
                        self.wav.write(pcm)
                elif ptype == RESET:
                    self.dev.alt = {}
                # (anything else: filters, cancels, disconnect acks: nothing to do)
        except (EOFError, ConnectionError):
            pass
        finally:
            self.mic_on = False
            if self.wav:
                self.wav.sync()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--port', type=int, required=True)
    ap.add_argument('--speed', choices=('high', 'full'), default='high')
    ap.add_argument('--speaker', help='the WAV file the speaker writes')
    ap.add_argument('--mic', type=float, help='the tone the microphone hears (Hz)')
    ap.add_argument('--uac2', action='store_true', help='a USB Audio Class 2.0 device')
    a = ap.parse_args()
    if not a.speaker and not a.mic:
        sys.exit('a --speaker, a --mic or both')
    wav = Wav(a.speaker) if a.speaker else None

    def done(*_):
        if wav:
            wav.sync()
        os._exit(0)
    signal.signal(signal.SIGTERM, done)
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('127.0.0.1', a.port))
    srv.listen(1)
    print(f'usbredirpeer: listening on {a.port}', flush=True)

    def sync_loop():
        while True:
            time.sleep(0.5)
            if wav:
                wav.sync()
    threading.Thread(target=sync_loop, daemon=True).start()
    while True:
        s, _ = srv.accept()
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        print('usbredirpeer: QEMU connected', flush=True)
        dev = Headset(SPEED_HIGH if a.speed == 'high' else SPEED_FULL, bool(a.speaker), bool(a.mic), a.uac2)
        Conn(s, dev, wav, a.mic or 0).run()
        print('usbredirpeer: QEMU disconnected', flush=True)


if __name__ == '__main__':
    main()
