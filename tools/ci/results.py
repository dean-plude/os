#!/usr/bin/env python3
"""One stable result check; missing, cancelled and unexpected skips fail closed."""
import argparse
import json
import os
from pathlib import Path

JOBS = {'ci': ('checks', 'code', ('build', 'compatibility', 'boot-test', 'graphics-test', 'boot-result')),
        'boot': ('checks', 'code', ('build', 'boot-test')),
        'corpus': ('gate', 'run', ('build', 'app-corpus', 'smp'))}


def evaluate(mode, needs):
    prerequisite, output, dependent = JOBS[mode]
    if not isinstance(needs, dict):
        needs = {}
    first = needs.get(prerequisite)
    first = first if isinstance(first, dict) else {}
    outputs = first.get('outputs')
    selected = outputs.get(output) if isinstance(outputs, dict) else None
    rows = [(prerequisite, first.get('result', 'missing'))]
    ok = first.get('result') == 'success' and selected in ('true', 'false')
    if selected not in ('true', 'false'):
        rows.append(('scope', 'missing or invalid'))
    for name in dependent:
        job = needs.get(name)
        status = job.get('result', 'missing') if isinstance(job, dict) else 'missing'
        expected = 'success' if selected == 'true' or name == 'boot-result' else 'skipped'
        ok = ok and status == expected
        rows.append((name, 'not-run' if status in ('missing', 'skipped') and selected != 'false' else status))
    return ok, rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('mode', choices=JOBS)
    args = ap.parse_args()
    try:
        needs = json.loads(os.environ.get('NEEDS_JSON', ''))
    except ValueError:
        needs = None
    ok, rows = evaluate(args.mode, needs)
    report = '\n'.join(['### Required execution', '', '| Job | Outcome |', '| --- | --- |',
                       *[f'| {n} | {r} |' for n, r in rows], '',
                       'All expected jobs completed.' if ok else 'Required work failed or did not complete.', ''])
    print(report)
    if os.environ.get('GITHUB_STEP_SUMMARY'):
        with Path(os.environ['GITHUB_STEP_SUMMARY']).open('a') as out:
            out.write(report)
    return 0 if ok else 1


if __name__ == '__main__':
    raise SystemExit(main())
