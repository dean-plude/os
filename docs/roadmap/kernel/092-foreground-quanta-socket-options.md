- ~~Scheduler and Winsock leftovers of the foreground boost: longer time
  slices for the foreground process, a Terminal's console program as the
  foreground process, and real `setsockopt`/`getsockopt`~~ Done
  (`prioritytest`: 60 ms slices in the foreground against 20 ms in the
  background; `looptest`: `TCP_NODELAY`, `SO_RCVTIMEO`, `SO_SNDTIMEO`,
  `SO_LINGER` and `SO_REUSEADDR` change what a socket does).
