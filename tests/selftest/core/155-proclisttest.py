# proclisttest: finding another running program by its name, owner and
# command line, as Microsoft Edge Update finds its running install worker
# before it uninstalls itself: EnumProcesses, the Tool Help snapshot,
# GetProcessImageFileName with QueryDosDevice, QueryFullProcessImageName,
# GetModuleFileNameEx and GetModuleBaseName of another process, its
# token's user, ProcessIdToSessionId, IsWow64Process(2), and its command
# line read from its PEB with ReadProcessMemory and through
# NtQueryInformationProcess(ProcessCommandLineInformation); the 64-bit run
# also looks up a 32-bit program.
DOC = '`proclisttest` (another program\'s image name, user and command line, as Edge Update looks for its install worker; 64- and 32-bit)'
TESTS = [
    Test('proclisttest x64', 'proclisttest', [r'proclisttest: \d+ passed, 0 failed'], timeout=120),
    Test('proclisttest x86', r'C:\Programs\x86\proclisttest.exe', [r'proclisttest: \d+ passed, 0 failed'], timeout=120),
]
