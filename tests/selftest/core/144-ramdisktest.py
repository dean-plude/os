# ramdisktest: files on drive C: (kept in memory) take the memory their
# contents need: a file written by appending gives back the rest of the
# buffer it grew into once it is closed, SetEndOfFile takes the length asked
# for (not the next power of two), contents survive files growing in turn,
# shrinking and growing again, and deleted files give their memory back.
# The app corpus ran out of memory with Krita and Firefox after the Roblox
# install, whose files had kept their doubled buffers.  Programs are also
# told the machine's real memory (GlobalMemoryStatusEx and
# NtQuerySystemInformation said 512 MB total, 256 MB free).
DOC = '`ramdisktest` (files on drive C: take the memory their contents need: appended files, SetEndOfFile, deleting; the memory programs are told)'
TESTS = [
    Test('ramdisktest', 'ramdisktest', [r'ramdisktest: \d+ passed, 0 failed'], timeout=300),
]
