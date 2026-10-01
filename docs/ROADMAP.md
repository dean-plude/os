# NovaOS — Roadmap to a Full Windows-Compatible Desktop OS

**Goal:** a from-scratch x86-64 OS that runs **native Windows executables
(PE32+) without emulation** — i.e. the binaries run directly on the CPU while
NovaOS provides the NT system-call ABI, the Win32 API surface, the GUI server,
loader semantics, and drivers they expect.

> "Without emulation" means no CPU emulation (the code is already native x86-64).
> It does **not** mean "without work": you must reproduce the Windows
> *environment*. This is the ReactOS problem — a clean-room Windows-compatible
> OS. Expect a multi-year arc. This document makes each step concrete and
> independently verifiable so progress is real, not aspirational.

---

## Where we are today (Phases 1–7, done)

| Area | Status |
|------|--------|
| UEFI boot, PMM/VMM/paging, GDT/IDT/APIC, scheduler | ✅ |
| NT executive skeleton: Ob, Ps, Se, Cm, Io, syscall dispatch | ✅ |
| VMA, section objects, PE32+ loader, stub DLLs | ✅ |
| VFS + InitRD (in-memory only) | ✅ |
| Per-process page tables, KPCR, PEB/TEB, SWAPGS | ✅ |
| User-mode SYSCALL thunk pages, kernel-helper syscalls, CSRSS shim | ✅ |
| GDI software renderer, window manager, **static** desktop shell | ✅ |
| **Boots from ISO under OVMF and renders the desktop (verified)** | ✅ |

**Since then (Phases 8–9):** an interactive desktop with real apps, HiDPI
graphics, networking (lwIP, DHCP, DNS), HTTP/1.1 and HTTPS (Mbed TLS), and
**real PE32+ `.exe` files running in ring 3** with our own `ntdll`,
`kernel32`, `msvcrt`, `ws2_32`, `user32` and `gdi32`.  Programs get multiple
threads and the full synchronization set, static TLS, DllMain, structured
exception handling (`__try`/`__except`/`__finally` with a real x64
unwinder), Winsock sockets over lwIP, and native Win32 windows in the
desktop's window manager — launched from the Terminal, with crash isolation
and full memory reclamation.  **Phase 9.5** put all of it to work: the
NetSurf web browser, built from source as a Windows program, browses HTTP
and HTTPS sites (its own fetcher over Winsock + Mbed TLS) with anti-aliased
TrueType text, on a C runtime that now has a POSIX layer.

**Honest gaps:** the real Microsoft DLLs are not loaded (these are
clean-room reimplementations — Path A below); there is no modal dialog
manager or common-controls library yet; no storage driver or persistence
(drive C: is in memory); and the browser runs without JavaScript.

---

## THE pivotal decision: how to get the Win32 API surface

Everything below branches from this. Decide early.

- **Path A — Reimplement (Wine-style).** Write our own `ntdll/kernel32/user32/
  gdi32/…`. Clean-room, no licensing, total control; but the API surface is
  vast and real apps break on missing edge cases.
- **Path B — Binary compat (ReactOS-style).** Match a *specific* Windows build's
  NT syscall ABI exactly, then load the **real Microsoft user-mode DLLs**.
  Instant huge coverage; but brittle to version and constrained by licensing on
  redistributing MS DLLs.
- **Path C — Hybrid (recommended).** Reimplement clean-room while holding
  ABI/struct compatibility so real DLLs/drivers *can* be loaded when desired.
  This is the ReactOS strategy and the most pragmatic.

> **DECISION: Path C — Hybrid.** We build clean-room components but keep
> structures (PEB/TEB/LDR) and syscall numbers ABI-faithful to a chosen target
> build (**Windows 10 1903 x64**, already referenced in the codebase), so real
> Microsoft DLLs/drivers can be loaded when we choose. ABI-conformance tests
> (see cross-cutting) guard this invariant.

---

## Phase 8 — Interactivity foundation
*Make the desktop actually react.*

- PS/2 keyboard + mouse drivers (i8042), IRQ1/IRQ12 via the existing APIC/IDT.
- Input event queue + WM **event loop** (replaces one-shot `DesktopRender`).
- Hardware/software cursor; hit-testing so Start button, dock icons, and window
  drag/close respond.
- Double-buffering (second VRAM mapping) + dirty-rectangle compositor.
- Real RTC (CMOS) driver → live clock.

**Exit:** click the Start menu, drag a window, move a cursor.

## Phase 9 — Prove native user-mode execution *(make-or-break)*
*One real, unmodified PE32+ runs in ring-3 and survives.*

- PE loader completeness: TLS directory + callbacks, delay/bound imports,
  forwarded exports, API-set (`api-ms-win-*`) resolution.
- Exact `PEB`/`TEB`/`PEB_LDR_DATA`/`LDR_DATA_TABLE_ENTRY` for the target build.
- Exception/unwind: `.pdata`/`.xdata`, `RtlVirtualUnwind`, `RtlUnwindEx`,
  `KiUserExceptionDispatcher`; SEH + VEH.
- User callback/APC dispatch: `KiUserApcDispatcher`, `KiUserCallbackDispatcher`,
  `NtContinue`.
- Real `RtlHeap`; correct `NtCreateThreadEx`.

**Exit:** a hand-built Win32 console `.exe` runs to completion via real
`NtWriteFile`. *Until this works, nothing above the kernel is real.*

## Phase 10 — Win32 GUI subsystem (`win32k`-style server)
*User apps create real windows.*

- Kernel-side **win32k** owns the Phase 8 WM and exposes it via syscalls.
- Clean-room `user32`/`gdi32`: `CreateWindowEx`, `GetMessage`/`DispatchMessage`,
  `WM_PAINT`, device contexts, `BitBlt`, text, basic common controls.
- Per-thread message queues, window classes, real `HWND` focus/z-order.

**Exit:** a real GUI `.exe` opens a window, paints, and handles input.

## Phase 11 — Persistence: storage + filesystem
- AHCI (SATA) and/or NVMe block driver; GPT/partition parsing.
- FAT32 read/**write**; then **NTFS read** (most Windows content is on NTFS).
  *NTFS read: done — NTFS volumes mount read-only as drives D:, E:, ...*
- Cache manager (`Cc`); registry hives persisted to disk.

**Exit:** files and registry survive reboot.

## Phase 12 — Loader & DLL ecosystem
- Grow `kernel32`, `advapi32`, `msvcrt`/UCRT, `shell32`/`comctl32` basics.
- **Strategic fork applies here** (A/B/C): expand clean-room DLLs, and/or load
  real DLLs against the ABI-matched syscall table.
- Activation contexts / SxS manifests (installers need these).

**Exit:** a moderately complex off-the-shelf Win32 app launches.

## Phase 13 — System integrity & services
- Full synchronization set (events, mutexes, semaphores, cond vars, SRW locks,
  critical sections, `NtWaitForAlertByThreadId`).
- Services subsystem; SMSS/CSRSS/winlogon-style boot chain (if pursuing the full
  Windows boot model).
- Token/ACL enforcement that real security APIs query.

## Phase 14 — Application-compatibility push
- Bring up apps in difficulty order: simple Win32 sample → Notepad-class → an
  installer-based app.
- App-compat shims; per-app API-hit telemetry drives the backlog.

## Phase 15 — Hardening & platform
- SMP (per-CPU KPCR, IPIs, spinlock audit).
- ACPI (full tables, shutdown/reboot, power), HPET/TSC-deadline timers.
- PnP driver model; USB/HID, NIC, better display modes.

---

## Cross-cutting (start immediately, maintain throughout)

- **Automated boot CI:** the QEMU+OVMF serial-log + framebuffer-screenshot
  harness used to validate Phase 7 should run on every commit (regression gate).
- **Reproducible build:** `nasm + clang/lld + lld-link` toolchain is proven;
  wire an `iso_image`/`run` CMake target around `scripts/create-iso.sh`.
- **Debugging infra:** GDB stub over QEMU, symbolized kernel backtraces, a
  KD-style protocol later; per-syscall tracing.
- **ABI conformance tests:** assert PEB/TEB/struct offsets and syscall numbers
  against the target build so Path B/C stays viable.
- **Test corpus:** a growing set of tiny PEs exercising imports, TLS, SEH,
  threads, and GUI — each a permanent regression test.

## Near-term critical path (next 3 milestones)

1. **Phase 8 input + event loop** — turns the mockup into a live desktop.
2. **Phase 9 single-PE execution** — the existential proof for the whole project.
3. **Phase 10 first windowed app** — first time a "Windows program" runs on NovaOS.

Until milestone 2 lands, treat every higher-level feature as unproven.

## Reality check

This is ReactOS-scale. A single contributor reaches "boots + runs simple apps"
in this plan; "runs arbitrary commercial Windows software" is a long-horizon,
many-person effort. The value here is a credible, ordered path where each phase
produces something demonstrably working.
