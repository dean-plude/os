# fpstate: the x87 control word and MXCSR a thread starts with (0x27F,
# 0x1F80), kept across context switches, restored by RtlRestoreContext,
# NtContinue and an SEH unwind; inexact results never fault; an unmasked
# x87 divide by zero is EXCEPTION_FLT_DIVIDE_BY_ZERO (64- and 32-bit)
DOC = '`fpstate` (x87 control word and MXCSR on new threads, across switches, `RtlRestoreContext` and SEH unwinds, 64- and 32-bit)'
TESTS = [
    Test('fpstate x64', 'fpstate', [r'fpstate: \d+ passed, 0 failed']),
    Test('fpstate x86', r'C:\Programs\x86\fpstate.exe', [r'fpstate: \d+ passed, 0 failed']),
]
