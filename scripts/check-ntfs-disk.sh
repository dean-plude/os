#!/usr/bin/env bash
# check-ntfs-disk.sh — after drivetest wrote to the disk make-ntfs-disk.sh made
#
# Usage: check-ntfs-disk.sh <disk.img>
#
# Runs ntfsfix -n and ntfssecaudit -a (ntfs-3g) and scripts/ntfs-check.py (a
# chkdsk-style check of bitmaps, records, indexes, link counts and $Secure)
# on the NTFS partition, then checks the files drivetest leaves in
# D:\WriteTest.  Needs ntfs-3g.
set -euo pipefail

IMG="${1:?usage: check-ntfs-disk.sh <disk.img>}"
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# The partition starts at LBA 2048 (make-ntfs-disk.sh)
dd if="$IMG" of="$WORK/vol.img" bs=1M skip=1 status=none
ntfsfix -n "$WORK/vol.img"
ntfssecaudit -a "$WORK/vol.img" | tail -1 | grep -q "No errors were found" || { echo "ntfssecaudit found errors"; exit 1; }
python3 "$HERE/ntfs-check.py" "$WORK/vol.img"
test "$(ntfscat "$WORK/vol.img" WriteTest/kept.txt)" = "$(printf 'Written by NovaOS\r')"
python3 - "$WORK/vol.img" <<'PY'
import subprocess, sys
data = subprocess.run(['ntfscat', sys.argv[1], 'WriteTest/kept.bin'], capture_output=True, check=True).stdout
want = bytes(((i * 7 + (i >> 9)) & 0xFF) ^ 0x5A for i in range(700000))
sys.exit(0 if data == want else 'kept.bin differs')
PY
echo "NTFS disk OK: no errors, and drivetest's files are there"
