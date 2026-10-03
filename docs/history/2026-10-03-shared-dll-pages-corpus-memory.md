## Shared DLL pages, and the app corpus under KVM

The first App corpus runs on KVM (PR #98) failed three programs: Firefox
crashed about a minute after it started, at a different place each run,
and Notepad++'s and PuTTY's screenshots differed from their references.
None of it needed KVM.  The same failures came back under TCG once the
guest's memory was as full as in the whole corpus.

- **Firefox ran out of memory.**  Drive C: lives in RAM, so the corpus's
  programs (Inkscape, .NET, Python, ffmpeg, KeePassXC and the rest) take
  much of the 4 GB before Firefox starts, and every Firefox process got
  its own copy of every DLL it loaded: 164 MB of `xul.dll` alone for each
  of about ten processes.  Thread creation failed with
  `STATUS_NO_MEMORY`, then `xul.dll` would not load in a new content
  process.  With 1.5 GB of extra files staged, the crash reproduced under
  TCG every time.  KVM only made Firefox start its processes sooner.
- **Shared image pages** (`kernel/um/um.c`).  A DLL's read-only pages
  (headers, code, read-only data) now map frames shared by every process
  whose page holds the same bytes, as Windows shares image sections.  A
  table keyed by the page's contents counts each frame's users, so a DLL
  relocated or bound differently in one process still shares every page
  that matches.  `PTE_IMAGE` (a bit the MMU ignores) marks them.  They are
  never writable: `VirtualProtect` to a writable protection, or a write
  from another process (`WriteProcessMemory`, as Firefox's launcher and
  sandbox do), gives that process its own copy first.  Writable sections
  are still private.  With the same 1.5 GB staged, Firefox loads its page.
- **AppContainer monikers** (`kernel32`, which is also `kernelbase`):
  `AppContainerRegisterSid`, `AppContainerUnregisterSid`,
  `AppContainerLookupMoniker` and `AppContainerFreeMemory`.  The Chromium
  sandbox inside Firefox looks them up in `kernelbase.dll` and stops the
  browser (a `CHECK`, breakpoint in `firefox.exe`) when one is missing.
  It only goes there for some utility processes, which start sooner when
  the machine is fast.  NovaOS runs no AppContainers; a registration is
  kept for the process so a lookup finds it.
- **Notepad++ and PuTTY** failed because of Firefox.  The corpus typed
  Notepad++'s command while Firefox's processes were still shutting down
  and had the keyboard, so the command was lost and Notepad++'s Alt+F4
  closed the Terminal instead.  `tools/appcorpus.py` now waits for every
  process a windowed program started to end (stopping any left after two
  minutes with `taskkill`) and checks that the Terminal answers before
  the next program.  The App Store window Firefox's install opened stayed
  behind PuTTY (5.5% of its screenshot); `store close`, a new Terminal
  command, closes it after Firefox.  Alone, Notepad++ and PuTTY matched
  their references all along (0.0% and 0.1%).
- **`[PMM] out of memory`** is logged with the request and the free
  memory, on the first failed page allocation and every 4096th after it,
  so a program that stops with an unrelated-looking crash shows why.

Under TCG, Firefox, Notepad++ and PuTTY now pass one after another.  The
KVM runs of PR #98 check the rest.
