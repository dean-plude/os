# fd: Rust (MSVC) (tools/appcorpus.py)
DOC = 'fd'
APP = App('fd', '10.2.0',
          'https://github.com/sharkdp/fd/releases/download/v10.2.0/fd-v10.2.0-x86_64-pc-windows-msvc.zip',
          'fd', [Test('fd --version', rf'{A}\fd\fd.exe --version', [r'fd 10\.2\.0']),
                 Test('fd find', rf'{A}\fd\fd.exe -e txt . {A}\data', [r'hello\.txt', r'notes\.txt'])],
          strip=1)
