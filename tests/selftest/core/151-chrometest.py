# chrometest: what Chromium (Steam's browser, CEF 126) needs before it starts
# its GPU, network and renderer processes.  A section duplicated with more
# rights than its handle holds is checked against the section's security
# descriptor, so a read-only handle cannot be widened to FILE_MAP_WRITE
# (base::ReadOnlySharedMemoryRegion); NtQuerySection without SEC_IMAGE;
# STARTUPINFOEX with PROC_THREAD_ATTRIBUTE_HANDLE_LIST passes only the
# listed handles to the child; the ordinal exports Chromium imports by
# number (shlwapi IsOS, QISearch, oleaut32, uxtheme), SHChangeNotifyRegister
# and the api-ms-win-power API set.
DOC = '`chrometest` (Chromium start-up: read-only shared memory, handle lists, ordinal exports; 64- and 32-bit)'

TESTS = [
    Test('chromium start-up x64', 'chrometest', [r'chrometest: \d+ passed, 0 failed']),
    Test('chromium start-up x86', r'C:\Programs\x86\chrometest.exe', [r'chrometest: \d+ passed, 0 failed']),
]
