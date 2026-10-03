# montest: two monitors (the graphics boot's second is a QEMU secondary-vga).
# The test pushes the pointer right when montest asks, and the screenshot
# (one PNG per monitor) shows its window on the second monitor.
import time


def push_right(nova):
    for _ in range(80):
        nova.hmp('mouse_move 40 0')
        time.sleep(0.02)


TESTS = [
    Test('montest', 'montest 2', [r'montest: \d+ passed, 0 failed'], shot=r'window on display 2',
         acts=[(r'move the pointer right', push_right)]),
]
