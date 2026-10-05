# Steam's own installer, as a user gets it from store.steampowered.com
# (tools/appcorpus.py): SteamSetup.exe /S installs the bootstrapper in
# C:\Programs\Steam; its first start then downloads the client from
# Valve's servers over HTTPS (so NovaOS needs the internet, QEMU's user
# network), first the 32-bit one, then the 64-bit one it updates itself
# to, and starts it with its browser, steamwebhelper.exe (Chromium).  The
# test passes when the 64-bit client and its browser are installed and the
# browser started.  The login window does not come up within the test's
# time under emulation: the browser starts its GPU, network and storage
# processes and creates its first page (docs/compatibility.md).  Signing
# in and installing a game need an account, a hand check in
# docs/compatibility/497-steam.md.
# Where the host reaches the internet through a proxy on its loopback (as
# in a sandbox), Steam is pointed at it the way Windows does it, in the
# user's Internet Settings (Steam reads them through WinHTTP).
import os, re, shutil

DOC = 'Steam (installs and updates itself; its browser does not open the login window yet)'
INSTALLER = 'SteamSetup.exe'
STEAM = r'C:\Programs\Steam'
INET = r'"HKCU\Software\Microsoft\Windows\CurrentVersion\Internet Settings"'


def unpack(app, files, dest):
    """the installer under its own name"""
    os.makedirs(dest)
    shutil.copy(files[0], os.path.join(dest, INSTALLER))


proxy = re.match(r'https?://(127\.0\.0\.1|localhost):(\d+)/?$', os.environ.get('HTTPS_PROXY', ''))
PROXY = (f'reg add {INET} /v ProxyServer /t REG_SZ /d 10.0.2.2:{proxy.group(2)} /f & '
         f'reg add {INET} /v ProxyEnable /t REG_DWORD /d 1 /f & ') if proxy else ''

APP = App('Steam', 'current', 'https://cdn.akamai.steamstatic.com/client/installer/SteamSetup.exe', 'Steam',
          [Test('install Steam', rf'cmd.exe /c "start /wait {A}\Steam\{INSTALLER} /S & dir /b {STEAM}\Steam.exe"',
                [r'Steam\.exe'], timeout=600),
           Test('update the client', rf'cmd.exe /c "{PROXY}start {STEAM}\Steam.exe & timeout /t 900 /nobreak & '
                rf'dir /b {STEAM}\steamclient64.dll {STEAM}\bin\cef\cef.win64\libcef.dll"',
                [r'steamclient64\.dll', r'libcef\.dll', r'\[UM\] Started steamwebhelper\.exe'], timeout=1500)],
          unpack=unpack, net=True, mutable=True)
