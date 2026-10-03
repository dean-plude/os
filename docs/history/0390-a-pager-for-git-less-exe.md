## A pager for git: `less.exe`

`git log`, `diff` and `config --list` on the Terminal now page without
`--no-pager`.  MinGit has no `less`, git's default pager, so git stopped
with "unable to execute pager 'less'" (it does on Windows too).  NovaOS
now ships `less.exe` in `C:\Windows\System32` (and SysWOW64), on `PATH`,
where git finds it:

- It shows a screenful (the console's rows), then asks `-- More --` and
  takes single keys, through the per-key console input of Phase 17.2:
  Space (or `f`, Page Down) the next page, Enter (or `j`, Down) one more
  line, `d` half a page, `/text` and Enter skips to the next line
  containing the text, `q` (or Esc) quits, and git then stops quietly.
  The prompt is erased as the text moves on.  On a console that only
  hands over whole lines it asks for a line instead (Enter, a number,
  `/text` or `q`).
- Output that fits on one screen goes straight through, as with git's
  `LESS=FRX`, and so does everything when the output is not a console
  (`git log | find` is unchanged).  Color escapes pass through, files
  given as arguments (`less a.txt`) work, and options are accepted.
- Why not the real `less`: its Windows build draws through the console
  screen-buffer calls (`SetConsoleCursorPosition`, `FillConsoleOutput*`),
  which are still no-ops, so it would draw garbage; it can come once they
  drive the Terminal's screen.
- The nightly corpus's `git log` test no longer passes `--no-pager`.
- The test tools (`tools/selftest.py`, `appcorpus.py`) match a program's
  expected output without the kernel's `[UM]`/`[SCHED]` log lines, which
  share the serial port and could land mid-line (`[[UM] jq.exe ... 40,2]`).
