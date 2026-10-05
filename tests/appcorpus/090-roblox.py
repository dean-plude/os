# Roblox's own installer, as a user gets it from roblox.com (tools/appcorpus.py):
# it fetches its settings and the current client's packages from Roblox's
# servers over HTTPS (so NovaOS needs the internet, QEMU's user network),
# installs the client in C:\AppData\Local\Roblox\Versions\version-*\ and
# starts it.  The test passes when the client is installed.  The client
# itself does not get far yet: its Hyperion anti-cheat ends it at start
# (docs/compatibility.md); the installer then exits on its own.  Without an
# account nothing past the login screen could be tested anyway.
# The download (about 220 MB over one HTTP/2 connection) takes 130-160 s
# under TCG.  It once stopped part way in two runs of five: the whole
# machine had frozen on kernel lock waiters that only yielded (fixed in
# kernel/um/um.c; dltest -w in the network self-tests covers that load).
# Where the host reaches the internet through a proxy on its loopback (as
# in a sandbox), the installer is pointed at it by HTTPS_PROXY (it uses
# libcurl, which reads it).
import os, re, shutil

DOC = 'Roblox (installs; the client stops in its anti-cheat)'
INSTALLER = 'RobloxPlayerInstaller.exe'


def unpack(app, files, dest):
    """the installer under its own name"""
    os.makedirs(dest)
    shutil.copy(files[0], os.path.join(dest, INSTALLER))


proxy = re.match(r'https?://(127\.0\.0\.1|localhost):(\d+)/?$', os.environ.get('HTTPS_PROXY', ''))
PROXY = f'set HTTPS_PROXY=http://10.0.2.2:{proxy.group(2)}&& ' if proxy else ''

APP = App('Roblox', 'current', 'https://www.roblox.com/download/client?os=win', 'Roblox',
          [Test('install the client', rf'cmd.exe /c "{PROXY}start /wait {A}\Roblox\{INSTALLER} & '
                r'dir /s /b C:\AppData\Local\Roblox\Versions\RobloxPlayerBeta.exe"',
                [r'Versions\\version-[0-9a-f]+\\RobloxPlayerBeta\.exe'], timeout=1500)],
          unpack=unpack, net=True, mutable=True)
