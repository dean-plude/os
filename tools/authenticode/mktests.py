#!/usr/bin/env python3
"""Make the Authenticode self-test's files (tests/selftest/core/
149-authtest.py, userland/programs/authtest.c):

    mktests.py OUT_DIR PE64 PE32

A test certificate authority ("NovaOS Test Root", generated from fixed
seeds, so every build makes the same files) signs copies of the two
programs in the ways WinVerifyTrust must accept or refuse.  The root is
written as testroot.cer and is trusted by nobody: authtest adds it to the
ROOT store for its checks and removes it afterwards.

    signed.exe          SHA-256, by the test signer               trusted
    signed-sha1.exe     SHA-1                                       trusted
    signed32.exe        a 32-bit (PE32) program                    trusted
    nested.exe          SHA-1, with a nested SHA-256 signature     trusted
    expired-ts.exe      expired signer, PKCS #9 timestamp in time  trusted
    expired-rfc3161.exe expired signer, RFC 3161 timestamp in time trusted
    tampered.exe        signed.exe with a code byte changed        TRUST_E_BAD_DIGEST
    badsig.exe          signed.exe with its signature damaged      TRUST_E_BAD_DIGEST
    untrusted.exe       signed under a root nobody trusts          CERT_E_UNTRUSTEDROOT
    expired.exe         expired signer, no timestamp               CERT_E_EXPIRED
    expired-latets.exe  expired signer, timestamped after expiry   CERT_E_EXPIRED
    wrongusage.exe      a TLS server certificate signed it         CERT_E_WRONG_USAGE
    unsigned.exe        no signature                               TRUST_E_NOSIGNATURE
    notpe.txt           not a program                              TRUST_E_SUBJECT_FORM_UNKNOWN
    signed.sha1         the SHA-1 catalog hash of signed.exe (hex)
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pki import Cert, Key                                       # noqa: E402
from sign import pad, pe_digest, sign                          # noqa: E402

LONG = ((2000, 1, 1, 0, 0, 0), (2049, 12, 31, 23, 59, 59))


def first_section(pe):
    """the file offset of the first section's data"""
    e = struct.unpack_from('<I', pe, 60)[0]
    nsec, opt = struct.unpack_from('<H', pe, e + 6)[0], struct.unpack_from('<H', pe, e + 20)[0]
    sec = e + 24 + opt
    return min(struct.unpack_from('<I', pe, sec + 40 * i + 20)[0] for i in range(nsec)
               if struct.unpack_from('<I', pe, sec + 40 * i + 16)[0])


def main(out, pe64_path, pe32_path):
    pe64, pe32 = open(pe64_path, 'rb').read(), open(pe32_path, 'rb').read()
    ca_key, leaf_key, rogue_key = Key('novaos-test-root'), Key('novaos-test-signer'), Key('novaos-test-rogue')
    root = Cert('NovaOS Test Root', ca_key, 1, LONG, ca=True)
    signer = Cert('NovaOS Test Signer', leaf_key, 2, ((2020, 1, 1, 0, 0, 0), LONG[1]), root, ca_key, usage='codesign')
    expired = Cert('NovaOS Test Signer (expired)', leaf_key, 3, ((2001, 1, 1, 0, 0, 0), (2002, 1, 1, 0, 0, 0)), root, ca_key,
                   usage='codesign')
    tsa = Cert('NovaOS Test Time Stamping', leaf_key, 4, LONG, root, ca_key, usage='timestamp')
    server = Cert('NovaOS Test Server', leaf_key, 5, LONG, root, ca_key, usage='server')
    rogue_root = Cert('NovaOS Untrusted Root', rogue_key, 1, LONG, ca=True)
    rogue = Cert('NovaOS Untrusted Signer', leaf_key, 2, LONG, rogue_root, rogue_key, usage='codesign')
    in_time, too_late = (2001, 6, 1, 12, 0, 0), (2003, 1, 1, 12, 0, 0)

    from sign import make_signature
    files = {
        'signed.exe': sign(pe64, signer, [root], 'sha256'),
        'signed-sha1.exe': sign(pe64, signer, [root], 'sha1'),
        'signed32.exe': sign(pe32, signer, [root], 'sha256'),
        'nested.exe': sign(pe64, signer, [root], 'sha1', nested=[make_signature(pad(pe64), signer, [root], 'sha256')]),
        'expired-ts.exe': sign(pe64, expired, [root], 'sha256', timestamp=('pkcs9', tsa, [root], in_time)),
        'expired-rfc3161.exe': sign(pe64, expired, [root], 'sha256', timestamp=('rfc3161', tsa, [root], in_time)),
        'untrusted.exe': sign(pe64, rogue, [rogue_root], 'sha256'),
        'expired.exe': sign(pe64, expired, [root], 'sha256'),
        'expired-latets.exe': sign(pe64, expired, [root], 'sha256', timestamp=('pkcs9', tsa, [root], too_late)),
        'wrongusage.exe': sign(pe64, server, [root], 'sha256'),
        'unsigned.exe': pe64,
        'notpe.txt': b'This is not a program.\r\n',
    }
    good = files['signed.exe']
    t = bytearray(good)
    t[first_section(good) + 16] ^= 0x01
    files['tampered.exe'] = bytes(t)
    b = bytearray(good)
    marker = bytes.fromhex('300d06092a864886f70d0101010500048201')   # rsaEncryption, NULL, then the signature
    at = good.rindex(marker) + len(marker) + 1
    b[at + 10] ^= 0x01
    files['badsig.exe'] = bytes(b)
    files['signed.sha1'] = pe_digest(pad(pe64), 'sha1').hex().upper().encode()
    files['testroot.cer'] = root.der
    os.makedirs(out, exist_ok=True)
    for n, data in files.items():
        with open(os.path.join(out, n), 'wb') as f:
            f.write(data)


if __name__ == '__main__':
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    main(*sys.argv[1:])
