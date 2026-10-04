## Sound threads above busy programs: MMCSS and the windowing boost

While Audacity recorded under QEMU without KVM, the thread handing back
`waveIn` buffers and PortAudio's own thread, both TIME_CRITICAL (15), were
held up 100 to 270 ms by Audacity's redraws.  Its window threads, woken by
mouse input, ran at 15 too: NovaOS gave a window's thread NT's +6 for
keyboard and mouse input and the foreground boost (+2) on top, 8 + 6 + 2
capped at 15, level with the sound threads, which then waited out time
slices behind them.  The `waveIn` change before this one kept those
delays from losing audio; this one stops the delays.

- **Input to a window is a +2 boost** (`kernel/um/um_gui.c`), win32k's
  windowing boost, as for any window message.  NT's +6 for keyboard and
  mouse is the I/O increment a driver gives the thread reading the
  device, which on NovaOS is the device poll thread, already at 19.  A
  foreground program's NORMAL window thread now wakes at 12 (8 + 2 + 2),
  below `THREAD_PRIORITY_HIGHEST` of an ABOVE_NORMAL class and below every
  TIME_CRITICAL thread.  The foreground boost still stacks on the wake
  boost, as NT's `KiDeferredReadyThread` adds PsPrioritySeparation to the
  base and increment (ReactOS's copy of it was checked); console input
  keeps +6.
- **A real Multimedia Class Scheduler** (`userland/avrt/avrt.c`,
  `sched_set_mmcss` in `kernel/ke/scheduler.c`).  `avrt.dll` used to set
  `THREAD_PRIORITY_HIGHEST`.  Now `AvSetMmThreadCharacteristics` with an
  audio task ("Pro Audio", "Audio", "Capture", "Playback", "Low Latency")
  runs the thread at real-time priority 18, with no privilege needed, as
  MMCSS's service lifts it on Windows: above any program thread however
  boosted and above the desktop (17), below device polling and the sound
  mixer (19).  Other tasks ("Games" and the rest) and audio tasks at
  `AVRT_PRIORITY_LOW` run at 16; a task Windows does not have fails with
  `ERROR_INVALID_TASK_NAME`; `AvRevertMmThreadCharacteristics` gives back
  the thread's own priority; the ANSI calls now read the task name (they
  passed an empty one).  As on Windows (SystemResponsiveness 20) a
  registered thread may use 80% of a CPU: found running at 8 of the 10
  ticks of a 100 ms period, it drops to its own base until the period
  ends, so a program spinning in one cannot freeze the desktop.
- **NovaOS's own sound threads register**: winmm's `waveOut` thread
  ("Playback"), `waveIn` thread ("Capture") and MIDI synthesizer,
  DirectSound's mixer and capture threads, and WASAPI's event thread
  ("Audio"), as Windows' audio engine runs under MMCSS.

`mmcsstest` (core self-test, x64 and x86): with a busy TIME_CRITICAL
thread on every CPU, a registered thread woken every 5 ms runs within
0.1-0.2 ms (95th percentile), a plain TIME_CRITICAL one after 24-34 ms
(median); "Pro Audio" threads spinning on every CPU for a second keep a
NORMAL thread waiting at most about 90 ms.
