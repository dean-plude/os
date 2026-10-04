# LBreakout2 (GPL, 64-bit, SDL 1.2) draws with GDI: SDL 1.2's "windib"
# driver blits a 16-bit DIB section into a 640x480 window.  Its full screen
# ('f', anywhere in the game) is SDL 1.2's own path: ChangeDisplaySettings
# to 640x480, the window's style changed to WS_POPUP with SetWindowLong and
# placed over the display with SetWindowPos, so the desktop's title bar and
# border must go with the caption (before, the picture sat a title bar's
# height too high).  The App Store installs the game (the official zip,
# unpacked into C:\Programs\LBreakout2) and it starts from its own folder,
# as the App Store's Open does.  After 'f' the display must be 640x480 (the
# screenshot's size) and the main menu must match
# tests/reference/lbreakout2.png.  Alt+F4 ends the game, which must put the
# display back (sysinfo: 2560x1600, the mode NovaOS boots in).  Takes the
# keyboard.
import os
import time

GAME = r'C:\Programs\LBreakout2\lbreakout2-2.6.5'

DOC = 'LBreakout2 (an SDL 1.2 game drawn with GDI; its full screen switches the display to 640x480, and back)'


def full_screen(nova, echo):
    """'f': SDL 1.2 switches the display to 640x480 and the window loses
    its frame"""
    from PIL import Image
    time.sleep(60)                                  # the window and its menu come up
    nova.keys('f')
    time.sleep(30)                                  # the display switches and the game draws again
    f = os.path.join(nova.work, 'full.png')
    nova.shot(f)
    size = Image.open(f).size
    if size != (640, 480):
        return f'the display is {size[0]}x{size[1]} in full screen, not 640x480'
    return None


APP = App('LBreakout2', '2.6.5',
          'https://downloads.sourceforge.net/project/lgames/lbreakout2/2.6/lbreakout2-2.6.5-win64.zip',
          'LBreakout2', [Test('install from the App Store', 'store install LBreakout2', store='LBreakout2', timeout=600),
                         Test('its folder', rf'cd "{GAME}"', builtin=True),
                         Test('full screen at 640x480', 'start lbreakout2.exe', timeout=90),
                         Test('display mode back', 'sysinfo', expect=[r'Display: 2560x1600'], builtin=True),
                         Test('back to Documents', r'cd C:\Documents', builtin=True)],
          store='LBreakout2', gui=True, interact=full_screen)
