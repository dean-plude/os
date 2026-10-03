- ~~Scheduler: a thread woken by an event or a timer preempts the running
  thread instead of waiting up to a 20 ms time slice~~ Done (`sleeptest
  timer` holds the wake-up to 1 ms under load).
