#!/usr/bin/env bash
# make-ntfs-disk.sh — a disk with an NTFS volume, for testing drive D:
#
# Usage: make-ntfs-disk.sh <out.img> [build-dir]
#
# Makes a 128 MiB disk (MBR, one NTFS partition from 1 MiB, labelled
# NOVATEST) holding what drivetest.exe checks; with a build directory it
# also gets Bin\hello.exe and Bin\testdll.dll from the build.  Attach it
# as a second SATA disk and run drivetest in the Terminal.  Needs mkntfs
# and ntfs-3g (FUSE), from the ntfs-3g package.
set -euo pipefail

OUT="${1:?usage: make-ntfs-disk.sh <out.img> [build-dir]}"
BUILD="${2:-}"
WORK="$(mktemp -d)"
trap 'fusermount -u "$WORK/mnt" 2>/dev/null || true; rm -rf "$WORK"' EXIT

truncate -s 127M "$WORK/vol.img"
mkntfs -F -Q -L NOVATEST -p 2048 -H 255 -S 63 "$WORK/vol.img" >/dev/null
mkdir "$WORK/mnt"
ntfs-3g -o compression "$WORK/vol.img" "$WORK/mnt"
python3 - "$WORK/mnt" "$BUILD" <<'PY'
import os, sys, shutil
root, build = sys.argv[1], sys.argv[2]
p = lambda *a: os.path.join(root, *a)
open(p('hello.txt'), 'wb').write(b'Hello from NTFS\r\n')
os.makedirs(p('Dir', 'Nested'))
open(p('Dir', 'Nested', 'data.bin'), 'wb').write(bytes((i * 7 + (i >> 9)) & 0xFF for i in range(300000)))
os.mkdir(p('Compressed'))
os.setxattr(p('Compressed'), 'system.ntfs_attrib_be', bytes.fromhex('00000810'))   # DIRECTORY | COMPRESSED
open(p('Compressed', 'words.txt'), 'wb').write(b'kernel ntfs ' * 50000)
os.makedirs(p('Unicode été'))
open(p('Unicode été', 'café.txt'), 'wb').write(b'accents\n')
os.mkdir(p('Many'))
for i in range(2000):
    open(p('Many', 'file%04d.txt' % i), 'wb').write(b'%d\n' % i * (i % 50))
os.mkdir(p('Bin'))
for f in ('hello.exe', 'testdll.dll'):
    src = os.path.join(build, 'kernel_build', 'userland', f) if build else ''
    if build and os.path.exists(src): shutil.copy(src, p('Bin', f))
PY
fusermount -u "$WORK/mnt"

# An MBR with one partition (type 07, NTFS) from LBA 2048
rm -f "$OUT"
truncate -s 128M "$OUT"
python3 - "$OUT" <<'PY'
import struct, sys
mbr = bytearray(512)
sectors = 127 * 2048
mbr[446:462] = struct.pack('<B3sB3sII', 0, b'\xfe\xff\xff', 0x07, b'\xfe\xff\xff', 2048, sectors)
mbr[510:512] = b'\x55\xaa'
with open(sys.argv[1], 'r+b') as f: f.write(mbr)
PY
dd if="$WORK/vol.img" of="$OUT" bs=1M seek=1 conv=notrunc,sparse status=none
echo "Created $OUT (NTFS volume NOVATEST)"
