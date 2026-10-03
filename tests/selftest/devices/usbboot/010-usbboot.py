# nova.iso written to a USB stick, the only thing to start from, and no
# display adapter NovaOS drives (the firmware's GOP only): NovaOS must
# start live from the stick, keep its log in \EFI\NOVA\bootlog.txt there
# (read back on the host with mtools once QEMU has quit), and a kernel
# fault's backtrace must reach that file too (a PC without a serial port)
import os, struct, subprocess

DOC = ('nova.iso written to a USB stick on xHCI, with only the firmware\'s GOP display: NovaOS starts live '
       'from it and its log, a kernel fault\'s backtrace included, is in `\\EFI\\NOVA\\bootlog.txt` on the stick')


def _bootlog(nova):
    """The text of \\EFI\\NOVA\\bootlog.txt on the stick (the GPT's EFI System Partition)"""
    stick = os.path.join(nova.work, 'stick.img')
    with open(stick, 'rb') as f:
        hdr = f.read(1024)[512:]
        table, count, size = struct.unpack('<QII', hdr[72:88])
        f.seek(table * 512)
        ents = f.read(count * size)
    esp = bytes.fromhex('28732ac11ff8d211ba4b00a0c93ec93b')        # C12A7328-F81F-11D2-BA4B-00A0C93EC93B
    first = next((struct.unpack('<Q', ents[i:i + size][32:40])[0] for i in range(0, len(ents), size)
                  if ents[i:i + 16] == esp), None)
    if first is None:
        return None
    r = subprocess.run(['mtype', '-i', f'{stick}@@{first * 512}', '::/EFI/NOVA/bootlog.txt'],
                       capture_output=True, env=dict(os.environ, MTOOLS_SKIP_CHECK='1'))
    return r.stdout.decode('latin-1') if r.returncode == 0 else None


def log_has(*want):
    def check(nova):
        text = _bootlog(nova)
        if text is None:
            return 'no \\EFI\\NOVA\\bootlog.txt on the stick\'s EFI System Partition'
        for w in want:
            if w not in text:
                return f'"{w}" is not in the boot log on the stick'
        return None
    return check


TESTS = [
    Test('usb live boot', 'cmd /c echo usbboot-ok', [r'usbboot-ok'], settle=3,
         boot_expect=[r'\[SETUP\] Running from the installation USB stick',
                      r'\[DISPLAY\] UEFI GOP framebuffer',
                      r'\[BOOTLOG\] Writing the boot log to \\EFI\\NOVA\\bootlog\.txt on usb\d',
                      r'\[DRIVES\] \w: is FAT\d+ volume "NOVA_EFI" on usb\d'],
         check=log_has('[NovaOS] Entering kernel main loop', '[UM] Started cmd.exe')),
    Test('fault in the log', 'crash kernel', [r'Backtrace:\r?\n  #0 [0-9a-f]{16}  KeCrashTestFault\+0x[0-9a-f]+'],
         timeout=60, crash=True, settle=5, check=log_has('KERNEL PAGE FAULT', 'KeCrashTestFault+0x')),
]
