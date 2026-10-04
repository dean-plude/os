## A Windows-numbered system-call table, for code that calls the kernel itself

Most programs reach the kernel through ntdll's stubs, so the number each
stub carries is all that has to match Windows.  Anti-cheat and sandbox
code does not trust the stubs: it works out a service's number itself, the
way the kernel assigns them, and issues its own `syscall` instructions.
Roblox's Hyperion (`RobloxPlayerBeta.dll`) does this, and on NovaOS every
number it worked out came out as 0, so the client crashed at start
([compatibility.md](../compatibility.md#roblox-and-anti-cheat)).

ntdll now has a stub for every Windows 10 1903 service, laid out in
service-number order with an unwind entry each, the way Windows' ntdll is
(one `STUB` per line in `userland/ntdll/ntdll.c`, from the shared
`nt1903_services.h`).  Code that ranks ntdll's `Zw` exports by address, or
counts them in `.pdata` order, now reads each service at its real number.

- **The services ntdll used to answer in C** (`NtQuerySystemInformation`,
  `NtQueryTimerResolution`, `NtRaiseHardError`, the token calls that only
  pretend, job objects and transactions NovaOS has none of, byte-range
  locks, quotas, extended attributes, memory locking, `NtDeviceIoControlFile`,
  `NtSignalAndWaitForSingleObject`, `NtTestAlert` and others) are now
  system calls the kernel answers (`kernel/um/um_services.c`), so their
  ntdll names are stubs at the right numbers.  32-bit programs keep
  ntdll's C versions.
- **`NtRaiseHardError`** reports a program's own error box, or a system
  error, on the serial log.
- **`NtQuerySystemInformation(SystemModuleInformation)`** lists the one
  kernel module, as Windows lists `ntoskrnl.exe`.
- **The number is read the way Windows reads it**, from the low bits of
  `EAX` (bit 12 picks the win32k table, which NovaOS has no numbers for);
  the noise Hyperion leaves in the top bits is ignored.
- **ntdll's loader** gained `LdrAddRefDll`, `LdrUnloadDll`,
  `LdrFindResource_U`, `LdrFindResourceDirectory_U`, `LdrAccessResource`
  and `LdrResSearchResource`, and ntdll carries a version resource
  (10.0.18362.1), which Hyperion reads to tell which Windows it runs on.

`syscalltest` checks the table the way Hyperion reads it: it ranks ntdll's
`Zw` exports by address, confirms a spread of them against Windows 10 1903
and that the kernel answers each number (and returns
`STATUS_INVALID_SYSTEM_SERVICE`, not the wrong service, for one it lacks).
With this, Hyperion lands every raw system call on the service it meant
and reaches a later integrity check, where it stops with "an unexpected
error" and exits on its own instead of crashing on service 0.
