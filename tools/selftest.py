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
            long downloads at once (dltest -w), winget against its REST
            source and an App Store download (a catalog from the test
            server, a redirect to a ~1,000-character signed address, a 503
            the Store tries again after, a body trickled over more than 30
            s), all against tools/h2server.js
            (needs node and openssl).  IPv6 on an IPv6-only
            network that is tools/v6peer.py: SLAAC and RDNSS (ipconfig),
            ping -6, curl -6 and Winsock over IPv6 (netcat), stateful
            DHCPv6 (ipconfig /all, dhcp6test, rebind and renew,
            ipconfig /release6 and /renew6).  Then a third
            boot with an Intel e1000e (82574L) instead of virtio-net
            (tests/selftest/network-e1000e): its PHY and link, the link
            pulled and plugged back, and the IPv4 tests again
  devices   a boot per device QEMU has that the core boot hasn't
            (tests/selftest/devices/NAME/): "touch", a virtio multi-touch
            screen (touchtest) and a USB digitizer (tools/padpeer.py
            --kind digitizer: a touch screen, a pen and a precision
            touchpad) whose collections run on the HID class driver's
            hiddigi and mouhid (digitest); "usbaudio", USB speakers on xHCI, OHCI and
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
            reset while it first starts; "driversearch", QEMU's
            virtio-serial port, which has no NovaOS driver, on a network
            answering as the Microsoft Update Catalog does from recorded
            pages (tools/drivertest/catalog_fixture.py): pnputil
            downloads its driver package by hardware ID and stages it;
            "gamepad", a wired Xbox 360, an
            Xbox One and a HID game pad on xHCI (tools/padpeer.py behind
            usb-redir devices): padtest reads their buttons and sticks
            through XInput and DirectInput 8 while the test moves them,
            sets their motors, and one is unplugged; "pnp", an xHCI
            controller into which the test plugs a keyboard, a hub and a
            USB stick and takes them out again: pnptest follows the
            Plug and Play device tree; "wdm", the machine as the core
            boot has it, where sc installs a Windows kernel-mode test
            driver (wdmtest.sys, test-signed), which loads once test
            signing is on and a restart later (unsigned and tampered
            copies never; pnputil refuses a package whose file does not
            match its catalog), and wdmtest talks to it; then a KMDF
            driver (kmdftest.sys) starts on Wdf01000.sys and WdfLdr.sys
            and kmdftest talks to it; "edu", QEMU's edu PCI device, on
            which pnputil installs a Plug and Play driver (edutest.sys)
            that gets its memory, interrupt and DMA from the PnP manager,
            and which pnputil disables and enables; "vioser", QEMU's
            virtio-serial controller, on which pnputil installs
            virtio-win's signed vioser.sys (KMDF): its port, a child
            device, carries bytes to the host and back; "viostor", a
            second disk on QEMU's virtio-blk card with an NTFS volume, on
            which pnputil installs virtio-win's signed viostor.sys: it runs
            on NovaOS's Storport host and the volume mounts as D:;
            "usbserial", QEMU's usb-serial converter (an FTDI FT232) on
            xHCI, which gets FTDI's own driver by hardware ID and becomes
            COM3, which serialtest loops data through; "umdf", the edu
            device again, on which pnputil installs a UMDF 2 driver
            (umdftest.dll) that runs in a driver host (WUDFHost.exe) on
            Microsoft's user-mode framework; "kmdfedu", the edu device
            once more, on which pnputil installs a KMDF driver
            (kmdfedu.sys) that runs on Wdf01000.sys with the device's
            memory, interrupt (line, then MSI) and DMA;
            "netkvm", QEMU's virtio-net card, on which pnputil installs
            virtio-win's signed netkvm.sys in place of the built-in
            driver: it runs on NovaOS's own NDIS host while DHCP, a ping
            and a download go through it; "wddm", the standard VGA, on
            which pnputil installs a WDDM display-only driver
            (wddmdod.sys) that takes the desktop over from the built-in
            driver through NovaOS's dxgkrnl host, changes mode as
            Settings asks and gives the screen back when disabled;
            "hid", a vendor-defined USB HID device on xHCI
            (tools/padpeer.py --kind vendor): the kernel's HID class
            driver runs it on its USB minidriver, and hidclasstest opens
            its collection through hid.dll, writes and reads input,
            output and feature reports and sees it unplugged;
            "devmgr", an e1000e card, a USB keyboard and tablet on xHCI
            and the edu device with no driver: Device Manager
            (devmgmt.exe) is opened from the Start menu, then checks
            setupapi's device list against cfgmgr32's tree and lists each
            device's class, status and resources; "camera", a
            USB Video Class camera on xHCI (tools/campeer.py): the kernel's
            UVC driver runs it, camtest lists it with Media Foundation and
            reads frames in each of its formats through a source reader,
            and the Camera app shows it; "isocam", the same camera as
            most laptops have it (tools/campeer.py --iso): an
            isochronous endpoint in two alternate settings and MJPEG
            frames, which the UVC driver reads through isochronous URBs
            and the source reader decodes; "composite", a composite USB
            device on xHCI (tools/compositepeer.py: a keyboard and two
            vendor-defined HID functions): NovaOS's keyboard driver takes
            one function and the HID class driver each of the others on
            a PDO of its own, and usbcompositetest checks the device
            tree, both HID functions' reports and the unplug; "diskmgr",
            a blank second disk: Disk Management (diskmgmt.exe) is opened
            from the Start menu, then initializes the disk, makes an NTFS
            and a FAT32 volume on it, moves one to drive T: (still T:
            after a restart), and the host checks both volumes;
            "vhidmini", a root-enumerated device (ROOT\\HIDCLASS\\0000) on
            which pnputil installs Microsoft's vhidmini2 sample, a KMDF HID
            minidriver, as a lower filter under mshidkmdf: vhidtest opens
            its collection through hid.dll and moves its reports, Device
            Manager lists it, it starts at boot and pnputil removes it

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
import argparse, copy, os, re, shutil, socket, struct, subprocess, sys, tempfile, time

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
    @cut: a regex; once @cmd's output matches it, the machine is reset
    while the program runs on (a power cut half-way through what it
    writes), and the test passes when the new boot's log shows @expect;
    later tests run in that boot.
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
                 quotes_panic=False, cut=None):
        self.name, self.cmd, self.expect, self.timeout, self.check = name, cmd, expect, timeout, check
        self.store, self.shot, self.crash, self.reboot = store, shot, crash, reboot
        self.acts, self.boot_expect, self.builtin, self.settle = acts, boot_expect, builtin, settle
        self.before, self.restart, self.quotes_panic, self.cut = before, restart, quotes_panic, cut


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
    tests/acpi/lid-thermal.asl) and raise its GPE (GPE 0 of the GPE block
    device at port 0xEA), and once NovaOS has gone to sleep open it, raise
    the lid's wake GPE (GPE 1 of the block), press a key on the USB keyboard
    and wake the machine.  (QEMU 8.2 has no USB remote-wakeup path to the
    platform: the key reaches the suspended xHCI port, and system_wakeup
    stands in for its PME# and the GPE's wake signal.)"""
    nova.hmp('o /b 0xe8 1')
    nova.hmp('o /b 0xea 1')
    for _ in range(240):
        if nova.status() == 'suspended':
            break
        time.sleep(0.25)
    time.sleep(2)
    nova.hmp('o /b 0xe8 0')
    nova.hmp('o /b 0xea 2')
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
        if 'Woke up' in log and re.search(r'\[KBDHID\] 0627:0001: removed[\s\S]*\[KBDHID\] 0627:0001: keyboard', log):
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
# router with SLAAC and RDNSS, a stateful DHCPv6 server, DNS for nova6.test,
# HTTP on port 80), over a QEMU datagram netdev.
NET4 = load_suite('network4')
NET6 = load_suite('network6')


def renamed(t, suffix):
    t = copy.copy(t)
    t.name += suffix
    return t


# The e1000e boot: its own tests, then the IPv4 ones (but ipconfig and ping)
NET_E1000E = load_suite('network-e1000e') + [renamed(t, ' (e1000e)') for t in NET4
                                             if t.name not in ('virtio-net', 'ping') and not t.name.startswith(('winget', 'store'))]
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
DRIVERSEARCH = load_suite('devices/driversearch')
GAMEPAD = load_suite('devices/gamepad')
HID = load_suite('devices/hid')
CAMERA = load_suite('devices/camera')
ISOCAM = load_suite('devices/isocam')
COMPOSITE = load_suite('devices/composite')
PNP = load_suite('devices/pnp')
NTFS = load_suite('devices/ntfs')
WDM = load_suite('devices/wdm')
EDU = load_suite('devices/edu')
USBSERIAL = load_suite('devices/usbserial')
AX211 = load_suite('devices/ax211')
WIFI = load_suite('devices/wifi')
VIOSER = load_suite('devices/vioser')
VIOSTOR = load_suite('devices/viostor')
UMDF = load_suite('devices/umdf')
KMDFEDU = load_suite('devices/kmdfedu')
WDDM = load_suite('devices/wddm')
DEVMGR = load_suite('devices/devmgr')
NETKVM = load_suite('devices/netkvm')
DISKMGR = load_suite('devices/diskmgr')
VHIDMINI = load_suite('devices/vhidmini')


def net4_boot(work):
    """Start tools/h2server.js with a throwaway certificate; QEMU's arguments and the server"""
    cert, key = os.path.join(work, 'h2cert.pem'), os.path.join(work, 'h2key.pem')
    subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', key, '-out', cert,
                    '-days', '2', '-subj', '/CN=novaos-test', '-addext', 'subjectAltName=IP:10.0.2.2'],
                   check=True, capture_output=True)
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
    """A virtio multi-touch screen (QEMU's input-send-event "mtt" events), a
    virtio tablet (an absolute pointer) and a USB digitizer on an xHCI
    controller: tools/padpeer.py --kind digitizer behind a QEMU usb-redir
    device (port 10717; a touch screen, a pen and a precision touchpad)"""
    log = os.path.join(work, 'padpeer-10717.log')
    p = subprocess.Popen([sys.executable, '-u', os.path.join(ROOT, 'tools', 'padpeer.py'), '--port', '10717',
                          '--kind', 'digitizer'], stdout=open(log, 'w'), stderr=subprocess.STDOUT)
    for _ in range(100):
        if 'listening' in open(log).read() or p.poll() is not None:
            break
        time.sleep(0.05)
    return ['-device', 'virtio-multitouch-pci', '-device', 'virtio-tablet-pci', '-device', 'qemu-xhci,id=xhci',
            '-chardev', 'socket,id=digi,host=127.0.0.1,port=10717',
            '-device', 'usb-redir,id=digi,chardev=digi,bus=xhci.0'], [p]


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
    p = subprocess.Popen([sys.executable, os.path.join(ROOT, 'tools', 'usbredirpeer.py'), '--port', str(port), '--log-times', *args],
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
    async2.wav), and a full-speed speaker (port 10709, slow.wav) that
    takes three seconds to answer its first request
    (tests/selftest/devices/usbheadset)"""
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
    procs.append(peer(work, 10709, '--speed', 'full', '--slow-control', '3000', '--product', 'Test Slow Speaker',
                      '--speaker', os.path.join(work, 'slow.wav')))
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


def hid_boot(work):
    """A vendor-defined USB HID device on an xHCI controller,
    tools/padpeer.py --kind vendor behind a QEMU usb-redir device (port
    10713), which echoes output reports as input reports
    (tests/selftest/devices/hid)"""
    log = os.path.join(work, 'padpeer-10713.log')
    p = subprocess.Popen([sys.executable, '-u', os.path.join(ROOT, 'tools', 'padpeer.py'), '--port', '10713',
                          '--kind', 'vendor'], stdout=open(log, 'w'), stderr=subprocess.STDOUT)
    for _ in range(100):
        if 'listening' in open(log).read() or p.poll() is not None:
            break
        time.sleep(0.05)
    return ['-device', 'qemu-xhci,id=xhci', '-chardev', 'socket,id=hiddev,host=127.0.0.1,port=10713',
            '-device', 'usb-redir,id=hiddev,chardev=hiddev,bus=xhci.0'], [p]


def camera_boot(work):
    """A USB Video Class camera on an xHCI controller, tools/campeer.py
    behind a QEMU usb-redir device (port 10714), streaming a test card
    (tests/selftest/devices/camera)"""
    log = os.path.join(work, 'campeer.log')
    p = subprocess.Popen([sys.executable, '-u', os.path.join(ROOT, 'tools', 'campeer.py'), '--port', '10714'],
                         stdout=open(log, 'w'), stderr=subprocess.STDOUT)
    for _ in range(100):
        if 'listening' in open(log).read() or p.poll() is not None:
            break
        time.sleep(0.05)
    return ['-device', 'qemu-xhci,id=xhci', '-chardev', 'socket,id=cam,host=127.0.0.1,port=10714',
            '-device', 'usb-redir,id=cam,chardev=cam,bus=xhci.0'], [p]


def isocam_boot(work):
    """A USB Video Class camera with an isochronous endpoint and MJPEG
    frames on an xHCI controller, tools/campeer.py --iso behind a QEMU
    usb-redir device (port 10716) (tests/selftest/devices/isocam)"""
    log = os.path.join(work, 'isocampeer.log')
    p = subprocess.Popen([sys.executable, '-u', os.path.join(ROOT, 'tools', 'campeer.py'), '--port', '10716', '--iso'],
                         stdout=open(log, 'w'), stderr=subprocess.STDOUT)
    for _ in range(100):
        if 'listening' in open(log).read() or p.poll() is not None:
            break
        time.sleep(0.05)
    return ['-device', 'qemu-xhci,id=xhci', '-chardev', 'socket,id=cam,host=127.0.0.1,port=10716',
            '-device', 'usb-redir,id=cam,chardev=cam,bus=xhci.0'], [p]


def composite_boot(work):
    """A composite USB device on an xHCI controller (a keyboard and two
    vendor-defined HID functions), tools/compositepeer.py behind a QEMU
    usb-redir device (port 10715) (tests/selftest/devices/composite)"""
    log = os.path.join(work, 'compositepeer.log')
    p = subprocess.Popen([sys.executable, '-u', os.path.join(ROOT, 'tools', 'compositepeer.py'), '--port', '10715'],
                         stdout=open(log, 'w'), stderr=subprocess.STDOUT)
    for _ in range(100):
        if 'listening' in open(log).read() or p.poll() is not None:
            break
        time.sleep(0.05)
    return ['-device', 'qemu-xhci,id=xhci', '-chardev', 'socket,id=combo,host=127.0.0.1,port=10715',
            '-device', 'usb-redir,id=combo,chardev=combo,bus=xhci.0'], [p]


def pnp_boot(work):
    """An xHCI controller with nothing on it: the tests plug a keyboard, a
    hub and a USB stick in and take them out (tests/selftest/devices/pnp)"""
    return ['-device', 'qemu-xhci,id=xhci'], []


def wdm_boot(work):
    """Nothing beyond the core boot's machine: sc loads a Windows kernel-mode
    driver, wdmtest.sys, once test signing is on, under Windows' signing
    policy, then a KMDF driver, kmdftest.sys (tests/selftest/devices/wdm)"""
    return [], []


def edu_boot(work):
    """QEMU's edu device (PCI 1234:11e8): a Plug and Play driver for it,
    edutest.sys, is installed with pnputil (tests/selftest/devices/edu).
    The device sits behind a PCIe root port, which QEMU describes with no
    _PRT of its own: its INTA reaches the driver through the bridge
    swizzle onto the root port's pin in the root bridge's _PRT"""
    return ['-device', 'pcie-root-port,id=rp1,chassis=1,slot=1', '-device', 'edu,bus=rp1'], []


def wifi_boot(work):
    """QEMU's edu device (PCI 1234:11e8) as a Wi-Fi card: wditest.sys, a WDI
    miniport whose radio hears three access points, is installed on it with
    pnputil, and netsh wlan scans through NovaOS's WDI command layer
    (tests/selftest/devices/wifi)"""
    return ['-device', 'edu'], []


def umdf_boot(work):
    """QEMU's edu device (PCI 1234:11e8): a UMDF 2 driver for it,
    umdftest.dll, is installed with pnputil and runs in a driver host
    (tests/selftest/devices/umdf)"""
    return ['-device', 'edu'], []


def wddm_boot(work):
    """The device boots' own standard VGA (PCI 1234:1111), on which pnputil
    installs a WDDM display-only driver, wddmdod.sys (tests/selftest/devices/wddm)"""
    return [], []


def devmgr_boot(work):
    """An e1000e card, an xHCI controller with a USB keyboard and a USB
    tablet, and QEMU's edu device with no driver: Device Manager lists them
    with the rest of the machine (tests/selftest/devices/devmgr)"""
    return ['-netdev', 'user,id=dm0', '-device', 'e1000e,netdev=dm0', '-device', 'qemu-xhci,id=xhci',
            '-device', 'usb-kbd,bus=xhci.0', '-device', 'usb-tablet,bus=xhci.0', '-device', 'edu'], []


def kmdfedu_boot(work):
    """QEMU's edu device (PCI 1234:11e8): a KMDF driver for it, kmdfedu.sys,
    is installed with pnputil and runs on Wdf01000.sys
    (tests/selftest/devices/kmdfedu)"""
    return ['-device', 'edu'], []


def netkvm_boot(work):
    """QEMU's virtio-net card on its user-mode network with tools/h2server.js
    as for the IPv4 boot, and virtio-win's signed netkvm package
    (tools/drivertest/netkvm.py) in C:\\Tests\\NetKVM: pnputil installs it on the
    card, which then runs on NovaOS's NDIS host (tests/selftest/devices/netkvm)"""
    sys.path.insert(0, os.path.join(ROOT, 'tools', 'drivertest'))
    import netkvm
    pkg = os.path.join(work, 'netkvm')
    netkvm.fetch(pkg)
    args, procs = net4_boot(work)
    return args, procs, dict(puts=[(pkg, r'C:\Tests\NetKVM')])


def vioser_boot(work):
    """QEMU's virtio-serial controller with one port, org.novaos.test.0,
    whose host end is tools/drivertest/serialpeer.py (port 10720), and
    virtio-win's signed vioser package (tools/drivertest/vioser.py) in
    C:\\Tests\\Vioser: pnputil installs it (tests/selftest/devices/vioser)"""
    sys.path.insert(0, os.path.join(ROOT, 'tools', 'drivertest'))
    import vioser
    pkg = os.path.join(work, 'vioser')
    vioser.fetch(pkg)
    log = os.path.join(work, 'serialpeer.log')
    p = subprocess.Popen([sys.executable, '-u', os.path.join(ROOT, 'tools', 'drivertest', 'serialpeer.py'), '10720'],
                         stdout=open(log, 'w'), stderr=subprocess.STDOUT)
    for _ in range(100):
        if 'listening' in open(log).read() or p.poll() is not None:
            break
        time.sleep(0.05)
    return ['-chardev', 'socket,id=vs,host=127.0.0.1,port=10720', '-device', 'virtio-serial-pci',
            '-device', 'virtserialport,chardev=vs,name=org.novaos.test.0'], [p], dict(puts=[(pkg, r'C:\Tests\Vioser')])


def ntfs_disk(path, label, mb=128, files=()):
    """A disk image at @path with one fresh NTFS volume from 1 MiB labelled
    @label (mkntfs, from the ntfs-3g package), holding @files ((name, text)
    pairs, written with ntfscp)"""
    mkntfs = shutil.which('mkntfs') or ('/usr/sbin/mkntfs' if os.path.exists('/usr/sbin/mkntfs') else None)
    if not mkntfs:
        raise RuntimeError('mkntfs is not installed (the ntfs-3g package)')
    vol = path + '.vol'
    with open(vol, 'wb') as f:
        f.truncate((mb - 1) << 20)
    subprocess.run([mkntfs, '-F', '-Q', '-L', label, '-p', '2048', '-H', '255', '-S', '63', vol],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for name, text in files:
        src = vol + '.file'
        with open(src, 'w', newline='\r\n') as f:
            f.write(text)
        ntfscp = shutil.which('ntfscp') or '/usr/bin/ntfscp'
        subprocess.run([ntfscp, '-f', vol, src, name], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        os.unlink(src)
    mbr = bytearray(512)                                    # one NTFS partition (type 7) from LBA 2048
    mbr[446:462] = struct.pack('<B3sB3sII', 0, b'\xfe\xff\xff', 0x07, b'\xfe\xff\xff', 2048, (mb - 1) * 2048)
    mbr[510:512] = b'\x55\xaa'
    with open(path, 'wb') as out, open(vol, 'rb') as src:
        out.write(mbr + bytes((1 << 20) - 512))
        shutil.copyfileobj(src, out)
    os.unlink(vol)


def ntfs_boot(work):
    """A second disk with a fresh, empty NTFS volume from 1 MiB, labelled
    NOVATEST (ntfs_disk): drive D:, read-write.  It stays in work/ntfs.img
    for the tests' check once QEMU has quit (tests/selftest/devices/ntfs)"""
    disk = os.path.join(work, 'ntfs.img')
    ntfs_disk(disk, 'NOVATEST')
    return ['-drive', f'format=raw,file={disk}'], []


def diskmgr_boot(work):
    """A second disk, 256 MB of zeros: Disk Management initializes it and
    makes volumes on it.  It stays in work/blank.img for the tests' check
    once QEMU has quit (tests/selftest/devices/diskmgr)"""
    disk = os.path.join(work, 'blank.img')
    with open(disk, 'wb') as f:
        f.truncate(256 << 20)
    return ['-drive', f'format=raw,file={disk}'], []


def vhidmini_boot(work):
    """Nothing beyond the core boot's machine: the device is root-enumerated,
    and Microsoft's vhidmini2 sample, a KMDF HID minidriver, is installed on
    it with pnputil (tests/selftest/devices/vhidmini)"""
    return [], []


def viostor_boot(work):
    """A second disk on QEMU's virtio-blk card (PCI 1af4:1001), which no
    NovaOS driver runs, with an NTFS volume labelled VIOSTOR holding
    viostor.txt (ntfs_disk), and virtio-win's signed viostor package
    (tools/drivertest/viostor.py) in C:\\Tests\\VioStor: pnputil installs it
    on the card, which then runs on NovaOS's Storport host, and the volume
    mounts as D: (tests/selftest/devices/viostor)"""
    sys.path.insert(0, os.path.join(ROOT, 'tools', 'drivertest'))
    import viostor
    pkg = os.path.join(work, 'viostor')
    viostor.fetch(pkg)
    disk = os.path.join(work, 'viostor.img')
    ntfs_disk(disk, 'VIOSTOR', 64, [('viostor.txt', 'NovaOS reads this file through viostor.sys\n')])
    return ['-drive', f'if=none,id=vd0,format=raw,file={disk}', '-device', 'virtio-blk-pci,drive=vd0'], [], \
        dict(puts=[(pkg, r'C:\Tests\VioStor')])


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


def driversearch_boot(work):
    """QEMU's virtio-serial port, which has no NovaOS driver, on a user-mode
    network where 10.0.2.2:18091 answers as the Microsoft Update Catalog
    does (tools/drivertest/catalog_fixture.py, from recorded pages): pnputil
    downloads its driver package by hardware ID (tests/selftest/devices/driversearch)"""
    srv = subprocess.Popen([sys.executable, os.path.join(ROOT, 'tools', 'drivertest', 'catalog_fixture.py'), '18091', work])
    time.sleep(1)
    if srv.poll() is not None:
        raise RuntimeError('the recorded Update Catalog did not start (is port 18091 in use?)')
    return ['-nic', 'user,model=virtio-net-pci', '-device', 'virtio-serial-pci'], [srv]


def usbserial_boot(work):
    """QEMU's usb-serial device (an FTDI FT232, USB\\VID_0403&PID_6001) on an
    xHCI controller, its serial line looped back by a socket that returns
    what it is sent, on a user-mode network where 10.0.2.2:18092
    answers as the Microsoft Update Catalog does with FTDI's own driver
    (tools/drivertest/ftdi_catalog.py): pnputil downloads and installs it
    by hardware ID and serialtest loops data through the COM port
    (tests/selftest/devices/usbserial)"""
    srv = subprocess.Popen([sys.executable, os.path.join(ROOT, 'tools', 'drivertest', 'ftdi_catalog.py'), '18092', work])
    end = time.time() + 900                   # (the first run downloads FTDI's cabinet)
    while time.time() < end and srv.poll() is None:
        try:
            socket.create_connection(('127.0.0.1', 18092), 1).close()
            break
        except OSError:
            time.sleep(1)
    if srv.poll() is not None or time.time() >= end:
        raise RuntimeError('the recorded Update Catalog for FTDI did not start (see catalog.log; is port 18092 in use?)')
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        port = s.getsockname()[1]
    # (QEMU attaches a usb-serial device once its chardev is connected: the
    # line's far end is a socket that sends back what it is sent)
    echo = subprocess.Popen([sys.executable, '-c', ECHO, str(port)])
    for _ in range(50):
        try:
            socket.create_connection(('127.0.0.1', port), 1).close()
            break
        except OSError:
            time.sleep(0.1)
    return ['-nic', 'user,model=virtio-net-pci', '-device', 'qemu-xhci,id=usbserial-xhci',
            '-chardev', f'socket,id=ftdi,host=127.0.0.1,port={port}',
            '-device', 'usb-serial,bus=usbserial-xhci.0,chardev=ftdi'], [srv, echo]


def ax211_boot(work):
    """QEMU's edu card standing in for an Intel AX211 Wi-Fi card
    (opt/novaos/pci-standin; QEMU has no Intel Wi-Fi) on a user-mode network
    where 10.0.2.2:18093 answers as the Microsoft Update Catalog does with
    Intel's own driver (tools/drivertest/ax211_catalog.py): pnputil
    downloads and installs it by hardware ID (tests/selftest/devices/ax211)"""
    srv = subprocess.Popen([sys.executable, os.path.join(ROOT, 'tools', 'drivertest', 'ax211_catalog.py'), '18093', work])
    end = time.time() + 900                   # (the first run downloads Intel's cabinet, 24 MB)
    while time.time() < end and srv.poll() is None:
        try:
            socket.create_connection(('127.0.0.1', 18093), 1).close()
            break
        except OSError:
            time.sleep(1)
    if srv.poll() is not None or time.time() >= end:
        raise RuntimeError('the recorded Update Catalog for the AX211 did not start (see catalog.log; is port 18093 in use?)')
    return ['-nic', 'user,model=virtio-net-pci', '-device', 'edu',
            '-fw_cfg', 'name=opt/novaos/pci-standin,string=1234:11e8 8086:51f1 8086:0094 0280'], [srv]


ECHO = """import socket, sys
srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(('127.0.0.1', int(sys.argv[1])))
srv.listen(4)
while True:
    c, _ = srv.accept()
    while True:
        d = c.recv(65536)
        if not d:
            break
        c.sendall(d)
    c.close()
"""


# The suites that boot once per entry: (label, tests, setup(work) -> (QEMU arguments, processes[,
# more Nova arguments]))
BOOTS = {
    'network': [('ipv4', NET4, net4_boot), ('ipv6', NET6, net6_boot)],
    'devices': [('touch', TOUCH, touch_boot), ('usbaudio', USBAUDIO, usbaudio_boot),
                ('usbheadset', USBHEADSET, usbheadset_boot), ('driversearch', DRIVERSEARCH, driversearch_boot),
                ('monitors', MONITORS, monitors_boot), ('usbboot', USBBOOT, usbboot_boot),
                ('cdboot', CDBOOT, cdboot_boot), ('laptop', LAPTOP, laptop_boot), ('update', UPDATE, update_boot),
                ('gamepad', GAMEPAD, gamepad_boot), ('hid', HID, hid_boot), ('camera', CAMERA, camera_boot),
                ('isocam', ISOCAM, isocam_boot), ('composite', COMPOSITE, composite_boot),
                ('pnp', PNP, pnp_boot), ('ntfs', NTFS, ntfs_boot),
                ('wdm', WDM, wdm_boot), ('edu', EDU, edu_boot), ('vioser', VIOSER, vioser_boot),
                ('usbserial', USBSERIAL, usbserial_boot), ('ax211', AX211, ax211_boot),
                ('wifi', WIFI, wifi_boot),
                ('viostor', VIOSTOR, viostor_boot), ('umdf', UMDF, umdf_boot),
                ('kmdfedu', KMDFEDU, kmdfedu_boot), ('netkvm', NETKVM, netkvm_boot),
                ('wddm', WDDM, wddm_boot), ('devmgr', DEVMGR, devmgr_boot),
                ('diskmgr', DISKMGR, diskmgr_boot), ('vhidmini', VHIDMINI, vhidmini_boot)],
}


def store_verdict(nova, t, out):
    """Wait for the App Store's outcome line for @t; (why it failed or None, the log).
    It must say "Installed", or match @t.expect[0] when there is one ("Downloaded"
    for a program that runs from C:\\Downloads)"""
    pat = re.compile(r'\[STORE\] ' + re.escape(t.store) + r': ([^\n]*)')
    end = time.time() + t.timeout
    while not pat.search(out) and time.time() < end and nova.q.poll() is None:
        time.sleep(1)
        out += nova.sr.read_new()
    m = pat.search(out)
    if not m:
        return f'no outcome from the App Store in {t.timeout} s', out
    good = re.match(t.expect[0], m.group(1)) if t.expect else m.group(1).startswith('Installed')
    return (None if good else m.group(1)), out


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
    exe = exe[:31]                  # (the kernel logs a process's name cut to 31 characters)
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
                for log in ['h2server.log', 'v6peer.log', 'http.log'] + [f'usbredirpeer-{p}.log' for p in range(10700, 10710)] \
                        + [f'padpeer-{p}.log' for p in (10710, 10711, 10712, 10713, 10717)] + ['serialpeer.log', 'catalog.log', 'campeer.log',
                                                                                 'isocampeer.log', 'compositepeer.log']:
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
            elif t.cut:
                if t.before:
                    t.before(nova)
                nova.sr.read_new()
                nova.qmp.type(t.cmd + '\n')
                out, end = '', time.time() + t.timeout
                while not re.search(t.cut, out) and time.time() < end and '[TERM-DONE]' not in out:
                    time.sleep(0.25)
                    out += nova.sr.read_new()
                ok = bool(re.search(t.cut, out))
                if ok:
                    time.sleep(1)                           # (well into the writing)
                    nova.qmp.cmd('system_reset')
                    try:
                        nova.start()
                        out += nova.boot_log
                    except RuntimeError as e:
                        out, ok = out + str(e), False
            else:
                if t.before:
                    t.before(nova)
                    full_log += nova.sr.read_new()          # (what the kernel said meanwhile)
                out, ok = nova.run(t.cmd, t.timeout, shot=(t.shot, png) if t.shot else None, acts=t.acts)
            if t.crash:
                miss = [e for e in t.expect if not re.search(e, out)]
                why = None if ok and not miss else ('no kernel fault' if not ok else 'no symbolized backtrace')
            elif t.reboot or t.cut:
                miss = [e for e in t.expect if not re.search(e, out)]
                why = ('did not boot again' if t.reboot else f'no "{t.cut}" to cut off, or no boot after it') if not ok \
                    else f'missing "{miss[0]}"' if miss else None
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
            if nova.stuck:
                # A program the Terminal cannot stop (a Venus fence that never signals, say) holds it
                # for good: every test after it would only wait out its own limit (the graphics job
                # burned its hour that way).  Reset the machine, as for a crash, and go on
                try:
                    nova.qmp.cmd('system_reset')
                    nova.start()
                    full_log += nova.boot_log
                    nova.stuck = False
                    print('    reset NovaOS: the program above did not stop', flush=True)
                except RuntimeError as e:
                    print(f'    did not start again after the hang: {str(e).splitlines()[0]}', flush=True)
                    nova.wedged = True
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
