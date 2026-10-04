## GOG GALAXY: exceptions thrown inside an MSVC catch block

GOG GALAXY carries Microsoft's own C++ runtime (its setup installs the
Visual C++ redistributable, so `vcruntime140.dll` and `msvcp140.dll` in
System32 are Microsoft's).  That runtime runs a catch block from
`RtlUnwindEx`'s consolidation callback (`STATUS_UNWIND_CONSOLIDATE`),
deep in the stack.  A throw from inside the catch block (a rethrow, or a
new exception) was dispatched by walking up from there: through the
callback, `RtlUnwindEx`, the frame handler, `RtlDispatchException` and
`KiUserExceptionDispatcher`, whose frames have no unwind data.  The walk
never reached the frames above the catching one, so the exception went
unhandled (the client service died setting up its folders on a fresh
install), or it read on past the dispatcher into stack garbage and
faulted inside `RtlVirtualUnwind` (the client, after its NTFS warning).

- **Dispatch** (`userland/ntdll/ntdll_exc.c`): `RtlDispatchException`
  now does what `RtlUnwindEx` already did, and what the unwind data of
  Windows' consolidation frame and of `KiUserExceptionDispatcher` lead
  to: past the frames running a catch block it carries on in the
  catching frame (its handler sees the new exception first), and past
  the frames dispatching an outer exception it carries on where that
  exception happened.
- **Stale entries**: the per-thread record of what the dispatcher and
  unwinder are in the middle of now checks each entry (a frame reused
  since ends the chain), and `RtlRestoreContext` forgets what ran below
  the resumed frame.  NovaOS's own `vcruntime140` resumes after a catch
  through `RtlRestoreContext` (it used `NtContinue`, which left the
  dispatch it came from on the record).
- **Tests**: `unwindtest` (14 checks, 5 new): a catch block run by a
  consolidating unwind continues where its callback says with the frame's
  registers, and an exception raised inside it reaches the catching
  frame's handler and then the caller's `__except`.  `cppeh` and
  `stltest` pass as before.
- **Where GOG GALAXY stops now**: the service sets up its folders and
  serves the client; the client gets past its NTFS warning, opens its
  sign-in window and starts its first Chromium renderer, then Qt ends it
  with "Could not get handle for shared context" (Qt WebEngine needs a
  shared OpenGL context; NovaOS's `CreateDXGIFactory2` also says
  `E_NOINTERFACE`).  The service still fails the client's "write Vulkan
  registry keys" request ([compatibility.md](../compatibility.md)).
