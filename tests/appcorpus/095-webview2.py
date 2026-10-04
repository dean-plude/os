# The Microsoft Edge WebView2 runtime's offline installer (tools/appcorpus.py),
# the runtime Roblox's login screen and many other programs embed.  It is
# Microsoft Edge Update (MicrosoftEdgeUpdate.exe, 32-bit) carrying the
# runtime's own setup.  Edge Update now starts, reads its policies and runs
# its install step, which it logs to %TEMP%\MicrosoftEdgeUpdate.log; the
# test passes when that log shows the install step ran.  The install itself
# stops there: Edge Update first makes an MSXML 6 DOMDocument, which NovaOS
# does not have yet, and reports that Windows needs an update
# (docs/compatibility.md).  Once MSXML lands, the next test expects the
# runtime's files in C:\Programs\Microsoft\EdgeWebView.
DOC = 'Microsoft Edge WebView2 runtime (its updater runs; the install stops: no MSXML 6 yet)'
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
