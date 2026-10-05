import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from download_cache import DownloadCache


class CacheTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.now = 100000
        self.calls = 0
        self.payload = b'installer one'
        self.cache = DownloadCache(self.tmp.name, clock=lambda: self.now,
                                   journal=Path(self.tmp.name) / 'downloads.json')
        def download(args, **kwargs):
            self.calls += 1
            Path(args[args.index('-o') + 1]).write_bytes(self.payload)
            return SimpleNamespace(stdout='https://cdn.example/154.0.4258.53/setup.exe')
        self.mock = patch('download_cache.subprocess.run', side_effect=download).start()
        self.addCleanup(patch.stopall)

    def test_url_identity_includes_host_path_and_query(self):
        urls = ['https://a.example/setup.exe', 'https://b.example/setup.exe',
                'https://a.example/setup.exe?v=2', 'https://a.example/other/setup.exe']
        self.assertEqual(len({self.cache.fetch(u) for u in urls}), 4)
        self.assertEqual(self.calls, 4)

    def test_mutable_refreshes_at_expiry_pinned_does_not(self):
        url = 'https://example/current'
        path = self.cache.fetch(url, mutable=True)
        self.now += 86399
        self.cache.fetch(url, mutable=True)
        self.assertEqual(self.calls, 1)
        self.now += 1
        self.payload = b'installer two'
        self.cache.fetch(url, mutable=True)
        self.assertEqual(Path(path).read_bytes(), self.payload)
        fixed = 'https://example/1.2.3/setup.exe'
        self.cache.fetch(fixed, version='1.2.3')
        self.now += 999999
        self.cache.fetch(fixed, version='1.2.3')
        self.assertEqual(self.calls, 3)

    def test_failed_or_empty_refresh_preserves_previous_bytes_and_metadata(self):
        url = 'https://example/current'
        path = self.cache.fetch(url, mutable=True)
        before = (Path(self.tmp.name) / 'downloads.json').read_bytes()
        self.now += 86400
        for failure in (subprocess.CalledProcessError(22, 'curl'), None):
            self.mock.side_effect = failure
            if failure is None:
                self.mock.side_effect = lambda args, **kw: SimpleNamespace(stdout='')
            with self.assertRaises((subprocess.CalledProcessError, ValueError)):
                self.cache.fetch(url, mutable=True)
            self.assertEqual(Path(path).read_bytes(), b'installer one')
            self.assertEqual((Path(self.tmp.name) / 'downloads.json').read_bytes(), before)
        self.assertFalse(list(Path(self.tmp.name).glob('.download-*')))

    def test_journal_records_provenance_and_lock_replays_retained_bytes(self):
        url = 'https://example/current'
        self.cache.fetch(url, mutable=True, version='evergreen')
        lock = Path(self.tmp.name) / 'lock.json'
        lock.write_bytes((Path(self.tmp.name) / 'downloads.json').read_bytes())
        record = json.loads(lock.read_text())['downloads'][0]
        self.assertEqual(record['source_url'], url)
        self.assertEqual(record['sha256'], hashlib.sha256(b'installer one').hexdigest())
        self.assertEqual(record['resolved_version'], '154.0.4258.53')
        self.now += 86400
        self.payload = b'installer two'
        self.cache.fetch(url, mutable=True)
        replay = DownloadCache(self.tmp.name, lock=lock)
        path = replay.fetch(url, mutable=True)
        self.assertEqual(Path(path).read_bytes(), b'installer one')
        self.assertEqual(self.calls, 2)
        with self.assertRaisesRegex(ValueError, 'no entry'):
            replay.fetch('https://example/unknown')
        with tempfile.TemporaryDirectory() as empty:
            replay = DownloadCache(empty, lock=lock)
            with self.assertRaisesRegex(ValueError, 'hash mismatch'):
                replay.fetch(url)
            self.assertFalse(list((Path(empty) / 'blobs').iterdir()))

    def test_observed_version_is_recorded_with_evidence(self):
        url = 'https://example/current'
        self.cache.fetch(url, mutable=True)
        self.cache.observe_version(url, '155.1.2.3', 'wv2host runtime discovery')
        record = json.loads((Path(self.tmp.name) / 'downloads.json').read_text())['downloads'][0]
        self.assertEqual(record['resolved_version'], '155.1.2.3')
        self.assertEqual(record['version_evidence'], 'wv2host runtime discovery')

    def test_corrupt_blob_is_not_reused(self):
        url = 'https://example/pinned.exe'
        path = self.cache.fetch(url)
        Path(path).write_bytes(b'corrupt')
        self.cache.fetch(url)
        self.assertEqual(self.calls, 2)
        self.assertEqual(Path(path).read_bytes(), self.payload)


if __name__ == '__main__':
    unittest.main()
