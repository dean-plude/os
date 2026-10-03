## Crash reports in C:\NovaOS\Crashes (Phase 22.3)

A crash used to leave nothing behind but a line in the serial log, which
a PC without a serial port never shows.  Now every crash leaves a text
file a user can attach to an issue.

- **A program's crash** (`kernel/um/um_crash.c`): when an exception goes
  unhandled (`UmFaultAt`), the crashing thread puts together
  `C:\NovaOS\Crashes\NAME-YYYYMMDD-HHMMSS-PID.txt`: the program, PID, path,
  bitness and thread count, the time and NovaOS version, the exception and
  its code, where it happened (`module+offset`), the address an access
  violation touched, up to 32 return addresses into modules on the stack,
  every loaded module with its base, size and path, and the last 4 KiB of
  the kernel's log.  The desktop thread writes it a moment later
  (`UmCrashPoll`, under the file-system lock), and drive C: saves it to
  the disk as usual.  At most 100 reports are kept.
- **The Terminal** names the report under the crash line (`Crash report:
  C:\NovaOS\Crashes\crash-20261003-194012-14.txt`); `crashes` lists the
  reports and `crashes last` prints the newest.
- **A kernel crash** cannot write drive C: (it lives in memory and the
  save needs the drivers the crash may have broken).  At boot, NovaOS sets
  aside `\NOVA\PANIC.TXT` (64 KiB, contiguous) on the FAT volume that keeps
  C:; a kernel page fault or unhandled exception, after printing its
  backtrace, writes a report (the time, the version and the last 31 KiB
  of the kernel's log, ending with the backtrace) into those sectors with
  the disk driver alone: no FAT changes, no allocation (`PersistPanicWrite`).
  The next start moves it to `C:\NovaOS\Crashes\kernel-YYYYMMDD-HHMMSS.txt`
  and blanks the slot.  NTFS C: has no slot yet.
- **Tests**: `crashtest` (core suite, 64- and 32-bit) starts `crash.exe` and
  checks its report; the Terminal's `Crash report:` line and `crashes
  last` are checked after `crash`; `crash kernel` (900) now resets the
  machine afterwards (`Test(restart=True)` in `tools/selftest.py`), and
  905 checks that the next start reports the kernel crash with
  `KeCrashTestFault` in its backtrace.
