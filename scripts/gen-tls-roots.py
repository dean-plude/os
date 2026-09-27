#!/usr/bin/env python3
"""Regenerate kernel/net/tls_roots.c - NovaOS's built-in root certificate
store - from a PEM bundle of trusted CAs.

    scripts/gen-tls-roots.py [bundle.pem]

Default: Mozilla's CA list as shipped by the Debian/Ubuntu ca-certificates
package (/usr/share/ca-certificates/mozilla/*.crt).  Each certificate is
embedded as DER; the kernel parses them with Mbed TLS at start-up.
"""
import base64, glob, os, re, subprocess, sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
out = os.path.join(root, 'kernel', 'net', 'tls_roots.c')

if len(sys.argv) > 1:
    pem = open(sys.argv[1]).read()
    source = os.path.basename(sys.argv[1])
else:
    files = sorted(glob.glob('/usr/share/ca-certificates/mozilla/*.crt'))
    pem = ''.join(open(f).read() for f in files)
    try:
        ver = subprocess.run(['dpkg-query', '-W', '-f', '${Version}', 'ca-certificates'],
                             capture_output=True, text=True).stdout.strip() or '?'
    except OSError:
        ver = '?'
    source = f'Mozilla CA list, ca-certificates {ver}'

blocks = re.findall(r'-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----', pem, re.S)
ders = [base64.b64decode(''.join(b.split())) for b in blocks]

lines = [
    '/*',
    ' * tls_roots.c — built-in trusted root certificates (GENERATED, do not edit)',
    ' *',
    f' * Source: {source} ({len(ders)} roots).',
    ' * Regenerate with scripts/gen-tls-roots.py.',
    ' */',
    '',
    '#include "tls.h"',
    '',
]
for i, der in enumerate(ders):
    lines.append(f'static const UINT8 ROOT{i}[{len(der)}] = {{')
    for j in range(0, len(der), 16):
        lines.append('    ' + ', '.join(f'0x{b:02X}' for b in der[j:j + 16]) + ',')
    lines.append('};')
lines.append('')
lines.append('const TlsRootDer g_tls_builtin_roots[] = {')
for i, der in enumerate(ders):
    lines.append(f'    {{ ROOT{i}, sizeof(ROOT{i}) }},')
lines.append('};')
lines.append('')
lines.append(f'const int g_tls_builtin_root_count = {len(ders)};')
open(out, 'w').write('\n'.join(lines) + '\n')
print(f'Wrote {out} ({len(ders)} roots)')
