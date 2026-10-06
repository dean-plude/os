"""Run the native Store catalog parser and refresh logic on a host compiler."""
import copy
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class StoreCatalogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.work = Path(cls.tmp.name)
        compiler = shutil.which('cc')
        if not compiler:
            raise RuntimeError('A C compiler is required for the Store catalog tests')
        # Compile the production URL parser without the networking engine.
        http = (ROOT / 'kernel/net/http.c').read_text()
        url = http[http.index('bool NetParseUrl('):]
        (cls.work / 'url.c').write_text('#include <stdint.h>\n#undef __always_inline\n#include "kernel/net/net.h"\n#include "kernel/lib/string.h"\n' + url)
        cls.binary = cls.work / 'store-test'
        subprocess.run([compiler, '-std=c11', '-fcommon', '-fshort-wchar', '-ffunction-sections',
                        '-fdata-sections', '-Wall', '-Wextra', '-Wno-unused-parameter',
                        '-Werror=implicit-function-declaration', '-I' + str(ROOT),
                        '-I' + str(ROOT / 'kernel'), '-I' + str(ROOT / 'include'),
                        str(ROOT / 'tests/unit/store_catalog.c'), str(cls.work / 'url.c'),
                        '-fsanitize=undefined', '-Wl,--gc-sections', '-o', str(cls.binary)], check=True)
        cls.bundled = json.loads((ROOT / 'userland/store/catalog.json').read_text())
        cls.catalog = {'schema_version': 1, 'apps': [
            {'name': '7-Zip', 'publisher': 'Igor Pavlov', 'summary': 'File archiver',
             'category': 'utilities', 'url': 'https://www.7-zip.org/a/7z2603-x64.exe',
             'file': '7z2603-x64.exe', 'dest': None, 'exe': '7-Zip\\7zFM.exe',
             'kind': 'setup', 'size_mb': 2, 'note': 'Tested', 'label': '7z', 'color': '#256EB8'},
            {'name': 'Example Archive', 'publisher': 'Example', 'summary': 'Archive package',
             'category': 'media', 'url': 'https://example.org/app.zip', 'file': 'app.zip',
             'dest': 'Example', 'exe': 'Example\\**\\app.exe', 'kind': 'archive',
             'size_mb': 60, 'note': 'Testing pending', 'label': 'Ex', 'color': '#F07E1A'}]}

    def run_catalog(self, catalog, valid=True):
        p = self.work / 'candidate.json'
        p.write_bytes(catalog if isinstance(catalog, bytes) else json.dumps(catalog).encode())
        r = subprocess.run([str(self.binary), str(p)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0 if valid else 1, r.stderr)
        return r.stdout.splitlines()

    def test_bundled_catalog_preserves_apps(self):
        rows = self.run_catalog(self.bundled)
        self.assertEqual(rows, [str(len(self.bundled['apps']))] + [app['name'] for app in self.bundled['apps']])

    def test_edit_add_remove_reorder_and_empty_list(self):
        catalog = copy.deepcopy(self.catalog)
        catalog['apps'] = catalog['apps'][::-1]
        app = copy.deepcopy(catalog['apps'][0])
        app.update(name='New catalog-only app', file='new-app.zip')
        catalog['apps'].insert(0, app)
        catalog['apps'].pop()
        self.assertEqual(self.run_catalog(catalog)[1], 'New catalog-only app')
        self.run_catalog({'schema_version': 1, 'apps': []})

    def test_json_escapes_unicode_and_unknown_metadata(self):
        catalog = copy.deepcopy(self.catalog)
        catalog['apps'][0]['name'] = 'München "editor" 🚀'
        catalog['future_metadata'] = {'a': [True, False, None, -2.5e30]}
        catalog['apps'][0]['extra'] = {'nested': ['ok', {}]}
        self.assertEqual(self.run_catalog(catalog)[1], catalog['apps'][0]['name'])

    def test_malformed_truncated_duplicate_and_unsupported_schema(self):
        raw = json.dumps(self.catalog).encode()
        for value in [b'', b'{}', raw[:-1], raw + b'trailing', raw + b'\0',
                      raw.replace(b'7-Zip', b'\xc0\x80'),
                      raw.replace(b'7-Zip', b'\xed\xa0\x80'),
                      b'{"schema_version":1,"schema_version":1,"apps":[]}',
                      b'{"schema_version":1,"apps":[],}',
                      b'{"schema_version":1,"apps":[],"extra":[1,]}',
                      b'{"schema_version":1,"apps":[],"extra":"\\uD800"}',
                      b'{"schema_version":1,"apps":[],"extra":"\\u0000"}',
                      b'{"schema_version":01,"apps":[]}',
                      {'schema_version': 2, 'apps': []}]:
            with self.subTest(value=str(value)[:80]):
                self.run_catalog(value, False)
        self.run_catalog(raw.replace(b'"name": "7-Zip"', b'"name":"7-Zip","name":"other"'), False)

    def test_install_fields_paths_urls_and_limits(self):
        for field, value in [('category', 'unknown'), ('kind', 'command'), ('color', '#xxx000'),
                             ('size_mb', -1), ('size_mb', 2**32), ('size_mb', 1.2),
                             ('file', '../evil.exe'), ('file', '\\evil.exe'), ('file', 'x" -oevil'),
                             ('dest', '..'), ('dest', 'elsewhere\\nested'), ('dest', None),
                             ('exe', '..\\evil.exe'), ('exe', 'C:\\evil.exe'),
                             ('system', '-oC:\\evil'), ('system', 'x64\\..\\evil.dll'),
                             ('system', 'x64\\good.dll>..\\evil'), ('system', 'x64\\good.dll -y'),
                             ('url', 'http://example.org/app.exe'), ('url', 'https://'), ('url', 'https:///missing-host'),
                             ('name', 'x' * 81), ('note', 'x' * 513), ('name', 'bad\nname')]:
            with self.subTest(field=field, value=value):
                catalog = copy.deepcopy(self.catalog)
                catalog['apps'][1][field] = value
                self.run_catalog(catalog, False)
        catalog = copy.deepcopy(self.catalog)
        del catalog['apps'][0]['url']
        self.run_catalog(catalog, False)
        catalog = copy.deepcopy(self.catalog)
        catalog['apps'].append(copy.deepcopy(catalog['apps'][0]))
        self.run_catalog(catalog, False)
        catalog['apps'][-1]['name'] = 'different name'
        self.run_catalog(catalog, False)  # download filename collision
        self.run_catalog(b' ' * (512 * 1024 + 1), False)
        catalog = copy.deepcopy(self.catalog)
        catalog['future'] = [[[[[[[[[[[[[[[[[[[]]]]]]]]]]]]]]]]]]]
        self.run_catalog(catalog, False)
        catalog = copy.deepcopy(self.catalog)
        catalog['apps'] = []
        for i in range(257):
            app = copy.deepcopy(self.catalog['apps'][0])
            app.update(name=f'App {i}', file=f'app{i}.exe')
            catalog['apps'].append(app)
        self.run_catalog(catalog, False)

    def test_refresh_failure_atomic_save_busy_and_offline_fallback(self):
        subprocess.run([str(self.binary), str(ROOT / 'userland/store/catalog.json'), 'integration'], check=True)


if __name__ == '__main__':
    unittest.main()
