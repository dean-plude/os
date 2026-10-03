# File Explorer's scroll bars: it opens on C:\Windows\System32 (more files
# than fit), which must show a vertical scroll bar; the wheel, Page Down,
# End, Home, a click on the bar's down arrow and one in its trough must each
# move the view.  Explorer logs a line whenever its view changes
# ("[EXPLORER] PATH: rows A-B of N, ...; bars: vertical; list X,Y WxH"),
# which the test reads.  It runs last: it leaves Explorer open (Alt+Tab
# gives the Terminal back the keyboard) so the screenshot shows it.
import re, time

VIEW = re.compile(r'\[EXPLORER\] [^\n]*System32: rows (\d+)-(\d+) of (\d+), scrolled (\d+) px; '
                  r'bars:([a-z ]*); list (\d+),(\d+) (\d+)x(\d+)')
state = {'why': 'Explorer was not driven'}


def _views(nova):
    return VIEW.findall(open(nova.serial_path, 'rb').read().decode('latin-1'))


def _wait_view(nova, seen, timeout=15):
    """The view after the @seen-th logged one, or None"""
    end = time.time() + timeout
    while time.time() < end:
        v = _views(nova)
        if len(v) > seen:
            time.sleep(0.5)                       # (a key may log more than once)
            return _views(nova)[-1]
        time.sleep(0.2)
    return None


def _move(nova, x, y):
    """The pointer to logical screen point (x, y), from the top-left corner"""
    for _ in range(40):
        nova.hmp('mouse_move -100 -100')
        time.sleep(0.01)
    while x > 0 or y > 0:
        dx, dy = min(x, 40), min(y, 40)
        nova.hmp(f'mouse_move {dx} {dy}')
        time.sleep(0.02)
        x -= dx
        y -= dy
    time.sleep(0.3)


def drive(nova):
    steps = []

    def step(what, act, ok):
        seen = len(_views(nova))
        act()
        v = _wait_view(nova, seen)
        steps.append(what)
        if not v or not ok(int(v[0]), int(v[1]), int(v[2])):
            raise AssertionError(f'{what}: the view did not move as expected '
                                 f'({"rows %s-%s of %s" % v[:3] if v else "no new view logged"})')
        return v

    try:
        seen = len(_views(nova))
        nova.qmp.type('start C:\\Windows\\System32\n')
        v = _wait_view(nova, seen, 30)
        if not v:
            raise AssertionError('Explorer logged no view of C:\\Windows\\System32')
        first, last, n = int(v[0]), int(v[1]), int(v[2])
        if 'vertical' not in v[4] or n <= last:
            raise AssertionError(f'no vertical scroll bar on {n} rows (rows {first}-{last} shown)')
        lx, ly, lw, lh = (int(x) for x in v[5:9])
        page = last - first + 1

        def wheel():
            _move(nova, lx + lw // 2, ly + lh // 2)
            for _ in range(2):
                nova.qmp.cmd('input-send-event', events=[{'type': 'btn', 'data': {'down': True, 'button': 'wheel-down'}}])
                nova.qmp.cmd('input-send-event', events=[{'type': 'btn', 'data': {'down': False, 'button': 'wheel-down'}}])
                time.sleep(0.2)
        v = step('the wheel', wheel, lambda a, b, n: a == 7)
        v = step('Home', lambda: nova.qmp.key('home'), lambda a, b, n: a == 1)
        nova.qmp.key('pgdn')                       # the selection to the last row in view
        time.sleep(0.5)
        v = step('Page Down', lambda: nova.qmp.key('pgdn'), lambda a, b, n: a == page)
        v = step('End', lambda: nova.qmp.key('end'), lambda a, b, n: b == n and a > 1)
        v = step('Home again', lambda: nova.qmp.key('home'), lambda a, b, n: a == 1)
        bx = lx + lw + 8                           # the bar, right of the list
        v = step('the down arrow', lambda: nova.click(bx, ly + lh - 8), lambda a, b, n: a == 2)
        v = step('the trough', lambda: nova.click(bx, ly + lh - 30), lambda a, b, n: a == 2 + page)
        state['why'] = None
    except AssertionError as e:
        state['why'] = str(e)
    nova.qmp.key('alt', 'tab')                     # the keyboard back to the Terminal
    time.sleep(1)


TESTS = [
    Test('explorer scroll bars', 'echo explorer driven', [r'explorer driven'], builtin=True, before=drive,
         check=lambda nova: state['why']),
]
