# xpwin: windows of different processes working together, as WebView2's
# host and the browser and GPU processes need.  A host parents another
# process's hidden window in a child window of its own (SetParent before
# the window has a desktop window, as the browser's is), places, moves and
# shows it (SetWindowPos, MoveWindow, ShowWindow, SetWindowLong), sends it
# messages (SendMessage, SendMessageTimeout, WM_COPYDATA, text and class
# queries) and gets its posted ones; the child sees its foreign parent,
# size and place; a third process draws into it with GetDC and BitBlt;
# the window follows its host and parent as they move, hide and show, and
# SetParent(NULL) lets it go.  64- and 32-bit.
DOC = '`xpwin` (windows of different processes: SetParent, SetWindowPos, ShowWindow and messages across processes, GetDC on another process\'s window; 64- and 32-bit)'
TESTS = [
    Test('xpwin x64', 'xpwin', [r'xpwin: \d+ passed, 0 failed \(64-bit\)'], timeout=240),
    Test('xpwin x86', r'C:\Programs\x86\xpwin.exe', [r'xpwin: \d+ passed, 0 failed \(32-bit\)'], timeout=240),
]
