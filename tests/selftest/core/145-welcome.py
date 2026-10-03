# the first-boot setup (apps/welcome.c), opened by hand: the test answers
# its screens from the keyboard (a name, a resolution tried and put back),
# then whoami.exe (GetUserName), started after it, runs as that name and
# the Terminal's own whoami says the same
import re, time

DOC = ('the first-boot setup, `start welcome` (name and display pages from the keyboard), '
       'then `whoami` as the name it gave (the program and the Terminal command)')

NAME = 'Ada Lovelace'


def _log(nova):
    return open(nova.serial_path, 'rb').read().decode('latin-1')


def _wait(nova, pattern, count=1, timeout=20):
    for _ in range(int(timeout / 0.25)):
        if len(re.findall(pattern, _log(nova))) >= count:
            return True
        time.sleep(0.25)
    return False


def answer(nova):
    """Next, the name, Next; on the display page one mode to the side and
    back again (the later tests keep their resolution); Next, Start"""
    pages = len(re.findall(r'\[WELCOME\] Page', _log(nova)))
    time.sleep(1)
    nova.qmp.key('ret')
    _wait(nova, r'\[WELCOME\] Page', pages + 1)
    nova.qmp.type(NAME)
    nova.qmp.key('ret')
    _wait(nova, r'\[WELCOME\] Page', pages + 2)
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
    _wait(nova, r'\[WELCOME\] Page', pages + 3)
    nova.qmp.key('ret')
    _wait(nova, r'\[WELCOME\] Finished')
    time.sleep(1)


TESTS = [
    Test('welcome', 'start welcome', [r'\[WELCOME\] Open \(started by hand\)'], builtin=True,
         acts=[(r'\[WELCOME\] Open', answer)]),
    Test('whoami', 'C:\\Windows\\System32\\whoami.exe', [r'nova-pc\\ada lovelace'],
         boot_expect=[r'\[WELCOME\] Finished: user "Ada Lovelace", display \d+x\d+']),
    Test('whoami builtin', 'whoami', [r'nova-pc\\ada lovelace'], builtin=True),
]
