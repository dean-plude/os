# Node.js from its .zip (tools/appcorpus.py)
DOC = 'Node.js'
APP = App('Node.js', '24.9.0', 'https://nodejs.org/dist/v24.9.0/node-v24.9.0-win-x64.zip',
          'node', [Test('node -v', rf'{A}\node\node.exe -v', [r'v24\.9\.0'], timeout=300),
                   Test('node -e', rf'{A}\node\node.exe -e "console.log(6*7, process.platform)"',
                        [r'42 win32'], timeout=300)],
          strip=1)
