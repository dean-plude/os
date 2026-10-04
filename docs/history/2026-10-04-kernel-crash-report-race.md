## A kernel crash report typed for too early

The self-test `kernel crash report` failed on a slow (TCG) CI runner: after
`crash kernel` and the reset, `crashes last` printed the older report of a
program that had crashed before the reset.  At boot the kernel's report is
only queued; the desktop loop writes it to `C:\NovaOS\Crashes` in `UmPoll`,
after it has handled the input waiting.  A command typed in the first
iteration of that loop ran before the report existed.  `crashes last` now
writes the queued reports first (`UmCrashNewest` does it under the
file-system lock the Terminal already holds).  Found by the MSXML 6 thread
on PR #170, which proposed calling `UmCrashPoll`; that takes the lock the
Terminal holds, so the write is split out instead.
