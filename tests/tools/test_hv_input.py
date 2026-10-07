"""The VMBus ring buffer and the Hyper-V keyboard and mouse protocols, on the host."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

class HvInputTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('gcc'), 'host GCC required')
    def test_ring_keyboard_and_mouse(self):
        with tempfile.TemporaryDirectory() as d:
            executable = str(Path(d) / 'hv_input')
            includes = ['kernel', 'include', 'third_party/lwip/src/include', 'kernel/net/port',
                        'third_party/mbedtls/include', 'third_party/uacpi/include']
            subprocess.run(['gcc', '-ffunction-sections', '-fdata-sections',
                            *[f'-I{ROOT / x}' for x in includes],
                            str(ROOT / 'tests/host/hv_input.c'), '-Wl,--gc-sections',
                            '-o', executable], check=True, capture_output=True)
            result = subprocess.run([executable], check=True, capture_output=True, text=True)
            self.assertIn('hv_input: all passed', result.stdout)

if __name__ == '__main__':
    unittest.main()
