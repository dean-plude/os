#!/usr/bin/env python3
"""Retry failed nightly runs; only real success or active work suppresses a fallback."""
import datetime as dt
import json
import os
from urllib.parse import urlencode
from urllib.request import Request, urlopen

ACTIVE = {'queued', 'in_progress', 'waiting', 'pending', 'requested'}


def pages(get, path, key):
    """Follow every API page, including a final full page without more results."""
    page = 1
    while True:
        data = get(path + ('&' if '?' in path else '?') + urlencode({'per_page': 100, 'page': page}))
        items = data[key]
        yield from items
        if len(items) < 100:
            return
        page += 1


def should_run(runs, jobs, current_id, branch, since):
    for run in runs:
        if (run['id'] == current_id or run['head_branch'] != branch or
                run['event'] not in {'schedule', 'workflow_dispatch'}):
            continue
        # A later queued fallback must not stop the earlier run that owns the slot.
        if run['status'] in ACTIVE:
            if run['status'] != 'queued' or run['id'] < current_id:
                return False, f"run {run['id']} is active"
        if run['created_at'] < since or run['conclusion'] != 'success':
            continue
        for job in jobs(run['id']):
            # continue-on-error hides a failed step in the API. Require job success
            # AND an executed corpus step, never workflow/gate success alone.
            if (job['name'] == 'App corpus' and job['conclusion'] == 'success' and
                    any(s['name'] == 'Run the programs' and s['conclusion'] == 'success'
                        for s in job.get('steps') or [])):
                return False, f"run {run['id']} completed the corpus successfully"
    return True, 'no active run or successful corpus in the last 20 hours'


def main():
    if os.environ['EVENT'] != 'schedule':
        run, why = True, 'PR/manual invocation'
    else:
        repo = os.environ['GITHUB_REPOSITORY']
        def get(path):
            req = Request('https://api.github.com/' + path, headers={
                'Authorization': 'Bearer ' + os.environ['GH_TOKEN'],
                'Accept': 'application/vnd.github+json', 'X-GitHub-Api-Version': '2022-11-28'})
            with urlopen(req, timeout=30) as response:
                return json.load(response)
        since = (dt.datetime.now(dt.timezone.utc) - dt.timedelta(hours=20)).strftime('%Y-%m-%dT%H:%M:%SZ')
        # Include manual runs, but never PR runs. Listing all active runs also
        # prevents duplication if one started before the success lookback window.
        runs = pages(get, f'repos/{repo}/actions/workflows/nightly.yml/runs?' +
                     urlencode({'branch': os.environ['GITHUB_REF_NAME']}), 'workflow_runs')
        def jobs(run_id):
            return pages(get, f'repos/{repo}/actions/runs/{run_id}/jobs?filter=latest', 'jobs')
        run, why = should_run(runs, jobs, int(os.environ['GITHUB_RUN_ID']),
                              os.environ['GITHUB_REF_NAME'], since)
    print(why)
    with open(os.environ['GITHUB_OUTPUT'], 'a') as output:
        output.write(f'run={str(run).lower()}\n')


if __name__ == '__main__':
    main()
