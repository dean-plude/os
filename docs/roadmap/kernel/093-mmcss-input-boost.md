- ~~Sound threads above busy programs: the Multimedia Class Scheduler
  (`AvSetMmThreadCharacteristics`) and NT's windowing boost for input~~
  Done (`mmcsstest`: a registered thread woken every 5 ms runs within
  2 ms while TIME_CRITICAL threads spin on every CPU, where a plain
  TIME_CRITICAL one waits 24-34 ms; busy registered threads still leave a
  NORMAL thread room).
