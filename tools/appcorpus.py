#!/usr/bin/env python3
"""Run unmodified Windows programs on NovaOS and say which still work.

    tools/appcorpus.py [--img build/nova.img] [--cache DIR] [--out DIR]
                       [--only NAME,...] [--summary FILE] [--update-reference]

The programs are tests/appcorpus/*.py, one file each, run in file-name
order.  Downloads each program's official Windows x64 release into --cache (kept
between runs), unpacks it onto drive C: (C:\\Apps\\NAME) with a few sample
files, boots NovaOS once and types each program's commands into the
Terminal (tools/novarun.py's Nova class).  A command passes as a self-test
does (tools/selftest.py): it exits with code 0 and prints what is expected.
Before it, NovaOS's own screens are checked (Phase 17.6): `dir` on drives
C: and D: (an empty NTFS disk made with mkntfs, from the ntfs-3g package)
must name each drive and give its own free space, and File Explorer's This
PC must list both drives.  The windowed programs (App(gui=True)) run last,
one at a time (each takes the keyboard): SumatraPDF opens a PDF, WinMerge
compares two files, KeePassXC unlocks a password database, Firefox is
installed by the App Store from Mozilla's installer and loads a page from
an HTTPS server this script runs on the host, Notepad++ opens a file and
PuTTY makes a raw connection
to an echo server this script runs on the host (10.0.2.2 on QEMU's user
network) and types a line, which the server must receive.  Each one's
screenshot (and This PC's) must match tests/reference/NAME.png
(--update-reference writes those files from this run instead); the
screenshots are kept in --out.  When a program needs sound (App(mic=True)
hears a 523 Hz tone on the microphone, App(sound=(hz, ms)) must play that
tone for that long), NovaOS boots with a sound card on a private
PulseAudio server (as tools/selftest.py's core suite does) and what it
played is kept in --out/sound.wav and checked after the run; without
pulseaudio those programs are skipped, which is not a failure.

The exit status is the number of programs that failed.  --summary appends
a Markdown pass/fail table, one row per program (the nightly workflow,
.github/workflows/nightly.yml, posts it).
"""
import argparse, http.server, math, os, re, shutil, socket, ssl, struct, subprocess, sys, tempfile, threading, time, zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from novarun import Nova, ROOT
from selftest import Test, verdict, store_verdict, PANIC, REC_HZ
import wavcheck

REFERENCES = os.path.join(ROOT, 'tests', 'reference')
DRIVE_LABEL = 'NOVACORPUS'
ANSI = re.compile(r'\x1b\[[0-9;]*m')
SAMPLE = 'NovaOS app corpus\na needle in a haystack\nthe end\n'
ECHO_PORT = 2323                    # the echo server PuTTY connects to (on the host)
HTTPS_PORT = 8443                   # the HTTPS server Firefox loads a page from (on the host)
STORE_C = os.path.join(ROOT, 'kernel', 'apps', 'store.c')
SEVENZIP = 'https://github.com/ip7z/7zip/releases/download/26.03/7z2603-x64.exe'   # unpacks App Store archives


class App:
    """@url's download (and @extra's), unpacked by @unpack into
    C:\\Apps\\@dir; @tests run in order.  @unpack may be a function
    (app, [downloaded files], dest) for a program its file stages itself.
    A windowed program (@gui) is started with each test's command, has its
    test's timeout to bring its window up, and its screenshot is compared
    with tests/reference/NAME.png; @interact(nova, echo) runs before the
    screenshot (typing into it) and returns why it failed, or None.  A
    windowed program's tests whose command does not start a program run in
    the Terminal after its window closed.  @net gives NovaOS QEMU's user
    network and starts the echo server; @https also starts the HTTPS
    server (https://10.0.2.2:8443/ in NovaOS) and sets app.ca to its CA
    certificate's file before @unpack runs.  @mic: the program hears a tone
    of REC_HZ (523 Hz) on the microphone; @sound=(hz, ms): it must play a
    tone of @hz for @ms, checked in the sound NovaOS played once the run
    ends.

    @store: the program's name in the App Store's catalog
    (kernel/apps/store.c), whose download @url must be.  The download is
    put in C:\\Downloads under the catalog's file name and 7-Zip in
    C:\\Programs\\7-Zip, so a test with Test(store=NAME) installs it with
    the Store's own button and no network; @unpack (a function) then gets
    the folder that becomes C:\\Programs, for settings the program reads.
    @processes: a windowed program of many processes of one executable
    (Firefox: a launcher that exits once the browser is up, child
    processes the browser ends itself), so only a crash fails it before
    the screenshot."""
    def __init__(self, name, version, url, dir, tests, unpack='zip', strip=0, extra=(),
                 gui=False, net=False, interact=None, https=False, store=None, processes=False,
                 mic=False, sound=None):
        self.name, self.version, self.url, self.dir, self.tests = name, version, url, dir, tests
        self.unpack, self.strip, self.extra = unpack, strip, list(extra)
        self.gui, self.net, self.interact = gui, net, interact
        self.https, self.store, self.ca, self.processes = https, store, None, processes
        self.mic, self.sound = mic, sound


A = r'C:\Apps'


def load_apps():
    """The programs in tests/appcorpus/*.py, in file-name order.  Each file
    defines APP (an App; App, Test, A and DRIVE_LABEL are given to it) and
    DOC, its name in README's list (tools/docgen.py).  One file per
    program, so changes adding programs add files instead of editing a
    shared list."""
    import glob
    apps = []
    for f in sorted(glob.glob(os.path.join(ROOT, 'tests', 'appcorpus', '*.py'))):
        ns = {'App': App, 'Test': Test, 'A': A, 'DRIVE_LABEL': DRIVE_LABEL, 'ROOT': ROOT, 'ECHO_PORT': ECHO_PORT,
              'HTTPS_PORT': HTTPS_PORT, '__file__': f}
        exec(compile(open(f).read(), f, 'exec'), ns)
        if not isinstance(ns.get('APP'), App):
            sys.exit(f'{f}: APP must be an App')
        apps.append(ns['APP'])
    return apps


APPS = load_apps()


def fetch(url, cache):
    if not url:
        return None
    f = os.path.join(cache, os.path.basename(url))
    if not os.path.exists(f) or not os.path.getsize(f):
        subprocess.run(['curl', '-sSLf', '--retry', '4', '-o', f + '.part', url], check=True)
        os.replace(f + '.part', f)
    return f


def stage(app, archive, dest):
    """Unpack @archive into @dest, dropping @app.strip leading folders (or
    keeping only the folder named @app.strip)"""
    if app.unpack is None:                        # nothing to download (NovaOS's own screens)
        return
    if app.unpack == 'exe':
        os.makedirs(dest)
        shutil.copy(archive, os.path.join(dest, app.dir + '.exe'))
        return
    if app.unpack == '7z':
        subprocess.run(['7z', 'x', '-y', f'-o{dest}', archive], check=True, stdout=subprocess.DEVNULL)
        return
    with zipfile.ZipFile(archive) as z:
        for m in z.infolist():
            parts = m.filename.split('/')
            if isinstance(app.strip, str):
                if parts[0] != app.strip:
                    continue
                parts = parts[1:]
            else:
                parts = parts[app.strip:]
            if not parts or not parts[-1]:
                continue
            path = os.path.join(dest, *parts)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with z.open(m) as src, open(path, 'wb') as out:
                shutil.copyfileobj(src, out)


def catalog_file(app):
    """The file name the App Store saves @app.store's download as; its
    catalog entry must have @app.url as the download"""
    m = re.search(r'\{ "' + re.escape(app.store) + r'", "[^"]*", "[^"]*",\s*CAT_\w+, (GH )?"([^"]+)", "([^"]+)"',
                  open(STORE_C).read())
    if not m or ('https://github.com/' if m.group(1) else '') + m.group(2) != app.url:
        raise RuntimeError(f'the App Store has no "{app.store}" downloading {app.url}')
    return m.group(3)


def stage_store(app, files, work):
    """@app's download in Downloads (C:\\Downloads) and 7-Zip in
    Programs\\7-Zip, so the Store can install it without a network"""
    downloads, programs = os.path.join(work, 'Downloads'), os.path.join(work, 'Programs')
    os.makedirs(downloads, exist_ok=True)
    shutil.copy(files[0], os.path.join(downloads, catalog_file(app)))
    if not os.path.exists(os.path.join(programs, '7-Zip')):
        subprocess.run(['7z', 'x', '-y', '-o' + os.path.join(programs, '7-Zip'), files[-1]],
                       check=True, stdout=subprocess.DEVNULL)
    if callable(app.unpack):
        app.unpack(app, files[:-1], programs)


class HttpsServer:
    """What Firefox loads: a page over HTTPS, with a certificate for
    10.0.2.2 from a test CA made for this run (openssl); .ca is the CA's
    certificate (PEM) and .requests the paths asked for"""
    PAGE = ('<!DOCTYPE html>\n<html><head><meta charset="utf-8"><title>NovaOS app corpus</title></head>\n'
            '<body style="font-family: sans-serif; background: #f4f6fb">\n'
            '<h1 style="color: #2a5db0">Hello from NovaOS</h1>\n'
            "<p>This page came over <b>HTTPS</b> from the app corpus's test server.</p>\n"
            '<ul><li>a needle in a haystack</li><li>the end</li></ul>\n</body></html>\n')

    def __init__(self, work, port=HTTPS_PORT):
        d = os.path.join(work, 'pki')
        os.makedirs(d)
        self.ca, key, csr, cert = (os.path.join(d, n) for n in ('ca.pem', 'ca.key', 'srv.csr', 'srv.pem'))
        skey, ext = os.path.join(d, 'srv.key'), os.path.join(d, 'ext')
        ssl_cmd = lambda *a: subprocess.run(['openssl', *a], check=True, stdout=subprocess.DEVNULL,
                                            stderr=subprocess.DEVNULL)
        ssl_cmd('req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', key, '-out', self.ca, '-days', '30',
                '-subj', '/CN=NovaOS app corpus test CA', '-addext', 'basicConstraints=critical,CA:TRUE',
                '-addext', 'keyUsage=critical,keyCertSign,cRLSign')
        ssl_cmd('req', '-newkey', 'rsa:2048', '-nodes', '-keyout', skey, '-out', csr, '-subj', '/CN=10.0.2.2')
        with open(ext, 'w') as f:
            f.write('subjectAltName=IP:10.0.2.2\nbasicConstraints=CA:FALSE\nextendedKeyUsage=serverAuth\n')
        ssl_cmd('x509', '-req', '-in', csr, '-CA', self.ca, '-CAkey', key, '-CAcreateserial', '-out', cert,
                '-days', '30', '-extfile', ext)
        self.requests = []
        server = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                server.requests.append(self.path)
                body = (server.PAGE if self.path == '/' else 'not found\n').encode()
                self.send_response(200 if self.path == '/' else 404)
                self.send_header('Content-Type', 'text/html; charset=utf-8')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *a):
                pass

        self.httpd = http.server.ThreadingHTTPServer(('0.0.0.0', port), Handler)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(cert, skey)
        self.httpd.socket = ctx.wrap_socket(self.httpd.socket, server_side=True)
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


def make_pdf(path, lines):
    """A one-page PDF with @lines in Helvetica (for SumatraPDF to show)"""
    text = 'BT /F1 24 Tf 72 720 Td 32 TL ' + ' '.join('(' + l.replace('\\', '\\\\').replace('(', '\\(').replace(')', '\\)') + ') Tj T*'
                                                      for l in lines) + ' ET'
    objs = ['<< /Type /Catalog /Pages 2 0 R >>',
            '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
            '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>',
            f'<< /Length {len(text)} >>\nstream\n{text}\nendstream',
            '<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>']
    out, offs = '%PDF-1.4\n', []
    for i, o in enumerate(objs):
        offs.append(len(out))
        out += f'{i + 1} 0 obj\n{o}\nendobj\n'
    xref = len(out)
    out += f'xref\n0 {len(objs) + 1}\n0000000000 65535 f \n' + ''.join(f'{o:010d} 00000 n \n' for o in offs)
    out += f'trailer\n<< /Size {len(objs) + 1} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n'
    with open(path, 'w', newline='\n') as f:
        f.write(out)


class EchoServer:
    """What PuTTY talks to: greets each connection and echoes its lines;
    .lines collects what was received"""
    def __init__(self, port=ECHO_PORT):
        self.lines = []
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(('0.0.0.0', port))
        self.sock.listen(5)
        threading.Thread(target=self.accept, daemon=True).start()

    def accept(self):
        while True:
            try:
                c, _ = self.sock.accept()
            except OSError:
                return
            threading.Thread(target=self.serve, args=(c,), daemon=True).start()

    def serve(self, c):
        try:
            c.sendall(b'Welcome to the NovaOS echo server\r\nType a line and it comes back.\r\n')
            buf = b''
            while True:
                d = c.recv(256)
                if not d:
                    break
                buf += d
                while b'\r' in buf or b'\n' in buf:
                    i = min(x for x in (buf.find(b'\r'), buf.find(b'\n')) if x >= 0)
                    line, buf = buf[:i], buf[i + 1:].lstrip(b'\r\n')
                    self.lines.append(line.decode('latin-1'))
                    c.sendall(b'echo: ' + line + b'\r\n')
        except OSError:
            pass
        c.close()

    def close(self):
        self.sock.close()


def make_data(d):
    """C:\\Apps\\data: text to search, a PDF, two files to compare and a git
    repository to clone"""
    os.makedirs(d)
    with open(os.path.join(d, 'hello.txt'), 'w', newline='\n') as f:
        f.write(SAMPLE)
    with open(os.path.join(d, 'hello2.txt'), 'w', newline='\n') as f:
        f.write(SAMPLE.replace('a needle', 'no needle') + 'one more line\n')
    make_pdf(os.path.join(d, 'corpus.pdf'), SAMPLE.splitlines())
    with open(os.path.join(d, 'notes.txt'), 'w', newline='\n') as f:
        f.write('more text\n')
    with open(os.path.join(d, 'ab.json'), 'w') as f:
        f.write('{"a": 40, "b": 2}\n')
    work = tempfile.mkdtemp(prefix='corpus-git')
    env = dict(os.environ, GIT_AUTHOR_NAME='NovaOS', GIT_AUTHOR_EMAIL='ci@novaos.invalid',
               GIT_COMMITTER_NAME='NovaOS', GIT_COMMITTER_EMAIL='ci@novaos.invalid',
               GIT_AUTHOR_DATE='2026-01-01T00:00:00Z', GIT_COMMITTER_DATE='2026-01-01T00:00:00Z')
    git = lambda *a: subprocess.run(['git', '-C', work, *a], check=True, env=env, stdout=subprocess.DEVNULL)
    git('init', '-q', '-b', 'main')
    shutil.copy(os.path.join(d, 'hello.txt'), work)
    git('add', '.')
    git('commit', '-q', '-m', 'First commit')
    shutil.copy(os.path.join(d, 'notes.txt'), work)
    git('add', '.')
    git('commit', '-q', '-m', 'Add the corpus notes')
    subprocess.run(['git', 'clone', '-q', '--bare', work, os.path.join(d, 'src.git')], check=True)
    shutil.rmtree(work)


def make_ntfs(path, mb=128):
    """A disk with one empty NTFS partition from 1 MiB (drive D:), or None
    without mkntfs"""
    mkntfs = shutil.which('mkntfs') or ('/usr/sbin/mkntfs' if os.path.exists('/usr/sbin/mkntfs') else None)
    if not mkntfs:
        return None
    vol = path + '.vol'
    with open(vol, 'wb') as f:
        f.truncate((mb - 1) << 20)
    subprocess.run([mkntfs, '-F', '-Q', '-L', DRIVE_LABEL, '-p', '2048', '-H', '255', '-S', '63', vol],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    mbr = bytearray(512)
    mbr[446:462] = struct.pack('<B3sB3sII', 0, b'\xfe\xff\xff', 0x07, b'\xfe\xff\xff', 2048, (mb - 1) * 2048)
    mbr[510:512] = b'\x55\xaa'
    with open(path, 'wb') as out, open(vol, 'rb') as src:
        out.write(mbr)
        out.write(bytes((1 << 20) - 512))
        shutil.copyfileobj(src, out)
    os.unlink(vol)
    return path


def compare(shot, ref, size=(640, 400), level=48):
    """The share of pixels (both images scaled to @size) whose colour
    differs from the reference by more than @level in some channel"""
    from PIL import Image, ImageChops
    a = Image.open(shot).convert('RGB').resize(size, Image.BOX)
    b = Image.open(ref).convert('RGB').resize(size, Image.BOX)
    diff = ImageChops.difference(a, b).convert('L').point(lambda v: 255 if v > level else 0)
    return diff.histogram()[255] / (size[0] * size[1])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--img', default=os.path.join(ROOT, 'build', 'nova.img'))
    ap.add_argument('--cache', default=os.path.join(ROOT, 'build', 'appcorpus-cache'))
    ap.add_argument('--out', default='appcorpus-out')
    ap.add_argument('--only')
    ap.add_argument('--summary')
    ap.add_argument('--update-reference', action='store_true')
    ap.add_argument('--max-diff', type=float, default=0.03, help='share of differing pixels allowed')
    a = ap.parse_args()

    apps = [x for x in APPS if not a.only or x.name in a.only.split(',')]
    os.makedirs(a.out, exist_ok=True)
    os.makedirs(a.cache, exist_ok=True)
    work = tempfile.mkdtemp(prefix='appcorpus')
    apps_dir = os.path.join(work, 'Apps')
    results = {}                    # app name -> (why it failed or None, seconds, [(test, why)])
    staged = []
    https = HttpsServer(work) if any(x.https for x in apps) else None
    for app in apps:
        app.ca = https and https.ca
        try:
            if app.store:
                stage_store(app, [fetch(u, a.cache) for u in [app.url] + app.extra + [SEVENZIP]], work)
            elif callable(app.unpack):
                app.unpack(app, [fetch(u, a.cache) for u in [app.url] + app.extra],
                           os.path.join(apps_dir, app.dir))
            else:
                stage(app, fetch(app.url, a.cache), os.path.join(apps_dir, app.dir))
            staged.append(app)
        except Exception as e:
            results[app.name] = (f'download or unpack failed: {e}', 0, [])
            print(f'FAIL  {app.name:10s} {results[app.name][0]}', flush=True)
    make_data(os.path.join(apps_dir, 'data'))

    ntfs = make_ntfs(os.path.join(work, 'ntfs.img')) if any(x.name == 'NovaOS' for x in staged) else None
    echo = EchoServer() if any(x.net for x in staged) else None
    rec = wav = None
    if any(x.mic or x.sound for x in staged):
        if shutil.which('pulseaudio'):
            rec = make_tone(os.path.join(work, 'mic.wav'))
            wav = os.path.join(a.out, 'sound.wav')
        else:
            for app in [x for x in staged if x.mic or x.sound]:
                results[app.name] = (SKIPPED + ': pulseaudio is not installed', 0, [])
                print(f'SKIP  {app.name:10s} {results[app.name][0]}', flush=True)
            staged = [x for x in staged if not (x.mic or x.sound)]
    puts = [(apps_dir, A)] + [(os.path.join(work, d), 'C:\\' + d) for d in ('Downloads', 'Programs')
                              if os.path.isdir(os.path.join(work, d))]
    t_boot = time.time()
    try:
        nova = Nova(a.img, os.path.join(work, 'boot'), puts, mem=4096, data_mb=2048,
                    extra_args=['-drive', f'format=raw,file={ntfs}'] if ntfs else [],
                    net=echo is not None or https is not None, rec=rec, wav=wav)
    except RuntimeError as e:
        print(e)
        for app in staged:
            results[app.name] = ('NovaOS did not boot', 0, [])
        report(a, apps, results)
        return len(apps)
    print(f'booted in {time.time() - t_boot:.0f} s', flush=True)
    log = nova.boot_log
    stopped = None
    try:
        for app in staged:
            if stopped:
                results[app.name] = (stopped, 0, [])
                continue
            t0, steps, why = time.time(), [], None
            for t in app.tests:
                ts = time.time()
                if t.store:
                    out, _ = nova.run(t.cmd, 30)
                    w, out = store_verdict(nova, t, out)
                elif app.gui and t.cmd.startswith('start '):
                    out, w = gui(nova, t, a, app, echo, close=app is not staged[-1] or len(app.tests) > 1)
                elif app.name == 'NovaOS':
                    out, w = screen(nova, t, a, ntfs)
                else:
                    out, ok = nova.run(t.cmd, t.timeout)
                    out = ANSI.sub('', out)          # rg and fd colour their output in a console
                    exe = re.split(r'[\\/]', t.cmd.split()[0])[-1]
                    w = verdict(t, out, ok, exe)
                if PANIC.search(out):
                    w = stopped = 'kernel panic'
                log += out
                steps.append((t.name, w))
                print(f'  {"ok  " if not w else "FAIL"}  {t.name:14s} {time.time() - ts:6.1f} s  {w or ""}', flush=True)
                if w:
                    print('    ' + '\n    '.join(l for l in out.splitlines() if not l.startswith('[SCHED]'))[-3000:])
                    why = why or f'{t.name}: {w}'
                    break
            results[app.name] = (why, time.time() - t0, steps)
            print(f'{"PASS" if not why else "FAIL"}  {app.name:10s} {time.time() - t0:6.1f} s', flush=True)
            if nova.q.poll() is not None:
                stopped = 'not run (NovaOS stopped)'
    finally:
        nova.close()
        if echo:
            echo.close()
        if https:
            https.close()
        with open(os.path.join(a.out, 'serial.log'), 'w') as f:
            f.write(log + nova.sr.read_new())
        shutil.rmtree(work, ignore_errors=True)
    for app in staged:                              # the sound NovaOS played, now that the file is complete
        why, secs, steps = results[app.name]
        if app.sound and not why:
            hz, ms = app.sound
            if not os.path.exists(wav) or not wavcheck.has_tone(wav, hz, ms):
                results[app.name] = (f'no {hz} Hz tone of {ms} ms in the sound NovaOS played', secs,
                                     steps + [('sound', 'no tone')])
                print(f'FAIL  {app.name:10s} {results[app.name][0]}', flush=True)
            else:
                results[app.name] = (why, secs, steps + [('sound', None)])
    report(a, apps, results)
    return sum(1 for r in results.values() if r[0] and not r[0].startswith(SKIPPED))


SKIPPED = 'skipped'


def make_tone(path):
    """A 10 s WAV of the microphone's tone (REC_HZ), as tools/selftest.py makes"""
    import wave
    with wave.open(path, 'wb') as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(48000)
        w.writeframes(b''.join(struct.pack('<hh', v, v) for v in
                               (int(12000 * math.sin(2 * math.pi * REC_HZ * i / 48000)) for i in range(48000 * 10))))
    return path


def gui(nova, t, a, app, echo, close):
    """Start a windowed program (t.timeout seconds for its window to come
    up, or it dies), run its interaction (PuTTY types a line the echo
    server must receive).  The screenshot must match the reference; @close
    closes the window after it (Alt+F4) so the next program gets the
    keyboard"""
    out, ok = nova.run(t.cmd, 30)
    exe = re.escape(re.search(r'([^\\/" ]+\.exe)', t.cmd, re.I).group(1))   # paths may be quoted
    m = None
    for _ in range(t.timeout):
        time.sleep(1)
        out += nova.sr.read_new()
        ends = 'crashed' if app.processes else '(exited|crashed|terminated)'
        m = re.search(rf'\[UM\] {exe} \(PID \d+\) {ends}[^\n]*', out, re.I)
        if m:
            break
    if m:
        return out, m.group(0).split(') ', 1)[1]
    w = app.interact(nova, echo) if app.interact else None
    time.sleep(3)
    w = check_shot(nova, a, app.name.lower() + '.png') or w
    if close:
        nova.keys('alt-f4')
        time.sleep(3)
    return out, w


def check_shot(nova, a, name):
    """Screenshot @name into --out; why it differs from its reference, or None"""
    shot, ref = os.path.join(a.out, name), os.path.join(REFERENCES, name)
    nova.shot(shot)
    time.sleep(1)
    if a.update_reference:
        os.makedirs(REFERENCES, exist_ok=True)
        from PIL import Image
        Image.open(shot).convert('RGB').resize((1280, 800), Image.BOX).save(ref, optimize=True)
        return None
    if not os.path.exists(ref):
        return 'no reference screenshot (run with --update-reference)'
    d = compare(shot, ref)
    print(f'  {name} differs from the reference in {d:.1%} of pixels', flush=True)
    return None if d <= a.max_diff else f'screenshot differs from the reference in {d:.1%} of pixels'


def screen(nova, t, a, ntfs):
    """NovaOS's own screens: `dir` names each drive and gives its own free
    space; File Explorer's This PC lists the drives"""
    if t.name == 'This PC':
        out, ok = nova.run(t.cmd, 30)
        if not ok:
            return out, 'did not start'
        time.sleep(4)                               # the window opens and draws
        w = check_shot(nova, a, 'this-pc.png')
        nova.keys('alt-f4')                         # back to the Terminal
        time.sleep(2)
        return out, w
    if t.name == 'dir D:' and not ntfs:
        return '', 'no drive D: (mkntfs, from the ntfs-3g package, is not installed)'
    out, ok = nova.run(t.cmd, 60)
    if not ok:
        return out, 'did not finish in 60 s'
    for e in t.expect:
        if not re.search(e, out):
            return out, f'missing "{e}"'
    free = re.search(r'Dir\(s\)\s+([\d,]+) bytes free', out).group(1)
    screen.free[t.name] = free
    if t.name == 'dir D:':
        time.sleep(1)
        nova.shot(os.path.join(a.out, 'dir.png'))
        if screen.free.get('dir C:') == free:
            return out, f"D: reports C:'s free space ({free} bytes)"
    return out, None


screen.free = {}


def report(a, apps, results):
    failed = sum(1 for r in results.values() if r[0])
    print(f'\n{len(results) - failed} of {len(results)} programs passed')
    if a.summary:
        with open(a.summary, 'a') as f:
            f.write(f'### NovaOS app corpus: {len(results) - failed} of {len(results)} programs passed\n\n')
            f.write('| Program | Version | Result | Checks | Time |\n|---|---|---|---|---|\n')
            for app in apps:
                if app.name not in results:
                    continue
                why, secs, steps = results[app.name]
                checks = ', '.join(f'`{n}`' + ('' if not w else ' ❌') for n, w in steps)
                mark = '✅ pass' if not why else ('⏭ ' if why.startswith(SKIPPED) else '❌ ') + why.replace('|', '/')
                f.write(f'| {app.name} | {app.version} | {mark} '
                        f'| {checks} | {secs:.0f} s |\n')
            f.write('\n')


if __name__ == '__main__':
    sys.exit(main())
