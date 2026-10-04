# Teeworlds 0.7.5 (free and open source; SDL2) starts in full screen (its
# default), goes through its first-start questions with the keyboard
# (Enter keeps English as the language, Enter again keeps the nickname
# "nameless tee") and reaches its start menu with its menu music playing:
# the screenshot must match
# tests/reference/teeworlds.png and the sound NovaOS played must hold the
# music (App(sound=...)).  The App Store installs it (the official 64-bit
# zip from GitHub, unpacked into C:\Programs\Teeworlds).  Teeworlds
# needs OpenGL 1.2 (3D textures), more than NovaOS's own OpenGL 1.1, so the
# App Store's Mesa 3D is installed first, as a user without a GPU driver
# would.  The mouse does not move in it yet: SDL's relative mouse mode,
# which Teeworlds' menus use, reads Raw Input (WM_INPUT), which NovaOS does
# not send yet, so the test uses only the keyboard.  Takes the keyboard.
import time

DOC = 'Teeworlds (full screen, through its first-start questions to its start menu, with music)'


def first_start(nova, echo):
    """Enter in the language box, Enter in the welcome box"""
    nova.keys('ret')                    # Language: Ok (English is chosen)
    time.sleep(20)
    nova.keys('ret')                    # Welcome: Enter
    time.sleep(30)
    return None


APP = App('Teeworlds', '0.7.5', 'https://github.com/teeworlds/teeworlds/releases/download/0.7.5/teeworlds-0.7.5-win64.zip',
          'Teeworlds', [Test('install Mesa 3D', 'store install Mesa 3D', store='Mesa 3D', timeout=1200),
                        Test('install from the App Store', 'store install Teeworlds', store='Teeworlds', timeout=600),
                        Test('start menu', r'start C:\Programs\Teeworlds\teeworlds-0.7.5-win64\teeworlds.exe',
                             timeout=90)],
          store='Teeworlds', gui=True, runtimes=['Mesa 3D'], sound=(None, 5000), interact=first_start)
