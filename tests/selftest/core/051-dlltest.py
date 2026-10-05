# dlltest (tools/selftest.py's core suite): DllMain, exported calls,
# static TLS, LoadLibrary, and a process whose imported DLL's DllMain
# returns FALSE ending with STATUS_DLL_INIT_FAILED (0xC0000142) before its
# entry point, and GetModuleFileName for every module with more than 64
# loaded, 64- and 32-bit
DOC = '`dlltest` (DllMain, static TLS, and a DllMain returning FALSE stopping the program with 0xC0000142, module file names past the 64th module, 64- and 32-bit)'
TESTS = [
    Test('dlltest x64', 'dlltest', [r'DLL/TLS self-test: \d+ passed, 0 failed']),
    Test('dlltest x86', r'C:\Programs\x86\dlltest.exe', [r'DLL/TLS self-test: \d+ passed, 0 failed']),
]
