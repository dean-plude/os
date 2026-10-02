# Python from its NuGet package (tools/appcorpus.py)
DOC = 'Python'
APP = App('Python', '3.14.0', 'https://api.nuget.org/v3-flatcontainer/python/3.14.0/python.3.14.0.nupkg',
          'Python', [Test('python -c', rf'{A}\Python\python.exe -c "import sys, json; '
                          r'print(json.dumps([sum(range(10)), sys.version_info[:2]]))"', [r'\[45, \[3, 14\]\]'],
                          timeout=300)],
          strip='tools')
