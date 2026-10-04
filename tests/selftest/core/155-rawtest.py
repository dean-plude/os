# rawtest: the mouse and the keyboard through Raw Input (RIM_TYPEMOUSE, RIM_TYPEKEYBOARD): the device
# list, WM_INPUT with relative motion, button flags and the wheel as the test moves and clicks the
# PS/2 mouse, keys through GetRawInputBuffer with RIDEV_NOLEGACY, 64- and 32-bit (tools/selftest.py's core suite)
import time


def point(nova):
    """The pointer onto rawtest's window (100, 100, 600 x 400)"""
    nova.move_to(400, 300)


def mouse(nova):
    """30 right and 20 down, a left click, a notch of the wheel, a right click"""
    for line in ('mouse_move 30 20', 'mouse_button 1', 'mouse_button 0', 'mouse_move 0 0 1', 'mouse_button 2', 'mouse_button 0'):
        nova.hmp(line)
        time.sleep(0.4)


def keys(nova):
    nova.keys('a right alt-x')


DOC = ('`rawtest` (the mouse and the keyboard through Raw Input: device list, names and info; WM_INPUT with relative '
       'motion, button flags and the wheel; keys through GetRawInputBuffer with RIDEV_NOLEGACY; 64- and 32-bit)')
ACTS = [(r'rawtest: point at the window', point), (r'rawtest: move 30 20', mouse), (r'rawtest: press a, right', keys)]
TESTS = [
    Test('rawtest', 'rawtest', [r'rawtest: \d+ passed, 0 failed'], acts=ACTS),
    Test('rawtest x86', r'C:\Programs\x86\rawtest.exe', [r'rawtest: \d+ passed, 0 failed'], acts=ACTS),
]
