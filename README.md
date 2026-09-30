# NovaOS — Windows-Compatible Operating System

A clean-room, from-scratch x86_64 operating system designed to run native
Windows executables without emulation: 64-bit (x64, PE32+) programs, and
32-bit (x86, PE32) ones, such as most setup programs, through its own
WoW64 layer.

## Status: Phase 13 — 32-bit Windows programs (WoW64), after unmodified 7-Zip, an App Store, Windows Installer (.msi) and a NovaOS installer

**What works:**

### Phase 1 — Boot & Kernel Foundation
- UEFI bootloader (PE32+ EFI application, loads kernel ELF from FAT32 ESP)
- Physical memory manager (bitmap allocator, reads UEFI memory map)
- Virtual memory (4-level paging, 64 GiB physmap, NX via EFER.NXE, CR4.PGE)
- Kernel heap (slab allocator: kmalloc/kfree/kzalloc)
- GDT/TSS with IST stacks for NMI, double fault, machine check
- IDT with all 256 handlers (CPU exceptions, APIC IRQs, INT 0x2E syscall gate)
- APIC timer at 100 Hz (legacy 8259A PIC disabled)
- Preemptive round-robin kernel scheduler (kernel threads, context switch via callee-saved regs)
- Serial console (COM1, 115200 baud) + GOP framebuffer text output
- Boots in QEMU with OVMF (UEFI firmware)

### Phase 2 — NT Kernel Personality
- Object Manager (`ObCreateObject`, `ObReferenceObject`, `ObDereferenceObject`, handle tables)
- Security Reference Monitor (`SeInitialize`, `SeCreateSystemToken`, ACL/ACE stubs)
- Process Manager (`EPROCESS`/`ETHREAD`/`KPROCESS`/`KTHREAD`, `PsCreateSystemProcess`, `PsCreateSystemThread`)
- Configuration Manager (registry hive bootstrap, `CmInitialize`)
- Syscall dispatcher: INT 0x2E (legacy) + SYSCALL/SYSRET MSR path (`KiSystemCall64`)
- NT syscall table with ~40 entries (NtAllocateVirtualMemory, NtCreateFile, etc.)

### Phase 3 — I/O, Sections, and PE Loader
- I/O Manager (`IoCreateDevice`, `IoCreateFile`, IRP dispatch: Create/Read/Write/QueryInfo)
- Virtual Memory Areas (`VMA_SPACE`, `VmaAllocate`, `VmaFree`, `VmaProtect` per-process)
- Section Objects (`MmCreateImageSection`, `MmMapImageView`, anonymous sections)
- PE32+ Loader (`LdrLoadImage`): header validation, section mapping, base relocation, import resolution
- Built-in stub DLLs: ntdll, kernel32, msvcrt, user32 (syscall thunks for import resolution)
- User-mode process creation (`PsCreateUserProcess` → IRETQ into ring-3 via `PsUserThreadEntry`)

### Phase 4 — VFS & InitRD
- VFS abstraction layer (`VfsMount`, `VfsOpen`, `VfsRead`, `VfsReadDir`, `VfsStat`)
- NT path alias resolution (`\??\C:\`, `\SystemRoot`, `\DosDevices\C:` → first mount)
- CPIO newc InitRD driver (`InitrdMount`): parses in-memory CPIO archive, mounts as `\Device\InitRD`
- Boot protocol v2: bootloader passes `initrd_base`/`initrd_size` in `BootInfo`
- File I/O via IRP dispatch: `NtReadFile`, `NtWriteFile` fully wired through VFS to InitRD

### Phase 5 — Process Isolation & User-Mode Foundation
- Per-process page tables: each user process gets its own PML4 (`paging_create_process_pt`)
- User-space mappings snapshotted into process PML4 at creation (`paging_clone_user_mappings`)
- CR3 switch on every context switch in the scheduler (`perform_switch`)
- KPCR (Kernel Processor Control Region): per-CPU GS-relative structure
  - `KPCR.KernelRsp` holds the current thread's kernel stack top for the SYSCALL path
  - Updated by the scheduler on every context switch
- SWAPGS: on SYSCALL entry, GS → KPCR; on SYSRET, GS → user TEB
- Kernel stack switch: `syscall_entry.asm` loads `gs:[KPCR_KERNEL_RSP]` before entering C
- PEB64/TEB64 allocation: `PsAllocatePebTeb` maps one page each at fixed user VAs
  - TEB.NtTib.Self initialized (GS:[0x30] self-pointer)
  - TEB.ClientId (PID/TID) and PEB.ImageBaseAddress set
- User GS base (`MSR_GS_BASE`) set to TEB VA before IRETQ so SWAPGS works on first syscall

### Phase 6 — Full User-Mode Foundation
- **User-mode SYSCALL thunk pages** (`ldr/user_stubs.c`): one 4 KiB page per stub DLL
  (ntdll, kernel32, msvcrt, user32) containing genuine ring-3 x86-64 machine code:
  `mov r10,rcx; mov eax,N; syscall; ret` (16-byte slots)
  - Mapped read+exec into every new process's private page table
  - IAT entries now hold user-mode VAs — imported functions callable from ring-3
- **Kernel-helper syscalls** (0x01F0–0x01FF): extend the NT dispatch table for Win32
  helpers with no NT syscall number:
  - `KH_RtlAllocateHeap/FreeHeap/ReAllocateHeap` → `VmaAllocate`/`VmaFree` on process VMA
  - `KH_GetCurrentProcessId/ThreadId` → read from EPROCESS/ETHREAD
  - `KH_GetLastError/SetLastError`, `KH_IsDebuggerPresent`, `KH_GetStdHandle`
  - `KH_DbgPrint`, `KH_RtlInitUnicodeString`, `KH_RtlZeroMemory/MoveMemory`
  - `KH_GetCurrentProcess` → `(HANDLE)-1`, `KH_GetCurrentThread` → `(HANDLE)-2`
- **New NT syscalls**: `NtSetInformationThread`, `NtFlushInstructionCache`,
  `NtCreateProcessEx`, `NtCreateThread` (stubs; full implementation Phase 7)
- **`KiSystemCallDispatch` now returns `UINT64`** so kernel-helper calls can return
  64-bit pointers (e.g., heap allocations) directly in RAX
- **PEB improvements**: `ProcessHeap`, `NtGlobalFlag`, `CriticalSectionTimeout`,
  `HeapSegment*`, `MaximumNumberOfHeaps`, `CurrentLocale` all initialized
- **CSRSS bootstrap** (`ps/csrss.c`): kernel-side shim that:
  - Runs a `csrss` kernel thread (event loop placeholder)
  - `CsrRegisterProcess()` — logs and tracks every new user process
  - `CsrClientCallServer()` — no-op stub returning `STATUS_SUCCESS`
  - Prevents ntdll's `LdrpInitializeProcess` from failing on CSRSS connect

### Phase 7 — GUI, Window Manager & Desktop Shell
- **GDI software renderer** (`gdi/gdi.c`): a dependency-free 2D rasterizer that
  draws straight into the GOP linear framebuffer (BGR/RGB aware, fully clipped):
  - Solid / alpha-blended fills, vertical & horizontal gradients
  - Rounded rectangles (solid, alpha, vertical-gradient), circles, H/V lines
  - Color lerp, and text via the embedded 8×16 VGA font
    (normal, bold double-strike, transparent, centered)
- **Window manager** (`wm/wm.c`): a compositing WM over GDI:
  - Fixed pool of `WND` windows with Z-ordering, focus, show/hide
  - Decorated frames — title bar, accent stripe, caption glyphs, drop shadow,
    rounded body and 1px border
  - `WmComposite()` paints background → windows (ascending Z) → overlay
- **Desktop shell** (`wm/desktop.c`): a Windows-11-style desktop, rendered fully
  in software to match the design mock:
  - Gradient wallpaper with six warm "wave" layers (integer-sine curves)
  - Left-column desktop icons (My PC, Documents, Personal, Proyect, Files, photos)
  - Centered **Start menu**: search box, pinned-app grid, "see all" pill, live
    tiles (Today/calendar, weather, media, cloud storage, photos, to-do, games),
    a "Recently used" list, and a user / power bar — with two app tiles peeking
    above the panel
  - Floating, rounded **taskbar/dock** with app glyphs and a clock
- **Framebuffer raw-surface API** (`fb_get_raw`, `fb_draw_string[_trans]`): lets
  GDI write pixels directly; `kprintf_set_fb_enabled(false)` silences the boot-log
  text console once the desktop is painted (serial keeps every message)
- Booted as **STEP 22** in `KiSystemStartup`: after the executive is up the kernel
  initializes GDI → WM → shell and renders one full desktop frame

### Phase 8 — Interactive Desktop
- **Preemptive multitasking fixed**: the APIC timer ISR now calls `sched_tick()`
  (it was a stubbed `TODO`), so created threads actually run and are
  time-sliced. The shell runs in its own `desktop` kernel thread.
- **PS/2 input** (`hal/ps2.c`): polled i8042 keyboard + mouse; bytes are decoded
  into an event queue (`wm/input.c`). Keyboard uses scancode set 1; the mouse
  uses the 3-byte streaming packet.
- **Software mouse cursor** (`wm/wm.c`): an arrow drawn with *save-under* so it
  moves without recompositing the whole 2560×1600 scene.
- **Event loop** (`DesktopRun`): polls input, drives the cursor, and recomposites
  only on state change or once per minute. Left-click hit-tests the Start button
  (toggles the menu) and click-away / **Esc** closes it.
- **Live clock** (`hal/rtc.c`): the dock clock + date are read from the CMOS RTC.
- Verified under QEMU+OVMF: cursor tracks the mouse, Esc closes the Start menu,
  and the clock advances in real time.

### Phase 8 (cont.) — Graphics & Working Desktop
- **HiDPI rendering** (`gdi/`): logical coordinates with an integer display
  scale (2× at 2560×1600); anti-aliased shapes, soft shadows, double
  buffering, and text in **Inter** / **Cascadia Mono** pre-rasterized at build
  time by `tools/mkfont.c` (fonts under `third_party/`, SIL OFL 1.1).
- **Window manager** (`wm/wm.c`): Windows 11-style frames; focus and z-order,
  drag by the title bar, double-click to maximize, minimize/maximize/close
  buttons, Alt+F4, keyboard focus, clipped client painting.
- **Built-in apps** (`apps/`): Terminal (a shell: `dir`, `cd`, `type`, `echo`,
  `mkdir`, `del`, `start`, `sysinfo`, `dmesg`, …), File Explorer, Notepad
  (edit + Ctrl+S), Settings (live system/display/storage info) and Calendar.
  Third-party apps pinned in the dock/Start menu open a "not available yet"
  note until real `.exe` files can run.
- **Drive C:** is a RAM disk (`fs/ramfs.c`) with starter folders and files;
  changes last until reboot.
- **Shell** (`wm/desktop.c`): the dock launches/focuses/minimizes apps and shows
  running indicators; Start menu tiles and "Recently Used" launch apps;
  desktop icons open on double-click; the Windows key toggles Start.
- Keyboard/mouse input is drained from the 100 Hz timer interrupt, so no
  input is lost while a frame is being drawn.
- Not yet real: apps are kernel-side callbacks (no user-mode GUI programs),
  there is no disk, and there are no image icons.

### Phase 8 (cont.) — Networking
- **PCI** (`hal/pci.c`): bus scan through configuration mechanism #1; BAR
  decoding (32/64-bit) and bus-master enable.
- **Network card** (`drivers/e1000.c`): Intel e1000 (82540EM, QEMU `e1000`)
  and e1000e (82574L, the q35 default); 32-entry RX/TX descriptor rings,
  polled by the net thread.
- **TCP/IP** (`net/net.c`): [lwIP 2.2.0](https://savannah.nongnu.org/projects/lwip/)
  (BSD licence, vendored in `third_party/lwip`) running on a dedicated `net`
  kernel thread — Ethernet/ARP, IPv4, ICMP, UDP, TCP, **DHCP** and **DNS**.
  Apps start asynchronous operations (resolve, ping, HTTP GET) and poll them
  from their window's tick, so the desktop never blocks on the network.
- **Terminal tools**: `ipconfig`, `ping [-n N] host`, `nslookup host`,
  `curl URL` (prints the response) and `wget URL` (saves to `C:\Downloads`);
  HTTP/1.0 with up to 5 redirects; Ctrl+C cancels. **Settings → Network**
  shows the adapter, MAC, lease, gateway and DNS servers live.
- QEMU's default user-mode network works out of the box (guest `10.0.2.15`,
  host reachable as `10.0.2.2`); add `-nic user,model=e1000` to test the
  older card.
- **HTTP/1.1 client** (`net/http.c`): keep-alive connection pool — follow-up
  requests and redirects to the same server reuse the open connection (idle
  connections are kept 30 s; a request that meets a connection the server
  already closed is retried once); `Content-Length`, chunked and
  close-delimited bodies; truncated responses are reported as errors.
- **HTTPS** (`net/tls.c`): [Mbed TLS 3.6](https://www.trustedfirmware.org/projects/mbed-tls/)
  (Apache-2.0, vendored in `third_party/mbedtls`, configured by
  `net/port/mbedtls_nova_config.h`) — **TLS 1.3 and 1.2** (ECDHE, AES-GCM,
  ChaCha20-Poly1305), full X.509 chain validation (trust, expiry, host name
  or IP address), and **session resumption** (TLS 1.3 tickets / TLS 1.2
  sessions) for new connections to a server seen before.  The built-in
  store holds the **146 Mozilla root CAs** (`net/tls_roots.c`, regenerated by
  `scripts/gen-tls-roots.py`); `certutil` lists them and
  `certutil -addstore root <file>` trusts extra PEM/DER CAs until reboot.
  Random numbers: Mbed TLS's CTR-DRBG seeded from a SHA-256 entropy pool
  fed by RDRAND (when present), TSC jitter and packet timing.
- **Terminal**: `curl`/`wget` take several URLs (`curl URL URL ...`) and show
  the protocol, cipher suite, reused connections and resumed sessions.
- Not yet: IPv6 and HTTP/2.  (The web browser arrived in Phase 9.5.)

### Phase 9 — Windows programs (ring 3)
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

### Phase 9.5 — The NetSurf web browser
- **[NetSurf](https://www.netsurf-browser.org/) 3.11** — a real HTML/CSS
  browser engine (libcss, libdom, hubbub) — built from source for NovaOS
  (`tools/build_netsurf.py`, clang + lld-link for x86_64-pc-windows-msvc)
  into `C:\Programs\NetSurf\netsurf.exe`, a Windows program that runs in
  ring 3 on the system DLLs.  Open it from the **globe icon in the dock**
  (or Start), or type `netsurf [url]` in the Terminal (its log then appears
  there).  It renders HTML and CSS 2.1 (floats, tables, positioning; web
  fonts are not loaded), PNG, JPEG, GIF and BMP images, and plain text;
  links, forms, cookies, history, Back/Forward/Reload and error pages work.
- **HTTP and HTTPS** (`userland/netsurf/fetch_nova.c`): NetSurf's fetcher
  interface implemented over Winsock (`ws2_32`) with Mbed TLS 3.6 in the
  program (TLS 1.3/1.2, SNI, ALPN, certificate verification against the
  Mozilla roots in `res\ca-bundle.der` plus any root added with
  `certutil -addstore root`, which the Terminal now also saves to
  `C:\Windows\System32\CertStore`).  Each fetch runs on its own thread;
  responses may be chunked and gzip/deflate compressed; redirects, 304,
  401 (Basic auth), POST (url-encoded and multipart) and cookies are
  handled.  Random numbers come from the kernel's entropy pool through a
  new `NtNovaGetRandom` service.
- **Display and input** (`userland/netsurf/nsfb_novaos.c`): a libnsfb
  surface whose framebuffer *is* the window's client bitmap, so NetSurf's
  plotters draw straight into the desktop window; keyboard (with key-up
  events, now delivered to program windows) and mouse come from the Win32
  message queue.
- **Text** (`userland/netsurf/font_nova.c`): anti-aliased TrueType text
  with sub-pixel positioning, rendered at run time by
  [stb_truetype](https://github.com/nothings/stb) from Inter (sans-serif)
  and DejaVu Sans Mono (monospace) in `C:\Windows\Fonts`; italic is
  synthesized.  JPEG decoding uses stb_image (`jpeg_stb.c`).
- **The C runtime grew a POSIX layer** for it (`userland/msvcrt/posix.c`,
  `iconv.c`, headers in `userland/include/posix`): file descriptors with
  `dup`/`fdopen`/`pread`/`pwrite`, `stat`, `opendir`/`scandir`,
  `getopt_long`, `gettimeofday`, `iconv` (UTF-8/16/32, Latin-1,
  Windows-1252), `asprintf`; `crt0` now runs static constructors.
- **JavaScript** (Duktape 2.x, NetSurf's engine, with its generated DOM
  bindings): page scripts, external scripts, `setTimeout`/`setInterval`,
  events (`addEventListener`, `onclick`), JSON, `Date`, and navigation from
  script run; a failing script does not stop the page.  It is on by
  default; `enable_javascript:0` in `C:\Programs\NetSurf\res\Choices`
  turns it off.  Like NetSurf 3.11 on every platform, changes a script makes
  to the page *after* it has been laid out are not redrawn yet.
- Not yet: SVG, IPv6, and window resizing.

### Desktop UX refresh
- **Start menu** (`wm/desktop.c`): live search as you type (Win key, then
  type) over the built-in apps, the programs in `C:\Programs`, Settings
  pages ("display", "wallpaper", "wifi"...) and every file and folder on C:.
  Up/Down pick a result, Enter opens it, Esc clears the query and then closes
  the menu.  The home view shows pinned apps, the installed programs (click
  to run in a Terminal), recently used apps, files and folders, and a footer
  with Files, Settings and a power menu (Restart, Shut down).
- **Window management** (`wm/wm.c`): drag a title bar to the left or right
  edge to snap to half the screen, or to the top to maximize (a translucent
  preview shows where it will land); dragging a tiled window restores its
  size.  Resize windows from any edge or corner.  Keyboard: **Alt+Tab**
  (Shift+Alt+Tab backwards) with a switcher, **Win+Left/Right/Up/Down**
  (snap, maximize, restore/minimize), **Win+D** (show desktop and back),
  **Win+E** (File Explorer), **Win+S** (search).
- **Right-click menus** on the desktop (Terminal, File Explorer, show
  desktop, next wallpaper, Personalize, Display settings), desktop icons
  (Open, Open in Terminal), title bars (maximize/restore, snap, minimize,
  close) and dock items (open or new window, close).
- **Dock and tray**: the Start menu, dock and tray are neutral dark
  frosted glass (`GdiBackdrop` blurs whatever is behind them, then tints
  it), so they sit well on any wallpaper.  The dock holds only launchers
  and windows, on a fixed grid (40 px targets, 28 px app tiles, 22 px
  glyphs); every window without a pinned app gets its own button with a
  running indicator.  The tray, a separate block at the right, shows the
  network status (tooltip: address; click: Settings > Network) and the
  clock (tooltip: full date; click: Calendar).
- **Themes**: three wallpapers (Sunset, Ocean, Twilight), chosen in the new
  **Settings > Personalization** page.
- **One icon style**: apps are rounded-square tiles with a white line glyph;
  folders (special ones show their purpose: Documents, Downloads, Pictures,
  Projects), documents and This PC are flat two-tone shapes; sidebars,
  toolbars and the tray use single-colour line glyphs; every line is 8% of
  the icon size thick.  The Start button is NovaOS's star.
- **Windows**: the title bar is a shade darker than toolbars, with a
  hairline under it, and every window has a faint light edge.
- **File Explorer**: a command bar with back/up, a clickable breadcrumb
  path (This PC > Local Disk (C:) > Documents; long paths collapse from the
  left) and "+ Folder" / "+ File" buttons; the sidebar has line glyphs per
  place (now including Downloads) and is divided from the list.
- **Desktop**: This PC, Documents, Downloads, Pictures and Projects open
  File Explorer there; NetSurf has an icon.  The selection is a soft
  translucent fill, and labels have a blurred drop shadow so they stay
  readable on bright wallpaper.
- **Windows icons (.ico)** (`gdi/icon.c`, `gdi/png.c`): the kernel reads
  .ico and .cur files, whose images may be BMP-style DIBs (1, 4, 8, 16, 24
  or 32 bits per pixel with the AND mask) or PNG (as 256x256 Vista-style
  icons are), and the icons in a program's resources (RT_GROUP_ICON /
  RT_ICON in any .exe or .dll).  Drawing picks the image that best fits the
  size at device resolution and scales it with alpha blending.  PNG is
  decoded by a small built-in inflate + PNG decoder (every colour type, bit
  depth and interlacing; it matches libpng on the PngSuite images).
  A program shows its own icon (a `NAME.ico` next to `NAME.exe` wins over
  the one in its resources) in the Start menu, search, the title bar, the
  dock, Alt+Tab and File Explorer; Explorer also shows .ico files as
  themselves and PNG thumbnails, with file types ("Icon", "Application",
  "PNG image").  Programs get icons at build time from `programs/NAME.rc`
  (compiled by `llvm-rc`): `winhello.exe` has one drawn by
  `tools/make_icons.py`, and `netsurf.exe` carries NetSurf's own.
- **Photos** opens .ico, .cur and .png files: the picture, and for icons
  every image in the file (size and colour depth) in a strip, the selected
  one enlarged pixel for pixel; Left/Right step through the folder, and on
  its own it shows a gallery of `C:\Pictures` (which now holds a sample
  icon and picture).  .exe files opened from Explorer run.
- Not yet: `LoadIcon`/`DrawIcon` and `WM_SETICON` for programs (a window
  shows its program's first icon), and animated cursors.

### Phase 10 — Standard DLLs, registry, COM and persistent storage
- **Unmodified Windows programs run**: stock release builds of ripgrep and
  fd (Rust, MSVC), jq (C, MinGW) and fzf (Go) work from the Terminal:
  searching, walking folders, filtering.  Copy an `.exe` onto the data disk
  (see [Where your files are kept](docs/building.md#where-your-files-are-kept))
  and type its name.
- **The standard DLLs**, all built from source in `userland/`:
  - `ucrtbase.dll` and the `api-ms-win-crt-*` API sets: the Universal C
    Runtime, from the same sources as `msvcrt.dll` (with musl's libm), each
    with its own `printf` rounding; `vcruntime140.dll` has the MSVC **C++
    exception** machinery (`__CxxFrameHandler3`, `_CxxThrowException`,
    RTTI) as well as `memcpy` and friends.
  - `kernel32.dll` grew I/O completion ports, overlapped and alertable I/O,
    file mappings, waitable timers, `WaitOnAddress`, `CreateProcess` and
    `GetExitCodeProcess`, `FormatMessage`, NLS (`CompareString`,
    `LCMapString`, code pages), resources, psapi, and the registry.
  - `advapi32.dll` (tokens, SIDs, security descriptors, CryptoAPI hashes and
    random numbers, the event log, services), `bcrypt.dll` (SHA-1/2, HMAC,
    RNG), `shell32.dll` (known folders, `CommandLineToArgvW`,
    `ShellExecute`), `shlwapi.dll` (paths, strings, URLs, the `SH*`
    registry helpers), `psapi.dll`, `version.dll`, `winmm.dll`,
    `comctl32.dll`, `comdlg32.dll`, `userenv.dll`; `ws2_32.dll` gained the
    Winsock 2 extensions (`WSASend`/`WSARecv`, overlapped operations through
    completion ports, `WSAEnumProtocols`, `GetAddrInfoW`...); `user32` and
    `gdi32` cover about 400 and 120 functions (DIB sections, fonts, text).
  - Every program also sees `KUSER_SHARED_DATA` at 0x7FFE0000 (the Go
    runtime reads its clock there).
- **The registry** (`kernel/um/um_registry.c`, `userland/kernel32/registry.c`):
  real keys and values behind the `NtCreateKey`/`NtQueryValueKey` family,
  the whole `Reg*` API (advapi32 forwards to kernel32) with the predefined
  roots, and the usual contents (`CurrentVersion`, `CentralProcessor`,
  environment, shell folders, time zone...).  It is saved to
  `C:\Windows\System32\config\REGISTRY.DAT`.  `reg query|add|delete|export`
  works as on Windows.
- **COM** (`userland/ole32`, `userland/oleaut32`): `CoInitializeEx`,
  `CoCreateInstance` through registered class objects or
  `HKCR\CLSID\{...}\InprocServer32` DLLs (`DllGetClassObject`,
  `DllCanUnloadNow`), ProgIDs, `CoTaskMem*`/`IMalloc`, GUID strings,
  `CreateStreamOnHGlobal`; OLE Automation with BSTRs, VARIANTs and
  `VariantChangeType`, SAFEARRAYs, dates and error info.  `testdll.dll` is
  a sample in-process server, `regsvr32` registers it, and the SDK now has
  `objbase.h`/`oleauto.h`.
- **Persistent storage** (`kernel/drivers/ahci.c`, `kernel/fs/fat.c`,
  `kernel/fs/persist.c`): an AHCI SATA driver, FAT16/FAT32 with long file
  names (read, write, format), and drive C: saved to disk: changes are
  written a second after they happen and restored at boot.  NovaOS uses a
  volume labelled `NOVADATA`, formats an empty disk, or falls back to the
  boot disk.  Settings > Storage and the Terminal's `vol`/`sync` show and
  control it.
- Tests: `apitest` 46/46, `comtest` 49/49, `cppeh` 17/17 and
  `disktest write` / `verify` across a restart (150 files, passing on FAT32
  and FAT16, with `fsck.fat` finding the volumes clean).
- Not yet: dialog boxes, menus and child-window
  controls, file-open dialogs (they report "cancelled"), the MSVC FH4 C++
  exception tables, type libraries, `RegNotifyChangeKeyValue` events, audio,
  and a clipboard shared between programs.

### Phase 11 — Multiprocessor (SMP)

- **Every CPU core runs threads** (`kernel/ke/smp.c`): the kernel finds the
  processors in the ACPI MADT, starts each one with INIT and STARTUP IPIs
  through a real-mode trampoline (`arch/x86_64/ap_trampoline.asm`), and gives
  it its own KPCR (reached through GS), GDT, TSS and exception stacks, LAPIC
  timer, and idle thread.  Up to 16 CPUs.
- **Programs run in parallel**, and much of the kernel runs on every core at
  once too (below).
- **Per-core ready queues** (`ke/scheduler.c`): each core has its own run
  queue, lock and timed-sleep list, so cores schedule without touching each
  other.  A thread stays on the core it last ran on (warm caches); a new
  thread starts on the core that created it, and a core with nothing to run
  steals a ready thread from a busy one (every tick while idle, or at once
  when it is woken).  Making a thread ready wakes its own core if that one is
  idle, else any idle core, so the work spreads out.
- TLB shootdowns (IPIs) keep the other cores' page-table caches right when a
  program frees or re-protects memory; halted cores are woken by IPI when a
  thread becomes ready.  The clock follows the TSC, so it keeps time whatever
  the cores are doing.
- Programs see the core count: `GetSystemInfo`, the PEB,
  `KUSER_SHARED_DATA` and `NUMBER_OF_PROCESSORS`.  `cpus.exe` runs the same
  work on 1 thread and then on one thread per core: with `-smp 4` it reports
  a speedup of up to about 3.7× (less when the host's own cores are busy).
- **Finer-grained kernel locking**: the big kernel lock now belongs to
  threads (it nests, and the scheduler drops it when a holder is switched
  out), and these run without it, on all cores at once, under locks of
  their own:
  - the scheduler (each core's run-queue lock, held across its switches),
    the timer and IPIs;
  - memory: the physical page allocator and the kernel heap (spinlocks), a
    program's address space (its process lock), with TLB shootdowns before
    pages are freed;
  - waits, events, mutexes, semaphores and handle closing (the handle table
    under the process lock, object state under one spinlock, destructors
    taking the big lock themselves);
  - sockets (`net_lock` around the network stack), and program windows:
    message queues, `PostMessage`, `InvalidateRect` and timers have a
    spinlock of their own, and the desktop draws without the big lock under
    `DesktopLock` (the built-in apps' painters take it back);
  - the list, and what each service relies on, is in `um_syscall.c`
    (`um_lock_free_init`); files, the registry, process creation, the
    console and the loader still use the big lock.
- Waits are woken, not polled: `SetEvent`, `ReleaseSemaphore`, a thread or
  process ending, a message arriving or network data coming in wakes the
  waiting threads at once (wait queues, `ke/waitq.c`), and `select()` waits
  in the kernel.  Copies to and from program memory survive the memory
  being freed by another thread meanwhile (they fail with an access
  violation instead of crashing the kernel).
- `smpstress.exe` works all of this from many threads (critical sections,
  event ping-pong, semaphores, memory, handles, and freeing memory while the
  kernel copies into it) and checks the results.
- Boot QEMU with `-smp 4` (or any count) to use it.
- Also: window shadows skip the part their window covers (the desktop draws
  about twice as fast), and the network thread and ntdll's lock waits sleep
  instead of spinning.

### Phase 12 — Windows GUI programs: 7-Zip, unmodified

The goal of this phase is running real, unchanged Windows programs with
a graphical interface.  The test case is **7-Zip 26.03 (x64)** from
`third_party/7z2603-x64.exe`: the installer, the file manager (`7zFM.exe`)
and the GUI (`7zG.exe`) all run as shipped.  Nothing in 7-Zip was
changed; every fix is in NovaOS.

- **What 7-Zip does on NovaOS**: the installer shows its dialog, picks a
  folder with the "..." browser and installs with the progress bar; the
  file manager shows its toolbar, path box and file list (icons, sizes,
  dates, sortable columns), browses drives and folders, opens archives
  and lists their contents; Add to Archive opens `7zG`'s full options
  dialog and writes a real `.7z`; Extract goes through the Copy dialog
  and the Confirm File Replace prompt; Tools → Options opens with all six
  pages.  The file manager refreshes when files change on disk, and
  7-Zip's own icons show in the dock and title bars.
- **A real window system in user32** (`userland/user32/`): the window
  tree lives in the program, and only top-level windows have a desktop
  window (made when first shown); children are rectangles of their
  parent's bitmap, painted parent-first with invalidation cascading to
  siblings.  Per-thread message queues, sent messages across threads,
  timers, hit-testing, capture, double clicks, `WM_SETCURSOR`, mouse
  leave tracking, keyboard focus and `TranslateMessage`, A/W conversion
  of every message, hooks, properties, atoms, clipboard.  The standard
  controls (button, static, edit, list box, combo box, scroll bar), menus
  (bars, pop-ups, sub-menus, check marks, accelerators), dialogs from
  resource templates with tab and group navigation, `IsDialogMessage`,
  message boxes with icons, `DrawText` (wrapping, ellipsis, prefixes),
  `DrawEdge`, `DrawFrameControl`, icons from `.ico` and PE resources.
  `gdi32` draws TrueType text (Inter and DejaVu through stb_truetype)
  with clipping, viewport origins and raster operations.
- **The common controls** (`userland/comctl32/`), a real DLL that
  registers its classes when loaded: list view (details, list, small and
  large icons; owner data, callbacks, the Windows selection rules,
  keyboard, type-ahead, sorting, custom draw, label editing), header
  (resizing, ordering, sort arrows), toolbar (image lists and bitmaps,
  text, check groups, drop-downs), rebar, ComboBoxEx, tree view (lines
  and buttons, check boxes, expand/select notifications, label editing,
  sorting), tab control, property sheets (`PropertySheet` with the pages'
  dialogs, OK/Cancel/Apply/Help, wizards), progress bar, status bar,
  up-down, trackbar, tooltips, SysLink, image lists (ARGB, masks, icons),
  `DPA`/`DSA`, and real subclassing.  A program that uses a common control
  without linking `comctl32` gets it loaded on demand, as Windows does
  through the manifest.
- **The shell** (`shell32`): a folder picker (`SHBrowseForFolder` with
  the `BFFM_*` callback protocol), the system image lists with drawn icons
  for folders, drives and the common file types (`SHGetFileInfo`,
  `SHGetImageList`), `DragAcceptFiles`/`DragQueryFile` for dropped files.
- **Kernel sections**: named shared memory between processes
  (`NtCreateSection`, `NtMapViewOfSection`; `CreateFileMapping` and
  `OpenFileMapping` by name), which 7zFM and 7zG use to talk to each
  other.  File-backed mappings are sections too: filled from the file and
  written back on unmap, flush and last close, so a mapping of a file is
  shared between processes.  `shmtest.exe` checks both.
- **Drag and drop between programs**: OLE `DoDragDrop` with
  `IDropSource`/`IDropTarget` and `RegisterDragDrop` within a process, and
  across programs through the desktop, which tells the dragging program
  which window is under the pointer and carries the dropped file list to
  the other program; there it reaches the registered `IDropTarget` (as a
  `CF_HDROP` data object) or arrives as `WM_DROPFILES`.  `droptest.exe`
  has a source window and both kinds of target.
- **Directory change notifications** (`FindFirstChangeNotification`): the
  kernel signals a program's event when a directory or its subtree
  changes.
- **Compatibility fixes found along the way**: x64 programs pass
  `HKEY_LOCAL_MACHINE` sign-extended (`0xFFFFFFFF80000002`);
  `SetWindowPlacement` must show a hidden window; `GetWindowPlacement`
  gives a child's position in its parent; `FindFirstFile` was dropping the
  file times; `GetDIBits` was dropping the alpha channel; static controls
  without word wrap still break lines.  `mpr.dll` (`WNet*`: no network),
  the `Lsa*` policy functions and `OpenFileMapping` were missing.
  Crashes now name the DLL and offset, a process's exit code is logged,
  `OutputDebugString` reaches the kernel log, and the Terminal's `trace
  NAME` prints a program's failing system calls.
- **Test programs**: `guitest.exe` (menus, accelerators, a resource
  dialog with combos, radios, checks and a list box, message boxes, a
  property sheet with a form page and a tree view page), `droptest.exe`,
  `shmtest.exe`, `ftprobe.exe` (what `FindFirstFile` reports).  Real
  programs are tested from a second disk image holding 7-Zip, Git, CMake,
  Ninja, Neovim, Notepad++ and others, driven by a QEMU harness that types
  Terminal commands, clicks, drags and takes screenshots.
- Not yet: the OLE clipboard, and drags from 7-Zip's own file manager
  onto other programs are untested.  (Pipes and `cmd.exe`: see below.)

### The App Store

The dock's **App Store** (`kernel/apps/store.c`, also `start store` in the
Terminal) is a catalog of free and open-source Windows programs: 7-Zip,
VLC, Firefox, Thunderbird, Notepad++, GIMP, Inkscape, Krita, Audacity,
HandBrake, OBS Studio, LibreOffice, SumatraPDF, KeePassXC, qBittorrent,
PuTTY, WinSCP, Git, Python, WinMerge and ShareX, plus a **Runtimes**
category (.NET Desktop Runtime, Visual C++ Redistributable, OpenJDK,
Mesa 3D), by category, with a note on how far each gets on NovaOS today.

- **64-bit packages where there are any.**  The store fetches each
  project's official 64-bit package: a portable `.zip` or `.7z`, an
  installer, a Windows Installer `.msi`, or, for Firefox and Thunderbird,
  the full installer, which is a 7-Zip self-extracting archive.  Apps
  whose only download is a 32-bit setup program (GIMP, qBittorrent,
  WinSCP, the Visual C++ Redistributable) now install through it too,
  since NovaOS runs 32-bit programs.
- **Get** downloads over HTTPS (following redirects, with the bytes
  received shown while it runs) to `C:\Downloads`, using the same network
  operations as the Terminal's `wget`.  Files may be up to 256 MB.
- **Install** depends on the package: archives are unpacked into
  `C:\Programs\<App>` by the installed 7-Zip's own `7z.exe`, unchanged
  (the store waits for it and reports its result, and asks for 7-Zip
  first if it is missing); `.msi` packages go to NovaOS's Windows
  Installer; installers, 64-bit or 32-bit, run.
  **Run** starts portable programs (PuTTY, SumatraPDF) from Downloads.
- **Open** starts the installed program from its own folder (console
  programs such as Python and Git in a Terminal); archives with a
  versioned top folder are found wherever the program landed.  The
  **Installed** view lists what is there; runtimes show "Installed".
- Tested in QEMU with stand-in packages (the sandbox this was built in
  cannot reach the publishers): 7-Zip installed from its x64 installer;
  a portable zip unpacked into `C:\Programs\Notepad++` and opened; a
  7-Zip self-extracting installer unpacked like Firefox's and opened.
  The apps themselves mostly need more of Windows than NovaOS has (the
  note on each row).
- Programs in a folder of their own under `C:\Programs` now start in
  that folder when opened from the desktop (they used to be looked up
  by name and not found).
- The Terminal gained `copy <source> <destination>`.
- A kernel bug the store shook out: `ksnprintf` looped forever when a
  `%s` argument had to be cut to fit the buffer, freezing the desktop on
  a long error message.

### Windows Installer (.msi packages)

NovaOS has its own Windows Installer: `msi.dll` (`userland/msi/`) and
`msiexec.exe` in `C:\Windows\System32`.  Double-clicking a `.msi` in
Explorer, `start package.msi` in the Terminal, and the App Store's
Install button all go through it, and programs can call
`MsiInstallProduct`, `MsiConfigureProduct`, `MsiQueryProductState`,
`MsiGetProductInfo` and `MsiEnumProducts`.

- **The package format**: the OLE compound file container (FAT, mini
  FAT, DIFAT, the encoded stream names), the string pool and the
  column-major table streams, and the cabinets inside (or beside) the
  package with MSZIP (deflate) and LZX decompression, files spanning
  data blocks and cabinets.  These readers use only the C library and
  are tested on the host against packages built with msitools.
- **The engine** runs `InstallExecuteSequence`: launch conditions,
  `AppSearch`/`RegLocator`, feature and component selection (levels,
  the Condition table, `ADDLOCAL`/`REMOVE`, component conditions),
  Directory resolution onto NovaOS's folders (`ProgramFilesFolder` is
  `C:\Programs`, `SystemFolder` is `C:\Windows\System32`, ...),
  `CreateFolders`, `InstallFiles`, the Registry table (all value
  types, `[Property]`, `[#File]` and `[$Component]` formatting),
  `FindRelatedProducts`/`RemoveExistingProducts` through the Upgrade
  table, and product registration under the Uninstall key with a
  cached copy of the package in `C:\Windows\Installer`.  Custom actions
  that set properties or directories (types 51 and 35) run; ones that
  execute code are logged and skipped.  `msiexec /x` reverses it all,
  on the folder chosen at install time.
- **msiexec** takes `/i`, `/x` (a package or a `{ProductCode}`), `/qn`,
  `/qb`, `/passive`, `/l*v FILE` and `PROPERTY=value` overrides, and
  shows the familiar progress window with Cancel (full UI adds the
  completion message box).  Logs also go to the kernel log (`dmesg`).
- Not yet: the packages' own dialogs (`InstallUISequence`), the
  `Shortcut` table, services, environment variables, and
  merge modules.  LZX decoding is written to the specification but has
  only been exercised with MSZIP cabinets so far.

### Phase 13 — 32-bit Windows programs (WoW64)

NovaOS runs 32-bit (x86, PE32) Windows programs next to 64-bit ones, the
way 64-bit Windows does: the CPU runs them in compatibility mode under
the 64-bit kernel, and they get a 32-bit copy of the whole userland in
`C:\Windows\SysWOW64`.

- **Kernel** (`kernel/um/`): a 32-bit user code segment (0x38) and a flat
  4 GiB data segment; FS based at each thread's 32-bit TEB (switched with
  the thread); a 32-bit address layout, everything below 2 GiB (PEB, TEBs,
  loader list, stacks, heap and DLLs); the PE32 loader (4-byte import
  thunks, HIGHLOW relocations); x86 PEB, TEB and process parameters; DLLs
  looked up in SysWOW64, and file system redirection (a 32-bit program's
  `C:\Windows\System32` is `SysWOW64`, unless it calls
  `Wow64DisableWow64FsRedirection`).  32-bit code enters the kernel
  through `int 0x2E` with a block of 64-bit arguments.  A crash at a bad
  address now also names the caller in the log.
- **The 32-bit ntdll** (`ntdll_wow.c`) turns every system call's
  arguments into the kernel's 64-bit forms: handles sign-extended,
  pointers zero-extended, `OBJECT_ATTRIBUTES`, `UNICODE_STRING`,
  `IO_STATUS_BLOCK`, `CONTEXT`, `EXCEPTION_RECORD`, memory and
  thread/process information, handle arrays and window messages
  rebuilt in their 64-bit layout and the results copied back.
- **x86 exceptions** (`exc_x86.h`): frames chained from `fs:[0]`,
  `RtlUnwind`, `RtlRaiseException`, and the `__try` handlers of MSVC and
  clang (`_except_handler3`, `_except_handler4_common`); vcruntime140
  has x86 C++ exceptions (`__CxxFrameHandler3`, `_CxxThrowException`)
  and RTTI with absolute addresses; msvcrt has x86 `setjmp`/`longjmp`.
- **Build**: `tools/build_userland.py` builds the userland twice, the
  second time with `--target=i686-pc-windows-msvc`.  Stdcall functions are
  exported undecorated (`GetLastError`, not `_GetLastError@0`) through a
  generated `.def`, which also caught every mismatched calling convention
  between our DLLs at link time.  The 64-bit division helpers x86 code
  calls are in `lib/x86rt.c`.  32-bit builds of the test programs are in
  `C:\Programs\x86`; `NOVA_NO_WOW64=1` leaves the 32-bit pass out.
- **Programs see WoW64**: `IsWow64Process` is TRUE, `GetSystemInfo`
  reports an x86 machine and `GetNativeSystemInfo` the AMD64 one,
  `GetSystemDirectory` is `SysWOW64`.
- **Tested**: every self-test (crttest, filetest, threads/SEH, DLL/TLS,
  posixtest, apitest, comtest, C++ exceptions, shared memory) passes as a
  32-bit program as well as a 64-bit one; 32-bit GUI programs (winhello,
  guitest); programs from the 32-bit MinGW toolchain (C, and C++ with
  exceptions); 7-Zip's own 32-bit self-extractors, console and GUI,
  unpacking an archive; and a real NSIS (Modern UI) installer going
  through its welcome, folder, progress and finish pages, installing
  files, its uninstaller and registry keys, and uninstalling again.
- Pointer-size assumptions fixed on the way: TEB offsets in kernel32 and
  ws2_32, PE data directories in `GetProcAddress` and resources,
  `Get/SetWindowLongPtr` and the `DWLP_*` offsets, rename information,
  SRW locks, `%p`/`%z`/`%I` in printf and scanf, `FILE` (32 bytes on x86).
- Also fixed on the way (for 64-bit programs too): `EndDialog` called
  from a message another thread sent now ends the modal loop (NSIS's
  finish page); `MoveFileEx(..., MOVEFILE_DELAY_UNTIL_REBOOT)` records
  the operation in `PendingFileRenameOperations` instead of acting at
  once (NSIS uninstallers copy themselves to Temp and schedule that copy
  for deletion; it used to vanish before it could run); `shfolder.dll`
  exists (`SHGetFolderPath`, which NSIS takes from it); msvcrt exports
  `_controlfp`, `_control87`, `__p___initenv` and friends.  The
  Terminal's `trace` now shows the file name of file system calls.
- Not yet: pending renames are not carried out at the next start.

### Shortcuts (.lnk) and overlapping controls

- **`IShellLink`** (`userland/shell32/shlink.c`): shell32's ShellLink
  class, `CLSID_ShellLink`, registered under `HKCR\CLSID` (a bare
  `shell32.dll`, so 32-bit and 64-bit programs each get their own), with
  `IShellLinkW`, `IShellLinkA` and `IPersistFile`.  `Save` writes the
  Windows `.lnk` format (MS-SHLLINK: header, LinkInfo with the target in
  ANSI and Unicode, Unicode strings for the description, working folder,
  arguments and icon); `Load` reads links made by Windows too.
- **The shell uses them**: opening a `.lnk` (Explorer, the desktop, the
  Start menu, `start` in the Terminal) runs its target with the
  shortcut's arguments in its working folder, or opens the folder or
  document it points to; a shortcut shows its target's icon and is
  called "Shortcut" in Explorer.  The Terminal's `start` opens any
  document, folder or shortcut the way the shell does.  The Start menu
  lists the shortcuts in `C:\AppData\Roaming\Start Menu\Programs`
  (where `$SMPROGRAMS` and `CSIDL_PROGRAMS` point) ahead of
  `C:\Programs`, keeping "Uninstall ..."
  entries out of the grid (search finds them), and the desktop shows
  what is in `C:\Desktop` after its own icons, noticing new files within
  half a second.
- **Overlapping controls paint as on Windows**: siblings paint from the
  top of the z-order down (the first control of a dialog first), so where
  controls without `WS_CLIPSIBLINGS` overlap, the lower one's pixels end
  on top.  NSIS's page header is a white static above its bold title,
  subtitle and icon; they were hidden under it and now show, and the
  welcome and finish pages cover the header as they should.
- Tested with a real NSIS installer that makes a desktop shortcut (with
  arguments) and two Start menu shortcuts: the desktop icon appears, a
  double-click starts the 32-bit program with its arguments in its
  folder, the Start menu lists it, and the uninstaller takes them away.
  comtest round-trips a shortcut through `IShellLinkW`, `IPersistFile`
  and `IShellLinkA` in both 64-bit and 32-bit builds.

### Pipes and cmd.exe

- **Pipes in the kernel** (`kernel/um/um_pipe.c`): named pipes
  (`NtCreateNamedPipeFile`, `\\.\pipe\NAME` opened with `CreateFile`) and
  anonymous ones (`CreatePipe` makes a named pipe with a private name, as
  Windows does).  Each direction is a ring buffer; byte and message pipes
  (a reader in message mode gets one message per read, the rest with
  `ERROR_MORE_DATA`), `ConnectNamedPipe`, `DisconnectNamedPipe`,
  `WaitNamedPipe`, `PeekNamedPipe`, `TransactNamedPipe`/`CallNamedPipe`,
  `GetNamedPipeInfo`, `Set/GetNamedPipeHandleState` (message read mode,
  `PIPE_NOWAIT`) and instance limits.  The ends are kernel objects, so
  closing the last handle to one is what the other side sees: a reader
  gets what is left and then `ERROR_BROKEN_PIPE` (end of file), a writer
  `ERROR_NO_DATA`.
- **Overlapped I/O that really waits**: on a handle opened with
  `FILE_FLAG_OVERLAPPED` a read, write or `ConnectNamedPipe` that cannot
  finish stays pending (`ERROR_IO_PENDING`); whoever changes the pipe later
  finishes it, writing the data and the status into the waiting program
  and setting its event.  Completion ports get their packet and
  `ReadFileEx`/`WriteFileEx` routines run at the next alertable wait
  (through a kernel32 helper thread), `CancelIo`/`CancelIoEx` end requests
  with `ERROR_OPERATION_ABORTED`, and `GetOverlappedResult(Ex)` waits for
  them.  A 32-bit program's I/O status block goes to the kernel as its own
  (32-bit layout) so it can be finished later.
- **Handles for child processes**: handles carry an inherit flag
  (`SECURITY_ATTRIBUTES.bInheritHandle`, `SetHandleInformation`,
  `DuplicateHandle(..., TRUE, ...)`), and `CreateProcess(..., TRUE, ...)`
  gives the child every inheritable handle at the same value; the
  standard handles come from `STARTUPINFO` or, without
  `STARTF_USESTDHANDLES`, from the parent's own (so a program's children
  write where it writes).  `DuplicateHandle` works into and out of another
  process.  `DETACHED_PROCESS`/`CREATE_NO_WINDOW` start without a console.
- **The environment** is one sorted table in kernel32 that `SetEnvironmentVariable`,
  `GetEnvironmentStrings(A/W)` and `CreateProcess` all see; a child gets
  its parent's environment (or `lpEnvironment`, ANSI or Unicode) instead
  of a fixed default.  `ComSpec` names cmd.exe.
- **`NUL`** is the null device (`>nul`, `CreateFile("nul")` in any folder).
- **C runtime**: `system()`/`_wsystem()` run `cmd.exe /c`, `_popen`/
  `_pclose`/`_wpopen` read or write a command through a pipe, `_pipe`
  makes one, and reading a pipe whose writer is gone is end of file.
- **cmd.exe** (`userland/programs/cmd.c`, in System32 and SysWOW64): the
  command interpreter, interactive or `/c`/`/k`.  Lines are expanded
  (`%VAR%`, `%VAR:~1,2%`, `%VAR:a=b%`, `%ERRORLEVEL%`, `%CD%`, `%DATE%`,
  `%RANDOM%`; `%0`-`%9`, `%*` and `%~dpnxfatz0` in batch files; `!VAR!`
  with delayed expansion), then parsed: `&`, `&&`, `||`, `|`, `( )`
  blocks, `^` escapes and redirections (`<`, `>`, `>>`, `2>`, `2>&1`,
  `>nul`).  IF (`==`, `/i`, `not`, `EQU`...`GEQ`, `errorlevel`, `exist`,
  `defined`) with ELSE; FOR, `/d`, `/r`, `/l` and `/f` over files,
  strings and command output (`tokens=`, `delims=`, `skip=`, `eol=`,
  `usebackq`).  Batch files: labels, GOTO, CALL (files and `:labels`),
  SHIFT, SETLOCAL/ENDLOCAL (with `enabledelayedexpansion`), `exit /b`,
  ECHO ON/OFF and `@`.  Internal commands: echo, set (`/a` arithmetic,
  `/p` input), cd, dir (`/b`, `/s`, `/a`), type, copy, del, md, rd `/s`,
  ren, move, pushd/popd, path, prompt, start, pause, title, ver and more.
  Programs run with the redirected handles; each side of a pipe that is
  not a program runs in a child cmd.exe, as on Windows.  Also new in
  System32: `find`, `findstr` (its regular expressions), `sort`, `more`
  and `timeout`.
- **The Terminal hands lines to cmd.exe** when they use pipes,
  redirections or `&&`/`||`, and when they name a `.bat`/`.cmd` file;
  `cmd` starts it interactively.  Ctrl+C now stops every program on the
  console, not just the first.
- Tests: `pipetest.exe` (62 checks, 64-bit and 32-bit): anonymous pipes
  and end of file, a 300 KB write through a 4 KB pipe, children reading
  and writing redirected pipes, inherited handles by value, the
  environment, `cmd /c`, a pipeline into a program, `_popen`, `system`,
  `_pipe`, NUL, named pipes in byte and message mode, overlapped connect
  and reads with events, cancelling, a completion port and a completion
  routine.  `cmdtest.bat` (29 checks) covers expansion, SET /A, IF, the
  FOR forms, CALL, delayed expansion, pipes into `find` and `sort`,
  redirections and error levels.
- Not yet: MinGit's `git.exe` loops at start-up (it did before this work
  too); `CREATE_SUSPENDED` is ignored and `CREATE_NEW_CONSOLE` shares the
  console; a file handed to a child has its own position (cmd.exe opens
  redirection targets for appending so output lands in order).

### Installing NovaOS on a disk

The ISO is also the installation disc.  Booted from it, NovaOS runs
live (nothing is kept after a restart) and opens **Install NovaOS**
(`kernel/apps/setup.c`, the engine in `kernel/fs/setup.c`); an icon on
the desktop brings it back.  On an installed system it is in the Start
menu and `start setup` in the Terminal opens it, to copy NovaOS to
another disk.

- **Welcome, choose a disk, confirm, install, finish.**  Setup lists the
  SATA disks with their size and what is on them, marks the one NovaOS
  started from and the one drive C: is kept on, and asks before erasing
  anything.  Disks under 256 MB are shown but cannot be picked.
- **What it writes**: a GPT (protective MBR, primary and backup headers
  and entry arrays with their CRCs) with two partitions: an EFI System
  Partition (FAT32 `NOVA_EFI`, 128 MiB) holding `\EFI\BOOT\BOOTX64.EFI`
  and `\EFI\NOVA\kernel.elf`, and a basic data partition (FAT32
  `NOVADATA`, the rest of the disk) where drive C: is saved.  The copied
  kernel is read back and compared.  UEFI firmware finds
  `\EFI\BOOT\BOOTX64.EFI` by itself, so no boot entry is written.
- **Where the files come from**: booted from the disc, the bootloader
  sees a CD-ROM node in its device path and hands the kernel the two
  boot files in memory (boot protocol v3).  On an installed system Setup
  reads them from the disk NovaOS started from.
- **Your session comes along**: from the disc, drive C: lives on a blank
  disk that NovaOS formatted at boot, often the very disk being
  installed on.  Setup stops saving there before erasing it, then
  writes everything on C: to the new data partition and keeps saving
  there.  A C: kept on another disk stays where it is.
- **Restart now** reboots into the installed system (remove the disc
  first, or pick the disk in the firmware's boot menu).
- Tested in QEMU/OVMF: installing from the ISO onto a blank 1 GB disk,
  booting that disk alone with a file made in the live session still in
  Documents, and installing from the installed system onto a second
  disk.  Both new FAT volumes pass `fsck.fat`, and the GPT's checksums
  verify.
- To try it:

```bash
truncate -s 1G disk.img
cp /usr/share/OVMF/OVMF_VARS_4M.fd /tmp/OVMF_VARS.fd
qemu-system-x86_64 -machine q35 -m 2G -smp 4 \
  -drive if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,unit=1,file=/tmp/OVMF_VARS.fd \
  -drive file=disk.img,format=raw -cdrom nova.iso
# after installing: the same command without -cdrom starts from disk.img
```

## Quick Start

```bash
# Install prerequisites
sudo apt install cmake nasm clang lld llvm qemu-system-x86 ovmf mtools dosfstools xorriso

# Build
mkdir build && cd build
cmake ..
make -j$(nproc)

# Run
cmake --build . --target run
```

`run` attaches a second disk, `build/nova-data.img`, where NovaOS keeps
drive C: across restarts and rebuilds (see
[docs/building.md](docs/building.md#where-your-files-are-kept)).

The first build compiles the NetSurf browser (about 800 files; later builds
reuse the cached objects). Set `NOVA_NO_NETSURF=1` in the environment to
leave the browser out.

### Bootable ISO

A ready-to-boot UEFI ISO (`nova.iso`) is committed at the repository root and
can be regenerated from a freshly built bootloader + kernel:

```bash
scripts/create-iso.sh nova.iso build/bootx64.efi build/kernel.elf
```

The ISO is El Torito **UEFI / no-emulation**: its EFI System Partition holds
`\EFI\BOOT\BOOTX64.EFI` (the bootloader) and `\EFI\NOVA\kernel.elf`. Boot it in
QEMU with OVMF firmware:

```bash
cp /usr/share/OVMF/OVMF_VARS_4M.fd /tmp/OVMF_VARS.fd
qemu-system-x86_64 -machine q35 -m 2G -smp 4 \
  -drive if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,unit=1,file=/tmp/OVMF_VARS.fd \
  -cdrom nova.iso
```

It boots through every phase to the desktop on the GOP framebuffer (verified
under OVMF at 2560×1600).  The ISO carries the whole userland (the system
DLLs, the test programs, NetSurf, `msiexec.exe`) and the App Store; give
the machine 2 GB so downloaded installers fit in the RAM disk.  Add a
second drive (`-drive file=disk.img,format=raw`) to keep drive C: and the
registry between boots.

The ISO is also the **installation disc**: booted from it, NovaOS runs
live and opens Install NovaOS, which puts it on a disk (see "Installing
NovaOS on a disk" above).  An empty disk attached to the live session is
formatted for drive C: at boot, and Setup can install onto that same
disk, taking the session's files along.

On macOS with Homebrew QEMU, the UEFI firmware ships with QEMU:

```bash
FW="$(brew --prefix qemu)/share/qemu/edk2-x86_64-code.fd"
qemu-system-x86_64 -machine q35 -m 2G -smp 4 \
  -drive if=pflash,format=raw,readonly=on,file="$FW" \
  -cdrom nova.iso -serial stdio
```

## Phase Roadmap

| Phase | Focus | Status |
|-------|-------|--------|
| 1 | Boot & kernel foundation | ✅ **Done** |
| 2 | NT kernel personality (Object Manager, syscalls, registry) | ✅ **Done** |
| 3 | I/O subsystem (IRP, PE loader, section objects) | ✅ **Done** |
| 4 | VFS + InitRD (virtual file system, in-memory ramdisk) | ✅ **Done** |
| 5 | Process isolation (per-process CR3, KPCR, PEB/TEB, SWAPGS) | ✅ **Done** |
| 6 | Full user-mode (SYSCALL thunk pages, Win32 helpers, CSRSS) | ✅ **Done** |
| 7 | GUI (window manager, GDI, desktop shell) | ✅ **Done** |
| 8 | Interactive desktop (PS/2 input, cursor, preemptive sched, RTC) | ✅ **Done** |
| 8.5 | Networking (e1000/e1000e, lwIP, DHCP, DNS, HTTP tools) | ✅ **Done** |
| 8.6 | HTTPS (Mbed TLS: TLS 1.3/1.2, Mozilla roots, certutil), HTTP/1.1 keep-alive | ✅ **Done** |
| 9 | Native user-mode PE execution: loader, TEB/PEB, threads, TLS, SEH, DllMain, sockets (ws2_32), GUI (user32/gdi32) | ✅ **Done** |
| 9.5 | NetSurf web browser (HTTP/HTTPS fetcher, window surface, TrueType text, POSIX C runtime) | ✅ **Done** |
| 10 | Standard DLLs (UCRT, C++ EH, advapi32, shell32, ...), registry, COM, AHCI + FAT persistent storage | ✅ **Done** |
| 11 | Multiprocessor: every core runs threads, per-core scheduling, fine-grained kernel locking | ✅ **Done** |
| 12 | Win32 GUI subsystem (real HWNDs, controls, menus, dialogs, comctl32, drag and drop); unmodified 7-Zip installs and runs; the App Store; Windows Installer (.msi); installing NovaOS on a disk | ✅ **Done** |
| 13 | 32-bit (x86) Windows programs (WoW64): compatibility mode, a SysWOW64 userland, x86 SEH and C++ exceptions; NSIS installers (with shortcuts) and 7-Zip's 32-bit self-extractors run | ✅ **Done** |
| 14 | Pipes (named, anonymous, overlapped), handle inheritance, `cmd.exe` with batch files, the OLE clipboard, more real programs | 🔄 In progress (pipes and `cmd.exe` done) |

See [`docs/ROADMAP.md`](docs/ROADMAP.md) for the full plan toward running native
Windows executables (Phases 8–15) and the chosen compatibility strategy.

## Architecture

```
os/
├── include/              # Shared headers (boot_protocol.h, types.h)
├── bootloader/           # UEFI bootloader (PE32+ EFI application)
│   ├── include/efi.h     # Minimal UEFI API headers
│   └── src/              # main.c, elf_loader.c, paging.c, console.c
├── kernel/               # NT-style kernel (freestanding ELF64)
│   ├── arch/x86_64/      # GDT, IDT, APIC, paging, CPU intrinsics
│   ├── mm/               # PMM (bitmap), VMM (slab), VMA, section objects
│   ├── hal/              # Serial, framebuffer, PS/2 input, CMOS RTC
│   ├── ke/               # Kernel executive: main, scheduler, printf, KPCR, syscall
│   ├── ob/               # Object Manager (handles, reference counting)
│   ├── ps/               # Process Manager (EPROCESS, ETHREAD, PEB, TEB, CSRSS)
│   ├── se/               # Security Reference Monitor
│   ├── cm/               # Configuration Manager (registry)
│   ├── io/               # I/O Manager (IRP, device objects, file objects)
│   ├── fs/               # VFS, InitRD, RAM disk (C:), block devices, FAT16/32,
│   │                     #   saving C: to disk (persist.c)
│   ├── drivers/          # e1000/e1000e network, AHCI (SATA) disks
│   ├── ldr/              # PE32+ loader + user-mode SYSCALL thunk generator
│   ├── gdi/              # GDI software renderer (2D rasterizer)
│   ├── wm/               # Window manager + desktop shell
│   ├── um/               # User-mode programs: processes, threads, PE loader, NT services,
│   │                     #   SEH delivery, sockets, GUI windows, consoles
│   ├── net/              # lwIP port, HTTP client, TLS (Mbed TLS)
│   ├── apps/             # Built-in desktop apps (Terminal, Notepad, Settings, ...)
│   ├── lib/              # Freestanding string/memory library
│   └── linker.ld         # Kernel linker script
├── userland/             # Windows SDK subset built with clang/lld-link:
│   │                     #   ntdll, kernel32, msvcrt/ucrtbase, vcruntime140,
│   │                     #   advapi32, bcrypt, ws2_32, user32, gdi32, shell32,
│   │                     #   shlwapi, ole32, oleaut32 and more; crt0, headers,
│   │                     #   sample and test programs
│   └── netsurf/          # NetSurf port: fetcher, window surface, fonts, JPEG
├── third_party/          # lwIP, Mbed TLS, musl (libm), NetSurf + libraries, stb, fonts
├── tools/                # Host build tools (build_userland.py, build_netsurf.py, mkfont)
├── cmake/                # Cross-compilation toolchain files
├── scripts/              # build.sh, run-qemu.sh, create-disk.sh
└── docs/                 # Architecture & build documentation
```

## Key Design Decisions

- **NT ABI compatibility**: syscall numbers, NTSTATUS codes, UNICODE_STRING, and structure
  offsets match Windows 10 1903 x64 so that real ntdll.dll stubs can be used unmodified.
- **Clean-room**: no GPL code in the operating system itself; all NT API
  implementations are written from scratch using public Microsoft documentation
  and reverse-engineering references.  The one GPL component is the NetSurf
  browser, a separate program (see [License](#license)).
- **SMP with a shrinking big kernel lock (Phase 11)**: every CPU runs threads,
  one KPCR and one ready queue per CPU.  The scheduler, memory, synchronization, sockets and the
  GUI have their own locks and run on all CPUs at once; the rest of the
  kernel still runs under the big lock, one CPU at a time (see `smp.h` for
  the rules and the lock order).
- **Software-only GDI**: Phase 7 rendering is a pure CPU rasterizer writing the GOP
  linear framebuffer — no GPU/2D-accel driver. It draws directly to VRAM (no back
  buffer yet); double-buffering arrives once a second VRAM mapping exists.
- **No image decoder yet**: application icons in the shell are rendered as rounded,
  brand-colored tiles with short labels — a faithful stand-in until a PNG/ICO
  decoder lands. App tiles, live tiles, and the dock are drawn from primitives.
- **Kernel-helper syscalls (0x01F0–0x01FF)**: reserved range at the top of the 512-entry
  NT dispatch table; used only internally by our user-mode stub pages — not part of the
  Windows NT ABI and invisible to real Windows binaries.
- **CSRSS Phase 6 scope**: the kernel-side shim accepts process registrations and
  returns STATUS_SUCCESS to all `CsrClientCallServer` calls; real LPC port objects
  and console server come in Phase 8.
- **Page table isolation**: each user process has its own PML4; kernel entries (256–511)
  are shared; user entries (0–255) are snapshotted at process creation.

## License

NovaOS is MIT licensed. The operating system (kernel, bootloader, system
DLLs, C runtime, desktop and apps) contains no GPL code; bundled third-party
code keeps its own permissive licence (lwIP: BSD 3-clause; Mbed TLS:
Apache-2.0; musl's libm: MIT; Inter and Cascadia Mono: SIL OFL 1.1; DejaVu Sans Mono:
Bitstream Vera licence; stb_truetype/stb_image: public domain or MIT).
All Win32 API implementations are clean-room based on public Microsoft
documentation, ReactOS reference, and Wine source study (but independently
written).

**NetSurf** (`third_party/netsurf/netsurf`) is licensed under the **GNU GPL
version 2**; its libraries are MIT, zlib and libpng licensed (see
`third_party/netsurf/NOVA-VENDOR.txt`).  `netsurf.exe` — NetSurf linked with
the NovaOS glue in `userland/netsurf` (MIT, GPL-compatible) — is a separate
program distributed under the GPL-2.0, with its complete source in this
repository; the kernel image merely carries it as a file for drive C:.
Build with `NOVA_NO_NETSURF=1` for an image without it.
