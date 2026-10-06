"""Compile the installer sizing/layout code, including undersized disk guards."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class InstallerLayoutTests(unittest.TestCase):
    def test_payload_growth_update_reserve_alignment_and_small_disks(self):
        source = (ROOT / 'kernel/fs/setup.c').read_text()
        start = source.index('static UINT64 setup_esp_sectors(')
        end = source.index('/* Write a protective MBR', start)
        fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
typedef uint64_t UINT64;
typedef uint32_t UINT32;
#define GPT_TABLE_SEC 32
static bool g_ntfs;
'''+source[start:end]+r'''
int main(void) {
    UINT64 esp, data, size;
    UINT64 sectors = setup_esp_sectors(189u<<20, 84u<<10);
    assert(sectors == 416ull*2048);
    assert(setup_esp_sectors(1u<<20, 84u<<10) == 128ull*2048);
    assert(setup_layout(2ull*1024*1024*1024/512, sectors, &esp, &data, &size));
    assert(esp==2048 && data==2048+sectors && data%2048==0 && size%2048==0);
    assert((sectors*512) > 2ull*((189ull<<20)+(84ull<<10))+(32ull<<20));
    assert(data+size < 2ull*1024*1024*1024/512-32);
    for(UINT64 tiny=0; tiny<4096; tiny++)
        assert(!setup_layout(tiny, sectors, &esp, &data, &size));
    assert(!setup_layout(256ull*2048, sectors, &esp, &data, &size));
    assert(!setup_layout(480ull*2048, sectors, &esp, &data, &size));
    assert(!setup_layout(UINT64_MAX, UINT64_MAX, &esp, &data, &size));
    assert(setup_layout(4ull<<32, sectors, &esp, &data, &size) && size==(1ull<<31));
    g_ntfs=true;
    assert(setup_layout(4ull<<32, sectors, &esp, &data, &size) && size>(1ull<<31));
    assert(setup_esp_sectors(UINT32_MAX, UINT32_MAX) > 2ull*UINT32_MAX/512);
}
'''
        with tempfile.TemporaryDirectory() as d:
            src, exe = Path(d)/'layout.c', Path(d)/'layout'
            src.write_text(fixture)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=undefined', str(src), '-o', str(exe)],
                           check=True, capture_output=True)
            subprocess.run([str(exe)], check=True, capture_output=True)
