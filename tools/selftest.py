#!/usr/bin/env python3
"""Boot NovaOS in QEMU, run the self-test programs and say which passed.

    tools/selftest.py [--suite core|graphics|network|devices] [--img build/nova.img] [--out DIR]
                      [--only NAME,...] [--junit FILE] [--summary FILE] [--list]

Suites (one file per test in tests/selftest/SUITE/, run in file-name
order; --list prints them):
  core      (default) the self-test programs (apitest, abitest, filetest...),
            an install finished by a restart, and last "crash kernel" (a
            deliberate kernel fault must print a symbolized backtrace;
            the machine is then reset and the next start must turn the
            fault into a report in C:\\NovaOS\\Crashes)
  graphics  on two monitors (a 3D virtio-gpu with Venus is the first, a
            QEMU secondary-vga the second; montest), installs "Mesa 3D",
            "DXVK" and "Venus" with the App Store, then runs tools/gltest
            (on Mesa's virgl, which runs OpenGL on this machine's GPU, and
            on llvmpipe) and tools/d3dtest (DXVK on Venus, which runs
            Vulkan there; also as ANGLE brings Direct3D 11 up), 64- and
            32-bit, the frame-rate tests (virgl against llvmpipe, Venus
            against lavapipe), and NetSurf on a page
            with an SVG and a script (nstest).  Needs --gfx DIR, made by
            tools/ci/stage-graphics.sh: 7-Zip, the three downloads and the
            test programs; and a QEMU with Venus with an OpenGL display
            (tools/ci/build-qemu-venus.sh; on a machine without a screen, run
            it under xvfb-run)
  network   two boots with a virtio-net adapter (tests/selftest/network4
            and network6).  IPv4 on QEMU's user-mode network: ipconfig, ping,
            Winsock (netcat), winhttp's HTTP/2 (httptest suite) and eight
            long downloads at once (dltest -w) against tools/h2server.js
            (needs node and openssl).  IPv6 on an IPv6-only
            network that is tools/v6peer.py: SLAAC and RDNSS (ipconfig),
            ping -6, curl -6 and Winsock over IPv6 (netcat).  Then a third
            boot with an Intel e1000e (82574L) instead of virtio-net
            (tests/selftest/network-e1000e): its PHY and link, the link
            pulled and plugged back, and the IPv4 tests again
  devices   a boot per device QEMU has that the core boot hasn't
            (tests/selftest/devices/NAME/): "touch", a virtio multi-touch
            screen (touchtest); "usbaudio", USB speakers on xHCI, OHCI and
            UHCI and no HD Audio card (soundtest; each speaker's WAV must
            hold its tones); "usbheadset", a high-speed USB headset on EHCI,
            USB microphones plugged into xHCI, OHCI and UHCI and a
            high-speed USB Audio 2.0 headset plugged into xHCI, each
            tools/usbredirpeer.py behind a QEMU usb-redir device (soundtest
            tone, record and capture); "monitors", one virtio-vga card with
            three outputs, whose monitors the test plugs in and unplugs
            while NovaOS runs (montest hotplug); "usbboot", nova.iso
            written to a USB stick and nothing else to start from, with
            only the firmware's GOP for a display: it must start live,
            write its boot log into EFI/NOVA/bootlog.txt on the stick,
            and a kernel fault must reach that file too; "cdboot", the
            same ISO as a disc in a SATA DVD drive, which must start live;
            "laptop", a laptop without S3 whose lid, battery and AC adapter
            are behind an embedded controller (tests/acpi/laptop.asl): the
            battery, the lid sleeping it in low-power S0 idle and waking
            it, then NovaOS installed from a USB stick onto an NVMe disk
            and started from there; "update", an installed NovaOS on a
            network serving update channels (tools/mkupdate.py): it
            orders a development build of its version after it,
            refuses channels that are unsigned, signed with another key
            or changed after signing, then updates itself from a signed
            one to a newer test build of this kernel, restarts
            into it twice, and goes back to it when the next update is
            reset while it first starts; "gamepad", a wired Xbox 360, an
            Xbox One and a HID game pad on xHCI (tools/padpeer.py behind
            usb-redir devices): padtest reads their buttons and sticks
            through XInput and DirectInput 8 while the test moves them,
            sets their motors, and one is unplugged

Each test is one Terminal command (tools/novarun.py's Nova class types it).
A test passes when the program exits with code 0 inside its time limit, has
printed no "FAIL" line and no "N failed" count above zero, and prints what
the test expects.  A kernel panic fails the run.  The serial log, a
screenshot after each test and the sound recordings are kept in --out.
The core boot's sound card hears a 523 Hz tone (novarun --rec, so the
host needs PulseAudio; without it the tests that record are reported as
failed and the rest run).

The exit status is the number of failed tests (0: all passed), so CI can
gate on it.  --summary appends a Markdown table (GitHub's step summary).
"""
import argparse, copy, os, re, shutil, subprocess, sys, tempfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from novarun import Nova, ROOT, qemu_binary
import wavcheck


class Test:
    """@cmd typed into the Terminal.  @store: instead of a program's exit,
    wait for the App Store's "[STORE] @store: Installed" line.  @shot: take
    the screenshot 2 s after the output matches this regex (while the
    program draws).  @crash: the command halts the kernel on purpose; the
    test passes when the serial log then shows @expect (it runs last,
    unless @restart: then the machine is reset and later tests run in the
    new boot, as after a crash on a real PC).
    @reboot: restart NovaOS ("shutdown /r", drive C: kept) and pass when
    the new boot's log shows @expect; later tests run in that boot.
    @acts: (regex, function(nova)) pairs run when the output matches (the
    program asks for something only the test can do).  @boot_expect: regexes
    the whole serial log so far must match (what the kernel logged at boot).
    @builtin: a Terminal command, not a program (no exit code; the output
    decides).  @settle: seconds to wait afterwards (NovaOS saves drive C:
    once it has been quiet for a second).  @before: function(nova) run
    before the command is typed (e.g. plug a device in).  @quotes_panic: the
    output may quote a kernel panic's log (a crash report shown), which is
    then not taken for a panic of this boot."""
    def __init__(self, name, cmd, expect=(), timeout=180, check=None, store=None, shot=None, crash=False, reboot=False,
                 acts=(), boot_expect=(), builtin=False, settle=0, before=None, restart=False,
                 quotes_panic=False):
        self.name, self.cmd, self.expect, self.timeout, self.check = name, cmd, expect, timeout, check
        self.store, self.shot, self.crash, self.reboot = store, shot, crash, reboot
        self.acts, self.boot_expect, self.builtin, self.settle = acts, boot_expect, builtin, settle
        self.before, self.restart, self.quotes_panic = before, restart, quotes_panic


def tones(*hz, wav=None, only=False):
    """A check on the sound recording: a tone near each of @hz, in order.
    @wav: instead of the sound card's recording, a WAV file a QEMU audiodev
    of the boot wrote in its work directory (copied to --out).  @only:
    nothing else sounds in it, not even briefly (a speaker that stopped
    playing must go quiet, not repeat what its ring last held)"""
    def check(nova):                       # (run after QEMU quit: the WAV is complete)
        path = nova.wav
        if wav:
            path = os.path.join(nova.work, wav)
            if not os.path.exists(path):
                return f'QEMU wrote no {wav}'
            shutil.copy(path, OUT)
        segs = wavcheck.segments(path)
        heard = [s[3] for s in segs if s[1] >= 300]
        want = list(hz)
        for h in heard:
            if want and abs(h - want[0]) < want[0] * 0.05:
                want.pop(0)
        if want:
            return f'no {want[0]} Hz tone in the recording (heard: ' + \
                ', '.join(f'{h:.0f} Hz' for h in heard) + ')'
        other = [s for s in segs if s[1] < 300 or not any(abs(s[3] - h) < h * 0.05 for h in hz)]
        if only and other:
            return f'{len(other)} other sound(s) in the recording besides the tones (first: ' + \
                f'{other[0][1]:.0f} ms at {other[0][0]:.2f} s, ~{other[0][3]:.0f} Hz)'
        return None
    return check


def close_lid(nova):
    """powertest asked: close the lid (pc-testdev port 0xE8, see
    tests/acpi/lid-thermal.asl), and once NovaOS has gone to sleep open it,
    press a key on the USB keyboard and wake the machine.  (QEMU 8.2 has no
    USB remote-wakeup path to the platform: the key reaches the suspended
    xHCI port, and system_wakeup stands in for its PME#.)"""
    nova.hmp('o /b 0xe8 1')
    for _ in range(240):
        if nova.status() == 'suspended':
            break
        time.sleep(0.25)
    time.sleep(2)
    nova.hmp('o /b 0xe8 0')
    nova.qmp.key('shift')                     # (the USB keyboard has QEMU's input; types nothing)
    time.sleep(1)
    nova.qmp.cmd('system_wakeup')
    # The tests after this one type on that keyboard: wait until NovaOS has
    # found it again after the wake (read from the file: run() owns the stream)
    # (NovaOS logs the keyboard coming back before or after "Woke up": the
    # xHCI port is re-enumerated while it resumes, so either order is fine)
    for _ in range(240):
        log = open(nova.serial_path, 'rb').read().decode('latin-1')
        log = log[log.rfind('Lid closed: sleeping'):]
        if 'Woke up' in log and re.search(r'keyboard removed[\s\S]*\[USB\] port \d+: keyboard \(report protocol', log):
            break
        time.sleep(0.25)


def set_temp(c):
    """powertest asked: the thermal zone reads @c degrees (pc-testdev port 0xE9)"""
    return lambda nova: nova.hmp(f'o /b 0xe9 {c}')


REC_HZ = 523          # what the core boot's microphone hears
OUT = 'selftest-out'  # --out


def recording(guest, hz, ms, gapless=False):
    """A check on a WAV a test recorded at @guest (C:\\...): a tone of @hz
    for @ms or longer (tools/wavcheck.py), and with @gapless one that never
    skips (no frames lost; wavcheck.gaps).  The file is copied to --out."""
    def check(nova):
        local = os.path.join(OUT, guest.replace('\\', '/').split('/')[-1])
        src = '::/NOVA/C/' + guest[3:].replace('\\', '/')
        r = subprocess.run(['mcopy', '-o', '-i', os.path.join(nova.work, 'data.img'), src, local], capture_output=True)
        if r.returncode:
            return f'no {guest} on the data disk'
        skips = wavcheck.gaps(local, hz) if gapless else []
        if skips:
            return f'{guest} skips {len(skips)} time(s): frames went missing at ' + \
                ', '.join(f'{t:.3f} s' for t, _ in skips[:5])
        if wavcheck.has_tone(local, hz, ms):
            return None
        return f'no {hz} Hz tone of {ms} ms in {guest} (heard: ' + \
            ', '.join(f'{s[3]:.0f} Hz for {s[1]:.0f} ms' for s in wavcheck.segments(local)) + ')'
    check.needs_mic = True
    return check


def load_suite(name):
    """The tests in tests/selftest/NAME/*.py, in file-name order.  Each file
    defines TESTS (a list of Test; Test and tones are given to it) and, for
    the core suite, DOC: its entry in README's list (tools/docgen.py).  One
    file per test, so changes adding tests add files instead of editing a
    shared list; the number prefix places a test in the run."""
    import glob
    tests = []
    for f in sorted(glob.glob(os.path.join(ROOT, 'tests', 'selftest', name, '*.py'))):
        ns = {'Test': Test, 'tones': tones, 'recording': recording, 'REC_HZ': REC_HZ, '__file__': f,
              'close_lid': close_lid, 'set_temp': set_temp}
        exec(compile(open(f).read(), f, 'exec'), ns)
        if not isinstance(ns.get('TESTS'), list) or not all(isinstance(t, Test) for t in ns['TESTS']):
            sys.exit(f'{f}: TESTS must be a list of Test')
        tests += ns['TESTS']
    names = [t.name for t in tests]
    dup = sorted({n for n in names if names.count(n) > 1})
    if dup:
        sys.exit(f'tests/selftest/{name}: more than one test named {", ".join(dup)}')
    return tests


# The core boot has an unplugged AC adapter and a battery
# (tests/acpi/battery.asl), a lid and a thermal zone (tests/acpi/lid-thermal.asl,
# with pc-testdev as its embedded controller), a USB keyboard on an xHCI
# controller at 00:05.0 and an Intel HD Audio card recorded to a WAV.
CORE = load_suite('core')
# The graphics boot: 7-Zip and the Mesa, DXVK and Venus downloads on drive
# C:, a 3D virtio-gpu with Venus as the first monitor (Vulkan on this
# machine's GPU: QEMU 9.2 or newer with virglrenderer built with Venus, and
# an OpenGL display; see venus_gpu) and a QEMU secondary-vga as the second
GRAPHICS = load_suite('graphics')
VENUS_GPU = 'virtio-vga-gl,venus=on,blob=on,hostmem=1G'


def venus_gpu():
    """The graphics boot's first monitor: the 3D virtio-gpu when this QEMU has
    Venus (its virtio-vga-gl has a "venus" property), otherwise the standard
    VGA, and the tests that need Venus fail"""
    try:
        props = subprocess.run([qemu_binary(), '-device', 'virtio-vga-gl,help'], capture_output=True,
                               text=True, timeout=30).stdout
    except (OSError, subprocess.TimeoutExpired):
        props = ''
    if re.search(r'^\s*venus=', props, re.M):
        return ('-vga', 'none', '-device', VENUS_GPU)
    print(f'{qemu_binary()} has no virtio-vga-gl with Venus: the first monitor is a standard VGA, '
          'and the Venus tests will fail', flush=True)
    return ('-vga', 'std')
# The network suite's two boots, each with a virtio-net adapter.  IPv4 and
# HTTP/2: QEMU's user-mode network, where 10.0.2.2 is this machine and
# tools/h2server.js serves HTTPS (HTTP/2 by ALPN, self-signed) on 18443 and
# HTTP/1.1 on 18080.  IPv6: an IPv6-only network that is tools/v6peer.py (a
# router with SLAAC and RDNSS, DNS for nova6.test, HTTP on port 80), over a
# QEMU datagram netdev.
NET4 = load_suite('network4')
NET6 = load_suite('network6')


def renamed(t, suffix):
    t = copy.copy(t)
    t.name += suffix
    return t


# The e1000e boot: its own tests, then the IPv4 ones (but ipconfig and ping)
NET_E1000E = load_suite('network-e1000e') + [renamed(t, ' (e1000e)') for t in NET4
                                             if t.name not in ('virtio-net', 'ping')]
# The devices suite: a boot for each device the core boot doesn't have
# (one that takes QEMU's input, like a touch screen, would take it from the
# core boot's mouse)
TOUCH = load_suite('devices/touch')
USBAUDIO = load_suite('devices/usbaudio')
USBHEADSET = load_suite('devices/usbheadset')
MONITORS = load_suite('devices/monitors')
USBBOOT = load_suite('devices/usbboot')
CDBOOT = load_suite('devices/cdboot')
LAPTOP = load_suite('devices/laptop')
UPDATE = load_suite('devices/update')
GAMEPAD = load_suite('devices/gamepad')


def net4_boot(work):
    """Start tools/h2server.js with a throwaway certificate; QEMU's arguments and the server"""
    cert, key = os.path.join(work, 'h2cert.pem'), os.path.join(work, 'h2key.pem')
    subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', key, '-out', cert,
                    '-days', '2', '-subj', '/CN=novaos-test'], check=True, capture_output=True)
    srv = subprocess.Popen(['node', os.path.join(ROOT, 'tools', 'h2server.js'), cert, key, '18443', '18080'],
                           stdout=open(os.path.join(work, 'h2server.log'), 'w'), stderr=subprocess.STDOUT)
    return ['-nic', 'user,model=virtio-net-pci'], [srv]


def net6_boot(work):
    """Start tools/v6peer.py; QEMU's arguments and the peer"""
    peer = subprocess.Popen([sys.executable, os.path.join(ROOT, 'tools', 'v6peer.py'), '--port', '10600'],
                            stdout=open(os.path.join(work, 'v6peer.log'), 'w'), stderr=subprocess.STDOUT)
    return ['-netdev', 'dgram,id=v6,local.type=inet,local.host=127.0.0.1,local.port=10601,'
                       'remote.type=inet,remote.host=127.0.0.1,remote.port=10600',
            '-device', 'virtio-net-pci,netdev=v6'], [peer]


def e1000e_boot(work):
    """tools/h2server.js as for the IPv4 boot, and an Intel e1000e (82574L)
    on QEMU's user-mode network whose link the tests pull (nic0)"""
    _, procs = net4_boot(work)
    return ['-netdev', 'user,id=net0', '-device', 'e1000e,netdev=net0,id=nic0'], procs


def touch_boot(work):
    """A virtio multi-touch screen (QEMU's input-send-event "mtt" events) and
    a virtio tablet (an absolute pointer)"""
    return ['-device', 'virtio-multitouch-pci', '-device', 'virtio-tablet-pci'], []


def usbaudio_boot(work):
    """No HD Audio card: QEMU usb-audio speakers, each recorded to its own
    WAV (usbN.wav in the work directory).  Speaker 1 is on an xHCI
    controller at boot; the tests plug 2 and 3 into an OHCI and a UHCI
    controller (tests/selftest/devices/usbaudio)"""
    args = []
    for n in (1, 2, 3):
        args += ['-audiodev', f'wav,id=usbsnd{n},path={os.path.join(work, f"usb{n}.wav")},out.frequency=48000']
    return args + ['-device', 'qemu-xhci,id=xhci', '-device', 'usb-audio,id=spk1,bus=xhci.0,audiodev=usbsnd1',
                   '-device', 'pci-ohci,id=ohci', '-device', 'piix3-usb-uhci,id=uhci'], []


def peer(work, port, *args):
    """Start tools/usbredirpeer.py on @port and wait until it listens"""
    log = os.path.join(work, f'usbredirpeer-{port}.log')
    p = subprocess.Popen([sys.executable, os.path.join(ROOT, 'tools', 'usbredirpeer.py'), '--port', str(port), *args],
                         stdout=open(log, 'w'), stderr=subprocess.STDOUT)
    for _ in range(100):
        if 'listening' in open(log).read() or p.poll() is not None:
            break
        time.sleep(0.05)
    return p


def usbheadset_boot(work):
    """No HD Audio card (an AC'97 card instead, which the HD Audio driver
    must not take): a high-speed USB headset (tools/usbredirpeer.py:
    its speaker writes headset.wav in the work directory, its microphone
    hears REC_HZ) on an EHCI controller, and an xHCI, an OHCI and a UHCI
    controller for the full-speed microphones the tests plug in (ports
    10701-10703), and a high-speed USB Audio 2.0 headset the tests plug
    into the xHCI controller (port 10704: its speaker writes uac2.wav, its
    microphone hears 988 Hz), then a 44.1 kHz USB Audio 2.0 surround
    headset (port 10705: six speaker channels whose front two surround.wav
    gets, four microphone channels hearing 1175 Hz) and a full-speed USB
    Audio 1.0 speaker (port 10706, spk.wav) for the device picker, and two
    speakers on their own clocks that say so through a feedback endpoint:
    a full-speed USB Audio 1.0 one at 48,500 frames a second (port 10707,
    async1.wav) and a high-speed USB Audio 2.0 one at 47,600 (port 10708,
    async2.wav) (tests/selftest/devices/usbheadset)"""
    procs = [peer(work, 10700, '--speaker', os.path.join(work, 'headset.wav'), '--mic', str(REC_HZ))]
    for n, hz in ((1, 784), (2, 659), (3, 880)):
        procs.append(peer(work, 10700 + n, '--speed', 'full', '--mic', str(hz)))
    procs.append(peer(work, 10704, '--uac2', '--speaker', os.path.join(work, 'uac2.wav'), '--mic', '988'))
    procs.append(peer(work, 10707, '--speed', 'full', '--feedback', '48500', '--product', 'Test Async Speaker',
                      '--speaker', os.path.join(work, 'async1.wav')))
    procs.append(peer(work, 10708, '--uac2', '--feedback', '47600', '--product', 'Test Async Headset',
                      '--speaker', os.path.join(work, 'async2.wav')))
    procs.append(peer(work, 10705, '--uac2', '--rates', '44100', '--channels', '6', '--mic-channels', '4',
                      '--product', 'Test Surround Headset', '--speaker', os.path.join(work, 'surround.wav'), '--mic', '1175'))
    procs.append(peer(work, 10706, '--speed', 'full', '--product', 'Test Speaker', '--speaker', os.path.join(work, 'spk.wav')))
    return ['-chardev', 'socket,id=headset,host=127.0.0.1,port=10700', '-device', 'usb-ehci,id=ehci',
            '-device', 'usb-redir,id=headset,chardev=headset,bus=ehci.0', '-device', 'qemu-xhci,id=xhci',
            '-device', 'pci-ohci,id=ohci', '-device', 'piix3-usb-uhci,id=uhci',
            '-audiodev', 'none,id=ac97snd', '-device', 'AC97,audiodev=ac97snd'], procs


def gamepad_boot(work):
    """Three USB game controllers on an xHCI controller, each
    tools/padpeer.py behind a QEMU usb-redir device: a wired Xbox 360
    controller (port 10710), an Xbox One controller (10711) and a HID game
    pad (10712); the tests set their buttons and sticks through each
    peer's control port (the port + 100) (tests/selftest/devices/gamepad)"""
    args, procs = ['-device', 'qemu-xhci,id=xhci'], []
    for port, kind, dev in ((10710, 'xbox360', 'pad360'), (10711, 'xboxone', 'padone'), (10712, 'hid', 'padhid')):
        log = os.path.join(work, f'padpeer-{port}.log')
        p = subprocess.Popen([sys.executable, '-u', os.path.join(ROOT, 'tools', 'padpeer.py'), '--port', str(port),
                              '--kind', kind], stdout=open(log, 'w'), stderr=subprocess.STDOUT)
        for _ in range(100):
            if 'listening' in open(log).read() or p.poll() is not None:
                break
            time.sleep(0.05)
        procs.append(p)
        args += ['-chardev', f'socket,id={dev},host=127.0.0.1,port={port}',
                 '-device', f'usb-redir,id={dev},chardev={dev},bus=xhci.0']
    return args, procs


def monitors_boot(work):
    """One card with three outputs: a virtio-vga (the boot display, on its
    first output) and a VNC server on each other output (work/vnc1.sock,
    vnc2.sock), through which the test connects and disconnects a monitor
    there (tests/selftest/devices/monitors)"""
    return ['-vga', 'none', '-device', 'virtio-vga,max_outputs=3,id=gpu'] + \
        [x for n in (1, 2) for x in ('-vnc', f'unix:{os.path.join(work, f"vnc{n}.sock")},id=vnc{n},display=gpu,head={n}')], []


def iso_path(work):
    """build/nova.iso, or one made from the build in @work"""
    iso = os.path.join(ROOT, 'build', 'nova.iso')
    if not os.path.exists(iso):
        iso = os.path.join(work, 'nova.iso')
        subprocess.run([os.path.join(ROOT, 'scripts', 'create-iso.sh'), iso,
                        os.path.join(ROOT, 'build', 'bootx64.efi'), os.path.join(ROOT, 'build', 'kernel.elf')],
                       check=True, stdout=subprocess.DEVNULL)
    return iso


def cdboot_boot(work):
    """No boot disk: nova.iso in a SATA DVD drive (tests/selftest/devices/cdboot)"""
    return ['-cdrom', iso_path(work)], [], {'img': False}


def usbboot_boot(work):
    """No boot disk: nova.iso written to a 2 GiB USB stick (work/stick.img)
    on an xHCI controller, and no display adapter NovaOS has a driver for
    (QEMU's ramfb, which only the firmware's GOP drives), as on a laptop
    with integrated graphics.  build/nova.iso, or one made from the build
    (tests/selftest/devices/usbboot)"""
    stick = os.path.join(work, 'stick.img')
    shutil.copy(iso_path(work), stick)
    with open(stick, 'r+b') as f:
        f.truncate(2 << 30)
    return ['-device', 'qemu-xhci,id=xhci', '-drive', f'if=none,id=stick,format=raw,file={stick}',
            '-device', 'usb-storage,bus=xhci.0,drive=stick,bootindex=0'], [], {'img': False, 'vga': ('-vga', 'none', '-device', 'ramfb')}


def laptop_boot(work):
    """A Modern Standby laptop (tests/selftest/devices/laptop): QEMU without
    \\_S3 or an HPET, and with tests/acpi/laptop.asl (its embedded controller, lid,
    battery and LPS0 device) and pc-testdev for the lid; nova.iso on a USB
    stick (build/nova.iso, or one made from the build) and an empty 2 GiB
    NVMe disk that is the first boot device (OVMF passes over it until
    NovaOS is installed there); the firmware's GOP for a display"""
    aml = os.path.join(work, 'laptop.aml')
    subprocess.run(['iasl', '-p', aml[:-4], os.path.join(ROOT, 'tests', 'acpi', 'laptop.asl')],
                   check=True, stdout=subprocess.DEVNULL)
    stick, nvme = os.path.join(work, 'stick.img'), os.path.join(work, 'nvme.img')
    shutil.copy(iso_path(work), stick)
    with open(stick, 'r+b') as f:
        f.truncate(2 << 30)
    with open(nvme, 'wb') as f:
        f.truncate(2 << 30)
    return ['-machine', 'hpet=off', '-global', 'ICH9-LPC.disable_s3=1', '-acpitable', f'file={aml}', '-device', 'pc-testdev',
            '-drive', f'if=none,id=nvm,format=raw,file={nvme}', '-device', 'nvme,drive=nvm,serial=nova0,bootindex=0',
            '-device', 'qemu-xhci,id=xhci', '-drive', f'if=none,id=stick,format=raw,file={stick}',
            '-device', 'usb-storage,bus=xhci.0,drive=stick,bootindex=1'], [], \
        {'img': False, 'vga': ('-vga', 'none', '-device', 'ramfb')}


def update_boot(work):
    """build/nova.img as an installed NovaOS (its writes kept while QEMU runs,
    across restarts) on QEMU's user-mode network, where 10.0.2.2:18090
    serves update channels made with tools/mkupdate.py from this build:
    v1/ stamped one version newer, v2/ two, same/ this version and dev/ a
    development build of it, all signed with the self-tests' key, whose
    public half QEMU hands to NovaOS (fw_cfg); and v1's channel unsigned/,
    signed with another key (otherkey/) and changed after it was signed
    (changed/) (tests/selftest/devices/update)"""
    sys.path.insert(0, os.path.join(ROOT, 'tools'))
    import ed25519
    upd = os.path.join(ROOT, 'tests', 'selftest', 'devices', 'update')
    test, key = os.path.join(upd, '010-update.py'), os.path.join(upd, 'TEST-ONLY-signing-key.txt')
    ns = {'Test': Test, '__file__': test}
    exec(compile(open(test).read(), test, 'exec'), ns)          # (its V1 and V2)
    root = os.path.join(work, 'channels')
    other = os.path.join(work, 'other-signing-key.txt')
    if not os.path.exists(other):
        with open(other, 'w') as f:
            f.write(ed25519.new_secret().hex() + '\n')
    for sub, ver, sign in (('v1', ns['V1'], key), ('v2', ns['V2'], key), ('unsigned', ns['V1'], None),
                           ('otherkey', ns['V1'], other), ('changed', ns['V1'], key),
                           ('same', ns['VER'], key), ('dev', ns['DEV'], key)):
        subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'mkupdate.py'), os.path.join(root, sub),
                        '--version', ver, '--notes', f'Self-test build {ver}'] + (['--sign', sign] if sign else []),
                       check=True, stdout=subprocess.DEVNULL)
    changed = os.path.join(root, 'changed', 'novaos-update.txt')
    data = open(changed, 'rb').read()
    open(changed, 'wb').write(data.replace(b'notes Self-test build', b'notes Self-test build, changed'))
    public = ed25519.public_key(ed25519.parse_secret(open(key).read())).hex()
    srv = subprocess.Popen([sys.executable, '-m', 'http.server', '18090', '--bind', '127.0.0.1', '--directory', root],
                           stdout=open(os.path.join(work, 'http.log'), 'w'), stderr=subprocess.STDOUT)
    time.sleep(1)
    if srv.poll() is not None:            # (another server on the port would serve other files)
        raise RuntimeError('the update channels\' web server did not start (is port 18090 in use?)')
    return ['-nic', 'user,model=virtio-net-pci', '-fw_cfg', f'name=opt/novaos/update-key,string={public}'], [srv]


# The suites that boot once per entry: (label, tests, setup(work) -> (QEMU arguments, processes[,
# more Nova arguments]))
BOOTS = {
    'network': [('ipv4', NET4, net4_boot), ('ipv6', NET6, net6_boot)],
    'devices': [('touch', TOUCH, touch_boot), ('usbaudio', USBAUDIO, usbaudio_boot),
                ('usbheadset', USBHEADSET, usbheadset_boot),
                ('monitors', MONITORS, monitors_boot), ('usbboot', USBBOOT, usbboot_boot),
                ('cdboot', CDBOOT, cdboot_boot), ('laptop', LAPTOP, laptop_boot), ('update', UPDATE, update_boot),
                ('gamepad', GAMEPAD, gamepad_boot)],
}


def store_verdict(nova, t, out):
    """Wait for the App Store's outcome line for @t; (why it failed or None, the log)"""
    pat = re.compile(r'\[STORE\] ' + re.escape(t.store) + r': ([^\n]*)')
    end = time.time() + t.timeout
    while not pat.search(out) and time.time() < end and nova.q.poll() is None:
        time.sleep(1)
        out += nova.sr.read_new()
    m = pat.search(out)
    if not m:
        return f'no outcome from the App Store in {t.timeout} s', out
    return (None if m.group(1).startswith('Installed') else m.group(1)), out


def verdict(t, out, ok, exe):
    """Why @t failed, or None"""
    if not ok:
        return f'did not finish in {t.timeout} s'
    if t.builtin:
        bad = re.search(r'^(FAIL[^\n]*)', out, re.M)
        if bad:
            return bad.group(1)
        for e in t.expect:
            if not re.search(e, out):
                return f'missing "{e}"'
        return None
    m = re.search(r'\[UM\] Started ' + re.escape(exe) + r' \(PID (\d+)\)', out)
    if not m:
        return 'did not start'
    pid = m.group(1)
    crash = re.search(r'\[UM\] ' + re.escape(exe) + r' \(PID ' + pid + r'\) (crashed[^\n]*|terminated[^\n]*)', out)
    if crash:
        return crash.group(1)
    code = re.search(r'\[UM\] ' + re.escape(exe) + r' \(PID ' + pid + r'\) exited with code (\d+)', out)
    if not code:
        return 'no exit code'
    if code.group(1) != '0':
        return f'exit code {code.group(1)}'
    bad = re.search(r'^(FAIL[^\n]*)', out, re.M) or re.search(r'\b([1-9]\d* failed)', out)
    if bad:
        return bad.group(1)
    # the kernel's log shares the serial port with the Terminal's copy and
    # can land in the middle of a line of output: match without it
    # (a program's line can also land in the middle of a kernel log line,
    # such as "[UM]   command line: ..." of a 1,100-character command, and
    # then goes with it: look in the whole output if it is not in the rest)
    text = KLOG.sub('', out)
    for e in t.expect:
        if not re.search(e, text) and not re.search(e, out):
            return f'missing "{e}"'
    return None


KLOG = re.compile(r'\[(?:UM|SCHED)\] [^\n]*\n')
PANIC = re.compile(r'KERNEL PANIC|KERNEL PAGE FAULT|DOUBLE FAULT|Unhandled kernel exception')
# The network suite's third boot, on the NIC family of PCs' built-in Ethernet
BOOTS['network'].append(('e1000e', NET_E1000E, e1000e_boot))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--suite', choices=('core', 'graphics') + tuple(BOOTS), default='core')
    ap.add_argument('--gfx', help='the graphics suite\'s files (tools/ci/stage-graphics.sh)')
    ap.add_argument('--img', default=os.path.join(ROOT, 'build', 'nova.img'))
    ap.add_argument('--out', default='selftest-out')
    ap.add_argument('--only')
    ap.add_argument('--junit')
    ap.add_argument('--summary')
    ap.add_argument('--list', action='store_true', help='print the suite\'s tests and exit')
    a = ap.parse_args()

    suite = CORE if a.suite == 'core' else GRAPHICS if a.suite == 'graphics' else \
        [t for _, tests, _ in BOOTS[a.suite] for t in tests]
    if a.list:
        for t in suite:
            print(f'{t.name:20s} {t.cmd}')
        return 0

    def chosen(suite):
        return [t for t in suite if not a.only or t.name in a.only.split(',') or t.cmd.split()[0] in a.only.split(',')]
    os.makedirs(a.out, exist_ok=True)
    global OUT
    OUT = a.out
    if a.suite in BOOTS:
        results = []
        for name, suite, setup in BOOTS[a.suite]:
            tests = chosen(suite)
            if not tests:
                continue
            work = tempfile.mkdtemp(prefix='selftest')
            procs = []
            try:
                args, procs, *more = setup(work)
                results += run_boot(a, tests, work, name, extra_args=args, data_mb=64, **(more[0] if more else {}))
            finally:
                for p in procs:
                    p.kill()
                for log in ['h2server.log', 'v6peer.log', 'http.log'] + [f'usbredirpeer-{p}.log' for p in range(10700, 10709)] \
                        + [f'padpeer-{p}.log' for p in range(10710, 10713)]:
                    if os.path.exists(os.path.join(work, log)):
                        shutil.copy(os.path.join(work, log), a.out)
                shutil.rmtree(work, ignore_errors=True)
        report(a, results)
        return sum(1 for r in results if r[1])

    tests = chosen(suite)
    work = tempfile.mkdtemp(prefix='selftest')
    tables = []
    for asl in ('battery', 'lid-thermal', 'i2c-touchpad'):
        aml = os.path.join(work, asl + '.aml')
        subprocess.run(['iasl', '-p', aml[:-4], os.path.join(ROOT, 'tests', 'acpi', asl + '.asl')],
                       check=True, stdout=subprocess.DEVNULL)
        tables += ['-acpitable', f'file={aml}']
    wav = os.path.join(a.out, 'sound.wav') if a.suite == 'core' else None
    rec, results = None, []
    if a.suite == 'core' and not shutil.which('pulseaudio'):
        print('PulseAudio is not installed: the tests that record cannot run', flush=True)
        results = [(t.name, 'needs PulseAudio on the host (novarun --rec)', 0, '') for t in tests
                   if getattr(t.check, 'needs_mic', False)]
        tests = [t for t in tests if not getattr(t.check, 'needs_mic', False)]
    elif a.suite == 'core':                                 # the microphone's tone
        import math, struct, wave
        rec = os.path.join(work, 'mic.wav')
        with wave.open(rec, 'wb') as w:
            w.setnchannels(2)
            w.setsampwidth(2)
            w.setframerate(48000)
            w.writeframes(b''.join(struct.pack('<hh', v, v) for v in
                                   (int(12000 * math.sin(2 * math.pi * REC_HZ * i / 48000)) for i in range(48000 * 10))))
    puts, data_mb = [], 64
    if a.suite == 'graphics':
        if not a.gfx:
            sys.exit('the graphics suite needs --gfx DIR (tools/ci/stage-graphics.sh DIR)')
        puts = [(os.path.join(a.gfx, '7zip'), r'C:\Programs\7-Zip'), (os.path.join(a.gfx, 'downloads'), r'C:\Downloads'),
                (os.path.join(a.gfx, 'tests'), r'C:\Tests')]
        data_mb = 1024
    try:
        # the graphics boot has a second monitor (montest): a QEMU secondary-vga
        heads = ['-device', 'secondary-vga,id=head2'] if a.suite == 'graphics' else []
        boot_args = dict(puts=puts, mem=4096 if a.suite == 'graphics' else 2048, data_mb=data_mb, rec=rec,
                         vga=venus_gpu() if a.suite == 'graphics' else ('-vga', 'std'),
                         extra_args=tables + ['-device', 'pc-testdev', '-device', 'qemu-xhci,id=xhci,addr=0x5',
                                              '-device', 'usb-kbd,id=usbkbd,bus=xhci.0'] + heads)
        results += run_boot(a, tests, work, None, wav=wav, **boot_args)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    report(a, results)
    return sum(1 for r in results if r[1])


def run_boot(a, tests, work, label, **nova_args):
    """Boot NovaOS once and run @tests; [(name, why it failed or None, seconds, output)].
    @label names the boot's serial log (serial-LABEL.log) when a suite has several"""
    results = []
    log_name = f'serial-{label}.log' if label else 'serial.log'
    raw_log = os.path.join(a.out, log_name.replace('.log', '-raw.log'))
    t_boot = time.time()
    try:
        nova = Nova(nova_args.pop('img', a.img), work, serial_path=raw_log, **nova_args)
    except RuntimeError as e:
        print(e)
        if os.path.exists(raw_log):
            shutil.copy(raw_log, os.path.join(a.out, log_name))
        return [('boot' + (f' ({label})' if label else ''), str(e).splitlines()[0], 0, '')]
    print(f'booted{" (" + label + ")" if label else ""} in {time.time() - t_boot:.0f} s', flush=True)
    full_log = nova.boot_log
    deferred = []
    try:
        for t in tests:
            t0 = time.time()
            slug = re.sub(r'\W+', '-', t.name)
            png = os.path.join(a.out, slug + '.png')
            if t.crash:
                nova.sr.read_new()
                nova.qmp.type(t.cmd + '\n')
                out, ok = nova.sr.wait('KERNEL PAGE FAULT', t.timeout)
                out += nova.sr.wait('halting', 5)[0]
                if not out.strip():                 # nothing at all: what the kernel said last
                    out = '(no output; the serial log ends:)\n' + \
                        open(nova.serial_path, 'rb').read().decode('latin-1')[-3000:]
            elif t.reboot:
                try:
                    out, ok = nova.reboot(t.timeout), True
                except RuntimeError as e:
                    out, ok = str(e), False
            else:
                if t.before:
                    t.before(nova)
                    full_log += nova.sr.read_new()          # (what the kernel said meanwhile)
                out, ok = nova.run(t.cmd, t.timeout, shot=(t.shot, png) if t.shot else None, acts=t.acts)
            if t.crash:
                miss = [e for e in t.expect if not re.search(e, out)]
                why = None if ok and not miss else ('no kernel fault' if not ok else 'no symbolized backtrace')
            elif t.reboot:
                miss = [e for e in t.expect if not re.search(e, out)]
                why = 'did not boot again' if not ok else f'missing "{miss[0]}"' if miss else None
            elif t.store:
                why, out = store_verdict(nova, t, out)
            else:
                exe = re.split(r'[\\/]', t.cmd.split()[0])[-1]
                why = verdict(t, out, ok, exe if exe.lower().endswith('.exe') else exe + '.exe')
                whole = open(nova.serial_path, 'rb').read().decode('latin-1') if t.boot_expect else ''
                for e in t.boot_expect:
                    if not why and not re.search(e, whole):
                        why = f'missing "{e}" in the serial log'
            full_log += out
            if PANIC.search(out) and not t.crash and not t.quotes_panic:
                why = 'kernel panic'
            if nova.q.poll() is None and not (t.shot and os.path.exists(png)):
                nova.shot(png)
            if not why and t.check:
                deferred.append((len(results), t))          # checked once QEMU has quit
            results.append((t.name, why, time.time() - t0, out))
            print(f'{"PASS" if not why else "FAIL"}  {t.name:18s} {time.time() - t0:6.1f} s  {why or ""}', flush=True)
            if why:
                print('    ' + '\n    '.join(l for l in out.splitlines() if not l.startswith('[SCHED]'))[-4000:], flush=True)
                try:                                        # (a run cancelled by its job's limit never gets to the end)
                    shutil.copy(nova.serial_path, os.path.join(a.out, log_name))
                except OSError:
                    pass
            if t.settle and not why:
                time.sleep(t.settle)
            if t.crash and t.restart and not why and nova.q.poll() is None:
                try:                                        # reset the halted machine, as a person would
                    nova.qmp.cmd('system_reset')
                    nova.start()
                    full_log += nova.boot_log
                    continue
                except RuntimeError as e:
                    print(f'    did not start again after the crash: {str(e).splitlines()[0]}', flush=True)
            if why == 'kernel panic' or t.crash or nova.q.poll() is not None or nova.wedged:
                break
    finally:
        nova.close()
        with open(os.path.join(a.out, log_name), 'w') as f:
            f.write(full_log + nova.sr.read_new())
        for i, t in deferred:                               # e.g. the sound recording, now complete
            why = t.check(nova)
            if why:
                name, _, secs, out = results[i]
                results[i] = (name, why, secs, out)
                print(f'FAIL  {t.name:18s} {why}', flush=True)
    ran = {r[0] for r in results}
    why_not = ('NovaOS stopped answering during an earlier test' if getattr(nova, 'wedged', False)
               else 'an earlier test stopped NovaOS')
    results += [(t.name, f'not run ({why_not})', 0, '') for t in tests if t.name not in ran]
    return results


def report(a, results):
    failed = sum(1 for r in results if r[1])
    print(f'\n{len(results) - failed} of {len(results)} self-tests passed')
    if a.summary:
        with open(a.summary, 'a') as f:
            f.write(f'### NovaOS {a.suite} self-tests: {len(results) - failed} of {len(results)} passed\n\n')
            f.write('| Test | Result | Time |\n|---|---|---|\n')
            for name, why, secs, _ in results:
                f.write(f'| `{name}` | {"✅ pass" if not why else "❌ " + why.replace("|", "/")} | {secs:.0f} s |\n')
            f.write('\n')
    if a.junit:
        from xml.sax.saxutils import escape, quoteattr
        with open(a.junit, 'w') as f:
            f.write(f'<testsuite name="novaos-selftest" tests="{len(results)}" failures="{failed}">\n')
            for name, why, secs, out in results:
                f.write(f'  <testcase name={quoteattr(name)} time="{secs:.1f}">')
                if why:
                    f.write(f'<failure message={quoteattr(why)}>{escape(out[-8000:])}</failure>')
                f.write('</testcase>\n')
            f.write('</testsuite>\n')


if __name__ == '__main__':
    sys.exit(main())
