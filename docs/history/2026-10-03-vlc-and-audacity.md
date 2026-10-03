## VLC and Audacity (19.6)

VLC 3.0.21 (the 32-bit PortableApps package) plays an H.264 and AAC MP4
with its Qt interface, video in its window and the sound through WASAPI,
and Audacity 3.7.4 (the 64-bit zip) records ten seconds from the
microphone, stops, and saves the project as an `.aup3` through its save
dialog.  The nightly app corpus runs both (`tests/appcorpus/870-vlc.py`,
`880-audacity.py`): `tools/appcorpus.py` boots with a microphone that hears
a 523 Hz tone and keeps what NovaOS played in `sound.wav` (`App(mic=True)`,
`App(sound=(hz, ms))`), as the core self-tests do, so VLC's 440 Hz tone is
checked after the run; without PulseAudio those two are skipped rather than
failed.  The ffmpeg test's clip is now thirty seconds of SMPTE colour bars with
the tone, which VLC loops; VLC offers the decoder its Direct3D formats
first and the display rejects each for want of a converter, which takes
seconds without KVM, so the screenshot waits for VLC's log to show the
software path settled.

Two message-loop gaps held VLC's video back.  Qt's Windows event
dispatcher drives its posted events from a `WH_GETMESSAGE` hook: the hook
resets the flag that lets another thread post the wake-up message, and
`user32` accepted the hook but never called it, so after the first wake-up
no cross-thread signal reached the Qt thread again and VLC's video thread
waited forever for the interface to hand it a window.  `GetMessage` and
`PeekMessage` now run the thread's `WH_GETMESSAGE` hooks on every message
they return.  The same hook decides with `GetQueueStatus(QS_INPUT |
QS_TIMER)` whether the queue still holds input; ours reported any pending
message under every flag, so it now reports what is actually queued
(posted messages, mouse, keys, paints, due timers, sent messages) masked by
the flags asked for.

wxWidgets' buffered painting blanked Audacity's toolbars once its shared
buffer had grown larger than the window: it blits the window's part of the
buffer with `StretchDIBits`, whose source y is measured from the bottom of
a bottom-up DIB, and `gdi32` read it from the top and drew the buffer's
empty bottom rows.  Behind that, every GDI call on a 24-bit DIB section
synchronised the 24-bit view with the pixels, which made Audacity's main
thread spend its time in `memcmp`; the view is now synchronised at the
points that read or write it (selecting the bitmap, blits, `GetPixel`,
`GetDIBits`, `SetDIBits`), and only the rows that changed.  `gdi32` holds
10,000 objects (Audacity's theme alone makes thousands of bitmaps) and
every DC starts with the 1x1 default bitmap, which `SelectObject` returns
and accepts back, as wxWidgets restores it.

Getting the two to start took more: the kernel's loader holds 1024 modules
(VLC loads every plugin, about 410), initialises them in dependency order
with an explicit stack, and the loader lock is a critical section the PEB
publishes as `LoaderLock` (Crashpad checks whether its thread owns it);
`WaitOnAddress` moved to `kernelbase.dll`, where VLC expects not to find it
in `kernel32`; with no network adapter lwIP still runs for 127.0.0.1 and
::1 (Audacity's plugin scanner talks to itself over loopback); `gdiplus`
exports every name wxWidgets' Direct2D-less renderer imports (brushes,
pens, paths, text, 606 in all); `comctl32`'s task dialog takes the
byte-packed `TASKDIALOGCONFIG`, its SysLink reports its ideal size and
strips quoted anchors; `kernel32` gained timer queues (`CreateTimerQueue`,
`CreateTimerQueueTimer`, `ChangeTimerQueueTimer`, `DeleteTimerQueueTimer`),
`user32` the DDE management library (no server answers, so no conversation
opens), `winspool` the printer enumeration (none), `advapi32` the
trustee and explicit-access builders, `wininet` the HTTP session calls
(unreachable, as `InternetOpen` gives no handle), and `msvcrt` about
seventy more calls.  A process killed from the terminal, or Ctrl+Alt+F12,
dumps every thread's user stack with module and offset, which is how the
stalls above were found.
