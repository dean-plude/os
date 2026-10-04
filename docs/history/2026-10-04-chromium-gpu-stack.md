## A sampling profiler's view of a waiting thread, and an exception dispatcher that stays on the stack

With Direct3D 11 working, Steam's browser started its GPU process on
DXVK, and some of those starts ended with "stack overflow" in ntdll.
Nothing in Vulkan ran out of stack: Chromium starts its GPU process with
`--start-stack-profiler`, a sampler that every so often suspends the main
thread, reads its registers with `GetThreadContext` and unwinds its stack
with `RtlLookupFunctionEntry` and `RtlVirtualUnwind`.  Three NovaOS gaps
turned one bad sample into the crash.

- **`GetThreadContext` of a thread in a system call.**  A thread is
  suspended most often while it waits, and NovaOS gave such a thread's
  context as if ntdll's stub had just returned (right) with every other
  register zero (wrong).  The Vulkan loader's functions find their frame
  from Rbp, so unwinding them from Rbp 0 read memory at address -0x10 and
  faulted.  The context now carries the nonvolatile registers (Rbp, Rbx,
  Rsi, Rdi, R12-R15) the thread entered the kernel with, which the
  system-call entry saves at the top of its kernel stack.
- **ntdll's own frames.**  The fault was raised from inside
  `RtlVirtualUnwind`, and the dispatcher could not unwind ntdll's frames:
  `RtlLookupFunctionEntry` did not look in ntdll itself (the kernel maps
  it without listing it), and the compiler left unwind data off ntdll
  functions that call nothing.  Each try treated a frame as a leaf, read a
  wrong return address and faulted again, deeper each time.  ntdll's own
  `.pdata` is now looked up, and every 64-bit NovaOS DLL is built with
  `-fasynchronous-unwind-tables`, so every function that saves registers
  or takes stack has unwind data, as the Windows ABI requires.
- **Frames off the stack.**  As on Windows, `RtlDispatchException` only
  walks frames that lie within the thread's stack (`DeallocationStack` or
  `StackLimit` up to `StackBase`) and sets `EXCEPTION_STACK_INVALID` on
  the record when one does not, and `RtlUnwindEx` stops there too.

`samplertest` (core suite, x64) waits in a system call with known values
in R12-R15 from a function with an Rbp frame, and suspends, reads and
unwinds it the way the profiler does.  Without the kernel change it ends
with the same stack overflow in ntdll that Steam's GPU process did.
