#!/usr/bin/env python3
"""Boot NovaOS in QEMU, run the self-test programs and say which passed.

    tools/selftest.py [--img build/nova.img] [--out DIR] [--only NAME,...]
                      [--junit FILE] [--summary FILE]

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
    def __init__(self, name, cmd, expect=(), timeout=180, check=None):
        self.name, self.cmd, self.expect, self.timeout, self.check = name, cmd, expect, timeout, check


def tones(*hz):
    """A check on the sound recording: a tone near each of @hz, in order"""
    def check(nova):
        nova.close()                       # QEMU writes the WAV header on quit
        segs = wavcheck.segments(nova.wav)
        heard = [s[3] for s in segs if s[1] >= 300]
        want = list(hz)
        for h in heard:
            if want and abs(h - want[0]) < want[0] * 0.05:
                want.pop(0)
        return None if not want else f'no {want[0]} Hz tone in the recording (heard: ' + \
            ', '.join(f'{h:.0f} Hz' for h in heard) + ')'
    return check


# The boot that runs the tests has an unplugged AC adapter and a battery
# (tests/acpi/battery.asl) and an Intel HD Audio card recorded to a WAV.
TESTS = [
    Test('apitest', 'apitest', [r'apitest: \d+ passed, 0 failed']),
    Test('filetest', 'filetest', [r'filetest: \d+ passed, 0 failed']),
    Test('pipetest', 'pipetest', [r'pipetest: \d+ passed, 0 failed']),
    Test('guitest', 'guitest auto', [r'guitest: \d+ passed, 0 failed']),
    Test('disptest', 'disptest', [r'\d+ passed, 0 failed']),
    Test('battery', 'battery', [r'Power source: battery', r'Battery: 75%', r'Time left: 3 h 00 min',
                                r'SystemBatteryState: present 1, AC 0, charging 0, discharging 1']),
    Test('soundtest tone', 'soundtest tone 440 1000', [r'played \d+ samples']),
    Test('soundtest wasapi', 'soundtest wasapi 660 1000', [r'played \d+ frames'], check=tones(440, 660)),
]


def verdict(t, out, ok, exe):
    """Why @t failed, or None"""
    if not ok:
        return f'did not finish in {t.timeout} s'
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
    for e in t.expect:
        if not re.search(e, out):
            return f'missing "{e}"'
    return None


PANIC = re.compile(r'KERNEL PANIC|KERNEL PAGE FAULT|DOUBLE FAULT|Unhandled kernel exception')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--img', default=os.path.join(ROOT, 'build', 'nova.img'))
    ap.add_argument('--out', default='selftest-out')
    ap.add_argument('--only')
    ap.add_argument('--junit')
    ap.add_argument('--summary')
    a = ap.parse_args()

    tests = [t for t in TESTS if not a.only or t.name in a.only.split(',') or t.cmd.split()[0] in a.only.split(',')]
    os.makedirs(a.out, exist_ok=True)
    work = tempfile.mkdtemp(prefix='selftest')
    aml = os.path.join(work, 'battery.aml')
    subprocess.run(['iasl', '-p', aml[:-4], os.path.join(ROOT, 'tests', 'acpi', 'battery.asl')],
                   check=True, stdout=subprocess.DEVNULL)
    wav = os.path.join(a.out, 'sound.wav')
    results = []
    t_boot = time.time()
    try:
        nova = Nova(a.img, work, data_mb=64, wav=wav, extra_args=['-acpitable', f'file={aml}'])
    except RuntimeError as e:
        print(e)
        shutil.copy(os.path.join(work, 'serial.log'), a.out)
        report(a, [('boot', str(e).splitlines()[0], 0, '')])
        return 1
    print(f'booted in {time.time() - t_boot:.0f} s', flush=True)
    full_log = nova.boot_log
    try:
        for t in tests:
            t0 = time.time()
            out, ok = nova.run(t.cmd, t.timeout)
            full_log += out
            why = verdict(t, out, ok, t.cmd.split()[0] + '.exe')
            if PANIC.search(out):
                why = 'kernel panic'
            slug = re.sub(r'\W+', '-', t.name)
            if nova.q.poll() is None:
                nova.shot(os.path.join(a.out, slug + '.png'))
            if not why and t.check:
                why = t.check(nova)
            results.append((t.name, why, time.time() - t0, out))
            print(f'{"PASS" if not why else "FAIL"}  {t.name:18s} {time.time() - t0:6.1f} s  {why or ""}', flush=True)
            if why:
                print('    ' + '\n    '.join(l for l in out.splitlines() if not l.startswith('[SCHED]'))[-4000:])
            if why == 'kernel panic' or nova.q.poll() is not None:
                break
    finally:
        nova.close()
        with open(os.path.join(a.out, 'serial.log'), 'w') as f:
            f.write(full_log + nova.sr.read_new())
        shutil.rmtree(work, ignore_errors=True)
    ran = {r[0] for r in results}
    results += [(t.name, 'not run (an earlier test stopped NovaOS)', 0, '') for t in tests if t.name not in ran]
    report(a, results)
    return sum(1 for r in results if r[1])


def report(a, results):
    failed = sum(1 for r in results if r[1])
    print(f'\n{len(results) - failed} of {len(results)} self-tests passed')
    if a.summary:
        with open(a.summary, 'a') as f:
            f.write(f'### NovaOS self-tests: {len(results) - failed} of {len(results)} passed\n\n')
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
