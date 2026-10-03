# .NET from its NuGet packages (tools/appcorpus.py): the muxer and hostfxr
# of 8.0 run any later runtime; tests/dotnet/culturetest.dll formats German
# and Japanese through ICU
import os, re, shutil, zipfile

DOC = '.NET (German and Japanese formatting through ICU)'
NUGET = 'https://api.nuget.org/v3-flatcontainer'


def stage_dotnet(app, archives, dest):
    """A dotnet folder as the installer lays it out: dotnet.exe,
    host\\fxr\\V\\hostfxr.dll and shared\\Microsoft.NETCore.App\\V, from the
    runtime, host and host-resolver packages; plus the culture test"""
    runtime, host, resolver = archives
    fxr = re.search(r'(\d+\.\d+\.\d+)\.nupkg$', resolver).group(1)
    shared = os.path.join(dest, 'shared', 'Microsoft.NETCore.App', app.version)
    for archive, folder, keep in [(host, dest, lambda n: n == 'dotnet.exe'),
                                  (resolver, os.path.join(dest, 'host', 'fxr', fxr), lambda n: True),
                                  (runtime, shared, lambda n: True)]:
        os.makedirs(folder, exist_ok=True)
        with zipfile.ZipFile(archive) as z:
            for m in z.namelist():
                name = m.rsplit('/', 1)[-1]
                if m.startswith('runtimes/win-x64/') and ('/native/' in m or '/lib/net' in m) and name and keep(name):
                    with z.open(m) as src, open(os.path.join(folder, name), 'wb') as out:
                        shutil.copyfileobj(src, out)
                elif name.startswith('Microsoft.NETCore.App.') and name.endswith('.json'):
                    with z.open(m) as src, open(os.path.join(shared, name), 'wb') as out:
                        shutil.copyfileobj(src, out)
    for n in ('culturetest.dll', 'culturetest.runtimeconfig.json'):
        shutil.copy(os.path.join(ROOT, 'tests', 'dotnet', n), dest)


APP = App('.NET', '10.0.12', f'{NUGET}/microsoft.netcore.app.runtime.win-x64/10.0.12/'
          'microsoft.netcore.app.runtime.win-x64.10.0.12.nupkg',
          'dotnet', [Test('dotnet runtimes', rf'{A}\dotnet\dotnet.exe --list-runtimes',
                          [r'Microsoft\.NETCore\.App 10\.0\.12'], timeout=300),
                     Test('culture formats', rf'{A}\dotnet\dotnet.exe {A}\dotnet\culturetest.dll',
                          [r'globalization: ICU', r'culturetest: ok'], timeout=300)],
          unpack=stage_dotnet,
          extra=[f'{NUGET}/runtime.win-x64.microsoft.netcore.dotnethost/8.0.31/'
                 'runtime.win-x64.microsoft.netcore.dotnethost.8.0.31.nupkg',
                 f'{NUGET}/runtime.win-x64.microsoft.netcore.dotnethostresolver/8.0.31/'
                 'runtime.win-x64.microsoft.netcore.dotnethostresolver.8.0.31.nupkg'])
