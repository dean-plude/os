# Blobby Volley 2 (GPL, 32-bit, SDL2) switches the display to 800x600 when
# it goes full screen: SDL's SDL_WINDOW_FULLSCREEN (not the desktop-sized
# kind) changes the display mode (ChangeDisplaySettingsEx), then draws
# with Direct3D 9 in exclusive full screen, which NovaOS's d3d9.dll hands
# to DXVK on Mesa 3D's Vulkan (llvmpipe), so the App Store's Mesa 3D and
# DXVK are installed first.  The App Store installs the game (the official
# zip, unpacked into C:\Programs\Blobby Volley 2), and it starts from its
# own folder (it reads its data from there; the App Store's Open and a
# shortcut start it there too) in an 800x600 window.  The keyboard goes
# Options, Graphic Options, Fullscreen Mode, OK: the display must then be
# 800x600 (the screenshot's size), and back in the main menu the
# screenshot must match tests/reference/blobby volley 2.png.  Alt+F4 ends
# the game, which must put the display back (sysinfo: 2560x1600, the mode
# NovaOS boots in).  Takes the keyboard.
import os
import time

GAME = r'C:\Programs\Blobby Volley 2\blobby-1.1.1'

DOC = 'Blobby Volley 2 (its full screen switches the display to 800x600 on Direct3D 9 through DXVK, and back)'


def keys(nova, *names, pause=3):
    """Press each key, @pause seconds apart (a menu takes a frame or two
    to move its selection, slowly without KVM)"""
    for n in names:
        nova.keys(n)
        time.sleep(pause)


def full_screen(nova, echo):
    """Options (the fourth item), Graphic Options (the eighth), Fullscreen
    Mode (the first), OK (two up from it, past Cancel); then OK on the
    options screen back to the main menu"""
    from PIL import Image
    time.sleep(60)                                  # the window and its menu come up
    keys(nova, 'down', 'down', 'down', 'down', 'ret')
    time.sleep(15)
    keys(nova, *['down'] * 8, 'ret')
    time.sleep(15)
    keys(nova, 'down', 'ret', 'up', 'up', 'ret')
    time.sleep(40)                                  # the display switches and the game draws again
    f = os.path.join(nova.work, 'full.png')
    nova.shot(f)
    size = Image.open(f).size
    if size != (800, 600):
        return f'the display is {size[0]}x{size[1]} in full screen, not 800x600'
    keys(nova, 'up', 'up', 'ret')                   # the options screen's OK: the main menu
    time.sleep(15)
    return None


APP = App('Blobby Volley 2', '1.1.1',
          'https://downloads.sourceforge.net/project/blobby/Blobby%20Volley%202%20%28Win32%29/1.1.1/blobby2-win32-1.1.1.zip',
          'Blobby Volley 2', [Test('install Mesa 3D', 'store install Mesa 3D', store='Mesa 3D', timeout=1200),
                              Test('install DXVK', 'store install DXVK', store='DXVK', timeout=600),
                              Test('install from the App Store', 'store install Blobby Volley 2', store='Blobby Volley 2',
                                   timeout=600),
                              Test('its folder', rf'cd "{GAME}"', builtin=True),
                              Test('full screen at 800x600', 'start blobby.exe', timeout=90),
                              Test('display mode back', 'sysinfo', expect=[r'Display: 2560x1600'], builtin=True),
                              Test('back to Documents', r'cd C:\Documents', builtin=True)],
          store='Blobby Volley 2', gui=True, runtimes=['Mesa 3D', 'DXVK'], interact=full_screen)
