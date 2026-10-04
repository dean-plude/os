# The Microsoft Edge WebView2 runtime's offline installer (tools/appcorpus.py),
# the runtime Roblox's login screen and many other programs embed.  It is
# Microsoft Edge Update (MicrosoftEdgeUpdate.exe, 32-bit) carrying the
# runtime's own setup.  Edge Update starts, reads its manifests with MSXML
# 6, installs itself, registers its COM servers and proxy/stub DLLs and
# runs its install step, which it logs to %TEMP%\MicrosoftEdgeUpdate.log;
# the test passes when that log shows the install step ran (whether the
# package is cached also depends on drive C:'s free space, which the
# programs before it in a full corpus run use up).  The install
# then unpacks the runtime's package, accepts Microsoft's signature on it
# and starts the runtime's own setup (Chromium's mini_installer and
# setup.exe), which cannot map its archive into memory yet and stops on
# the missing wer.dll, so nothing is installed yet; the installer then
# deletes its temporary files on close, which must not loop
# (docs/compatibility.md).  Once the setup passes, the next test expects
# the runtime's files in C:\Programs\Microsoft\EdgeWebView.
DOC = 'Microsoft Edge WebView2 runtime (its updater installs itself, runs the install, accepts the runtime\'s signature and starts the runtime\'s setup, which cannot unpack its archive yet)'
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
