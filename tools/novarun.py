#!/usr/bin/env python3
"""Run commands in NovaOS's Terminal under QEMU and capture their output.

    tools/novarun.py [options] [--put HOSTDIR=C:\\Dir ...] COMMAND ...

Boots build/nova.img (a throwaway snapshot) with a fresh data disk holding
the --put trees, opens the Terminal (Win, "terminal", Enter), turns on
"serial on" and types each COMMAND, waiting for the Terminal's end-of-command
mark.  Prints each command's output.  A COMMAND of the form
"!shot NAME.png" saves a screenshot, "!wait N" waits N seconds and
"!keys a b ctrl-c" presses QEMU key names, "!click X Y" clicks at a
logical screen point (1280x800 at the default mode), "!type TEXT" types
without waiting (\\n Enter, \\e Esc), "!done N" waits up to N seconds
for the running command to end and "!reboot" restarts NovaOS
("shutdown /r": the data disk keeps drive C:) and opens the Terminal again.

Options: --mem MiB (2048), --smp N (2), --timeout S per command (120),
--keep DIR (keep the serial log, data disk and screenshots there),
--img PATH (the boot image), --wav PATH (an Intel HD Audio card whose
output QEMU records to PATH), --rec PATH (an Intel HD Audio card whose
microphone hears the WAV at PATH over and over, through a private
PulseAudio server; with --wav its output is recorded too), --net (a network card on QEMU user
networking; the host is 10.0.2.2), --monitors N (N - 1 more monitors, QEMU
secondary-vga devices; "!shot NAME.png" then also saves NAME-2.png, ...).

Other tools (tools/selftest.py) import the Nova class to drive a boot.
"""
import argparse, json, os, re, shlex, shutil, signal, socket, subprocess, sys, tempfile, time

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


def qemu_binary():
    """The QEMU to run: NOVARUN_QEMU, or qemu-system-x86_64 from PATH"""
    return os.environ.get('NOVARUN_QEMU', 'qemu-system-x86_64')


def display_args(args):
    """QEMU's -display: none, but a GPU with 3D (virtio-vga-gl, virtio-gpu-gl-pci)
    needs an OpenGL display; on a host without a screen that is SDL on Xvfb
    (DISPLAY set; NOVARUN_GL_DISPLAY overrides)"""
    if any(re.match(r'virtio-(?:vga|gpu)-gl', a) for a in args):
        return ['-display', os.environ.get('NOVARUN_GL_DISPLAY', 'sdl,gl=on')]
    return ['-display', 'none']


def accel_args():
    """KVM when /dev/kvm is usable, TCG otherwise.  NOVARUN_ACCEL=tcg|kvm forces one."""
    want = os.environ.get('NOVARUN_ACCEL', 'auto')
    if want == 'auto':
        want = 'kvm' if os.access('/dev/kvm', os.R_OK | os.W_OK) else 'tcg'
    return ['-accel', want]


class Nova:
    """One NovaOS boot in QEMU with its Terminal open and mirrored to serial"""

    def __init__(self, img=None, work=None, puts=(), mem=2048, smp=2, data_mb=1024, wav=None,
                 extra_args=(), boot_timeout=300, net=False, keep_data=False, vga=('-vga', 'std'), rec=None):
        self.work = work or tempfile.mkdtemp(prefix='novarun')
        # more monitors: QEMU display devices with an id (-device secondary-vga,id=head2), and
        # each further output of a virtio GPU with an id (-device virtio-vga,max_outputs=3,id=gpu)
        self.heads = [(m.group(1), 0) for a in extra_args
                      for m in [re.match(r'(?:secondary-vga|bochs-display),(?:.*,)?id=([\w-]+)', a)] if m]
        for a in extra_args:
            m, n = re.match(r'virtio-(?:vga|gpu-pci),(?:.*,)?id=([\w-]+)', a), re.search(r'max_outputs=(\d+)', a)
            if m and n:
                self.heads += [(m.group(1), h) for h in range(1, int(n.group(1)))]
        os.makedirs(self.work, exist_ok=True)
        data, self.serial_path, sock = (os.path.join(self.work, n) for n in ('data.img', 'serial.log', 'qmp.sock'))
        for p in (self.serial_path, sock):
            if os.path.exists(p):
                os.unlink(p)
        if not (keep_data and os.path.exists(data)):     # keep_data: the drive C: an earlier boot saved
            make_data(data, puts, data_mb)
        self.wav = wav
        self.rec = rec
        self.extra = list(extra_args)
        self.qmp = None
        env = None
        if rec:
            audio, env = self.pulse_input(rec)
        elif wav:
            audio = ['-audiodev', f'wav,id=snd0,path={os.path.abspath(wav)},out.frequency=48000',
                     '-device', 'intel-hda', '-device', 'hda-output,audiodev=snd0']
        else:
            audio = []
        self.q = subprocess.Popen([qemu_binary(), '-machine', 'q35'] + accel_args() + ['-cpu', 'qemu64,+rdtscp,+ssse3,+sse4.1,+sse4.2,+popcnt',
                                   '-m', str(mem), '-smp', str(smp),
                                   '-drive', f'if=pflash,format=raw,readonly=on,file={OVMF}',
                                   '-drive', f'format=raw,file={img or os.path.join(ROOT, "build", "nova.img")},snapshot=on',
                                   '-drive', f'format=raw,file={data}',
                                   '-serial', f'file:{self.serial_path}'] + list(vga) + display_args(list(vga) + list(extra_args)) + [
                                   '-nic', 'user,model=e1000e' if net else 'none',
                                   '-qmp', f'unix:{sock},server,nowait'] +
                                  audio +
                                  (['-s'] if os.environ.get('NOVARUN_GDB') else []) +   # gdb server on :1234
                                  list(extra_args), env=env)
        try:
            self.sr = Serial(self.serial_path)
            self.sock = sock
            self.start(boot_timeout)
        except BaseException:
            self.close()
            raise

    def start(self, boot_timeout=300):
        """Wait for the desktop, then open the Terminal mirrored to serial"""
        out, ok = self.sr.wait('Entering kernel main loop', boot_timeout)
        self.boot_log = out
        if not ok:
            raise RuntimeError('NovaOS did not boot:\n' + out[-3000:])
        if not self.qmp:
            self.qmp = Qmp(self.sock)
        time.sleep(2)
        self.qmp.key('meta_l')
        time.sleep(1)
        self.qmp.type('terminal\n')
        time.sleep(3)
        self.sr.read_new()
        self.qmp.type('serial on\n')
        self.sr.wait('[TERM-DONE]', 30)

    def reboot(self, boot_timeout=300):
        """Restart NovaOS (shutdown /r) and open the Terminal again; the
        boot's log (up to the desktop) is returned"""
        self.sr.read_new()
        self.qmp.type('shutdown /r\n')
        self.start(boot_timeout)
        return self.boot_log

    def run(self, cmd, timeout=120, shot=None, acts=()):
        """Type @cmd into the Terminal; returns (serial output, finished in time).
        @shot = (regex, path): a screenshot 2 s after the output matches
        regex (while the program is still drawing).  @acts: (regex, function)
        pairs; each function is called with this Nova once the output
        matches its regex (a program asking the test to do something)"""
        self.sr.read_new()
        self.qmp.type(cmd + '\n')
        got, ok, end = '', False, time.time() + timeout
        pending = list(acts)
        pat = re.compile(shot[0]) if shot else None
        while (pending or pat) and time.time() < end and '[TERM-DONE]' not in got:
            time.sleep(0.25)
            got += self.sr.read_new()
            if pat and pat.search(got) and '[TERM-DONE]' not in got:
                time.sleep(2)
                self.shot(shot[1])
                pat = None
            for a in [a for a in pending if re.search(a[0], got)]:
                pending.remove(a)
                a[1](self)
        ok = '[TERM-DONE]' in got
        if not ok:
            more, ok = self.sr.wait('[TERM-DONE]', max(1, end - time.time()))
            got += more
            ok = '[TERM-DONE]' in got
        if not ok:                          # Ctrl+C: the kernel logs where its threads are
            self.qmp.key('ctrl', 'c')
            more, _ = self.sr.wait('[TERM-DONE]', 20)
            got += more
        return got.replace('\n[TERM-DONE]\n', '').rstrip(), ok

    def pulse_input(self, rec):
        """QEMU arguments for an hda-micro card on a private PulseAudio
        server: its microphone hears the null sink "novain", which paplay
        keeps playing @rec into, and its output goes to the null sink
        "novaout", whose monitor parecord saves (for --wav).  Null sinks run
        on a clock, so the guest records and plays in real time."""
        d = os.path.join(self.work, 'pulse')
        os.makedirs(d, exist_ok=True)
        sock = os.path.join(d, 'sock')
        env = dict(os.environ, PULSE_RUNTIME_PATH=d, HOME=d, PULSE_SERVER='unix:' + sock)
        self.pulse = [subprocess.Popen(
            ['pulseaudio', '-n', '--daemonize=no', '--exit-idle-time=-1', '--disallow-exit', '--use-pid-file=no',
             '-L', f'module-native-protocol-unix socket={sock} auth-anonymous=1',
             '-L', 'module-null-sink sink_name=novain rate=48000 channels=2',
             '-L', 'module-null-sink sink_name=novaout rate=48000 channels=2'],
            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)]
        for _ in range(100):
            if subprocess.run(['pactl', 'info'], env=env, capture_output=True).returncode == 0:
                break
            time.sleep(0.1)
        else:
            raise RuntimeError('PulseAudio did not start (is it installed?)')
        self.pulse.append(subprocess.Popen(['sh', '-c', 'while paplay -d novain "$0"; do :; done', os.path.abspath(rec)],
                                           env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                           start_new_session=True))
        if self.wav:
            self.pulse.append(subprocess.Popen(['parecord', '-d', 'novaout.monitor', '--rate=48000', '--channels=2',
                                                '--format=s16le', '--file-format=wav', os.path.abspath(self.wav)],
                                               env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                               start_new_session=True))
        return (['-audiodev', f'pa,id=snd0,server=unix:{sock},in.name=novain.monitor,out.name=novaout,'
                 'in.frequency=48000,out.frequency=48000',
                 '-device', 'intel-hda', '-device', 'hda-micro,audiodev=snd0'], None)

    def stop_pulse(self):
        for p in reversed(getattr(self, 'pulse', [])):     # parecord first: SIGINT finishes its WAV
            try:
                os.killpg(p.pid, signal.SIGINT)
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
            except ProcessLookupError:
                pass
        self.pulse = []

    def shot(self, path):
        """A screenshot of the primary display at @path; with more monitors,
        each further one's at PATH-2.png, PATH-3.png, ... (an output with no
        monitor on it has none)"""
        r = self.qmp.cmd('screendump', filename=os.path.abspath(path), format='png')
        if 'error' in r:
            raise RuntimeError(f"screendump: {r['error'].get('desc', r['error'])}")
        for i, (dev, head) in enumerate(self.heads):
            self.qmp.cmd('screendump', filename=os.path.abspath(re.sub(r'(\.png)?$', f'-{i + 2}.png', path, count=1)),
                         format='png', device=dev, **({'head': head} if head else {}))

    def hmp(self, line):
        """A QEMU monitor command (e.g. "o /b 0xe8 1": write an I/O port)"""
        return self.qmp.cmd('human-monitor-command', **{'command-line': line}).get('return', '')

    def status(self):
        return self.qmp.cmd('query-status').get('return', {}).get('status')

    def move_to(self, x, y):
        """Move the mouse to logical screen point (x, y): HMP relative moves from the top-left
        corner, 40 pixels at a time (QMP input-send-event moves do nothing on this mouse)"""
        hmp = lambda c: self.qmp.cmd('human-monitor-command', **{'command-line': c})
        for _ in range(40):
            hmp('mouse_move -100 -100')
            time.sleep(0.01)
        while x > 0 or y > 0:
            dx, dy = min(x, 40), min(y, 40)
            hmp(f'mouse_move {dx} {dy}')
            time.sleep(0.02)
            x -= dx
            y -= dy
        time.sleep(0.3)

    def click(self, x, y, button=1):
        """Click at logical screen point (x, y)"""
        hmp = lambda c: self.qmp.cmd('human-monitor-command', **{'command-line': c})
        self.move_to(x, y)
        hmp(f'mouse_button {button}')
        time.sleep(0.1)
        hmp('mouse_button 0')
        time.sleep(0.3)

    def keys(self, names):
        for k in names.split():
            self.qmp.key(*k.split('-'))
            time.sleep(0.1)

    def close(self, keep=True):
        if (self.wav or self.rec or '-audiodev' in self.extra) and self.qmp and self.q.poll() is None:
            # quit cleanly: QEMU finishes the WAV header
            try:
                self.qmp.cmd('quit')
                self.q.wait(timeout=10)
            except Exception:
                pass
        self.q.kill()
        self.q.wait()
        self.stop_pulse()
        if not keep:
            shutil.rmtree(self.work, ignore_errors=True)


def vga_args(name):
    """QEMU arguments for the display adapter @name: a -vga type, or a -device"""
    if name in ('std', 'cirrus', 'vmware', 'qxl', 'virtio', 'none'):
        return ('-vga', name)
    return ('-vga', 'none', '-device', name)


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
    ap.add_argument('--rec', help='a WAV the guest\'s microphone hears (needs PulseAudio)')
    ap.add_argument('--net', action='store_true', help='a network card on QEMU user networking (the host is 10.0.2.2)')
    ap.add_argument('--display', default='std',
                    help='the display adapter: a -vga name (std, cirrus, vmware, qxl, virtio) or a -device name (bochs-display)')
    ap.add_argument('--extra', action='append', default=[], help='more QEMU arguments (split like a shell)')
    ap.add_argument('--monitors', type=int, default=1,
                    help='monitors: each one past the first is a QEMU secondary-vga (screenshots save one PNG per monitor)')
    ap.add_argument('commands', nargs='*')
    a = ap.parse_args()

    try:
        nova = Nova(a.img, a.keep, [p.split('=', 1) for p in a.put], a.mem, a.smp, a.data_mb, a.wav, net=a.net,
                    vga=vga_args(a.display), rec=a.rec, extra_args=[x for e in a.extra for x in shlex.split(e)] +
                    [x for i in range(2, a.monitors + 1) for x in ('-device', f'secondary-vga,id=head{i}')])
    except RuntimeError as e:
        sys.exit(str(e))
    try:
        for c in a.commands:
            if c.startswith('!shot '):
                nova.shot(os.path.join(nova.work, c[6:].strip()))
                print(f'### screenshot {os.path.join(nova.work, c[6:].strip())}', flush=True)
                continue
            if c.startswith('!wait '):
                time.sleep(float(c[6:]))
                continue
            if c.startswith('!type '):      # type without waiting (\n, \e: Enter, Esc)
                text = c[6:].replace('\\n', '\n')
                for i, part in enumerate(text.split('\\e')):
                    if i:
                        nova.qmp.key('esc')
                        time.sleep(0.1)
                    nova.qmp.type(part)
                continue
            if c.startswith('!done '):      # wait for the running command to end
                t0 = time.time()
                got, ok = nova.sr.wait('[TERM-DONE]', float(c[6:]))
                print(got.replace('\n[TERM-DONE]\n', '').rstrip(), flush=True)
                print(f'### {"done" if ok else "TIMEOUT"} in {time.time() - t0:.1f}s', flush=True)
                continue
            if c.startswith('!click '):     # !click X Y: left click at a logical screen point
                x, y = (int(v) for v in c[7:].split()[:2])
                nova.click(x, y)
                continue
            if c.strip() == '!reboot':
                print('### !reboot', flush=True)
                print(nova.reboot(), flush=True)
                continue
            if c.startswith('!keys '):
                nova.keys(c[6:])
                continue
            if c.startswith('!bg '):       # type a command and leave it running
                nova.sr.read_new()
                print(f'### (running) {c[4:]}', flush=True)
                nova.qmp.type(c[4:] + '\n')
                continue
            print(f'### {c}', flush=True)
            t0 = time.time()
            got, ok = nova.run(c, a.timeout)
            print(got, flush=True)
            print(f'### {"done" if ok else "TIMEOUT"} in {time.time() - t0:.1f}s', flush=True)
        if a.keep:
            tail = nova.sr.read_new()
            if tail.strip():
                print(tail)
    finally:
        nova.close(keep=bool(a.keep))


if __name__ == '__main__':
    main()
