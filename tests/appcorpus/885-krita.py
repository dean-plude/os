# Krita 5.3 (Qt 5, built with LLVM's MinGW toolchain: libc++ and libunwind)
# starts, and its welcome page's "New Image" opens the new-document dialog,
# whose Create gives an empty A4 image on the OpenGL canvas with the
# toolbox, colour selector, layers and brush presets; that must match
# tests/reference/krita.png.  The portable zip from download.kde.org, the
# download the App Store lists.  Krita needs OpenGL 3 for its canvas, so
# the App Store's Mesa 3D (llvmpipe) is installed first, as a user without
# a GPU driver would.  Windowed; it loads about 90 DLLs, its resources and
# Python plugins before the window.
import time

DOC = 'Krita'


def new_image(nova, echo):
    """Click "New Image" on the welcome page, then Create in the dialog"""
    nova.click(340, 323)
    time.sleep(60)
    nova.keys('ret')
    time.sleep(90)
    return None


APP = App('Krita', '5.3.4', 'https://download.kde.org/stable/krita/5.3.4/krita-x64-5.3.4.zip',
          'Krita', [Test('install Mesa 3D', 'store install Mesa 3D', store='Mesa 3D', timeout=1200),
                    Test('new image', rf'start {A}\Krita\bin\krita.exe --nosplash', timeout=300)],
          strip=1, gui=True, interact=new_image, runtimes=['Mesa 3D'])
