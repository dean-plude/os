#!/usr/bin/env python3
"""Post each scheduled app corpus result as a comment on its tracking issue."""
import json
import os
from pathlib import Path
import re
import sys
from urllib.request import Request, urlopen

from corpus_report import collect

STATUSES = ('passed', 'failed', 'skipped', 'not run')


def matrix_entries(matrix):
    entries = matrix.get('include') if isinstance(matrix, dict) else None
    if not isinstance(entries, list) or not entries:
        return []
    ids = []
    for entry in entries:
        if (not isinstance(entry, dict) or
                not isinstance(entry.get('id'), str) or
                not re.fullmatch(r'\d{2}', entry['id']) or
                not isinstance(entry.get('app'), str) or not entry['app']):
            return []
        ids.append(entry['id'])
    if len(ids) != len(set(ids)):
        return []
    return entries


def app_reports(root, matrix):
    entries = matrix_entries(matrix)
    rows, details, accelerators = [], [], set()
    counts = dict.fromkeys(STATUSES, 0)
    for entry in entries:
        try:
            result = collect(root, {'include': [entry]})[0]
        except (OSError, ValueError, KeyError, TypeError):
            continue
        rows.append(result)
        counts[result['outcome']] += 1
        try:
            text = (Path(root) / ('appcorpus-' + entry['id']) / 'table.md').read_text()
        except OSError:
            text = ''
        lines = text.splitlines()
        accelerators.update(line for line in lines if line.startswith('Test VMs ran under '))
        details.extend(line for line in lines
                       if line.startswith('|') and not line.lower().startswith('| program |')
                       and not all(char in '|-: ' for char in line))
    return entries, rows, counts, details, sorted(accelerators)


def job_result(needs, name):
    if not isinstance(needs, dict):
        return 'missing'
    job = needs.get(name)
    return job.get('result', 'missing') if isinstance(job, dict) else 'missing'


def make_comment(root, matrix, needs, repository, ref, sha, run_id):
    if not isinstance(needs, dict):
        needs = {}
    gate = needs.get('gate')
    outputs = gate.get('outputs') if isinstance(gate, dict) else None
    run = outputs.get('run') if isinstance(outputs, dict) else None
    lines = ['### Required job results', '', '| Job | Result |', '| --- | --- |']
    for name, label in (('gate', 'Gate'), ('build', 'Build'),
                        ('app-corpus', 'App corpus'), ('smp', 'SMP stress')):
        result = job_result(needs, name)
        if name == 'gate' and run in ('true', 'false'):
            result += f' (run={run})'
        lines.append(f'| {label} | {result} |')

    if run == 'false':
        lines.extend(['', '### NovaOS app corpus: did not run', '',
                      'The nightly gate selected no corpus execution for this run.'])
    elif run == 'true':
        entries, rows, counts, details, accelerators = app_reports(root, matrix)
        if len(rows) == len(entries) and entries:
            lines.extend(['', f'### NovaOS app corpus: {counts["passed"]} of {len(entries)} programs passed'])
        else:
            lines.extend(['', f'### NovaOS app corpus: incomplete '
                              f'({len(rows)} of {len(entries)} program reports available)'])
        lines.extend(['', ', '.join(f'{counts[status]} {status}' for status in STATUSES), ''])
        lines.extend(accelerators)
        if accelerators:
            lines.append('')
        if details:
            lines.extend(['| Program | Version | Result | Checks | Time |',
                          '|---|---|---|---|---|', *details])
        elif rows:
            lines.append('Per-program details were not available.')
        else:
            lines.append('No per-program reports were available.')
    else:
        lines.extend(['', '### NovaOS app corpus: did not run', '',
                      'The workflow did not produce a corpus execution decision.'])

    smp_log = Path(root) / 'smp-out' / 'smpstress.log'
    lines.extend(['', '### smpstress on 4 CPUs', ''])
    if smp_log.is_file():
        lines.extend(['```', smp_log.read_text(errors='replace').rstrip(), '```'])
    else:
        status = job_result(needs, 'smp')
        message = 'did not run' if status == 'skipped' else 'log unavailable'
        lines.append(f'SMP stress {message} (job result: {status}).')
    lines.extend(['', f'{ref} at {sha[:7]}, '
                       f'[run log and screenshots](https://github.com/{repository}/actions/runs/{run_id})'])
    return '\n'.join(lines) + '\n'


def post_comment(repository, issue, token, body):
    if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository):
        raise ValueError('invalid repository')
    if not str(issue).isdigit():
        raise ValueError('invalid issue number')
    request = Request(
        f'https://api.github.com/repos/{repository}/issues/{issue}/comments',
        data=json.dumps({'body': body}).encode(),
        headers={'Authorization': 'Bearer ' + token,
                 'Accept': 'application/vnd.github+json',
                 'X-GitHub-Api-Version': '2022-11-28',
                 'Content-Type': 'application/json'},
        method='POST')
    with urlopen(request, timeout=30) as response:
        response.read()


def main():
    if os.environ.get('EVENT') != 'schedule':
        return 0
    try:
        matrix = json.loads(os.environ.get('CORPUS_MATRIX', ''))
    except (ValueError, TypeError):
        matrix = None
    try:
        needs = json.loads(os.environ.get('NEEDS_JSON', ''))
    except (ValueError, TypeError):
        needs = None
    body = make_comment(
        Path(sys.argv[1]), matrix, needs,
        os.environ['GITHUB_REPOSITORY'], os.environ['GITHUB_REF_NAME'],
        os.environ['GITHUB_SHA'], os.environ['GITHUB_RUN_ID'])
    print(body)
    if os.environ.get('GITHUB_STEP_SUMMARY'):
        with Path(os.environ['GITHUB_STEP_SUMMARY']).open('a') as summary:
            summary.write(body)
    post_comment(os.environ['GITHUB_REPOSITORY'], os.environ['CORPUS_ISSUE'],
                 os.environ['GH_TOKEN'], body)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
