# dpitest: per-monitor DPI.  The program (per-monitor aware v2 by its
# manifest) sets the boot's 2560x1600 monitor to 192 DPI for DPI-aware
# programs and back, and checks WM_DPICHANGED, GetDpiForMonitor and the
# coordinates it and two children (unaware and system aware, by
# __COMPAT_LAYER) see.
DOC = ('`dpitest` (per-monitor DPI: the manifest\'s `dpiAwareness`, `GetDpiForMonitor`, `GetDpiForWindow`, '
       '`WM_DPICHANGED` and its suggested rectangle when the monitor goes to 192 DPI and back, and the '
       'window and monitor coordinates an aware, an unaware and a system-aware process see)')
TESTS = [
    Test('dpitest', 'dpitest', [r'dpitest: \d+ passed, 0 failed', r'dpitest unaware child: \d+ passed, 0 failed',
                                r'dpitest system child: \d+ passed, 0 failed']),
]
