## VLC closes: windows of an ended thread answer nothing

VLC stopped responding as it closed (Alt+F4 in the App corpus): its last
log line was "releasing video...", and every program after it used to
fail until the corpus learnt to stop it from a second Terminal.  A dump of
VLC's threads (Ctrl+Alt+F12) showed four waits chained together: the main
thread joining the playlist, the playlist waiting for the video output to
close, the video output waiting for Qt's window thread to release the
video widget (a blocking queued signal), and Qt's window thread inside
`DestroyWindow`, waiting for an answer to `WM_DESTROY` from a window of a
thread that had already ended.

The video widget's native window had a child window made by VLC's video
event thread.  Qt destroyed the widget; `DestroyWindow` sent `WM_DESTROY`
to that child, VLC's event thread answered it by leaving its message loop
and ended, and `DestroyWindow` then sent `WM_DESTROY` to the child's own
child (also the event thread's).  `user32` sent messages to another
thread's window by queuing them for that thread and waiting until it
answered, and a thread that has ended never answers.  On Windows a
thread's windows go with it, so the message gets no answer and the sender
carries on.

`SendMessage` (and every message `user32` sends itself, such as the
`WM_DESTROY` and `WM_NCDESTROY` of `DestroyWindow`) now also waits on the
receiving thread: when that thread has ended, or ends without taking the
message, the message is taken back and the call returns 0.  VLC now closes
within a second of Alt+F4 and the corpus programs after it start normally.
The new `wndthreads` self-test (64- and 32-bit) destroys a window whose
child belongs to a thread that ends on the child's `WM_DESTROY`, and one
whose child's thread had already ended, and sends a message to the window
of an ended thread; a watchdog fails the test rather than leave the
Terminal waiting.
