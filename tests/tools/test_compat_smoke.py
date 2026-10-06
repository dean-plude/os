from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import compat_smoke as smoke
from selftest import verdict


class SmokeTests(unittest.TestCase):
    def test_scope(self):
        for path in ('kernel/um/um.c', 'kernel/ke/scheduler.c', 'kernel/mm/pmm.c',
                     'userland/kernel32/sync.c', 'userland/ntdll/ntdll_ldr.c',
                     'include/windows.h', 'tools/compat_smoke.py', '.github/workflows/ci.yml'):
            self.assertTrue(smoke.required([path]), path)
        self.assertFalse(smoke.required(['docs/history/change.md', 'README.md', 'LICENSE']))

    def test_selection_is_explicit_bounded_and_keeps_assertions(self):
        tests = smoke.selected()
        self.assertEqual(tuple(t.name for t in tests), smoke.NAMES)
        self.assertLessEqual(sum(t.timeout for t in tests), 1260)
        for t in tests:
            exe = t.cmd.split('\\')[-1].split()[0]
            if not exe.endswith('.exe'):
                exe += '.exe'
            start_only = f'[UM] Started {exe} (PID 1)\n[UM] {exe} (PID 1) exited with code 0\n'
            self.assertIsNotNone(verdict(t, start_only, True, exe), t.name)
        with self.assertRaises(ValueError):
            smoke.selected(tests[:-1])
        with self.assertRaises(ValueError):
            smoke.selected(tests + tests[:1])


if __name__ == '__main__':
    unittest.main()
