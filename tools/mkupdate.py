#!/usr/bin/env python3
"""Make the files of a NovaOS update channel (kernel/fs/update.h).

    tools/mkupdate.py OUTDIR [--kernel build/kernel.elf] [--loader build/bootx64.efi]
                      [--version V] [--notes TEXT] [--base URL] [--no-loader]
                      [--sign KEYFILE | --sign-env NAME]
    tools/mkupdate.py --new-key KEYFILE
    tools/mkupdate.py --public-key KEYFILE

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

--sign signs the channel file with the Ed25519 key in KEYFILE (--sign-env:
the key in an environment variable, as the release workflow has it): a
last line "signature ed25519 PUBLIC-KEY SIGNATURE" covers every byte
before it.  A NovaOS with an update signing key built in
(kernel/fs/update_key.h) only installs from a channel signed with that
key.  --new-key makes a key pair (the secret into KEYFILE, which must
not exist yet; the public key is printed), --public-key prints a key
file's public key (docs/releasing.md).
"""
import argparse, hashlib, os, re, sys

import ed25519

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KEY_H = os.path.join(ROOT, 'kernel', 'fs', 'update_key.h')
MARK = b'NovaOS-version-stamp:'      # kernel/ke/version.h: NOVA_STAMP_MARK_LEN
VER_MAX = 27                         # NOVA_STAMP_VER_MAX (with the NUL)


def stamp_offset(data):
    """Where the version field is in a kernel image (the marker occurs once)"""
    at = data.find(MARK)
    if at < 0 or data.find(MARK, at + 1) >= 0:
        sys.exit('the kernel has no version stamp, or more than one (kernel/ke/version.c)')
    return at + len(MARK)


def built_in_key():
    """The public key kernel/fs/update_key.h builds into NovaOS ('' if none)"""
    m = re.search(r'#define UPDATE_SIGNING_KEY "([0-9A-Fa-f]*)"', open(KEY_H).read())
    return m.group(1).lower() if m else ''


def load_secret(a):
    if a.sign_env:
        text = os.environ.get(a.sign_env, '')
        if not text.strip():
            sys.exit(f'the environment variable {a.sign_env} holds no key')
    else:
        text = open(a.sign).read()
    try:
        return ed25519.parse_secret(text)
    except ValueError as e:
        sys.exit(f'bad update signing key: {e}')


def new_key(path):
    if os.path.exists(path):
        sys.exit(f'{path} exists: a new key never replaces one')
    secret = ed25519.new_secret()
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'w') as f:
        f.write(secret.hex() + '\n')
    print(ed25519.public_key(secret).hex())


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('out', nargs='?')
    ap.add_argument('--kernel', default=os.path.join(ROOT, 'build', 'kernel.elf'))
    ap.add_argument('--loader', default=os.path.join(ROOT, 'build', 'bootx64.efi'))
    ap.add_argument('--no-loader', action='store_true', help='leave the boot loader out of the update')
    ap.add_argument('--version', help='stamp the kernel copy with this version')
    ap.add_argument('--notes', default='')
    ap.add_argument('--base', help='URL of the folder the files are served from (default: beside the channel file)')
    ap.add_argument('--sign', metavar='KEYFILE', help='sign the channel file with this Ed25519 key')
    ap.add_argument('--sign-env', metavar='NAME', help='sign it with the key in this environment variable')
    ap.add_argument('--new-key', metavar='KEYFILE', help='make a new signing key pair and print its public key')
    ap.add_argument('--public-key', metavar='KEYFILE', help="print a key file's public key")
    a = ap.parse_args()
    if a.new_key:
        return new_key(a.new_key)
    if a.public_key:
        a.sign = a.public_key
        return print(ed25519.public_key(load_secret(a)).hex())
    if not a.out:
        ap.error('OUTDIR is needed')
    secret = load_secret(a) if a.sign or a.sign_env else None

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
    channel = ('\n'.join(lines) + '\n').encode()
    signed = ''
    if secret:
        public = ed25519.public_key(secret).hex()
        channel += f'signature ed25519 {public} {ed25519.sign(secret, channel).hex()}\n'.encode()
        signed = f', signed with {public}'
        builtin = built_in_key()
        if builtin and builtin != public:
            print(f'mkupdate: warning: this tree builds in the key {builtin} (kernel/fs/update_key.h), '
                  'not the one signing', file=sys.stderr)
    with open(os.path.join(a.out, 'novaos-update.txt'), 'wb') as f:
        f.write(channel)
    print(f'NovaOS {version}: ' + ', '.join(f'{name} ({len(data)} bytes)' for _, name, data in files) +
          f' and novaos-update.txt in {a.out}{signed}')


if __name__ == '__main__':
    main()
