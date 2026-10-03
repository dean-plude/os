## 1088 TLS indexes and FLS callbacks at process exit

The two pieces the C runtime work left open now behave as on Windows.

- **TLS expansion slots.**  `TlsAlloc` stopped at the TEB's 64 slots, few
  enough that a program loading many DLLs (Firefox's) could run out.  It
  now hands out 64 + 1024 indexes, from `ntdll`
  (`userland/ntdll/ntdll_tls.c`: `RtlTlsAlloc`, `RtlTlsFree`,
  `RtlTlsSetValue`).  Indexes 64-1087 live in a per-thread array the TEB's
  `TlsExpansionSlots` field points at, allocated the first time the
  thread sets one of them and freed when the thread ends, after the DLLs'
  `DLL_THREAD_DETACH`.  `TlsFree` makes the index read zero again in
  every thread of the process, not only the caller: the kernel clears it
  in each thread's TEB or array (`NtSetInformationThread` with
  `ThreadZeroTlsCell`, as on Windows).  The 32-bit `TlsGetValue` and
  `TlsSetValue` also stopped addressing the TEB's slots 8 bytes apart.
- **FLS callbacks at process exit.**  `ExitProcess`, and returning from
  `main`, now run the exiting thread's `FlsAlloc` callbacks before any
  `DLL_PROCESS_DETACH`, as Windows' `LdrShutdownProcess` does; the values
  of threads still running reach their callbacks when a DLL frees its
  index during the detach, and none runs twice.
  `RtlDllShutdownInProgress` now says TRUE while the process detaches.
- **Test.**  Core self-test `tlsslots` (64- and 32-bit).
