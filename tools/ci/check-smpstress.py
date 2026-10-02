#!/usr/bin/env python3
"""Check a `smpstress scaling 0` log (tools/novarun.py output) from the
nightly run: every test passed, nothing hung, and, where the machine runs
the CPUs side by side, files and the registry scale at least half as well
as bare system calls do in the same run.

The bare system call loop (NtQuerySystemTime) takes no lock, so its scaling
is what the machine allows; work still under one big lock would stay near
1x while it reaches 3-4x.  A shared runner often does not run the emulated
CPUs side by side at all (system calls 0.6-1x, moving a lot from run to
run): there the figures are only recorded.  Exit status 0: pass."""
import re, sys

RATIO = 0.5           # of the system calls' scaling
PARALLEL = 2.0        # system calls scaling at least this: the CPUs ran side by side


def main(path):
    log = open(path, errors='replace').read()
    if 'TIMEOUT' in log:
        print('smpstress hung (TIMEOUT)')
        return 1
    m = re.search(r'smpstress: (\d+) passed, (\d+) failed', log)
    if not m or m.group(2) != '0':
        print('smpstress: ' + (m.group(0) if m else 'no result line'))
        return 1
    x = {name.strip(): float(v) for name, v in
         re.findall(r'^  ([a-z() ]+?)\s+1 thread .*: ([0-9.]+)x$', log, re.M)}
    calls = x.get('(system calls)')
    if not calls:
        print('no system call yardstick in the log')
        return 1
    cpus = re.search(r'smpstress: (\d+) CPUs', log)
    if cpus:                            # (a slow one-thread sample can make it look better than possible)
        calls = min(calls, float(cpus.group(1)))
    if calls < PARALLEL:
        print(f'bare system calls scaled {calls}x: the CPUs did not run side by side here, '
              'so scaling is recorded, not judged')
        return 0
    bad = 0
    for name in ('files', 'registry'):
        got = x.get(name)
        ok = got is not None and got >= RATIO * calls
        print(f'{name}: {got}x against bare system calls {calls}x: {"ok" if ok else "too low"}')
        bad += not ok
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1]))
