#!/usr/bin/env python3
"""Write the notes of a NovaOS release (Markdown, to standard output).

    tools/release_notes.py VERSION [--tag TAG] [--dist DIR] [--repo OWNER/NAME]

The notes are docs/releases/VERSION.md (written by hand: what the release
is, how to start it, what is known not to work), then the files of the
release with their sizes and SHA-256 when --dist names the folder they are
in, then "What is new": the heading of every docs/history/ section added
since the previous release tag (all of them for the first release), in
HISTORY.md's order, each linking to its section at the tag.
.github/workflows/release.yml runs it; run it after a build to read the
notes before tagging (docs/releasing.md).
"""
import argparse, hashlib, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HISTORY = 'docs/history'

# what each file of a release is, in the order the notes list them
ASSETS = [
    ('nova.iso', 'the bootable ISO: a UEFI CD image and a USB stick image, the live system and its installer'),
    ('nova.iso.sha256', "the ISO's SHA-256 (`sha256sum -c nova.iso.sha256`)"),
    ('kernel.elf', 'the kernel, for the update channel'),
    ('bootx64.efi', 'the boot loader, for the update channel'),
    ('novaos-update.txt', 'the update channel an installed NovaOS reads ([docs/updates.md]({blob}/docs/updates.md))'),
    ('SHA256SUMS', 'the SHA-256 of every file above'),
]


def git(*args):
    r = subprocess.run(['git', '-C', ROOT, *args], capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else None


def previous_tag(tag):
    """The newest version tag before this one on its history, or None"""
    rev = tag if git('rev-parse', '-q', '--verify', f'refs/tags/{tag}') else 'HEAD'
    return git('describe', '--tags', '--abbrev=0', '--match', 'v[0-9]*', '--exclude', tag, rev)


def history_sections(since):
    """[(file, heading)] of docs/history, all of them or those added after `since`"""
    names = sorted(n for n in os.listdir(os.path.join(ROOT, HISTORY)) if n.endswith('.md'))
    if since:
        added = git('diff', '--name-only', '--diff-filter=A', since, 'HEAD', '--', HISTORY) or ''
        new = {os.path.basename(p) for p in added.split()}
        names = [n for n in names if n in new]
    out = []
    for n in names:
        with open(os.path.join(ROOT, HISTORY, n), encoding='utf-8') as f:
            head = next((l for l in f if l.startswith('## ')), None)
        if head:
            out.append((n, head[3:].strip()))
    return out


def size(n):
    for unit in ('bytes', 'KB', 'MB', 'GB'):
        if n < 1024 or unit == 'GB':
            return f'{n} {unit}' if unit == 'bytes' else f'{n:.1f} {unit}'
        n /= 1024


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('version')
    ap.add_argument('--tag', help='the release tag (default: v + VERSION)')
    ap.add_argument('--dist', help='folder holding the release files, for their sizes and SHA-256')
    ap.add_argument('--repo', default='dean-plude/os')
    a = ap.parse_args()
    tag = a.tag or 'v' + a.version
    blob = f'https://github.com/{a.repo}/blob/{tag}'

    intro = os.path.join(ROOT, 'docs', 'releases', a.version + '.md')
    if not os.path.exists(intro):
        sys.exit(f'{intro} is missing: write the release notes first (docs/releasing.md)')
    out = [open(intro, encoding='utf-8').read().rstrip().replace('{blob}', blob), '']

    if a.dist:
        out += ['## Files', '', '| File | Size | SHA-256 | What it is |', '| --- | --- | --- | --- |']
        for name, what in ASSETS:
            p = os.path.join(a.dist, name)
            if not os.path.exists(p):
                sys.exit(f'{p} is missing')
            digest = hashlib.sha256(open(p, 'rb').read()).hexdigest()
            out.append(f'| `{name}` | {size(os.path.getsize(p))} | `{digest}` | {what.format(blob=blob)} |')
        out.append('')

    since = previous_tag(tag)
    sections = history_sections(since)
    out += ['## What is new', '']
    out.append(f'Everything added since {since}' if since else
               'Everything NovaOS has gained up to this first release')
    out[-1] += (f', one line per section of [docs/HISTORY.md]({blob}/docs/HISTORY.md) '
                f'({len(sections)} sections):')
    out += ['', '<details><summary>The full list</summary>', '']
    out += [f'- [{head}]({blob}/{HISTORY}/{n})' for n, head in sections]
    out += ['', '</details>', '']
    sys.stdout.write('\n'.join(out))


if __name__ == '__main__':
    main()
