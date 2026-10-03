## Floating-point state and exceptions, as on Windows

Under KVM Audacity stopped at start with `0xc000008f` (inexact result)
in wxWidgets, and Krita with `0xc0000090` (invalid operation) in Qt
Quick, after a C++ exception had been caught.  App corpus round three
fixed the cause (`RtlCaptureContext` left `FltSave` empty, so the state
reloaded after the `catch` unmasked every exception).  Writing a test
for the whole floating-point state turned up three more differences
from Windows:

- **x87 errors had one name.**  The kernel reported every x87 fault
  (#MF) as `STATUS_FLOAT_INVALID_OPERATION`.  It now reads the x87
  status and control words, as it already read MXCSR for SSE faults, so
  a divide by zero is `STATUS_FLOAT_DIVIDE_BY_ZERO`, an overflow
  `STATUS_FLOAT_OVERFLOW`, a stack fault `STATUS_FLOAT_STACK_CHECK`.
- **Handlers ran with the fault still pending.**  The exception flags
  that caused an x87 or SSE fault stayed set while the program's
  handlers ran, so a handler's own x87 instruction faulted again.  The
  `CONTEXT` keeps the flags and the handlers now run with them cleared,
  as on Windows.
- **Threads started with the x87 control word `0x37F`** (64-bit
  precision); Windows starts them with `0x27F` (53-bit precision), and
  `_fpreset` sets the same.

- **Test.**  Core self-test `fpstate` (64- and 32-bit): the state on
  the main thread and a new thread, across context switches, after
  `RtlRestoreContext` and an SEH unwind, inexact results without a fault,
  and an unmasked divide by zero caught with its own code.  QEMU's TCG
  never raises SSE exceptions, so the unmasked SSE check only bites under
  KVM (CI's Build and boot-test) and on real hardware.
