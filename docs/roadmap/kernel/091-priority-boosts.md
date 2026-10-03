- ~~Scheduler: NT's priority boosts (a woken thread runs above its base
  priority by the waker's increment and decays back one level per
  quantum; the balance set lifts starving threads)~~ Done (`boosttest`:
  an event-woken thread runs within 2 ms while same-priority threads
  spin, where it waited out a 20 ms slice).
