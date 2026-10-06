"""The compatibility fixtures must consume the same JSON catalog as the OS."""
import json
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
