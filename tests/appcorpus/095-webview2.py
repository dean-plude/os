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
# setup.exe), which maps and unpacks its 728 MB archive, copies the
# runtime in, adds an entry to its folder's permissions and installs the
# runtime; the installer then deletes its temporary files on close, which
# must not loop (docs/compatibility.md).  Then wv2host (a WebView2 host
# NovaOS builds, userland/programs/wv2host.c) starts the runtime through
# WebView2Loader.dll from Microsoft's WebView2 SDK (its NuGet package,
# BSD-licensed, the DLL copied next to the installer): the loader finds
# the installed runtime and the browser process (msedgewebview2.exe) and
# its GPU, network and storage processes start; the test passes when the
# WebView2 environment is made (the controller, and so a page, is not
# yet: docs/compatibility.md), or when no runtime was installed (drive C:
# full in a full corpus run, as above) and so none could start.
DOC = 'Microsoft Edge WebView2 runtime (its updater installs itself, runs the install, accepts the runtime\'s signature and starts the runtime\'s setup, which installs the runtime; a host program starts the runtime\'s browser process)'
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
                [r'\[GoopdateImpl::DoInstall\]'], timeout=600),
           Test('start the runtime\'s browser process',
                rf'cmd.exe /c "cd /d {A}\WebView2 & wv2host"',
                [r'wv2host: (environment\r?\n|no runtime found)'], timeout=600)],
          unpack=unpack, extra=[SDK])
