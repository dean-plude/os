#!/usr/bin/env python3
"""Stamp and verify the exact image shared by all jobs in this workflow run."""
import argparse
import hashlib
import json
from pathlib import Path

FILES = ('nova.img', 'nova.iso', 'kernel.elf', 'bootx64.efi')


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def stamp(directory, sha):
    root = Path(directory)
    data = {'commit': sha, 'files': {name: digest(root / name) for name in FILES}}
    (root / 'ci-image.json').write_text(json.dumps(data, indent=2) + '\n')


def verify(directory, sha):
    root = Path(directory)
    data = json.loads((root / 'ci-image.json').read_text())
    if data['commit'] != sha or set(data['files']) != set(FILES):
        raise ValueError('image provenance differs from the checked-out commit')
    for name in FILES:
        if data['files'][name] != digest(root / name):
            raise ValueError(f'image checksum differs: {name}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('mode', choices=('stamp', 'verify'))
    ap.add_argument('--directory', default='build')
    ap.add_argument('--sha', required=True)
    args = ap.parse_args()
    globals()[args.mode](args.directory, args.sha)


if __name__ == '__main__':
    main()
