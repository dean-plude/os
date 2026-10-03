## Long command lines in the Terminal and CreateProcess

A 163-character command typed into the Terminal ran as its first 158
characters, with nothing to say so.  Command lines now reach programs
whole, up to Windows' own limits.

- **Terminal.**  The line being typed holds up to 8,191 characters (the
  limit of Windows' `cmd.exe`; it used to be 158) and wraps onto as many
  rows as it needs at the window's width, the prompt's colour, the cursor
  and mouse selection following it across rows.  The same goes for a line
  typed to a program reading a line at a time (after its own prompt, or in
  a full-screen program's window), and for lines recalled with Up/Down,
  pasted, or passed by `TerminalRun`.  The command history keeps the whole
  lines, and the built-in `echo` prints any number of words as typed
  (quotes included, as `cmd.exe` does).  `start PROGRAM ARGS...` and lines
  handed to `cmd.exe /c` have no length limit of their own.
- **Console input.**  A line sent to a program in line mode is a key record
  per character; the console's queue held 1,024, so a longer line typed at
  `cmd.exe`'s prompt lost its end and its Enter.  It holds 16,384 (two
  lines of 8,191 characters).
- **CreateProcess.**  `NtNovaCreateProcess` read at most 8,191 bytes of
  the command line; it now takes 32,766 UTF-16 characters, the longest
  Windows takes (32,767 with the closing NUL), copied a page at a time,
  and refuses a longer one with `STATUS_NAME_TOO_LONG`, which
  `CreateProcess` reports as `ERROR_FILENAME_EXCED_RANGE` (206) as on
  Windows.  A command line too long for the process parameters' pages
  goes in a region of its own.  The parameters' strings (command line,
  image path, current folder) are converted from UTF-8 properly, so
  characters outside ASCII (accents, CJK, emoji as surrogate pairs) reach
  `GetCommandLineW` intact; they used to arrive one UTF-16 unit per byte.
- **Test harness.**  `tools/novarun.py` (which the self-tests and the app
  corpus use) typed a key every 30 ms holding each for 60 ms.  QEMU
  replays a key's hold as a delay in its input queue, so the keys piled up
  there and, some 600 characters in, it began dropping them, key releases
  and Enter included; a dropped release left a key held down, repeating.
  Keys are now held 30 ms and sent 50 ms apart, with a one-second pause
  every 40 keys so that a slow emulated desktop keeps up with long lines.
- **Tests.**  Core self-test `cmdlinetest`: a 1,200-character command typed
  into the Terminal and an 1,100-character one typed at `cmd.exe`'s prompt
  reach the program whole (length, `argv` with quoted paths and escaped
  quotes, last argument); `cmdlinetest spawn` passes 1,000, 8,191 and
  32,766 characters through `CreateProcessW`, 8,191 through
  `CreateProcessA` and 8,000 through `cmd.exe /c`, and checks that 32,767
  are refused (64- and 32-bit).
