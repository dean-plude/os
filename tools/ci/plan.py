#!/usr/bin/env python3
"""Determine explicit CI scope; unknown files and missing diffs require execution."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def docs_only(paths):
    return bool(paths) and all(p.startswith('docs/') or p.endswith('.md') or
                               p == 'LICENSE' for p in paths)


def scope(event, paths):
    code = event not in {'pull_request', 'merge_group'} or not docs_only(paths)
    corpus = code and (not paths or event not in {'pull_request', 'merge_group'} or any(
        p.startswith(('tests/appcorpus/', 'tests/reference/', 'tests/tools/',
                      'tools/ci/', '.github/')) or
        p in {'tools/appcorpus.py', 'tools/download_cache.py', 'tools/novarun.py',
              'kernel/apps/store.c', 'tools/bundle_firefox.py'} for p in paths))
    return {'code': code, 'corpus': corpus}


def changed(event, payload):
    if event == 'pull_request':
        base = payload['pull_request']['base']['sha']
    elif event == 'merge_group':
        base = payload['merge_group']['base_sha']
    else:
        return []
    # Diff the checked-out merge tree against its declared base, not HEAD^1
    # (which is wrong for some merge queues and local workflow invocations).
    return subprocess.check_output(['git', 'diff', '--name-only', '-z', base, 'HEAD'],
                                   cwd=ROOT).decode().rstrip('\0').split('\0')


def app_matrix(apps):
    names = [a.name for a in apps]
    if not names or len(names) != len(set(names)) or any(',' in n for n in names):
        raise ValueError('corpus requires unique, nonempty names without commas')
    return {'include': [{'id': f'{i:02d}', 'app': n} for i, n in enumerate(names, 1)]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--matrix', action='store_true')
    args = ap.parse_args()
    if args.matrix:
        sys.path.insert(0, str(ROOT / 'tools'))
        import appcorpus
        print(json.dumps(app_matrix(appcorpus.APPS), separators=(',', ':')))
        return
    event = os.environ['GITHUB_EVENT_NAME']
    payload = json.loads(Path(os.environ['GITHUB_EVENT_PATH']).read_text())
    selected = scope(event, changed(event, payload))
    with Path(os.environ['GITHUB_OUTPUT']).open('a') as out:
        for key, value in selected.items():
            out.write(f'{key}={str(value).lower()}\n')
    print(json.dumps(selected))


if __name__ == '__main__':
    main()
