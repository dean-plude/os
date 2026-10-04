## A finished program's files are closed before its parent wakes

Main's CI went red on `tlsslots x86`: its parent deleted the log file a
child had just written and started the next child, whose create failed
with `STATUS_DELETE_PENDING` (exit code 2).  Since delete pending
(WebView2 process list), deleting a file another handle still holds waits
for that handle to close.  The kernel signaled a process as ended when its
last thread exited, but closed its handles only later, on a scheduler
tick, so the parent could win the race.

Windows closes a process's handle table before it signals the process, so
the last thread to exit now closes the process's file and directory
handles first (`um_close_file_handles`).  `tlsslots` repeats its two child
runs three times so the race shows up on every run.
