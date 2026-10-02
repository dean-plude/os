# jq: C (MinGW), a bare .exe (tools/appcorpus.py)
DOC = 'jq'
APP = App('jq', '1.7.1', 'https://github.com/jqlang/jq/releases/download/jq-1.7.1/jq-windows-amd64.exe',
          'jq', [Test('jq --version', rf'{A}\jq\jq.exe --version', [r'jq-1\.7\.1']),
                 Test('jq filter', rf'{A}\jq\jq.exe -c ".a+.b, [.[]]" {A}\data\ab.json', [r'(?m)^42\r?$', r'\[40,2\]'])],
          unpack='exe')
