"""Fail closed on incomplete CI jobs; intentional scope/gate skips stay explicit."""
import argparse
import json
import os
from pathlib import Path


def evaluate(mode, needs):
    if not isinstance(needs, dict):
        return False, [('workflow', 'missing job results')]
    if mode == 'ci':
        prerequisite, output, dependent = 'checks', 'code', ['boot-test', 'graphics-test']
    elif mode == 'corpus':
        prerequisite, output, dependent = 'gate', 'run', ['app-corpus']
    else:
        raise ValueError(mode)
    first = needs.get(prerequisite, {})
    if not isinstance(first, dict):
        first = {}
    result = first.get('result', 'missing')
    outputs = first.get('outputs', {})
    scope = outputs.get(output) if isinstance(outputs, dict) else None
    ok = result == 'success' and scope in ('true', 'false')
    rows = [(prerequisite, result)]
    if result == 'success' and scope not in ('true', 'false'):
        rows.append(('scope', f'invalid {output} output'))
    expected = 'success' if scope == 'true' else 'skipped'
    for name in dependent:
        job = needs.get(name, {})
        result = job.get('result', 'missing') if isinstance(job, dict) else 'missing'
        ok = ok and result == expected
        label = 'not-run' if result in ('skipped', 'missing') and scope != 'false' else result
        rows.append((name, label))
    return ok, rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=['ci', 'corpus'])
    args = parser.parse_args()
    try:
        needs = json.loads(os.environ.get('NEEDS_JSON', ''))
    except (ValueError, TypeError):
        needs = None
    ok, rows = evaluate(args.mode, needs)
    report = '\n'.join(['### Required job results', '', '| Job | Result |',
                        '| --- | --- |', *[f'| {name} | {result} |' for name, result in rows],
                        '', 'All expected jobs completed.' if ok else
                        'Required execution failed or did not complete.', ''])
    print(report)
    if os.environ.get('GITHUB_STEP_SUMMARY'):
        with Path(os.environ['GITHUB_STEP_SUMMARY']).open('a') as summary:
            summary.write(report)
    return 0 if ok else 1


if __name__ == '__main__':
    raise SystemExit(main())
