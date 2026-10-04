# OpenTyrian (Tyrian 2.1, freeware since 2004, on its free and open-source
# engine; SDL2) draws with Direct3D 9, SDL's first choice on Windows, which
# NovaOS's d3d9.dll hands to DXVK on Mesa 3D's Vulkan (llvmpipe), so the
# App Store's Mesa 3D and DXVK are installed first, as a user without a GPU
# driver would.  The App Store installs the game (the official 64-bit zip,
# which carries the freeware game data, unpacked into C:\Programs\OpenTyrian).
# Its attract-mode demo plays level 1 in a window; Alt+Enter switches it to
# full screen (SDL resets the Direct3D 9 device at the display's size) and
# Enter goes through its menus (one-player full game, episode 1, normal) to
# the game's own menu between levels: the screenshot must match
# tests/reference/opentyrian.png and the sound NovaOS played must hold its
# music (App(sound=...)).  The game fades between its menus, slowly without
# KVM, and takes no keys while it fades, so Enter is pressed only once the
# screen stands still, until the game menu shows.  Takes the keyboard.
import os
import time

DOC = 'OpenTyrian (Tyrian 2.1 on Direct3D 9 through DXVK, its demo, menus and full screen, with music)'


def screen(nova):
    """The display now, small and grey (enough to tell screens apart)"""
    from PIL import Image
    f = os.path.join(nova.work, 'screen.png')
    nova.shot(f)
    return Image.open(f).convert('L').resize((320, 200))


def changed(a, b):
    """Whether two screen() images differ by more than noise"""
    from PIL import ImageChops
    return ImageChops.difference(a, b).point(lambda v: v > 3 and 255).getbbox() is not None


def settled(nova, most=120):
    """Wait (up to @most seconds) until the screen has stopped changing for
    five seconds: the game fades between its menus, slowly without KVM,
    and takes no keys while it fades; returns the still screen"""
    prev, still = screen(nova), 0
    for _ in range(most):
        time.sleep(1)
        cur = screen(nova)
        still = 0 if changed(cur, prev) else still + 1
        prev = cur
        if still >= 5:
            break
    return prev


def like(a, b):
    """Whether two screen() images show the same screen (a fade's frames
    and the game's animation aside)"""
    from PIL import ImageChops, ImageStat
    return ImageStat.Stat(ImageChops.difference(a, b)).mean[0] < 12


def play(nova, echo):
    """Full screen while the demo plays, Enter for the title menu, then
    Enter on each still screen (Start New Game, 1 Player Full Game,
    Episode 1, Normal) until the game menu shows"""
    from PIL import Image
    menu = Image.open(os.path.join(ROOT, 'tests', 'reference', 'opentyrian.png')).convert('L').resize((320, 200))
    time.sleep(40)                      # the demo plays level 1 in a window
    nova.keys('alt-ret')                # full screen
    time.sleep(10)
    nova.keys('ret')                    # the title menu
    for _ in range(10):
        if like(settled(nova), menu):
            return None
        nova.keys('ret')
    return 'the game menu did not show after ten presses of Enter'


APP = App('OpenTyrian', '2.1.20260913',
          'https://github.com/opentyrian/opentyrian/releases/download/v2.1.20260913/opentyrian-v2.1.20260913-windows-x86_64.zip',
          'OpenTyrian', [Test('install Mesa 3D', 'store install Mesa 3D', store='Mesa 3D', timeout=1200),
                         Test('install DXVK', 'store install DXVK', store='DXVK', timeout=600),
                         Test('install from the App Store', 'store install OpenTyrian', store='OpenTyrian', timeout=600),
                         Test('game menu, full screen', r'start C:\Programs\OpenTyrian\opentyrian\opentyrian.exe', timeout=90)],
          store='OpenTyrian', gui=True, runtimes=['Mesa 3D', 'DXVK'], sound=(None, 5000), interact=play)
