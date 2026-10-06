#!/usr/bin/env python3
"""Combine only complete, correctly attributed per-program corpus reports."""
import json
import os
from pathlib import Path
import sys


def collect(root, matrix):
    entries = matrix['include']
    if not entries or len({e['id'] for e in entries}) != len(entries):
        raise ValueError('empty or duplicate corpus matrix')
    rows = []
    for entry in entries:
        if not entry['id'].isdigit() or len(entry['id']) != 2:
            raise ValueError('invalid corpus artifact ID')
        data = json.loads((Path(root) / ('appcorpus-' + entry['id']) / 'results.json').read_text())
        programs = data['programs']
        if len(programs) != 1 or programs[0]['name'] != entry['app']:
            raise ValueError('corpus report does not match its selected program')
        result = programs[0]
        if result['outcome'] not in {'passed', 'failed', 'skipped', 'not run'}:
            raise ValueError('invalid corpus program outcome')
        expected = dict.fromkeys(('passed', 'failed', 'skipped', 'not run'), 0)
        expected[result['outcome']] = 1
        if data['counts'] != expected:
            raise ValueError('corpus counts differ from recorded outcomes')
        rows.append(result)
    return rows


def main():
    rows = collect(sys.argv[1], json.loads(os.environ['CORPUS_MATRIX']))
    counts = {status: sum(r['outcome'] == status for r in rows)
              for status in ('passed', 'failed', 'skipped', 'not run')}
    report = ['### App corpus', '', ', '.join(f'{n} {s}' for s, n in counts.items()), '',
              '| Program | Version | Outcome | Reason |', '| --- | --- | --- | --- |']
    for r in rows:
        report.append('| ' + ' | '.join(str(r[k] or '').replace('|', '/') for k in
                      ('name', 'version', 'outcome', 'reason')) + ' |')
    text = '\n'.join(report) + '\n'
    print(text)
    with Path(os.environ['GITHUB_STEP_SUMMARY']).open('a') as out:
        out.write(text)
    return int(bool(counts['failed'] or counts['not run']))


if __name__ == '__main__':
    raise SystemExit(main())
