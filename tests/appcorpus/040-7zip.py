# 7-Zip's console program, from its installer (tools/appcorpus.py)
DOC = '7-Zip'
APP = App('7-Zip', '26.03', 'https://github.com/ip7z/7zip/releases/download/26.03/7z2603-x64.exe',
          '7-Zip', [Test('7z a', rf'{A}\7-Zip\7z.exe a {A}\data.7z {A}\data', [r'Everything is Ok']),
                    Test('7z t', rf'{A}\7-Zip\7z.exe t {A}\data.7z', [r'Type = 7z', r'Everything is Ok'])],
          unpack='7z')
