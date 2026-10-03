## Qt programs: Krita

The last program of Phase 20.3: Krita 5.3.4, the portable zip from
download.kde.org, unmodified.  Unlike KeePassXC's Qt (built with Visual
Studio), Krita and its Qt 5, KDE Frameworks and about 90 other DLLs are
built with LLVM's MinGW toolchain: C++ through `libc++.dll`, exceptions
through `libunwind.dll` on Windows' structured exception handling.  It
did not get past loading its first DLLs; it now starts, and "New Image"
on its welcome page opens an empty A4 image on its OpenGL canvas (Mesa
3D's llvmpipe, from the App Store) with the toolbox, colour selector,
layers and brush presets.  What was missing:

- **The UCRT's `_l` functions.**  libc++ builds every `std::locale` facet
  on the locale-taking C functions, and the loader stopped Krita at the
  first one (`_mbtowc_l`).  `ucrtbase.dll` has them now for its one "C"
  locale: `_isctype` and `_isctype_l`, `_tolower_l`/`_toupper_l`,
  `_towlower_l`/`_towupper_l`, the `_isw*_l` classes, `_strxfrm_l`,
  `_wcscoll_l`, `_wcsxfrm_l`, `_mbtowc_l`, `_strtod_l`, `_strtol_l`,
  `_strtoi64_l`, `_strtoui64_l`, `wcrtomb_s` and `_swab` (LibRaw).
- **`RtlUnwindEx` as Windows does it.**  The CONTEXT a caller passes is
  only storage: Windows starts the unwind where the caller is.  NovaOS
  started from whatever the CONTEXT held, which worked for Visual Studio
  programs (they pass the exception's own CONTEXT) but not for libunwind
  or GCC's libgcc: at the frame that catches, their handler starts a
  second, *collided* unwind to the landing pad with a CONTEXT it never
  filled in, and `_Unwind_Resume` (the end of every cleanup) does the
  same from no handler at all.  The first exception any Krita library
  threw and caught (OpenColorIO's, while reading its configuration)
  landed with the trap flag set from that garbage and ended the program.
  ntdll now keeps, per thread, what a handler is running inside: a
  dispatch (an unwind then starts at the exception's frame, as before),
  an unwind's frame handler (a collided unwind starts again at that
  frame, whose handler then sees the new record) or neither (it starts
  at the caller of `RtlUnwindEx`).
- **Tests**: `unwindtest` (libunwind's throw, catch and collided unwind
  with a CONTEXT full of garbage, a cleanup frame in between, and
  `_Unwind_Resume`'s unwind from no handler) in the core self-tests, and
  Krita in the nightly corpus (`tests/appcorpus/885-krita.py`): the App
  Store installs Mesa 3D, Krita starts, "New Image" and Create open an
  image, and the window must match `tests/reference/krita.png`.  A
  corpus program can now name App Store runtimes it needs
  (`App(runtimes=[...])`), staged for the Store to install without a
  network.  The App Store lists Krita 5.3.4.

Not yet: Krita needs an `opengl32.dll`, and NovaOS has none until Mesa 3D
is installed (Windows always has its own OpenGL 1.1, which would send
Krita to its software canvas; without any, Krita ends); the Python
scripter plugin fails to import `asyncio` (`WSAEOPNOTSUPP` from a
socket call); Qt Quick's JIT cannot register its unwind tables; Ctrl+N
right after start does not reach Krita's window (a click does); and
painting strokes on the canvas is not tested yet.
