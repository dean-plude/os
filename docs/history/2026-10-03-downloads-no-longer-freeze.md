## Long downloads no longer freeze the machine

Roblox's installer fetches about 220 MB of packages over one HTTP/2
connection through a proxy.  Under TCG it stopped part way in two runs of
five, which looked like a network stall: the transfer went to zero and
never came back.  It was not the network.  The connection's state at the
stop (from a debugging dump of every TCP connection: windows, unread
bytes, queued segments) was healthy, with the installer simply no longer
reading; the whole machine had stopped, desktop and network thread too,
with both processors spinning in the kernel.

- **The cause: lock waiters that only yield.**  The kernel's locks for
  program threads (`um_lock`, and the wait for a read-write lock's
  readers to leave) spin briefly and then call `sched_yield` until the
  lock is free.  A yield hands the processor only to a thread of the same
  or higher priority.  When the waiter had been lifted above the holder by
  a wake-up boost and both were queued on one processor, the waiter took
  the processor back at every yield and the holder never ran to let go.
  In one frozen run an installer thread spun for the desktop lock
  (`NtCreateSection` takes it) while the desktop thread, which held it,
  waited for the network lock to draw the dock; in the other the
  installer held the desktop lock and spun for the file-system lock's
  readers to leave.  QEMU's monitor showed the two processors handing the
  big kernel lock back and forth between the spinning threads.
- **The fix.**  After eight yields a waiter now sleeps for 0.1 ms
  (`um_give_way` in `kernel/um/um.c`): off the run queue, it lets the
  holder run.  The network lock already did this (`net_lock`, after
  Firefox's start froze the desktop the same way).  With the fix the
  installer completed in seven runs of seven, in 133-162 s.
- **Faster socket reads.**  A socket's receive ring is now copied with
  `memcpy` in at most two pieces instead of a byte at a time
  (`kernel/net/sock.c`).
- **A new self-test, `dltest -w`,** in the network suite on virtio-net
  and e1000e: eight 32 MB downloads at once from `tools/h2server.js`'s
  new `/stream` address, every byte checked and written to a file while
  two threads map files over and over, as an installer does.  It fails
  if the transfer stops for 5 s (about 11 MB/s under TCG).
