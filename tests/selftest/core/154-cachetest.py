# cachetest: drive C:'s saved files have their contents let go of when
# memory runs short (NtSetSystemInformation's MemoryPurgeStandbyList asks
# for it here) and read back from the data disk when wanted: the memory
# comes back, the files read back as they were (also mapped, renamed,
# hard-linked or emptied while only on the disk), and after a restart C:
# is restored without reading the large files until they are wanted.
DOC = ('`cachetest` (saved files of drive C: let go of and read back from the data disk: memory, ReadFile, a mapped view, '
       'renamed, hard-linked and emptied files, `shutdown /r`, `cachetest after`)')
TESTS = [
    Test('let go of saved files', 'cachetest', [r'cachetest: \d+ passed, 0 failed'], timeout=300),
    Test('restart with saved files left', 'shutdown /r', [r'\[PERSIST\] Restored \d+ file\(s\) to drive C:; [1-9]\d* MB of them are read when wanted'],
         reboot=True),
    Test('read back after the restart', 'cachetest after', [r'cachetest: \d+ passed, 0 failed']),
]
