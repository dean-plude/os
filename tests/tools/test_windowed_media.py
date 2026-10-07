"""Exercise the production DirectDraw presentation helper on a host compiler."""
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import appcorpus


class WindowedMediaTests(unittest.TestCase):
    def test_normal_mode_uses_desktop_without_installing_game_hooks(self):
        source = (ROOT / 'third_party/cnc-ddraw/src/dd.c').read_text()
        start = source.index('#ifdef NOVAOS', source.index('HRESULT dd_SetCooperativeLevel('))
        end = source.index('#endif', start)
        normal = source[start:end] + '#endif\n'
        self.assertLess(end, source.index('        hook_init();', start))
        fixture = r'''
#include <assert.h>
#define NOVAOS 1
#define DDSCL_NORMAL 1
#define DDSCL_FULLSCREEN 2
#define DD_OK 0
#define FALSE 0
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
typedef unsigned DWORD;
static struct { unsigned width, height, bpp; int windowed_hack; } g_ddraw;
static unsigned real_GetSystemMetrics(int metric) { return metric ? 800 : 1280; }
static int setup(DWORD dwFlags) {
''' + normal + r'''
    return 42; /* Continue to the original fullscreen/game path. */
}
int main(void) {
    assert(setup(DDSCL_NORMAL)==DD_OK);
    assert(g_ddraw.width==1280 && g_ddraw.height==800 && g_ddraw.bpp==32);
    assert(!g_ddraw.windowed_hack);
    g_ddraw.width=640; g_ddraw.height=480; g_ddraw.bpp=16;
    assert(setup(DDSCL_FULLSCREEN)==42);
    assert(setup(DDSCL_NORMAL|DDSCL_FULLSCREEN)==42);
    assert(g_ddraw.width==640 && g_ddraw.height==480 && g_ddraw.bpp==16);
}
'''
        with tempfile.TemporaryDirectory() as d:
            src, exe = Path(d)/'normal.c', Path(d)/'normal'
            src.write_text(fixture)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            str(src), '-o', str(exe)], check=True, capture_output=True)
            subprocess.run([str(exe)], check=True, capture_output=True)

    def test_windowed_primary_presentation_clipping_and_failures(self):
        source = (ROOT / 'third_party/cnc-ddraw/src/ddsurface.c').read_text()
        start = source.index('static HRESULT nova_present_windowed_primary(')
        helper = source[start:source.index('\n#endif', start)]
        fixture = r'''
#include <assert.h>
#include <stddef.h>
#define DDSCAPS_PRIMARYSURFACE 1
#define DD_OK 0
#define DDERR_GENERIC -1
#define DIB_RGB_COLORS 0
#define SRCCOPY 1
#define NULL ((void *)0)
typedef int HRESULT;
typedef void *HWND;
typedef void *HDC;
typedef struct { int left, top, right, bottom; } RECT;
typedef struct { int x, y; } POINT;
typedef struct { struct { int biHeight; } bmiHeader; } BITMAPINFO;
typedef struct { HWND hwnd; } Clipper;
typedef struct { int caps; Clipper *clipper; void *surface; BITMAPINFO *bmi; unsigned width, height; } IDirectDrawSurfaceImpl;
static struct { struct { int run; } render; } g_ddraw;
static int presents, releases, fail_rect, fail_dc, fail_copy, ox=340, oy=172;
static int GetClientRect(HWND h, RECT *r) { *r=(RECT){0,0,485,240}; return !fail_rect; }
static int ClientToScreen(HWND h, POINT *p) { *p=(POINT){ox,oy}; return 1; }
static HDC GetDC(HWND h) { return fail_dc ? NULL : (void *)2; }
static int ReleaseDC(HWND h, HDC d) { releases++; return 1; }
static int real_StretchDIBits(HDC dc, int dx, int dy, int w, int h,
    int sx, int sy, int sw, int sh, void *bits, BITMAPINFO *bmi, int usage, int rop) {
    sy = 768 - sy - sh;                 /* (counted from the bottom, as Windows does) */
    assert(dx==sx-ox && dy==sy-oy && w==sw && h==sh);
    assert(sx>=0 && sy>=0 && sx+w<=1024 && sy+h<=768);
    if(ox==340) assert(sx==340 && sy==172 && w==485 && h==240);
    if(ox==-10) assert(sx==0 && dx==10 && w==475);
    if(ox==900) assert(w==124);
    assert(bits && bmi && rop==SRCCOPY); presents++;
    return fail_copy ? -1 : h;
}
'''
        fixture += helper + r'''
int main(void) {
    BITMAPINFO bmi={{-768}}; Clipper clipper={(void *)1};
    IDirectDrawSurfaceImpl s={1,&clipper,(void *)3,&bmi,1024,768};
    assert(nova_present_windowed_primary(&s)==DD_OK && presents==1 && releases==1);
    ox=-10; assert(nova_present_windowed_primary(&s)==DD_OK);
    ox=900; assert(nova_present_windowed_primary(&s)==DD_OK);
    ox=2000; assert(nova_present_windowed_primary(&s)==DD_OK && presents==3);
    ox=340; g_ddraw.render.run=1; assert(nova_present_windowed_primary(&s)==DD_OK && presents==3);
    g_ddraw.render.run=0; s.caps=0; assert(nova_present_windowed_primary(&s)==DD_OK && presents==3);
    s.caps=1; s.clipper=NULL; assert(nova_present_windowed_primary(&s)==DD_OK && presents==3);
    s.clipper=&clipper; fail_rect=1; assert(nova_present_windowed_primary(&s)==DDERR_GENERIC);
    fail_rect=0; fail_dc=1; assert(nova_present_windowed_primary(&s)==DDERR_GENERIC);
    fail_dc=0; fail_copy=1; assert(nova_present_windowed_primary(&s)==DDERR_GENERIC && releases==4);
}
'''
        with tempfile.TemporaryDirectory() as d:
            src, exe = Path(d) / 'present.c', Path(d) / 'present'
            src.write_text(fixture)
            subprocess.run(['cc', '-std=c11', '-Werror=implicit-function-declaration',
                            '-fsanitize=undefined', str(src), '-o', str(exe)], check=True,
                           capture_output=True)
            subprocess.run([str(exe)], check=True, capture_output=True)

    def test_interaction_failure_keeps_its_reason_and_saves_screenshot(self):
        nova = SimpleNamespace(run=lambda *args: ('', True))
        app = SimpleNamespace(processes=False, interact=lambda *args: 'video stayed dark', name='VLC')
        test = SimpleNamespace(cmd='start vlc.exe', timeout=0)
        with patch.object(appcorpus.time, 'sleep'), patch.object(appcorpus, 'check_shot',
                return_value='screenshot differs') as shot:
            output, reason = appcorpus.gui(nova, test, None, app, None, close=False)
        self.assertEqual(reason, 'video stayed dark')
        shot.assert_called_once_with(nova, None, 'vlc.png')
