# MinGit: clones the sample repository the corpus stages (tools/appcorpus.py)
DOC = 'MinGit (cloning a repository)'
APP = App('MinGit', '2.51.0',
          'https://github.com/git-for-windows/git/releases/download/v2.51.0.windows.1/MinGit-2.51.0-64-bit.zip',
          'MinGit', [Test('git clone', rf'{A}\MinGit\cmd\git.exe clone {A}\data\src.git {A}\clone',
                          [r'Cloning into'], timeout=300),
                     Test('git log', rf'{A}\MinGit\cmd\git.exe -C {A}\clone log --format=%s',
                          [r'Add the corpus notes', r'First commit']),
                     Test('git status', rf'{A}\MinGit\cmd\git.exe -C {A}\clone status --short --branch',
                          [r'## main\.\.\.origin/main'])])
