## The userland build runs its 64-bit, 32-bit and NetSurf parts side by side

The parallel userland build left three parts of `tools/build_userland.py`
one after another: the x64 pass, the x86 pass and NetSurf's build. They now
share the one `--jobs` budget as a single set of dependency-ordered tasks.
NetSurf's roughly 800 objects compile from the start (they need only
headers); `netsurf.exe` links once the x64 start-up objects and the import
libraries of `msvcrt`, `kernel32`, `ntdll`, `ws2_32`, `user32` and `gdi32`
exist. Of the tasks that are ready, the passes' go before NetSurf's objects,
so the 64-bit DLL chain is never queued behind them.

- **The architecture is per task.** Build hooks read it as `b.ARCH`; that is
  now a property of the running task's thread, so the two passes can
  not see each other's value. The files built, their order in the image and
  the `--check` result are unchanged.
- **`msiscript_js.h`** (written by both passes) is replaced atomically.
- **Failures** name their pass (`[x64]`, `[x86]`, `[netsurf]`); NetSurf's
  compile errors are listed in file order.
- `tools/build_netsurf.py` split `build()` into `plan()`, `compile_one()` and
  `link()`; run on its own it behaves as before.
