# Krita 5.3 (Qt 5, built with LLVM's MinGW toolchain: libc++ and libunwind)
# starts, and Ctrl+N, typed as soon as its window is up with no click
# first (the keyboard must already be Krita's), opens the new-document
# dialog, whose Create gives an empty A4 image on the OpenGL canvas with
# the toolbox, colour selector, layers and brush presets.  A stroke dragged
# across the canvas with the mouse (the default freehand brush) must change
# its pixels, and Ctrl+Z must take it away again (so Krita closes with no
# "Save?"); the window must then match tests/reference/krita.png.  The
# portable zip from download.kde.org, the download the App Store lists.
# Krita needs OpenGL 3 for its canvas, so the App Store's Mesa 3D
# (llvmpipe) is installed first, as a user without a GPU driver would.
# Windowed; it loads about 90 DLLs, its resources and Python plugins before
# the window.
import os, tempfile, time

DOC = 'Krita'
CANVAS = (400, 380, 680, 600)       # inside the empty image (logical screen points)


def dark_pixels(nova, path):
    """How many pixels of CANVAS are not near-white"""
    from PIL import Image
    nova.shot(path)
    time.sleep(1)
    im = Image.open(path).convert('L')
    k = im.width // 1280                # screenshots are at the display's scale
    box = im.crop(tuple(v * k for v in CANVAS))
    return sum(1 for v in box.getdata() if v < 200)


def new_image(nova, echo):
    """Ctrl+N with no click first, Create in the dialog, a stroke, Ctrl+Z"""
    nova.keys('ctrl-n')
    time.sleep(60)
    nova.keys('ret')
    time.sleep(90)
    work = tempfile.mkdtemp(prefix='krita')
    before = dark_pixels(nova, os.path.join(work, 'before.png'))
    nova.drag(430, 420, 650, 560)
    time.sleep(30)
    after = dark_pixels(nova, os.path.join(work, 'after.png'))
    print(f'  canvas: {before} dark pixels before the stroke, {after} after', flush=True)
    if after < before + 200:
        return f'the stroke did not draw on the canvas ({before} dark pixels before, {after} after)'
    nova.keys('ctrl-z')                 # back to an unmodified image, which closes with no "Save?"
    time.sleep(30)
    undone = dark_pixels(nova, os.path.join(work, 'undone.png'))
    print(f'  canvas: {undone} dark pixels after Ctrl+Z', flush=True)
    if undone > before + 100:
        return f'Ctrl+Z did not undo the stroke ({undone} dark pixels, {before} before the stroke)'
    return None


APP = App('Krita', '5.3.4', 'https://download.kde.org/stable/krita/5.3.4/krita-x64-5.3.4.zip',
          'Krita', [Test('install Mesa 3D', 'store install Mesa 3D', store='Mesa 3D', timeout=1200),
                    Test('new image', rf'start {A}\Krita\bin\krita.exe --nosplash', timeout=300)],
          strip=1, gui=True, interact=new_image, runtimes=['Mesa 3D'])
