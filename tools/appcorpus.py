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
PC must list both drives.  Notepad++ runs last (it takes the keyboard): it
opens a file.  The This PC and Notepad++ screenshots must match
tests/reference/this-pc.png and notepad++.png (--update-reference writes
those files from this run instead); the screenshots are kept in --out.

The exit status is the number of programs that failed.  --summary appends
a Markdown pass/fail table, one row per program (the nightly workflow,
.github/workflows/nightly.yml, posts it).
"""
import argparse, os, re, shutil, struct, subprocess, sys, tempfile, time, zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from novarun import Nova, ROOT
from selftest import Test, verdict, PANIC

REFERENCES = os.path.join(ROOT, 'tests', 'reference')
DRIVE_LABEL = 'NOVACORPUS'
ANSI = re.compile(r'\x1b\[[0-9;]*m')
SAMPLE = 'NovaOS app corpus\na needle in a haystack\nthe end\n'


class App:
    """@url's download, unpacked by @unpack into C:\\Apps\\@dir; @tests run in order"""
    def __init__(self, name, version, url, dir, tests, unpack='zip', strip=0):
        self.name, self.version, self.url, self.dir, self.tests = name, version, url, dir, tests
        self.unpack, self.strip = unpack, strip


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
        ns = {'App': App, 'Test': Test, 'A': A, 'DRIVE_LABEL': DRIVE_LABEL, '__file__': f}
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


def make_data(d):
    """C:\\Apps\\data: text to search and a git repository to clone"""
    os.makedirs(d)
    with open(os.path.join(d, 'hello.txt'), 'w', newline='\n') as f:
        f.write(SAMPLE)
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
    for app in apps:
        try:
            stage(app, fetch(app.url, a.cache), os.path.join(apps_dir, app.dir))
            staged.append(app)
        except Exception as e:
            results[app.name] = (f'download or unpack failed: {e}', 0, [])
            print(f'FAIL  {app.name:10s} {results[app.name][0]}', flush=True)
    make_data(os.path.join(apps_dir, 'data'))

    ntfs = make_ntfs(os.path.join(work, 'ntfs.img')) if any(x.name == 'NovaOS' for x in staged) else None
    t_boot = time.time()
    try:
        nova = Nova(a.img, os.path.join(work, 'boot'), [(apps_dir, A)], mem=4096, data_mb=2048,
                    extra_args=['-drive', f'format=raw,file={ntfs}'] if ntfs else [])
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
                if app.name == 'Notepad++':
                    out, w = notepad(nova, t, a)
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
        with open(os.path.join(a.out, 'serial.log'), 'w') as f:
            f.write(log + nova.sr.read_new())
        shutil.rmtree(work, ignore_errors=True)
    report(a, apps, results)
    return sum(1 for r in results.values() if r[0])


def notepad(nova, t, a):
    """Start Notepad++ on a file; its screenshot must match the reference"""
    out, ok = nova.run(t.cmd, 30)
    shot = os.path.join(a.out, 'notepad++.png')
    m = None
    for _ in range(60):                           # its window comes up (or it dies)
        time.sleep(1)
        out += nova.sr.read_new()
        m = re.search(r'\[UM\] notepad\+\+\.exe \(PID \d+\) (exited|crashed|terminated)[^\n]*', out)
        if m:
            break
    if m:
        return out, m.group(0).split(') ', 1)[1]
    time.sleep(5)
    return out, check_shot(nova, a, 'notepad++.png')


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
                f.write(f'| {app.name} | {app.version} | {"✅ pass" if not why else "❌ " + why.replace("|", "/")} '
                        f'| {checks} | {secs:.0f} s |\n')
            f.write('\n')


if __name__ == '__main__':
    sys.exit(main())
