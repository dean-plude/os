## Graphics tests in CI, ABI conformance, kernel backtraces

- **Graphics in CI**: a second CI job stages 7-Zip and the Mesa 3D and DXVK
  archives (`tools/ci/stage-graphics.sh`), installs both with the App
  Store, and runs `tools/gltest` (14) and `tools/d3dtest` (17), 64- and
  32-bit, with a screenshot of each while it draws.  The Terminal's new
  `store install NAME` presses a program's App Store button; the outcome
  goes to the serial log as `[STORE] NAME: Installed ...`.
- **`abitest`** checks NovaOS's binary interface against Windows 10 1903
  x64, each offset written out as Windows has it: the TEB, PEB, process
  parameters and loader lists, `KUSER_SHARED_DATA`, `CONTEXT` and
  `EXCEPTION_RECORD` (at compile time and at run time, through an
  exception handler that edits `Rip` and `Rax`, and `GetThreadContext` on a
  suspended thread), ntdll's stubs and all 464 system-call numbers, plus
  raw `syscall` instructions that bypass ntdll.  What it found and fixed:
  - 42 services had NovaOS numbers rather than 1903's (`NtQuerySystemTime`
    0x52 instead of 0x5A, `NtTerminateThread`, `NtResumeThread`, the
    registry, timer, directory and symbolic-link services, ...).  Every
    service Windows has is now at its 1903 number; NovaOS's own services
    moved to 0x200 and up.
  - ntdll's stubs are now Windows's bytes (`mov r10, rcx; mov eax, N; test
    byte [7FFE0308h], 1; jne; syscall; ret; int 2Eh; ret`), which
    sandboxes and hooking libraries parse.
  - The program now heads `InLoadOrderModuleList` and
    `InMemoryOrderModuleList` and is not in the initialization-order list.
  - `KUSER_SHARED_DATA.NtBuildNumber` said 19045; it is 18362, as the PEB
    and registry say.  `GetTickCount` reads the shared page's tick count,
    as on Windows, so both agree.
  - `RtlCaptureContext` fills in the segment registers.
  - `GetThreadContext` on a thread just suspended while running in user
    mode failed: the kernel waited a number of yields for it to stop, which
    can pass in microseconds; it now waits up to a second.
- **Symbolized kernel backtraces**: the kernel is linked twice; the first
  link's functions (`tools/mkksyms.py`) become a `.ksyms` table that the
  second link embeds after `.text`, so no function moves (the build checks).
  The kernel is built with frame pointers; a kernel page fault, exception,
  `KPANIC` or `KASSERT` prints `Backtrace:` and `#N address function+offset`
  frames on the serial log.  `crash kernel` (a new `NtNovaBugCheck`
  service, guarded by a magic argument) faults three calls deep to show
  it; CI checks the frames.
