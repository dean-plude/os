## Longer time slices in the foreground, console programs as the foreground process, and Winsock socket options

Three things the foreground boost (the history entry before) left out.

- **Quantum stretching** (`kernel/ke/scheduler.c`): the foreground
  process's threads now get a time slice three times as long as everyone
  else's, 60 ms (6 ticks) against 20 ms (2 ticks), as client Windows
  gives the foreground process 6 clock intervals against 2 ("Programs" in
  System Properties, PsPrioritySeparation 2).  A busy thread of the
  program the user works with is switched out a third as often by
  background programs of its priority.  The boost decay stays one level
  per 20 ms.
- **A console program in the Terminal is the foreground process** while
  that Terminal is active (`TerminalProgram`, `UmUpdateForeground`), as
  Windows treats a console's programs while their console window is in
  front: it gets the foreground boost and the longer slices.  Before, the
  active Terminal, a built-in app, made no process foreground at all.
  Other processes attached to the same console (a program's children)
  stay background, as the scheduler has one foreground process.
- **Winsock `setsockopt` and `getsockopt` are real** (`ws2_32`,
  `kernel/net/sock.c`, `NtNovaSockCtl` 9 and 10): they were a no-op that
  read back 0.  `TCP_NODELAY` switches Nagle's algorithm off in lwIP;
  `SO_RCVTIMEO` and `SO_SNDTIMEO` end a blocked `recv`, `recvfrom` or
  `send` with `WSAETIMEDOUT`; `SO_LINGER` with a zero timeout resets the
  connection on `closesocket` (`SO_DONTLINGER` too); `SO_REUSEADDR` lets
  two sockets that both set it share a port (lwIP's `SO_REUSE` is now on);
  `SO_KEEPALIVE` turns on lwIP's keep-alive probes (after 2 hours idle, as
  Windows); `SO_BROADCAST` and `IP_TTL`/`IPV6_UNICAST_HOPS` reach the
  PCB; `SO_RCVBUF`/`SO_SNDBUF` read back what was set (the receive ring
  stays 32 KB).  `SO_TYPE`, `SO_ERROR` (a failed connect or a reset) and
  `SO_ACCEPTCONN` answer.  An accepted socket takes its listener's
  options.  Options NovaOS does not model (`IPV6_V6ONLY`,
  `SO_EXCLUSIVEADDRUSE`, the AcceptEx context updates) are still accepted
  and ignored.
- **Measured** in QEMU (TCG, 2 CPUs): with twice as many busy NORMAL
  threads as CPUs, `prioritytest`'s run as the Terminal's program got
  slices of 59.6 ms at the median and a background copy's 19.7 ms; its
  boosts-off NORMAL waiter now waits out a 59 ms slice where it waited
  20 ms.  As the Terminal's console program, with a busy HIGHEST thread
  of a background process on every CPU, its woken NORMAL thread ran after
  0.17 ms at the 95th percentile and the background one's after 3.2 s.
  `looptest`: with Nagle's algorithm the second of two one-byte sends
  arrived after 199 ms (the delayed ACK), with `TCP_NODELAY` after 9.6 ms;
  `SO_RCVTIMEO` 300 ms ended `recv` after 299 ms.  `boosttest`, run from the Terminal, now expects the +2 foreground boost
  on top of each increment.  The core suite passes but for `soundtest
  volume`, which needs PulseAudio on the host; the network suite passes.
- **Not done**: `SO_LINGER` with a non-zero timeout closes as without it
  (a blocking `closesocket` does not wait for the data to be sent);
  `SO_RCVBUF` does not resize the receive ring or TCP window; sending a
  broadcast still works without `SO_BROADCAST` (lwIP's `IP_SOF_BROADCAST`
  stays off).
