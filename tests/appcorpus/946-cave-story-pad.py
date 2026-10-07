# Cave Story (Studio Pixel's 2004 freeware, Aeon Genesis' English
# translation; 32-bit) played with a game pad: an Xbox 360 controller
# (tools/padpeer.py behind QEMU's usb-redir), which the game finds with
# DirectInput 7 (NovaOS's dinput.dll, dinput8.c built with
# NOVA_DINPUT_LEGACY: EnumDevices(DIDEVTYPE_JOYSTICK), c_dfDIJoystick,
# GetDeviceState) because its Config.dat has the game pad on.  Its
# buttons are numbered as Windows numbers an Xbox controller's (A is
# button 1, B button 2), so the game's default layout makes B jump and
# say OK.  The App Store installs the game (as 945-cave-story.py) and it
# starts from its own folder.  On the title screen B starts a new game,
# whose opening line must show; more presses of B take the opening talk
# to the Start Point cave, where Quote's health bar shows; the stick then
# walks Quote right to the cave's far corner (the picture must change),
# which must match tests/reference/cave story (game pad).png.  Alt+F4 ends
# the game, which must put the display back.  Takes the keyboard and the
# pad, never both.
import os
import time

GAME = r'C:\Programs\Cave Story\CaveStory'

DOC = 'Cave Story played with a game pad (DirectInput 7 joystick): the opening talk, then Quote walks in the Start Point cave'

B_BUTTON = 0x2000                                   # (the game's button 2: jump and OK)


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


def lit(a, box):
    return share(a, box, lambda r, g, b: max(r, g, b) > 96)


def title(a):
    """The title screen: "Cave Story" lit at the top, black above it"""
    return lit(a, (90, 40, 230, 70)) > 0.08 and lit(a, (0, 0, 320, 30)) < 0.01


def message(a):
    """A message box: its top edge lit across, and text in it"""
    edge = max(lit(a, (45, y, 275, y + 1)) for y in range(174, 181))
    return not title(a) and edge > 0.9 and lit(a, (50, 198, 250, 210)) > 0.05


def playing(a):
    """Quote in a cave: his red health bar at the top left"""
    return share(a, (10, 36, 60, 50), lambda r, g, b: r > 150 and g < 80 and b < 80) > 0.05


def press(buttons, hold=1.5):
    """Hold @buttons (the game reads the pad once a frame: held, as a player
    does)"""
    w = pad(f'buttons={buttons:#x}')
    time.sleep(hold)
    return w or pad('buttons=0')


def play(nova, echo):
    """The title screen, B for a new game, B through the opening talk, the
    stick to walk"""
    for _ in range(90):
        a, size = screen(nova)
        if title(a):
            break
        time.sleep(2)
    else:
        return 'the title screen did not show'
    if size != (640, 480):
        return f'the display is {size[0]}x{size[1]} in full screen, not 640x480'
    time.sleep(3)
    w = press(B_BUTTON)                             # New
    if w:
        return w
    for _ in range(30):
        time.sleep(2)
        a, _ = screen(nova)
        if message(a):
            break
    else:
        return 'the pad\'s B did not start a new game'
    time.sleep(6)                                   # ("From somewhere, a transmission..." is written)
    for _ in range(25):                             # (B says OK to each line)
        w = press(B_BUTTON)
        if w:
            return w
        time.sleep(3)
        a, _ = screen(nova)
        if playing(a):
            break
    else:
        return 'the pad\'s B did not take the opening talk to the Start Point cave'
    time.sleep(4)                                   # ("Start Point" goes)
    before, _ = screen(nova)
    w = pad('lx=32767')                             # Quote walks right, off the ledge to the far corner
    time.sleep(5)
    w = w or pad('lx=0')
    if w:
        return w
    time.sleep(3)
    after, _ = screen(nova)
    from PIL import ImageChops
    diff = ImageChops.difference(before.convert('L'), after.convert('L')).crop((80, 120, 320, 240))
    moved = sum(1 for v in diff.getdata() if v > 40) / (240 * 120)
    print(f'  the stick changed {moved:.1%} of the cave\'s lower right', flush=True)
    if moved < 0.003:
        return 'Quote did not move with the stick'
    return None


APP = App('Cave Story (game pad)', '1.0.0.6',
          'https://www.cavestory.org/downloads/cavestoryen.zip',
          'Cave Story', [Test('install from the App Store', 'store install Cave Story', store='Cave Story', timeout=600),
                         Test('its folder', rf'cd "{GAME}"', builtin=True),
                         Test('new game, Start Point with the pad', 'start Doukutsu.exe', timeout=90),
                         Test('display mode back', 'sysinfo', expect=[r'Display: 2560x1600'], builtin=True),
                         Test('back to Documents', r'cd C:\Documents', builtin=True)],
          store='Cave Story', gui=True, interact=play, pad='xbox360')
