# savetest: saving drive C: holds no lock while the disk is written.  The
# program writes 32 MiB to C:\Temp (the core boot's data disk has 64 MiB) and times calls behind the big kernel
# lock, the desktop lock and the file-system lock while NovaOS saves it;
# the kernel's "[PERSIST] Saved" line for that save must show the
# file-system lock held under 20 ms (the regex takes 0.00 to 19.99 ms).
DOC = ('`savetest` (while NovaOS saves 32 MiB of drive C: to its disk, calls behind the kernel, desktop and '
       'file-system locks keep answering; the save holds the file-system lock under 20 ms)')
TESTS = [
    Test('savetest', 'savetest', [r'savetest: PASS',
                                  r'\[PERSIST\] Saved \d+ file\(s\), \d{5,} KiB in \d+ ms; '
                                  r'the file-system lock was held 1?\d\.\d\d ms']),
]
