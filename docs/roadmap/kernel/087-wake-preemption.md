- ~~Scheduler: a thread woken by a timer preempts the running thread
  instead of waiting up to a 20 ms time slice~~ Done (`sleeptest timer`
  holds the timer queue case to 1 ms under load).
