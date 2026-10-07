#!/usr/bin/env python3
"""A USB Audio Class 1 or 2 headset or microphone for testing NovaOS, as a
QEMU usb-redir device.

    tools/usbredirpeer.py --port 10700 [--speed high|full] [--uac2] [--speaker OUT.wav] [--mic HZ]
                          [--rates 44100,96000] [--channels N] [--mic-channels N] [--product NAME]
                          [--feedback HZ] [--slow-control MS]

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

--rates lists the sampling rates the device offers instead of 48 kHz
alone (Audio 2.0: its clock's RANGE, one subrange a rate, and setting the
clock to any other rate stalls; Audio 1.0: the format's rate list and the
endpoint's rate control); the speaker's WAV is written at the rate the
host chose, and the microphone's sine is made at it, packets carrying a
varying whole number of frames (5 or 6 a microframe at 44.1 kHz).
--channels and --mic-channels give the speaker and the microphone more
(or fewer) than two and one channels: the WAV holds the speaker's first
two (front left and right; a mono speaker's one twice), and any sample
of the others that is not silent is counted into OUT.wav.extra; the
microphone's first channel and second (if any) hear the sine and the
others a 300 Hz one, which the host must not take.  --product names the
device (its product string).

--feedback makes the speaker's endpoint asynchronous, with a feedback
endpoint (0x81) through which the device says it plays HZ frames a
second, as a device on its own clock does: at full speed in 10.14 fixed
point frames a millisecond (Audio 1.0: an endpoint the speaker's
bSynchAddress names), at high speed in 16.16 frames a microframe (Audio
2.0: an endpoint of usage "feedback"), sent every millisecond.  The host
must then fill each packet with HZ / packets-a-second frames on average
instead of the nominal rate's: OUT.wav.fb gets the packets and frames
received once the feedback has been running for half a second.

--slow-control holds the first control request's answer back MS milliseconds,
as a device (or a busy host) that is slow to answer its first descriptor
request: NovaOS must wait for it rather than give the device up.

The program is as cheap as a Python program can be at 8,000 packets a
second a direction (CI runs it beside QEMU on a shared machine): it reads
the socket in big chunks, converts and writes the speaker's samples in
batches, and sends the microphone's packets one by one (a burst of them in a single write makes QEMU's buffer overflow).  With
--log-times (selftest's logs) it prints what happens on the connection (the
control requests, stream starts and stops, resets), each with the time
since the program started, and every five seconds the packets it has
taken and sent, so a stall shows in the log.

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

T0 = time.monotonic()
LOG_TIMES = False                                   # --log-times

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

EP_SPK, EP_MIC, EP_FB = 0x01, 0x82, 0x81
CLOCK, SELECTOR = 0x10, 0x11        # Audio 2.0 clock source and selector IDs


def le16(v):
    return struct.pack('<H', v)


def rate3(v):
    return struct.pack('<I', v)[:3]


class Headset:
    """The descriptors and state of one device"""

    def __init__(self, speed, speaker, mic, uac2=False, rates=(RATE,), channels=2, mic_channels=1, product=None,
                 feedback=0):
        self.speed, self.speaker, self.mic, self.uac2 = speed, speaker, mic, uac2
        self.rates, self.channels, self.mic_channels, self.product_name = list(rates), channels, mic_channels, product
        self.feedback = feedback if speaker else 0
        high = speed == SPEED_HIGH
        # (bInterval, microframes or frames between packets, bytes a packet)
        self.spk_int = 1                                   # every microframe / every frame
        self.mic_int = 1 if uac2 or not high else 4        # every microframe (2.0) or millisecond
        self.spk_per_s = 8000 if high else 1000
        self.mic_per_s = 8000 if high and uac2 else 1000
        self.spk_sub, self.mic_sub, self.bits = (4, 3, 24) if uac2 else (2, 2, 16)   # bytes a sample, its bits
        top = max(self.rates)                              # (a frame more where packets vary)
        spk_top = top * 9 // 8 if self.feedback else top   # (room for the feedback's eighth either way)
        self.spk_mps = (-(-spk_top // self.spk_per_s) + (spk_top % self.spk_per_s != 0)) * channels * self.spk_sub
        self.fb_mps = 4 if high else 3                     # 16.16 a microframe / 10.14 a millisecond
        self.fb_int = 4 if high else 1                     # every millisecond (8 microframes / 1 frame)
        self.mic_mps = (-(-top // self.mic_per_s) + (top % self.mic_per_s != 0)) * mic_channels * self.mic_sub
        self.rate = 0                                      # what the host set the clock (2.0) or endpoint (1.0) to
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
            ac += bytes([12, 0x24, 2, 1]) + le16(0x0101) + bytes([0, self.channels]) + le16(3) + bytes([0, 0])
            ac += bytes([10, 0x24, 6, 2, 1, 1, 0x03, 0, 0, 0])
            ac += bytes([9, 0x24, 3, 3]) + le16(0x0301) + bytes([0, 2, 0])
        if self.mic:       # microphone (4) -> feature unit (5) -> USB streaming out (6)
            ac += bytes([12, 0x24, 2, 4]) + le16(0x0201) + bytes([0, self.mic_channels]) + le16(0) + bytes([0, 0])
            ac += bytes([9, 0x24, 6, 5, 4, 1, 0x03, 0, 0])
            ac += bytes([9, 0x24, 3, 6]) + le16(0x0101) + bytes([0, 5, 0])
        hdr_len = 8 + len(streaming)
        ac = bytes([hdr_len, 0x24, 1]) + le16(0x0100) + le16(hdr_len + len(ac)) + bytes([len(streaming)] + streaming) + ac
        d = bytes([9, 4, 0, 0, 0, 1, 1, 0, 0]) + ac
        for n in streaming:
            spk = n == 1
            d += bytes([9, 4, n, 0, 0, 1, 2, 0, 0])                         # alt 0: no bandwidth
            fb = spk and self.feedback
            d += bytes([9, 4, n, 1, 2 if fb else 1, 1, 2, 0, 0])
            d += bytes([7, 0x24, 1, 1 if spk else 6, 1]) + le16(1)          # PCM
            d += bytes([8 + 3 * len(self.rates), 0x24, 2, 1, self.channels if spk else self.mic_channels, 2, 16,
                        len(self.rates)]) + b''.join(rate3(r) for r in self.rates)
            ep, attrs = (EP_SPK, 0x05 if fb else 0x09) if spk else (EP_MIC, 0x05)   # adaptive (or async) OUT, async IN
            d += bytes([9, 5, ep, attrs]) + le16(self.spk_mps if spk else self.mic_mps) + \
                bytes([self.spk_int if spk else self.mic_int, 0, EP_FB if fb else 0])   # (bSynchAddress)
            d += bytes([7, 0x25, 1, 0x01, 0]) + le16(0)                     # sampling frequency control
            if fb:                                                          # the synch endpoint: refreshed every 2 ms
                d += bytes([9, 5, EP_FB, 0x01]) + le16(self.fb_mps) + bytes([self.fb_int, 1, 0])
        return bytes([9, 2]) + le16(9 + len(d)) + bytes([len(self.ifaces), 1, 0, 0x80, 50]) + d

    def _config2(self, streaming):
        """The Audio 2.0 configuration: clock source CLOCK behind selector
        SELECTOR, which every terminal names as its clock"""
        ac = bytes([8, 0x24, 0x0A, CLOCK, 0x03, 0x07, 0, 0])            # internal programmable; rate rw, valid r
        ac += bytes([8, 0x24, 0x0B, SELECTOR, 1, CLOCK, 0x03, 0])       # one input; selector rw
        if self.speaker:   # USB streaming in (1) -> feature unit (2) -> speaker (3)
            ac += bytes([17, 0x24, 2, 1]) + le16(0x0101) + bytes([0, SELECTOR, self.channels]) + struct.pack('<I', 3) + \
                bytes([0]) + le16(0) + bytes([0])
            ac += bytes([18, 0x24, 6, 2, 1]) + struct.pack('<III', 0x0F, 0, 0) + bytes([0])   # master mute+volume rw
            ac += bytes([12, 0x24, 3, 3]) + le16(0x0301) + bytes([0, 2, SELECTOR]) + le16(0) + bytes([0])
        if self.mic:       # microphone (4) -> feature unit (5) -> USB streaming out (6)
            ac += bytes([17, 0x24, 2, 4]) + le16(0x0201) + bytes([0, SELECTOR, self.mic_channels]) + struct.pack('<I', 0) + \
                bytes([0]) + le16(0) + bytes([0])
            ac += bytes([14, 0x24, 6, 5, 4]) + struct.pack('<II', 0x0F, 0) + bytes([0])
            ac += bytes([12, 0x24, 3, 6]) + le16(0x0101) + bytes([0, 5, SELECTOR]) + le16(0) + bytes([0])
        ac = bytes([9, 0x24, 1]) + le16(0x0200) + bytes([0x04]) + le16(9 + len(ac)) + bytes([0]) + ac   # headset
        d = bytes([8, 0x0B, 0, len(self.ifaces), 1, 0, 0x20, 0])                                  # association
        d += bytes([9, 4, 0, 0, 0, 1, 1, 0x20, 0]) + ac
        for n in streaming:
            spk = n == 1
            d += bytes([9, 4, n, 0, 0, 1, 2, 0x20, 0])                      # alt 0: no bandwidth
            fb = spk and self.feedback
            d += bytes([9, 4, n, 1, 2 if fb else 1, 1, 2, 0x20, 0])
            d += bytes([16, 0x24, 1, 1 if spk else 6, 0, 1]) + struct.pack('<I', 1) + \
                bytes([self.channels if spk else self.mic_channels]) + struct.pack('<I', 3 if spk else 0) + bytes([0])   # PCM
            d += bytes([6, 0x24, 2, 1, self.spk_sub if spk else self.mic_sub, self.bits])
            ep, attrs = (EP_SPK, 0x05 if fb else 0x09) if spk else (EP_MIC, 0x05)   # adaptive (or async) OUT, async IN
            d += bytes([7, 5, ep, attrs]) + le16(self.spk_mps if spk else self.mic_mps) + \
                bytes([self.spk_int if spk else self.mic_int])
            d += bytes([8, 0x25, 1, 0, 0, 0]) + le16(0)
            if fb:                                                          # explicit feedback (usage 01)
                d += bytes([7, 5, EP_FB, 0x11]) + le16(self.fb_mps) + bytes([self.fb_int])
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
        s = {1: 'NovaOS', 2: self.product_name or ('Test Headset' if self.speaker else 'Test Microphone') +
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
                if self.feedback:
                    eps.append((EP_FB, 1, (1 << (self.fb_int - 1)) if self.speed == SPEED_HIGH else 1, 1, self.fb_mps))
            if self.mic and self.alt.get(2):
                eps.append((EP_MIC, 1, (1 << (self.mic_int - 1)) if self.speed == SPEED_HIGH else 1, 2, self.mic_mps))
        return eps


class Wav:
    """A 16-bit stereo WAV file (48 kHz until the host sets another rate),
    its header kept current; @extra counts the speaker's samples beyond its
    first two channels that are not silent (written to PATH.extra).  The
    speaker's packets are queued by add() and converted in batches (@sub,
    @ch: bytes a sample slot, channels of the device's own format)"""

    BATCH = 256                                        # packets

    def __init__(self, path, sub=2, ch=2):
        self.f = open(path, 'wb')
        self.path = path
        self.n = 0
        self.rate = RATE
        self.extra = 0
        self.sub, self.ch = sub, ch
        self.pending = []
        self.lock = threading.Lock()
        self._header()

    def _header(self):
        self.f.seek(0)
        self.f.write(b'RIFF' + struct.pack('<I', 36 + self.n) + b'WAVEfmt ' +
                     struct.pack('<IHHIIHH', 16, 1, 2, self.rate, self.rate * 4, 4, 16) + b'data' + struct.pack('<I', self.n))
        self.f.seek(0, 2)

    def convert(self, pcm):
        """@pcm (whole frames of the device's format) as 16-bit stereo: the
        top 16 bits of each slot, the first two channels (a mono one's twice);
        the other channels' samples that are not silent go into @extra"""
        sub, ch = self.sub, self.ch
        if sub != 2:
            lo, hi = pcm[sub - 2::sub], pcm[sub - 1::sub]
            pcm = bytearray(2 * len(lo))
            pcm[0::2], pcm[1::2] = lo, hi
        if ch == 2:
            return bytes(pcm)
        step = 2 * ch
        out = bytearray(2 * len(pcm) // ch)
        if ch == 1:
            out[0::4] = out[2::4] = pcm[0::2]
            out[1::4] = out[3::4] = pcm[1::2]
            return bytes(out)
        for k in range(4):
            out[k::4] = pcm[k::step]
        for k in range(2, ch):
            lo, hi = pcm[2 * k::step], pcm[2 * k + 1::step]
            if lo.strip(b'\0') or hi.strip(b'\0'):
                self.extra += sum(1 for a, b in zip(lo, hi) if a or b)
        return bytes(out)

    def _drain(self):
        if self.pending:
            data = self.convert(b''.join(self.pending))
            self.pending = []
            self.f.write(data)
            self.n += len(data)

    def add(self, packet):
        """One speaker packet (a partial last frame is dropped)"""
        fb = self.sub * self.ch
        if len(packet) % fb:
            packet = packet[:len(packet) // fb * fb]
        with self.lock:
            self.pending.append(packet)
            if len(self.pending) >= self.BATCH:
                self._drain()

    def sync(self):
        with self.lock:
            self._drain()
            self._header()
            self.f.flush()
            with open(self.path + '.extra', 'w') as x:
                x.write(f'{self.extra}\n')


class Conn:
    """One QEMU usb-redir connection"""

    def __init__(self, sock, dev, wav, mic_hz, slow_control=0):
        self.sock, self.dev, self.wav, self.mic_hz = sock, dev, wav, mic_hz
        self.slow_control = slow_control / 1000            # (the first control answer waits this long)
        self.wlock = threading.Lock()
        self.rbuf = bytearray()
        self.ids64 = False
        self.mic_stop = None                               # the running microphone loop's Event (set: stop)
        self.mic_wave = None                               # (rate, a second of frames)
        self.spk_packets = self.mic_sent = 0               # (for the log)
        self.fb_on = False
        self.fb_since = None                           # when feedback started (for the counts)
        self.fb_packets = self.fb_frames = 0

    def make_mic_wave(self):
        """(rate, a second of the microphone's frames at its rate: a whole
        number of cycles for a whole HZ; the sine on its first two channels,
        300 Hz on any others; 16-bit samples in the top bytes of the device's
        slots), made once for each rate the host sets"""
        d, sub = self.dev, self.dev.mic_sub
        rate = d.rate or RATE
        if self.mic_wave and self.mic_wave[0] == rate:
            return self.mic_wave
        def slot(v):
            return struct.pack('<i', int(v) << 16)[4 - sub:]
        frames = []
        for i in range(rate):
            a = slot(12000 * math.sin(2 * math.pi * self.mic_hz * i / rate))
            b = slot(12000 * math.sin(2 * math.pi * 300 * i / rate))
            frames.append(a * min(2, d.mic_channels) + b * max(0, d.mic_channels - 2))
        self.mic_wave = (rate, b''.join(frames))           # (one assignment: a running loop sees whole ones)
        return self.mic_wave

    def send(self, ptype, hdr=b'', data=b'', pid=0):
        h = struct.pack('<II', ptype, len(hdr) + len(data)) + struct.pack('<Q' if self.ids64 else '<I', pid)
        with self.wlock:
            self.sock.sendall(h + hdr + data)

    def recv_exact(self, n):
        """The next @n bytes (the socket is read in big chunks: at 16,000
        packets a second a recv for each would be most of this program's work)"""
        buf = self.rbuf
        while len(buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise EOFError
            buf += chunk
        out = bytes(buf[:n])
        del buf[:n]
        return out

    def stats(self):
        """Every five seconds, what moved (to the log: a stall shows there)"""
        last = (0, 0)
        while self.alive:
            time.sleep(5)
            now = (self.spk_packets, self.mic_sent)
            if now != last:
                self.log(f'speaker packets taken {now[0]}, microphone packets sent {now[1]}')
                last = now

    def log(self, text):
        if LOG_TIMES:
            print(f'usbredirpeer: +{time.monotonic() - T0:.3f}s {text}', flush=True)

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

    def mic_packets(self, run, n):
        """The next @n packets of the microphone's sine (a whole number of
        frames each, averaging its rate); @run is the loop's phase [frame, remainder]"""
        out = []
        rate, wave = self.make_mic_wave()
        per_s, fb = self.dev.mic_per_s, self.dev.mic_sub * self.dev.mic_channels
        for _ in range(n):
            run[1] += rate
            frames, run[1] = run[1] // per_s, run[1] % per_s
            a, b = run[0], run[0] + frames
            pkt = wave[a * fb:b * fb]
            if b > rate:
                pkt += wave[:(b - rate) * fb]
            out.append(pkt)
            run[0] = b % rate
        return out

    def mic_loop(self, stop):
        """Send the microphone's packets in real time until @stop is set, a
        write for each (a burst of them in one write overflows QEMU's buffer
        and the guest hears nothing; a loop of its own for each start: a stop
        and a start close together never leave two running)"""
        t0, sent, run = time.monotonic(), 0, [0, 0]
        per_s = self.dev.mic_per_s
        while not stop.is_set():
            due = int((time.monotonic() - t0) * per_s)
            if due - sent > per_s // 5:                 # (stalled: don't flood)
                sent = due - per_s // 50
            if due > sent:
                pkts = self.mic_packets(run, due - sent)
                tail = struct.pack('<Q' if self.ids64 else '<I', 0)
                try:
                    for pk in pkts:
                        with self.wlock:
                            self.sock.sendall(struct.pack('<II', ISO_PACKET, 4 + len(pk)) + tail + struct.pack('<BBH', EP_MIC, OK, len(pk)) + pk)
                except OSError:                         # (QEMU went away)
                    return
                self.mic_sent += len(pkts)
                sent = due
            time.sleep(0.002)

    def mic_start(self):
        self.mic_stop_loop()
        self.mic_stop = threading.Event()
        threading.Thread(target=self.mic_loop, args=(self.mic_stop,), daemon=True).start()

    def mic_stop_loop(self):
        if self.mic_stop:
            self.mic_stop.set()
            self.mic_stop = None

    def fb_value(self):
        """The feedback endpoint's value: the frames the device plays, at high
        speed in 16.16 a microframe, at full speed in 10.14 a millisecond"""
        if self.dev.speed == SPEED_HIGH:
            return struct.pack('<I', round(self.dev.feedback * 65536 / 8000))
        return struct.pack('<I', round(self.dev.feedback * 16384 / 1000))[:3]

    def fb_loop(self):
        """Send the feedback every millisecond while its stream runs"""
        t0, sent, pkt = time.monotonic(), 0, self.fb_value()
        self.fb_since = t0
        while self.fb_on:
            due = int((time.monotonic() - t0) * 1000)
            if due - sent > 200:
                sent = due - 20
            try:
                for _ in range(due - sent):
                    self.send(ISO_PACKET, struct.pack('<BBH', EP_FB, OK, len(pkt)), pkt)
            except OSError:
                return
            sent = due
            time.sleep(0.002)

    def fb_sync(self):
        if self.wav and self.dev.feedback:
            with open(self.wav.path + '.fb', 'w') as f:
                f.write(f'{self.fb_packets} {self.fb_frames}\n')

    def control(self, pid, hdr, data):
        ep, req, rtype, _, value, index, length = struct.unpack('<BBBBHHH', hdr)
        d = self.dev
        reply, status = b'', OK
        self.log(f'control type {rtype:02x} request {req:02x} value {value:04x} index {index:04x} length {length}')
        if self.slow_control:                          # the first request's answer comes late
            time.sleep(self.slow_control)
            self.slow_control = 0
        if rtype == 0x80 and req == 6:                 # GET_DESCRIPTOR
            kind, i = value >> 8, value & 0xFF
            reply = {1: d.device(), 2: d.cfg}.get(kind) if kind != 3 else d.string(i)
            if reply is None:
                reply, status = b'', STALL
        elif rtype == 0x80 and req == 0:               # GET_STATUS
            reply = b'\0\0'
        elif d.uac2 and rtype == 0xA1 and value == 0x0100 and index >> 8 == CLOCK:   # clock rate CUR / RANGE
            reply = struct.pack('<I', d.rate or d.rates[0]) if req == 1 else \
                struct.pack('<H', len(d.rates)) + b''.join(struct.pack('<III', r, r, 0) for r in d.rates)
        elif d.uac2 and rtype == 0xA1 and req == 1 and index >> 8 == SELECTOR:
            reply = bytes([1])
        elif rtype & 0x80:                             # class GET_CUR/MIN/MAX/RES: zeros
            reply = bytes(length)
        elif d.uac2 and rtype == 0x21 and req == 1 and value == 0x0100 and index >> 8 == CLOCK:
            rate = struct.unpack('<I', data[:4])[0] if len(data) >= 4 else 0
            if rate not in d.rates:
                status = STALL
            else:
                self.set_rate(rate)
                print(f'usbredirpeer: clock set to {rate} Hz', flush=True)
        elif d.uac2 and rtype == 0x21 and req == 1 and index >> 8 == SELECTOR and data[:1] != b'\x01':
            status = STALL
        elif rtype == 0x22 and req == 1 and value >> 8 == 1 and len(data) >= 3:      # SET_CUR sampling frequency
            rate = data[0] | data[1] << 8 | data[2] << 16
            if rate not in d.rates:
                status = STALL
            else:
                self.set_rate(rate)
        if rtype & 0x80:
            reply = reply[:length]
        self.send(CONTROL_PACKET, struct.pack('<BBBBHHH', ep, req, rtype, status, value, index,
                                              len(reply) if rtype & 0x80 else length), reply, pid)

    def set_rate(self, rate):
        self.dev.rate = rate
        if self.wav:
            self.wav.rate = rate

    def run(self):
        if self.wav:
            self.wav.rate = self.dev.rates[0]           # (until the host sets one)
        self.send(HELLO, b'novaos usbredirpeer'.ljust(64, b'\0') + struct.pack('<I', CAPS))
        connected = False
        self.alive = True
        threading.Thread(target=self.stats, daemon=True).start()
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
                    self.log(f'set configuration {body[0]}')
                    self.dev.config, self.dev.alt = body[0], {}
                    self.send_info()
                    self.send(CONFIGURATION_STATUS, bytes([OK, self.dev.config]), pid=pid)
                elif ptype == GET_CONFIGURATION:
                    self.send(CONFIGURATION_STATUS, bytes([OK, self.dev.config]), pid=pid)
                elif ptype == SET_ALT_SETTING:
                    iface, alt = body[0], body[1]
                    self.log(f'set interface {iface} alternate setting {alt}')
                    ok = any(f[0] == iface for f in self.dev.ifaces) and alt <= (1 if iface else 0)
                    if ok:
                        self.dev.alt[iface] = alt
                        self.send_info()
                    self.send(ALT_SETTING_STATUS, bytes([OK if ok else STALL, iface, alt]), pid=pid)
                elif ptype == GET_ALT_SETTING:
                    self.send(ALT_SETTING_STATUS, bytes([OK, body[0], self.dev.alt.get(body[0], 0)]), pid=pid)
                elif ptype == START_ISO_STREAM:
                    self.log(f'start iso stream {body[0]:02x}')
                    self.send(ISO_STREAM_STATUS, bytes([OK, body[0]]), pid=pid)
                    if body[0] == EP_MIC and not self.mic_stop:
                        self.mic_start()
                    if body[0] == EP_FB and not self.fb_on:
                        self.fb_on = True
                        threading.Thread(target=self.fb_loop, daemon=True).start()
                elif ptype == STOP_ISO_STREAM:
                    self.log(f'stop iso stream {body[0]:02x}')
                    if body[0] == EP_MIC:
                        self.mic_stop_loop()
                    if body[0] == EP_FB:
                        self.fb_on = False
                    self.send(ISO_STREAM_STATUS, bytes([OK, body[0]]), pid=pid)
                elif ptype == ISO_PACKET:
                    ep, st, n = struct.unpack('<BBH', body[:4])
                    if ep == EP_SPK and self.wav:
                        self.spk_packets += 1
                        if self.fb_since and time.monotonic() - self.fb_since > 0.5:
                            self.fb_packets += 1
                            self.fb_frames += n // (self.dev.spk_sub * self.dev.channels)
                            if self.fb_packets % 500 == 0:
                                self.fb_sync()
                        self.wav.add(body[4:4 + n])
                elif ptype == RESET:
                    self.log('reset')
                    self.dev.alt = {}
                # (anything else: filters, cancels, disconnect acks: nothing to do)
        except (EOFError, ConnectionError):
            pass
        finally:
            self.alive = False
            self.mic_stop_loop()
            self.fb_on = False
            if self.wav:
                self.wav.sync()
                self.fb_sync()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--port', type=int, required=True)
    ap.add_argument('--speed', choices=('high', 'full'), default='high')
    ap.add_argument('--speaker', help='the WAV file the speaker writes')
    ap.add_argument('--mic', type=float, help='the tone the microphone hears (Hz)')
    ap.add_argument('--uac2', action='store_true', help='a USB Audio Class 2.0 device')
    ap.add_argument('--rates', default=str(RATE), help='the sampling rates it offers (comma-separated)')
    ap.add_argument('--channels', type=int, default=2, help="the speaker's channels")
    ap.add_argument('--mic-channels', type=int, default=1, help="the microphone's channels")
    ap.add_argument('--product', help='its product string')
    ap.add_argument('--feedback', type=int, default=0, help="the speaker's own rate, sent on a feedback endpoint (Hz)")
    ap.add_argument('--slow-control', type=int, default=0, metavar='MS', help="answer the first control request MS late")
    ap.add_argument('--log-times', action='store_true', help='log the connection\'s events with times')
    a = ap.parse_args()
    global LOG_TIMES
    LOG_TIMES = a.log_times
    rates = [int(r) for r in a.rates.split(',')]
    if not a.speaker and not a.mic:
        sys.exit('a --speaker, a --mic or both')
    uac2_slots = (4, a.channels) if a.uac2 else (2, a.channels)     # (the speaker's slot bytes, channels)
    wav = Wav(a.speaker, *uac2_slots) if a.speaker else None

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
        dev = Headset(SPEED_HIGH if a.speed == 'high' else SPEED_FULL, bool(a.speaker), bool(a.mic), a.uac2,
                      rates, a.channels, a.mic_channels, a.product, a.feedback)
        Conn(s, dev, wav, a.mic or 0, a.slow_control).run()
        print('usbredirpeer: QEMU disconnected', flush=True)


if __name__ == '__main__':
    main()
