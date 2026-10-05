# SuperTux 0.1.3 (2005, GPL; 32-bit, built with MinGW against SDL 1.2) is
# played with a game pad: an Xbox 360 controller (tools/padpeer.py behind
# QEMU's usb-redir), which SDL 1.2 reads through winmm's joystick
# functions (joyGetPosEx, joyGetDevCaps).  Its Inno Setup 4 installer is
# the App Store's download, run silently as the Store's Install would run
# it with its defaults (it needs shfolder.dll for the Start menu); the game
# needs crtdll.dll (its zlib.dll), switches the display to 640x480 for its
# full screen and runs the blitters SDL 1.2 writes into its data (32-bit
# programs not marked NX-compatible run with DEP off, as on Windows).
# The pad's A button picks Start Game and slot 1 on the title screen;
# Enter skips the story; on the world map the stick walks Tux to the
# first level and B enters it; in "Welcome to Antarctica" the stick walks
# Tux right (the picture must change); Start opens the pause menu, where
# the stick picks Abort Level and A takes it.  The world map, with Tux on
# the first level, must then match tests/reference/supertux.png.  Alt+F4
# ends the game, which must put the display back.  Takes the keyboard.
import os
import time

GAME = r'C:\Programs\SuperTux'
SETUP = r'C:\Downloads\supertux-0.1.3-setup.exe'

DOC = 'SuperTux (an SDL 1.2 game played with a game pad, through winmm\'s joystick functions: its first level)'

A_BUTTON, B_BUTTON, START = 0x1000, 0x2000, 0x4000     # (SuperTux's "start" is button 3: the pad's X)


def screen(nova):
    """The display now, small (enough to tell screens apart), and its size"""
    from PIL import Image
    f = os.path.join(nova.work, 'screen.png')
    nova.shot(f)
    im = Image.open(f).convert('RGB')
    return im.resize((320, 240)), im.size


def share(a, box, test):
    px = list(a.crop(box).getdata())
    return sum(1 for p in px if test(*p)) / len(px)


def world_map(a):
    """The world map: its dark blue sea around the island"""
    return share(a, (0, 0, 320, 240), lambda r, g, b: r < 70 and 40 < g < 110 and 90 < b < 170) > 0.2


def sky(a):
    """A level (or the title screen): the light sky across the top"""
    return share(a, (0, 30, 320, 90), lambda r, g, b: b > 170 and r < 190) > 0.6


def press(buttons, hold=1.0):
    w = pad(f'buttons={buttons:#x}')
    time.sleep(hold)
    return w or pad('buttons=0')


def stick(line, hold):
    w = pad(line)
    time.sleep(hold)
    return w or pad('lx=0 ly=0')


def wait_for(nova, test, seconds):
    for _ in range(seconds // 2):
        a, size = screen(nova)
        if test(a):
            return a, size
        time.sleep(2)
    return None, None


def play(nova, echo):
    """The title screen, a new game, the first level with the pad, back to
    the world map"""
    for _ in range(60):
        a, size = screen(nova)
        if size == (640, 480) and sky(a):
            break
        time.sleep(2)
    else:
        return 'the title screen did not show in full screen at 640x480'
    time.sleep(10)                                  # (the title menu takes its first frames to come up)
    w = press(A_BUTTON)                             # Start Game
    time.sleep(5)
    w = w or press(A_BUTTON)                        # Slot 1 - Free: the story
    if w:
        return w
    time.sleep(8)
    nova.keys('ret')                                # (the story scrolls; Enter skips it)
    a, _ = wait_for(nova, world_map, 60)
    if not a:
        return 'the pad did not start a new game (no world map)'
    time.sleep(3)
    w = stick('ly=-32767', 1.0)                     # down the path to the first level
    time.sleep(6)
    w = w or press(B_BUTTON)
    if w:
        return w
    a, _ = wait_for(nova, lambda a: not world_map(a) and sky(a), 60)
    if not a:
        return 'the pad did not enter the first level'
    time.sleep(8)                                   # ("Welcome to Antarctica" goes)
    before, _ = screen(nova)
    w = stick('lx=32767', 0.8)                      # Tux walks right (before the first enemy)
    time.sleep(1)
    after, _ = screen(nova)
    from PIL import ImageChops
    diff = ImageChops.difference(before.convert('L'), after.convert('L')).crop((0, 120, 320, 230))
    moved = sum(1 for v in diff.getdata() if v > 40) / (320 * 110)
    print(f'  the pad moved {moved:.1%} of the level\'s lower half', flush=True)
    if w:
        return w
    if moved < 0.005:
        return 'Tux did not move with the stick'
    w = press(START)                                # the pause menu: Continue, Options, Abort Level
    time.sleep(3)
    for _ in range(2):
        w = w or stick('ly=-32767', 0.5)
        time.sleep(1.5)
    w = w or press(A_BUTTON)
    if w:
        return w
    a, _ = wait_for(nova, world_map, 60)
    if not a:
        return 'Abort Level did not go back to the world map'
    time.sleep(5)
    return None


APP = App('SuperTux', '0.1.3',
          'https://downloads.sourceforge.net/project/super-tux/supertux/0.1.3/supertux-0.1.3-setup.exe',
          'SuperTux', [Test('install', rf'{SETUP} /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP-', timeout=900),
                       Test('its folder', rf'cd "{GAME}"', builtin=True),
                       Test('title, first level with the pad', 'start supertux.exe', timeout=90),
                       Test('display mode back', 'sysinfo', expect=[r'Display: 2560x1600'], builtin=True),
                       Test('back to Documents', r'cd C:\Documents', builtin=True)],
          store='SuperTux', gui=True, interact=play, pad='xbox360')
