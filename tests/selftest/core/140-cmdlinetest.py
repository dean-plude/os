# cmdlinetest: long command lines reach programs whole.  A 1,200-character
# command is typed into the Terminal (the Terminal used to stop at 158) and
# another into cmd.exe's prompt (line input through the console); the program
# checks its GetCommandLineW/A length, its argv (quoted paths with spaces,
# escaped quotes) and the last argument.  "cmdlinetest spawn" passes command
# lines of 1,000, 8,191 and 32,766 characters to CreateProcessW/A and
# cmd.exe /c, and checks that 32,767 are refused, 64- and 32-bit.
DOC = '`cmdlinetest` (a 1,200-character command typed into the Terminal and into cmd.exe, CreateProcess with up to 32,766 characters, 64- and 32-bit)'


def _fnv(h, s):
    for b in s.encode('utf-8') + b'\0':
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def _quote(a):
    if a and not any(c in a for c in ' \t"'):
        return a
    out, bs = '"', 0
    for c in a:
        if c == '\\':
            bs += 1
            continue
        out += '\\' * (2 * bs + 1 if c == '"' else bs) + c
        bs = 0
    return out + '\\' * (2 * bs) + '"'


def long_line(total, prog='cmdlinetest'):
    """'PROG len=N sum=S ARGS... xxx END', @total characters (as cmdlinetest.c's make_line)"""
    args = [r'C:\Program Files\A Folder With Spaces\and a long file name.txt', 'say "hello" twice',
            'C:\\trailing backslash\\', r'back\\slashes\in\the\middle']
    head = len(prog) + 23
    used = lambda: sum(1 + len(_quote(a)) for a in args)          # (each with the space before it)
    i = 1
    while total - head - used() - 5 > 8:
        args.append('w%05d' % i)
        i += 1
    pad = total - head - used() - 5
    args += ['x' * pad, 'END']
    h = 2166136261
    for a in args:
        h = _fnv(h, a)
    line = '%s len=%05d sum=%08x %s' % (prog, total - len(prog) - 1, h, ' '.join(_quote(a) for a in args))
    assert len(line) == total, (len(line), total)
    return line


def _into_cmd(nova):
    nova.qmp.type(long_line(1100) + '\n')
    nova.qmp.type('exit\n')


OK = r'cmdlinetest: command line of (\d+) characters, \d+ arguments: ok'
TESTS = [
    Test('cmdline typed', long_line(1200), [r'cmdlinetest: command line of 1200 characters, \d+ arguments: ok'],
         timeout=240),
    Test('cmdline into cmd.exe', 'cmd', [r'cmdlinetest: command line of 1100 characters, \d+ arguments: ok'],
         timeout=300, acts=[(r'NovaOS \[Version 10', _into_cmd)]),
    Test('cmdline spawn x64', 'cmdlinetest spawn', [OK, r'cmdlinetest spawn: \d+ passed, 0 failed']),
    Test('cmdline spawn x86', r'C:\Programs\x86\cmdlinetest.exe spawn', [OK, r'cmdlinetest spawn: \d+ passed, 0 failed']),
]
