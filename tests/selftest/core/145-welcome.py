# the first-boot setup (apps/welcome.c), opened by hand: the test answers
# its screens from the keyboard (a name, a time zone found by typing a
# city, the German keyboard layout tried in its test field: Y and Z swap
# and the dead acute accent goes on an e, a resolution tried and put
# back); kbdtest (64-bit) sees German as programs do (ToUnicode,
# MapVirtualKey, AltGr, dead keys) and puts US back through
# SPI_SETDEFAULTINPUTLANG, before any test types a command it would
# garble (kbdtest's own command line types the same in both); then
# whoami.exe (GetUserName),
# started after it, runs as that name and the Terminal's own whoami says
# the same; tztest (64- and 32-bit) sees the zone chosen and its offset
# in GetLocalTime and the C runtime's localtime, and checks the zones'
# daylight-saving rules; tzutil puts UTC back for the tests after these
import re, time

DOC = ('the first-boot setup, `start welcome` (name, time zone, keyboard layout and display pages from '
       'the keyboard; German typed in the layout\'s test field), `kbdtest` with German in effect and US '
       'put back, then `whoami` as the name it gave (the program and the Terminal command), `tztest` in the zone '
       'it chose (Tokyo: UTC+9 in GetLocalTime and localtime; Berlin, Sydney and Los Angeles rules) '
       'and `tzutil /s UTC`')

NAME = 'Ada Lovelace'
CITY = 'Tokyo'


def _log(nova):
    return open(nova.serial_path, 'rb').read().decode('latin-1')


def _wait(nova, pattern, count=1, timeout=20):
    for _ in range(int(timeout / 0.25)):
        if len(re.findall(pattern, _log(nova))) >= count:
            return True
        time.sleep(0.25)
    return False


def answer(nova):
    """Next, the name, Next; the time zone found by its city, Next; German
    chosen from the list of layouts (Down from US) and tried: the keys
    printed Y and Z, then the acute accent key and e; Next; on the display
    page one mode to the side and back again (the later tests keep their
    resolution); Next, Start"""
    pages = len(re.findall(r'\[WELCOME\] Page', _log(nova)))
    time.sleep(1)
    nova.qmp.key('ret')
    _wait(nova, r'\[WELCOME\] Page', pages + 1)
    nova.qmp.type(NAME)
    nova.qmp.key('ret')
    _wait(nova, r'\[WELCOME\] Page', pages + 2)
    time.sleep(1)
    nova.qmp.type(CITY)
    time.sleep(1)
    nova.qmp.key('ret')
    _wait(nova, r'\[WELCOME\] Page', pages + 3)
    time.sleep(1)
    for _ in range(6):                                   # US, UK, Dvorak, German
        if re.search(r'\[KBD\] Keyboard layout 00000407', _log(nova)):
            break
        layouts = len(re.findall(r'\[KBD\] Keyboard layout', _log(nova)))
        nova.qmp.key('down')
        _wait(nova, r'\[KBD\] Keyboard layout', layouts + 1, 10)
    time.sleep(1)
    nova.qmp.key('y')
    _wait(nova, r'\[WELCOME\] Typed', 1, 10)
    nova.qmp.key('z')
    _wait(nova, r'\[WELCOME\] Typed', 2, 10)
    nova.qmp.key('equal')                                # German: the dead acute accent
    time.sleep(0.5)
    nova.qmp.key('e')
    _wait(nova, r'\[WELCOME\] Typed', 3, 10)
    time.sleep(1)
    nova.qmp.key('ret')
    _wait(nova, r'\[WELCOME\] Page', pages + 4)
    modes = len(re.findall(r'\[WELCOME\] Display', _log(nova)))
    there, back = ('right', 'left')
    nova.qmp.key(there)
    if not _wait(nova, r'\[WELCOME\] Display', modes + 1, 10):   # (the last mode: try the one before)
        there, back = back, there
        nova.qmp.key(there)
        _wait(nova, r'\[WELCOME\] Display', modes + 1, 10)
    time.sleep(2)
    nova.qmp.key(back)
    _wait(nova, r'\[WELCOME\] Display', modes + 2, 10)
    time.sleep(2)
    nova.qmp.key('ret')
    _wait(nova, r'\[WELCOME\] Page', pages + 5)
    nova.qmp.key('ret')
    _wait(nova, r'\[WELCOME\] Finished')
    time.sleep(1)


TZ_TOKYO = r'\[TZ\] zone Tokyo Standard Time bias -540 local-utc 540 crt 540'

TESTS = [
    Test('welcome', 'start welcome', [r'\[WELCOME\] Open \(started by hand\)'], builtin=True,
         acts=[(r'\[WELCOME\] Open', answer)]),
    Test('kbdtest', 'kbdtest 00000407 us',
         [r'\[KBD\] layout 00000407 hkl 0x04070407', r'\[KBD\] now 04090409', r'kbdtest: \d+ passed, 0 failed'],
         boot_expect=[r'\[KBD\] Keyboard layout 00000407 \(German\)',
                      r'\[WELCOME\] Typed "zy<U\+00E9>" with 00000407',
                      r'\[WELCOME\] Finished: user "Ada Lovelace", time zone Tokyo Standard Time, '
                      r'keyboard 00000407, display \d+x\d+',
                      r'\[KBD\] Keyboard layout 00000409']),
    Test('whoami', 'C:\\Windows\\System32\\whoami.exe', [r'nova-pc\\ada lovelace'],
         boot_expect=[r'\[TZ\] Time zone Tokyo Standard Time \(bias -540\)']),
    Test('whoami builtin', 'whoami', [r'nova-pc\\ada lovelace'], builtin=True),
    Test('tztest', 'tztest', [TZ_TOKYO, r'tztest: \d+ passed, 0 failed']),
    Test('tztest x86', r'C:\Programs\x86\tztest.exe', [TZ_TOKYO, r'tztest: \d+ passed, 0 failed']),
    Test('kbdtest x86', r'C:\Programs\x86\kbdtest.exe', [r'\[KBD\] layout 00000409', r'kbdtest: \d+ passed, 0 failed']),
    Test('tzutil', 'tzutil /s UTC', []),
    Test('tzutil get', 'tzutil /g', [r'(?m)^UTC\r?$']),
]
