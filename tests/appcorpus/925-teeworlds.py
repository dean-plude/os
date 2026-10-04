# Teeworlds 0.7.5 (free and open source; SDL2) starts in full screen (its
# default), goes through its first-start questions with the keyboard
# (Enter keeps English as the language, Enter again keeps the nickname
# "nameless tee"), reaches its start menu with its menu music playing and
# opens Settings with the mouse:
# the screenshot must match
# tests/reference/teeworlds.png and the sound NovaOS played must hold the
# music (App(sound=...)).  The App Store installs it (the official 64-bit
# zip from GitHub, unpacked into C:\Programs\Teeworlds).  Teeworlds
# needs OpenGL 1.2 (3D textures), more than NovaOS's own OpenGL 1.1, so the
# App Store's Mesa 3D is installed first, as a user without a GPU driver
# would.  Then the mouse opens Settings, with Teeworlds' own default
# (inp_grab 0): SDL's relative mouse mode reads WM_MOUSEMOVE and recentres
# the pointer in the window with SetCursorPos every time it moves; the
# test pushes the menu cursor into the top-left corner and moves it over
# the Settings button (1:1 with the mouse) and clicks, and the settings
# page must show.  Takes the keyboard and the mouse.
import time

DOC = 'Teeworlds (full screen, through its first-start questions to its start menu, with music; Settings opened with the mouse, SDL recentring the pointer with SetCursorPos)'


def first_start(nova, echo):
    """Enter in the language box, Enter in the welcome box, then a click on Settings"""
    nova.keys('ret')                    # Language: Ok (English is chosen)
    time.sleep(20)
    nova.keys('ret')                    # Welcome: Enter
    time.sleep(30)
    for _ in range(20):                 # the menu cursor into the top-left corner
        nova.hmp('mouse_move -100 -100')
        time.sleep(0.05)
    time.sleep(10)
    x, y = 533, 461                     # Settings
    while x > 0 or y > 0:
        nova.hmp(f'mouse_move {min(x, 40)} {min(y, 40)}')
        x, y = x - min(x, 40), y - min(y, 40)
        time.sleep(0.05)
    time.sleep(10)
    nova.hmp('mouse_button 1')
    time.sleep(0.3)
    nova.hmp('mouse_button 0')
    time.sleep(30)
    return None


APP = App('Teeworlds', '0.7.5', 'https://github.com/teeworlds/teeworlds/releases/download/0.7.5/teeworlds-0.7.5-win64.zip',
          'Teeworlds', [Test('install Mesa 3D', 'store install Mesa 3D', store='Mesa 3D', timeout=1200),
                        Test('install from the App Store', 'store install Teeworlds', store='Teeworlds', timeout=600),
                        Test('settings with the mouse', r'start C:\Programs\Teeworlds\teeworlds-0.7.5-win64\teeworlds.exe',
                             timeout=90)],
          store='Teeworlds', gui=True, runtimes=['Mesa 3D'], sound=(None, 5000), interact=first_start)
