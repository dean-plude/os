# etwtest: advapi32's event-tracing controller and consumer functions
# answer as Windows does with no logging session running and none able to
# start (StartTrace finds the session limit reached, StopTrace and
# friends find no such session, a real-time consumer has nothing to
# read); Steam's service stops and starts a session of its own and falls
# back when the start fails.
DOC = '`etwtest` (event-tracing controllers with no sessions: StartTrace, StopTrace, ControlTrace, EnableTrace, QueryAllTraces, OpenTrace, ProcessTrace, CloseTrace; 64- and 32-bit)'

TESTS = [
    Test('event tracing x64', 'etwtest', [r'etwtest: \d+ passed, 0 failed']),
    Test('event tracing x86', r'C:\Programs\x86\etwtest.exe', [r'etwtest: \d+ passed, 0 failed']),
]
