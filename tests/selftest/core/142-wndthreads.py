# wndthreads: a window whose thread has ended answers a sent message with
# 0 at once, and DestroyWindow on a parent returns when a child belongs to
# another thread, one that ends on the child's WM_DESTROY or has already
# ended (VLC never closed: its Qt window thread waited for ever on the
# child window of VLC's ended video thread), 64- and 32-bit.  A watchdog
# fails the test instead of letting a hang freeze the Terminal.
DOC = '`wndthreads` (child windows of other threads, sends to an ended thread\'s window, 64- and 32-bit)'
TESTS = [
    Test('wndthreads x64', 'wndthreads', [r'wndthreads: \d+ passed, 0 failed'], timeout=120),
    Test('wndthreads x86', r'C:\Programs\x86\wndthreads.exe', [r'wndthreads: \d+ passed, 0 failed'], timeout=120),
]
