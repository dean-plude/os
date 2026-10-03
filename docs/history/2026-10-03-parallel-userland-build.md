## A parallel userland build

`tools/build_userland.py` compiled and linked the 80 DLLs and 56
programs one after another (only a few third-party libraries inside a DLL's
`build.py` used several cores).  It now schedules every step as a task that
starts when the tasks it needs are done, on a pool of threads:

- **`--jobs N` / `-j N`.**  At most `N` compiler, linker and resource
  compiler processes run at once; the default is the CPU count and
  `--jobs 1` builds one step at a time.  `--check` and the other options
  behave as before, and CMake and CI keep calling the script without the
  flag, so they use every core.
- **Order that matters is kept.**  A DLL links after the DLLs in its
  `deps`; a program links after the DLLs it imports and after its own
  object.  Nothing else waits on anything, so the x64 and the x86 pass
  each keep all cores busy.
- **Same output.**  The files, their paths and their order in
  `userland_files.c` do not depend on `N`: what each task adds to the
  image is collected per task (`b.built` and `b.placed` in a DLL's
  `build.py` still work) and put in link order afterwards.
- **Readable failures.**  A failed command prints its command line and
  output in one piece, the build stops starting new work, and the script
  exits with status 1.
- **`userland/msvcp140/build.py`** builds `msvcprt_static.lib` under a
  lock, since several DLLs and programs link it.

Cold build of the userland without NetSurf on a 4-core machine: 6 min 16 s
before, 3 min 39 s after (CPU time unchanged, 9 min 41 s).
