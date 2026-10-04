#!/usr/bin/env python3
"""Fetch the Sound Open Firmware the audio DSP driver loads.

    tools/fetch_sof_firmware.py [--platform rpl] [--all] [--tarball FILE]

Intel's laptops from Tiger Lake on record from their digital microphones
through the audio DSP beside the HD Audio controller, and the DSP runs
Sound Open Firmware (kernel/drivers/sof.c boots it).  The firmware is a
binary Intel signs and the SOF project publishes as release tarballs at
https://github.com/thesofproject/sof-bin/releases (BSD-3-Clause, with
Intel's firmware licence, LICENCE.Intel, which allows redistribution).  It
is not kept in this repository: this script downloads the pinned release,
checks its SHA-256, and writes into third_party/sof-bin (ignored by git):

    sof-ipc4/rpl/sof-rpl.ri  -> C:\\Windows\\Firmware\\Intel\\sof-ipc4\\rpl\\sof-rpl.ri
    LICENCE.Intel            -> C:\\Windows\\Firmware\\Intel\\LICENCE.Intel

tools/build_userland.py builds whatever is there into the system, so run
this before building.  Without it NovaOS still builds and runs; the DSP
is left off and the boot log says the firmware is missing.

--platform picks the firmware (rpl = Raptor Lake-P, the ThinkPad T14
Gen 4; also tgl, tgl-h, adl, adl-s, adl-n, rpl-s; repeatable); --all takes
every cAVS 2.5 one.  Each is about 700 KB.  --tarball uses an already
downloaded release tarball instead of the network.
"""
import argparse, hashlib, os, shutil, sys, tarfile, tempfile, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'third_party', 'sof-bin')

SOF_VERSION = '2026.09.1'
URL = (f'https://github.com/thesofproject/sof-bin/releases/download/v{SOF_VERSION}/'
       f'sof-bin-{SOF_VERSION}.tar.gz')
SHA256 = '42ce40ec98f366365eab8e046d779b416d80b6ff2513b8f6be2a61a88e679b73'

# cAVS 2.5, the generation kernel/drivers/sof.c boots (its g_plats table)
PLATFORMS = ['tgl', 'tgl-h', 'adl', 'adl-s', 'adl-n', 'rpl', 'rpl-s']


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--platform', action='append', choices=PLATFORMS)
    ap.add_argument('--all', action='store_true')
    ap.add_argument('--tarball')
    args = ap.parse_args()
    plats = PLATFORMS if args.all else (args.platform or ['rpl'])

    with tempfile.TemporaryDirectory() as tmp:
        tgz = args.tarball
        if not tgz:
            tgz = os.path.join(tmp, 'sof-bin.tar.gz')
            print(f'downloading {URL}')
            with urllib.request.urlopen(URL) as r, open(tgz, 'wb') as f:
                shutil.copyfileobj(r, f)
        digest = hashlib.sha256(open(tgz, 'rb').read()).hexdigest()
        if digest != SHA256:
            sys.exit(f'{tgz}: SHA-256 {digest}, expected {SHA256} (sof-bin v{SOF_VERSION})')
        top = f'sof-bin-{SOF_VERSION}'
        with tarfile.open(tgz) as t:
            def extract(member, dest):
                src = t.extractfile(f'{top}/{member}')
                if src is None:
                    sys.exit(f'{member} is not in the tarball')
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                with open(dest, 'wb') as f:
                    shutil.copyfileobj(src, f)
                print(f'  {dest} ({os.path.getsize(dest)} bytes)')
            extract('LICENCE.Intel', os.path.join(OUT, 'LICENCE.Intel'))
            for p in plats:
                extract(f'sof-ipc4/{p}/intel-signed/sof-{p}.ri',
                        os.path.join(OUT, 'sof-ipc4', p, f'sof-{p}.ri'))
    with open(os.path.join(OUT, 'VERSION'), 'w') as f:
        f.write(f'sof-bin v{SOF_VERSION}\n')


if __name__ == '__main__':
    main()
