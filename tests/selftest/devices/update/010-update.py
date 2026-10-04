# Updating NovaOS (kernel/fs/update.h): the boot disk is build/nova.img, an
# installed NovaOS of this build's version, with a network on which
# tools/mkupdate.py's update channels are served (update_boot in
# tools/selftest.py): v1/ is this very build stamped one version newer (a
# "0.1.1-test" for a 0.1.0 build), v2/ one newer again.  NovaOS finds v1's
# update, downloads and stages it, restarts into it (the boot loader starts
# kernel.new once and the new kernel makes it the installed one), and
# restarts cleanly again.  Then v2's update is staged, and the machine is
# reset while the new kernel is still starting: the next start goes back
# to the v1 kernel and throws the update away.  Both channels are signed
# with the self-tests' key (005-signature.py).
import os, re, time

DOC = ('an installed NovaOS updating itself from an update channel to a newer test build, restarting into it '
       'twice, and going back to that version when the next update is reset while it first starts')


def _version():
    """The built kernel's stamped version (kernel/ke/version.c), and the next two"""
    try:
        data = open(os.path.join(os.path.dirname(__file__), '..', '..', '..', '..', 'build', 'kernel.elf'), 'rb').read()
        at = data.index(b'NovaOS-version-stamp:') + 21
        ver = data[at:at + 27].split(b'\0')[0].decode()
    except (OSError, ValueError):
        ver = '0.1.0'
    nums = [int(x) for x in re.match(r'[0-9.]+', ver).group(0).strip('.').split('.')]
    nums += [0] * (3 - len(nums))
    return ver, '.'.join(map(str, nums[:2] + [nums[2] + 1])) + '-test', '.'.join(map(str, nums[:2] + [nums[2] + 2])) + '-test'


VER, V1, V2 = _version()
CHANNEL = 'http://10.0.2.2:18090/{}/novaos-update.txt'


def _serial(nova):
    return open(nova.serial_path, 'rb').read().decode('latin-1')


def reset_while_trying(nova):
    """Restart into the staged update and reset the machine as soon as the
    new kernel says it is starting (before it reaches the desktop and
    finishes the update), then wait for the next start"""
    nova.sr.read_new()
    seen = len(_serial(nova))
    nova.qmp.type('shutdown /r\n')
    end = time.time() + 300
    while "This is the update's first start" not in _serial(nova)[seen:] and time.time() < end:
        time.sleep(0.05)
    nova.qmp.cmd('system_reset')
    nova.start()


def E(v):
    return re.escape(v)


TESTS = [
    Test('update channel', f'update channel {CHANNEL.format("v1")}', [r'Update channel: http://10\.0\.2\.2:18090/v1/'],
         builtin=True),
    Test('update check', 'update', [rf'NovaOS {E(V1)} is available \(', r"Type 'update install'"], builtin=True, timeout=120,
         boot_expect=[r"\[UPDATE\] The channel's signature is good \(key 8cafb488b4fdb69e"]),
    Test('update install', 'update install', [rf'NovaOS {E(V1)} is ready: restart'], builtin=True, timeout=900,
         boot_expect=[rf'\[UPDATE\] NovaOS {E(V1)} is staged']),
    Test('restart into it', 'shutdown /r', [rf"\[UPDATE\] This is the update's first start \(NovaOS {E(V1)}",
                                             rf'\[version {E(V1)}\]'], reboot=True, timeout=600),
    Test('updated', 'ver', [rf'NovaOS \[Version {E(V1)}\]'], builtin=True,
         boot_expect=[rf'\[UPDATE\] Updated NovaOS from {E(VER)} to {E(V1)}']),
    Test('restart again', 'shutdown /r', [rf'\[version {E(V1)}\]'], reboot=True, timeout=600),
    Test('up to date', 'update', [rf'NovaOS {E(V1)} is up to date'], builtin=True, timeout=120),
    Test('next update', f'update channel {CHANNEL.format("v2")}', [r'Update channel: http://10\.0\.2\.2:18090/v2/'],
         builtin=True),
    Test('stage it', 'update install', [rf'NovaOS {E(V2)} is ready: restart'], builtin=True, timeout=900),
    Test('reset while trying', 'ver', [rf'NovaOS \[Version {E(V1)}\]'], builtin=True, before=reset_while_trying,
         timeout=600,
         boot_expect=[r'\[UPDATE\] The boot loader started the previous kernel',
                      rf'\[UPDATE\] The update to NovaOS {E(V2)} did not finish starting, so NovaOS {E(V1)} started again']),
    Test('offered again', 'update', [rf'The update to NovaOS {E(V2)} did not start, so NovaOS went back to {E(V1)}',
                                     rf'NovaOS {E(V2)} is available \('], builtin=True, timeout=120),
    Test('store updates', 'store updates', [r"Opened the App Store's updates"], builtin=True),
]
