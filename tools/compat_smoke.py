#!/usr/bin/env python3
"""Bounded compatibility regressions using the core suite's existing assertions."""
import argparse
from pathlib import Path
import tempfile
import selftest

NAMES = ('dlltest x64', 'dlltest x86', 'wait migration x64', 'wait migration x86',
         'sectest', 'unwindtest', 'wvstarttest')


def selected(core=selftest.CORE):
    tests = []
    for name in NAMES:
        matches = [t for t in core if t.name == name]
        if len(matches) != 1:
            raise ValueError(f'smoke test {name!r}: expected one definition, found {len(matches)}')
        t = matches[0]
        if not t.expect or t.timeout > 180 or t.crash or t.reboot or t.store or t.builtin:
            raise ValueError(f'smoke test {name!r} lost its bounded program assertions')
        tests.append(t)
    return tests


def required(paths):
    # Broad kernel/userland scope includes loader, scheduler, sync, MM and APIs;
    # also rerun when headers, the selected tests or their harness change.
    return any(p.startswith(('kernel/', 'userland/', 'include/', 'tests/selftest/core/')) or
               p in {'CMakeLists.txt', 'tools/selftest.py', 'tools/novarun.py',
                     'tools/compat_smoke.py', '.github/workflows/ci.yml',
                     'tests/tools/test_compat_smoke.py'} or p.startswith('cmake/') for p in paths)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--img', default='build/nova.img')
    ap.add_argument('--out', default='compat-smoke-out')
    ap.add_argument('--junit')
    ap.add_argument('--summary')
    ap.add_argument('--list', action='store_true')
    a = ap.parse_args()
    tests = selected()
    if a.list:
        for t in tests:
            print(f'{t.name}: {t.cmd} (timeout {t.timeout}s)')
        return 0
    Path(a.out).mkdir(parents=True, exist_ok=True)
    a.suite = 'compatibility smoke'
    # No downloads, audio, ACPI compiler or deliberate panic/reboot needed.
    with tempfile.TemporaryDirectory(prefix='compat-smoke-') as work:
        results = selftest.run_boot(a, tests, work, None, smp=4, data_mb=64, vga=('-vga', 'std'))
    selftest.report(a, results)
    completed = [r[0] for r in results]
    if completed != list(NAMES):
        print('FAIL: smoke suite did not produce a result for every selected test')
        return 1
    return int(any(r[1] for r in results))


if __name__ == '__main__':
    raise SystemExit(main())
