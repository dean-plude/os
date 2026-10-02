# ripgrep: Rust (MSVC) (tools/appcorpus.py; App, Test, A = C:\Apps given)
DOC = 'ripgrep'
APP = App('ripgrep', '14.1.1',
          'https://github.com/BurntSushi/ripgrep/releases/download/14.1.1/ripgrep-14.1.1-x86_64-pc-windows-msvc.zip',
          'rg', [Test('rg --version', rf'{A}\rg\rg.exe --version', [r'ripgrep 14\.1\.1']),
                 Test('rg search', rf'{A}\rg\rg.exe -n needle {A}\data', [r'hello\.txt\r?\n2:a needle in a haystack'])],
          strip=1)
