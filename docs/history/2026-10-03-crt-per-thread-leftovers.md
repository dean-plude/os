## Fiber-local storage callbacks, per-thread locales and thread-safe getenv

The C runtime's per-thread block (`errno` and friends) left a few pieces
shared by the whole process.  They now behave as on Windows.

- **Fiber-local storage.**  `FlsAlloc` used to hand out TLS slots and
  store its callback without ever calling it, so a runtime that keeps its
  per-thread data in FLS (Microsoft's `vcruntime140.dll` and
  `ucrtbase.dll`, which programs ship next to themselves) leaked that data
  for every thread that ended.  FLS now lives in `ntdll`
  (`userland/ntdll/ntdll_fls.c`, with `RtlFlsAlloc`, `RtlFlsFree`,
  `RtlFlsGetValue`, `RtlFlsSetValue` and `RtlProcessFlsData`): 128 slots
  of its own, so FLS no longer uses up the 64 TLS slots, and a block of
  values per fiber reached from the TEB's `FlsData` field.  A slot's
  callback runs on each value still set when a thread ends (before the
  DLLs' `DLL_THREAD_DETACH`, as on Windows), on every live thread's value
  when the slot is freed, and on a fiber's values when `DeleteFiber`
  deletes it.  `SwitchToFiber` swaps `FlsData` with the fiber, so each
  fiber has its own values.
- **Per-thread locale.**  `_configthreadlocale(_ENABLE_PER_THREAD_LOCALE)`
  gives the calling thread its own copy of the locale names in
  `msvcrt.dll` and `ucrtbase.dll`; its `setlocale`/`_wsetlocale` calls
  change only that copy and other threads' calls no longer reach it,
  until `_DISABLE_PER_THREAD_LOCALE` puts it back on the process's
  locale.  It returns the previous setting and rejects unknown values
  with -1.  The text rules themselves are still those of the "C" locale.
- **getenv.**  `getenv` kept 16 rotating 512-byte buffers, rewritten in
  place, and `_wgetenv` one buffer freed on the next call, so a thread
  could read a value another thread was overwriting, or a freed one.
  Both now return the runtime's own copy per variable, kept until the
  value changes (a replaced copy is never freed, as another thread may
  hold it), with no length limit.  `GetEnvironmentVariableW` no longer
  returns an unfilled buffer when another thread lengthens the value
  between its two internal reads.  `tmpfile`'s name counter is atomic.
- **Still process-wide.**  The internal `mbstate_t` of `mbrtowc` and
  `wcrtomb` (static on Windows too).
- **Test.**  Core self-test `crtthreads` (64- and 32-bit).
