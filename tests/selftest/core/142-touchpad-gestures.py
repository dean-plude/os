# touchpad gestures: `hwcheck` switches its modelled precision touchpad to
# touchpad mode (SET_REPORT of the Input Mode feature, as Windows does; the
# T14 Gen 4's touchpad is one) and feeds it finger reports, one finger a
# report as hybrid-mode touchpads send them: a one-finger tap is a left
# click, a two-finger tap a right click, a touch held too long none, one
# finger moves the pointer by the pad's physical size, two fingers moving
# down or left turn the wheel and the horizontal wheel in whole notches (the
# content follows the fingers), a palm moves nothing and pressing the pad
# with two fingers on it is a right click.  QEMU has no touchpad to
# do this with
DOC = ('`hwcheck` touchpad gestures on a modelled precision touchpad: tap to click, two-finger tap for the right '
       'button, two-finger scrolling (vertical and horizontal) as wheel notches, palms ignored')
TESTS = [
    Test('touchpad gestures', 'hwcheck', [r'ok   touchpad: switched to touchpad mode \(Input Mode 3, switches 3\)',
                                          r'ok   touchpad: one-finger tap: left click: m1,0,0 m0,0,0',
                                          r'ok   touchpad: two-finger tap: right click: m2,0,0 m0,0,0',
                                          r'ok   touchpad: two fingers 6 mm down: the wheel 2 notches up',
                                          r'ok   touchpad: two fingers 7 mm left: the horizontal wheel 2 notches right',
                                          r'hwcheck: all passed, 0 failed'], builtin=True),
]
