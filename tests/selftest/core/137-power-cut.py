# A power cut in the middle of saving drive C:: the save had written new
# clusters and linked them in the FAT, but no directory entry points at
# them yet, and the volume's clean-shutdown bit (FAT[1]) is still clear.
# The test stops QEMU, makes that state on the data disk (three chains no
# file reaches), and resets the machine; at boot NovaOS must free exactly
# those clusters and end up with as much free space as a copy of the same
# disk has once fsck.fat has repaired it.
import os, shutil, struct, subprocess, time

EXPECT = []
ORPHANS = (40, 25, 1)                     # the lengths of the chains no file reaches


def fat_layout(img):
    """(FAT offset in bytes, FAT size in bytes, number of FATs, clusters + 2) of a FAT32 image"""
    with open(img, 'rb') as f:
        bs = f.read(512)
    bps, spc, reserved, nfats = struct.unpack_from('<HBHB', bs, 11)
    total = struct.unpack_from('<H', bs, 19)[0] or struct.unpack_from('<I', bs, 32)[0]
    fatsz = struct.unpack_from('<I', bs, 36)[0]
    clusters = (total - reserved - nfats * fatsz) // spc
    return reserved * bps, fatsz * bps, nfats, clusters + 2


def read_fat(img):
    off, size, _, n = fat_layout(img)
    with open(img, 'rb') as f:
        f.seek(off)
        return list(struct.unpack_from(f'<{n}I', f.read(size)))


def free_clusters(img):
    return sum(1 for x in read_fat(img)[2:] if not x & 0x0FFFFFFF)


def power_cut(nova):
    time.sleep(4)                          # (drive C: is saved once it has been quiet for a second)
    nova.qmp.cmd('stop')
    img = os.path.join(nova.work, 'data.img')
    off, size, nfats, n = fat_layout(img)
    fat = read_fat(img)
    free = [c for c in range(n - 1, 1, -1) if not fat[c] & 0x0FFFFFFF][:sum(ORPHANS)]
    free.sort()
    i = 0
    for length in ORPHANS:                 # chains, as a save that was cut off leaves them
        chain = free[i:i + length]
        i += length
        for a, b in zip(chain, chain[1:] + [None]):
            fat[a] = (fat[a] & 0xF0000000) | (b if b else 0x0FFFFFFF)
    fat[1] &= ~0x08000000                  # not closed cleanly
    data = struct.pack(f'<{n}I', *fat)
    with open(img, 'r+b') as f:
        for k in range(nfats):
            f.seek(off + k * size)
            f.write(data)
    copy = img + '.fsck'
    shutil.copy(img, copy)
    # fsck.fat -a keeps what it reclaims as \FSCKnnnn.REC files: deleting them frees it
    subprocess.run(['fsck.fat', '-a', '-w', copy], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(['mdel', '-i', copy, '::/FSCK*.REC'], env=dict(os.environ, MTOOLS_SKIP_CHECK='1'),
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    want = free_clusters(copy)
    os.unlink(copy)
    EXPECT.append(rf'Drive C: was not closed cleanly: reclaimed {sum(ORPHANS)} cluster\(s\)[^\n]*; {want} clusters free')
    nova.qmp.cmd('system_reset')
    nova.qmp.cmd('cont')
    nova.start()


DOC = ('a power cut in the middle of saving drive C: (the FAT holds chains no file reaches; at the next boot NovaOS '
       'frees them and has as much free space as `fsck.fat` finds)')
TESTS = [
    Test('power cut', 'dir C:\\', [r'bytes free'], builtin=True, before=power_cut, boot_expect=EXPECT, timeout=60),
]
