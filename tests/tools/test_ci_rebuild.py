"""Exercise CI decisions, actual CLI exit codes, provenance and the job graph."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import yaml

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import appcorpus
from selftest import Test


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    m = importlib.util.module_from_spec(spec)
    if name == 'nstest':
        m.Test = Test
    spec.loader.exec_module(m)
    return m


plan = module('ci_plan', 'tools/ci/plan.py')
results = module('ci_results', 'tools/ci/results.py')
manifest = module('ci_manifest', 'tools/ci/image_manifest.py')
kvm = module('ci_kvm', 'tools/ci/probe_kvm.py')
corpus = module('ci_corpus', 'tools/ci/corpus_report.py')
ns = module('nstest', 'tests/selftest/graphics/060-nstest.py')


class CIRebuildTests(unittest.TestCase):
    def test_scope_docs_queues_unknown_and_corpus_infrastructure(self):
        for event in ('pull_request', 'merge_group'):
            self.assertEqual(plan.scope(event, ['docs/building.md', 'README.md']),
                             {'code': False, 'corpus': False})
            for paths in ([], ['new-input.bin'], ['kernel/ke/sched.c']):
                self.assertTrue(plan.scope(event, paths)['code'])
            for path in ('tools/ci/plan.py', '.github/workflows/build.yml',
                         'tests/reference/Firefox.png', 'tools/bundle_firefox.py',
                         'kernel/apps/store.c', 'tests/tools/test_ci_rebuild.py'):
                self.assertTrue(plan.scope(event, [path])['corpus'])
            self.assertFalse(plan.scope(event, ['kernel/ke/sched.c'])['corpus'])
        for event in ('push', 'workflow_dispatch', 'schedule'):
            self.assertEqual(plan.scope(event, ['README.md']), {'code': True, 'corpus': True})

    def test_diff_uses_event_base_for_pr_and_merge_queue(self):
        for event, payload in [('pull_request', {'pull_request': {'base': {'sha': 'pr-base'}}}),
                               ('merge_group', {'merge_group': {'base_sha': 'queue-base'}})]:
            expected = 'pr-base' if event == 'pull_request' else 'queue-base'
            with patch.object(plan.subprocess, 'check_output', return_value=b'kernel/a.c\0deleted.c\0') as call:
                self.assertEqual(plan.changed(event, payload), ['kernel/a.c', 'deleted.c'])
                self.assertEqual(call.call_args.args[0], ['git', 'diff', '--name-only', '-z', expected, 'HEAD'])
        with self.assertRaises(KeyError):
            plan.changed('pull_request', {})

    def test_raw_serial_survives_vm_cleanup_and_boot_failure(self):
        import novarun
        import shutil
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); work = root / 'vm'; evidence = root / 'reports/raw.log'
            command = []
            def launch(args, **kwargs):
                command.extend(args)
                serial = Path(args[args.index('-serial') + 1][5:])
                serial.write_text('kernel boot diagnostics\n')
                return SimpleNamespace()
            def close(vm):
                shutil.rmtree(vm.work)
            with patch.object(novarun, 'make_data'), \
                 patch.object(novarun.subprocess, 'Popen', side_effect=launch), \
                 patch.object(novarun.Nova, 'start', side_effect=RuntimeError('boot failed')), \
                 patch.object(novarun.Nova, 'close', close):
                with self.assertRaises(RuntimeError):
                    novarun.Nova(img=False, work=str(work), serial_path=str(evidence))
            self.assertFalse(work.exists())
            self.assertEqual(evidence.read_text(), 'kernel boot diagnostics\n')
            self.assertIn('file:' + str(evidence), command)

    def test_every_app_is_selected_exactly_once_and_unknown_selection_fails(self):
        matrix = plan.app_matrix(appcorpus.APPS)['include']
        self.assertEqual([e['app'] for e in matrix], [a.name for a in appcorpus.APPS])
        for entry in matrix:
            self.assertEqual(len(appcorpus.selected_apps(appcorpus.APPS, entry['app'])), 1)
        for names in ('', 'Firefox,Firefox', 'Typo', 'Firefox,Typo', ',Firefox'):
            with self.assertRaises(ValueError):
                appcorpus.selected_apps(appcorpus.APPS, names)
        for apps in ([], [SimpleNamespace(name='a'), SimpleNamespace(name='a')]):
            with self.assertRaises(ValueError):
                plan.app_matrix(apps)

    def test_final_results_reject_missing_cancelled_skipped_and_bad_scope(self):
        for mode, (first, output, jobs) in results.JOBS.items():
            for selected in ('true', 'false'):
                needs = {first: {'result': 'success', 'outputs': {output: selected}},
                         **{j: {'result': 'success' if selected == 'true' or j == 'boot-result' else 'skipped'} for j in jobs}}
                self.assertTrue(results.evaluate(mode, needs)[0])
                for job in (first, *jobs):
                    for bad in ('failure', 'cancelled', 'missing', 'unexpected',
                                'skipped' if needs[job]['result'] == 'success' else 'success'):
                        changed = json.loads(json.dumps(needs))
                        changed[job]['result'] = bad
                        self.assertFalse(results.evaluate(mode, changed)[0], (mode, job, bad))
                    changed = dict(needs); del changed[job]
                    self.assertFalse(results.evaluate(mode, changed)[0])
                for bad in (None, '', 'TRUE', True):
                    changed = json.loads(json.dumps(needs))
                    changed[first]['outputs'][output] = bad
                    self.assertFalse(results.evaluate(mode, changed)[0])
            for value in (None, [], {'checks': []}):
                self.assertFalse(results.evaluate(mode, value)[0])
        for raw in ('invalid json', '{}'):
            r = subprocess.run([sys.executable, str(ROOT / 'tools/ci/results.py'), 'ci'],
                               env=dict(os.environ, NEEDS_JSON=raw), capture_output=True)
            self.assertEqual(r.returncode, 1)

    def test_image_verification_rejects_corruption_commit_mismatch_and_missing_file(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            for name in manifest.FILES:
                (root / name).write_bytes(name.encode())
            manifest.stamp(root, 'commit-a')
            manifest.verify(root, 'commit-a')
            with self.assertRaises(ValueError):
                manifest.verify(root, 'commit-b')
            (root / 'nova.img').write_bytes(b'corrupt')
            with self.assertRaises(ValueError):
                manifest.verify(root, 'commit-a')
            (root / 'nova.img').unlink()
            with self.assertRaises(FileNotFoundError):
                manifest.verify(root, 'commit-a')

    def test_kvm_probe_requires_supported_api_and_actual_vm(self):
        with tempfile.NamedTemporaryFile() as f:
            with patch.object(kvm.fcntl, 'ioctl', side_effect=[12, 321]) as call, \
                 patch.object(kvm.os, 'close') as close:
                self.assertTrue(kvm.probe(f.name))
                self.assertEqual(call.call_count, 2)
                close.assert_called_once_with(321)
            for answer in ([11], [12, OSError('KVM denied')]):
                with patch.object(kvm.fcntl, 'ioctl', side_effect=answer):
                    self.assertFalse(kvm.probe(f.name))
        self.assertFalse(kvm.probe('/nonexistent/kvm'))

    def test_corpus_aggregation_rejects_missing_wrong_and_inconsistent_reports(self):
        matrix = {'include': [{'id': '01', 'app': 'Firefox'}]}
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            with self.assertRaises(FileNotFoundError):
                corpus.collect(root, matrix)
            out = root / 'appcorpus-01'; out.mkdir()
            counts = dict.fromkeys(('passed', 'failed', 'skipped', 'not run'), 0)
            for status in counts:
                data = {'counts': dict(counts, **{status: 1}),
                        'programs': [{'name': 'Firefox', 'version': '157', 'outcome': status, 'reason': None}]}
                file = out / 'results.json'; file.write_text(json.dumps(data))
                self.assertEqual(corpus.collect(root, matrix)[0]['outcome'], status)
            for field, bad in [('name', 'WebView2'), ('outcome', 'success')]:
                data['programs'][0][field] = bad
                file.write_text(json.dumps(data))
                with self.assertRaises(ValueError):
                    corpus.collect(root, matrix)
                data['programs'][0][field] = 'Firefox' if field == 'name' else 'not run'
            data['counts']['passed'] = 1; file.write_text(json.dumps(data))
            with self.assertRaises(ValueError):
                corpus.collect(root, matrix)
            with self.assertRaises(ValueError):
                corpus.collect(root, {'include': []})

    def test_graph_builds_once_shares_verified_image_and_never_masks_failures(self):
        workflows = {p.stem: yaml.safe_load(p.read_text()) for p in (ROOT / '.github/workflows').glob('*.yml')}
        ci, nightly, build = (workflows[n] for n in ('ci', 'nightly', 'build'))
        for workflow, result, prerequisites in ((ci, 'ci-result', ['checks','build','compatibility','boot-test','graphics-test','boot-result']),
                                               (nightly, 'corpus-result', ['gate','build','app-corpus','smp'])):
            jobs = workflow['jobs']
            self.assertEqual(jobs[result]['needs'], prerequisites)
            self.assertEqual(jobs[result]['if'], 'always()')
            self.assertEqual(jobs['build']['uses'], './.github/workflows/build.yml')
            for name in [n for n in prerequisites[2:] if n != 'boot-result']:
                steps = jobs[name]['steps']
                self.assertTrue(any('image_manifest.py verify' in s.get('run', '') for s in steps))
                self.assertFalse(any('make -C build' in s.get('run', '') for s in steps))
                self.assertFalse(any(s.get('continue-on-error') for s in steps))
        self.assertFalse(ci['jobs']['boot-test']['strategy']['fail-fast'])
        self.assertFalse(nightly['jobs']['app-corpus']['strategy']['fail-fast'])
        self.assertEqual(nightly['jobs']['app-corpus']['strategy']['max-parallel'], 4)
        self.assertEqual(ci['jobs']['publish-iso']['needs'], ['ci-result'])
        self.assertEqual(ci['jobs']['boot-result']['name'], 'Build and boot-test')
        self.assertEqual(ci['jobs']['boot-result']['needs'], ['checks', 'build', 'boot-test'])
        artifacts = [s['with']['name'] for s in build['jobs']['image']['steps'] if s.get('uses') == 'actions/upload-artifact@v4']
        self.assertIn('nova-iso', artifacts); self.assertIn('nova-boot', artifacts)
        self.assertIn('merge_group', ci['on']); self.assertIn('workflow_call', ci['on'])
        self.assertNotIn('paths', nightly['on']['pull_request'])
        self.assertEqual(nightly['permissions'], {'contents':'read','actions':'read'})

    def test_netsurf_ignores_desktop_but_checks_every_page_pixel_and_geometry(self):
        width, height = 1280, 800
        original = [bytes(width * 3) for _ in range(height)]
        bounds = (100, 100, 500, 500)
        def change(x, y):
            rows = list(original); row = bytearray(rows[y]); row[x*3] = 255; rows[y] = bytes(row)
            return rows
        for x, y, different in [(28,66,False), (110,590,False), (110,110,True), (599,581,True)]:
            with patch.object(ns, 'png_rgb', side_effect=[(width,height,original),(width,height,change(x,y))]):
                self.assertEqual(ns.same_page('a','b',bounds) is not None, different)
        for bad in (None,(0,0,0,500),(-1,0,500,500),(100,100,1500,500)):
            with patch.object(ns, 'png_rgb', return_value=(width,height,original)):
                self.assertIsNotNone(ns.same_page('a','b',bad))
        # Verify the excluded strip matches NetSurf's actual default furniture.
        self.assertIn('NSOPTION_INTEGER(fb_furniture_size, 18)',
                      (ROOT/'third_party/netsurf/netsurf/frontends/framebuffer/options.h').read_text())

    @unittest.skipUnless(__import__('shutil').which('cc'), 'host compiler unavailable')
    def test_actual_tcg_detection_decodes_little_endian_cpuid(self):
        source = (ROOT/'userland/programs/prioritytest.c').read_text()
        function = source[source.index('static int under_tcg(void)'):source.index('static int check_network(void)')]
        program = '#include <assert.h>\n#include <string.h>\nstatic unsigned regs[3];\n'
        program += '#define __cpuid(leaf,a,b,c,d) do { (void)(leaf); (a)=0; (b)=regs[0]; (c)=regs[1]; (d)=regs[2]; } while(0)\n'
        program += function + '\nint main(void) { memcpy(regs,"TCGTCGTCGTCG",12); assert(under_tcg()); memcpy(regs,"KVMKVMKVM\\0\\0\\0",12); assert(!under_tcg()); }\n'
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)/'probe.c'; p.write_text(program)
            subprocess.run(['cc', '-Wall', '-Wextra', str(p), '-o', str(Path(d)/'probe')], check=True, capture_output=True)
            subprocess.run([str(Path(d)/'probe')], check=True)


if __name__ == '__main__':
    unittest.main()
