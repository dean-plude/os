# Audacity records 10 s from the microphone (which hears the corpus's 523 Hz
# tone, App(mic=True)) and saves the project: its screenshot after Stop must
# match tests/reference/audacity.png, and C:\Apps\rec10.aup3 must exist.
# The official 64-bit zip.  Windowed: runs after the console programs (880),
# takes the keyboard; the clicks are at Audacity's default window position
# on NovaOS's 1280x800 desktop, and each dialog is waited for by looking at
# the screen (Audacity's start-up and its finishing of a recording take a
# while without KVM).
import os, tempfile, time

DOC = 'Audacity (records 10 s from the microphone and saves the project)'


def pixel(nova, x, y):
    """The screen's pixel at logical (x, y), as (r, g, b)"""
    from PIL import Image
    f = os.path.join(tempfile.gettempdir(), 'audacity-look.png')
    nova.shot(f)
    time.sleep(1)
    im = Image.open(f).convert('RGB')
    sx, sy = im.width // 1280, im.height // 800
    return im.getpixel((x * sx, y * sy))


def wait_light(nova, x, y, light, timeout, what):
    """Until the pixel at (x, y) is light (a dialog over the dark track
    panel) or dark (it went away): why it did not happen, or None"""
    for _ in range(timeout // 5):
        r, g, b = pixel(nova, x, y)
        if (r + g + b > 384) == light:
            return None
        time.sleep(5)
    return f'{what} did not appear' if light else f'{what} did not close'


def record(nova, echo):
    """Through the first-run dialogs, Record for 10 s, Stop, save as rec10"""
    time.sleep(60)                                  # the first-run message
    nova.keys('ret')
    time.sleep(30)
    nova.keys('tab')
    nova.keys('ret')
    w = wait_light(nova, 821, 500, True, 240, 'the update-check dialog')
    if w:
        return w
    nova.click(821, 500)                            # update check: Accept & continue
    time.sleep(5)
    nova.click(1003, 678)                           # Welcome: OK
    time.sleep(3)
    nova.click(378, 96)                             # Record
    time.sleep(10)
    nova.click(205, 96)                             # Stop
    time.sleep(10)
    r, g, b = pixel(nova, 640, 272)                 # a dark title bar mid-screen: the dropout warning
    if max(r, g, b) < 60:                           # (a slow machine lost samples); OK keeps the recording
        nova.click(789, 453)
        time.sleep(3)
    for _ in range(12):                             # Save is enabled once the recording is finished
        nova.keys('ctrl-s')
        time.sleep(5)
        r, g, b = pixel(nova, 734, 584)
        if r + g + b > 384:
            break
    else:
        return 'the save dialog did not appear'
    nova.click(734, 584)                            # How would you like to save: Save to computer
    time.sleep(20)                                  # the file dialog
    nova.qmp.type(rf'{A}\rec10')
    time.sleep(2)
    nova.keys('ret')
    time.sleep(40)                                  # the project is written
    nova.keys('home ctrl-f')                        # start + fit project; recording auto-scroll varies
    time.sleep(3)
    return None


APP = App('Audacity', '3.7.4',
          'https://github.com/audacity/audacity/releases/download/Audacity-3.7.4/audacity-win-3.7.4-64bit.zip',
          'Audacity', [Test('record and save', rf'start {A}\Audacity\Audacity.exe', timeout=120),
                       Test('project file', rf'dir {A}\rec10.aup3', [r'rec10\.aup3'], builtin=True)],
          strip=1, gui=True, mic=True, interact=record)
