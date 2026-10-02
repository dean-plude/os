#!/usr/bin/env python3
"""Boot NovaOS in QEMU, run the self-test programs and say which passed.

    tools/selftest.py [--suite core|graphics|network] [--img build/nova.img] [--out DIR]
                      [--only NAME,...] [--junit FILE] [--summary FILE]

Suites:
  core      (default) apitest, abitest, filetest, pipetest, proctest, guitest auto,
            disptest, battery, soundtest, sleeptest timer (Sleep and wait
            timeouts within 1 ms under load), powertest (the lid, a thermal zone
            and sleep, driven from the QEMU monitor), and last "crash
            kernel" (a deliberate kernel fault must print a symbolized
            backtrace)
  graphics  installs "Mesa 3D" and "DXVK" with the App Store, then runs
            tools/gltest and tools/d3dtest, 64- and 32-bit.  Needs --gfx DIR,
            made by tools/ci/stage-graphics.sh: 7-Zip, the two downloads and
            the four test programs
  network   two boots with a virtio-net adapter.  IPv4 on QEMU's user-mode
            network: ipconfig, ping, Winsock (netcat) and winhttp's HTTP/2
            (httptest suite) against tools/h2server.js (needs node and
            openssl).  IPv6 on an IPv6-only network that is tools/v6peer.py:
            SLAAC and RDNSS (ipconfig), ping -6, curl -6 and Winsock over
            IPv6 (netcat)

Each test is one Terminal command (tools/novarun.py's Nova class types it).
A test passes when the program exits with code 0 inside its time limit, has
printed no "FAIL" line and no "N failed" count above zero, and prints what
the test expects.  A kernel panic fails the run.  The serial log, a
screenshot after each test and the sound recording are kept in --out.

The exit status is the number of failed tests (0: all passed), so CI can
gate on it.  --summary appends a Markdown table (GitHub's step summary).
"""
import argparse, os, re, shutil, subprocess, sys, tempfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from novarun import Nova, ROOT
import wavcheck


class Test:
    """@cmd typed into the Terminal.  @store: instead of a program's exit,
    wait for the App Store's "[STORE] @store: Installed" line.  @shot: take
    the screenshot 2 s after the output matches this regex (while the
    program draws).  @crash: the command halts the kernel on purpose; the
    test passes when the serial log then shows @expect (it runs last).
    @acts: (regex, function(nova)) pairs run when the output matches (the
    program asks for something only the test can do).  @boot_expect: regexes
    the whole serial log so far must match (what the kernel logged at boot).
    @builtin: a Terminal command, not a program (no exit code; the output
    decides)"""
    def __init__(self, name, cmd, expect=(), timeout=180, check=None, store=None, shot=None, crash=False,
                 acts=(), boot_expect=(), builtin=False):
        self.name, self.cmd, self.expect, self.timeout, self.check = name, cmd, expect, timeout, check
        self.store, self.shot, self.crash = store, shot, crash
        self.acts, self.boot_expect, self.builtin = acts, boot_expect, builtin


def tones(*hz):
    """A check on the sound recording: a tone near each of @hz, in order"""
    def check(nova):                       # (run after QEMU quit: the WAV is complete)
        segs = wavcheck.segments(nova.wav)
        heard = [s[3] for s in segs if s[1] >= 300]
        want = list(hz)
        for h in heard:
            if want and abs(h - want[0]) < want[0] * 0.05:
                want.pop(0)
        return None if not want else f'no {want[0]} Hz tone in the recording (heard: ' + \
            ', '.join(f'{h:.0f} Hz' for h in heard) + ')'
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
    for _ in range(240):
        log = open(nova.serial_path, 'rb').read().decode('latin-1')
        if 'Woke up' in log and re.search(r'Woke up[\s\S]*\[USB\] port \d+: keyboard', log):
            break
        time.sleep(0.25)


def set_temp(c):
    return lambda nova: nova.hmp(f'o /b 0xe9 {c}')


# The boot that runs the tests has an unplugged AC adapter and a battery
# (tests/acpi/battery.asl), a lid and a thermal zone (tests/acpi/lid-thermal.asl,
# with pc-testdev as its embedded controller), a USB keyboard on an xHCI
# controller at 00:05.0 and an Intel HD Audio card recorded to a WAV.
CORE = [
    Test('apitest', 'apitest', [r'apitest: \d+ passed, 0 failed']),
    Test('abitest', 'abitest', [r'abitest: \d+ passed, 0 failed']),
    Test('filetest', 'filetest', [r'filetest: \d+ passed, 0 failed']),
    Test('pipetest', 'pipetest', [r'pipetest: \d+ passed, 0 failed']),
    Test('proctest', 'proctest', [r'proctest: \d+ passed, 0 failed']),
    Test('guitest', 'guitest auto', [r'guitest: \d+ passed, 0 failed']),
    Test('disptest', 'disptest', [r'\d+ passed, 0 failed']),
    Test('battery', 'battery', [r'Power source: battery', r'Battery: 75%', r'Time left: 3 h 00 min',
                                r'SystemBatteryState: present 1, AC 0, charging 0, discharging 1']),
    Test('soundtest tone', 'soundtest tone 440 1000', [r'played \d+ samples']),
    Test('soundtest wasapi', 'soundtest wasapi 660 1000', [r'played \d+ frames'], check=tones(440, 660)),
    Test('sleeptest timer', 'sleeptest timer', [r'sleeptest: resolution \d+\.\d+ ms under load', r'sleeptest: PASS'],
         boot_expect=[r'\[HPET\] At 0x[0-9a-f]+: \d+ Hz', r'calibrated against the HPET',
                      r'\[APIC\] (One-shot|TSC-deadline) timer started']),
    Test('powertest', 'powertest', [r'powertest: \d+ passed, 0 failed', r'\[SHELL\] Lid closed: sleeping',
                                    r'\[SLEEP\] Woke up', r'\[ACPI\] Lid open',
                                    r'TZ00: 70\.0 C, at or above the passive trip point \(60\.0 C\): passive cooling on',
                                    r'TZ00: 45\.0 C, below the passive trip point \(60\.0 C\): passive cooling off'],
         acts=[(r'powertest: close the lid', close_lid), (r'powertest: heat to 70 C', set_temp(70)),
               (r'powertest: cool to 45 C', set_temp(45))],
         boot_expect=[r'\[ACPI\] SCI on IRQ \d+ \(GSI \d+\)', r'\[ACPI\] PCI interrupt routing: \d+ entries',
                      r'\[ACPI\] Lid \\_SB_\.LID0', r'\[ACPI\] Thermal zone \\_TZ_\.TZ00: 40\.0 C',
                      r'\[ACPI\] Wake device \\_SB_\.LID0 \(lid\)',
                      r'\[ACPI\] Wake device \\_SB_\.PCI0\.XHC0 \(USB controller\)',
                      r'\[USB\] [^\n]*keyboard[^\n]*wakes the machine'],
         timeout=300),
    # last: crash.exe asks the kernel to fault, which must print a backtrace with names
    Test('kernel backtrace', 'crash kernel', [r'Backtrace:\r?\n  #0 [0-9a-f]{16}  KeCrashTestFault\+0x[0-9a-f]+\r?\n'
                                              r'  #1 [0-9a-f]{16}  KeCrashTest\+0x[0-9a-f]+\r?\n'
                                              r'  #2 [0-9a-f]{16}  sys_nova_bugcheck\+0x[0-9a-f]+\r?\n'],
         timeout=60, crash=True),
]

# The graphics boot: 7-Zip in C:\Programs\7-Zip and the Mesa and DXVK
# downloads in C:\Downloads, so the Store's button installs without a network
GRAPHICS = [
    Test('install Mesa 3D', 'store install Mesa 3D', store='Mesa 3D', timeout=1200),
    Test('install DXVK', 'store install DXVK', store='DXVK', timeout=600),
    Test('gltest x64', r'C:\Tests\gltest.exe 6', [r'gltest: 14 passed, 0 failed'], timeout=600, shot=r'GLSL '),
    Test('gltest x86', r'C:\Tests\gltest32.exe 6', [r'gltest: 14 passed, 0 failed'], timeout=600, shot=r'GLSL '),
    Test('d3dtest x64', r'C:\Tests\d3dtest.exe 6', [r'd3dtest: 17 passed, 0 failed'], timeout=900, shot=r'D3D9 pixels'),
    Test('d3dtest x86', r'C:\Tests\d3dtest32.exe 6', [r'd3dtest: 17 passed, 0 failed'], timeout=900, shot=r'D3D9 pixels'),
]


# The network suite: two boots, each with a virtio-net adapter.
# IPv4 and HTTP/2: QEMU's user-mode network, where 10.0.2.2 is this machine
# and tools/h2server.js serves HTTPS (HTTP/2 by ALPN, self-signed) on 18443
# and HTTP/1.1 on 18080.
NET4 = [
    Test('virtio-net', 'ipconfig', [r'Virtio network adapter', r'IPv4 Address[ .]*: 10\.0\.2\.15'], builtin=True,
         boot_expect=[r'\[VIRTIO\] Network adapter at [0-9a-f:.]+, MAC [0-9a-f:]+, queues \d+/\d+, link up']),
    Test('ping', 'ping 10.0.2.2', [r'Received = 4, Lost = 0'], builtin=True),
    Test('winsock IPv4', 'netcat -p 18080 10.0.2.2 /hello', [r'10\.0\.2\.2 -> 10\.0\.2\.2 \(IPv4\)',
                                                          r'HTTP/1\.1 200 OK', r'Hello over HTTP/1\.[01]']),
    Test('winhttp HTTP/2', 'httptest suite https://10.0.2.2:18443 http://10.0.2.2:18080',
         [r'httptest: 9 passed, 0 failed']),
]
# IPv6: an IPv6-only network that is tools/v6peer.py (a router with SLAAC
# and RDNSS, DNS for nova6.test, HTTP on port 80), over a QEMU datagram netdev
NET6 = [
    Test('IPv6 address', 'ipconfig', [r'IPv6 Address[ .]*: fd00:6e6f:7661:0:[0-9a-f:]+',
                                      r'DNS Servers[ .]*: fd00:6e6f:7661::1'], builtin=True),
    Test('ping -6', 'ping -6 nova6.test', [r'Reply from fd00:6e6f:7661::1', r'Received = 4, Lost = 0'], builtin=True),
    Test('curl -6', 'curl -6 http://nova6.test/', [r'200 OK', r'Hello over IPv6'], builtin=True),
    Test('winsock IPv6', 'netcat nova6.test', [r'nova6\.test -> fd00:6e6f:7661::1 \(IPv6\)',
                                               r'connected \[fd00:6e6f:7661:[0-9a-f:]+\]:\d+ -> \[fd00:6e6f:7661::1\]:80',
                                               r'Hello over IPv6']),
]


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
    text = KLOG.sub('', out)
    for e in t.expect:
        if not re.search(e, text):
            return f'missing "{e}"'
    return None


KLOG = re.compile(r'\[(?:UM|SCHED)\] [^\n]*\n')
PANIC = re.compile(r'KERNEL PANIC|KERNEL PAGE FAULT|DOUBLE FAULT|Unhandled kernel exception')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--suite', choices=('core', 'graphics', 'network'), default='core')
    ap.add_argument('--gfx', help='the graphics suite\'s files (tools/ci/stage-graphics.sh)')
    ap.add_argument('--img', default=os.path.join(ROOT, 'build', 'nova.img'))
    ap.add_argument('--out', default='selftest-out')
    ap.add_argument('--only')
    ap.add_argument('--junit')
    ap.add_argument('--summary')
    a = ap.parse_args()

    def chosen(suite):
        return [t for t in suite if not a.only or t.name in a.only.split(',') or t.cmd.split()[0] in a.only.split(',')]
    os.makedirs(a.out, exist_ok=True)
    results = []
    if a.suite == 'network':
        for name, suite, setup in (('ipv4', NET4, net4_boot), ('ipv6', NET6, net6_boot)):
            tests = chosen(suite)
            if not tests:
                continue
            work = tempfile.mkdtemp(prefix='selftest')
            procs = []
            try:
                args, procs = setup(work)
                results += run_boot(a, tests, work, name, extra_args=args, data_mb=64)
            finally:
                for p in procs:
                    p.kill()
                for log in ('h2server.log', 'v6peer.log'):
                    if os.path.exists(os.path.join(work, log)):
                        shutil.copy(os.path.join(work, log), a.out)
                shutil.rmtree(work, ignore_errors=True)
        report(a, results)
        return sum(1 for r in results if r[1])

    tests = chosen(CORE if a.suite == 'core' else GRAPHICS)
    work = tempfile.mkdtemp(prefix='selftest')
    tables = []
    for asl in ('battery', 'lid-thermal'):
        aml = os.path.join(work, asl + '.aml')
        subprocess.run(['iasl', '-p', aml[:-4], os.path.join(ROOT, 'tests', 'acpi', asl + '.asl')],
                       check=True, stdout=subprocess.DEVNULL)
        tables += ['-acpitable', f'file={aml}']
    wav = os.path.join(a.out, 'sound.wav') if a.suite == 'core' else None
    puts, data_mb = [], 64
    if a.suite == 'graphics':
        if not a.gfx:
            sys.exit('the graphics suite needs --gfx DIR (tools/ci/stage-graphics.sh DIR)')
        puts = [(os.path.join(a.gfx, '7zip'), r'C:\Programs\7-Zip'), (os.path.join(a.gfx, 'downloads'), r'C:\Downloads'),
                (os.path.join(a.gfx, 'tests'), r'C:\Tests')]
        data_mb = 1024
    try:
        results = run_boot(a, tests, work, None, puts=puts, mem=4096 if a.suite == 'graphics' else 2048,
                           data_mb=data_mb, wav=wav,
                           extra_args=tables + ['-device', 'pc-testdev', '-device', 'qemu-xhci,id=xhci,addr=0x5',
                                                '-device', 'usb-kbd,id=usbkbd,bus=xhci.0'])
    finally:
        shutil.rmtree(work, ignore_errors=True)
    report(a, results)
    return sum(1 for r in results if r[1])


def run_boot(a, tests, work, label, **nova_args):
    """Boot NovaOS once and run @tests; [(name, why it failed or None, seconds, output)].
    @label names the boot's serial log (serial-LABEL.log) when a suite has several"""
    results = []
    log_name = f'serial-{label}.log' if label else 'serial.log'
    t_boot = time.time()
    try:
        nova = Nova(a.img, work, **nova_args)
    except RuntimeError as e:
        print(e)
        shutil.copy(os.path.join(work, 'serial.log'), os.path.join(a.out, log_name))
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
            else:
                out, ok = nova.run(t.cmd, t.timeout, shot=(t.shot, png) if t.shot else None, acts=t.acts)
            if t.crash:
                miss = [e for e in t.expect if not re.search(e, out)]
                why = None if ok and not miss else ('no kernel fault' if not ok else 'no symbolized backtrace')
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
            if PANIC.search(out) and not t.crash:
                why = 'kernel panic'
            if nova.q.poll() is None and not (t.shot and os.path.exists(png)):
                nova.shot(png)
            if not why and t.check:
                deferred.append((len(results), t))          # checked once QEMU has quit
            results.append((t.name, why, time.time() - t0, out))
            print(f'{"PASS" if not why else "FAIL"}  {t.name:18s} {time.time() - t0:6.1f} s  {why or ""}', flush=True)
            if why:
                print('    ' + '\n    '.join(l for l in out.splitlines() if not l.startswith('[SCHED]'))[-4000:])
            if why == 'kernel panic' or t.crash or nova.q.poll() is not None:
                break
    finally:
        nova.close()
        with open(os.path.join(a.out, log_name), 'w') as f:
            f.write(full_log + nova.sr.read_new())
    for i, t in deferred:                                   # e.g. the sound recording, now complete
        why = t.check(nova)
        if why:
            name, _, secs, out = results[i]
            results[i] = (name, why, secs, out)
            print(f'FAIL  {t.name:18s} {why}', flush=True)
    ran = {r[0] for r in results}
    results += [(t.name, 'not run (an earlier test stopped NovaOS)', 0, '') for t in tests if t.name not in ran]
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
