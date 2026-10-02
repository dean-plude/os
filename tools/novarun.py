#!/usr/bin/env python3
"""Run commands in NovaOS's Terminal under QEMU and capture their output.

    tools/novarun.py [options] [--put HOSTDIR=C:\\Dir ...] COMMAND ...

Boots build/nova.img (a throwaway snapshot) with a fresh data disk holding
the --put trees, opens the Terminal (Win, "terminal", Enter), turns on
"serial on" and types each COMMAND, waiting for the Terminal's end-of-command
mark.  Prints each command's output.  A COMMAND of the form
"!shot NAME.png" saves a screenshot, "!wait N" waits N seconds and
"!keys a b ctrl-c" presses QEMU key names.

Options: --mem MiB (2048), --smp N (2), --timeout S per command (120),
--keep DIR (keep the serial log, data disk and screenshots there),
--img PATH (the boot image), --wav PATH (an Intel HD Audio card whose
output QEMU records to PATH).
"""
import argparse, json, os, shutil, socket, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OVMF = next((p for p in ('/usr/share/ovmf/OVMF.fd', '/usr/share/OVMF/OVMF_CODE.fd',
                         '/usr/share/qemu/OVMF.fd') if os.path.exists(p)), None)

SHIFTED = {'!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6', '&': '7', '*': '8',
           '(': '9', ')': '0', '_': 'minus', '+': 'equal', '{': 'bracket_left',
           '}': 'bracket_right', '|': 'backslash', ':': 'semicolon', '"': 'apostrophe',
           '<': 'comma', '>': 'dot', '?': 'slash', '~': 'grave_accent'}
PLAIN = {' ': 'spc', '-': 'minus', '=': 'equal', '[': 'bracket_left', ']': 'bracket_right',
         '\\': 'backslash', ';': 'semicolon', "'": 'apostrophe', ',': 'comma', '.': 'dot',
         '/': 'slash', '`': 'grave_accent', '\n': 'ret', '\t': 'tab'}


def keys_for(ch):
    if ch.isalpha():
        return (['shift'] if ch.isupper() else []) + [ch.lower()]
    if ch.isdigit():
        return [ch]
    if ch in SHIFTED:
        return ['shift', SHIFTED[ch]]
    return [PLAIN[ch]]


class Qmp:
    def __init__(self, path):
        for _ in range(100):
            try:
                self.s = socket.socket(socket.AF_UNIX)
                self.s.connect(path)
                break
            except OSError:
                time.sleep(0.1)
        self.f = self.s.makefile('rw')
        self.f.readline()
        self.cmd('qmp_capabilities')

    def cmd(self, name, **args):
        self.f.write(json.dumps({'execute': name, 'arguments': args}) + '\n')
        self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if 'return' in r or 'error' in r:
                return r

    def key(self, *names, hold=60):
        self.cmd('send-key', keys=[{'type': 'qcode', 'data': n} for n in names], **{'hold-time': hold})

    def type(self, text):
        for ch in text:
            self.key(*keys_for(ch))
            time.sleep(0.03)


class Serial:
    def __init__(self, path):
        self.path, self.pos = path, 0

    def read_new(self):
        try:
            with open(self.path, 'rb') as f:
                f.seek(self.pos)
                d = f.read()
        except FileNotFoundError:
            return ''
        self.pos += len(d)
        return d.decode('latin-1')

    def wait(self, needle, timeout):
        buf, end = '', time.time() + timeout
        while time.time() < end:
            buf += self.read_new()
            if needle in buf:
                return buf, True
            time.sleep(0.25)
        return buf, False


def make_data(path, puts, size_mb):
    with open(path, 'wb') as f:
        f.truncate(size_mb << 20)
    subprocess.run(['mkfs.fat', '-F32', '-n', 'NOVADATA', path], check=True, stdout=subprocess.DEVNULL)
    env = dict(os.environ, MTOOLS_SKIP_CHECK='1')
    subprocess.run(['mmd', '-i', path, '::/NOVA', '::/NOVA/C'], check=True, env=env)
    made = set()
    for host, dest in puts:
        parts = [p for p in dest.replace('C:', '', 1).strip('\\').split('\\') if p]
        for i in range(1, len(parts) + 1):
            d = '::/NOVA/C/' + '/'.join(parts[:i])
            if d not in made and (i < len(parts) or os.path.isdir(host)):
                subprocess.run(['mmd', '-i', path, d], env=env, stderr=subprocess.DEVNULL)
                made.add(d)
        target = '::/NOVA/C/' + '/'.join(parts)
        if os.path.isdir(host):
            for e in sorted(os.listdir(host)):
                subprocess.run(['mcopy', '-s', '-o', '-i', path, os.path.join(host, e), target + '/'],
                               check=True, env=env)
        else:
            subprocess.run(['mcopy', '-o', '-i', path, host, target], check=True, env=env)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--put', action='append', default=[])
    ap.add_argument('--mem', type=int, default=2048)
    ap.add_argument('--smp', type=int, default=2)
    ap.add_argument('--timeout', type=int, default=120)
    ap.add_argument('--data-mb', type=int, default=1024)
    ap.add_argument('--keep')
    ap.add_argument('--img', default=os.path.join(ROOT, 'build', 'nova.img'))
    ap.add_argument('--wav')
    ap.add_argument('--net', action='store_true', help='a network card on QEMU user networking (the host is 10.0.2.2)')
    ap.add_argument('commands', nargs='*')
    a = ap.parse_args()

    work = a.keep or tempfile.mkdtemp(prefix='novarun')
    os.makedirs(work, exist_ok=True)
    data, serial, sock = (os.path.join(work, n) for n in ('data.img', 'serial.log', 'qmp.sock'))
    for p in (serial, sock):
        if os.path.exists(p):
            os.unlink(p)
    make_data(data, [p.split('=', 1) for p in a.put], a.data_mb)

    q = subprocess.Popen(['qemu-system-x86_64', '-machine', 'q35', '-cpu', 'qemu64,+rdtscp,+ssse3,+sse4.1,+sse4.2,+popcnt',
                          '-m', str(a.mem), '-smp', str(a.smp),
                          '-drive', f'if=pflash,format=raw,readonly=on,file={OVMF}',
                          '-drive', f'format=raw,file={a.img},snapshot=on',
                          '-drive', f'format=raw,file={data}',
                          '-serial', f'file:{serial}', '-vga', 'std', '-display', 'none', '-nic', 'user,model=e1000e' if a.net else 'none',
                          '-qmp', f'unix:{sock},server,nowait'] +
                         (['-audiodev', f'wav,id=snd0,path={os.path.abspath(a.wav)},out.frequency=48000',
                           '-device', 'intel-hda', '-device', 'hda-output,audiodev=snd0'] if a.wav else []))
    qmp = None
    try:
        sr = Serial(serial)
        out, ok = sr.wait('Entering kernel main loop', 300)
        if not ok:
            print(out[-3000:])
            sys.exit('NovaOS did not boot')
        qmp = Qmp(sock)
        time.sleep(2)
        qmp.key('meta_l')
        time.sleep(1)
        qmp.type('terminal\n')
        time.sleep(3)
        sr.read_new()
        qmp.type('serial on\n')
        sr.wait('[TERM-DONE]', 30)
        for c in a.commands:
            if c.startswith('!shot '):
                qmp.cmd('screendump', filename=os.path.abspath(os.path.join(work, c[6:].strip())), format='png')
                print(f'### screenshot {os.path.join(work, c[6:].strip())}', flush=True)
                continue
            if c.startswith('!wait '):
                time.sleep(float(c[6:]))
                continue
            if c.startswith('!keys '):
                for k in c[6:].split():
                    qmp.key(*k.split('-'))
                    time.sleep(0.1)
                continue
            sr.read_new()
            print(f'### {c}', flush=True)
            qmp.type(c + '\n')
            t0 = time.time()
            got, ok = sr.wait('[TERM-DONE]', a.timeout)
            if not ok:                      # Ctrl+C: the kernel logs where its threads are
                qmp.key('ctrl', 'c')
                more, _ = sr.wait('[TERM-DONE]', 20)
                got += more
            print(got.replace('\n[TERM-DONE]\n', '').rstrip(), flush=True)
            print(f'### {"done" if ok else "TIMEOUT"} in {time.time() - t0:.1f}s', flush=True)
        if a.keep:
            tail = sr.read_new()
            if tail.strip():
                print(tail)
    finally:
        if a.wav and qmp and q.poll() is None:   # quit cleanly: QEMU finishes the WAV header
            try:
                qmp.cmd('quit')
                q.wait(timeout=10)
            except Exception:
                pass
        q.kill()
        q.wait()
        if not a.keep:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    main()
