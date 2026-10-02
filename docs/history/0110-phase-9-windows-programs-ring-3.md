## Phase 9 — Windows programs (ring 3)
- **Real PE32+ `.exe` files run in ring 3** (`kernel/um/`): each program has
  its own page table (kernel half shared), a PEB/TEB and
  `RTL_USER_PROCESS_PARAMETERS` (UTF-16 command line, environment, current
  directory, standard handles).  The loader maps the image and its DLLs from
  drive C: with relocations, imports, forwarders and per-section protections
  (NX for data); ntdll then runs each module's TLS callbacks and `DllMain`
  in dependency order, so real DLL initialization happens.
- **Threads and synchronization**: `NtCreateThreadEx` and a per-process
  thread table (each thread with its own stack, TEB, FPU/SSE state and GS
  base); kernel objects reached through handles — events (auto/manual
  reset), mutexes (recursive, abandoned on owner exit), semaphores and
  thread objects — with `WaitForSingleObject`/`WaitForMultipleObjects`.
  ntdll critical sections, SRW locks, condition variables, one-time init;
  kernel32 `CreateThread`, TLS/FLS slots and the `Interlocked*` intrinsics.
- **Static TLS**: `__declspec(thread)` data, `_tls_index`/`_tls_used`, per
  thread, with `TlsAlloc`/`TlsGetValue` slots.
- **Structured exceptions (SEH)**: the kernel delivers CPU faults to ntdll's
  `KiUserExceptionDispatcher`; ntdll implements the x64 unwinder
  (`RtlLookupFunctionEntry`, `RtlVirtualUnwind`, `RtlUnwindEx`) and
  `__C_specific_handler`, so `__try`/`__except`/`__finally`, vectored
  handlers and `RaiseException` work (built with `-fasync-exceptions`, so
  hardware faults are caught).  An unhandled fault ends the process with a
  precise report; the rest of the system keeps running.
- **System DLLs built from source** (`userland/`, compiled with
  `clang --target=x86_64-pc-windows-msvc` + `lld-link` by
  `tools/build_userland.py`) and installed in `C:\Windows\System32`:
  - `ntdll.dll` — NT system-call stubs, the loader, `Rtl*` helpers, a heap;
  - `kernel32.dll` — files, directories, console, memory, environment, time,
    processes/threads, sync, dynamic loading; A (UTF-8) and W entry points;
  - `msvcrt.dll` — a C runtime (`stdio`, `stdlib`, `string`, `math`, ...);
  - `ws2_32.dll` — **Winsock 2**: `socket`/`connect`/`bind`/`listen`/
    `accept`/`send`/`recv`/`sendto`/`recvfrom`/`select`/`gethostbyname`/
    `getaddrinfo`, over lwIP (`kernel/net/sock.c`);
  - `user32.dll` + `gdi32.dll` — a **Win32 GUI**: window classes,
    `CreateWindowEx`, the `GetMessage`/`DispatchMessage` loop,
    `DefWindowProc`, painting (`BeginPaint`, `FillRect`, `TextOut`,
    `Rectangle`, `Ellipse`, pens/brushes), timers and mouse/keyboard input.
    Each program window is a real window in the desktop's window manager,
    drawn from a client bitmap the program owns (`kernel/um/um_gui.c`).
- **NT services** (`um/um_syscall.c` and friends): files, directories, file
  and volume information, virtual memory (256 MB per process), time, delays,
  thread/process/synchronization objects, sockets and windows.  Programs may
  reach only the implemented services and every user pointer is checked.
- **Terminal**: type a program's name — console programs (`hello`, `mandel`,
  `primes 2000000`, `wc ...`, `guess`) stream output and take input, with
  **Ctrl+C** and **Ctrl+Z**; GUI programs (`winhello`) open their own
  window; `netcat host path` fetches a URL over sockets.  `tasklist` and
  `taskkill /PID n` list and stop programs; several run at once.
- Sample and self-test programs live in `userland/programs/`
  (`crttest` 26/26, `filetest` 30/30, `sectest` 13/13, `threads` 37/37,
  `dlltest` 10/10, `posixtest` 28/28, plus `netcat` and `winhello`).
- Not yet: loading the real Microsoft DLLs and a full modal dialog manager.
  *(The dialog manager came in Phase 12; the system DLLs are still NovaOS's
  own.)*
