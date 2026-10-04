## Self-tests log where a hung program sat

The graphics job's `d3dtest x86` once hung for its whole 900 s (run
37148175085 on PR #145, a change that touches no graphics code; 2 of about
200 graphics runs since the Venus job began have hung that way, the other
on the virgl branch before its fix; both in the 32-bit program, and in
this one no DXVK thread had started after the queue-family lines, so it
sat in or just before Venus's `vkCreateDevice`).  The log said only "did
not finish": the harness's Ctrl+C ended the program with exit code 3 and
the kernel logged no thread dump, so where the main thread sat is unknown.

- **The harness** (`tools/novarun.py`, `Nova.run`): a program that has not
  finished in its time now first gets Ctrl+Alt+F12, which makes the kernel
  log every program's threads (system call, user stack, what each waits
  on), and then Ctrl+C as before.  The dump lands in the failure's output,
  so the next hang names its own cause.  Nothing else about the wait
  changed: the time limit is the same and a program that finishes in time
  is never touched.
