# Keep this last (900): Notepad++ takes the keyboard.  It opens a file; its
# screenshot must match tests/reference/notepad++.png (tools/appcorpus.py)
DOC = 'Notepad++'
APP = App('Notepad++', '8.8.3',
          'https://github.com/notepad-plus-plus/notepad-plus-plus/releases/download/v8.8.3/npp.8.8.3.portable.x64.zip',
          'npp', [Test('open a file', rf'start {A}\npp\notepad++.exe {A}\data\hello.txt')])
