- ~~Move files, the registry, process creation and the console off the
  big kernel lock~~ Done (Phase 17.7): file and registry throughput scale
  about 3x from one CPU to four (`smpstress scaling 3`, run nightly).
