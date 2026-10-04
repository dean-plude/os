## WebView2: the runtime's browser process starts

With the WebView2 runtime installed, nothing had yet run it.  A host
program needs Microsoft's `WebView2Loader.dll` (from the WebView2 SDK,
BSD-licensed), which finds the runtime and starts its browser process,
`msedgewebview2.exe` (Chromium).  NovaOS now builds such a host,
`wv2host`, which makes an environment, a controller and a page through
the loader and says how far it got.  Each stop on the way was something
NovaOS lacked; nothing in the runtime is changed.

- **DLL search order**: the loader sits next to the host in its folder,
  which NovaOS never searched.  After the system folders, NovaOS now
  searches the process's current folder (unless `SetDllDirectory` set a
  folder), Windows' safe DLL search order.
- **COM's apartment in the TEB**: Chromium reads the thread's apartment
  from the TEB's `ReservedForOle` block (the flags of COM's per-thread
  data) instead of asking COM, and failed with `CO_E_NOTINITIALIZED`.
  ole32 now publishes the block: the single-threaded or multithreaded
  flag on the first `CoInitializeEx`, cleared by the last
  `CoUninitialize`.
- **Functions the browser calls**: ws2_32's `GetAddrInfoExW` (also
  asynchronous, with an `OVERLAPPED`, an event or a completion routine,
  and its cancel and result functions); ntdll's
  `RtlIpv4StringToAddress(Ex)` and `RtlIpv6StringToAddress(Ex)`, A and W,
  and `LdrLockLoaderLock`/`LdrUnlockLoaderLock`; crypt32's
  `CryptFindOIDInfo`; advapi32's performance counter provider API (a
  provider, its counter sets and instances, nothing reading them; the
  browser's delay-load hook turned the missing function into a
  breakpoint); powrprof's power setting notifications; kernel32's
  `GetDllDirectory`, `GetPhysicallyInstalledSystemMemory`, the packaged-app
  queries (no packages) and `AppPolicyGetThreadInitializationType`.
- **Fiber-local storage**: up to 4080 slots per process, as on Windows
  (each statically linked C runtime takes some), in blocks made as they
  are needed.
- **TerminateProcess on itself**: it ran DLL detach, as `ExitProcess`
  does; on Windows it does not, and Chromium's DllMain deliberately
  crashes on a detach it does not expect (its GPU process ended that
  way).  It now ends the process at once.
- **A `__finally` run twice**: unwinding to an `__except` whose
  `__try` sits inside a `__try`/`__finally` of the same frame ran that
  `__finally` too, though the frame goes on inside it: the unwind never
  told the frame's handler where it continues
  (`DISPATCHER_CONTEXT.TargetIp`), and NovaOS's own
  `__C_specific_handler` did not stop at the target.  Microsoft's C++
  runtime catches a rethrow in such an `__except` and raises it again
  from there; its `__finally` then unlinked the catch block's frame
  twice and `oneauth.dll` aborted the browser.
- **Tests**: `wvstarttest` checks the apartment flags, `TerminateProcess`
  on itself (a child whose FLS callback would leave a file), 300 FLS
  slots and the new functions; `unwindtest` checks the `__finally` runs
  once.  The WebView2 corpus test now also runs `wv2host` with the SDK's
  loader and expects the environment.
- **Where it stops now**: the environment is made and the browser starts
  its GPU, network and storage processes.  The GPU process finds no
  Direct3D 11 adapter for ANGLE and exits, the first browser process
  ends and the loader starts another, and `CreateCoreWebView2Controller`
  fails with `RPC_E_DISCONNECTED`
  ([compatibility.md](../compatibility.md#webview2)).
