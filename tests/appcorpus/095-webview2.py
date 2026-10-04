# The Microsoft Edge WebView2 runtime's offline installer (tools/appcorpus.py),
# the runtime Roblox's login screen and many other programs embed.  It is
# Microsoft Edge Update (MicrosoftEdgeUpdate.exe, 32-bit) carrying the
# runtime's own setup.  Edge Update starts, reads its manifests with MSXML
# 6, installs itself, registers its COM servers and proxy/stub DLLs and
# runs its install step, which it logs to %TEMP%\MicrosoftEdgeUpdate.log;
# the test passes when that log shows the install step ran.  The install
# then unpacks the runtime's package and stops at Edge Update's check that
# the package carries Microsoft's signature (0xa0430233), so nothing is
# installed yet (docs/compatibility.md).  Once that passes, the next test
# expects the runtime's files in C:\Programs\Microsoft\EdgeWebView.
DOC = 'Microsoft Edge WebView2 runtime (its updater installs itself and runs the install; the install stops at its check of Microsoft\'s signature on the runtime)'
import os, shutil

INSTALLER = 'MicrosoftEdgeWebView2RuntimeInstallerX64.exe'


def unpack(app, files, dest):
    """the installer under its own name"""
    os.makedirs(dest)
    shutil.copy(files[0], os.path.join(dest, INSTALLER))


APP = App('WebView2', 'evergreen', 'https://go.microsoft.com/fwlink/?linkid=2124701', 'WebView2',
          [Test('run Edge Update\'s install step',
                rf'cmd.exe /c "start /wait {A}\WebView2\{INSTALLER} /silent /install & '
                r'type C:\AppData\Local\Temp\MicrosoftEdgeUpdate.log"',
                [r'\[GoopdateImpl::DoInstall\]'], timeout=600)],
          unpack=unpack)
