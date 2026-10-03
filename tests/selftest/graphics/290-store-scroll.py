# The App Store's scroll bar: "store open" shows it on "All apps" (more
# programs than fit), which must have a vertical scroll bar; the wheel,
# Home, Page Down, End, a click on the bar's down arrow, one in its trough
# and a drag of its thumb must each move the list by what they should.
# The Store logs a line whenever its view changes ("[STORE] view: All apps:
# scrolled P of M px, page H; bar: vertical; list X,Y WxH"), which the test
# reads.  Esc closes the Store afterwards, giving the Terminal back the
# keyboard.
import re, time

VIEW = re.compile(r'\[STORE\] view: All apps: scrolled (\d+) of (\d+) px, page (\d+); '
                  r'bar:([a-z ]*); list (\d+),(\d+) (\d+)x(\d+)')
ROW_H = 84                                       # kernel/apps/store.c
state = {'why': 'the App Store was not driven'}


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


def _hmp(nova, c):
    nova.qmp.cmd('human-monitor-command', **{'command-line': c})


def _move(nova, x, y):
    """The pointer to logical screen point (x, y), from the top-left corner"""
    for _ in range(40):
        _hmp(nova, 'mouse_move -100 -100')
        time.sleep(0.01)
    while x > 0 or y > 0:
        dx, dy = min(x, 40), min(y, 40)
        _hmp(nova, f'mouse_move {dx} {dy}')
        time.sleep(0.02)
        x -= dx
        y -= dy
    time.sleep(0.3)


def drive(nova):
    def step(what, act, ok):
        seen = len(_views(nova))
        act()
        v = _wait_view(nova, seen)
        if not v or not ok(int(v[0])):
            raise AssertionError(f'{what}: the list did not move as expected '
                                 f'({"scrolled %s of %s px" % v[:2] if v else "no new view logged"})')
        return v

    try:
        seen = len(_views(nova))
        nova.qmp.type('store open\n')
        if not _wait_view(nova, seen, 30):
            raise AssertionError('the App Store logged no view of All apps')
        nova.qmp.key('home')                       # (an App Store left open may be scrolled)
        time.sleep(1)
        v = _views(nova)[-1]
        if int(v[0]) != 0:
            raise AssertionError(f'Home: the list did not go to the top (scrolled {v[0]} px)')
        maxpos, page = int(v[1]), int(v[2])
        if 'vertical' not in v[3] or maxpos <= 0:
            raise AssertionError(f'no vertical scroll bar (scrolled {v[0]} of {maxpos} px)')
        lx, ly, lw, lh = (int(x) for x in v[4:8])

        def wheel():
            _move(nova, lx + lw // 2, ly + lh // 2)
            for _ in range(2):
                nova.qmp.cmd('input-send-event', events=[{'type': 'btn', 'data': {'down': True, 'button': 'wheel-down'}}])
                nova.qmp.cmd('input-send-event', events=[{'type': 'btn', 'data': {'down': False, 'button': 'wheel-down'}}])
                time.sleep(0.2)
        step('the wheel', wheel, lambda p: p == min(2 * ROW_H, maxpos))
        step('Home', lambda: nova.qmp.key('home'), lambda p: p == 0)
        step('Page Down', lambda: nova.qmp.key('pgdn'), lambda p: p == min(page, maxpos))
        step('End', lambda: nova.qmp.key('end'), lambda p: p == maxpos)
        step('Home again', lambda: nova.qmp.key('home'), lambda p: p == 0)
        bx = lx + lw + 8                           # the bar, right of the list
        step('the down arrow', lambda: nova.click(bx, ly + lh - 8), lambda p: p == min(ROW_H, maxpos))
        step('the trough', lambda: nova.click(bx, ly + lh - 30), lambda p: p == min(ROW_H + page, maxpos))
        step('Home before the drag', lambda: nova.qmp.key('home'), lambda p: p == 0)

        def drag():                                # the thumb, just under the up arrow, to the bottom
            _move(nova, bx, ly + 17 + 6)
            _hmp(nova, 'mouse_button 1')
            time.sleep(0.2)
            for _ in range(lh // 20 + 2):
                _hmp(nova, 'mouse_move 0 20')
                time.sleep(0.05)
            _hmp(nova, 'mouse_button 0')
        step('the thumb dragged down', drag, lambda p: p == maxpos)
        state['why'] = None
    except AssertionError as e:
        state['why'] = str(e)
    nova.qmp.key('esc')                            # close the Store: the keyboard back to the Terminal
    time.sleep(1)


TESTS = [
    Test('store scroll bar', 'echo store driven', [r'store driven'], builtin=True, before=drive,
         check=lambda nova: state['why']),
]
