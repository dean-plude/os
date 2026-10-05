import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('nightly_gate', Path(__file__).resolve().parents[2] / 'tools/ci/nightly_gate.py')
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


def run(id=1, status='completed', conclusion='success', event='schedule', branch='main', date='2026-10-05T03:00:00Z'):
    return dict(id=id, status=status, conclusion=conclusion, event=event, head_branch=branch, created_at=date)


def job(conclusion='success', steps=None):
    return dict(name='App corpus', conclusion=conclusion,
                steps=[dict(name='Run the programs', conclusion='success')] if steps is None else steps)


class GateTests(unittest.TestCase):
    def decision(self, runs, jobs=()):
        return gate.should_run(runs, lambda _: jobs, 10, 'main', '2026-10-04T20:00:00Z')[0]

    def test_failed_cancelled_and_skipped_gate_runs_retry(self):
        for conclusion in ('failure', 'cancelled', 'timed_out'):
            self.assertTrue(self.decision([run(conclusion=conclusion)], [job()]))
        self.assertTrue(self.decision([run()], [job('skipped', [])]))
        self.assertTrue(self.decision([run()], [job(steps=[])]))
        self.assertTrue(self.decision([run()], [job('failure')]))

    def test_real_success_suppresses_retry(self):
        self.assertFalse(self.decision([run()], [job()]))
        self.assertFalse(self.decision([run(event='workflow_dispatch')], [job()]))

    def test_active_and_queue_order(self):
        for status in gate.ACTIVE:
            self.assertFalse(self.decision([run(status=status, conclusion=None)]))
        self.assertTrue(self.decision([run(id=11, status='queued', conclusion=None)]))
        self.assertFalse(self.decision([run(id=11, status='in_progress', conclusion=None)]))
        self.assertFalse(self.decision([run(status='in_progress', date='2026-10-01T00:00:00Z', conclusion=None)]))

    def test_ignore_current_other_branch_pr_and_old_success(self):
        for r in (run(id=10), run(branch='other'), run(event='pull_request'), run(date='2026-10-01T00:00:00Z')):
            self.assertTrue(self.decision([r], [job()]))

    def test_paginated_runs_and_jobs(self):
        calls = []
        def get(path):
            calls.append(path)
            key = 'jobs' if '/jobs?' in path else 'workflow_runs'
            if path.endswith('&page=1'):
                return {key: [job('skipped', []) if key == 'jobs' else run(id=10)] * 100}
            return {key: [job() if key == 'jobs' else run()]}
        runs = gate.pages(get, 'runs?branch=main', 'workflow_runs')
        self.assertFalse(gate.should_run(runs, lambda _: gate.pages(get, 'runs/1/jobs?filter=latest', 'jobs'),
                                         10, 'main', '2026-10-04T20:00:00Z')[0])
        self.assertEqual(len(calls), 4)

    def test_api_failure_is_not_permission_to_run(self):
        def broken(path):
            raise RuntimeError('API unavailable')
        with self.assertRaises(RuntimeError):
            list(gate.pages(broken, 'runs', 'workflow_runs'))


if __name__ == '__main__':
    unittest.main()
