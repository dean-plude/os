## A general-protection fault is looked into, the way Windows does

A `MOVDQA` or `MOVAPS` with a memory operand that is not 16-byte aligned
raises a general-protection fault on the processor, not a page fault.  On
Windows the kernel does not report that straight away: it decodes the
instruction that faulted first, and for a misaligned 16-byte SSE move, in
a 64-bit thread that has alignment-fault fixup turned on, it rewrites the
instruction in the program's own code to the unaligned form (`MOVDQA` to
`MOVDQU`, `MOVAPS` to `MOVUPS`) and runs it again.  Programs lean on this,
and Roblox's Hyperion (`RobloxPlayerBeta.dll`) uses it as a check: it
turns the fixup on, runs a deliberately misaligned `MOVDQA` in a page it
has just allocated, and expects the store to go through rather than the
process to die.

NovaOS now does the same (`kernel/um/um_gpfault.c`).  A general-protection
fault in a program is decoded like Windows' `KiPreprocessFault`:

- a **misaligned 16-byte SSE move** (`66 0F 6F`/`7F`, `0F 28`/`29`), in a
  64-bit thread with fixup on, is patched to the unaligned opcode through
  the kernel's own mapping of the page and retried; nothing else sees a
  fault.  The fixup is turned on the Windows ways: `SetErrorMode`
  with `SEM_NOALIGNMENTFAULTEXCEPT` (which reaches the kernel as
  `NtSetInformationProcess(ProcessDefaultHardErrorMode)`),
  `NtSetInformationProcess(ProcessEnableAlignmentFaultFixup)` or
  `NtSetInformationThread(ThreadEnableAlignmentFaultFixup)`;
- a **privileged instruction** (`CLI`, `HLT`, `IN`/`OUT`, `LGDT`, `MOV` to
  a control register, `RDMSR`, and the rest) is reported as
  `STATUS_PRIVILEGED_INSTRUCTION`, and `RSM` as
  `STATUS_ILLEGAL_INSTRUCTION`, each with no parameters, as Windows does;
- anything else stays an access violation.

Two smaller things Hyperion also checks are now faithful:
`NtSetInformationThread(ThreadHideFromDebugger)` takes no data, so a
non-zero length is `STATUS_INFO_LENGTH_MISMATCH`, and the flag reads back
through `NtQueryInformationThread`; and ntdll has the extended-context
functions that describe the processor's save state
(`RtlGetExtendedContextLength`, `RtlInitializeExtendedContext`,
`RtlLocateLegacyContext`, `RtlCopyExtendedContext` and the rest, in
`userland/ntdll/ntdll_xstate.c`), backed by an `XSTATE_CONFIGURATION` in
`KUSER_SHARED_DATA` that lists the legacy x87 and SSE state NovaOS keeps.

`aligntest` checks all of it.  With the fixup, Hyperion gets past this
check and runs further into its start-up before stopping at its next step
([compatibility.md](../compatibility.md#roblox-and-anti-cheat)).  NovaOS
satisfies the check by behaving as Windows does; it does not change Roblox
or defeat the check.
