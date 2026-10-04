# warptest: SetCursorPos and ClipCursor: the foreground window moves and confines the pointer (WM_MOUSEMOVE,
# no WM_INPUT), the mouse stays inside the rectangle, a background process is refused, 64- and 32-bit
# (tools/selftest.py's core suite)
import time


def mouse(nova):
    """300 left and up, in three steps"""
    for _ in range(3):
        nova.hmp('mouse_move -100 -100')
        time.sleep(0.3)


DOC = ('`warptest` (SetCursorPos and ClipCursor: the pointer moved and confined by the window in front, WM_MOUSEMOVE '
       'but no WM_INPUT for a warp, the mouse kept in the rectangle, a background process refused; 64- and 32-bit)')
ACTS = [(r'warptest: move the mouse', mouse)]
TESTS = [
    Test('warptest', 'warptest', [r'warptest: \d+ passed, 0 failed'], acts=ACTS),
    Test('warptest x86', r'C:\Programs\x86\warptest.exe', [r'warptest: \d+ passed, 0 failed'], acts=ACTS),
]
