"""Exercise scheduled corpus issue comments without GitHub runner/API access."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

import yaml

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/ci'))
import corpus_comment


class CorpusCommentTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.matrix = {'include': [{'id': '01', 'app': 'Firefox'},
                                   {'id': '02', 'app': 'VLC'}]}
        for entry, outcome, row in (
                (self.matrix['include'][0], 'passed',
                 '| Firefox | 157.0 | ✅ pass | `load HTTPS page` | 12 s |'),
                (self.matrix['include'][1], 'failed',
                 '| VLC | 3.0.21 | ❌ play failed | `play MP4` ❌ | 68 s |')):
            directory = self.root / ('appcorpus-' + entry['id'])
            directory.mkdir()
            counts = dict.fromkeys(corpus_comment.STATUSES, 0)
            counts[outcome] = 1
            (directory / 'results.json').write_text(json.dumps({
                'counts': counts,
                'programs': [{'name': entry['app'], 'version': '1',
                              'outcome': outcome, 'reason': None}]}))
            (directory / 'table.md').write_text(
                f'### NovaOS app corpus: 1 {outcome}\n\n'
                'Test VMs ran under KVM.\n\n'
                '| Program | Version | Result | Checks | Time |\n'
                '|---|---|---|---|---|\n' + row + '\n')

    def test_complete_report_contains_app_details_smp_log_and_run_link(self):
        (self.root / 'smp-out').mkdir()
        (self.root / 'smp-out/smpstress.log').write_text(
            'critical section  8 threads, 20 ms\nsmpstress: 10 passed, 0 failed\n')
        needs = {
            'gate': {'result': 'success', 'outputs': {'run': 'true'}},
            'build': {'result': 'success'},
            'app-corpus': {'result': 'failure'},
            'smp': {'result': 'success'}}
        body = corpus_comment.make_comment(self.root, self.matrix, needs,
                                           'dean-plude/os', 'main', 'abcdef123', '42')
        self.assertIn('1 of 2 programs passed', body)
        self.assertIn('1 passed, 1 failed, 0 skipped, 0 not run', body)
        self.assertIn('Test VMs ran under KVM.', body)
        self.assertIn('Firefox | 157.0', body)
        self.assertIn('VLC | 3.0.21', body)
        self.assertIn('smpstress: 10 passed, 0 failed', body)
        self.assertIn('main at abcdef1', body)
        self.assertIn('actions/runs/42', body)

    def test_gate_skip_produces_did_not_run_comment_without_reports(self):
        needs = {'gate': {'result': 'success', 'outputs': {'run': 'false'}},
                 'build': {'result': 'skipped'}, 'app-corpus': {'result': 'skipped'},
                 'smp': {'result': 'skipped'}}
        body = corpus_comment.make_comment(self.root, self.matrix, needs,
                                           'dean-plude/os', 'main', 'abcdef123', '42')
        self.assertIn('NovaOS app corpus: did not run', body)
        self.assertIn('no corpus execution', body)
        self.assertIn('SMP stress did not run', body)

    def test_missing_workflow_context_fails_closed_in_report(self):
        body = corpus_comment.make_comment(self.root, None, None,
                                           'dean-plude/os', 'main', 'abcdef123', '42')
        self.assertIn('| Gate | missing |', body)
        self.assertIn('corpus execution decision', body)
        self.assertIn('SMP stress log unavailable', body)

    def test_missing_report_is_reported_as_incomplete(self):
        (self.root / 'appcorpus-02').rename(self.root / 'missing-02')
        needs = {'gate': {'result': 'success', 'outputs': {'run': 'true'}},
                 'build': {'result': 'success'}, 'app-corpus': {'result': 'failure'},
                 'smp': {'result': 'failure'}}
        body = corpus_comment.make_comment(self.root, self.matrix, needs,
                                           'dean-plude/os', 'main', 'abcdef123', '42')
        self.assertIn('incomplete (1 of 2 program reports available)', body)
        self.assertIn('Firefox | 157.0', body)
        self.assertNotIn('VLC | 3.0.21', body)
        self.assertIn('SMP stress log unavailable', body)

    def test_post_comment_uses_authenticated_issue_comments_api(self):
        class Response:
            def __enter__(self):
                return self

            def __exit__(self, *_):
                return False

            def read(self):
                return b'{}'

        with patch.object(corpus_comment, 'urlopen', return_value=Response()) as open_url:
            corpus_comment.post_comment('dean-plude/os', '210', 'test-token', 'report')
        request = open_url.call_args.args[0]
        self.assertEqual(request.full_url,
                         'https://api.github.com/repos/dean-plude/os/issues/210/comments')
        self.assertEqual(request.method, 'POST')
        self.assertTrue(request.get_header('Authorization').startswith('Bearer '))
        self.assertEqual(json.loads(request.data), {'body': 'report'})

    def test_workflow_posts_only_scheduled_results_with_scoped_permission(self):
        workflow = yaml.safe_load((ROOT / '.github/workflows/nightly.yml').read_text())
        job = workflow['jobs']['corpus-result']
        self.assertEqual(job['permissions'], {
            'contents': 'read', 'actions': 'read', 'issues': 'write'})
        comment = next(step for step in job['steps']
                       if step.get('name') == 'Post nightly results to issue')
        self.assertEqual(comment['if'], "always() && github.event_name == 'schedule'")
        self.assertIn('python3 tools/ci/corpus_comment.py', comment['run'])
        self.assertEqual(comment['env']['CORPUS_ISSUE'], '210')
        self.assertTrue(any(step.get('name') == 'Collect SMP log' for step in job['steps']))


if __name__ == '__main__':
    unittest.main()
