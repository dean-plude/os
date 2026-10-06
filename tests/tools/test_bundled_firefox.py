"""Verify installer integrity and compile the actual shell routing function."""
import hashlib
import importlib.util
import io
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('bundle_firefox', ROOT / 'tools/bundle_firefox.py')
ff = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ff)


class BundleTests(unittest.TestCase):
    def test_checksum_selection_is_exact_and_fails_closed(self):
        digest = 'a' * 128
        line = digest + '  ' + ff.INSTALLER + '\n'
        self.assertEqual(ff.installer_digest('b' * 128 + '  other.exe\n' + line), digest)
        for data in ['', line + line, 'invalid  ' + ff.INSTALLER]:
            with self.assertRaises(ValueError):
                ff.installer_digest(data)

    def test_verified_cache_and_failed_download_preserve_previous_file(self):
        good = b'unmodified installer fixture'
        digest = hashlib.sha512(good).hexdigest()
        with tempfile.TemporaryDirectory() as d:
            target = Path(d) / 'installer.exe'
            target.write_bytes(good)
            with patch.object(ff.urllib.request, 'urlopen', side_effect=AssertionError('cache redownloaded')):
                ff.download('https://example.invalid/installer', target, digest)
            for body in [b'', b'corrupt']:
                with patch.object(ff.urllib.request, 'urlopen', return_value=io.BytesIO(body)):
                    with self.assertRaises(ValueError):
                        ff.download('https://example.invalid/installer', target, 'b' * 128)
                self.assertEqual(target.read_bytes(), good)
                self.assertEqual(list(Path(d).iterdir()), [target])
            target.unlink()
            with patch.object(ff.urllib.request, 'urlopen', return_value=io.BytesIO(good)):
                ff.download('https://example.invalid/installer', target, digest)
            self.assertEqual(target.read_bytes(), good)

    def test_payload_includes_all_original_files_and_rejects_incomplete_or_symlink(self):
        with tempfile.TemporaryDirectory() as d:
            core = Path(d)
            with self.assertRaises(ValueError):
                ff.payload_files(core)
            for file in ['firefox.exe', 'xul.dll', 'license.html', 'browser/omni.ja']:
                path = core / file
                path.parent.mkdir(exist_ok=True)
                path.write_bytes(file.encode())
            files = ff.payload_files(core)
            self.assertEqual(len(files), 4)
            self.assertTrue(all(dest.startswith(ff.DEST + '\\') for dest, _ in files))
            self.assertTrue(any(dest.endswith('browser\\omni.ja') for dest, _ in files))
            (core / 'bad').symlink_to(core / 'xul.dll')
            with self.assertRaises(ValueError):
                ff.payload_files(core)

    def test_stage_keeps_payload_bytes_and_records_provenance(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            installer = b'installer'
            digest = hashlib.sha512(installer).hexdigest()
            def fetch(url, target, expected=None):
                if url.endswith('SHA512SUMS'):
                    Path(target).write_text(digest + '  ' + ff.INSTALLER + '\n')
                else:
                    self.assertEqual(expected, digest)
                    Path(target).write_bytes(installer)
            def extract(args, **kwargs):
                core = Path(next(a[2:] for a in args if a.startswith('-o'))) / 'core'
                core.mkdir()
                for name in ['firefox.exe', 'xul.dll', 'license.html']:
                    (core / name).write_bytes(name.encode())
            with patch.dict(ff.os.environ, NOVA_FIREFOX_CACHE=str(root / 'cache')), \
                 patch.object(ff, 'download', side_effect=fetch), \
                 patch.object(ff.shutil, 'which', return_value='/bin/7z'), \
                 patch.object(ff.subprocess, 'run', side_effect=extract):
                files = ff.stage(root / 'out')
            self.assertEqual(len(files), 3)
            for _, path in files:
                self.assertEqual(Path(path).read_bytes(), Path(path).name.encode())
            import json
            data = json.loads((root / 'out/firefox/provenance.json').read_text())
            self.assertEqual(data['sha512'], digest)
            self.assertEqual(data['version'], ff.VERSION)

    def test_disk_script_grows_for_bundled_payload_and_retains_minimum(self):
        # Run the actual image script; stub only external FAT utilities.
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            bindir = root / 'bin'
            bindir.mkdir()
            for name in ['mkdosfs', 'mformat', 'mmd', 'mcopy', 'mdir']:
                stub = bindir / name
                stub.write_text('#!/bin/sh\nexit 0\n')
                stub.chmod(0o755)
            boot = root / 'boot.efi'
            boot.write_bytes(b'boot')
            kernel = root / 'kernel.elf'
            image = root / 'nova.img'
            import os
            env = dict(os.environ, PATH=str(bindir) + os.pathsep + os.environ['PATH'])
            for payload_mb, expected_mb in [(1, 128), (200, 256), (300, 352)]:
                with kernel.open('wb') as f:
                    f.truncate(payload_mb * 1048576)
                subprocess.run(['bash', str(ROOT / 'scripts/create-disk.sh'),
                                str(image), str(boot), str(kernel)],
                               env=env, check=True, stdout=subprocess.DEVNULL)
                self.assertEqual(image.stat().st_size, expected_mb * 1048576)

    @unittest.skipUnless(shutil.which('cc'), 'host compiler unavailable')
    def test_shell_routes_web_and_html_to_firefox_retains_fallback_and_errors(self):
        source = (ROOT / 'userland/shell32/shell32.c').read_text()
        function = source[source.index('static int execute('):source.index('SHSTDAPI_(HINSTANCE) ShellExecuteW')]
        prelude = r'''
#include <assert.h>
#include <wchar.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define MAX_PATH 260
#define INVALID_FILE_ATTRIBUTES (~0u)
#define FALSE 0
#define CREATE_SUSPENDED 4
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_PATH_NOT_FOUND 3
#define ERROR_NOT_ENOUGH_MEMORY 8
typedef wchar_t WCHAR;
typedef const WCHAR *LPCWSTR;
typedef unsigned DWORD;
typedef unsigned char BYTE;
typedef int BOOL, NTSTATUS;
typedef void *HANDLE;
typedef struct { unsigned cb; } STARTUPINFOW;
typedef struct { HANDLE hProcess, hThread; } PROCESS_INFORMATION;
static int firefox = 1, netsurf = 1, creates, fail;
static WCHAR captured[2048];
static int starts_with(LPCWSTR s, const char *prefix) { while (*prefix) if (*s++ != *prefix++) return 0; return 1; }
static int ends_with(LPCWSTR s, const char *suffix) {
    size_t n = strlen(suffix), len = wcslen(s); return len >= n && starts_with(s + len - n, suffix);
}
static unsigned wlen(LPCWSTR s) { return wcslen(s); }
static void wcopy(WCHAR *a, LPCWSTR b) { wcscpy(a, b); }
static void u2w(const char *s, WCHAR *d, int n) { mbstowcs(d, s, n); }
static DWORD GetFileAttributesW(LPCWSTR p) {
    if (wcsstr(p, L"firefox.exe")) return firefox ? 0 : INVALID_FILE_ATTRIBUTES;
    if (wcsstr(p, L"netsurf.exe")) return netsurf ? 0 : INVALID_FILE_ATTRIBUTES;
    return wcsstr(p, L"missing") ? INVALID_FILE_ATTRIBUTES : 0;
}
static int lstrcmpiW(LPCWSTR a, LPCWSTR b) { return wcscmp(a, b); }
static HANDLE elevated_token(void) { return 0; }
static DWORD GetLastError(void) { return ERROR_FILE_NOT_FOUND; }
static int CloseHandle(HANDLE h) { (void)h; return 1; }
static void NtClose(HANDLE h) { (void)h; }
static NTSTATUS NtSetInformationProcess(HANDLE p, int c, void *v, unsigned n) { (void)p;(void)c;(void)v;(void)n;return 0; }
static void TerminateProcess(HANDLE p, int s) { (void)p;(void)s; }
static void ResumeThread(HANDLE h) { (void)h; }
static BOOL CreateProcessW(void *app, WCHAR *cmd, void *a, void *b, int inherit, int flags,
                          void *env, LPCWSTR dir, STARTUPINFOW *si, PROCESS_INFORMATION *pi) {
    (void)app;(void)a;(void)b;(void)inherit;(void)flags;(void)env;(void)dir;(void)si;
    wcscpy(captured, cmd); creates++; pi->hProcess = (void *)1; pi->hThread = (void *)2; return !fail;
}
'''
        main = r'''
int main(void) {
    assert(execute(0, L"https://example.org/a?q=1&b=2", 0, 0, 0) == 0);
    assert(!wcscmp(captured, L"\"C:\\Programs\\Mozilla Firefox\\core\\firefox.exe\" \"https://example.org/a?q=1&b=2\""));
    assert(execute(0, L"http://example.org/", 0, 0, 0) == 0);
    assert(wcsstr(captured, L"firefox.exe"));
    assert(execute(0, L"C:\\My Documents\\page.html", 0, 0, 0) == 0);
    assert(wcsstr(captured, L"\"C:\\My Documents\\page.html\""));
    firefox = 0;
    assert(execute(0, L"https://example.org/", 0, 0, 0) == 0);
    assert(wcsstr(captured, L"NetSurf\\netsurf.exe"));
    netsurf = 0;
    int before = creates;
    assert(execute(0, L"https://example.org/", 0, 0, 0) == 31 && creates == before);
    firefox = netsurf = 1;
    assert(execute(0, L"https://example.org/\" -bad", 0, 0, 0) == 31 && creates == before);
    WCHAR big[2200]; wmemset(big, L'a', 2199); big[2199] = 0;
    wmemcpy(big, L"https://", 8);
    assert(execute(0, big, 0, 0, 0) == 31 && creates == before);
    fail = 1;
    assert(execute(0, L"https://example.org/", 0, 0, 0) == 2);
    puts("Firefox/default/fallback/quoted HTML/missing/oversize/launch failures pass");
}
'''
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'routing.c'; path.write_text(prelude + function + main)
            binary = Path(d) / 'routing'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(path), '-o', str(binary)],
                           check=True, capture_output=True, text=True)
            subprocess.run([str(binary)], check=True, capture_output=True, timeout=10)
