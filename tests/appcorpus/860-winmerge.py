# WinMerge compares two files (hello.txt and hello2.txt) side by side; its
# screenshot must match tests/reference/winmerge.png.  Windowed, takes the
# keyboard: runs after the console programs.
DOC = 'WinMerge'
APP = App('WinMerge', '2.16.50',
          'https://github.com/WinMerge/winmerge/releases/download/v2.16.50/winmerge-2.16.50-x64-exe.zip',
          'WinMerge', [Test('compare two files', rf'start {A}\WinMerge\WinMergeU.exe {A}\data\hello.txt {A}\data\hello2.txt',
                            timeout=25)],
          strip=1, gui=True)
