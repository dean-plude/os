## The app corpus under KVM, round two

The first App corpus run under KVM with the shared DLL pages (Actions run
#44) passed 11 of 19 programs.  Its screenshots showed that most of the
failures were one failure seen many times: every screenshot from VLC on
was the same picture, VLC's "Errors" box saying `C:\Apps\in.mp4` could
not be opened, with the Terminal behind it never having received another
command.

- **ffmpeg's MP4 was never made.**  The Terminal took only the first 158
  characters of a typed line, so the command ended at `C:\Apps\i` and
  ffmpeg stopped with "Unable to choose an output format", exit code -22
  (EINVAL).  The VLC change (#126) shortened the command the same way,
  and the Terminal's own line length is raised separately.
- **One stuck program failed every program after it.**  With no MP4,
  VLC (`--loop`) kept its error box up; Alt+F4 closed the box, the next
  loop opened it again, and the corpus typed `taskkill` and `echo ready`
  into VLC instead of the Terminal.  Audacity, Inkscape, Firefox,
  Notepad++ and PuTTY then never started (Firefox's "no outcome in
  1500 s" is the App Store command that never ran).
  `tools/appcorpus.py` now checks that the Terminal answers before it
  stops what a program left running; when it does not, it opens a second
  Terminal from Start (the Windows key reaches Start whichever window has
  the keyboard), stops the program from there and closes that Terminal
  again so the next screenshots show the usual desktop.  The program that
  kept the keyboard still fails.  A program that keeps opening new
  windows (VLC's error box on every loop) can still take the second
  Terminal's keys.
- **fd panicked: "keyed events not available".**  Rust's standard
  library parks a thread with `WaitOnAddress`, which it finds with
  `GetModuleHandle("api-ms-win-core-synch-l1-2-0")`.  NovaOS maps that API
  set to `kernelbase.dll`, but kernelbase was loaded only into programs
  importing from it, so fd got no module and fell back to keyed events
  (`NtCreateKeyedEvent`), which NovaOS lacks.  It only parks when its
  threads contend, which they do on four real CPUs.  As on Windows,
  `kernelbase.dll` is now in every process that has `kernel32.dll`
  (`kernel/um/um.c`).  `crtthreads` checks the API set lookup, 64- and
  32-bit.
- **Notepad++ crashed on start (main since #122).**  Found while checking
  the recovery: Notepad++ turns a structured exception into a C++
  exception with `_set_se_translator`, which throws from inside its frame
  handler while the first exception is being dispatched.  Since #122,
  `RtlUnwindEx` starts at the new exception's own frame, and the walk up
  from there went through the dispatcher's stack and never reached the
  catching frame, so it resumed with a wrong stack and the next `free`
  faulted (`ntdll.dll+0x1e9f`).  Past the frames dispatching an outer
  exception, the walk now carries on where that exception happened (as
  `KiUserExceptionDispatcher`'s unwind data leads on Windows), and from
  inside a catch block it goes straight to the catch's own frame (as
  Windows' consolidation frame does).  Notepad++ opens its file again and
  matches its reference (0.0%); the exception self-tests (`cppeh`,
  `unwindtest`, `stltest`, `rttest`, `qttest`) pass.

Under TCG: fd, ffmpeg (all four steps), Notepad++ and PuTTY pass.  The
first KVM run with these changes passed 16 of 20 programs (it was 11 of
19): fd, ffmpeg, Inkscape, Krita and Notepad++ now pass.  Still failing
there: VLC hangs as it exits (its windows close, threads in
`libmmdevice_plugin.dll` and the Qt plugin keep waiting; a separate
change), Audacity's recording shows two dropouts (7.8%), and Firefox
(5.5%) and PuTTY (6.7%) had the second Terminal in their screenshots,
which it now closes.
