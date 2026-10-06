# The Microsoft Edge WebView2 runtime's offline installer (tools/appcorpus.py),
# the runtime Roblox's login screen and many other programs embed.  It is
# Microsoft Edge Update (MicrosoftEdgeUpdate.exe, 32-bit) carrying the
# runtime's own setup.  Edge Update starts, reads its manifests with MSXML
# 6, installs itself, registers its COM servers and proxy/stub DLLs and
# runs its install step, which it logs to %TEMP%\MicrosoftEdgeUpdate.log;
# the test requires a successful runtime installation, then wv2host must
# find the runtime, create an environment and controller, load its page,
# execute JavaScript with the expected result and finish. Missing runtimes
# and full disks are failures, not evidence of compatibility.
DOC = 'Microsoft Edge WebView2 runtime (installs; a host creates a controller, loads a page and executes JavaScript)'
import os, shutil, zipfile

SDK = 'https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.4258.31'

INSTALLER = 'MicrosoftEdgeWebView2RuntimeInstallerX64.exe'


def unpack(app, files, dest):
    """the installer under its own name; the SDK's x64 loader beside it"""
    os.makedirs(dest)
    shutil.copy(files[0], os.path.join(dest, INSTALLER))
    with zipfile.ZipFile(files[1]) as z:
        with open(os.path.join(dest, 'WebView2Loader.dll'), 'wb') as f:
            f.write(z.read('runtimes/win-x64/native/WebView2Loader.dll'))


APP = App('WebView2', 'evergreen', 'https://go.microsoft.com/fwlink/?linkid=2124701', 'WebView2',
          [Test('run Edge Update\'s install step',
                rf'cmd.exe /c "start /wait {A}\WebView2\{INSTALLER} /silent /install & '
                r'type C:\AppData\Local\Temp\MicrosoftEdgeUpdate.log"',
                [r'\[GoopdateImpl::DoInstall\]', r'InstallApp returned(?:\]\[| )0x0\b'], timeout=600),
           Test('load a page and execute JavaScript',
                rf'cmd.exe /c "cd /d {A}\WebView2 & wv2host"',
                [r'wv2host: runtime [^\r\n]+', r'wv2host: environment\r?\n',
                 r'wv2host: controller \(browser process \d+\)', r'wv2host: navigation ok\r?\n',
                 r'wv2host: script "NovaOS WebView2 / 42"\r?\n', r'wv2host: done\r?\n'], timeout=600)],
          unpack=unpack, extra=[SDK], mutable=True, online=True)
