#!/usr/bin/env python3
"""Fail when a merge left conflict debris in the tree.

    tools/ci/check-conflict-markers.py [FILE...]   (default: every tracked file)

Merges of main into pull requests have twice committed git's conflict
markers into README.md and docs/ (PRs #27 and #37), and once left the bare
line "main" (the rest of a deleted ">>>>>>> main") in
tools/build_userland.py.  This flags, in text files:
  - "<<<<<<< ", ">>>>>>> " and "||||||| " lines (and the bare markers),
    everywhere, vendored code included;
  - "=======" lines and lines holding nothing but a branch name (main,
    master, HEAD, origin/..., claude/...), outside third_party/.
CI runs it on every pull request (.github/workflows/ci.yml).
"""
import os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MARKER = re.compile(r'^(<{7}|>{7}|\|{7})( .*)?$')
DEBRIS = re.compile(r'^(={7}|HEAD|main|master|origin/\S+|claude/\S+)\s*$')


def files():
    out = subprocess.run(['git', '-C', ROOT, 'ls-files', '-z'], capture_output=True, check=True).stdout
    return [f for f in out.decode().split('\0') if f]


def main():
    bad = 0
    for f in sys.argv[1:] or files():
        path = os.path.join(ROOT, f)
        if not os.path.isfile(path) or os.path.islink(path):
            continue
        data = open(path, 'rb').read()
        if b'\0' in data[:8192]:              # binary
            continue
        vendored = f.startswith('third_party/')
        for n, line in enumerate(data.decode('utf-8', 'replace').splitlines(), 1):
            if MARKER.match(line) or (not vendored and DEBRIS.match(line)):
                print(f'::error file={f},line={n}::{f}:{n}: merge conflict debris: {line[:60]!r}')
                bad += 1
    if bad:
        print(f'{bad} line(s) look like an unfinished merge: resolve them (keep both sides\' changes) and commit')
    else:
        print('no conflict markers')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
