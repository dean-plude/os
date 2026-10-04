# Cave Story (Studio Pixel's 2004 freeware, Aeon Genesis' English
# translation; 32-bit) draws with DirectDraw: NovaOS's ddraw.dll is
# cnc-ddraw, which takes the game's 640x480 16-bit full screen (the
# display switches to 640x480) and draws its surfaces with GDI, OpenGL or
# Direct3D 9.  Its keys are window messages (WM_KEYDOWN, which reach it
# because activation puts the keyboard focus in its window, as on Windows,
# though its WM_ACTIVATE never calls DefWindowProc), and its text is drawn
# with GDI on DirectDraw surfaces.  The App Store installs the game (the
# translation's zip, unpacked into C:\Programs\Cave Story) and it starts
# from its own folder, as the App Store's Open does.  The title screen
# must show (the display at 640x480); Z (the game's OK key, held: it reads
# the keys once a frame) starts a new game, whose opening line, "From
# somewhere, a transmission...", must match tests/reference/cave story.png
# once it is all written.  Alt+F4 ends the game, which must put the
# display back (sysinfo: 2560x1600, the mode NovaOS boots in).  Takes the
# keyboard.
import os
import time

GAME = r'C:\Programs\Cave Story\CaveStory'

DOC = 'Cave Story (a DirectDraw game, on cnc-ddraw: its title screen in full screen at 640x480, then a new game)'


def screen(nova):
    """The display now, small and grey (enough to tell screens apart), and
    its size"""
    from PIL import Image
    f = os.path.join(nova.work, 'screen.png')
    nova.shot(f)
    im = Image.open(f)
    return im.convert('L').resize((320, 240)), im.size


def lit(a, box):
    """The share of @box's pixels (in screen() coordinates) that are lit"""
    px = list(a.crop(box).getdata())
    return sum(1 for v in px if v > 96) / len(px)


def title(a):
    """The title screen: "Cave Story" lit at the top, its menu below, and
    black around them"""
    return lit(a, (90, 40, 230, 70)) > 0.08 and lit(a, (0, 0, 320, 30)) < 0.01


def message(a):
    """The opening line: the message box's top edge, a lit line across,
    and its text in it"""
    edge = max(lit(a, (45, y, 275, y + 1)) for y in range(174, 181))
    return not title(a) and edge > 0.9 and lit(a, (50, 198, 250, 210)) > 0.05


def play(nova, echo):
    """The title screen, then Z for a new game, until its first line shows"""
    for _ in range(90):
        a, size = screen(nova)
        if title(a):
            break
        time.sleep(2)
    else:
        return 'the title screen did not show'
    if size != (640, 480):
        return f'the display is {size[0]}x{size[1]} in full screen, not 640x480'
    for _ in range(3):
        nova.qmp.key('z', hold=1500)                # (read once a frame: held, as a player does)
        for _ in range(60):
            time.sleep(2)
            a, _ = screen(nova)
            if message(a):
                time.sleep(10)                      # the rest of the line is written
                return None
            if not title(a):
                break
    return 'a new game did not start'


APP = App('Cave Story', '1.0.0.6',
          'https://www.cavestory.org/downloads/cavestoryen.zip',
          'Cave Story', [Test('install from the App Store', 'store install Cave Story', store='Cave Story', timeout=600),
                         Test('its folder', rf'cd "{GAME}"', builtin=True),
                         Test('title screen, new game', 'start Doukutsu.exe', timeout=90),
                         Test('display mode back', 'sysinfo', expect=[r'Display: 2560x1600'], builtin=True),
                         Test('back to Documents', r'cd C:\Documents', builtin=True)],
          store='Cave Story', gui=True, interact=play)
