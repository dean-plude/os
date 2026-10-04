## Windows' segment layout, 32-bit code in 64-bit programs, and a thread's own context

Roblox's Hyperion (`RobloxPlayerBeta.dll`), past the alignment-fault
check, runs a piece of 32-bit code inside the 64-bit client.  It maps
memory just below 4 GiB, puts a `CPUID` in the last two bytes, points the
stack there and far-jumps through selector 0x23, which on Windows is the
32-bit user code segment, so the processor drops into compatibility mode;
the instruction pointer then wraps from 0xFFFFFFFF to 0 and the fetch
faults, and Hyperion's handler looks at what the fault reports.  NovaOS
had its own selector numbers, and 0x23 was its 64-bit code segment, so the
"32-bit" bytes ran as 64-bit code and the client crashed at 0x100000000.

NovaOS now uses Windows' x64 GDT layout (`KGDT64_*`):

| Selector | What |
|---|---|
| 0x10, 0x18 | kernel code and data |
| 0x23 | user code, 32-bit (compatibility mode) |
| 0x2B | user data and stack |
| 0x33 | user code, 64-bit |
| 0x40 | the TSS |
| 0x53 | the 32-bit TEB, FS in 32-bit code |

so a 64-bit program sees CS 0x33, SS, DS, ES and GS 0x2B and FS 0x53, as on
Windows, and the STAR MSR holds Windows' value (SYSRET's selectors come
from the 32-bit code selector, which is why Windows orders them this way).
32-bit programs run with CS 0x23 as before, and the 0x53 descriptor follows
the running thread's TEB.

With 32-bit code possible in any program, the exception path follows the
mode the program was in: a fault there reaches the program's handlers with
`SegCs` 0x23 in its CONTEXT (the dispatcher itself runs in the process's
own mode), and `NtContinue`, `NtRaiseException` and `NtSetContextThread`
take 0x33 or 0x23 from the CONTEXT, as Windows' kernel does.

Hyperion then reads its own registers: `NtGetContextThread` on the calling
thread.  NovaOS answered only for other, suspended threads and failed this
with `STATUS_UNSUCCESSFUL`; it now returns the registers the system call
left user mode with, as Windows' trap frame gives them (the SYSCALL entry
keeps the program's nonvolatile registers in a fixed place for it), with
the debug registers clear.  `NtSetContextThread` on the calling thread
resumes at the CONTEXT it is given, and kernel32's
`Get`/`SetThreadContext` now go straight to the kernel on x64.

`__fastfail` (`int 0x29`, the code in RCX) now ends a program the Windows
way, with `STATUS_STACK_BUFFER_OVERRUN` and its fail code in the crash
report, without running its exception handlers; before, the interrupt was
not open to programs and became an access violation they could catch.

With all of this Hyperion goes on to read the firmware's SMBIOS table and
the display devices and reports "Virtual Machine detected", the right
answer under QEMU: Roblox refuses virtual machines on Windows too
([compatibility.md](../compatibility.md#roblox-and-anti-cheat)).  NovaOS
behaves as Windows does; it does not change Roblox, defeat its checks or
hide the virtual machine.  `gatetest` covers the selectors, the far jumps
to and from 32-bit code, the wrapped fetch and its CONTEXT, the calling
thread's context and `__fastfail`.
