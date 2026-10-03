#!/usr/bin/env python3
"""Make the files of a NovaOS update channel (kernel/fs/update.h).

    tools/mkupdate.py OUTDIR [--kernel build/kernel.elf] [--loader build/bootx64.efi]
                      [--version V] [--notes TEXT] [--base URL] [--no-loader]

Writes OUTDIR/kernel.elf, OUTDIR/bootx64.efi and OUTDIR/novaos-update.txt,
the channel file an installed NovaOS reads: its version, each file's name,
size and SHA-256, and a line of notes.  Served together (a GitHub
release's assets, or any web server), they update NovaOS: the App Store's
Updates page, or `update install` in the Terminal.  The file names are
relative to the channel file's address unless --base gives the URL of
the folder they are in.

--version stamps the copy of the kernel with another version than the
one it was built with (the kernel keeps it in a marked field, see
kernel/ke/version.c), without building it again: the self-test's
"0.1.1-test" update is the same build stamped so.  Without --version
the kernel's own version is the channel's.
"""
import argparse, hashlib, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MARK = b'NovaOS-version-stamp:'      # kernel/ke/version.h: NOVA_STAMP_MARK_LEN
VER_MAX = 27                         # NOVA_STAMP_VER_MAX (with the NUL)


def stamp_offset(data):
    """Where the version field is in a kernel image (the marker occurs once)"""
    at = data.find(MARK)
    if at < 0 or data.find(MARK, at + 1) >= 0:
        sys.exit('the kernel has no version stamp, or more than one (kernel/ke/version.c)')
    return at + len(MARK)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('out')
    ap.add_argument('--kernel', default=os.path.join(ROOT, 'build', 'kernel.elf'))
    ap.add_argument('--loader', default=os.path.join(ROOT, 'build', 'bootx64.efi'))
    ap.add_argument('--no-loader', action='store_true', help='leave the boot loader out of the update')
    ap.add_argument('--version', help='stamp the kernel copy with this version')
    ap.add_argument('--notes', default='')
    ap.add_argument('--base', help='URL of the folder the files are served from (default: beside the channel file)')
    a = ap.parse_args()

    kernel = bytearray(open(a.kernel, 'rb').read())
    off = stamp_offset(kernel)
    if a.version:
        if not re.fullmatch(r'[0-9]+(\.[0-9]+)*(-[A-Za-z0-9.]+)?', a.version) or len(a.version) >= VER_MAX:
            sys.exit(f'bad version {a.version!r} (like 0.1.1 or 0.1.1-test, at most {VER_MAX - 1} characters)')
        kernel[off:off + VER_MAX] = a.version.encode().ljust(VER_MAX, b'\0')
    version = bytes(kernel[off:off + VER_MAX]).split(b'\0')[0].decode()

    os.makedirs(a.out, exist_ok=True)
    files = [('kernel', 'kernel.elf', bytes(kernel))]
    if not a.no_loader:
        files.append(('loader', 'bootx64.efi', open(a.loader, 'rb').read()))
    lines = ['NovaOS update 1', f'version {version}']
    for kind, name, data in files:
        with open(os.path.join(a.out, name), 'wb') as f:
            f.write(data)
        url = a.base.rstrip('/') + '/' + name if a.base else name
        lines.append(f'{kind} {url} {len(data)} {hashlib.sha256(data).hexdigest()}')
    if a.notes:
        lines.append('notes ' + ' '.join(a.notes.split()))
    with open(os.path.join(a.out, 'novaos-update.txt'), 'w') as f:
        f.write('\n'.join(lines) + '\n')
    print(f'NovaOS {version}: ' + ', '.join(f'{name} ({len(data)} bytes)' for _, name, data in files) +
          f' and novaos-update.txt in {a.out}')


if __name__ == '__main__':
    main()
