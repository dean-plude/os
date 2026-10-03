# dpitest: per-monitor DPI.  The program (per-monitor aware v2 by its
# manifest) sets the boot's 2560x1600 monitor to 192 DPI for DPI-aware
# programs and back, and checks WM_DPICHANGED, GetDpiForMonitor and the
# coordinates it and two children (unaware and system aware, by
# __COMPAT_LAYER) see; user32's own parts at 192 DPI against a window of
# a thread in another awareness context.
DOC = ('`dpitest` (per-monitor DPI: the manifest\'s `dpiAwareness`, `GetDpiForMonitor`, `GetDpiForWindow`, '
       '`WM_DPICHANGED` and its suggested rectangle when the monitor goes to 192 DPI and back, and the '
       'window and monitor coordinates an aware, an unaware and a system-aware process see; user32\'s controls, '
       'menus, scroll bars, dialogs, metrics and fonts at 192 DPI; per-thread awareness contexts)')
TESTS = [
    Test('dpitest', 'dpitest', [r'dpitest: \d+ passed, 0 failed', r'dpitest unaware child: \d+ passed, 0 failed',
                                r'dpitest system child: \d+ passed, 0 failed']),
]
