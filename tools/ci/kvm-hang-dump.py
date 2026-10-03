#!/usr/bin/env python3
"""Temporary (kernel-under-KVM work): after a hung self-test run, print
each still-running NovaOS VM's CPU registers (QMP), the kernel functions
their RIPs are in, and the end of its serial log."""
import glob, json, os, re, socket, subprocess, sys

ELF = sys.argv[1] if len(sys.argv) > 1 else 'build/kernel.elf'

def qmp(path, *hmp):
    s = socket.socket(socket.AF_UNIX)
    s.settimeout(10)
    s.connect(path)
    f = s.makefile('rw')
    f.readline()
    out = []
    for c in [None] + list(hmp):
        if c is None:
            f.write(json.dumps({'execute': 'qmp_capabilities'}) + '\n')
        else:
            f.write(json.dumps({'execute': 'human-monitor-command', 'arguments': {'command-line': c}}) + '\n')
        f.flush()
        while True:
            r = json.loads(f.readline())
            if 'return' in r or 'error' in r:
                break
        if c:
            out.append(f'--- {c}\n' + (r.get('return') or str(r.get('error'))))
    return '\n'.join(out)

def where(addrs):
    if not addrs:
        return ''
    try:
        return subprocess.run(['llvm-addr2line', '-f', '-C', '-e', ELF] + addrs,
                              capture_output=True, text=True).stdout
    except OSError as e:
        return str(e)

for d in sorted(glob.glob('/tmp/novarun*')):
    print(f'===== {d}')
    sock = os.path.join(d, 'qmp.sock')
    if os.path.exists(sock):
        try:
            regs = qmp(sock, 'info cpus', 'info registers -a', 'info lapic')
            print(regs)
            rips = sorted(set(re.findall(r'RIP=([0-9a-f]{16})', regs)))
            print('--- RIPs')
            print(where(['0x' + r for r in rips]))
            for m in re.finditer(r'RBP=([0-9a-f]{16})', regs):
                pass
        except OSError as e:
            print('(no QMP:', e, ')')
    log = os.path.join(d, 'serial.log')
    if os.path.exists(log):
        text = open(log, 'rb').read().decode('latin-1')
        print('--- serial log, last 150 lines')
        print('\n'.join(text.splitlines()[-150:]))
