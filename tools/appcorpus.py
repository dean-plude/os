#!/usr/bin/env python3
"""Run unmodified Windows programs on NovaOS and say which still work.

    tools/appcorpus.py [--img build/nova.img] [--cache DIR] [--out DIR]
                       [--only NAME,...] [--summary FILE] [--update-reference]

Downloads each program's official Windows x64 release into --cache (kept
between runs), unpacks it onto drive C: (C:\\Apps\\NAME) with a few sample
files, boots NovaOS once and types each program's commands into the
Terminal (tools/novarun.py's Nova class).  A command passes as a self-test
does (tools/selftest.py): it exits with code 0 and prints what is expected.
Notepad++ runs last (it takes the keyboard): it opens a file and its
screenshot must match tests/reference/notepad++.png (--update-reference
writes that file from this run instead).

The exit status is the number of programs that failed.  --summary appends
a Markdown pass/fail table, one row per program (the nightly workflow,
.github/workflows/nightly.yml, posts it).
"""
import argparse, os, re, shutil, subprocess, sys, tempfile, time, zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from novarun import Nova, ROOT
from selftest import Test, verdict, PANIC

REFERENCE = os.path.join(ROOT, 'tests', 'reference', 'notepad++.png')
ANSI = re.compile(r'\x1b\[[0-9;]*m')
SAMPLE = 'NovaOS app corpus\na needle in a haystack\nthe end\n'


class App:
    """@url's download, unpacked by @unpack into C:\\Apps\\@dir; @tests run in order"""
    def __init__(self, name, version, url, dir, tests, unpack='zip', strip=0):
        self.name, self.version, self.url, self.dir, self.tests = name, version, url, dir, tests
        self.unpack, self.strip = unpack, strip


A = r'C:\Apps'
APPS = [
    App('ripgrep', '14.1.1',
        'https://github.com/BurntSushi/ripgrep/releases/download/14.1.1/ripgrep-14.1.1-x86_64-pc-windows-msvc.zip',
        'rg', [Test('rg --version', rf'{A}\rg\rg.exe --version', [r'ripgrep 14\.1\.1']),
               Test('rg search', rf'{A}\rg\rg.exe -n needle {A}\data', [r'hello\.txt\r?\n2:a needle in a haystack'])],
        strip=1),
    App('fd', '10.2.0',
        'https://github.com/sharkdp/fd/releases/download/v10.2.0/fd-v10.2.0-x86_64-pc-windows-msvc.zip',
        'fd', [Test('fd --version', rf'{A}\fd\fd.exe --version', [r'fd 10\.2\.0']),
               Test('fd find', rf'{A}\fd\fd.exe -e txt . {A}\data', [r'hello\.txt', r'notes\.txt'])],
        strip=1),
    App('jq', '1.7.1', 'https://github.com/jqlang/jq/releases/download/jq-1.7.1/jq-windows-amd64.exe',
        'jq', [Test('jq --version', rf'{A}\jq\jq.exe --version', [r'jq-1\.7\.1']),
               Test('jq filter', rf'{A}\jq\jq.exe -c ".a+.b, [.[]]" {A}\data\ab.json', [r'(?m)^42\r?$', r'\[40,2\]'])],
        unpack='exe'),
    App('7-Zip', '26.03', 'https://github.com/ip7z/7zip/releases/download/26.03/7z2603-x64.exe',
        '7-Zip', [Test('7z a', rf'{A}\7-Zip\7z.exe a {A}\data.7z {A}\data', [r'Everything is Ok']),
                  Test('7z t', rf'{A}\7-Zip\7z.exe t {A}\data.7z', [r'Type = 7z', r'Everything is Ok'])],
        unpack='7z'),
    App('MinGit', '2.51.0',
        'https://github.com/git-for-windows/git/releases/download/v2.51.0.windows.1/MinGit-2.51.0-64-bit.zip',
        'MinGit', [Test('git clone', rf'{A}\MinGit\cmd\git.exe clone {A}\data\src.git {A}\clone',
                        [r'Cloning into'], timeout=300),
                   Test('git log', rf'{A}\MinGit\cmd\git.exe -C {A}\clone log --format=%s',
                        [r'Add the corpus notes', r'First commit']),
                   Test('git status', rf'{A}\MinGit\cmd\git.exe -C {A}\clone status --short --branch',
                        [r'## main\.\.\.origin/main'])]),
    App('Python', '3.14.0', 'https://api.nuget.org/v3-flatcontainer/python/3.14.0/python.3.14.0.nupkg',
        'Python', [Test('python -c', rf'{A}\Python\python.exe -c "import sys, json; '
                        r'print(json.dumps([sum(range(10)), sys.version_info[:2]]))"', [r'\[45, \[3, 14\]\]'],
                        timeout=300)],
        strip='tools'),
    App('Node.js', '24.9.0', 'https://nodejs.org/dist/v24.9.0/node-v24.9.0-win-x64.zip',
        'node', [Test('node -v', rf'{A}\node\node.exe -v', [r'v24\.9\.0'], timeout=300),
                 Test('node -e', rf'{A}\node\node.exe -e "console.log(6*7, process.platform)"',
                      [r'42 win32'], timeout=300)],
        strip=1),
    App('Notepad++', '8.8.3',
        'https://github.com/notepad-plus-plus/notepad-plus-plus/releases/download/v8.8.3/npp.8.8.3.portable.x64.zip',
        'npp', [Test('open a file', rf'start {A}\npp\notepad++.exe {A}\data\hello.txt')]),
]


def fetch(url, cache):
    f = os.path.join(cache, os.path.basename(url))
    if not os.path.exists(f) or not os.path.getsize(f):
        subprocess.run(['curl', '-sSLf', '--retry', '4', '-o', f + '.part', url], check=True)
        os.replace(f + '.part', f)
    return f


def stage(app, archive, dest):
    """Unpack @archive into @dest, dropping @app.strip leading folders (or
    keeping only the folder named @app.strip)"""
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

    t_boot = time.time()
    try:
        nova = Nova(a.img, os.path.join(work, 'boot'), [(apps_dir, A)], mem=4096, data_mb=2048)
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
    nova.shot(shot)
    time.sleep(1)
    if a.update_reference:
        os.makedirs(os.path.dirname(REFERENCE), exist_ok=True)
        from PIL import Image
        Image.open(shot).convert('RGB').resize((1280, 800), Image.BOX).save(REFERENCE, optimize=True)
        return out, None
    if not os.path.exists(REFERENCE):
        return out, 'no reference screenshot (run with --update-reference)'
    d = compare(shot, REFERENCE)
    print(f'  screenshot differs from the reference in {d:.1%} of pixels', flush=True)
    return out, None if d <= a.max_diff else f'screenshot differs from the reference in {d:.1%} of pixels'


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
