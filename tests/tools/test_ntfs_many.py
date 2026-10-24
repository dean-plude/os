"""Many files on one NTFS volume, on the host: the real kernel/fs/ntfs.c on
a RAM disk (tests/host/ntfs_many.c), formatted as Setup does and as mkntfs
does.  40,000 files must all be made (the MFT grows past its first bitmap
cluster, in few extents); files that fill their record must take a long new
name and a link (their contents move to clusters); all must mount again
whole and pass scripts/ntfs-check.py."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
MKNTFS = shutil.which('mkntfs') or ('/usr/sbin/mkntfs' if Path('/usr/sbin/mkntfs').exists() else None)


class NtfsManyFilesTests(unittest.TestCase):
    def run_many(self, d, *image):
        exe = str(Path(d) / 'ntfs_many')
        subprocess.run(['gcc', '-O2', '-w', f'-I{ROOT / "kernel"}', f'-I{ROOT / "include"}',
                        str(ROOT / 'tests/host/ntfs_many.c'), '-o', exe], check=True, capture_output=True)
        checker = f'{sys.executable} -I {ROOT / "scripts/ntfs-check.py"}'
        r = subprocess.run([exe, checker, str(Path(d) / 'out.img'), *image], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout[-3000:] + r.stderr[-1000:])
        self.assertIn('ntfs_many: 40000 files, 0 bad', r.stdout)
        self.assertRegex(r.stdout, r'0 error\(s\)')
        # it grew in few steps, not 64 records at a time
        self.assertLess(r.stdout.count('the MFT grew'), 20, r.stdout[-3000:])

    @unittest.skipUnless(shutil.which('gcc'), 'host GCC required')
    def test_setup_format(self):
        with tempfile.TemporaryDirectory() as d:
            self.run_many(d)

    @unittest.skipUnless(shutil.which('gcc') and MKNTFS, 'host GCC and mkntfs required')
    def test_mkntfs_format(self):
        with tempfile.TemporaryDirectory() as d:
            img = Path(d) / 'mkntfs.img'
            with open(img, 'wb') as f:
                f.truncate(1 << 30)
            subprocess.run([MKNTFS, '-F', '-Q', '-p', '0', '-H', '255', '-S', '63', str(img)], check=True,
                           capture_output=True)
            self.run_many(d, str(img))


if __name__ == '__main__':
    unittest.main()
