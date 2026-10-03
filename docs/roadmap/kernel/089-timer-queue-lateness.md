- ~~Scheduler: timer queue timers fire on time with every CPU busy (a
  deadline the timer had not fired yet was re-armed over, a timer wake
  behind a kernel thread went last, a thread preempted at a tick lost its
  place)~~ Done (`sleeptest timer`: the 1 ms timer queue timer within 1 ms
  under load again).
