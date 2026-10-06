"""The compatibility fixtures must consume the same JSON catalog as the OS."""
import json
import os
import subprocess
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import appcorpus
from ci import plan


class CorpusStoreCatalogTests(unittest.TestCase):
    def test_all_store_apps_and_runtime_fixtures_resolve_from_json(self):
        for app in appcorpus.APPS:
            if app.store:
                with self.subTest(app=app.name):
                    self.assertTrue(appcorpus.catalog_file(app))
            for runtime in app.runtimes:
                with self.subTest(runtime=runtime):
                    url, file = appcorpus.catalog_entry(runtime)
                    self.assertTrue(url.startswith('https://'))
                    self.assertTrue(file)

    def test_missing_duplicate_schema_and_unsafe_filename_fail_before_staging(self):
        entry = {'name': 'Fixture', 'url': 'https://example.org/a.zip', 'file': 'a.zip'}
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'catalog.json'
            with patch.object(appcorpus, 'STORE_JSON', str(path)):
                for catalog in ({'schema_version': 2, 'apps': [entry]},
                                {'schema_version': 1, 'apps': []},
                                {'schema_version': 1, 'apps': [entry, entry]},
                                {'schema_version': 1, 'apps': [dict(entry, file='../a.zip')]}):
                    path.write_text(json.dumps(catalog))
                    with self.assertRaises(RuntimeError):
                        appcorpus.catalog_entry('Fixture')
                path.write_text(json.dumps({'schema_version': 1, 'apps': [entry]}))
                self.assertEqual(appcorpus.catalog_entry('Fixture'), (entry['url'], entry['file']))

    def test_catalog_edits_select_native_compatibility_checks(self):
        for path in ('userland/store/catalog.json', 'kernel/apps/store_catalog.h'):
            self.assertEqual(plan.scope('pull_request', [path]), {'code': True, 'corpus': True})

    def test_graphics_staging_downloads_json_catalog_runtimes(self):
        root = Path(__file__).resolve().parents[2]
        with tempfile.TemporaryDirectory() as d:
            work = Path(d)
            bindir = work / 'bin'
            bindir.mkdir()
            curl = bindir / 'curl'
            curl.write_text("""#!/usr/bin/env python3
import os, pathlib, sys
args = sys.argv[1:]
pathlib.Path(args[args.index('-o') + 1]).write_bytes(b'archive fixture')
with open(os.environ['FIXTURE_DOWNLOAD_LOG'], 'a') as f:
    f.write(args[-1] + '\\n')
""")
            curl.chmod(0o755)
            for name in ('7z', 'x86_64-w64-mingw32-gcc', 'i686-w64-mingw32-gcc'):
                stub = bindir / name
                stub.write_text('#!/bin/sh\nexit 0\n')
                stub.chmod(0o755)
            venus = work / 'venus.7z'
            venus.write_bytes(b'venus fixture')
            log = work / 'downloads.log'
            out, cache = work / 'gfx', work / 'cache'
            env = dict(os.environ, PATH=str(bindir) + os.pathsep + os.environ['PATH'],
                       VENUS_7Z=str(venus), FIXTURE_DOWNLOAD_LOG=str(log))
            subprocess.run(['bash', str(root / 'tools/ci/stage-graphics.sh'), str(out), str(cache)],
                           env=env, check=True, stdout=subprocess.DEVNULL)
            urls = log.read_text().splitlines()
            for name in ('Mesa 3D', 'DXVK'):
                url, file = appcorpus.catalog_entry(name)
                self.assertIn(url, urls)
                self.assertTrue((out / 'downloads' / url.rsplit('/', 1)[-1]).is_file())
            self.assertEqual(len(urls), 3)
            self.assertTrue((out / 'downloads/venus.7z').is_file())
