# cabtest: cabinet.dll's File Decompression Interface, as installers call
# it (WiX Burn bundles such as the Visual C++ Redistributable extract their
# payloads with it): FDIIsCabinet, and FDICopy extracting an MSZIP and an
# LZX cabinet, skipping a file, reading a cabinet attached to another file,
# following a folder into the next cabinet of a set, and failing cleanly on
# a missing, foreign or damaged cabinet or when the caller aborts.
DOC = '`cabtest` (cabinet.dll: MSZIP and LZX cabinets, an attached container, a cabinet set; 64- and 32-bit)'

TESTS = [
    Test('cabinet x64', 'cabtest', [r'cabtest: \d+ passed, 0 failed']),
    Test('cabinet x86', r'C:\Programs\x86\cabtest.exe', [r'cabtest: \d+ passed, 0 failed']),
]
