"""Exercise final workflow decisions without runner/API dependencies."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'tools/ci/check_job_results.py'
spec = importlib.util.spec_from_file_location('job_results', SCRIPT)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class JobResultsTests(unittest.TestCase):
    def needs(self, mode, scope='true'):
        if mode == 'ci':
            first, key, children = 'checks', 'code', ['boot-test', 'graphics-test']
        else:
            first, key, children = 'gate', 'run', ['app-corpus']
        return {first: {'result': 'success', 'outputs': {key: scope}},
                **{name: {'result': 'success' if scope == 'true' else 'skipped'} for name in children}}

    def test_success_and_intentional_skips(self):
        for mode in ['ci', 'corpus']:
            for scope in ['true', 'false']:
                with self.subTest(mode=mode, scope=scope):
                    self.assertTrue(module.evaluate(mode, self.needs(mode, scope))[0])

    def test_each_required_job_fails_closed(self):
        for mode in ['ci', 'corpus']:
            for name in self.needs(mode):
                for result in ['failure', 'cancelled', 'skipped', None]:
                    needs = self.needs(mode)
                    if result is None:
                        del needs[name]
                    else:
                        needs[name]['result'] = result
                    with self.subTest(mode=mode, job=name, result=result):
                        self.assertFalse(module.evaluate(mode, needs)[0])

    def test_bad_scope_and_missing_data_fail(self):
        for mode in ['ci', 'corpus']:
            for scope in [None, '', 'TRUE', True]:
                self.assertFalse(module.evaluate(mode, self.needs(mode, scope))[0])
            for data in [None, [], {}, {'checks': None}, {'gate': {'outputs': None}}]:
                self.assertFalse(module.evaluate(mode, data)[0])
            needs = self.needs(mode, 'false')
            name = 'boot-test' if mode == 'ci' else 'app-corpus'
            needs[name]['result'] = 'failure'
            self.assertFalse(module.evaluate(mode, needs)[0])

    def test_cli_exit_and_summary_show_incomplete_jobs(self):
        with tempfile.TemporaryDirectory() as d:
            summary = Path(d) / 'summary.md'
            needs = self.needs('corpus')
            needs['app-corpus']['result'] = 'skipped'
            env = dict(os.environ, NEEDS_JSON=json.dumps(needs), GITHUB_STEP_SUMMARY=str(summary))
            result = subprocess.run([sys.executable, str(SCRIPT), 'corpus'], env=env,
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 1)
            self.assertIn('| app-corpus | not-run |', summary.read_text())
            env['NEEDS_JSON'] = 'invalid JSON'
            result = subprocess.run([sys.executable, str(SCRIPT), 'corpus'], env=env,
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 1)
