"""Host-side corpus assertion/reporting regressions; no VM or downloads required."""
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import appcorpus
from selftest import verdict


class CorpusTests(unittest.TestCase):
    def test_report_separates_outcomes_and_includes_missing_results(self):
        apps = [SimpleNamespace(name=n, version='1') for n in ('pass', 'fail', 'skip', 'blocked', 'missing')]
        results = {'pass': (None, 1, [('test', None)]), 'fail': ('kernel panic', 1, [('test', 'kernel panic')]),
                   'skip': ('skipped: no audio', 0, []), 'blocked': ('not run: kernel panic', 0, [])}
        with tempfile.TemporaryDirectory() as d:
            summary = Path(d) / 'summary.md'
            with contextlib.redirect_stdout(io.StringIO()) as out:
                appcorpus.report(SimpleNamespace(summary=str(summary), out=d), apps, results)
            self.assertIn('1 passed, 1 failed, 1 skipped, 2 not run (5 programs selected)', out.getvalue())
            text = summary.read_text()
            self.assertIn('| missing | 1 | ⏭ not run: no result recorded', text)
            self.assertIn('| fail | 1 | ❌ kernel panic', text)
            report = json.loads((Path(d) / 'results.json').read_text())
            self.assertEqual(report['counts'], {'passed': 1, 'failed': 1, 'skipped': 1, 'not run': 2})
            self.assertEqual([p['outcome'] for p in report['programs']],
                             ['passed', 'failed', 'skipped', 'not run', 'not run'])

    def test_webview_requires_install_success(self):
        t = next(a for a in appcorpus.APPS if a.name == 'WebView2').tests[0]
        prefix = '[UM] Started cmd.exe (PID 1)\n'
        suffix = '\n[UM] cmd.exe (PID 1) exited with code 0\n'
        attempted = prefix + '[GoopdateImpl::DoInstall]\n' + suffix
        self.assertIsNotNone(verdict(t, attempted, True, 'cmd.exe'))
        self.assertIsNone(verdict(t, attempted + 'InstallApp returned 0x0\n', True, 'cmd.exe'))

    def test_webview_requires_every_host_milestone(self):
        t = next(a for a in appcorpus.APPS if a.name == 'WebView2').tests[1]
        prefix = '[UM] Started cmd.exe (PID 1)\n'
        suffix = '[UM] cmd.exe (PID 1) exited with code 0\n'
        lines = ['wv2host: runtime 1.2.3', 'wv2host: environment',
                 'wv2host: controller (browser process 2)', 'wv2host: navigation ok',
                 'wv2host: script "NovaOS WebView2 / 42"', 'wv2host: done']
        def check(items):
            return verdict(t, prefix + '\n'.join(items) + '\n' + suffix, True, 'cmd.exe')
        self.assertIsNone(check(lines))
        for i in range(len(lines)):
            with self.subTest(missing=lines[i]):
                self.assertIsNotNone(check(lines[:i] + lines[i + 1:]))
        self.assertIsNotNone(check(['wv2host: no runtime found (80070002)']))
        self.assertIsNotNone(check(lines[:2]))
        self.assertIsNotNone(check(lines[:-2] + ['wv2host: script "wrong / 42"', lines[-1]]))


if __name__ == '__main__':
    unittest.main()
