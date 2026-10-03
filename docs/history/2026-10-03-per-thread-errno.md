## errno per thread in the C runtime

`msvcrt.dll` and `ucrtbase.dll` kept one `errno` for the whole process,
so a multi-threaded program that checked `errno` after a failing call on
one thread could read a value another thread had just set.  Windows keeps
it, and the rest of the C runtime's per-thread data, in a block per
thread; NovaOS now does the same (`userland/msvcrt/ptd.c`).

- **What moved into the block.**  `errno` (`_errno`, `_get_errno`,
  `_set_errno`), `_doserrno` (`__doserrno`, `_get_doserrno`,
  `_set_doserrno`, now also set to the Win32 error behind each `errno`
  the runtime maps, as Windows' `_dosmaperr` does), `_fpecode`, `rand`'s
  seed, `strtok`'s and `_wcstok`'s position, and the buffers `gmtime`,
  `localtime`, `asctime`, `ctime`, `_wcserror`, `tmpnam(NULL)` and
  `_wtmpnam(NULL)` return.  A new thread starts with `errno` and
  `_doserrno` 0 and `rand` seeded with 1, as on Windows.
- **How.**  The block lives in a TLS slot (`TlsAlloc`), made on a thread's
  first use and freed by the DLL's new `DllMain` on `DLL_THREAD_DETACH`
  (and on `FreeLibrary`).  Looking it up keeps `GetLastError`
  unchanged, so code that reads `errno` and then `GetLastError` sees the
  failing call's error.  `msvcrt.dll` and `ucrtbase.dll` each keep their
  own block, as they are separate runtimes on Windows too.
  `tools/build_userland.py`'s `link_dll` takes an entry point for DLLs
  linked under another name (`ucrtbase`).
- **Who depends on it.**  Firefox's DLLs (`xul.dll`, `nss3.dll`,
  `mozglue.dll`, `mozavcodec.dll`, `onnxruntime.dll`...) and Microsoft's
  `msvcp140.dll` import `_errno` from the UCRT and run many threads.
- **Test.**  Core self-test `errnotest` (64- and 32-bit).
