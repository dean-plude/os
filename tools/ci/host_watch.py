#!/usr/bin/env python3
"""Watch the host side of the graphics self-tests.

    tools/ci/host_watch.py OUT_DIR [HANG_SECONDS] &

The graphics job runs a QEMU whose virtio-gpu hands Venus's Vulkan to
virglrenderer, which runs each guest context in a worker process of its
virgl_render_server.  When a test hangs, the guest only says that it waits
for the host; this says what the host was doing.  Every 15 seconds one line
goes to OUT_DIR/host-watch.log: the guest's serial log size, and the CPU
time and thread count of QEMU and of the render server's processes (a stuck
side shows as a CPU time that stands still, a spinning one as one that
climbs).  When the serial log has been silent for HANG_SECONDS (default 360;
the tests' own limit is 900) it also writes OUT_DIR/host-hang-N.txt (up to
three, five minutes apart): the last serial lines, every host thread's state
and wait channel, its CPU time over five seconds, and gdb's backtrace of
QEMU and of each render process (sudo gdb, when gdb is there).
"""
import glob, os, subprocess, sys, time

EVERY = 15


def pids(comm):
    """Process ids whose command name starts with @comm"""
    out = []
    for p in glob.glob('/proc/[0-9]*'):
        try:
            if open(p + '/comm').read().strip().startswith(comm):
                out.append(int(p[6:]))
        except OSError:
            pass
    return sorted(out)


def cpu(pid, tid=None):
    """CPU ticks (user + system) of a process, or of one of its threads"""
    path = f'/proc/{pid}/stat' if tid is None else f'/proc/{pid}/task/{tid}/stat'
    try:
        f = open(path).read().rsplit(')', 1)[1].split()
        return int(f[11]) + int(f[12])
    except (OSError, IndexError, ValueError):
        return 0


def threads(pid):
    """(tid, name, state, wait channel, CPU ticks) of each thread of @pid"""
    rows = []
    for t in sorted(glob.glob(f'/proc/{pid}/task/[0-9]*'), key=lambda s: int(s.rsplit('/', 1)[1])):
        tid = int(t.rsplit('/', 1)[1])
        try:
            head, rest = open(t + '/stat').read().rsplit(')', 1)
            name = head.split('(', 1)[1]
            state = rest.split()[0]
            wchan = open(t + '/wchan').read().strip() or '-'
        except (OSError, IndexError):
            continue
        rows.append((tid, name, state, wchan, cpu(pid, tid)))
    return rows


def serial_path(qemu):
    try:
        for a in open(f'/proc/{qemu}/cmdline').read().split('\0'):
            if a.startswith('file:'):
                return a[5:]
    except OSError:
        pass
    return None


def size(path):
    try:
        return os.path.getsize(path)
    except OSError:
        return 0


def tail(path, n=25):
    try:
        return ''.join(open(path, errors='replace').readlines()[-n:])
    except OSError:
        return '(no serial log)\n'


def gdb(pid):
    cmd = ['sudo', '-n', 'gdb', '-p', str(pid), '-batch', '-nx', '-ex', 'set pagination off',
           '-ex', 'set debuginfod enabled off', '-ex', 'thread apply all bt 14']
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        return r.stdout + r.stderr[-2000:]
    except (OSError, subprocess.TimeoutExpired) as e:
        return f'(gdb: {e})\n'


def dump(out, n, qemu, serial):
    procs = [qemu] + pids('virgl_render')
    lines = [f'host snapshot {n} at {time.strftime("%H:%M:%S")}: the guest has been silent a while\n',
             '--- the serial log ends:\n', tail(serial) if serial else '(no serial log)\n']
    before = {p: {t[0]: t[4] for t in threads(p)} for p in procs}
    time.sleep(5)
    for p in procs:
        try:
            name = open(f'/proc/{p}/comm').read().strip()
        except OSError:
            continue
        lines.append(f'--- {name} pid {p}: threads (state, wait channel, CPU ticks in the last 5 s of 1/100 s)\n')
        for tid, nm, st, wc, ticks in threads(p):
            lines.append(f'  {tid:7d} {nm:16s} {st} {wc:24s} {ticks - before[p].get(tid, ticks):5d}\n')
    for p in procs[:8]:
        lines.append(f'--- gdb, pid {p}\n')
        lines.append(gdb(p))
    open(os.path.join(out, f'host-hang-{n}.txt'), 'w').write(''.join(lines))


def main():
    out = sys.argv[1]
    hang = int(sys.argv[2]) if len(sys.argv) > 2 else 360
    os.makedirs(out, exist_ok=True)
    log = open(os.path.join(out, 'host-watch.log'), 'a', buffering=1)
    last_size, last_change, dumps, last_dump = -1, time.time(), 0, 0.0
    while True:
        time.sleep(EVERY)
        q = pids('qemu-system')
        if not q:
            last_size, last_change = -1, time.time()
            continue
        serial = serial_path(q[0])
        sz = size(serial) if serial else 0
        now = time.time()
        if sz != last_size:
            last_size, last_change = sz, now
        r = pids('virgl_render')
        log.write(f'{time.strftime("%H:%M:%S")} serial {sz} qemu {q[0]} cpu {cpu(q[0])} threads {len(threads(q[0]))}'
                  f' render {len(r)} proc(s) cpu {sum(cpu(p) for p in r)}\n')
        if now - last_change >= hang and dumps < 3 and now - last_dump >= 300:
            dumps += 1
            last_dump = now
            log.write(f'{time.strftime("%H:%M:%S")} silent for {int(now - last_change)} s: host-hang-{dumps}.txt\n')
            dump(out, dumps, q[0], serial)


if __name__ == '__main__':
    main()
