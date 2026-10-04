## The app corpus under KVM, round three

The App corpus run on round two's branch under KVM (nightly.yml run #52)
passed 14 of 20 programs.  The App Store installs that had timed out in
earlier runs finished in seconds: they were never slow, the Terminal had
simply lost the keyboard before them, which round two fixed.  What was
left were two NovaOS bugs, each failing several programs.

- **Floating-point exceptions came unmasked after a C++ `catch`.**
  Audacity stopped at start with `0xc000008f` (inexact result) in
  wxWidgets and Krita with `0xc0000090` (invalid operation) in Qt Quick,
  both ordinary operations that Windows never traps.  `RtlCaptureContext`
  set `CONTEXT_FLOATING_POINT` but filled in only `MxCsr`, not the
  `FltSave` area, and `NtRaiseException` and `NtContinue` reloaded the
  x87 control word and MXCSR from `FltSave`: whatever the stack held
  there, often zeros, which unmasks every exception.  `RtlCaptureContext`
  now saves the whole floating-point state there (`fxsave`), and the
  kernel takes MXCSR from `CONTEXT.MxCsr`, as Windows does.  `cppeh`
  checks both.
- **Firefox froze the whole desktop.**  As Firefox's processes started,
  the desktop thread stopped (the watchdog's backtrace: drawing the dock,
  asking the network for its status, waiting for the network lock) and
  never came back, so Firefox's, Notepad++'s and PuTTY's screenshots
  were the same frozen screen.  The network lock is a spinning lock whose
  waiters yield, and every program asking for random bytes
  (`NtNovaGetRandom`, which every Firefox process does as it starts)
  took it, because the entropy pool lived under it.  Waiters of higher
  priority handing the CPU to each other can starve a holder of lower
  priority behind them indefinitely.  The entropy pool now has its own
  short lock, so random bytes never wait for the network, and a waiter
  for the network lock sleeps briefly after a few yields, which lets
  the holder run.  `smpstress` gained a test of random bytes from
  high-, normal- and low-priority threads at once.

Still failing after this round: VLC does not exit after Alt+F4 (its own
thread; the corpus stops it and carries on), and Audacity's recording
shows dropouts under KVM.
