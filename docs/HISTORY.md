# NovaOS — Feature History, Phase by Phase
<!-- The regions between "BEGIN generated" and "END generated" markers are built from fragment files by tools/docgen.py: edit those files, not the regions (CONTRIBUTING.md). -->

This is the detailed record of what each phase of NovaOS added, in the
order it landed.  The [README](../README.md) has the short version: what
runs today, how to build it, and where the project is going.

Each section describes the system as that phase left it.  Where a "Not
yet" item was done by a later phase, a note says which; everything else
under "Not yet" is still open (see [ROADMAP.md](ROADMAP.md)).

Contents:
[1 Boot](#phase-1--boot--kernel-foundation) ·
[2 NT kernel](#phase-2--nt-kernel-personality) ·
[3 I/O and PE loader](#phase-3--io-sections-and-pe-loader) ·
[4 VFS](#phase-4--vfs--initrd) ·
[5 Isolation](#phase-5--process-isolation--user-mode-foundation) ·
[6 User mode](#phase-6--full-user-mode-foundation) ·
[7 GUI](#phase-7--gui-window-manager--desktop-shell) ·
[8 Desktop](#phase-8--interactive-desktop) ·
[8 Networking](#phase-8-cont--networking) ·
[9 Windows programs](#phase-9--windows-programs-ring-3) ·
[9.5 NetSurf](#phase-95--the-netsurf-web-browser) ·
[Desktop UX](#desktop-ux-refresh) ·
[10 DLLs, registry, COM, storage](#phase-10--standard-dlls-registry-com-and-persistent-storage) ·
[11 SMP](#phase-11--multiprocessor-smp) ·
[12 7-Zip](#phase-12--windows-gui-programs-7-zip-unmodified) ·
[App Store](#the-app-store) ·
[Windows Installer](#windows-installer-msi-packages) ·
[13 WoW64](#phase-13--32-bit-windows-programs-wow64) ·
[Shortcuts](#shortcuts-lnk-and-overlapping-controls) ·
[Pipes and cmd.exe](#pipes-and-cmdexe) ·
[Clipboard](#the-clipboard) ·
[Git](#git) ·
[MSYS2](#the-msys2-runtime-shexe-clone-push) ·
[Runtimes](#language-runtimes-java-net-nodejs-python) ·
[Installing NovaOS](#installing-novaos-on-a-disk) ·
[Sound](#sound-intel-hd-audio-winmm-and-wasapi)

<!-- BEGIN generated:history -->

## Phase 1 — Boot & Kernel Foundation
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

## Phase 2 — NT Kernel Personality
- Object Manager (`ObCreateObject`, `ObReferenceObject`, `ObDereferenceObject`, handle tables)
- Security Reference Monitor (`SeInitialize`, `SeCreateSystemToken`, ACL/ACE stubs)
- Process Manager (`EPROCESS`/`ETHREAD`/`KPROCESS`/`KTHREAD`, `PsCreateSystemProcess`, `PsCreateSystemThread`)
- Configuration Manager (registry hive bootstrap, `CmInitialize`)
- Syscall dispatcher: INT 0x2E (legacy) + SYSCALL/SYSRET MSR path (`KiSystemCall64`)
- NT syscall table with ~40 entries (NtAllocateVirtualMemory, NtCreateFile, etc.)

## Phase 3 — I/O, Sections, and PE Loader
- I/O Manager (`IoCreateDevice`, `IoCreateFile`, IRP dispatch: Create/Read/Write/QueryInfo)
- Virtual Memory Areas (`VMA_SPACE`, `VmaAllocate`, `VmaFree`, `VmaProtect` per-process)
- Section Objects (`MmCreateImageSection`, `MmMapImageView`, anonymous sections)
- PE32+ Loader (`LdrLoadImage`): header validation, section mapping, base relocation, import resolution
- Built-in stub DLLs: ntdll, kernel32, msvcrt, user32 (syscall thunks for import resolution)
- User-mode process creation (`PsCreateUserProcess` → IRETQ into ring-3 via `PsUserThreadEntry`)

## Phase 4 — VFS & InitRD
- VFS abstraction layer (`VfsMount`, `VfsOpen`, `VfsRead`, `VfsReadDir`, `VfsStat`)
- NT path alias resolution (`\??\C:\`, `\SystemRoot`, `\DosDevices\C:` → first mount)
- CPIO newc InitRD driver (`InitrdMount`): parses in-memory CPIO archive, mounts as `\Device\InitRD`
- Boot protocol v2: bootloader passes `initrd_base`/`initrd_size` in `BootInfo`
- File I/O via IRP dispatch: `NtReadFile`, `NtWriteFile` fully wired through VFS to InitRD

## Phase 5 — Process Isolation & User-Mode Foundation
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

## Phase 6 — Full User-Mode Foundation
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

## Phase 7 — GUI, Window Manager & Desktop Shell
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

## Phase 8 — Interactive Desktop
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

## Phase 8 (cont.) — Graphics & Working Desktop
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
  there is no disk, and there are no image icons.  *(Since done: Windows
  programs in Phase 9, a disk in Phase 10, icons in the Desktop UX
  refresh.)*

## Phase 8 (cont.) — Networking
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
- Not yet: IPv6 and HTTP/2.  (The web browser arrived in Phase 9.5; IPv6
  and HTTP/2 came with "IPv6, HTTP/2 and virtio-net (Phase 18.8)".)

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

## Phase 9.5 — The NetSurf web browser
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
  message queue.  The browser window can be resized from any edge,
  maximized, snapped to half the screen and restored: `WM_SIZE` becomes a
  libnsfb resize event, the toolbar, scroll bars and status bar move, and
  the page is laid out again for the new width.
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
- Not yet: SVG and IPv6.  *(IPv6 came with "IPv6, HTTP/2 and virtio-net
  (Phase 18.8)".)*

## Desktop UX refresh
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
  shows its program's first icon), and animated cursors.  *(Phase 12's
  user32 has icons from `.ico` files and PE resources and `WM_SETICON`;
  animated cursors came with "Program pointers and animated cursors"
  below.)*

## Phase 10 — Standard DLLs, registry, COM and persistent storage
- **Unmodified Windows programs run**: stock release builds of ripgrep and
  fd (Rust, MSVC), jq (C, MinGW) and fzf (Go) work from the Terminal:
  searching, walking folders, filtering.  Copy an `.exe` onto the data disk
  (see [Where your files are kept](building.md#where-your-files-are-kept))
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
  and a clipboard shared between programs.  *(Dialogs, menus and controls
  came in Phase 12, the shared clipboard after Phase 13, audio with HD
  Audio, and type libraries and FH4 in "COM type libraries and FH4 C++
  exceptions" below; the rest is still open.)*

## Phase 11 — Multiprocessor (SMP)

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
  - the program loader holds `DesktopLock` only to look a module's file
    up: the file is then pinned (`RamfsPin`: its contents stay put, and
    writes to it fail, as Windows refuses writes to a mapped image) and
    copied, relocated, bound and committed without it, and without the
    big lock (`bkl_drop`; the work touches only the heap, the page
    allocator and the new image's address space); `LoadLibrary`
    serializes on a per-process loader lock instead.  Programs started from the Terminal,
    Explorer, `start` or the desktop are loaded on a worker thread
    (`UmSpawnStart`), so starting `node.exe` (90 MB) or loading CoreCLR
    no longer freezes the desktop for seconds.
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

## Phase 12 — Windows GUI programs: 7-Zip, unmodified

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
  has a source window and both kinds of target.  As on Windows the drop
  is synchronous: the dragging program's `DoDragDrop` returns only once
  the target has handled it, with the effect the target took, and no
  window keeps the mouse afterwards.  7-Zip's file manager depends on
  both: files dragged out of an archive are extracted to a temporary
  folder that 7-Zip deletes as soon as `DoDragDrop` returns, and its panel
  takes the mouse while it extracts.
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
- Drags from 7-Zip's file manager onto other programs work: one or more
  files from an archive or from a folder, onto a `WM_DROPFILES` window or
  an OLE drop target (tested with both `droptest` targets; each reports
  the dropped files' sizes, so a file that has already gone shows as
  missing).  (Pipes, `cmd.exe` and the clipboard: see below.)

## The App Store

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

## Windows Installer (.msi packages)

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
- Custom actions, the packages' own dialogs, shortcuts, services and LZX
  on real packages came later; see "Windows Installer depth" below.

## Phase 13 — 32-bit Windows programs (WoW64)

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
- Not yet: pending renames are not carried out at the next start.  *(Since
  done: Session Manager's `PendingFileRenameOperations` run at boot, see
  "Phase 17: kernel and API correctness".)*

## Shortcuts (.lnk) and overlapping controls

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

## Pipes and cmd.exe

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
- Not yet: `CREATE_SUSPENDED` is ignored and `CREATE_NEW_CONSOLE` shares
  the console; a file handed to a child has its own position (cmd.exe
  opens redirection targets for appending so output lands in order).
  *(Since done: `CREATE_SUSPENDED`, `CREATE_NEW_CONSOLE` windows and shared
  file positions came with "Phase 17: kernel and API correctness".)*

## The clipboard

- **One clipboard for everything** (`kernel/wm/clipboard.c`, reached by
  programs through `NtNovaClipboard`): each format is a copy of its
  bytes; `CF_TEXT`/`CF_OEMTEXT` and `CF_UNICODETEXT` are converted into
  each other on request (with `CF_LOCALE`); formats a program registers
  travel by name, since each program numbers them differently.
- **user32** (`OpenClipboard` ... `GetClipboardData`,
  `EnumClipboardFormats`, `GetClipboardSequenceNumber`,
  `GetPriorityClipboardFormat`) now uses it, so text, files (`CF_HDROP`)
  and private formats copied in one program paste in another.
  `CF_BITMAP` travels as a `CF_DIB` and comes back as a bitmap; a format
  set with a NULL handle is rendered by its owner (`WM_RENDERFORMAT`) when
  the clipboard is closed.
- **The OLE clipboard** (`userland/ole32/clipbrd.c`): `OleSetClipboard`
  copies a data object's formats onto it, `OleGetClipboard` gives a data
  object that reads it (`GetData`, `QueryGetData`, `EnumFormatEtc`),
  `OleIsCurrentClipboard`, `OleFlushClipboard`.
- **The built-in apps**: Notepad selects (Shift with the arrows, Home,
  End, PgUp/PgDn; mouse drags; double-click for a word; Ctrl+A) and has
  Ctrl+C/X/V with Copy and Paste buttons.  The Terminal selects with a
  mouse drag (double-click: a word); Ctrl+C copies while something is
  selected (else it still interrupts), Ctrl+Shift+C always copies, and
  Ctrl+V, Shift+Insert or a right click paste into the prompt or the
  running program; the wheel scrolls.  File Explorer copies, cuts and
  pastes files and folders (Ctrl+C/X/V, Copy and Paste buttons) as
  `CF_HDROP` with a "Preferred DropEffect", so programs see them too.
- Tests: `cliptest.exe` (23 checks, 64-bit and 32-bit, each reading back
  in a second process): Unicode text read as `CF_TEXT` and the other way,
  a registered format by name, `CF_HDROP` with `DragQueryFile`, a bitmap
  as a 3x2 DIB and back, the sequence number, and the OLE clipboard.

## Git

MinGit's `git.exe` (2.47) runs: `--version`, `init`, `add`, `commit`,
`log`, `status`, `diff`, output into pipes (`git log | find`) and a pager
(`core.pager=more`).  What it needed from NovaOS:

- **A recursive loader lock**: a DLL's initialisation that itself loads
  a library (git's C runtime start-up does) spun forever on ntdll's
  loader lock; the lock now belongs to a thread and nests, as Windows'
  does, and a module being initialised is not initialised twice.
- **`OpenProcess` for other programs** (`NtOpenProcess`): git waits for
  its children by process id (`waitpid`); the handle shares the process's
  exit object, which keeps the exit code after the process is gone.
- **The C runtime's standard descriptors**: `dup2` onto 0-2 now moves the
  standard handle and `stdout`/`stderr` with it, and closing the last
  descriptor of a handle closes it even for 0-2, so a pager sees the end
  of its input.  New: `_spawn*`/`_exec*`/`_cwait`, `_flushall`, `_umask`,
  `_wchmod`, `_mktemp`/`_wmktemp`; `_vscprintf`, `_scprintf` and
  `_wfreopen` are exported.
- **The rest of its imports**: NUMA queries (`GetNumaHighestNodeNumber`,
  `GetNumaNodeProcessorMask`...), volume enumeration, `CreateRemoteThread`
  (this process), `PeekConsoleInput`, `GetSystemTimeAdjustment`,
  `NtSetEaFile`/`NtQueryEaFile`, `QueryServiceStatusEx`,
  `SetEntriesInAcl`, and Winsock's `WSAEventSelect`/`WSAEnumNetworkEvents`
  (a helper thread watches the sockets) and `getnameinfo`.
- **The whole MinGit layout**: the test disk carries MinGit unzipped as
  it is on Windows (`C:\Apps\MinGit` with `cmd`, `etc`, `mingw64` and
  `usr`).  Run as `C:\Apps\MinGit\cmd\git.exe` with nothing on `PATH`,
  git finds its templates (a new repository gets its sample hooks), its
  system `gitconfig` and its own helper programs; `checkout -b`, `merge`,
  `gc` (`pack-objects` and `repack` as child processes), `count-objects`
  and `fsck` work too.  That took two fixes:
  - **File names up to 255 characters** on drive C: (they were cut at 47);
    `gc` renames its pack to a 59-character temporary name.
  - **Opening a process that has exited but is still held**: `OpenProcess`
    now finds it while any handle keeps it, as on Windows, so `waitpid`
    on a finished child gets its exit code.
- Not yet: MinGit ships no `less`, git's default pager, so give `log` and
  `config --list` `--no-pager` or `-c core.pager=more`.  *(Since done:
  NovaOS's own `less`, see [A pager for git](#a-pager-for-git-lessexe).)*

## The MSYS2 runtime: `sh.exe`, `clone`, `push`

MinGit's shell and Unix tools (`usr\bin`: `sh.exe` is bash, `ls`, `cat`,
`wc`...) are MSYS2 programs, built on `msys-2.0.dll`, a fork of the
Cygwin runtime.  They run unmodified: `sh -c "..."` with pipes,
`$(...)`, subshells, globbing and redirection, starting Windows programs
and MSYS ones.  git starts `git-upload-pack` and `git-receive-pack`
through `sh`, so `git clone`, `fetch` and `push` between repositories on
C: work.  What the runtime needed from NovaOS:

- **Native API breadth**: object directories and symbolic links
  (`\BaseNamedObjects\...` with `RootDirectory`-relative names), timer
  objects, `NtQueryEvent`/`NtQuerySemaphore`, `NtOpenThread`,
  `NtRead/WriteVirtualMemory` of another process,
  `NtAllocateVirtualMemoryEx` and `NtMapViewOfSectionEx` with address
  requirements, `NtQueryObject` names (`\Device\HarddiskVolume1\...`,
  `\Device\NamedPipe\...`), more `NtQueryInformationFile`,
  `NtQueryDirectoryFile` and volume classes, tokens, SIDs, ACLs and
  security descriptors, LSA policy and account queries, and the
  `RtlGetCurrentDirectory_U` code shape the runtime searches for its
  `FAST_CWD` pointer.
- **Guard pages and stack growth**: `PAGE_GUARD` pages raise
  `STATUS_GUARD_PAGE_VIOLATION` once; on a thread's stack (from the TEB)
  the next page down becomes the guard and `StackLimit` follows, and new
  stack pages are read/write even when the reservation says
  `PAGE_NOACCESS`.  The runtime moves the main stack to its own area.
- **APCs at start-up**: the runtime queues its signal thread as an APC
  from a DLL's initialisation; ntdll now runs queued APCs once the loader
  is done, and alertable waits run them.
- **fork**: `STARTUPINFO.lpReserved2` reaches the child (the process
  parameters' `RuntimeData`), inheritable handles are really inheritable
  (`OBJ_INHERIT` and `bInheritHandle` on events, mutexes, semaphores,
  sections, timers and directories), so the child finds its parent's
  shared memory and rebuilds itself at the same addresses.
- **Pipes by directory handle**: the runtime opens `\Device\NamedPipe\`
  and creates its pipes relative to that handle.
- **Windows' directory listings**: every directory but a drive's root
  lists `.` and `..` first, so opening an empty directory succeeds (git
  moves pushed objects out of an empty quarantine folder).
- **Winsock tells sockets from pipes**: `WSAEnumNetworkEvents` and
  `WSAEventSelect` fail with `WSAENOTSOCK` on other handles, which is how
  git's `poll` finds out a pipe was closed.
- Smaller pieces: C runtime `swprintf` in msvcrt's legacy form (git's
  `git-*.exe` launchers), `_findfirst*`/`_findnext*`, console input queries
  failing for files, `GetConsoleWindow`, new stub DLLs (`iphlpapi`,
  `netapi32`, `secur32`, `authz`, `dnsapi`, `pdh`) and more kernel32
  (`VirtualAlloc2`, `MapViewOfFile3`, `QueryDosDeviceW`, console buffer
  and input calls...).
- Not yet: hard links (drive C: behaves like FAT, so git renames), and
  interactive `sh` sessions have not been tried; `sh -c` and scripts
  are what is tested.  *(Since done: `sh --login -i` runs interactively since
  Phase 17.2; hard links are still to come.)*

## Language runtimes: Java, .NET, Node.js, Python

The official Windows x64 builds of four runtimes install and run
unmodified, from their own installers or archives:

| Runtime | Package | Tested |
|---|---|---|
| Java (Eclipse Temurin 21) | JRE `.msi`, JDK `.zip` | `java -version`, a stress program (threads, exceptions, stack overflow, files, lambdas), `javac` compiling a program that then runs |
| .NET 10 | runtime + host from NuGet, Roslyn | `dotnet --info`, `dotnet hello.dll`, `dotnet csc.dll` compiling a C# test that then passes |
| Node.js 24 | `.msi`, `.zip` | `node -v`, `-e`, `npm -v` (`npm.cmd` through cmd), output into a pipe, a test script (crypto hashes and random bytes, fs and fs.promises, JSON, regex, exceptions, timers, environment) |
| Python 3.14 | NuGet package (`python`) | `-c`, a test script (hashlib, JSON, regex, files, exceptions, threads, sleep, environment, subprocess) |

`msiexec /i temurin-jre.msi /qn` and `msiexec /i node.msi /qn` install
into `C:\Programs`, write their registry keys and add themselves to
`PATH` through the MSI `Environment` table (`=`/`+`/`-`/`!`/`*` name
flags, `[~]` for prepending or appending to the current value).  New
processes build their environment from the registry
(`HKLM\...\Session Manager\Environment`, then `HKCU\Environment`, user
`Path` after the system one, `REG_EXPAND_SZ` expanded), so a new `cmd`
finds `java` and `node`.  What it took:

- **Loader**: implicit TLS for DLLs loaded later (every thread's TLS
  array grows; HotSpot keeps `Thread::current()` there), the system UCRT
  and API sets ahead of copies shipped next to a program (as Windows
  10 does), dependencies looked up in the folder of the DLL importing
  them (Python's `DLLs\`), `LOAD_LIBRARY_AS_DATAFILE`/`AS_IMAGE_RESOURCE`,
  IL-only assemblies of the other architecture mapped as data, 64-character
  module names, images over 64 MB (`node.exe`), full-path
  `GetModuleHandleEx`, and 8192 memory regions per process.
- **Code generators**: dynamic function tables (`RtlAddFunctionTable`,
  `RtlInstallFunctionTableCallback`, growable tables) for JIT-compiled
  code, and a fuller `RtlVirtualUnwind`: it reports where each
  register was saved (`KNONVOLATILE_CONTEXT_POINTERS`, which the .NET
  GC uses to update object references held in registers), finishes
  epilogues, follows chained unwind info and returns only the requested
  kind of handler.  `RtlCaptureStackBackTrace` walks real frames.
- **Threads**: fibers (`CreateFiber`, `SwitchToFiber`, `ConvertThreadToFiber`;
  the MSVC runtime asks `IsThreadAFiber`), `GetCurrentProcessorNumber`,
  XState context calls, `GetThreadIOPendingFlag`, NUMA and
  processor-group queries.
- **Locales without ICU**: .NET falls back to NLS, so `GetLocaleInfoEx`
  answers every `LOCALE_*` field for `en-US`, `en` and the invariant
  locale; `FindNLSStringEx`, `FindStringOrdinal`, preferred-UI-language
  calls, number and currency formatting; `LINGUISTIC_IGNORECASE` is
  honoured.
- **COM and WinRT**: `CoGetContextToken`/`CoGetObjectContext` return a
  real context object, `RoInitialize`.
- **Console and files**: console input queries succeed only for console
  input handles (libuv and Python decide whether stdin is a console that
  way), `*.*` matches names without an extension (Python's
  `encodings` package search), `NtDeviceIoControlFile`, and pipes answer
  `FileAccessInformation`/`FileModeInformation` (libuv opens a piped
  stdout as a pipe stream).  `PATHEXT` in the registry lists `.COM;.EXE;.BAT;.CMD`.
- **Processors in the registry**: `HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\N`
  for every CPU (`os.cpus()` in Node).
- **New DLLs**: `crypt32` (empty certificate stores), `dbghelp`,
  `rpcrt4` (UUIDs), `powrprof`, `winhttp`, `mswsock`, and more of
  `iphlpapi`, `ws2_32`, `advapi32` (key-less CryptoAPI), `ole32` and the
  UCRT (`_create_locale`, conio, `_wspawnve`...).
- **Windows Installer**: the string pool's encoding of strings of 64 KB
  or more (Node's licence text), which shifted every later string id.
- Not yet: nothing from this list.  (MSI custom actions run since
  "Windows Installer depth", and .NET's globalization goes through ICU
  since "ICU: .NET globalization and kernel32's locales".)

## Installing NovaOS on a disk

The ISO is also the installation disc.  Booted from it, NovaOS runs
live (nothing is kept after a restart) and opens **Install NovaOS**
(`kernel/apps/setup.c`, the engine in `kernel/fs/setup.c`); an icon on
the desktop brings it back.  On an installed system it is in the Start
menu and `start setup` in the Terminal opens it, to copy NovaOS to
another disk.

- **Welcome, choose a disk, confirm, install, finish.**  Setup lists the
  SATA, NVMe and USB disks with their size and what is on them, marks the one NovaOS
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

## OpenGL: Mesa as the system `opengl32.dll`

Programs that draw with OpenGL get OpenGL 4.5 rendered on the CPU by
Mesa's llvmpipe, the same approach Wine takes: an existing open-source
DLL ships as an OS component, and the programs are not changed.  The
App Store's **Mesa 3D** (Runtimes) installs the
[mesa-dist-win](https://github.com/pal1000/mesa-dist-win) 24.2.4 build:
7-Zip unpacks just `opengl32.dll`, `libgallium_wgl.dll` and
`libglapi.dll`, 64-bit into `C:\Windows\System32` and 32-bit into
`C:\Windows\SysWOW64`, so every program that imports `opengl32.dll`
finds them.

- **WGL in gdi32** (`userland/gdi32/wgl.c`): `ChoosePixelFormat`,
  `DescribePixelFormat`, `GetPixelFormat`, `SetPixelFormat` and
  `SwapBuffers` load `opengl32.dll` on first use and call its `wgl*`
  functions, as on Windows.  GDI remembers each window's pixel format
  (set once), and a per-thread guard answers Mesa's own calls back into
  these from what GDI knows.
- **Presenting frames**: Mesa shows each frame with `StretchDIBits`.  An
  unscaled 32-bit blit now copies rows directly, and user32's
  `NovaFlushDC` puts the change on screen, since GL programs draw
  through a DC they keep for the window's life instead of painting in
  `WM_PAINT`.
- **x86 `SLIST_HEADER`** is 8 bytes, as on Windows (it was the 16-byte
  x64 layout, which 32-bit Mesa's aligned stores faulted on).
- Also `RtlGetLastNtStatus`, `HeapWalk`/`HeapLock`/`HeapUnlock` and
  `EnumDisplaySettingsA`.
- Tested in QEMU with `tools/gltest/gltest.c` built with MinGW, 64-bit
  and 32-bit, after installing Mesa from the App Store: vendor, renderer
  and version strings, clearing, immediate-mode drawing, a GLSL shader,
  pixel read-back, then animated frames with `SwapBuffers` (14/14 checks,
  about 48 frames per second in a 320x240 window under QEMU without KVM).  Mesa
  also tries its Zink (Vulkan) and D3D12 drivers first and logs that
  `vulkan-1.dll` and `d3d12.dll` are missing; it then uses llvmpipe.
- Next: Direct3D on top of this (WineD3D to OpenGL, or DXVK on Mesa's
  lavapipe Vulkan).

## App coverage: Notepad++, bat, fd

- **Notepad++ 8.7.9** (64-bit portable) opens with its menus, toolbar and
  editor, and takes typing.  What it needed: the `.exe`'s TLS slot is now
  always 0 (MSVC's thread-safe statics assume it), `GetFileAttributesEx`
  leaves its output alone when the file is missing, and `RtlUnwindEx`
  runs a consolidating unwind's callback, so `catch` blocks in programs
  with the static MSVC C++ runtime run (a `throw` used to resume after
  the throw site).
- **bat** (`CompareObjectHandles`, a new `NtCompareObjects` call) and
  **fd** (`GetModuleHandle` of an API set name) now run; rg and jq ran
  already.
- **New DLLs**: `uxtheme` (no theme; real buffered paint), `dwmapi`
  (composition off), `imm32` (no IME), `msimg32`, `wintrust` (nothing is
  signed), `sensapi`, `wininet` (URL parsing; offline).  `kernel32` has
  `.ini` files (`GetPrivateProfileString` and friends), `gdi32` gradient
  fills, pattern brushes and coordinate conversion, `crypt32`
  `CryptStringToBinary`.
- **`tools/novarun.py`** boots the image in QEMU with programs copied onto
  a data disk, types Terminal commands and takes screenshots:
  `python3 tools/novarun.py --put 'DIR=C:\Apps\x' 'cd C:\Apps\x' 'x.exe' '!shot x.png'`.
- Not yet: ~~Notepad++'s status bar draws black and its toolbar is cut
  short~~ (fixed in Phase 17.6); ~~Neovim hangs on exit (console input handles cannot be waited
  on)~~ fixed in Phase 17.2;
  ffmpeg needs `avrt`, `ncrypt`, `d2d1`, `dwrite` and more (see
  [More compatibility](#more-compatibility-schannel-uniscribe-idn-crt-gaps)).

## ACPI power: shut down, restart, power button

- **`kernel/hal/acpi.c`** reads the FADT (PM1 event and control blocks,
  the reset register, hardware-reduced sleep registers) and finds the S5
  sleep type in the `\_S5` package of the DSDT or an SSDT, which is plain
  data, so no AML interpreter is needed.  It switches the chipset into ACPI
  mode through `SMI_CMD` and enables the fixed-feature power button.
  The MADT lookup in `smp.c` now goes through `AcpiFindTable`.
- **Shut down** writes `SLP_TYP | SLP_EN` to PM1a/PM1b control, falling
  back to the virtual machines' ports.  **Restart** uses the FADT reset
  register, then port `0xCF9`, then the 8042, then a triple fault.  Both
  save drive C: first and show a "Shutting down" / "Restarting" screen.
- **Power button**: the desktop polls `PWRBTN_STS` and shuts down, as
  Windows does by default (`system_powerdown` in the QEMU monitor).
- **Programs**: `NtShutdownSystem`, `ExitWindowsEx` (shut down, power off,
  restart; no log-off), `InitiateSystemShutdown[Ex]` (at once; there is no
  countdown to abort), and a `shutdown.exe` (`/s`, `/p`, `/r`).

## USB keyboards and mice (xHCI)

- **xHCI host controller driver** (`kernel/drivers/xhci.c`): takes the
  controller from the firmware, resets it, and runs one command ring and
  one event ring.  Like the disk and network drivers it is polled: the
  timer tick drains the event ring, so no interrupt routing is needed.
- **Enumeration**: each connected root port is reset, given a device slot
  and an address, and its device and configuration descriptors are read.
  The first HID boot-protocol keyboard or mouse interface is configured
  (Configure Endpoint, `SET_CONFIGURATION`, `SET_PROTOCOL(boot)`) and a
  transfer is kept queued on its interrupt endpoint.
- **HID** (`kernel/drivers/usbhid.c`): keyboard reports become set-1
  scancodes, the codes a PS/2 keyboard sends, so the window manager and
  `user32` see one kind of keyboard; held keys repeat (500 ms, then every
  30 ms) as PS/2 keyboards do on their own.  Mouse reports (buttons, motion,
  wheel) go to the same input queue as PS/2 mouse packets.
- **Hot-plug**: a small `usb` kernel thread enumerates devices plugged in
  after boot and releases the keys of a keyboard that is pulled out.
- `PciMapBar` maps BARs that lie above the 64 GiB physmap (OVMF puts 64-bit
  BARs at 512 GiB and up); the PS/2 driver now notices when there is no
  8042 controller instead of reading phantom bytes.
- Tested in QEMU with `-machine q35,i8042=off -device qemu-xhci -device
  usb-kbd -device usb-mouse`: Terminal commands typed and the pointer moved
  over USB alone, and a keyboard added and removed with `device_add` /
  `device_del` while running.
- Not yet: a keyboard's media keys and a mouse's extra buttons.  (Done
  since: media keys and side buttons under "Media keys, side buttons and
  the horizontal wheel"; hubs, absolute pointers and report protocol in Phase 18.1, USB
  mass storage in 18.2, and keyboard LEDs, several controllers and the
  older UHCI/OHCI/EHCI controllers under "Older USB controllers".)

## Direct3D: DXVK on Mesa's Vulkan

Programs that draw with Direct3D 8, 9, 10 or 11 get it rendered on the
CPU: [DXVK](https://github.com/doitsujin/dxvk) translates Direct3D to
Vulkan, and Mesa's lavapipe runs the Vulkan on the CPU (llvmpipe again
underneath).  As with OpenGL, these are existing open-source DLLs shipped
as OS components, and the programs are unchanged.  WineD3D (Direct3D to
OpenGL) was the other candidate; its source was out of reach from the
build machine, and DXVK is the faster, more complete path anyway.

- **App Store**: **Mesa 3D** now also installs `vulkan_lvp.dll` and its
  manifest (`lvp_icd.x86_64.json`, `lvp_icd.x86.json`) and registers the
  manifests under `HKLM\SOFTWARE\Khronos\Vulkan\Drivers`, as a driver
  installer does.  The new **DXVK** (Runtimes) installs `d3d8`, `d3d9`,
  `d3d10core` and `d3d11` into `System32`/`SysWOW64`, and DXVK's `dxgi`
  as `dxgi_dxvk.dll`.  The Store gained `.tar.gz` downloads (7-Zip takes
  the `.gz` layer off first), `path>name` renames, and the kernel helper
  `um_registry_set_dword` for installers' registrations.
- **`vulkan-1.dll`, NovaOS's own** (`userland/vulkan-1/`): finds the
  driver from that registry key (or `VK_DRIVER_FILES`/`VK_ICD_FILENAMES`),
  loads the first manifest's library that suits the process (64- or
  32-bit), negotiates the driver interface, and hands programs the
  driver's commands directly.  It exports the Khronos loader's 246 names;
  most are a jump through a pointer filled when the program creates its
  instance.  There are no layers and one driver at a time.  Mesa's Zink
  now finds it too, declines the CPU device, and OpenGL stays on llvmpipe.
- **`dxgi.dll`, NovaOS's own** (`userland/dxgi/`): a factory with no
  adapters, so programs that import DXGI start; with DXVK installed, it
  hands out DXVK's factory instead.  A Khronos `vulkan-1.dll` that a
  program carries beside its `.exe` loads `System32\dxgi.dll` and needs a
  factory before it reads its drivers; it keeps getting NovaOS's, since
  DXVK's would start Vulkan again and deadlock on DXVK's own lock.
- **`cfgmgr32.dll`** with empty device lists, for the loader and DXVK.
- **Frames on screen**: lavapipe presents with `StretchBlt` from a DIB
  section to the window's DC.  `BitBlt` and `StretchBlt` now copy rows
  for unscaled 32-bit copies and flush window DCs to the screen through
  `NovaFlushDC`, which skips a DC between `BeginPaint` and `EndPaint`
  (`EndPaint` presents).  The 64-bit D3D9 test went from 5 to 12 frames
  in three seconds at 320x240.
- **32-bit heap blocks are 16-byte aligned**, as on Windows: lavapipe's
  generated code reads them with `movdqa`.
- Also `AllocateLocallyUniqueId`, `EnumDisplayDevicesA`, the display
  configuration queries (`QueryDisplayConfig` and friends, answering "not
  supported"), `__C_specific_handler` from `kernel32` on x64, and the
  code bytes at a fault in JIT code in the serial log.
- Tested in QEMU with `tools/d3dtest/d3dtest.c` built with MinGW, 64-bit
  and 32-bit, after installing Mesa 3D and DXVK from the App Store: the
  D3D9 adapter (llvmpipe), a device, a fixed-function triangle read back
  through `GetRenderTargetData`, D3D11 at feature level 11_0 through a
  DXGI factory and adapter, a cleared render target read back through a
  staging texture, then animated frames with `Present` in a window
  (17/17 checks; D3D11 about 9 frames per second in a 320x240 window
  under QEMU without KVM).  OpenGL's `tools/gltest` still passes 14/14.

## Sound: Intel HD Audio, winmm and WASAPI

- **Driver** (`kernel/drivers/hda.c`): finds the HD Audio controller on
  PCI (class 04/03), resets it, talks to the codecs through the CORB/RIRB
  rings and routes every output pin with something attached (line out,
  speaker, headphones) through mixers and selectors to a DAC, unmuting
  the path at 0 dB.  One output stream plays a 64 KiB ring of 48 kHz
  16-bit stereo.  It polls: no interrupt routing or shared PCI code
  changed.  Tested with QEMU's `intel-hda` + `hda-output`/`hda-duplex`.
- **Mixer** (`kernel/drivers/audio.c`): up to 32 streams of 48 kHz s16
  stereo frames, each with its own queue and left/right volume.  A
  kernel thread wakes every tick, reads the hardware's position and mixes
  the running streams into the ring 80 ms ahead of it.  A stream reports
  frames written, mixed and actually played.
- **System calls** (`kernel/um/um_audio.c`, NovaOS-private like the
  socket ones): `NtNovaAudioOpen`, `NtNovaAudioWrite`, `NtNovaAudioCtl`.
  A stream is a handle object, so it stops when the program closes it or
  exits.
- **winmm**: `waveOut*` (open/write/pause/restart/reset/close, positions,
  volume, device caps; function, window, thread and event callbacks)
  converts any 8/16/24/32-bit PCM or float format, 1 to 8 channels, any
  rate, to the mixer's format.  `PlaySound`/`sndPlaySound` play files,
  `WAVE` resources, memory images and system sound aliases
  (`HKCU\AppEvents\Schemes`); NovaOS ships no `.wav` files, so the
  default sound is a synthesized two-note chime.  `waveIn`, MIDI and the
  mixer API still report no devices.
- **kernel32 `Beep`** plays its tone on the sound card; **`MessageBeep`**
  (and so every message box) plays the system sound.
- **mmdevapi** (WASAPI): `MMDeviceEnumerator` lists one render endpoint
  with its property store (friendly name, device format); `IAudioClient`
  (and `IAudioClient2`/`3`) in shared mode with event or polling
  clients, `IAudioRenderClient`, `IAudioClock`, `ISimpleAudioVolume`,
  `IAudioStreamVolume`, `IAudioSessionControl`.  The mix format is 48 kHz
  float stereo, and other PCM/float formats are converted.  **avrt**
  (`AvSetMmThreadCharacteristics`) raises the thread's priority.
- **Testing**: `soundtest` plays tones through each path;
  `tools/novarun.py --wav out.wav` gives QEMU a sound card that records to
  a file, and `tools/wavcheck.py` lists each tone's start, length, level
  and pitch.  Checked 64- and 32-bit: `waveOut` at 22.05 kHz mono and 48 kHz
  float, WASAPI, both at once (the mixer sums them at full level),
  `PlaySound` of an 8-bit 22.05 kHz file, the default sound and `Beep`;
  no discontinuities inside any tone.
- Not yet: recording, DirectSound (`dsound.dll`), XAudio2, MIDI,
  `IAudioEndpointVolume`, exclusive mode, and real programs (VLC,
  Audacity, SDL games) on it.

## ACPI sleep (S3)

- **Sleep**: Start > Sleep (right-click Start, or the power button in the
  Start menu), or `SetSuspendState`, `NtSetSystemPowerState` and
  `NtInitiatePowerAction`, which return once the machine is awake.
  Drive C: is saved first.  A keypress wakes it (`system_wakeup` in the
  QEMU monitor); the power button that wakes it doesn't also shut down.
- **CPUs** (`kernel/ke/sleep.c`): the other CPUs are stopped with an NMI
  and save their state; each CPU's control registers, GDT/TSS, IDT, MSRs,
  MTRRs and FPU state are kept, and the FACS waking vectors (real mode and
  32-bit) point at the CPU start-up trampoline, found in `\_S3`.  On wake
  the boot CPU restores itself and the PCI configuration of every function,
  restarts the other CPUs through the trampoline, and each CPU jumps back
  into what it was doing.
- **Devices** set up again: AHCI disks, e1000 network, PS/2 keyboard and
  mouse (with scancode translation forced on), the Bochs/QEMU display
  mode, xHCI USB (the controller restarts and the keyboards and mice are
  enumerated again) and HD Audio (codec paths and the output stream).  The
  wall clock moves on by what the CMOS clock measured.
- `sleeptest.exe` sleeps and then checks the clock, threads and files.
  Tested in QEMU (OVMF, q35) with 1, 2 and 4 CPUs, three sleeps in a row,
  and with a USB keyboard and mouse and HD Audio attached.
- Not yet: wake devices such as USB keyboards.  (Display modes on other
  adapters came later: see "Display adapters: QXL, virtio, VMware, Cirrus,
  and their modes after sleep"; USB wake came with "ACPI: SCI interrupt, lid,
  thermal zones, wake devices, _PRT (Phase 18.6)".)

## ACPI namespace (uACPI): batteries and AC power

- **AML interpreter**: uACPI 6.1 (`third_party/uacpi`, MIT) loads the DSDT
  and SSDTs.  `kernel/hal/aml.c` gives it memory, ports, PCI configuration
  space, time, locks and a work queue.  It runs on an `acpi` kernel thread
  that starts after boot and then polls the SCI every 100 ms (device
  interrupts stay off in NovaOS), running GPE methods and `Notify`
  handlers.
- **Batteries and AC adapters** (`PNP0C0A`, `ACPI0003`): `_BIX` or `_BIF`,
  `_BST` and `_PSR`, read every 5 s and on notifications, converted to
  mWh/mW.  `GetSystemPowerStatus` (kernel32), `CallNtPowerInformation`
  (`SystemBatteryState`) and `GetPwrCapabilities` report them through the
  new `NtPowerInformation`.  `battery.exe` prints them.
- **Power buttons**: once the namespace is loaded, the fixed button goes
  through uACPI and control-method buttons (`PNP0C0C`, `Notify 0x80`)
  work too.
- **Sleep**: `_PTS` and `_WAK` now run around S3, with only wake GPEs on
  while asleep.
- Tested in QEMU with extra SSDTs (`-acpitable`) describing a battery in
  mWh on battery power (75%, 3 h left) and one in mAh charging on AC
  (25%); sleep and the power button pass as before.

## Regression gate: boot CI on every pull request

- **GitHub Actions** (`.github/workflows/ci.yml`): every pull request and
  every push to main builds the kernel, bootloader, userland and
  `build/nova.img` on Ubuntu 24.04, boots it in QEMU (q35, OVMF, TCG) and
  runs the self-tests.  A failing test fails the "Build and boot-test"
  check; the step summary has a table of results and the serial log,
  screenshots and sound recording are kept as an artifact.
- **`tools/selftest.py`**: one boot, then each test typed into the
  Terminal.  A test passes on exit code 0, no `FAIL` line, no non-zero
  "failed" count and the output it expects; a kernel panic ends the run.
  The boot has an HD Audio card recorded to a WAV, which must hold the
  tones `soundtest` played, and the battery in `tests/acpi/battery.asl`
  (75%, 3 h left), which `battery` must report.
- **`guitest auto`** drives its own menus, edit and list boxes, the
  resource dialog, a message box and the property sheet, and reports.
- `tools/novarun.py`'s boot-and-type logic is now a `Nova` class that
  other tools import.

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

## Nightly app corpus

- **`tools/appcorpus.py`** downloads the official Windows x64 releases of
  ripgrep, fd, jq, 7-Zip, MinGit, Python (the NuGet package), Node.js and
  Notepad++ (portable), unpacks them into `C:\Apps` with a few sample files
  and a bare git repository, boots once and types each program's commands:
  a search, a `find`, a JSON filter, an archive made and tested, `git
  clone`, `log` and `status`, `python -c`, `node -e`.  Notepad++ opens a
  file and its screenshot is compared with `tests/reference/notepad++.png`
  (scaled down; at most 3% of pixels may differ).
- **`.github/workflows/nightly.yml`** runs it every night on main (and on
  pull requests that change the corpus) and posts the pass/fail table to
  the run's summary and as a comment on the "Nightly app corpus" issue.
- Not yet: Notepad++'s tab bar and status bar still draw black; the
  reference shows them so, and an improvement means updating it
  (`--update-reference`).  *(Since done: the bars draw since Phase 17.6, see
  "Phase 17: kernel and API correctness", and the reference was updated.)*

## More compatibility: Schannel, Uniscribe, IDN, CRT gaps

Driven by ffmpeg's imports (`tools/pe_imports.py`).  Before writing each
DLL we looked for an MIT, BSD or zlib licensed one to reuse; none existed
for these, so the new ones are written here, and Schannel reuses the Mbed
TLS already in the tree.

- **Schannel** (`userland/secur32/schannel.c`): `InitializeSecurityContext`,
  `EncryptMessage`, `DecryptMessage`, `QueryContextAttributes` (stream
  sizes, connection info, ALPN), `ApplyControlToken` (shutdown) and the
  `InitSecurityInterface` tables, on Mbed TLS with the Mozilla roots
  (`C:\Windows\System32\ca-bundle.der`).  It takes `SCHANNEL_CRED` and
  `SCH_CREDENTIALS`, SNI, ALPN, manual validation and
  `SCH_CRED_NO_SERVERNAME_CHECK`, and reports untrusted roots, expired
  certificates and name mismatches as Windows does.  Client side only.
- **New DLLs**: `usp10` (Uniscribe for left-to-right scripts:
  `ScriptItemize`, `ScriptShape`, `ScriptPlace`, `ScriptTextOut`,
  `ScriptBreak`, `ScriptString*`…), `normaliz` (`IdnToAscii`/`IdnToUnicode`,
  RFC 3492 Punycode), `ncrypt` (the provider opens; there are no stored
  keys), `avicap32` (no capture devices), `d2d1` (the matrix helpers;
  factories report `E_NOTIMPL`).
- **More of existing DLLs**: the CRT's `mbstowcs_s`/`wcstombs_s`, the
  `_nolock` functions, `freopen_s`, `tmpnam_s`, `_utime64`, `_wspawnvp`
  and the single-byte `_mbs*` set; `ws2_32` `getservbyname`/`getservbyport`,
  `gethostbyaddr` and `WSAPoll`; `winmm` `waveIn*` (no recording devices);
  `dnsapi` `DnsQuery_UTF8`; `iphlpapi` `GetIpForwardTable2` and friends;
  `advapi32` `RegLoadMUIStringW`; `shlwapi` `SHCreateStreamOnFileEx` and
  `StrRetTo*`; `ole32` `CreateBindCtx`, `ReadClassStm`/`WriteClassStm`,
  `OleSaveToStream`/`OleLoadFromStream`; `gdi32` DIB colour tables.
- **Loader**: a program's own TLS callbacks now run (process and thread
  attach and detach), not only those of DLLs; GLib checks for this.
- **Exceptions**: `RtlRaiseException` reports its caller's frame, so a
  handler that continues execution (the "set thread name" exception
  `0x406D1388`) resumes after the call instead of raising again forever.
- **`tools/novarun.py --net`** gives the guest a network card on QEMU's
  user network (the host is `10.0.2.2`).
- Tested with an FFmpeg nightly (BtbN's static x64 build) in QEMU, with
  the GDI and DirectWrite functions of the Firefox work in place: an x264
  encode, decoding it back, and streaming a WAV over HTTPS from the host
  with TLS 1.3 and with TLS 1.2; with `tls_verify` on (ffmpeg's default) a
  self-signed server is refused as an untrusted root.

## A pager for git: `less.exe`

`git log`, `diff` and `config --list` on the Terminal now page without
`--no-pager`.  MinGit has no `less`, git's default pager, so git stopped
with "unable to execute pager 'less'" (it does on Windows too).  NovaOS
now ships `less.exe` in `C:\Windows\System32` (and SysWOW64), on `PATH`,
where git finds it:

- It shows a screenful (the console's rows), then asks `-- More --` and
  takes single keys, through the per-key console input of Phase 17.2:
  Space (or `f`, Page Down) the next page, Enter (or `j`, Down) one more
  line, `d` half a page, `/text` and Enter skips to the next line
  containing the text, `q` (or Esc) quits, and git then stops quietly.
  The prompt is erased as the text moves on.  On a console that only
  hands over whole lines it asks for a line instead (Enter, a number,
  `/text` or `q`).
- Output that fits on one screen goes straight through, as with git's
  `LESS=FRX`, and so does everything when the output is not a console
  (`git log | find` is unchanged).  Color escapes pass through, files
  given as arguments (`less a.txt`) work, and options are accepted.
- Why not the real `less`: its Windows build draws through the console
  screen-buffer calls (`SetConsoleCursorPosition`, `FillConsoleOutput*`),
  which are still no-ops, so it would draw garbage; it can come once they
  drive the Terminal's screen.
- The nightly corpus's `git log` test no longer passes `--no-pager`.
- The test tools (`tools/selftest.py`, `appcorpus.py`) match a program's
  expected output without the kernel's `[UM]`/`[SCHED]` log lines, which
  share the serial port and could land mid-line (`[[UM] jq.exe ... 40,2]`).

## Phase 17: kernel and API correctness

- **Every thread has a real `ETHREAD`** (17.1): `PsGetCurrentThread` used
  to cast the running scheduler `Thread` to an `ETHREAD`, which is only
  true for threads made by `PsCreateSystemThread`.  Kernel threads from
  `sched_create_thread`, Windows programs' threads and the idle threads
  are bare `Thread`s, so writes through it (`PsInitialize` naming the boot
  thread TID 4, `PsTerminateSystemThread`) landed past the end of the
  structure.  `Thread.ethread` now leads to the thread's `ETHREAD`; a bare
  thread gets one (a real Thread object in the System process) the first
  time it asks, released when the scheduler frees it.  With interrupts
  off the lookup returns NULL rather than touch the heap.  The boot-time
  `[PSTEST]` self-test (`kernel/ps/ps_test.c`) checks it from plain
  kernel threads, including `NtCurrentThread()`,
  `PsLookupThreadByThreadId` and `PsTerminateSystemThread`.
- **Waitable console input and `WriteConsoleInputW`** (17.2): console
  input is a kernel queue of `INPUT_RECORD`s (keys, window size changes,
  what programs write into it) with a waitable object, so console input
  handles work with `WaitForSingleObject`, `RegisterWaitForSingleObject`
  and libuv.  `ReadConsoleInput`, `PeekConsoleInput`, `WriteConsoleInput`,
  `GetNumberOfConsoleInputEvents`, `FlushConsoleInputBuffer` and the
  console modes go through one NovaOS service, `NtNovaConsole`.  A program
  that turns off line input gets its keys as records, or as xterm
  sequences with `ENABLE_VIRTUAL_TERMINAL_INPUT`.
- **Full-screen programs in the Terminal**: while a program is on the
  alternate screen or reads raw input, its output goes to a libvterm 0.3.3
  (MIT) screen grid that the Terminal paints in colour, and libvterm's
  answers to terminal queries come back through console input.
- **What Neovim needed besides**: completion packets for overlapped pipe
  requests now go to the port the request started with, so closing a
  handle still delivers the cancellation libuv waits for (Neovim, and any
  `nvim -l` script, used to hang on exit); a `RegisterWaitForSingleObject`
  callback may unregister itself; `CreatePseudoConsole` and friends exist
  (and fail), which tells Neovim's `--embed` server to keep its RPC on the
  pipes and use `CONIN$`/`CONOUT$` for the terminal; the CRT reuses closed
  descriptors 0-2 first and moves the standard handles with them;
  `RtlRunOnceExecuteOnce`, `RtlUTF8ToUnicodeN`, `RtlUnicodeToUTF8N`, and
  UCRT `_o_` imports resolve to the plain functions.
- **What MSYS2 `sh` needed**: `\Device\Null` opens as the null device and
  counts as existing (Cygwin asks `NtOpenSymbolicLinkObject`); an empty
  name relative to a file handle reopens that file; a line feed on the
  screen grid also returns the carriage unless the program set
  `DISABLE_NEWLINE_AUTO_RETURN`.
- **Process creation flags** (17.3): `CREATE_SUSPENDED` starts the first
  thread suspended until `ResumeThread` (the same change as the Firefox
  work's).  `CREATE_NEW_CONSOLE` gives a console program a console of its
  own in a Terminal window titled with its path; the window closes when
  the program ends, and closing it ends the programs on that console.
  GUI programs ignore the flag, as on Windows, and `cmd`'s `start` uses it
  unless given `/B`.  `GetConsoleProcessList` now lists the processes on
  the caller's console.  A file's position belongs to the open file, not
  the handle: duplicates, inherited handles and handles passed as a
  child's standard output share it, so a parent and child writing to one
  log file follow each other instead of overwriting.  `proctest` covers
  all three in the core suite.
- **Security on objects** (17.4): tokens are kernel objects.  Each process
  has a primary token, inherited from the process that started it (the
  desktop user's otherwise: a standard user in Users, with Administrators
  only for denying since nothing is elevated), and a thread can
  impersonate an impersonation token.  `NtOpenProcessToken(Ex)`,
  `NtOpenThreadToken(Ex)`, `NtDuplicateToken`, `NtFilterToken`,
  `NtQueryInformationToken`, `NtImpersonateAnonymousToken`,
  `NtAccessCheck` and `NtQuery/SetSecurityObject` moved from ntdll into
  the kernel at their Windows 10 1903 numbers, and
  `NtSetInformationThread(ThreadImpersonationToken)` sets or ends
  impersonation.  `NtFilterToken` (advapi32's `CreateRestrictedToken`)
  makes SIDs deny-only, removes privileges and adds restricting SIDs, and
  can make the token write-restricted.  A named event, mutex, semaphore,
  timer, section, directory or symbolic link keeps the security descriptor
  it was created with (`SECURITY_ATTRIBUTES`), and opening it, or creating
  an existing name, checks the access asked for against it as the calling
  thread: the DACL in order, deny-only groups only in deny ACEs, the
  owner's implicit `READ_CONTROL | WRITE_DAC`, and for a restricted token a
  second pass with its restricting SIDs.  kernel32's `OpenEvent`,
  `OpenMutex` and `OpenSemaphore` pass the access asked for, not all.
  `GetKernelObjectSecurity`, `SetKernelObjectSecurity`, `GetSecurityInfo`
  and `SetSecurityInfo` read and change the descriptor; `CheckTokenMembership`,
  `ImpersonateSelf`, `RevertToSelf`, `SetThreadToken` and
  `ImpersonateLoggedOnUser` work on real tokens.  Files keep no
  descriptor yet: the file system takes that part in Phase 18 (18.5), on
  the same check (`um_access_check_sd`).  `sectest` and `acltest` (also
  32-bit) show a restricted token refused a protected event.
- **Registry change events and pending renames** (17.5):
  `RegNotifyChangeKeyValue` works, on the new `NtNotifyChangeKey`: a watch
  on a key (optionally with its subkeys) signals its event once when a
  value is set or deleted, a subkey is added, deleted or renamed, or the
  key itself is deleted, as its filter asks; without `async` the call
  waits for that.  `MoveFileEx(MOVEFILE_DELAY_UNTIL_REBOOT)` writes full
  `\??\` paths to Session Manager's `PendingFileRenameOperations` (with
  `!` for `MOVEFILE_REPLACE_EXISTING`), and the kernel now carries the
  list out at boot, after loading the registry and before any program
  runs, then deletes it.  A running program's files (its `.exe` and DLLs)
  are now held while it runs, as on Windows, so they cannot be deleted or
  replaced until it ends (renaming them still works).  The core suite
  runs an installer that has to replace a running program, restarts
  (`tools/novarun.py` and `tools/selftest.py` can restart NovaOS) and
  checks the replacement happened.  Hard links followed (below, 2026-10-03).
- **Small visible bugs** (17.6): File Explorer's This PC is now a list of
  the drives (C: and each mounted volume, D:, E:, ...) with their free
  space and size, and opening `C:\` or the desktop's This PC shows it; the
  up button and Backspace go from a drive's root back to This PC.  The
  Terminal's `dir` and cmd's `dir` name the drive they list and give that
  drive's own free space (cmd always asked C: before), and
  `GetDiskFreeSpaceEx` asks each volume (`FileFsFullSizeInformation`),
  C: included, whose free space is the free memory it lives in.
  Notepad++'s tab bar and status bar drew black: it double-buffers them by
  sending `WM_PRINT` into a memory DC, which `DefWindowProc` ignored.
  `WM_PRINT` now erases, sends `WM_PRINTCLIENT` and prints the children;
  the status bar, tab control and progress bar draw on `WM_PRINTCLIENT`;
  and the tab control lets its parent draw `TCS_OWNERDRAWFIXED` tabs
  (`WM_DRAWITEM`), sizes tabs from their text, icon and `TCM_SETPADDING`
  (`TCM_SETITEMSIZE`'s width only with `TCS_FIXEDWIDTH`) and takes
  `TCM_SETMINTABWIDTH`.  The nightly app corpus now boots with an empty
  NTFS drive D: and keeps screenshots of `dir C:\` and `dir D:\` (each
  with its own free space), This PC and Notepad++ (the last two compared
  with references in `tests/reference/`).
- **Off the big kernel lock** (17.7): files, the registry, the console and
  starting processes and threads now run beside each other on every CPU.
  The file system has a reader/writer lock: opening a file that is there,
  reading, writing, seeking and closing take it shared (each open file's
  contents under a lock of its own), and only changes to the tree take it
  alone.  The desktop thread's loop no longer holds it every tick, only
  around input, drawing and built-in windows' timers that may use files.
  The registry has a reader/writer lock of its own, a lock per key and
  value names on the stack.  A process's lock is a reader/writer lock too,
  and handles are opened, looked up and closed under it shared, each slot
  with its own lock; console output takes turns on the console's lock.
  Programs' memory allocation got arenas picked by thread (one heap lock
  was 18% of a 4-thread registry run) and stopped losing every freed small
  block (a tag bug kept them from being reused); kmalloc keeps a few
  objects per CPU; copies to and from programs move 8 bytes at a time and
  msvcrt's `memcmp` compares 8 at a time.  On one CPU in QEMU, files went
  from 1,800 to 18,000 operations a second and the registry from 5,000 to
  70,000; four CPUs do about 3x that (`smpstress scaling 3`; computing
  alone scales 3.7–4x on the same host).  smpstress also starts copies of
  itself from several threads at once, and the nightly run boots it on 4
  CPUs.  The Terminal's new `profile` command samples where the CPUs spend
  their time.
- Tested in QEMU: Neovim 0.10.4 and 0.11.4 open `t.txt`, take `ihello
  world<Esc>:wq` and exit with code 0 leaving the file written; MinGit's
  `sh --login -i` shows its coloured prompt and runs `ls`, pipes,
  `$(...)` and redirections to `/dev/null`; the core self-tests pass.

## Display adapters: QXL, virtio, VMware, Cirrus, and their modes after sleep

- **More adapters with the DISPI registers** (`kernel/hal/display.c`): the
  VBE driver now also drives QEMU's QXL (`-vga qxl`), virtio-vga
  (`-vga virtio`) and VMware SVGA II (`-vga vmware`, whose VGA core has
  them, with the framebuffer in BAR1) besides the standard VGA,
  bochs-display and VirtualBox's VBoxVGA.  The adapter table follows
  OVMF's QemuVideoDxe (BSD-2-Clause-Patent).  All of them get run-time
  resolutions and Settings > Display names the adapter.
- **Cirrus Logic GD5446** (`-vga cirrus`): 800x600 and 640x480 at 32 bpp,
  set with QemuVideoDxe's VGA and Cirrus register tables.  The bootloader
  now only picks 32-bit GOP modes, so Cirrus boots in 800x600 instead of
  its 24-bit 1024x768 mode, which drew garbled.
- **Modes after S3** (`DisplayResume`): the driver sets the current mode
  again on wake from what it knows, not from registers saved through I/O
  ports, so bochs-display (MMIO only) and Cirrus come back too; the page
  that was on screen stays on screen, and the desktop is redrawn in case
  video memory was lost.
- `tools/novarun.py --display NAME` boots on another adapter (`cirrus`,
  `vmware`, `qxl`, `virtio`, or a `-device` such as `bochs-display`).
- Tested in QEMU on all six adapters: switch to a non-boot mode with
  `disptest W H`, `sleeptest`, `system_wakeup`, and the desktop is back in
  that mode; also with page flipping (`-global VGA.vgamem_mb=64`).
- Not yet: real GPUs (Intel, AMD, NVIDIA) and virtio-gpu without VGA have
  no driver, so they stay on the UEFI framebuffer, and after sleep they
  show whatever the firmware's wake path sets up, which is often nothing.

## Program pointers and animated cursors (.ani)

Until now `SetCursor` only remembered its argument: the desktop always
drew its own arrow.  The pointer is now the program's, and animated
cursors play.  No MIT, BSD or zlib licensed .ani reader was found (Wine's
is LGPL), so the parser is written here; the format is a small RIFF file.

- **The kernel draws a program's pointer** (`kernel/wm/wm.c`,
  `kernel/gdi/gdi.c`): `NtNovaGuiCtl` op 19 hands it a shape (up to
  64 x 64 logical pixels, its hot spot, up to 64 frames and 256 steps,
  each step's time in jiffies), or asks for the arrow or no pointer.  It
  shows over the client area of that process's windows, and anywhere while
  one of them has the mouse captured; the desktop, title bars, borders and
  window drags keep the arrow.  The desktop's tick steps animated shapes
  (100 Hz against the .ani's 60 Hz jiffies) and redraws the pointer when
  the window under it changes.  Op 20 reports what the pointer shows, for
  tests.
- **user32**: `SetCursor` sends the cursor to the kernel when it changes
  (the system `IDC_*` cursors are the arrow; `NULL` hides the pointer),
  `ShowCursor` below zero hides it, and `GetCursorInfo` says whether it
  shows.
- **Animated cursors** (`userland/user32/res.c`): RIFF `ACON` files with
  `anih`, `rate`, `seq ` and the `fram` list of .cur/.ico frames, from
  `LoadCursorFromFile`, `LoadImage(LR_LOADFROMFILE)`,
  `CreateIconFromResourceEx` and `ANICURSOR`/`ANIICON` resources
  (`LoadCursor`, `LoadImage`).  `DrawIconEx` draws the frame of the step it
  is given, and `GetCursorFrameInfo` reports each step's frame and rate.
  `LoadCursorFromFile` loads .cur files too, and returns `NULL` for a
  missing file as Windows does (it used to return the arrow).
- **`anitest.exe`** (in the core self-tests) loads a spinner
  (`userland/programs/anitest.ani`, made by `tools/mkani.py`: 8 frames
  played in a custom order with two rates) from its resource, from memory
  and from a file, checks the steps, rates, hot spot and each step's
  drawing, then makes it the pointer over a window and checks the desktop
  shows it and steps through at least 6 of the 8 frames in 1.5 s, that
  `SetCursor(NULL)` hides it and the arrow comes back.  `anitest show N`
  keeps the window up for N seconds.  It runs as a 32-bit program too
  (`C:\Programs\x86\anitest`).
- Not yet: the system cursors themselves (I-beam, resize arrows, the
  busy and "working in background" animations) are all the arrow;
  `SetSystemCursor` does nothing; `CopyIcon` of an animated cursor keeps
  only its first frame; at 200 % the pointer is scaled up by nearest
  neighbour.

## COM type libraries and FH4 C++ exceptions

Two items left open since Phase 10.  We looked for MIT, BSD or zlib
licensed code first: Wine's typelib and FH4 code are LGPL, and nothing
permissive covers either, so both are written here from the file formats
(checked against `widl` output and real MSVC binaries).

- **Type libraries** (`userland/oleaut32/typelib.c`, `typeinfo.c`,
  `invoke.c`): `LoadTypeLib`/`LoadTypeLibEx` read MSFT-format libraries
  from `.tlb` files and from the `TYPELIB` resources of DLLs and EXEs
  (`file.dll\2` picks a resource), with `stdole2.tlb` built in (`IUnknown`,
  `IDispatch`, `IEnumVARIANT`).  `ITypeLib2`, `ITypeInfo2` and `ITypeComp`
  cover enums, records, coclasses, interfaces and dispinterfaces,
  including both views of a dual interface (`href -1`), default values,
  references into imported libraries and documentation strings (aliases
  and modules are read too, but no test library has them yet).
  `RegisterTypeLib`, `UnRegisterTypeLib` (and the `ForUser` forms), `QueryPathOfRegTypeLib` and `LoadRegTypeLib` keep
  `HKCR\TypeLib` and the interfaces' `ProxyStubClsid32` keys.
  `LHashValOfNameSys` gives a case-insensitive hash, not Windows' exact
  values (the lookups here compare names, so the hash is never needed).
- **Calling through type information**: `ITypeInfo::Invoke` (and so
  `DispInvoke`, `DispGetIDsOfNames` and `CreateStdDispatch`) converts
  DISPPARAMS to each method's own argument types, with named arguments,
  `[optional]` and `[defaultvalue]`, `[in, out]` by reference, `[retval]`
  and property puts, calls the vtable, and turns a failing HRESULT into
  `DISP_E_EXCEPTION` with the object's error info.  `DispCallFunc` calls
  any function or vtable slot (x64 register and stack arguments, x86
  stdcall and cdecl, floating-point and structure returns).
- **FH4** (`userland/vcruntime140/eh.c`, `vcruntime140_1.dll`):
  `__CxxFrameHandler4`, the compressed exception tables that MSVC has
  emitted for x64 since Visual Studio 2019, decoded into the same state
  machine as `__CxxFrameHandler3` (unwind maps, try blocks, catch
  continuations, separated code, `noexcept` functions).  The new
  `vcruntime140_1.dll` forwards to `vcruntime140.dll`, as Microsoft's does.
- **The sample COM server** (`testdll.dll`, `Nova.Calc`) now embeds its
  type library (`userland/testdll/idl/novacalc.idl`, compiled with `widl`
  by `make_tlb.sh`); its `IDispatch` is `DispInvoke` over that library and
  `DllRegisterServer` registers it.  DLLs can now carry an `.rc` file.
- **CRT**: the `<fenv.h>` functions (`fetestexcept`, `feclearexcept`,
  `fegetround`...) are exported from `msvcrt.dll` and `ucrtbase.dll`.
- Tests: the new `tlbtest` passes 110/110, 64- and 32-bit; `comtest`
  59/59 and `cppeh` 17/17, both architectures, all in the CI core suite
  now.  Python 3.14 with the kiwisolver 1.5.1 wheel, run against NovaOS's
  own `vcruntime140.dll` and `vcruntime140_1.dll` (Microsoft's copies
  removed from the Python folder), raises and catches kiwisolver's C++
  exceptions (`DuplicateConstraint`, `UnsatisfiableConstraint`,
  `UnknownConstraint`, `UnknownEditVariable`) through FH4 tables and gets
  the same results as on Linux.
- Not yet: NumPy still stops at the C99 complex functions (`cabs`,
  `cexp`...) the UCRT exports, and `AddDllDirectory` is a stub, so
  `os.add_dll_directory` paths are not searched.  `msvcp140.dll` (the C++
  standard library) is not provided.  (All three since closed: see "The
  C++ standard library, C99 complex math and DLL directories".)

## Complex text: HarfBuzz, FreeType and Uniscribe

Phase 19.1.  Arabic, Hebrew and the Indic scripts are shaped: letters join
into their contextual forms, ligate, reorder and run right to left.

- **`novatext.dll`** (`userland/novatext`) is the text core, built once and
  shared by Uniscribe, DirectWrite and Direct2D: **HarfBuzz 11.2.1**
  (`third_party/harfbuzz`, MIT) and **FreeType 2.13.3**
  (`third_party/freetype`, the FreeType License; TrueType, OpenType/CFF,
  Type 1, CID and `.fon` drivers, the smooth and mono rasterizers, the
  auto-hinter and the stroker).  It exports both libraries' C APIs (`hb_*`,
  `FT_*`).  No MIT Uniscribe or HarfBuzz-free shaper fitted, so this reuses
  the standard pair.  HarfBuzz is C++: `tools/build_userland.py` compiles it
  with clang against libc++'s headers (`libc++-dev`; nothing of libc++ is
  linked, and HarfBuzz needs no C++ runtime).  For that, the userland's
  `math.h` and `stdlib.h` gained the C++ overloads the Windows SDK's have,
  `locale.h` Windows' full `lconv`, and `include/cxx/functional` a slim
  `<functional>`.
- **`usp10.dll`** (`userland/usp10/usp10.c`) is rewritten on it.
  `ScriptItemize` splits text by script (HarfBuzz's Unicode data) and by
  bidirectional level (a compact UAX #9: strong letters, European and
  Arabic numbers, neutrals between them).  `ScriptShape` reads the DC's
  font through `GetFontData`, shapes the run with HarfBuzz at gdi32's
  pixel size and returns glyphs in visual order, logical clusters and
  visual attributes; it reports `USP_E_SCRIPT_NOT_IN_FONT` when the font
  lacks a complex script.  `ScriptPlace` hands out HarfBuzz's advances and
  mark offsets, `ScriptTextOut` draws marks at their offsets, and
  `ScriptCPtoX`, `ScriptXtoCP` and `ScriptGetLogicalWidths` handle
  right-to-left clusters.  New: `ScriptShapeOpenType`,
  `ScriptPlaceOpenType` (OpenType features), `ScriptItemizeOpenType`'s
  real script tags, `ScriptGetFontScriptTags`, `ScriptStringGetOrder`.
  The `ScriptString*` layer shapes each run with a fallback font when the
  DC's lacks its script (`SSA_FALLBACK`), lays the runs out in visual
  order and draws them all on the DC font's baseline.
- **GDI**: `ExtTextOut`, `TextOut` and `GetTextExtentPoint32` send text
  with complex-script characters through Uniscribe, as Windows' LPK does
  (`ETO_GLYPH_INDEX` and `ETO_IGNORELANGUAGE` skip it; `ETO_RTLREADING`
  makes the line right to left).  GDI gained the faces Noto Sans Arabic
  and Noto Sans Devanagari (`C:\Windows\Fonts`), also under Windows'
  names for those scripts (Traditional Arabic, Mangal, Nirmala UI...).
- **`usptest`** (in the CI core suite, 64- and 32-bit) checks itemizing,
  the Arabic contextual forms and lam-alef ligature, the Devanagari kssa
  conjunct with its reordered i sign and a half form, against the glyphs
  HarfBuzz gives on the build host, and that `ExtTextOut` draws mixed
  Latin, Arabic and Devanagari lines pixel for pixel as `ScriptStringOut`
  does.

## Direct2D

Phase 19.2.  `d2d1.dll` (`userland/d2d1`) is a software Direct2D.  No
MIT, zlib or BSD Direct2D exists (Wine's is LGPL), and the permissive
vector libraries (ThorVG, Blend2D, plutovg) could not be fetched here, so
it is written for NovaOS on a small core of its own:

- **Geometry** (`path.c`, `geometry.c`): figures of lines and cubic
  Béziers; rectangles, rounded rectangles, ellipses, arcs (SVG's endpoint
  form) and quadratic curves become them.  Path geometries and their
  sinks, transformed geometries and groups.  `GetBounds` is exact (curve
  extrema); `FillContainsPoint`, `ComputeLength`, `ComputePointAtLength`;
  `Tessellate`, `ComputeArea`, `CompareWithGeometry`,
  `CombineWithGeometry` (union, intersect, xor, exclude) and `Outline`
  cut the flattened shapes into horizontal slabs and join the result back
  into clean outlines.
- **Strokes** (`stroke.c`): each figure is flattened, cut into dashes
  (solid, dash, dot, dash-dot, dash-dot-dot and custom, with an offset),
  and outlined: miter (with its limit, clipped or bevelled), bevel and
  round joins; flat, square, round and triangle caps; separate start, end
  and dash caps.  `Widen`, `GetWidenedBounds` and `StrokeContainsPoint`
  use the same outline.
- **Rasterizer** (`raster.c`): exact-area anti-aliasing (each edge adds
  the area it covers to the cells it crosses, and a running sum gives
  every pixel's coverage), nonzero and even-odd fills, aliased mode, in
  bands.  Paint is source-over into premultiplied BGRA.
- **Brushes** (`brush.c`): solid; linear and radial gradients (with the
  gradient origin offset, clamp, wrap and mirror, sRGB or linear-light
  interpolation, from a 256-entry table); bitmap brushes with nearest or
  linear sampling and extend modes; bitmaps from memory or a WIC bitmap.
- **Render targets** (`target.c`): HWND targets (a DIB section copied to
  the window at `EndDraw`, `Resize`), DC targets (`BindDC`) and bitmap
  targets (`CreateCompatibleRenderTarget`), with DPI, transforms,
  axis-aligned clips, layers (opacity, geometric masks, opacity brushes),
  `FillOpacityMask`, meshes, drawing state blocks and
  `ID2D1GdiInteropRenderTarget`.
- **Text** (`text.c`): `DrawGlyphRun` fills the outlines from
  `IDWriteFontFace::GetGlyphRunOutline`; `DrawTextLayout` runs
  `IDWriteTextLayout::Draw` with Direct2D's text renderer (drawing effects
  that are brushes colour their ranges, underlines and strikethroughs are
  drawn); `DrawText` lays the text out with the shared DirectWrite
  factory first.
- **`tools/d2dtest`** (MinGW, so it uses MinGW's `d2d1.h` like any
  Windows program) checks geometry answers, draws a scene of fills,
  strokes, gradients, a dashed path of arcs, a bitmap and a rotated
  translucent rectangle, and compares it with the image
  `tools/d2dtest/reference.py` draws with Skia: under QEMU the mean
  difference is 0.24 of 255 per pixel.  It then checks layers, clips and
  an HWND render target in a window.  It runs in the CI graphics suite,
  64- and 32-bit.  Its text check draws "NovaOS" with `DrawText` and
  saves it as `d2dtext.bmp`.  DirectWrite's text formats and layouts (on
  HarfBuzz from `novatext.dll`) were written here and went in with the
  Firefox work, and with them the check passes, 64- and 32-bit.

## DirectSound, XAudio2 and MIDI

Phase 19.5.  Three more ways for programs to make sound, all mixed by the
kernel mixer like `waveOut` and WASAPI.

- **DirectSound** (`dsound.dll`, NovaOS's own): `DirectSoundCreate`,
  `DirectSoundCreate8`, the enumerators and the COM classes.  The primary
  buffer is the kernel stream; each secondary buffer (any PCM or float
  format, static or streaming, looping, volume, pan and frequency, position
  notifications) is converted and mixed into it by a thread that stays a
  few milliseconds ahead of the card.  `DirectSoundCapture` records through
  a capture stream into the program's ring, with notifications.
- **XAudio2** 2.7, 2.8 and 2.9 and **X3DAudio** (`xaudio2_7.dll`,
  `xaudio2_8.dll`, `xaudio2_9.dll`, `x3daudio1_7.dll`) on FAudio 26.07
  (zlib).  NovaOS supplies FAudio's platform layer, which renders a quantum
  at a time into a kernel stream (folding surround to stereo), and a COM
  layer that gives each version its own vtables: voice and engine callbacks,
  sends, effect chains (the built-in reverb and volume meter, and a
  program's own XAPOs, adapted in both directions), and XAudio2 2.7's
  `CoCreateInstance` classes.
- **MIDI** in `winmm`: `midiOut` (short and system-exclusive messages, GM,
  GS and XG resets, volume), `midiStream` (tempo, time division, callbacks,
  position) and the MCI sequencer (`open`, `play`, `pause`, `seek`,
  `status` and the rest, by string or by `mciSendCommand`), playing through
  TinySoundFont (MIT).  Its instruments come from `gm.sf2`, a 75 KB General
  MIDI soundfont that `tools/make_gm_soundfont.py` builds from looped
  single-cycle waves, one per instrument family, plus a drum kit.

New self-tests play and record each one, on x64 and x86, and check that the
recording has the tones: `soundtest dsound` and `soundtest dscapture`,
`xa2test` (XAudio2 2.9 with callbacks and a volume meter, 2.7 through COM
with a submix voice, and X3DAudio panning) and `miditest`.

The first CI runs of this work never got past the build: with
`-fasync-exceptions`, clang 18 inlined the `__try` that checks a MIDI
handle into `midiOutClose` along with the synthesizer's cleanup, and its
x86-64 instruction selector never finished that function.  The check is now
kept out of line (`__declspec(noinline)`), and `midi.c` compiles in about
two seconds.

## One file per item: parallel changes without merge conflicts

Up to seven pull requests were open at once, and nearly every one edited
the same lines of the same files: the DLL table and program sets in
`tools/build_userland.py`, the test lists in `tools/selftest.py` and
`tools/appcorpus.py`, and the lists in the README, `docs/HISTORY.md`,
`docs/ROADMAP.md` and `docs/building.md`.  Each merge left the others
conflicted, and twice conflict markers reached main.  Those lists are now
directories with one file per item, so changes add files instead of
editing shared lines.  [CONTRIBUTING.md](../CONTRIBUTING.md) is the guide.

- **DLLs**: `userland/NAME/dll.json` registers each DLL (dependencies,
  load addresses, extra source directories, entry point, implicit TLS,
  export ordinals); `tools/build_userland.py` finds them and links each
  after its dependencies.  A DLL that needs more (Mbed TLS for secur32,
  the msvcrt/ucrtbase double link) keeps that code in its own
  `userland/NAME/build.py`.  The existing DLLs keep their addresses; a
  new DLL leaves them out and gets a free 16 MiB slot, so two branches can
  no longer pick the same address (three open ones had all chosen
  0x7FFE50000000), and the build stops if two DLLs' images overlap.  The
  userland it builds is byte-for-byte the same as before (link timestamps
  aside).
- **Programs**: `userland/programs/NAME.json` replaces the 32-bit and
  System32 sets (`x86`, `system`, extra `libs`, `selftest`).
- **Tests**: one file per self-test in `tests/selftest/core/` and
  `graphics/`, one per program in `tests/appcorpus/`, run in file-name
  order; `tools/selftest.py --list` prints a suite.
- **Docs**: README's program table, "What is inside" list, licences,
  core-suite and self-test lists, the roadmap's "What comes next" items,
  building.md's self-test table and every HISTORY section are built by
  `tools/docgen.py` from files in `docs/readme/`, `docs/roadmap/`,
  `docs/selftests/` and `docs/history/` (and the tests' `DOC` strings).
  Pull requests add fragments and leave the generated regions alone; the
  Docs workflow (`.github/workflows/docs.yml`) rebuilds them on main after
  each merge.
- **CI**: a quick Checks job runs before the boot tests: no conflict
  markers or stray branch-name lines (`tools/ci/check-conflict-markers.py`),
  the manifests and test files load, and a pull request has not edited a
  generated region by hand.

## ffmpeg

Phase 19.3.  `tools/pe_imports.py` on a current Windows ffmpeg build
(BtbN's, statically linked, 168 MB) listed nine missing functions, now
added:
- gdi32: `ExtCreateRegion` (kept as a bounding box like every region),
  `GetGraphicsMode`, `Get`/`Set`/`ModifyWorldTransform` (kept per DC and
  reported back; drawing stays in device coordinates),
  `GetOutlineTextMetricsA`/`W` (from the font's head, hhea, OS/2, post and
  name tables) and `GetFontUnicodeRanges`.
- The CRT: `getenv_s` and `_wgetenv_s`.
- ws2_32: `WSASendMsg` (control data is not carried).
The tenth, `DWriteCreateFactory`, is the Firefox branch's DirectWrite.

Running it found three kernel bugs, all from ffmpeg's many threads:
- **Thread stacks**: new threads got a fixed 256 KB stack.  They now get at
  least the image's stack reserve, as on Windows (2 MB for ffmpeg, whose
  H.264 decoder threads overflowed the smaller stack into each other's).
- **Lost sleeps**: a thread woken early from a timed sleep stays on its
  CPU's sleep list until that CPU's next tick drops it.  A thread that
  exited in between was freed while still on the list, and every sleeper
  after it was lost: their `Sleep` never returned.  An exiting thread now
  leaves the list first, and a thread that moved to another CPU leaves the
  old CPU's list before sleeping on the new one.
- **Handles**: a process could hold 256 handles.  winpthreads makes events
  and semaphores for every mutex and condition variable, so ffmpeg ran out
  ("Cannot allocate memory", "Resource temporarily unavailable").  The
  limit is 4096.

`ffmpeg -i in.mp4 out.webm` (H.264 and AAC in, VP9 and Opus out) completes
under QEMU with its default threads, as do MPEG-4 encoding and 16 decoder
threads; the result plays on Linux.  The process dump on Ctrl+C now shows
each thread's scheduler state.  The nightly app corpus runs Gyan's ffmpeg
7.1.1 build (`tests/appcorpus/080-ffmpeg.py`): it makes an MP4 from its
test sources, converts it to WebM, and ffprobe must find VP9 and Opus.

## Firefox (Floorp)

Floorp 12.19, a Firefox build (the Firefox 157 engine), starts from the
Terminal, creates its profile and draws its full browser window.  The
browser is run as shipped; everything below is in NovaOS.

- **Imports**: the C runtime pieces Gecko uses (`_wsetlocale` and the
  rest), the delay-loaded DLLs it asks for, and cross-process
  `NtQueryInformationProcess`.
- **DirectWrite** (`userland/dwrite`): NovaOS's own `dwrite.dll`.  The
  factory, the system font collection (scanned from `%WINDIR%\Fonts`,
  with the common Windows family names mapped to the bundled fonts),
  font families, fonts, font faces (metrics, glyph indices, advances,
  kerning, outlines into a geometry sink, font tables), GDI interop and
  glyph run analysis (aliased and ClearType alpha textures).  Fonts are
  read with stb_truetype (public domain).  Text formats and text layouts
  (`layout.c`, written in the Phase 19 work for Direct2D's `DrawText`)
  break lines, handle bidirectional text, carry per-range font
  attributes, and answer metrics and hit tests.  They shape with
  HarfBuzz from `novatext.dll` when it is present, and with the font's
  plain glyphs and advances otherwise.
- **Kernel**: `NtQuerySection`, `MEM_RESET`/`MEM_RESET_UNDO`, a
  per-process handle table of 4096 (Gecko keeps far more than the old
  256 open), and `C:\AppData\Roaming`, `Local`, `LocalLow` and
  `C:\ProgramData` made at boot.
- **C runtime**: `_vsnwprintf` (the legacy option of
  `__stdio_common_vswprintf`) now fills a buffer exactly, without the
  terminator, when the output is exactly the buffer's size.  Gecko formats
  its 16-digit install hash that way; returning -1 made the profile
  service fail and Firefox show "Profile Missing".
- **user32**: window class names up to 256 characters (Gecko's remote
  window class contains the profile path).
- **Window station security**: the sandbox's alternate desktop reads and
  sets the window station's and desktop's security with `GetSecurityInfo`
  and `SetSecurityInfo` (`SE_WINDOW_OBJECT`).  Those are user32 pseudo
  handles, so they answer with the default descriptor instead of failing;
  the failure had left the broker without a desktop and crashed it.
- **Debugging aids**: the kernel prints each new process's command line;
  `tools/novarun.py` takes `!bg COMMAND` to leave a program running while
  it waits and takes screenshots, and `NOVARUN_GDB=1` starts QEMU with a
  gdb server so breakpoints can be set in a program's code.
- **Sandbox** (Chromium's, which Firefox uses for its child processes):
  - ntdll's system call exports have the Windows byte layout (see the
    ABI conformance work below), so the sandbox can copy and patch them
    to intercept calls in the child.
  - Token handles know whether they are primary or impersonation tokens
    and at which level; `SetThreadToken`, `OpenThreadToken`,
    `ImpersonateSelf` and `RevertToSelf` track a token per thread.
    `CreateWellKnownSid` covers every well-known SID type.
  - New system calls: `NtOpenProcessToken(Ex)`, `NtOpenThreadToken(Ex)`,
    `NtImpersonateAnonymousToken`, `NtQueryFullAttributesFile`,
    `NtSetInformationProcess`, and the `ProcessHandleCount` and
    `ProcessHandleTable` classes of `NtQueryInformationProcess`.
  - `CREATE_SUSPENDED` really suspends a new process, so the parent can
    patch the child before it runs.
  - ntdll exports the heap and string functions the sandbox resolves in
    the child (`RtlCreateHeap`, `NtSignalAndWaitForSingleObject`,
    `_strnicmp`, `wcslen`...), and `GetProcessHeaps` includes an empty
    csrss port heap the sandbox expects to find before it cuts a content
    process off from csrss.
  - A process can have 256 threads (was 64); Firefox's main process runs
    more than 64.
- **Overlapped I/O**: a pipe read or write that fails at once (a broken
  pipe when a child process exits) no longer sets its event or queues a
  completion packet or routine; Windows does none of these, and Firefox's
  IPC and Rust I/O free the `OVERLAPPED` after such a failure, so the late
  packet crashed the main process with a use-after-free.
- **GDI**: `CreateDIBSection` with a file-mapping handle puts the pixels in
  that mapping (Firefox's GPU process draws the browser into one shared
  with the main process).
- **Window handles across processes**: an `HWND` now names the same
  window in every process, as on Windows.  user32 builds each handle from
  a tag the kernel gives the process (unique among running processes), so
  handles never collide, and tells the kernel each desktop window's handle
  and client area.  `IsWindow`, `GetClientRect`, `GetWindowRect`,
  `ClientToScreen`, `ScreenToClient`, `IsWindowVisible`, `IsIconic`,
  `IsZoomed` and `GetWindowThreadProcessId` answer for another process's
  window.  Firefox's GPU process sizes its frames from the main process's
  window; before, it saw a 0 x 0 window and never drew, so the browser
  showed white.  The full browser now draws through the GPU process.
- **Locks that sleep**: `WaitOnAddress`, SRW locks and condition
  variables park the thread until another wakes it, the way Windows 8
  and later do: ntdll lists the waiters per address and they sleep in the
  new `NtWaitForAlertByThreadId` system call until a waker calls
  `NtAlertThreadByThreadId`.  They used to poll every 10 ms, which left
  Firefox's main thread too slow to read its input.  Also
  `SleepConditionVariableSRW`/`CS` return FALSE with `ERROR_TIMEOUT` when
  they time out, as on Windows.
- **Drawing from another thread**: `ReleaseDC` shows what was drawn at
  once even while part of the window waits for `WM_PAINT` (Firefox
  presents from its own thread while the window may never stop being
  invalidated).
- **Debugging aids**: Ctrl+Alt+F12 writes every program's threads to the
  serial log (state, last system call and its first argument, return
  addresses on the stack); the syscall trace shows the thread id
  (`[TRACE] name pid/tid`); the standard error of a detached process
  (Firefox's sandboxed children) goes to the serial log; and
  `tools/novarun.py` takes `!click X Y`.
- Not yet: a page's content (the tab area stays empty) and fetching a page
  over the network.

## ICU: .NET globalization and kernel32's locales

.NET 5 and later do their globalization through ICU when
`C:\Windows\System32\icu.dll` loads, and fall back to NLS otherwise;
until now NovaOS had no `icu.dll`, so .NET programs only knew English and
the invariant culture.  NovaOS now ships ICU the way Windows 10 does.

- **`icu.dll`** (`third_party/icu`, ICU 77.1, Unicode License v3): one
  DLL per architecture (System32 and SysWOW64) holding ICU's common and
  i18n libraries, exporting ICU's C API (1193 functions) under unversioned
  names (`ucol_open`, not `ucol_open_77`), which is what .NET looks up.
  `tools/build_icu.py` builds it from the official ICU4C source with
  MinGW-w64; the outputs are committed.
- **ICU's data** is a separate file, as on Windows:
  `C:\Windows\Globalization\ICU\icudt77l.dat`, which `icu.dll` points ICU
  at when it loads (`tools/icu/nova_icu.c`; `ICU_DATA` overrides it).  It
  is ICU's own data less legacy code-page converters, the word-break
  dictionaries, transliteration, unit names and character names (18 MB
  instead of 31).  Every culture .NET knows (1301, plus the time zones)
  gives the same names, formats and comparisons with the trimmed data as
  with the full data.
- **kernel32's locales** (`userland/kernel32/locale.c`): .NET on Windows
  also asks `GetLocaleInfoEx(name, LOCALE_SNAME)` whether Windows knows a
  culture before it uses ICU's, and kernel32 knew only English.  It now
  has the table of all 864 Windows locales (`locale_data.h`, generated by
  `tools/gen_locales.py` from .NET's MIT-licensed `IcuLocaleData.cs`):
  names, LCIDs, ANSI/OEM/Mac/EBCDIC code pages, GEOIDs, list separators,
  parents.  `LocaleNameToLCID`, `LCIDToLocaleName`, `IsValidLocale(Name)`,
  `ResolveLocaleName` and `EnumSystemLocales(A/W/Ex)` use it, and for
  locales other than English `GetLocaleInfoEx`/`W`/`A` answer from ICU:
  names in the locale's own language and in English, number and currency
  symbols and patterns, grouping, day and month names (genitive too),
  short and long dates, times, year-month and month-day formats, first day
  of the week, measurement system and paper size (the date-pattern and
  number-pattern conversions follow .NET's own, MIT).  English answers
  from the same tables as before.
- **msvcrt** exports the `_timezone`, `_daylight` and `_tzname` variables
  (MinGW-built DLLs such as `icu.dll` import them).
- **Smaller kernel image**: system files of 1 MiB or more (ICU's data and
  DLLs, NetSurf) are embedded zlib-compressed and inflated onto drive C:
  at boot, so `kernel.elf` is 27 MB rather than 48 MB with ICU in it.
- **Stack overflows** a program handles itself (the .NET runtime prints
  "Stack overflow." and exits) are logged with where they happened.
- **Tests**: `icutest` (in the CI core suite, 64- and 32-bit, 56 checks)
  loads `icu.dll` as .NET does and checks German and Japanese display
  names, numbers, currencies, dates, the Japanese calendar's era,
  collation, Turkish and German case mapping, Windows-to-IANA time zones,
  IDNA, NFC, eight threads at once, and kernel32's answers for those
  locales.  The nightly app corpus now runs .NET 10.0.12 (from NuGet) with
  `tests/dotnet/culturetest.dll`: `de-DE` gives `1.234.567,89`,
  `1.234,50 €`, `Freitag, 2. Oktober 2026`, `02.10.2026`; `ja-JP` gives
  `￥1,235`, `2026年10月2日金曜日`, `2026/10/02`, `日本語 (日本)`; German
  sorts `ä` with `a`, Japanese compares kana and widths, all through ICU.
- Not yet: `GetDateFormat`, `GetNumberFormat` and `GetCurrencyFormat`
  still format the English way for every locale, and the user's locale is
  always `en-US`.  (Both came with "Locale formatting and the user
  locale".)

## Older USB controllers: EHCI, OHCI and UHCI

USB devices now work on every kind of PC USB controller, not only xHCI,
and on any number of controllers at once, so machines from before about
2012 (and virtual machines that emulate only USB 1.1 or 2.0) get their
keyboards, mice, hubs and sticks too.

- **USB core** (`kernel/drivers/usb.c`, `usb_hc.h`): what used to be part
  of the xHCI driver and is the same for every controller moved into a
  core of its own: enumeration (SET_ADDRESS, or xHCI's Address Device),
  descriptors, a pipe per endpoint, offering interfaces to the hub, HID and
  mass-storage drivers, taking devices away, and the `usb` thread that
  watches the root ports.  A controller driver supplies root-port reset,
  control, bulk and interrupt IN transfers, and its own per-device and
  per-endpoint state (`UsbHcOps`).  The class drivers (`usb.h`) are
  unchanged.
- **xHCI** (`xhci.c`): its state moved into a per-controller structure,
  so every xHCI controller is started, each a bus of its own (`usb1 port
  3` in the log).  On Intel 7- to 9-series chipsets it takes the shared
  USB 2 ports over from EHCI, as Windows does.
- **EHCI** (`ehci.c`, USB 2): a queue head per endpoint, control and bulk
  on the asynchronous ring and interrupt ones on a periodic list that
  every frame points at; transfers are chains of qTDs (20 KiB each) and a
  short packet ends a bulk or interrupt transfer through an inactive
  "stop" qTD.  Full- and low-speed devices on a root port are passed to
  the companion controller that shares the port (`PORT_OWNER`); behind a
  high-speed hub they go through the hub's transaction translator (split
  transactions).  The firmware's legacy support is turned off first.
- **OHCI** (`ohci.c`, USB 1.1): an endpoint descriptor per endpoint on the
  control, bulk or interrupt list, transfers queued behind a dummy TD
  (8 KiB per TD), the data toggle kept in the ED, and a short packet in
  the middle of a bulk transfer ending it.  The controller is taken from
  the firmware's SMM driver (`OwnershipChangeRequest`).
- **UHCI** (`uhci.c`, USB 1.1): skeleton queue heads for interrupt,
  control and bulk transfers behind every frame, a queue head per
  endpoint, a TD per packet with the data toggle kept by the driver, and
  short-packet detection that ends a transfer (or moves a control
  transfer on to its status stage).  Legacy keyboard emulation is turned
  off (`USBLEGSUP`).
- All of them are polled from the timer tick like the rest of NovaOS's
  drivers (their interrupts stay off); an interrupt endpoint is polled
  each frame.  Controllers start in the order that lets EHCI hand devices
  to its companions: xHCI, EHCI, then OHCI and UHCI.  Before sleep (S3)
  every xHCI controller arms its root ports to wake the machine (Phase
  18.6's keyboard wake, now a controller operation, `prepare_sleep`);
  after it each controller is reset and its devices enumerated again.
- **Keyboard LEDs**: Num Lock, Caps Lock and Scroll Lock follow the lock
  keys (`InputLockState`, which now tracks Num and Scroll Lock too).  The
  report descriptor's LED outputs are found (the boot-protocol descriptor
  gained them), and the `usb` thread sends an output report
  (`SET_REPORT`) to every keyboard when the state changes.
- Tested in QEMU 8.2 (`-machine q35,i8042=off`, so typing goes over USB)
  with a keyboard, a tablet and a FAT stick (`dir`, `type`) on each of
  `qemu-xhci`, `usb-ehci`, `pci-ohci` and `piix3-usb-uhci` (on OHCI and
  UHCI the stick sat behind the hub QEMU adds when the root ports run
  out); an ICH9 EHCI with three
  UHCI companions (a full-speed hub with keyboard and mouse passed to a
  companion, a high-speed stick and tablet on EHCI); EHCI with an OHCI
  companion and full-speed keyboards (`usb_version=1`) unplugged and
  plugged in on other ports; two xHCI controllers with the keyboard moved
  from one to the other; devices added and removed while running on
  each; `sleeptest` on xHCI (two controllers), OHCI and ICH9 EHCI + UHCI
  with every device back after waking.  Caps Lock reached the keyboards
  as `SET_REPORT` output reports (QEMU's USB packet capture).
- Not yet: isochronous transfers (webcams, USB audio), interrupt
  endpoints are polled every frame whatever their interval, and split
  transactions through a high-speed hub's transaction translator are
  untested (QEMU's EHCI has none).  Unplugging a hub from a port that EHCI
  passed to a UHCI companion aborts QEMU 8.2 (an assertion in its USB
  core when it hands the port back); unplugging other devices there, or
  a hub elsewhere, works.

## USB hubs and report-protocol HID (Phase 18.1)

- **USB core** (`kernel/drivers/xhci.c`, `usb.h`): devices are enumerated
  on root ports and behind hubs (the slot context carries the route
  string, the root port and, for low and full speed devices behind a high
  speed hub, the transaction translator).  Every endpoint of the
  configuration gets a ring in one Configure Endpoint, then
  SET_CONFIGURATION, and each interface is offered to the class drivers,
  which open pipes: interrupt IN with a completion callback, and bulk
  transfers that wait.  A device that leaves takes everything behind it
  with it; its drivers' `gone` callbacks run on the `usb` thread.
- **Hubs** (`usbhub.c`): USB 2 and USB 3 hubs.  Ports are powered, the
  status-change endpoint says which port changed, and the `usb` thread
  reads its status, debounces, resets it and enumerates the device.
- **HID** (`usbhid.c`): report protocol.  The report descriptor is parsed
  into fields (report IDs, usage pages, arrays and bitmaps, push/pop);
  keyboards report keys held (array or bitmap), mice relative X/Y, wheel
  and buttons, tablets and touch screens absolute X/Y (scaled to the
  screen) with a button or the first contact's tip switch.  Boot-class
  devices whose report descriptor can't be used fall back to boot
  protocol.  Absolute pointers move the cursor to a position
  (`InputEvent.absolute`, `WmCursorMoveAbs`).
- Tested in QEMU (`-machine q35,i8042=off -device qemu-xhci`) with a
  `usb-hub` on port 1 holding a `usb-kbd` and a `usb-mouse`, and a
  `usb-tablet` on port 2: typing in Terminal, relative motion and absolute
  positions all work; the keyboard was unplugged and a new one added on
  another hub port, and the tablet unplugged and added on a third root
  port, with `device_del` / `device_add`.
- Not yet: multi-touch (done since: see "Multi-touch").  (Keyboard LEDs,
  more than one controller and the older UHCI/OHCI/EHCI controllers: see "Older USB controllers".)

## USB mass storage (Phase 18.2)

- **Bulk-Only Transport + SCSI** (`kernel/drivers/usbmsc.c`): INQUIRY,
  TEST UNIT READY, REQUEST SENSE, READ CAPACITY (10 and 16), READ/WRITE
  (10 and 16) and SYNCHRONIZE CACHE, with stall handling and reset
  recovery.  Each stick becomes a removable block device (`usb0`, `usb1`,
  ...); `BlockUnregister` takes it off the list when it is pulled.
- **Drives** (`kernel/fs/drives.c`): a removable disk's FAT12/16/32 and
  NTFS volumes (whole disk, MBR or GPT) are mounted as the next free drive
  letter when it arrives, and unmounted when it goes
  (`RamfsUnmountDrive`: nodes still held stay valid but read nothing).
  Fixed disks still only mount NTFS, since their FAT volumes are NovaOS's
  own.  Mounts are read-only for now.
- **File Explorer** lists the other drives (D: to Z:) under the places in
  its sidebar, names them by label in the title and breadcrumb, and goes
  back to This PC when the drive it shows is unplugged.
- Tested in QEMU with a FAT32 (MBR) `usb-storage` stick present at boot
  (`dir`, `type`, Explorer, Notepad), unplugged and plugged back in with
  `device_del` / `device_add`, and an NTFS stick added while running.

## NVMe disks (Phase 18.3)

- **Driver** (`kernel/drivers/nvme.c`): resets each NVMe controller, sets
  up an admin queue and one I/O queue pair (64 entries, polled through the
  completion phase bit), identifies the controller and its active
  namespaces, and registers each namespace with 512-byte blocks as a
  block device (`nvme0n1`, ...).  Reads and writes go through a 128 KiB
  bounce buffer described by PRP entries or a PRP list; Flush backs
  `BlockDev.flush`.  After S3 the controllers are reset and their queues
  created again.
- **Install**: NVMe disks are found with the SATA disks (`PersistInit`),
  so Setup lists them, a blank one holds drive C: in a live session, and
  NovaOS installs to them.
- Tested in QEMU/OVMF with a blank 1 GB `-device nvme` and the ISO:
  Setup installed onto `nvme0n1`, then the NVMe disk alone booted
  (OVMF's `UEFI QEMU NVMe Ctrl` entry), and a file written in Terminal was
  saved to its NOVADATA partition.

## NTFS write (Phase 18.4)

- **Writer** (`kernel/fs/ntfs.c`, NovaOS's own code: ntfs-3g and the
  Linux ntfs3 driver are GPL): rewrites a file's data (resident when it
  fits in its MFT record, else in newly allocated clusters, the old ones
  freed once the record points at the new ones), creates files and
  directories (a `$STANDARD_INFORMATION` carrying the parent's security
  id, a Win32 `$FILE_NAME`, an empty `$DATA` or `$I30` index), renames
  and moves them, and deletes files and empty directories (a file with
  other hard links only loses the name).  Directory indexes are rebuilt
  whole on each change, as a B+ tree of INDX blocks packed bottom up
  (`$UpCase` collation), in the index root alone while it fits; the
  `$INDEX_ALLOCATION` grows, and is given back when the index fits in the
  root again.  The MFT grows 64 records at a time.  The cluster and MFT
  bitmaps are kept in memory and written through; records 0-3 are
  mirrored to `$MFTMirr`.  There is no journal: `$LogFile` is emptied when
  writing starts (as `ntfsfix` does) and each call leaves the volume
  consistent.  Files in an attribute list, and compressed, sparse or
  encrypted files, are not rewritten: the first time something asks
  whether such a file is writable its record is read, and it shows as
  read-only from then on (the directory's copy of its attributes can't be
  trusted for this; ntfs-3g leaves it stale).
- **When it is writable**: a volume Windows left hibernated (a
  `hiberfil.sys` starting `hibr`, which Fast Startup leaves too), marked
  dirty, or with unfinished transactions in `$LogFile` stays read-only.
- **Drives** (`kernel/fs/drives.c`, `ramfs.c`): `RamfsSource` gained
  create, remove, rename, write and free-space calls.  Creating, deleting
  and renaming on a writable drive go to the disk at once; a file's new
  contents are written when nothing holds it any more, after a quiet
  second (`DrivesPoll`), and at shutdown (`DrivesSync` from `UmSaveAll`).
  Volume information reports the real free space and drops
  `FILE_READ_ONLY_VOLUME`.  Renames within drives D:, E:, ... are real
  renames now (they were refused as "another device", so `MoveFile`
  copied and deleted).
- **Checks**: `drivetest` now writes (small, appended and 700 KB files,
  overwrites, renames, moves, a case change, a folder of 300 files, then
  deletes) and leaves two files for the host; `scripts/check-ntfs-disk.sh`
  runs `ntfsfix -n` and `scripts/ntfs-check.py`, a chkdsk-style check of
  the cluster and MFT bitmaps against what the records own, the records'
  headers, attributes and link counts, every `$I30` index (order, leaf
  depth, VCNs, index bitmap, entries against the `$FILE_NAME`s) and
  `$LogFile`.  It reports no errors on volumes mkntfs and ntfs-3g made,
  and none after drivetest and Terminal tests (MFT growth, a 90-file
  folder split across INDX blocks and shrunk back, a USB stick written to
  and pulled out without a sync) wrote to them.
  Windows `chkdsk`, run on a disk NovaOS had written to (3 October),
  reported no errors either.

## NTFS as drive C: and file ACLs (Phase 18.5)

Drive C: can now be kept on NTFS, with each file's security descriptor,
and the kernel enforces those DACLs.

- **`$Secure` writing** (`kernel/fs/ntfs.c`): `NtfsAddSecurity` stores a
  descriptor once (found again by hash and bytes), appending it to `$SDS`
  and its mirror 256 KiB on, never across a 256 KiB block, and adds it to
  the `$SII` and `$SDH` view indexes; `NtfsSetSecurityId` points a file's
  standard information at it.  The index code takes any named index now,
  view-index roots are kept small so record 9 doesn't fill, `$SDS` grows
  64 KiB at a time, allocations extend a stream's last run when they can,
  and attributes of one type are kept in name order (ntfs-3g looks them up
  that way).  `NtfsSetInfo` saves times and attribute bits and
  `NtfsLookup` finds a name in a folder.  New volumes give the root
  `CREATOR OWNER` full control, inherited, as Windows' C:\ has.
- **File security** (`kernel/fs/fsec.c`): a RamNode may carry a
  self-relative descriptor; one without inherits from the nearest folder
  above that has one (object- and container-inherit ACEs, no-propagate,
  `CREATOR OWNER`).  `FsecAccess` checks a request against the user's
  token SIDs with the file generic mapping; the owner can always read and
  change the DACL.  With no descriptor anywhere above, as on FAT, anyone
  may do anything.
- **Syscalls** (`kernel/um/um_syscall.c`): opening, creating, overwriting,
  deleting, renaming and setting basic information check the DACL (delete
  falls back to the parent's `FILE_DELETE_CHILD`; `MAXIMUM_ALLOWED` gets
  what is granted).  A descriptor given in `OBJECT_ATTRIBUTES` (from
  `CreateFile`'s or `CreateDirectory`'s `SECURITY_ATTRIBUTES`) is applied
  to the new file.  `NtQuerySecurityObject` (0x155) and
  `NtSetSecurityObject` (0x1A1, their Windows 10 1903 numbers) moved
  from ntdll into the kernel; other
  handles still get the default descriptor.  advapi32's
  `Get/SetNamedSecurityInfo`, `Get/SetSecurityInfo` and
  `Get/SetFileSecurity` are real now.
- **Persistence** (`kernel/fs/persist.c`): an NTFS volume labelled
  `NOVADATA` can hold C:.  C: is its root (on FAT it stays `\NOVA\C`),
  NovaOS's own bookkeeping goes in a hidden `$NovaOS` folder, and each
  file is saved with its times, attributes and security id (one that
  inherits gets the root's id, read back as "inherit").
- **Installer**: the confirm page asks how to keep drive C:, NTFS
  (recommended) or FAT32; NTFS data partitions have no 1 TiB cap.
- **Checks**: `acltest` gained file tests on C:\AclTest (inheritance,
  deny write and delete, rename, `MAXIMUM_ALLOWED`, the owner restoring
  access, a DACL from `SECURITY_ATTRIBUTES`) and leaves `kept.txt`, whose
  DACL a second run after a restart checks.  `scripts/ntfs-check.py`
  checks `$SII`, `$SDH` and `$SDS` against each other and attribute order;
  `check-ntfs-disk.sh` also runs `ntfssecaudit -a`.  In QEMU, NovaOS was
  installed with C: on NTFS to a blank NVMe disk and booted from it;
  `acltest` passed 33 of 33 on both boots and `kept.txt` kept its DACL.
  ntfs-check, `ntfsfix -n` and `ntfssecaudit` found no errors on the
  partition, nor on a host test volume with 5,100 descriptors.
- **Not done**: hard links (`CreateHardLink`) are still to come.

## ACPI: SCI interrupt, lid, thermal zones, wake devices, _PRT (Phase 18.6)

- **The SCI is an interrupt** (`kernel/hal/ioapic.c`): the I/O APICs come
  from the MADT, every input is masked at boot, and the SCI (FADT
  `SCI_INT`, with the MADT's interrupt source override) is routed to vector
  0x32 on the boot CPU.  Its handler runs uACPI's, which queues GPE methods
  and `Notify` handlers and wakes the `acpi` thread to run them.  The
  thread still calls the handler once a second in case an edge was missed,
  and polls every 100 ms as before when there is no I/O APIC.  The routes
  are put back after S3.  It is the only device interrupt; the drivers
  still poll.
- **PCI interrupt routing**: with an I/O APIC, `\_PIC(1)` selects APIC
  mode and the root bridge's `_PRT` is read, link devices (`PNP0C0F`)
  resolved through their `_CRS`; `AmlPciIrq` answers which GSI a pin is
  wired to, and each function on bus 0 gets it in its Interrupt Line
  register.  (QEMU's q35: 128 entries, all through links.)
- **The lid** (`PNP0C0D`): `_LID` is read at load, on `Notify 0x80` and
  after waking.  Closing it puts the machine to sleep, as Windows does by
  default.
- **Thermal zones**: `_TMP`, `_PSV`, `_HOT`, `_CRT` and `_TZP`, read every
  `_TZP` (at least every 10 s) and on `Notify 0x80`/`0x81`.  Crossing the
  passive trip point is logged (NovaOS can't throttle the CPUs yet);
  `_HOT` puts the machine to sleep and `_CRT` shuts it down, drive C: saved
  first.
- **Wake devices** (`_PRW`): the lid, power buttons and USB host
  controllers have their wake GPEs set up, and before S3 `_DSW` (or
  `_PSW`) tells them to arm.  USB keyboards that can are set for remote
  wakeup (`SET_FEATURE(DEVICE_REMOTE_WAKEUP)`), and before S3 the xHCI root
  ports enable wake on connect, disconnect and over-current, suspend
  (U3) the ports with a device and turn on PME#.  After waking the kernel
  logs which wake GPE (or the power button, or the RTC alarm) woke it.
- **For programs**: `NtPowerInformation` answers `SystemPowerCapabilities`
  (`LidPresent`, `SystemS3`, `ThermalControl`, batteries),
  `ThermalInformation` (the first zone; converted to the 32-bit layout in
  ntdll) and `LastSleepTime`/`LastWakeTime`; powrprof's
  `GetPwrCapabilities` and `CallNtPowerInformation` use them.
- **Self-test**: `tests/acpi/lid-thermal.asl` describes a lid, a thermal
  zone (40 C; passive 60, hot 90, critical 95 C) and the xHCI controller as
  a wake device, with QEMU's `pc-testdev` (ports 0xE8 and 0xE9, written
  from the QEMU monitor) as the embedded controller.  `powertest` checks
  the capabilities and readings, then asks the test to close the lid:
  NovaOS sleeps, the test opens the lid, presses a key on the USB keyboard
  and wakes the machine, and `powertest` sees `LastSleepTime` and
  `LastWakeTime` move on; then the zone is heated to 70 C (passive cooling
  on) and cooled to 45 C.  It runs in the CI boot, which now also has a
  USB keyboard on an xHCI controller (the self-tests are typed through
  it).  Also tested by hand: 91 C sleeps and 96 C shuts down.
- **Not done**: in QEMU a USB key can't wake the machine itself.  QEMU
  8.2 delivers the key to the suspended port (`xhci_wakeup`) but has no
  path from there to the platform, so the test wakes it with
  `system_wakeup` (which QEMU reports as the power button).  On a real
  PC a USB key press does wake it from S3 (checked 3 October).  Still
  unchecked: GPE block devices other than `\_GPE` and routing behind PCI
  bridges.

## HPET and one-shot/TSC-deadline timers (Phase 18.7)

- **HPET** (`kernel/hal/hpet.c`): found through the ACPI `HPET` table
  (read straight from the RSDP, before the rest of ACPI starts), its main
  counter started; it replaces the PIT as the reference that calibrates
  the TSC and the local APIC timer (the PIT stays the fallback).
- **The APIC timer is one-shot**, in TSC-deadline mode where the CPU has
  it.  Each timer interrupt re-arms it for the CPU's next 10 ms tick or
  the earliest TSC-deadline sleeper on that CPU, whichever is sooner, so
  the tick work stays at 100 Hz while sleeps end when they are due.
- **Sleeps and timed waits** (`NtDelayExecution`, `NtWaitFor*Object(s)`
  timeouts) sleep until a TSC deadline (`sched_sleep_until_tsc`) instead
  of whole 10 ms ticks.  A thread woken by its deadline goes to the front
  of its run queue and preempts a running thread of no higher priority.
- `sleeptest timer` measures how late `Sleep(1)`, `Sleep(5)` and a 1 ms
  wait timeout end, idle and with a busy thread on every CPU; it is in the
  core self-tests.  In QEMU (TCG, 2 CPUs) the 95th percentile under load
  was 0.26 ms late (it was 10 to 20 ms with the 100 Hz tick).
- **Fix (2026-10-03):** `sleeptest timer` failed in CI at 1.4 to 1.6 ms
  (95th percentile under load).  Two causes.  The keyboard and mouse
  polls (PS/2 and USB) ran in the timer interrupt, and their port and MMIO
  reads block for up to a few milliseconds under QEMU (it serializes
  device access), with interrupts off, so every sleep due on that CPU
  meanwhile ended late; they now run in a kernel thread (`devpoll`) woken
  by its TSC deadline at each tick, interrupts enabled.  And a timer
  interrupt taken while a CPU halted waiting for the kernel lock re-armed
  the timer for the 10 ms grid only, forgetting that CPU's TSC-deadline
  sleepers; it now re-arms for the soonest sleeper too.
- QEMU emulates the TSC-deadline timer only with KVM, so the self-tests
  exercise the one-shot mode; TSC-deadline mode is untested.
- Not yet: waitable timers (`SetWaitableTimer`) still fire on the 10 ms
  tick.

## IPv6, HTTP/2 and virtio-net (Phase 18.8)

- **virtio-net** (`kernel/drivers/virtio_net.c`): virtio 1.0 network
  adapters (QEMU `virtio-net-pci`), through the modern PCI capabilities;
  one receive and one transmit queue of 64 buffers, polled like the e1000
  driver.  The network stack tries the e1000 first, then virtio-net, and
  sets either up again after sleep.
- **IPv6** in lwIP: a link-local address, SLAAC addresses from router
  advertisements, DNS servers from RDNSS (added beside DHCP's, not in their
  place: a small change to `nd6.c`), MLD, and AAAA lookups (IPv6 first
  when there is a global IPv6 address).  `ipconfig` shows the addresses,
  `ping -6`, `curl -6` and `wget -6` force IPv6 (`-4` forces IPv4), and
  `curl http://[addr]/` takes literals.
- **Winsock over IPv6**: `AF_INET6` sockets (dual-stack: IPv4 peers show
  as v4-mapped), `sockaddr_in6` in `connect`, `bind`, `accept`,
  `sendto`/`recvfrom` and the name calls; `getaddrinfo` returns IPv6 and
  IPv4 addresses (with `AI_V4MAPPED`, `AI_CANONNAME`, numeric hosts with
  `%zone`, service names), and `inet_pton`/`inet_ntop`,
  `WSAStringToAddress`/`WSAAddressToString` and `getnameinfo` handle IPv6.
  `netcat` resolves with `getaddrinfo` and takes `-4`/`-6`/`-p`.
- **Loopback** (for Firefox, whose processes talk over a socket pair):
  lwIP's loopback interface is on, so 127.0.0.1, ::1 and the machine's own
  addresses reach its own sockets (the net thread delivers them).  A
  connection is given its socket as soon as it arrives, so bytes the client
  sends before `accept` wait for it instead of being dropped, and closing a
  listener no longer trips an lwIP assertion that stopped the network.
  `getaddrinfo("localhost")` gives ::1 and 127.0.0.1 and `gethostbyname`
  127.0.0.1, without DNS.  `looptest` checks all of it in the network suite.
- **winhttp.dll** (`userland/winhttp`) became a real HTTP client: sessions,
  connections and requests, request headers, request bodies
  (`WinHttpWriteData`), `WinHttpQueryHeaders` (by index, name, number or
  date), `WinHttpQueryDataAvailable`/`WinHttpReadData`, redirects, Basic
  credentials, `WinHttpCrackUrl`/`WinHttpCreateUrl`, options and time-outs,
  and asynchronous sessions that report through the status callback.
  HTTPS goes through Schannel (secur32); with
  `WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL` it offers `h2` by ALPN and speaks
  HTTP/2 through [nghttp2](https://nghttp2.org/) 1.64.0 (MIT, vendored in
  `third_party/nghttp2`) when the server picks it
  (`WINHTTP_OPTION_HTTP_PROTOCOL_USED` says which).  Each request opens its
  own connection; there is no proxy support.
- **Tests**: `tools/selftest.py --suite network` boots twice with a
  virtio-net card.  On QEMU's user network it checks `ipconfig`, `ping`,
  Winsock over IPv4 and `httptest suite` (`userland/programs/httptest.c`:
  HTTP/2 negotiated, a 300 KB body, POST, redirects, a refused untrusted
  certificate, chunked HTTP/1.1, the asynchronous API) against
  `tools/h2server.js` (Node).  On an IPv6-only network, `tools/v6peer.py`
  (a router, DNS and HTTP server reached through a QEMU datagram netdev,
  so the host needs no IPv6) checks SLAAC and RDNSS, `ping -6`, `curl -6`
  and Winsock over IPv6.
- Also fixed: the USB hot-plug thread could be enumerating when the
  machine went to sleep, so after the wake its command timed out and the
  USB keyboard did not come back (an intermittent `powertest` failure).
  Sleep now waits for it, and it stays idle until the controller is
  running again.

## Display persistence (Phase 18.9)

- **The chosen resolution survives a restart.**  Choosing a mode in
  Settings > Display, or `ChangeDisplaySettings` with
  `CDS_UPDATEREGISTRY`, saves it where Windows keeps it,
  `HKLM\SYSTEM\CurrentControlSet\Control\Video\{NovaOS-Display}\0000`
  (`DefaultSettings.XResolution`, `YResolution`, `BitsPerPel`), which
  reaches the disk with the rest of drive C:.  At boot, once drive C: and
  the registry are loaded and before the desktop starts, the kernel
  switches to that mode if the adapter has it (`[DISPLAY] Restored the
  saved mode WxH`); on another adapter without it, NovaOS stays in the
  boot mode.
- **Windows grow back.**  A window a smaller mode shrank or pushed aside
  remembers the frame it had and gets it back when a later mode has room
  for it, unless it was moved or resized in between.  Maximized windows
  already followed the work area.
- `disptest` checks both (46 checks): a 1000x640 window shrinks to fit
  800x600 and comes back to its size and place, and `CDS_UPDATEREGISTRY`
  writes the registry values.  The core self-tests save 1024x768 with
  `disptest 1024 768`, and after the suite's restart (`shutdown /r`)
  `disptest saved 1024 768` passes only if NovaOS came up in that mode.
- Not yet: a per-monitor layout (NovaOS drives one display).  (Done since:
  "More than one monitor".)

## Recording: waveIn, WASAPI capture and endpoint volume

Phase 19.4.  The HD Audio driver now also programs an input stream: it
picks the codec's first input pin, preferring a microphone, then line in,
aux and CD, routes it through the mixer and selector widgets to an ADC,
and records 48 kHz 16-bit stereo into a ring the mixer thread reads every
tick.  The kernel mixer gained capture streams: each running one gets a
copy of what was recorded (the oldest frames dropped and counted when a
program falls behind), and the card records only while one runs.  Each
direction has a master volume, applied as the mixer mixes or copies.
`NtNovaAudioOpen` with its top bit set opens a capture stream, and
`NtNovaAudioCtl` gained four operations: read frames, describe the
recording device, and set and get the endpoint volume.

On top of that:
- winmm: `waveIn` (devices, capabilities, open with every callback kind,
  prepare and add buffers, start, stop, reset, position).  A thread
  converts the mixer's frames into each buffer in the program's format and
  hands it back full (`WIM_DATA`); stop hands back a partly filled one.
  The converter learned the reverse direction (any rate, 8 to 32-bit or
  float, mono as the average of both sides).
- mmdevapi: a second endpoint, the recording one ("Microphone (High
  Definition Audio)", or "Line in"), in the enumerator's lists and as the
  default `eCapture` device; its `IAudioClient` fills its buffer from a
  capture stream and `IAudioCaptureClient` hands it out ten milliseconds
  at a time, flagging a discontinuity when frames were dropped.  Both
  endpoints have `IAudioEndpointVolume` (scalar and decibel levels from
  -65.25 to 0 dB, per channel, mute, steps), kept in the kernel so every
  program sees the same volume.

To test it, `tools/novarun.py --rec FILE.wav` gives QEMU an `hda-micro`
card on a private PulseAudio server: the microphone hears FILE played over
and over into a null sink, and the speakers go to a second null sink
(saved with `--wav`).  Null sinks run on a clock, so the guest records in
real time; QEMU's ALSA backend with the file plugin delivered audio about
ten times too fast.  The core self-tests now record 3 s through `waveIn`
at 44.1 kHz mono and 3 s through WASAPI in the mix format, and
`tools/wavcheck.py --tone 523 2500` must find the 523 Hz tone in each
recording; the WASAPI test turns the recording volume down to a quarter
half way and must measure the level a quarter as loud, and `soundtest
volume` checks the volume controls on both endpoints.

## Windows Installer depth: custom actions, dialogs, shortcuts, services

Roadmap step 20.5.  Before writing anything we looked for a permissive
(MIT, BSD, zlib) Windows Installer to reuse; there is none (Wine's is
LGPL), so the engine stays NovaOS's own, with `stb_image` (public domain
or MIT) for the dialogs' pictures.

- **Custom actions that run code.**  DLL actions (types 1 and 17) run out
  of process, as on Windows: `msiexec /novaca` is a custom-action server
  that loads the DLL and calls its entry point, and a 32-bit DLL (WiX's
  `WixCA` usually is, even in x64 packages) runs in `SysWOW64`'s
  `msiexec`.  Its `MsiGetProperty`, `MsiDatabaseOpenView`,
  `MsiProcessMessage`... calls cross a pipe back to the installation.
  EXE actions (2, 18, 34, 50) start the program from the Binary table, an
  installed file, a folder or a property; type 19 shows its error; 35 and
  51 set folders and properties.  The flags are honoured: continue on
  error, asynchronous, run once, deferred with `CustomActionData`, commit
  actions after `InstallFinalize`; rollback actions are kept unused (there
  is no rollback yet).  Script actions (VBScript, JScript) and nested
  installs are logged and skipped.
- **The MSIHANDLE API** (`userland/msi/api.c`), exported at Windows'
  ordinals: records, views with an SQL engine (`sql.c`: `SELECT ... WHERE
  ... ORDER BY`, joins, `INSERT`/`UPDATE`/`DELETE`, `CREATE TABLE` and
  temporary rows through `MsiViewModify`), properties, formatted strings
  (`[Prop]`, `[#File]`, `[!File]`, `[$Comp]`, `[%ENV]`, `[1]`, `{...}`
  groups), feature and component states, target and source paths,
  `MsiDoAction`, `MsiSequence`, `MsiEvaluateCondition`, the summary
  information stream, `MsiGetMode` and `MsiSetMode`.
- **The packages' own dialogs.**  At full UI a package with an
  `InstallUISequence` shows its own wizard (`dialog.c`): the Dialog,
  Control, ControlEvent, ControlCondition, EventMapping, TextStyle,
  RadioButton, CheckBox, ListBox and ComboBox tables; text, bitmaps (BMP,
  PNG, JPEG), icons, lines and group boxes painted in place; push buttons,
  check boxes, edit and path fields, radio groups, the licence's RTF,
  progress bars, the feature tree (with check boxes), folder lists and
  combos, volume lists, list and combo boxes as real controls.  Events:
  property changes, `NewDialog`, `SpawnDialog`, `EndDialog`, `DoAction`,
  `SetTargetPath`, `Reset`, `AddLocal`, `Remove`, `SetInstallLevel`,
  `SelectionBrowse` and the folder-list events; the progress dialog
  follows `ActionText`, `ActionData` and `SetProgress`; Cancel asks first.
  The success, cancel and failure dialogs (-1, -2, -3) end it.  Packages
  without dialogs keep the progress window.
- **Shortcuts**: the Shortcut table (`IShellLink`, with arguments,
  working folder, description, show command and icons from the Icon
  table, saved under `C:\Windows\Installer\{ProductCode}`), into the
  Start menu (`C:\AppData\Roaming\Start Menu\Programs`) and the
  desktop; `msiexec /x` removes them.  shell32 gains the
  InternetShortcut class (`.url` files with `IUniformResourceLocator`,
  `IPersistFile` and its property set) that WiX's internet shortcuts use.
- **Services**: `ServiceInstall` and `ServiceControl` (install, start,
  stop, delete, on install and on uninstall), on a **service control
  manager** in `advapi32` (`service.c`).  Services live under
  `HKLM\SYSTEM\CurrentControlSet\Services` as on Windows;
  `StartService` runs the image, whose `StartServiceCtrlDispatcher` runs
  `ServiceMain` on a thread and takes controls on a pipe; `ControlService`,
  `QueryServiceStatus(Ex)`, `QueryServiceConfig(2)`, `ChangeServiceConfig(2)`,
  `EnumServicesStatus(Ex)`, `DeleteService` and the A forms work.
  `tools/msitest/make_service_package.sh` builds a package around a test
  service (`svc.c`).  Services marked automatic do not start at boot yet.
- **AppSearch** follows DrLocator (with Parent chains and Depth) and the
  Signature table (file names, minimum and maximum versions and sizes),
  besides RegLocator, whose directory and file types now check the
  signature too.
- **Smaller pieces**: `CostingComplete`, empty files missing from the
  cabinet are created, `MsiEnumRelatedProducts`, the features installed
  are remembered so maintenance runs and removals know them, and
  `ADDLOCAL`/`REMOVE` follow the dialogs' feature choices.  New:
  `activeds.dll` (ADSI; binding fails, as on a machine in no domain),
  which WiX's util custom actions import.  `advapi32` names all the usual
  well-known SIDs (`LOCAL SERVICE`, `Guests`, `Performance Log Users`...)
  and clears the last error on success where callers look at it.
- **FAT**: creating a file whose 8.3 alias was taken walked the folder
  once per `~N` tried, so a folder of a thousand similar names (CMake's
  documentation) froze the desktop while drive C: was saved.  One walk
  now marks the taken numbers.
- **Merge modules** are merged into a package's own tables when it is
  built (the `Module*` tables only record what came from where), so the
  engine installs them like any other component; none of the packages
  tested here carries one, so that is untested.
- **LZX on real packages**: 7-Zip's MSI (LZX:21) extracts byte for byte
  as `cabextract` does, and Node.js, CMake, Temurin and KeePassXC
  install.
- **`taskkill.exe`** (`/IM`, `/PID`, `/F`), which WiX's quiet-exec
  actions run to close a program before it is replaced.
- **C runtime**: MinGW programs lock a stream themselves, entering the
  critical section Microsoft's CRT keeps after each `FILE` (`_FILEX`) and
  setting a flag in `_flag` for the standard three.  msvcrt now allocates
  streams that way and keeps its flags where `_flag` sits, so a MinGW
  `fprintf` to a file no longer hangs (the test service's `ServiceMain`
  did).
- Tested in QEMU: 7-Zip and CMake through their own wizards (welcome,
  licence, options, folder, ready, progress, finish; CMake's
  `ValidatePath` and `DetectNsisOverwrite` DLL actions run from its
  dialogs); 7-Zip's two Start menu shortcuts; with `/qn`, CMake
  (`cmake --version`), Node.js (64-bit and 32-bit WiX custom actions,
  `node -e`, Start menu and internet shortcuts) and Temurin (the
  `WixRemoveFoldersEx` action, `java -version`) installed, ran and were
  removed with `/x`; KeePassXC (32-bit `WixQuietExec` running
  `taskkill`) installed and was removed, though it needs `MSVCP140.dll`
  (the Visual C++ runtime) to start; the test service installed,
  started, wrote its state, stopped and was deleted.  PowerShell 7 stops
  at its own launch condition: it wants Windows' `pwrshplugin.dll`
  (WinRM remoting), which NovaOS lacks.
- Not yet: rollback, script custom actions, nested installs, patches
  (`.msp`) and transforms (`.mst`), advertised features, services at boot.

## The app corpus under KVM, round three

The App corpus run on round two's branch under KVM (nightly.yml run #52)
passed 14 of 20 programs.  The App Store installs that had timed out in
earlier runs finished in seconds: they were never slow, the Terminal had
simply lost the keyboard before them, which round two fixed.  What was
left were two NovaOS bugs, each failing several programs.

- **Floating-point exceptions came unmasked after a C++ `catch`.**
  Audacity stopped at start with `0xc000008f` (inexact result) in
  wxWidgets and Krita with `0xc0000090` (invalid operation) in Qt Quick,
  both ordinary operations that Windows never traps.  `RtlCaptureContext`
  set `CONTEXT_FLOATING_POINT` but filled in only `MxCsr`, not the
  `FltSave` area, and `NtRaiseException` and `NtContinue` reloaded the
  x87 control word and MXCSR from `FltSave`: whatever the stack held
  there, often zeros, which unmasks every exception.  `RtlCaptureContext`
  now saves the whole floating-point state there (`fxsave`), and the
  kernel takes MXCSR from `CONTEXT.MxCsr`, as Windows does.  `cppeh`
  checks both.
- **Firefox froze the whole desktop.**  As Firefox's processes started,
  the desktop thread stopped (the watchdog's backtrace: drawing the dock,
  asking the network for its status, waiting for the network lock) and
  never came back, so Firefox's, Notepad++'s and PuTTY's screenshots
  were the same frozen screen.  The network lock is a spinning lock whose
  waiters yield, and every program asking for random bytes
  (`NtNovaGetRandom`, which every Firefox process does as it starts)
  took it, because the entropy pool lived under it.  Waiters of higher
  priority handing the CPU to each other can starve a holder of lower
  priority behind them indefinitely.  The entropy pool now has its own
  short lock, so random bytes never wait for the network, and a waiter
  for the network lock sleeps briefly after a few yields, which lets
  the holder run.  `smpstress` gained a test of random bytes from
  high-, normal- and low-priority threads at once.

Still failing after this round: VLC does not exit after Alt+F4 (its own
thread; the corpus stops it and carries on), and Audacity's recording
shows dropouts under KVM.

## The app corpus under KVM, round two

The first App corpus run under KVM with the shared DLL pages (Actions run
#44) passed 11 of 19 programs.  Its screenshots showed that most of the
failures were one failure seen many times: every screenshot from VLC on
was the same picture, VLC's "Errors" box saying `C:\Apps\in.mp4` could
not be opened, with the Terminal behind it never having received another
command.

- **ffmpeg's MP4 was never made.**  The Terminal took only the first 158
  characters of a typed line, so the command ended at `C:\Apps\i` and
  ffmpeg stopped with "Unable to choose an output format", exit code -22
  (EINVAL).  The VLC change (#126) shortened the command the same way,
  and the Terminal's own line length is raised separately.
- **One stuck program failed every program after it.**  With no MP4,
  VLC (`--loop`) kept its error box up; Alt+F4 closed the box, the next
  loop opened it again, and the corpus typed `taskkill` and `echo ready`
  into VLC instead of the Terminal.  Audacity, Inkscape, Firefox,
  Notepad++ and PuTTY then never started (Firefox's "no outcome in
  1500 s" is the App Store command that never ran).
  `tools/appcorpus.py` now checks that the Terminal answers before it
  stops what a program left running; when it does not, it opens a second
  Terminal from Start (the Windows key reaches Start whichever window has
  the keyboard), stops the program from there and closes that Terminal
  again so the next screenshots show the usual desktop.  The program that
  kept the keyboard still fails.  A program that keeps opening new
  windows (VLC's error box on every loop) can still take the second
  Terminal's keys.
- **fd panicked: "keyed events not available".**  Rust's standard
  library parks a thread with `WaitOnAddress`, which it finds with
  `GetModuleHandle("api-ms-win-core-synch-l1-2-0")`.  NovaOS maps that API
  set to `kernelbase.dll`, but kernelbase was loaded only into programs
  importing from it, so fd got no module and fell back to keyed events
  (`NtCreateKeyedEvent`), which NovaOS lacks.  It only parks when its
  threads contend, which they do on four real CPUs.  As on Windows,
  `kernelbase.dll` is now in every process that has `kernel32.dll`
  (`kernel/um/um.c`).  `crtthreads` checks the API set lookup, 64- and
  32-bit.
- **Notepad++ crashed on start (main since #122).**  Found while checking
  the recovery: Notepad++ turns a structured exception into a C++
  exception with `_set_se_translator`, which throws from inside its frame
  handler while the first exception is being dispatched.  Since #122,
  `RtlUnwindEx` starts at the new exception's own frame, and the walk up
  from there went through the dispatcher's stack and never reached the
  catching frame, so it resumed with a wrong stack and the next `free`
  faulted (`ntdll.dll+0x1e9f`).  Past the frames dispatching an outer
  exception, the walk now carries on where that exception happened (as
  `KiUserExceptionDispatcher`'s unwind data leads on Windows), and from
  inside a catch block it goes straight to the catch's own frame (as
  Windows' consolidation frame does).  Notepad++ opens its file again and
  matches its reference (0.0%); the exception self-tests (`cppeh`,
  `unwindtest`, `stltest`, `rttest`, `qttest`) pass.

Under TCG: fd, ffmpeg (all four steps), Notepad++ and PuTTY pass.  The
first KVM run with these changes passed 16 of 20 programs (it was 11 of
19): fd, ffmpeg, Inkscape, Krita and Notepad++ now pass.  Still failing
there: VLC hangs as it exits (its windows close, threads in
`libmmdevice_plugin.dll` and the Qt plugin keep waiting; a separate
change), Audacity's recording shows two dropouts (7.8%), and Firefox
(5.5%) and PuTTY (6.7%) had the second Terminal in their screenshots,
which it now closes.

## App Store scroll bar

The App Store's list of programs had only a thin grey line at its right
edge to show where it was scrolled: it could not be clicked or dragged, so
the list moved only with the wheel and the keys.  It now has the same
scroll bar as File Explorer (`UiScroll` in `kernel/apps/apps.c`, from
[File Explorer scroll bars](#file-explorer-scroll-bars)), so the two
built-in apps scroll the same way.

- **The bar.**  The list of programs (every category, Installed included;
  the list is the Store's only view that scrolls) has a vertical scroll bar
  whenever its rows don't fit, 17 pixels wide with user32's arrows, trough
  and a thumb sized to the page, in place of the thin line.  The rows and
  their buttons make room for it, and it comes and goes as the window is
  resized and the category changes.
- **Using it.**  The arrows scroll a row (84 pixels), a click in the trough
  scrolls a page toward the pointer, the thumb drags, and a held arrow or
  trough repeats as in File Explorer.  The wheel still scrolls a row a
  notch, Up and Down a row, Page Up and Page Down a page, Home and End to
  the top and the bottom, and choosing a category goes back to the top.
- **`store open`.**  The Terminal's `store` command opens the App Store
  (or brings it forward) with `store open`, beside `store install` and
  `store close`.
- **The test.**  The graphics suite's `store scroll bar` runs `store open`
  and checks that All apps has a vertical bar, then that the wheel, Home,
  Page Down, End, a click on the down arrow, one in the trough and a drag
  of the thumb each move the list by what they should.  The Store logs a
  `[STORE] view:` line each time its view changes (the category, how far
  the list is scrolled, the bar and where the list is on the screen),
  which the test reads; Esc then closes the Store.

## The audio DSP boots Sound Open Firmware (the T14's microphones, part 1)

The ThinkPad T14 Gen 4's built-in microphones are digital microphones on
Intel's audio DSP, not on the Realtek codec, so Phase 21.4 left them
silent.  This is the first of three steps to record from them
([hardware.md](../hardware.md#the-digital-microphones-behind-the-audio-dsp)).

- **NHLT.**  The ACPI NHLT table describes the microphones: the endpoint
  of link type PDM that records, its microphone array type (two or four
  microphones), its formats and, per format, the configuration blob the
  firmware needs to clock them.  `kernel/drivers/sof.c` reads it and logs
  what it found (`[DSP] NHLT: ...`).
- **The firmware.**  `tools/fetch_sof_firmware.py` downloads Intel's
  signed Sound Open Firmware from the SOF project's sof-bin release
  (v2026.09.1, BSD-3-Clause with Intel's firmware licence, SHA-256
  checked) into `third_party/sof-bin`, which git ignores; the build puts
  it at `C:\Windows\Firmware\Intel\sof-ipc4\rpl\sof-rpl.ri`.  The driver
  checks the file: its extended manifest, the `$CPD` partition the ROM
  takes, and the `$AM1` module list (the copier, which the next step
  uses, is module 4 in this release).
- **The boot.**  On a Tiger Lake to Raptor Lake controller with its DSP
  on, a background thread powers the DSP's first core while it is held
  in reset, asks the ROM to load the firmware from a host DMA stream,
  lets the core run, streams the image to the ROM over the last output
  stream, decoupled from the link through the processing pipe (with the
  software position limit at the image's end, link power saving off
  while loading, and an input stream held for the DSP's full current),
  waits for the firmware to start, answers its FW_READY message and asks
  it for its FW_CONFIG over IPC4.  Any failure switches the core off and
  restores the processing pipe, so playback and the headset microphone
  are never affected; each step is logged for the boot log on the stick.
- **Tests.**  QEMU has no audio DSP, so `hwcheck` (core self-test
  `hwcheck dsp`) runs the NHLT reader on a modelled two-microphone table,
  the manifest reader on a modelled firmware file and the whole boot on a
  modelled DSP that reads the image back through the code loader's BDL
  and compares it, answers the IPC4 request, and must be left off after a
  bad image.  Its last line says what the real DSP did; on the T14 it
  should name the running firmware and two microphones (unverified: not
  yet run on the machine).
- Not yet: the IPC4 capture pipeline (DMIC gateway copier to a host input
  stream) and the recording device for it.

## The audio DSP records the T14's microphones (part 2)

The second of three steps to the ThinkPad T14 Gen 4's built-in
microphones ([part 1](#the-audio-dsp-boots-sound-open-firmware-the-t14s-microphones-part-1),
[hardware.md](../hardware.md#the-digital-microphones-behind-the-audio-dsp)):
once Sound Open Firmware runs on the audio DSP, NovaOS has it record
the digital microphones into memory it owns.

- **The pipeline.**  `kernel/drivers/sof.c` sends the firmware the IPC4
  messages for one pipeline: CREATE_PIPELINE; INIT_INSTANCE of a copier
  on the DMIC gateway, whose configuration is NHLT's blob for the first
  format; INIT_INSTANCE of a second copier on the host input gateway of
  the last HD Audio input stream, which turns the samples into 16-bit
  ones; BIND of the first to the second; then SET_PIPELINE_STATE to
  PAUSED and, once the stream's DMA runs, RUNNING.  The payload layouts
  follow SOF's BSD-licensed headers (`ipc4_copier_module_cfg`, the base
  module configuration, audio format and gateway node ID); the copier's
  module ID is read from the firmware's module list.
- **The ring.**  The input stream is decoupled from the link (the
  processing pipe), so the DSP's host DMA fills its 128 KiB ring and
  its position register says how far; `SofCaptureRing()` and
  `SofCapturePosition()` hand both to the mixer for the next step.  If
  the firmware refuses a message, the pipeline is deleted and the stream
  given back to the link, and the firmware keeps running.
- **Tests.**  The modelled DSP behind `hwcheck` (core self-test
  `hwcheck dsp`) now checks every pipeline message against the layouts
  (node IDs, formats, buffer sizes, the NHLT blob, the order of states),
  fills the ring with a quarter-scale square wave while the pipeline
  runs, and refuses the host copier once to check the clean-up.  On the
  T14, `hwcheck`'s last line should say the microphones are recording
  and give their level (unverified: not yet run on the machine).
- Not yet: the recording device over the ring ("Microphone Array" in the
  Sound settings, `waveIn`, Audacity) and the DSP booted again after
  sleep.

## The T14's microphones are a recording device (part 3)

The last of three steps to the ThinkPad T14 Gen 4's built-in
microphones ([part 2](#the-audio-dsp-records-the-t14s-microphones-part-2),
[hardware.md](../hardware.md#the-digital-microphones-behind-the-audio-dsp)):
what the audio DSP records reaches programs.

- **Microphone Array (DSP).**  Once the capture pipeline runs,
  `kernel/drivers/sof.c` attaches its ring to the mixer as a recording
  device, the default input from then on as on Windows.  Settings >
  Sound, `waveIn`, WASAPI and DirectSound list it, so Audacity can
  record from it.  Inputs of the mixer may now have any channel count:
  a microphone array's four become stereo (the even channels left, the
  odd ones right), and its 16 kHz or 48 kHz is converted as before.
- **Paused while nothing records.**  The DSP's thread now stays: when
  the last recorder stops it pauses the pipeline (SET_PIPELINE_STATE
  PAUSED, the stream's DMA off) and runs it again when one starts.
- **After sleep.**  S3 takes the DSP's power, its firmware and its
  pipeline; `SofResume()`, after `HdaResume()` on wake, has the thread
  boot the DSP again from the same firmware file and build the pipeline
  again, and the mixer reads the new ring from where the DSP starts
  writing.
- **Tests.**  `hwcheck` checks the pause, the run again and the boot
  again after a modelled sleep on its modelled DSP.  `hwcheck mic` boots
  a live model whose DMA writes a 1 kHz tone and attaches it as
  "Microphone Array (DSP model)": the new core self-test `hwcheck mic`
  has `waveIn`, WASAPI and DirectSound list it and `soundtest record`
  and `capture` hear the tone, before and after `hwcheck mic sleep`
  takes its power.  On the T14 the check is by hand (hardware.md;
  unverified: not yet run on the machine).

## Starting from a USB stick, with the boot log on the stick (Phase 21.2)

Phase 21 takes NovaOS from QEMU to a real PC.  Its second step is the
way in: `nova.iso` written to a USB stick, started on UEFI firmware with
Secure Boot off and the firmware's GOP framebuffer as the display.

- **The ISO is a USB stick image now.**  `scripts/create-iso.sh` passed
  `-isohybrid-gpt-basdat`, which on its own (it needs `-isohybrid-mbr`)
  writes no partition table at all: the ISO started as a disc but was
  just bytes to firmware when written to a stick.  The EFI System
  Partition image is now also a partition of a GPT (partition 2)
  behind a protective MBR (`-efi-boot-part --efi-boot-image`), the same sectors the El
  Torito boot entry points at, so the ISO starts from a stick and still
  from a disc.  (An appended partition, the other common layout, broke
  the disc: firmware sizes a disc's El Torito image from the ISO9660
  volume, which an appended partition lies outside.)
- **Live from the stick.**  The bootloader told the installation disc
  from an installed disk by the CD-ROM node in its boot device's path.
  The ISO's ESP now carries `\EFI\NOVA\bootlog.txt`, which the installer
  does not copy, so a boot volume holding it is the installation media
  wherever it is; a USB node in the path adds `BOOT_FLAG_LIVE_USB`.
  NovaOS then runs live as from the disc, opens Install NovaOS ("You
  are running NovaOS from the installation USB stick"), and the disk
  list marks the stick as the disk NovaOS started from.  Installing from
  the stick onto an NVMe disk and starting from that disk was checked in
  QEMU.
- **GPTs with more than 128 entries.**  xorriso writes 248; drive C:'s
  search and the drive letters skipped such disks, so the stick's ESP
  never mounted.  Both now read up to 1,024 entries.
- **The boot log on the stick** (`kernel/fs/bootlog.c`).  Most laptops
  have no serial port, so after a hang there was nothing to read.  The
  kernel now keeps its output from the start, whole up to about 1 MiB
  (`klog_boot_text`, beside the Terminal's 16 KB `dmesg` ring).  When
  the stick NovaOS started from appears, the log so far is written into
  `bootlog.txt` (1 MiB set aside on the ISO, its clusters one after
  another), then what follows at most once a second, before a restart or
  shutdown, and once more after a kernel fault (a page fault or another
  exception in kernel mode) if the stick is free.  Writing in place,
  sector by sector, changes no FAT metadata, so the stick's read-only
  drive letter (`NOVA_EFI`) stays right.  Read it on another computer:
  the stick's EFI partition, `EFI\NOVA\bootlog.txt`.
- **Tests** (devices suite): `usbboot` starts with nothing but the ISO
  written to a 2 GiB USB stick on xHCI and QEMU's `ramfb` (a display
  only the firmware's GOP drives, like a laptop's integrated graphics);
  it checks the live start, the GOP display, the log file being found
  and the stick's drive letter, then reads `bootlog.txt` off the stick
  image with mtools and finds the end of the boot and the Terminal's
  program in it, and, after `crash kernel`, the fault and its symbolized
  backtrace.  `cdboot` starts from the same ISO as a disc.

On the reference machine (the Lenovo ThinkPad T14 Gen 4, Intel), the
manual check is: write the ISO to a stick, turn Secure Boot off in the
firmware setup (F1), start from the stick with F12, reach the desktop
and Install NovaOS, then shut down and read `EFI\NOVA\bootlog.txt` on
another computer.  It has not been done on the machine yet.

## Test VMs under KVM in CI

`tools/novarun.py` (and so `tools/selftest.py`) now passes `-accel kvm` when
`/dev/kvm` is readable and writable and `-accel tcg` otherwise;
`NOVARUN_ACCEL=tcg|kvm` forces one.  The CPU model and the SMP counts are
unchanged.  GitHub's Linux runners have `/dev/kvm` once a udev rule opens it
to the runner user (`KERNEL=="kvm", GROUP="kvm", MODE="0666"`).

**CI stays on TCG.**  A trial with KVM on every VM job (PR #75) failed:

- Core boot-test: the boot died with a #GP in `uacpi_gas_read_mapped` (from
  `uacpi_setup_gpe_for_wake` in the ACPI thread's `add_wake`, with a
  non-canonical pointer in RDI) while the other kernel threads were starting.
- Network suite: `winsock IPv4`, `winhttp HTTP/2`, `looptest` and `winsock
  IPv6` did not finish in 180 s (virtio-net, ping and `curl -6` passed).
- Graphics job: passed once (test step 111 s against 193 s on TCG), then the
  next run died at boot with no kernel output after the NIC probe.

All pass under TCG, so the faster guest exposes a kernel race or timing
assumption (the ACPI thread runs without the big kernel lock).  The jobs set
`NOVARUN_ACCEL=tcg`; the tests are not loosened.  TCG reference times: core
self-tests 228 s, network 63 s, graphics step 193 s.  Turn KVM on by deleting
that line and adding the udev step once the boot is reliable.

**CI now runs the test VMs under KVM.**  The kernel fixes since PR #75
(SYSRET's stack selector on AMD CPUs, the kernel-lock and idle-CPU races) and
the scheduler fixes that kept the 1 ms timer queue timer on time removed the
failures above.  `tools/ci/enable-kvm.sh` runs first in the boot-test,
graphics and nightly jobs: it opens `/dev/kvm` to the runner user with a udev
rule and exports `NOVARUN_ACCEL=kvm`; when the runner has no usable
`/dev/kvm` it exports `tcg` and prints a warning, so a runner without KVM
still tests, only slower.  No test was loosened or skipped.

Measured on GitHub's runners with KVM: the graphics job takes 4 to 6 minutes
(9 to 10 under TCG), the build-and-boot job about 11 to 12 minutes including
the build, and a boot takes 11 s.  The nightly app corpus runs under KVM too.

## Run CI on merge queue groups

GitHub's merge queue builds each queued pull request on a temporary
merge-group branch and only starts workflows that listen for the
`merge_group` event.  `.github/workflows/ci.yml` now does, so the three
required checks (Checks, Build and boot-test, Graphics tests) run for
queued pull requests.  The job names are unchanged.  The generated-docs
check stays pull-request only, since the pull request already passed it;
the other jobs have no conditions that depend on a pull request, and the
concurrency group falls back to the merge-group ref.

## The C++17 special math functions (`msvcp140_2.dll`)

The last satellite of the C++ standard library the
[msvcp140 work](#the-c-standard-library-c99-complex-math-and-dll-directories)
left out.  Programs built by Visual Studio that call `<cmath>`'s special
functions (`std::cyl_bessel_j`, `std::expint`, `std::riemann_zeta`...)
import them from `msvcp140_2.dll` as `__std_smf_*`, and did not load on
NovaOS.  Nothing new was written: Microsoft builds the DLL from one STL
source, `special_math.cpp`, a thin wrapper over Boost.Math (Boost
Software License), and NovaOS now does the same.

- **`third_party/msstl/src/special_math.cpp`** joins the vendored STL
  (same `vs-2022-17.13` tag), and **`third_party/boost-math`** holds the
  110 Boost.Math headers it reaches, from the commit that STL tag pins,
  unchanged, with Boost's licence.  `userland/msvcp140_2/build.py`
  compiles it like the other satellites, with
  `BOOST_MATH_STANDALONE=1` as Microsoft's build does (no other Boost
  libraries needed).
- **The DLL exports the 44 `__std_smf_*` functions** (`double` and `f`
  forms; the `l` forms are inline in `<cmath>` and call the `double`
  ones) at ordinals 1 to 44 in name order, as the linker numbers
  Microsoft's, for 64- and 32-bit programs.  Outside a function's domain
  it returns NaN and sets the Universal C Runtime's `errno` to `EDOM`.
- **Self-test `smftest`** (64- and 32-bit) checks 28 values against
  closed forms and published constants (ζ(2) = π²/6, B(2, 3) = 1/12,
  J₀(1), K(0.5)...) and the `EDOM` case.  None of the nightly corpus
  programs or App Store downloads imports `msvcp140_2.dll` today, so the
  self-test is the proof.
- **Build fix**: changing a C++ program (`userland/**/*.cpp`) now
  rebuilds the userland; before, only C sources, headers and manifests
  were tracked.

## The C++ standard library, C99 complex math and DLL directories

Three gaps the Python and Qt programs ran into, all closed.  Before
writing anything we looked for permissively licensed code to reuse, and
two of the three are existing open-source code shipped as OS components.

- **`msvcp140.dll`, the C++ standard library, is Microsoft's own STL.**
  Microsoft publishes the STL that Visual Studio ships
  ([microsoft/STL](https://github.com/microsoft/STL), Apache-2.0 WITH
  LLVM-exception, the licence family of LLVM's libc++).  It is the only
  source whose classes have the layouts, names and exports MSVC-built
  programs import, so NovaOS builds it rather than an imitation (Wine's
  `msvcp` is LGPL).  `third_party/msstl` holds the `vs-2022-17.13`
  release's headers and sources; `userland/msvcp140/build.py` compiles
  them with clang in MSVC mode against MinGW-w64's headers, plus a few
  shim headers for the VC runtime's private ones (`userland/msvcp140/inc`).
  The DLL exports all 1515 names of Microsoft's current `msvcp140.dll`.
  The satellites come from the same sources: `msvcp140_1` (`std::pmr`),
  `msvcp140_atomic_wait` (atomic waits, parallel algorithms, `<syncstream>`,
  the time-zone database over `icu.dll`) and `msvcp140_codecvt_ids`;
  `msvcp140_2` (the special math functions) is not built yet, as it needs
  Boost.Math.  Each module also gets its own `operator new`/`delete` and
  start-up code (`start.cpp`, `new.cpp`), and the import library's static
  part (`msvcprt_static.lib`: `std::filesystem`, `to_chars`, `std::format`,
  `shared_mutex`...).  One clang difference needed care: under
  `#pragma init_seg(compiler)`, clang files the initializers of exported
  objects (`std::cerr`) apart from the static ones that use them, so the
  build renames those sections and each source's initializers keep their
  order.
- **C99 complex math in `ucrtbase`/`msvcrt`**: musl's `src/complex` (MIT,
  next to the musl libm already there): `cabs`, `carg`, `cexp`, `clog`,
  `csqrt`, `cpow`, the trigonometric and hyperbolic functions, their `f`
  and `l` forms, `creal`/`cimag`, plus the UCRT's own `_Cbuild`,
  `_Cmulcc`, `_Cmulcr`, `norm` and their float and long double forms.
  clang's `_Complex` passes and returns exactly as MSVC's `_Dcomplex`
  and `_Fcomplex` structures do, so the functions take what MSVC-built
  callers hand them.  `_cprintf`, `_cputs` and the UCRT's
  `__conio_common_vcprintf` family write to the console.
- **`AddDllDirectory`, `RemoveDllDirectory` and `SetDllDirectory` are
  real.**  The loader searches the `SetDllDirectory` folder and then every
  added folder after the importing module's own folder, for
  `LoadLibrary` and for imports alike, so Python's
  `os.add_dll_directory` (NumPy's `numpy.libs`) works.  The folders live
  in the process (`kernel/um/um.c`, through `NtNovaLoadDll`).
- **For KeePassXC**: a `d3d11.dll` of NovaOS's own (Qt's GUI library
  imports it): without DXVK it answers `DXGI_ERROR_UNSUPPORTED`, as
  Windows does with no Direct3D 11 device, and with the App Store's DXVK,
  which now installs its `d3d11.dll` as `d3d11_dxvk.dll` (like its
  `dxgi`), it passes the calls on.  New: `winscard.dll` (no smart card
  service: `SCARD_E_NO_SERVICE`), `HidD_GetFeature`/`SetFeature`,
  `GetCharABCWidthsI`, `UpdateLayeredWindowIndirect`,
  `Shell_NotifyIconGetRect`, `WTSQuerySessionInformationW`,
  `CheckRemoteDebuggerPresent`, `SetSearchPathMode`, `WSAHtonl` and
  friends, and `CommandLineToArgvW` through the `shcore` API set.  32-bit
  `vcruntime140` and `ucrtbase` now export `__std_terminate`, `_setjmp`,
  `_except_handler3`, `_wcstoui64` and the like under their real names
  (the linker had dropped a leading underscore).
- **Tests**: `stltest` (28 checks: strings, containers, streams, locales,
  exceptions, threads, `std::async`, atomic waits, `pmr`,
  `std::filesystem`, `std::format`, `std::regex`) and `rttest` (30:
  complex math, conio, DLL directories) pass 64- and 32-bit and run in the
  CI core suite.  Python 3.14 with the NumPy 2.5.3 wheel, on NovaOS's own
  runtime DLLs, imports NumPy and prints the same complex `exp`, `sqrt`,
  `log`, `sin`, `tanh`, `power`, `linalg.inv`, `fft` and `eigvals` results
  as NumPy on Linux.  KeePassXC 2.7.11 starts and shows its main window
  and first-run dialog.
- Not yet: Qt's widgets draw their shapes and icons in KeePassXC but not
  their text; `msvcp140_2.dll`.

## Crash reports in C:\NovaOS\Crashes (Phase 22.3)

A crash used to leave nothing behind but a line in the serial log, which
a PC without a serial port never shows.  Now every crash leaves a text
file a user can attach to an issue.

- **A program's crash** (`kernel/um/um_crash.c`): when an exception goes
  unhandled (`UmFaultAt`), the crashing thread puts together
  `C:\NovaOS\Crashes\NAME-YYYYMMDD-HHMMSS-PID.txt`: the program, PID, path,
  bitness and thread count, the time and NovaOS version, the exception and
  its code, where it happened (`module+offset`), the address an access
  violation touched, up to 32 return addresses into modules on the stack,
  every loaded module with its base, size and path, and the last 4 KiB of
  the kernel's log.  The desktop thread writes it a moment later
  (`UmCrashPoll`, under the file-system lock), and drive C: saves it to
  the disk as usual.  At most 100 reports are kept.
- **The Terminal** names the report under the crash line (`Crash report:
  C:\NovaOS\Crashes\crash-20261003-194012-14.txt`); `crashes` lists the
  reports and `crashes last` prints the newest.
- **A kernel crash** cannot write drive C: (it lives in memory and the
  save needs the drivers the crash may have broken).  At boot, NovaOS sets
  aside `\NOVA\PANIC.TXT` (64 KiB, contiguous) on the FAT volume that keeps
  C:; a kernel page fault or unhandled exception, after printing its
  backtrace, writes a report (the time, the version and the last 31 KiB
  of the kernel's log, ending with the backtrace) into those sectors with
  the disk driver alone: no FAT changes, no allocation (`PersistPanicWrite`).
  The next start moves it to `C:\NovaOS\Crashes\kernel-YYYYMMDD-HHMMSS.txt`
  and blanks the slot.  NTFS C: has no slot yet.
- **Tests**: `crashtest` (core suite, 64- and 32-bit) starts `crash.exe` and
  checks its report; the Terminal's `Crash report:` line and `crashes
  last` are checked after `crash`; `crash kernel` (900) now resets the
  machine afterwards (`Test(restart=True)` in `tools/selftest.py`), and
  905 checks that the next start reports the kernel crash with
  `KeCrashTestFault` in its backtrace.

## Fiber-local storage callbacks, per-thread locales and thread-safe getenv

The C runtime's per-thread block (`errno` and friends) left a few pieces
shared by the whole process.  They now behave as on Windows.

- **Fiber-local storage.**  `FlsAlloc` used to hand out TLS slots and
  store its callback without ever calling it, so a runtime that keeps its
  per-thread data in FLS (Microsoft's `vcruntime140.dll` and
  `ucrtbase.dll`, which programs ship next to themselves) leaked that data
  for every thread that ended.  FLS now lives in `ntdll`
  (`userland/ntdll/ntdll_fls.c`, with `RtlFlsAlloc`, `RtlFlsFree`,
  `RtlFlsGetValue`, `RtlFlsSetValue` and `RtlProcessFlsData`): 128 slots
  of its own, so FLS no longer uses up the 64 TLS slots, and a block of
  values per fiber reached from the TEB's `FlsData` field.  A slot's
  callback runs on each value still set when a thread ends (before the
  DLLs' `DLL_THREAD_DETACH`, as on Windows), on every live thread's value
  when the slot is freed, and on a fiber's values when `DeleteFiber`
  deletes it.  `SwitchToFiber` swaps `FlsData` with the fiber, so each
  fiber has its own values.
- **Per-thread locale.**  `_configthreadlocale(_ENABLE_PER_THREAD_LOCALE)`
  gives the calling thread its own copy of the locale names in
  `msvcrt.dll` and `ucrtbase.dll`; its `setlocale`/`_wsetlocale` calls
  change only that copy and other threads' calls no longer reach it,
  until `_DISABLE_PER_THREAD_LOCALE` puts it back on the process's
  locale.  It returns the previous setting and rejects unknown values
  with -1.  The text rules themselves are still those of the "C" locale.
- **getenv.**  `getenv` kept 16 rotating 512-byte buffers, rewritten in
  place, and `_wgetenv` one buffer freed on the next call, so a thread
  could read a value another thread was overwriting, or a freed one.
  Both now return the runtime's own copy per variable, kept until the
  value changes (a replaced copy is never freed, as another thread may
  hold it), with no length limit.  `GetEnvironmentVariableW` no longer
  returns an unfilled buffer when another thread lengthens the value
  between its two internal reads.  `tmpfile`'s name counter is atomic.
- **Still process-wide.**  The internal `mbstate_t` of `mbrtowc` and
  `wcrtomb` (static on Windows too).
- **Test.**  Core self-test `crtthreads` (64- and 32-bit).

## The desktop's redraws no longer hold the file-system lock

The [save without locks](#saving-drive-c-without-holding-the-locks) work
left one long hold of the file-system lock: the desktop thread took it for
the whole of every redraw, because a few things it draws come from files
(program and file icons, the files on the desktop, the Start menu, most
built-in apps' windows).  A redraw takes 50-180 ms in QEMU without KVM, so
any file call (`GetFileAttributes`, `CreateFile`, a directory listing)
could wait that long, and on a busy CI runner `savetest` saw 275 ms
against its 250 ms limit.

- **Lock only what reads files.**  `kernel/wm/desktop.c` no longer takes
  the file-system lock around `WmComposite`.  What reads files takes it
  itself, around just that: program icons (`AppDrawProgramIcon`), the
  desktop's files (looking each one up and drawing its icon, not the
  labels' blurred shadows), the Start menu while it is open, and built-in
  apps' painters.  A built-in window whose painter reads no files sets the
  new `WND.paint_fs_free` (the Terminal: its prompt's path takes the lock
  for the moment it is read).  Program windows already drew without it.
- **Cheaper window edges.**  `GdiRoundBorderAlpha`, the hairline edge of
  every window and of the dock, worked out a distance for every pixel
  inside the box only to skip it; it now skips the inside of each row
  straight away (the same pixels are drawn).  A redraw of the desktop with
  the Terminal open went from 76-107 ms to 53-68 ms in QEMU (TCG), which
  is also how long calls behind the desktop lock can wait for one.
- **Measured** with `savetest` in QEMU (TCG, 2 processors), the longest
  wait for the file-system lock during the 32 MiB save fell from 177 ms to
  1.3-12 ms; no hold of the file-system lock by the desktop went over
  15 ms.  `savetest` now fails when the file-system call waits 100 ms or
  more (the kernel and desktop calls keep the 250 ms limit).

## Generated docs are rebuilt by pull request

Once main only took pull requests, the Docs workflow could neither push the
rebuilt README, ROADMAP, HISTORY and building.md regions nor open a pull
request, and the regions fell about 600 lines behind.  The docs check
(`tools/docgen.py --check-pr`) now accepts a changed generated region when
it is byte-identical to a fresh `tools/docgen.py` run on the pull request's
tree, and still rejects a hand edit.  The Docs workflow only reports stale
files.  The daily "Docs sync" pull request regenerates the regions and
lands through auto-merge.

## Controls rescaled on a DPI change, coordinates across awareness contexts, comctl32 at 192 DPI

- **Controls follow a DPI change.**  User32's controls were measured
  when they were made, so a combo box, list box or edit control made at
  96 DPI kept its line and item heights when its window moved to a 192
  DPI monitor (or the monitor's DPI changed), and drew its text twice
  the size in a field half as tall.  Now, when a window's DPI changes,
  its built-in controls that use the default font measure it again at
  the new DPI (list box items, a combo box's field and height, an edit
  control's lines and margins).  In a per-monitor v2 dialog, as
  Windows' dialog manager does, the controls' places and sizes scale
  with the dialog and the template's font is made again at the new DPI
  (8 points: 11 pixels at 96, 21 at 192) for the dialog and the controls
  that use it, so dialog units follow.  The common controls get
  `WM_DPICHANGED_AFTERPARENT` and measure their default font and sizes
  again (tree view, list view, toolbar, status bar, rebar, ComboBoxEx).
- **Coordinates across awareness contexts.**  `GetWindowRect` and the
  other coordinate calls on a window of another DPI awareness returned
  that window's own coordinates.  Now, as on Windows, they are converted
  to the calling thread's: a per-monitor-aware thread sees an unaware
  window in its own 192 DPI pixels and an unaware one sees an aware
  window in logical pixels.  This covers `GetWindowRect`,
  `GetClientRect`, `ClientToScreen`, `ScreenToClient`,
  `MapWindowPoints`, `GetWindowInfo`, `GetWindowPlacement`,
  `SetWindowPos`/`MoveWindow` (whose coordinates are the caller's) and
  `WindowFromPoint`, and another program's windows, whose rectangles
  the desktop gives in logical pixels.
- **comctl32 at 192 DPI.**  The common controls used the UI font at
  their window's DPI but kept fixed 96 DPI sizes around it.  Now their
  margins, gaps and minimum sizes scale with the window's DPI: the
  header's minimum height, text margins and divider grab zone, the tree
  view's indent (19 pixels at 96 DPI, 38 at 192), item height, check
  boxes and buttons, the list view's rows, icon cells and check boxes,
  the toolbar's default padding, separators and drop-down arrows, the
  status bar's height, borders, icons and size grip, the tab control's
  padding and borders, the trackbar's channel and thumb, the up-down
  control's width and the rebar's gripper and band borders.  Sizes a
  program sets (`TVM_SETINDENT`, `TB_SETPADDING`, `TCM_SETPADDING`,
  `TVM_SETITEMHEIGHT`) are kept as given.
- `dpitest` checks it: comctl32's sizes in a 192 DPI window against a
  96 DPI one, a window and a dialog made at 96 DPI measured again at 192
  and back at 96, and the coordinate calls both ways between its
  per-monitor-aware thread and an unaware window.
- Not yet: a combo box's drop-down list is a window of its own and keeps
  its item height until it is made again; only 96 and 192 DPI exist
  (no 120 or 144).

## user32's own parts at 192 DPI, and per-thread DPI awareness

- **Controls, menus and dialogs follow the window's DPI.**  Since
  per-monitor DPI, a DPI-aware program on a 192 DPI monitor got twice the
  pixels, but user32 still drew its own parts at their 96 DPI sizes in
  them, so they came out half size.  Now, in a window at 192 DPI, the
  built-in controls (buttons, check boxes and radio buttons, group boxes,
  statics, edits, list boxes, combo boxes and their drop-down lists,
  scroll bars), the non-client area (borders, client edges, the menu bar,
  window scroll bars and the caption hit area) and popup menus are drawn
  twice the size, with the UI font (Segoe UI 9 point) at 192 DPI.  List
  box items, combo box fields and menu bars grow with the font.  Dialog
  templates are laid out with their font made at the dialog's DPI, so
  dialog units (`MapDialogRect`, the controls' places and the dialog's
  size) scale with it, and `MessageBox` is sized at the DPI it opens at.
  The common controls (list view, tree view, header, toolbar, rebar,
  status bar, tabs, links, ComboBoxEx) use the UI font at their window's
  DPI.  Windows at 96 DPI, including every window of a DPI-unaware
  program, look as before.
- **Metrics and stock fonts at the system DPI.**  `GetSystemMetrics`
  returns its sizes (scroll bars, caption, menu, borders, icons, ...) at
  the system DPI the calling thread sees, as on Windows: twice the 96 DPI
  values in a system-aware program started on a 192 DPI monitor, and the
  96 DPI ones for unaware threads.  So do `SystemParametersInfo`'s
  `SPI_GETNONCLIENTMETRICS` (whose ANSI form now fills in the fonts
  rather than returning zeros), `SPI_GETICONTITLELOGFONT` and
  `SPI_GETICONMETRICS`, `GetDialogBaseUnits`, and gdi32's stock fonts
  (`DEFAULT_GUI_FONT`, `SYSTEM_FONT` and the others).
  `GetSystemMetricsForDpi` and `SystemParametersInfoForDpi` give them at
  any DPI.
- **Awareness per thread.**  `SetThreadDpiAwarenessContext` used to be
  remembered and reported while everything followed the process.  Now a
  thread's context decides what it sees (`GetSystemMetrics`, monitor and
  cursor positions, `GetDpiForSystem`, stock fonts), and a window keeps
  the context of the thread that created it: its coordinates, bitmap and
  DPI are that awareness's, so an unaware thread of an aware program
  makes windows the desktop scales up and an aware thread of an unaware
  program makes sharp ones that get `WM_DPICHANGED`.  While a window
  procedure runs, its thread has the window's context, as on Windows.
  `SetProcessDpiAwarenessContext` sets the process default, which threads
  that never set their own follow.  `SetThreadDpiHostingBehavior` and
  `GetThreadDpiHostingBehavior` are accepted.
- `dpitest` checks it: at 192 DPI it measures a window of its own
  per-monitor-aware thread against one made by a thread switched to the
  unaware context (list box items, combo box field, scroll bar, menu bar,
  dialog units and dialog size twice the size, `GetDpiForWindow`,
  `GetWindowDpiAwarenessContext`, the window procedure's context, the
  unaware window's logical rectangle), and the system metrics, stock font
  and non-client metrics at 96; its unaware child does the same with a
  per-monitor-aware thread and its system-aware child with an unaware
  thread, where the metrics and fonts are at 192.  It also checks the
  `DPI_AWARENESS_CONTEXT_*` pseudo-handles.
- Not yet: a window's own parts are sized when it is created and drawn,
  so an edit or combo box made at 96 DPI keeps its measured line height
  when its window moves to 192 DPI (Windows rescales per-monitor v2
  dialogs on a DPI change; NovaOS does not); `GetWindowRect` and the
  other coordinate calls on a window of another awareness return that
  window's own coordinates rather than converting them; fixed pixel
  sizes inside the common controls (header minimum height, indents,
  margins) are still 96 DPI ones.

## soundtest dscapture: the recording buffer was half its size

With CI's test VMs on KVM, the core suite's `soundtest dscapture` crashed
after recording ("access violation at ntdll.dll+0x1e9f", that is
`RtlFreeHeap`, with a non-canonical address), while every TCG run passed.
It was not a DirectSound start-up race: `dsound`'s capture and mixer
threads start only after their buffer or device is fully set up.  The
fault was in the test program.  `dscapture` gathered its samples into
`calloc(total + half, 1)`, a size in bytes, then copied `total` 16-bit
samples into it, writing about 88 KB past the block.  What the overrun hit
depended on where the heap placed that block: on the TCG runs, memory
nobody used again; on the KVM run, the capture buffer's own `CBuffer`, so
releasing it freed pointers made of recorded samples.

Under TCG the crash reproduces by freeing a 200 KB block just before
`CreateCaptureBuffer`, so the recording buffer reuses that lower block and
`CBuffer` lies inside the overrun.  The buffer is now
`calloc(total, sizeof(short))`, and the same placement no longer crashes.

## Faster CI: ccache and docs-only pull requests

A pull request's CI took about 12 minutes, and half of that was building
NovaOS from scratch, twice: the boot-test and graphics jobs each compiled
about 2,400 files (most of them NetSurf, Mbed TLS, FreeType and musl's
libm) one at a time through `tools/build_userland.py`.

- **ccache.**  Every clang call in the build (kernel and userland) goes
  through ccache.  The boot-test job saves the cache after each build;
  pull requests start from main's latest cache, the graphics job and the
  nightly app corpus restore it too.  A build whose sources mostly match
  the cache takes about 2.5 minutes instead of 6.
- **Docs-only pull requests.**  When a pull request changes only Markdown,
  `docs/` or `LICENSE`, the Checks job says so and the build and boot jobs
  are skipped, which GitHub counts as passing for required checks.  Those
  pull requests finish in about a minute.

Next candidates, each its own change: compiling in parallel inside
`tools/build_userland.py` (the cached build is now mostly its serial
work), and running the test VMs with KVM, which the runners offer.

## File Explorer scroll bars

File Explorer's file list used to show only the rows that fit: the rest
could be reached only with the arrow keys, nothing said there was more,
and the wheel did nothing.  It now has scroll bars, as Windows Explorer
does.

- **The bars.**  The list has a vertical scroll bar whenever its rows don't
  fit and a horizontal one when the window is narrower than its columns
  need (the Name column keeps at least 200 pixels; below that the columns
  scroll sideways, the header with them).  Each bar takes room from the
  other way, so the two can appear together, with the corner between
  them filled.  The sidebar (the places and the drives) gets a vertical
  bar of its own when it doesn't fit.  The bars come and go as the window
  is resized and as the folder changes, and the row only partly in view
  at the bottom is drawn too.
- **Using them.**  The wheel scrolls three rows a notch (the sidebar when
  the pointer is over it, sideways when the list only scrolls that way),
  the horizontal wheel scrolls sideways, the arrows scroll a row, a click
  in the trough scrolls a page toward the pointer, and the thumb drags;
  a held arrow or trough repeats after 350 ms, every 50 ms, until the
  thumb reaches the pointer.  Page Down moves the selection to the last
  row in view and then a page on, Page Up the same way up, Home and End
  go to the first and last rows, Left and Right scroll sideways, and the
  selection is kept in view, including a new folder or file and a click
  on the part-shown row.
- **One scroll bar for the built-in apps.**  The built-in apps draw with
  the kernel's GDI, not user32, so the bar is `UiScroll` in
  `kernel/apps/apps.c`: user32's parts, sizes and behaviour
  (`userland/user32/scroll.c`: 17-pixel arrows, a thumb sized to the
  page, `SetScrollInfo`'s range, page and position) in the dark theme's
  colours.  Other built-in apps can use it the same way.
- **The test.**  The graphics suite's `explorer scroll bars` opens File
  Explorer on `C:\Windows\System32` and checks the vertical bar is there,
  then that the wheel, Page Down, End, Home, a click on the down arrow and
  one in the trough each move the view by what they should.  Explorer
  logs a `[EXPLORER]` line each time its view changes (the folder, the
  rows shown, the bars and where the list is on the screen), which the
  test reads.

## FileInternalInformation on a non-file handle

The "App corpus" check failed on pull requests (and would have failed
nightly): the first real ripgrep search died with a kernel page fault in
`RamfsFileId`, called from `UmSyscall` with a null node (CR2 0x170).
ripgrep asks `NtQueryInformationFile` for `FileInternalInformation` on its
standard handles, and for a console or pipe handle there is no `RamNode`, so
the dereference of `h->node` faulted and halted the VM; every later program
in the run then reported as not run (0 of 14).

`FileInternalInformation` now returns the node pointer as the file ID only
for file and directory handles and 0 for others, as `FileStatInformation`
already did.  No corpus entry was changed or removed, and the three required
check names are unchanged.

## Firefox in the App Store

The last program of the Phase 20 catalog item before Krita: stock
Firefox, not the Floorp fork that Phase 16 was debugged with, is now a
catalog program that installs and runs unmodified.  Floorp 12.19 and
Firefox 157 share the engine, so the browser itself ran as Floorp did;
what changed in NovaOS is around it.

- **The catalog entry** downloads Mozilla's full installer for 157.0
  (`archive.mozilla.org`, pinned like the other entries instead of
  `download.mozilla.org`'s floating "latest").  The full installer is a
  7-Zip self-extractor holding `core\` (the browser) and `setup.exe`; the
  Store's Install button unpacks it into `C:\Programs\Mozilla Firefox`
  and Open starts `core\firefox.exe`.
- **The nightly corpus installs it through the Store.**  An app-corpus
  program can now name its catalog entry (`App(store=...)`): its download
  must be the catalog's, it is put in `C:\Downloads` under the catalog's
  file name with 7-Zip in `C:\Programs\7-Zip`, and
  `Test(store=...)` runs `store install NAME` and waits for the Store's
  "Installed" line, so the corpus tests the same path a user's click
  takes.  Unpacking the installer takes a few minutes under TCG.
- **An HTTPS page.**  `tools/appcorpus.py` runs an HTTPS server on the
  host for programs that ask for it (`App(https=True)`), with a
  certificate for 10.0.2.2 from a CA made for the run.  Firefox trusts
  that CA through `distribution\policies.json` beside `firefox.exe`,
  Mozilla's documented way to configure a deployment, which also turns
  off the first-run pages, update checks, telemetry and the terms-of-use
  prompt (a dimmed tab-modal sheet on a new profile).  The page's
  screenshot is `tests/reference/firefox.png`.
- **Firefox's launcher process.**  `firefox.exe` starts a second
  `firefox.exe` (the browser) suspended, installs its DLL blocklist hooks
  in it, resumes it and exits.  The hooks' trampolines live in a
  `SEC_RESERVE` section the launcher commits and writes through its own
  view and maps into the child near ntdll (`MapViewOfFile3` with an
  address range, or `NtMapViewOfSection` with the child's process handle).
  NovaOS mapped views only into the calling process, so the launcher
  logged its failure (HRESULT 0x80070507 from `DllBlocklistInit.cpp`),
  ended the child, turned the launcher off in the registry and ran the
  browser itself.  `NtMapViewOfSection`, `NtMapViewOfSectionEx` and
  `NtUnmapViewOfSection` now take another process's handle, and
  committing pages of a section view succeeds (a section's pages are
  always there); `shmtest` checks both.  `ReportEventW` also prints an
  event's binary data, which is how the source file showed up.
- **Window titles.**  The desktop's title bars and the taskbar draw ASCII,
  and every other character of a window's title became `?`, so Firefox's
  "Page — Mozilla Firefox" read "Page ? Mozilla Firefox".  Dashes,
  curly quotes, non-breaking spaces, bullets and the ellipsis now get
  their nearest ASCII form, as Windows' best-fit code pages give them.

Firefox's other `[UM]` lines are harmless probes: `mfplat.dll` (Media
Foundation, it falls back to its own decoders), `profapi.dll` for the
Windows App SDK, `D3DKMTQueryStatistics`, and WinRT activation looking
for `Windows.UI.dll` and friends.  Still open: a publicly trusted HTTPS
site (the test network has no internet), and Thunderbird, which shares
the runtime but is untested.

## Firefox's delay-loaded DLLs and DirectWrite fallback

Two Phase 16 steps for Firefox (tested with Floorp 12.19): every DLL
`xul.dll` delay-loads now exists (16.2), and DirectWrite's font fallback
has a test (16.1).  No MIT, BSD or zlib implementation of these DLLs
exists (Wine's are LGPL, and the only HLSL compilers are LGPL too), so
they are NovaOS's own.

- **`d3d11.dll`** (`userland/d3d11`): NovaOS's own front for Direct3D 11.
  With DXVK installed from the App Store, which now installs DXVK's
  `d3d11` as `d3d11_dxvk.dll`, every entry point hands the call to DXVK
  (DXVK's `d3d10core` reaches it through `D3D11CoreCreateDevice`).
  Without DXVK, device creation fails with `DXGI_ERROR_UNSUPPORTED`, as
  on a PC with no Direct3D 11 driver, so programs use their software
  path.
- **`urlmon.dll`**: `CreateUri` (an `IUri` with every string and number
  property, IPv4/IPv6/DNS host types and default ports) and
  `CoInternetParseUrl` (scheme, domain, document, anchor, canonicalize,
  and path and URL conversion through shlwapi).
- **`winspool.drv`**: the print spooler's client, built and installed
  under its `.drv` name (`userland/winspool/build.py`), with Windows'
  ordinals for the default-printer calls (Firefox imports
  `GetDefaultPrinterW` as ordinal 203).  There are no printers yet: the
  lists are empty and opening a printer fails.
- **`credui.dll`**: the credential prompts report that the user
  cancelled, since NovaOS has no credential dialog yet.
- **`dhcpcsvc.dll`**: `DhcpRequestParams` finds no extra DHCP options, so
  a WPAD lookup moves on.
- **`d3dcompiler_47.dll`**: blobs (`D3DCreateBlob`, `D3DStripShader`),
  and a `D3DCompile` that fails with a message in its error blob.
- **`tools/pe_imports.py`** now checks delay-loaded imports, marked
  "(delay)", counts DLLs shipped beside a program, and reads `.drv`
  files.  It reports `0 missing` for Floorp's `xul.dll`.
- **Tests**: the new `delaytest` self-test exercises every one of these
  DLLs (31 checks, 64- and 32-bit).  The new `tools/dwtest`, in the
  graphics suite, lays out "Hello", an Arabic word and a Devanagari word
  in one line from a Latin-only font.  It checks that each script falls
  back to a font that has it, that the Arabic is joined and runs right to
  left, that the Devanagari conjunct forms, and that every run draws, and
  it shows the line in a window for the screenshot.
- `profapi.dll`, also on the 16.2 list, is not added.  Only
  `Microsoft.Internal.FrameworkUdk.dll` imports it, and that DLL also
  needs `Bcp47Langs`, `CoreMessaging` and `dcomp`.  It is Windows App SDK
  code Floorp runs without.

## Firefox over HTTPS: getpeername and shutdown (Phase 16.4)

With `http://` pages loading, every `https://` page stayed blank.  The
server saw a TCP connection close without a TLS ClientHello.  Firefox's
network log showed the first write failing with
`PR_ADDRESS_NOT_SUPPORTED_ERROR`: before it starts a handshake, NSS asks
the socket for its peer (`getpeername`) and accepts only an IPv4 or IPv6
address.  NovaOS filled in a socket's peer only when a *blocking*
`connect` returned, so after Firefox's non-blocking connect the peer came
back empty.

Once handshakes ran, the kernel crashed in lwIP's pool allocator a few
minutes in.  Firefox ends its TLS connections with
`shutdown(SD_BOTH)`, which NovaOS passed to lwIP as "close": lwIP then
frees the connection's control block on its own (at once when unread data
forces a reset, or when the connection ends, with no callback once the
receive side is shut), while the socket still pointed at it, and the
later `closesocket` freed it a second time.

- The kernel's TCP layer (`kernel/net/sock.c`) records the peer when the
  connection is made, so `getpeername` answers after a blocking or a
  non-blocking `connect` alike.
- `shutdown` shuts the receive side in NovaOS's own socket (unread and
  later data is dropped, `recv` returns 0) and only sends lwIP the FIN;
  lwIP reports the connection's end through the error callback
  (`ERR_CLSD`, now an orderly close rather than a reset), which clears the
  socket's pointer.
- `looptest` (network suite) checks `getpeername` after a non-blocking
  connect over 127.0.0.1 and ::1, and three rounds of
  `shutdown(SD_BOTH)` with unread data followed by the peer closing.
- With these, Floorp completes TLS handshakes (a self-signed test server
  gets its certificate warning page), and typing into a form field and
  scrolling a long page work.

## Firefox loads pages: wsock32.dll (Phase 16.4)

Floorp showed a blank page for every URL and no request ever left the
machine, while the same page from a `file:` URL rendered.  The parent's
socket thread opened a TCP socket and closed it again at once, never
calling `connect`.  The caller was NSPR's `_PR_MD_SOCKET` in `nss3.dll`,
which imports Winsock 1.1 (`wsock32.dll`) by ordinal and calls
`ioctlsocket` (`FIONBIO`) right after `socket`.  NovaOS answered
`wsock32.dll` with `ws2_32.dll` on the assumption that the two share
their ordinals; they do not: in `wsock32` `inet_addr` is 10, `inet_ntoa`
11 and `ioctlsocket` 12, where `ws2_32` has `ioctlsocket` at 10.  So
NSPR's `ioctlsocket` landed in `inet_ntoa`, whose non-zero return read as
failure, and every socket was closed before use.

- **wsock32.dll** (`userland/wsock32`) is now its own DLL, with Winsock
  1.1's ordinals, calling `ws2_32`'s functions, plus the old blocking-hook
  calls (`WSAIsBlocking` and friends, which never block).  The alias in
  the kernel loader, `ntdll` and `tools/pe_imports.py` is gone.
- A socket `accept` gives is non-blocking when its listener is, as on
  Windows.  NSPR's socket pair (the socket thread's wake-up) counts on
  that: with a blocking accepted end, the socket thread stalled in `recv`
  holding a lock the main thread then waited for.
- `looptest` (network suite) checks `wsock32`'s ordinals, takes a socket
  non-blocking the way NSPR does, and checks the inherited mode.
- With it, Floorp fetches and renders `http://` pages served to QEMU's
  guest network.

## First-boot setup: Welcome to NovaOS (Phase 22.1, name and display)

An installed NovaOS used to start straight onto a desktop that belonged to
"Dean Plude".  The first time it now starts from the disk Setup put it on,
**Welcome to NovaOS** (`kernel/apps/welcome.c`) opens before anything else
and asks, with no Terminal commands:

- **Your name.**  Up to 20 characters, without the characters Windows
  refuses in a user name.  It goes to `HKLM\SOFTWARE\NovaOS\Setup`
  (`UserName`) and to `RegisteredOwner`; programs started from then on
  get it as `USERNAME`, so `GetUserName` returns it, and the Start menu
  shows it with its initials.
- **Display.**  The modes the display offers, the current one marked; a
  click or the arrow keys switch to a mode at once and keep it across
  restarts, as Settings does.
- **Finish.**  `FirstBootDone` is set and the desktop takes over.

It opens by itself only when NovaOS did not start from the installation
media and drive C: is kept on a disk with Setup's two partitions
(`NOVA_EFI` and `NOVADATA`), so the QEMU test images and live sessions are
unchanged.  `start welcome` opens it on any system.

`whoami.exe` (System32) prints `nova-pc\name` from `GetUserName`, and the
Terminal's own `whoami` gives the same answer.  The core self-test
`welcome` answers the screens from the keyboard (a name, a resolution
tried and put back); `whoami` and `whoami builtin` then check the name.
Checked by hand in QEMU: installed from `nova.iso` onto an NVMe disk, the
first start from that disk opened the setup, and the second did not.

Time zone and keyboard layout pages are the rest of step 22.1: NovaOS
still keeps UTC and the US layout.

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

## Longer time slices in the foreground, console programs as the foreground process, and Winsock socket options

Three things the foreground boost (the history entry before) left out.

- **Quantum stretching** (`kernel/ke/scheduler.c`): the foreground
  process's threads now get a time slice three times as long as everyone
  else's, 60 ms (6 ticks) against 20 ms (2 ticks), as client Windows
  gives the foreground process 6 clock intervals against 2 ("Programs" in
  System Properties, PsPrioritySeparation 2).  A busy thread of the
  program the user works with is switched out a third as often by
  background programs of its priority.  The boost decay stays one level
  per 20 ms.
- **A console program in the Terminal is the foreground process** while
  that Terminal is active (`TerminalProgram`, `UmUpdateForeground`), as
  Windows treats a console's programs while their console window is in
  front: it gets the foreground boost and the longer slices.  Before, the
  active Terminal, a built-in app, made no process foreground at all.
  Other processes attached to the same console (a program's children)
  stay background, as the scheduler has one foreground process.
- **Winsock `setsockopt` and `getsockopt` are real** (`ws2_32`,
  `kernel/net/sock.c`, `NtNovaSockCtl` 9 and 10): they were a no-op that
  read back 0.  `TCP_NODELAY` switches Nagle's algorithm off in lwIP;
  `SO_RCVTIMEO` and `SO_SNDTIMEO` end a blocked `recv`, `recvfrom` or
  `send` with `WSAETIMEDOUT`; `SO_LINGER` with a zero timeout resets the
  connection on `closesocket` (`SO_DONTLINGER` too); `SO_REUSEADDR` lets
  two sockets that both set it share a port (lwIP's `SO_REUSE` is now on);
  `SO_KEEPALIVE` turns on lwIP's keep-alive probes (after 2 hours idle, as
  Windows); `SO_BROADCAST` and `IP_TTL`/`IPV6_UNICAST_HOPS` reach the
  PCB; `SO_RCVBUF`/`SO_SNDBUF` read back what was set (the receive ring
  stays 32 KB).  `SO_TYPE`, `SO_ERROR` (a failed connect or a reset) and
  `SO_ACCEPTCONN` answer.  An accepted socket takes its listener's
  options.  Options NovaOS does not model (`IPV6_V6ONLY`,
  `SO_EXCLUSIVEADDRUSE`, the AcceptEx context updates) are still accepted
  and ignored.
- **Measured** in QEMU (TCG, 2 CPUs): with twice as many busy NORMAL
  threads as CPUs, `prioritytest`'s run as the Terminal's program got
  slices of 59.6 ms at the median and a background copy's 19.7 ms; its
  boosts-off NORMAL waiter now waits out a 59 ms slice where it waited
  20 ms.  As the Terminal's console program, with a busy HIGHEST thread
  of a background process on every CPU, its woken NORMAL thread ran after
  0.17 ms at the 95th percentile and the background one's after 3.2 s.
  `looptest`: with Nagle's algorithm the second of two one-byte sends
  arrived after 199 ms (the delayed ACK), with `TCP_NODELAY` after 9.6 ms;
  `SO_RCVTIMEO` 300 ms ended `recv` after 299 ms.  `boosttest`, run from the Terminal, now expects the +2 foreground boost
  on top of each increment.  The core suite passes but for `soundtest
  volume`, which needs PulseAudio on the host; the network suite passes.
- **Not done**: `SO_LINGER` with a non-zero timeout closes as without it
  (a blocking `closesocket` does not wait for the data to be sent);
  `SO_RCVBUF` does not resize the receive ring or TCP window; sending a
  broadcast still works without `SO_BROADCAST` (lwIP's `IP_SOF_BROADCAST`
  stays off).

## GOG: OpenTTD installs and plays, GOG GALAXY's setup runs

Dean asked for games.  GOG's own copies need an account to download, so
the free game GOG offers, OpenTTD, comes from its own Windows installer
(NSIS) with OpenGFX, and GOG GALAXY from GOG's public offline installer
(Inno Setup 6, 342 MB).

Setup programs need to run as administrator.  The desktop user's token
is now the limited half of an administrator's (Administrators deny-only,
`TokenElevationType` limited) with a full, elevated linked token
(`TokenLinkedToken`); `ShellExecuteEx`'s `runas` verb, a program whose
manifest asks for `requireAdministrator` or `highestAvailable`, and
`CreateProcessAsUser` with the linked token start a program elevated
(`NtSetInformationProcess(ProcessAccessToken)`), and `IsUserAnAdmin`
answers from the token.  Drive C: takes files up to 2 GB (was 256 MB) and
grows a file's buffer by a quarter past 64 MB.

For Inno Setup's wizard: `riched20.dll` and `msftedit.dll` (Rich Edit
2.0 and 4.1 on the Edit control, taking RTF by `EM_STREAMIN` and the rest,
for its licence page), code pages 1252, 28591 and 20127 in
`MultiByteToWideChar` and `WideCharToMultiByte` (RTF's `\'e9` escapes),
oleaut32's `VarAdd` ... `VarCmp` (Delphi's variants), `AddFontResource` of
a bare file name finding the font in the Fonts folder, task dialogs with
custom buttons (Inno's Retry/Ignore/Cancel prompt showed only OK), the
shortcut object's `IPropertyStore` (its AppUserModelID), and
`CryptProtectMemory`/`RtlEncryptMemory` for the Visual C++
Redistributable's installer.

OpenTTD 15.3 installs silently and reaches its main menu: the new app
corpus test `910-gog-openttd`.  GOG GALAXY's setup now copies its files
and makes its shortcuts; starting the client fails on `mfc140u.dll`,
which comes with the Visual C++ Redistributable, whose installer (WiX
Burn) needs MSXML first.  New self-test `setuptest` (x64 and x86) covers
all of the above.

## A GPU path in QEMU (Venus)

- **The virtio GPU driver does 3D.**  On a QEMU `virtio-vga-gl` or
  `virtio-gpu-gl-pci` that offers 3D, blobs and context types (QEMU 9.2 or
  newer with `venus=on,blob=on,hostmem=...`), `kernel/drivers/virtio_gpu.c`
  takes them, reads the capability sets and finds the card's host-visible
  memory (`[VGPU] QEMU virtio-vga: 3D, 3 capability set(s), 1024 MiB of
  host-visible memory at ...`).  It now keeps up to 42 commands in flight
  at once, each in its own slot, because fenced 3D commands finish out of
  order; the 2D path (monitors, hot-plug) goes through the same queue.
  On a card without 3D nothing changes.
- **Vulkan programs reach it through `NtNovaGpuCtl`** (system call
  0x263, `kernel/um/um_gpu.c`), NovaOS's own version of what Linux's
  virtio-gpu DRM interface gives Mesa.  A handle is a 3D context on a
  capability set.  It submits command streams with fences, creates blobs,
  and maps a mappable blob as a section of the card's host-visible memory
  that `NtMapViewOfSection` maps (its views go away with the process).  It
  also keeps timelines that the fences advance, which a process reads,
  writes and waits on.  Every structure uses 64-bit fields, so 32-bit
  programs pass the same ones.
- **Venus, Mesa's Vulkan driver for virtio-gpu, runs on it.**
  `tools/build_venus.py` fetches Mesa 26.2.4 and patches it
  (`third_party/mesa-venus`): a renderer back end for NovaOS
  (`vn_renderer_nova.c`), Windows window surfaces for Venus, and a present
  that waits for the frame.  It then builds `vulkan_virtio.dll`, 64- and
  32-bit, with MinGW-w64.  The CI publishes the result, `venus.7z`, beside
  `nova.iso` on the "latest" release, and the App Store lists it as
  **Venus** (Runtimes).  Installed next to Mesa 3D and DXVK, it runs
  Vulkan programs, and Direct3D 8-11 ones through DXVK, on the host's GPU:
  `D3D9 adapter  Virtio-GPU Venus (llvmpipe (LLVM 20.1.2, 256 bits))`
  where the host has only Mesa's lavapipe, as CI's runners do.  Frames
  come back through host-visible memory and are drawn with the GDI.
- **The Vulkan loader prefers a GPU.**  `vulkan-1.dll` tries the drivers
  registered under `HKLM\SOFTWARE\Khronos\Vulkan\Drivers` that draw on a
  GPU before the ones that draw on the CPU (lavapipe), as the Khronos
  loader orders its physical devices.  Venus declines when there is no 3D
  virtio-gpu, so the same installation falls back to lavapipe on any
  other display.
- **CI's graphics boot runs on it.**  Its first monitor is now a
  `virtio-vga-gl,venus=on,blob=on,hostmem=1G` (the second is still a
  `secondary-vga`).  QEMU 10.2.1 and virglrenderer 1.3.0 with Venus are
  built from source by `tools/ci/build-qemu-venus.sh` and cached, and run
  on Xvfb with an SDL OpenGL window.  Ubuntu 24.04's QEMU 8.2 has no Venus.
  `tools/novarun.py` runs the QEMU that `NOVARUN_QEMU` names and opens
  that window only for a 3D GPU.  `d3dtest` (17 tests, 64- and 32-bit)
  must report the Venus adapter.  The new `d3dtest fps 10` draws a Direct3D
  9 scene that keeps the rasterizer busy (64 blended quads over 640x480)
  on Venus and on lavapipe, each in a child process whose
  `VK_DRIVER_FILES` names the driver, and Venus must draw more frames per
  second.  Under TCG it draws 14 to 17 frames per second against
  lavapipe's 0.12 to 0.13, and the animated part of `d3dtest` 461 frames against
  62.
- Not yet: virgl, which would put OpenGL on the host's GPU (OpenGL stays
  on llvmpipe); showing Vulkan's frames on the virtio GPU directly
  instead of copying them through the GDI; placed memory maps
  (`VK_EXT_map_memory_placed`); and a test on a host with a real GPU
  (CI's is lavapipe).

## GTK pointers and Wintab pen tablets

Two gaps [GTK programs](#gtk-programs-inkscape) left open: GTK's own
pointers did not show (Inkscape's tools all had the arrow), and there was
no `wintab32.dll`, so GTK, Qt and Krita found no pen pressure.

- **DIB sections of 1, 4, 8 and 16 bits per pixel.**  GDK makes a cursor
  from a pixbuf as a 32-bit image with alpha (a `BITMAPV5HEADER` section)
  and a 1-bit mask (a `BITMAPV4HEADER` section with two colours), then
  `CreateIconIndirect`.  gdi32 took only 24- and 32-bit sections, so the
  mask failed and GDK fell back to the default pointer.  A section of
  fewer bits now keeps the program's own rows and colour table beside the
  32-bit pixels gdi32 draws on, synced at each use the way 24-bit sections
  already were: what the program writes is read as its colours, and what
  gdi32 draws is written back as the nearest index (only where it
  changed).  `GetObject` describes them, `GetDIBColorTable` and
  `SetDIBColorTable` work on them (recolouring keeps the indices), 16-bit
  ones are 5-5-5 or, with `BI_BITFIELDS`, 5-6-5, and a DIB's colour table
  is found after its header whatever the header's size.
- **Monochrome cursors.**  `CreateIconIndirect` with only a mask (twice the
  cursor's height, the AND half over the XOR half) and `CreateCursor`'s
  planes now give white, black and clear pixels (white was black before);
  an "invert the screen" pixel, which the desktop cannot draw, is black.
- **Pens** (`kernel/wm/tablet.c`).  USB digitizer pens (HID page 0x0D: tip
  pressure, in range, barrel buttons, eraser) report packets beside the
  pointer motion they make (the tip clicks, the barrel button
  right-clicks); `usbcheck` runs a pen's report descriptor through the
  parser.  A plain absolute pointer such as QEMU's `usb-tablet` stays a
  mouse, as on Windows.  Programs can make a pen too:
  `CreateSyntheticPointerDevice(PT_PEN)` and `InjectSyntheticPointerInput`
  (Windows 10's pointer injection) move the pointer, click with the tip
  and send the pen's pressure; `SM_DIGITIZER` reports `NID_EXTERNAL_PEN`
  while a pen is there.  The desktop keeps every pen's last 256 packets,
  numbered, for programs to read (`NtNovaGuiCtl` op 30).
- **`wintab32.dll`**, written from the Wintab 1.4 specification (Wine's is
  LGPL and was not used).  With no pen, `WTInfo(0, 0, NULL)` is 0, which
  GTK and Qt take as "no Wintab", and `WTOpen` fails.  With one, there is
  one device with a pen and an eraser cursor, X and Y 0-65535 and pressure
  0-1023.  A context gets the packets that come while it is enabled and
  one of its process's windows is in front: mapped to its output extents
  (a negative extent turns the axis round, as GTK asks for Y), laid out
  as its `lcPktData` asks, buttons and pressure absolute or relative
  (`lcPktMode`), in a queue of the size `WTQueueSizeSet` gives, with
  `WT_PACKET`, `WT_PROXIMITY` and `WT_CSRCHANGE` posted to its window.
  `WTPacket`, `WTPacketsGet`/`Peek`, `WTDataGet`/`Peek`,
  `WTQueuePacketsEx`, `WTEnable`, `WTOverlap`, `WTGet`/`WTSet` and the
  rest of the interface are there, by name and by Wintab's ordinals.
- **Tests**: `bmpcurtest` (the sections, GDK's two kinds of cursor, and the
  desktop showing a program's pointer set as the class cursor) and
  `wintabtest` (wintab32 loaded as GTK loads it, with no pen and with a
  synthetic one) in the core self-tests.

Not yet: tilt and rotation (no pen reports them yet), pens on virtio input
(QEMU has none with pressure), and pen `WM_POINTER` messages.

## GTK programs: Inkscape

Phase 20.4 starts with Inkscape 0.91, unmodified: the GTK 2 build in
conda-forge's win-64 channel (a MinGW build carrying its own GTK 2,
cairo, pango and about 70 DLLs).  Inkscape 1.x is a GTK 3 program whose
downloads (inkscape.org, MSYS2's mirrors, GitLab's artifacts) are not
reachable from CI, so it is not tested yet.  Inkscape now starts with a
new document and shows its menus, tool bars, toolbox, rulers, canvas,
palette and status bar.  What was missing:

- **More DLLs per process.**  The loader stopped at 64 modules ("too many
  DLLs"); a GTK program brings about 70.  The limit is 128 now, and the
  loader information page ntdll reads (`NOVA_LDR_INFO`) grew to six
  pages, moving the process parameters and stubs after it.  GTK loads its
  pixbuf loaders and theme engine by relative paths with forward slashes
  (`lib/gdk-pixbuf-2.0/2.10.0/loaders/...`), which the loader now takes as
  paths.
- **Regions with more than one rectangle.**  gdi32 kept every region as
  its bounding box.  GDK 2 draws client-side child windows and clips
  each toplevel paint to the window minus its children, so the bounding
  box let the toplevel's background erase the canvas.  Regions
  (`userland/gdi32/region.c`) now keep their rectangles: `CombineRgn`
  with all five modes, `ExtCreateRegion`, `GetRegionData`, `PtInRegion`,
  `RectInRegion`, `EqualRgn`, `OffsetRgn`, `FillRgn` and `PaintRgn`; a
  DC's clip keeps up to 128 rectangles (beyond that, their bounding box),
  and fills and blits draw piece by piece.
- **Text under a world transform.**  cairo's win32 backend creates its
  fonts 32 times too large and draws them through a `GM_ADVANCED` world
  transform of 1/32, asking `GetGlyphOutline` for metrics in device
  space.  `ExtTextOut` (positions, the clip rectangle and the `lpDx`
  advances) and `GetGlyphOutline` now apply a scale-and-translate
  transform, so the text is the right size and spacing instead of blank
  or spread out.
- **`StretchDIBits` with negative extents.**  cairo uploads image surfaces
  with both heights negative and a source y counted from the bottom of
  the image, top-down images included, as Windows does.  gdi32 took the
  source y from the top and lost a row of each band; it now follows
  Windows (an extent of -h from y covers y-h+1..y, and the image is
  mirrored only when the destination and source signs differ).
- **Smaller pieces**: `mscms.dll` (`GetColorDirectory` and an empty
  `EnumColorProfiles`), `Arc`/`Pie`/`Chord`, `CreateBitmapIndirect`,
  `MaskBlt`, `GetNearestPaletteIndex`, `ResetDC`, failing enhanced-metafile
  calls, `SetCriticalSectionSpinCount` (Inkscape's GLib crashed on its
  stub), `GetCPInfoExA`, `Module32First/Next`, `GetCurrentHwProfileA`,
  `AssocQueryKeyW`, `ExtractIconExA`, `SHAppBarMessage`, and msvcrt's
  `swscanf`/`vswscanf` exports, `_splitpath`, `_wsplitpath` and
  `__lconv_init`.
- **Tests**: Inkscape in the nightly corpus (`tests/appcorpus/880-inkscape.py`):
  its new-document window's screenshot must match
  `tests/reference/inkscape.png`.

Still open: GDK's monochrome cursors (`CreateDIBSection` takes only 24-
and 32-bit DIBs, so GTK falls back to the default pointer), the hicolor
icon theme warning, and no Wintab tablets.

## Install and power on a laptop (Phase 21.5)

The reference ThinkPad T14 Gen 4 keeps its lid and battery behind an
embedded controller, has no S3 sleep, may hide its HPET, and has its disk
on NVMe.  NovaOS now handles each of these; [install-and-power.md](install-and-power.md)
has the details and the checks to run on the machine.

- **Embedded controller** (`kernel/hal/ec.c`): the ACPI embedded
  controller from the ECDT or the PNP0C09 device, as uACPI's address
  space handler for EmbeddedControl regions (with the global lock when
  `_GLK` asks), and its events (`_Qxx` after QR_EC) from its GPE, polled
  as well.  Without it a laptop's `_LID`, `_BST` and `_PSR` read nothing.
- **Sleep without S3** (`kernel/ke/sleep.c`, `kernel/hal/aml.c`): on
  firmware with no `\_S3` but the FADT's low-power S0 flag or an LPS0
  device, Sleep and closing the lid blank the screen, call the LPS0
  device's `_DSM` (Intel's and Microsoft's functions, in Linux's order)
  and idle until the lid opens, the power button is pressed or, with the
  lid open, a key or the mouse is used.  `GetPwrCapabilities` reports
  AoAc instead of S3 there.
- **Timers without an HPET** (`kernel/arch/x86_64/apic.c`): the TSC and
  the APIC timer from CPUID leaf 0x15 (0x16's base frequency when the
  crystal isn't given); the PIT, the last resort, can no longer hang the
  boot when the chipset gates its clock.
- **Installing** (`kernel/apps/terminal.c`, `bootloader/src/main.c`,
  `kernel/drivers/nvme.c`): the Terminal's `install [disk] [/fat]` does
  what the Setup app does; the first start from an installed disk adds a
  "NovaOS" firmware boot entry and puts it first in BootOrder; an Intel
  VMD controller hiding the NVMe disks is named in the log and by
  `install`, with the firmware setting that turns it off.
- **Test.**  The devices suite boots a "laptop" (`tests/acpi/laptop.asl`,
  QEMU without S3): the battery through the embedded controller (a model
  of one in `ec.c`, as QEMU has none), the lid sleeping it in S0 idle and
  waking it, `install` from the USB stick onto an NVMe disk, and the first
  start from that disk adding its boot entry.

## The I219 Ethernet controller of Intel PCs (Phase 21.3)

The reference machine's wired port is an Intel I219, the Ethernet MAC
built into Intel chipsets since 2015, with its PHY on a separate chip.
NovaOS's `e1000e` driver knew only the 82574L that QEMU emulates.

- **The driver** (`kernel/drivers/e1000.c`) now takes the 55 I219-LM and
  I219-V device IDs from Sunrise Point to Nova Lake chipsets (the
  ThinkPad T14 Gen 4's Raptor Lake-P among them), names each as Windows
  does ("Intel Ethernet Connection (16) I219-LM") and brings it up after
  Intel's BSD-licensed shared code in FreeBSD (`e1000_ich8lan.c`): out
  of D3 and Ultra Low Power mode (through the ME firmware on vPro
  machines, a LANPHYPC power cycle otherwise), the PHY found on MDIO
  (directly, in SMBus mode, or after a power cycle), the reset the MAC
  and the PHY share under the hardware semaphore, the per-generation
  errata, and the settings for the speed the link comes up at.  It
  repeats this after sleep, and logs each step, so a boot log shows how
  far a real machine got.  [docs/ethernet.md](ethernet.md) lists the IDs
  and the steps.
- **Shared with the 82574L**: the PHY over MDIO (its ID in the boot
  log), auto-negotiation at 10, 100 and 1000 Mb/s, link changes logged
  with speed and duplex, the BAR mapped wherever the firmware put it,
  and the PCI function woken from D3.
- **Test.**  The network suite boots a third time, with QEMU's e1000e
  instead of virtio-net (`tests/selftest/network-e1000e`): the PHY and
  link in the boot log, the link pulled and plugged back (`ipconfig`
  shows the media disconnected, then the address again), `ping`, sleep
  and wake with `ping` after it, and the IPv4 tests (Winsock, winhttp's
  HTTP/2, `looptest`, `prioritytest net`) on it.  The I219's own steps
  can only be checked on the machine itself.

## Kernel service threads above programs, and the foreground boost

Since programs got real priorities, a busy `HIGH_PRIORITY_CLASS` program
(13) outranked the kernel's network, USB, ACPI and drive-saving threads,
which still ran at 8: on a machine with every CPU busy at HIGH, a byte
sent over a 127.0.0.1 connection took 3.6 s to arrive, the time the balance
set takes to lift a starved thread.  And Windows' boost for the process
the user is working with was missing.

- **Every kernel thread above programs** (`kernel/ke/scheduler.h`), as
  Windows runs its system threads in the real-time range, ordered by how
  short and urgent their work is: the device poll thread and the audio
  mixer at 19 (were 17), the network stack and the USB thread at 18 (were
  8), the desktop and the threads that start programs at 17 (were 16 and
  8), and bulk work at 16: saving drive C:, ACPI and Setup (were 8).  The
  only kernel threads left at 8 or below are boot-time tests and csrss's
  stub, which never runs.  None of them spins: each blocks or sleeps when
  idle and gives way to any thread while it polls a device.
- **The network thread no longer yields round and round** while a program
  waits for data: above programs, each yield handed the CPU to a busy
  program for its whole 20 ms slice.  It now polls the adapter every
  millisecond on a timed wake-up, which preempts a busy program, and keeps
  the CPU free otherwise (it used to spin a CPU for as long as any program
  waited on a socket).  While data is moving it goes round again at once,
  or, when programs are waiting for its CPU, after 0.2 ms
  (`sched_yield_goes_lower`).
- **The foreground boost**: the process whose window is active (an owned
  or modal dialog's too, not an IDLE-class one) is the foreground process.
  Its threads get NT's PsPrioritySeparation, 2 on client Windows, on top
  of every wake-up boost, still never above 15: a woken NORMAL thread of
  the foreground process runs at 11 where a background one runs at 9.
  The desktop follows the active window every tick (`UmUpdateForeground`),
  and `NtQueryInformationProcess(ProcessPriorityClass)` reports it in
  `Foreground`.  Windows' other foreground mechanism, longer time slices
  for the foreground process (quantum stretching), is not done.
- **Measured** in QEMU (TCG, 2 CPUs) with `prioritytest`: with a busy
  HIGH thread on every CPU, 40 loopback round trips took 8.9 ms at the median and 20 ms at worst
  (on main the first one took 3.6 s); with a
  background process's busy `THREAD_PRIORITY_HIGHEST` thread on every CPU,
  the foreground process's woken NORMAL thread ran after 0.09 ms at the
  95th percentile and the background process's after 3.1 s (the balance
  set).  `sleeptest timer` (0.37-0.56 ms) and `boosttest` (0.07-0.08 ms) are
  as on main; `smpstress` on 4 CPUs passes, its 8-thread critical section
  1740-2420 ms against 910-2180 ms on main on the same host (no change
  beyond the run-to-run spread); the network suite and the devices
  suite's USB boots all pass.
- **Not done**: quantum stretching; a console program in the Terminal is
  never the foreground process (the Terminal is a built-in app, not the
  program's window); Winsock's `setsockopt` is still a no-op, so
  `TCP_NODELAY` does nothing (the test bounces each byte back so Nagle's
  algorithm never waits for a delayed ACK).

## Kernel under KVM

The kernel now runs correctly under KVM on AMD hosts such as GitHub's
runners.  The failures the earlier trial found ("Test VMs under KVM in CI")
had one cause: SYSRET's stack selector.

- **SYSRET and RPL 3.**  STAR[63:48] held 0x10.  Intel CPUs, and QEMU's
  emulation, force RPL 3 on the selectors SYSRET loads, but AMD CPUs load SS
  as STAR[63:48] + 8 unchanged, so programs ran with SS = 0x18 (RPL 0).
  That works in 64-bit mode until an interrupt taken in the program returns:
  IRETQ checks the saved SS and raises #GP after the SWAPGS, and the kernel
  went on with the program's TEB as its per-CPU block.  This was the boot
  #GP in the ACPI thread (`uacpi_gas_read_mapped`), the network tests that
  never finished, and the silent hangs at boot.  The STAR base is now
  0x13, so SS = 0x1B and CS = 0x23 on every CPU.
- **GS checks.**  An interrupt from the kernel that finds the user GS base
  loaded, or a return to a program with a kernel GS base, now stops the
  kernel with a report (vector, frame, backtrace, the IRETQ frame) instead
  of running on with the wrong per-CPU block.
- **Big kernel lock.**  `bkl_acquire`, `bkl_switch_in` and `bkl_relax`
  raised the thread's lock depth before `raw_lock` had the lock, so an
  interrupt taken while it halted (the ACPI SCI) ran its handler as though
  it held the lock.  The depth now goes up once the lock is taken.
- **Scheduler.**  A thread still on a timed-sleep list when it blocks
  leaves the list first (it could otherwise be queued twice), and the run
  queue stops the kernel with a backtrace if a thread is ever queued twice
  or switched to when not ready.  An idle CPU looks at the run queues again
  after marking itself idle, so a thread queued in that window is not left
  until the next tick.  A switch asked for while a CPU waited for the
  kernel lock now happens on the way back to the program.  The ACPI thread
  records its own thread pointer at start, closing a race with
  `AmlInitialize`.
- **Serial output.**  The Terminal's copy of program output to the serial
  port takes the kprintf lock, so a kernel message from another CPU no
  longer lands in the middle of a program's line (stltest failed on that).

Results under KVM on GitHub's runners (five CI runs and three-job soak
runs): boot in 10-11 s; network suite 9 of 9 (about 55 s, against 63 s on
TCG); devices suite 6 of 6; graphics job green (test step 111 s, against
193 s on TCG); core suite 56 of 57.  The one left is `sleeptest timer`: a
timed wait on an idle or busy CPU now and then ends at the next 10 ms tick
instead of its deadline (traced: the CPU's timer did not fire for the
sleeper's deadline), which puts the 1 ms timer queue timer's 95th
percentile at about 9 ms under load.  So CI stays on TCG
(`NOVARUN_ACCEL=tcg`); switching it to KVM is the udev step and dropping
that line once `sleeptest timer` passes there.  No test was loosened.

## Keyboard layouts: the first-boot setup's keyboard page (Phase 22.1)

Every key typed what a US keyboard prints on it, in the desktop's own
apps and in Windows programs alike.  The keyboard layout is now the
user's choice, kept where Windows keeps it, `HKCU\Keyboard Layout\Preload`
`"1"` (the layout id, `00000407` for German):

- **Welcome to NovaOS** has a **Keyboard** page after the time zone: a
  list of layouts (arrow keys, Page Up/Down or the mouse) that switches
  as you move through it, and a box under it to try the keys in.
  **Settings > Time & language** shows the layout, and its Change button
  opens the same page alone.  This finishes step 22.1: a fresh install
  reaches the desktop through name, time zone, keyboard and display
  pages with no Terminal command.
- **The layouts**: US, UK, US Dvorak, German, Swiss German, French, Swiss
  French, Canadian French, Spanish, Italian, Portuguese, Brazilian
  (ABNT2, with its extra `/?` key), Swedish, Finnish, Norwegian and
  Danish, each with Shift, AltGr and Shift+AltGr characters and dead
  keys (accents for the next letter: ´ then e is é).  The tables
  (`userland/include/kbdlayouts.h`) are generated by
  `tools/gen_keyboards.py` from xkeyboard-config (MIT/X11 licence)
  through libxkbcommon; virtual-key codes follow Windows' rules (letters
  their own, so German Y and Z swap theirs).  The registry lists them as
  Windows does, `HKLM\SYSTEM\CurrentControlSet\Control\Keyboard
  Layouts\KLID` (`Layout Text`), rebuilt at every start.
- **The kernel** (`kernel/wm/kbdlayout.c`, `wm/input.c`) types with the
  layout in the desktop, the Terminal and every built-in app, and sends
  Windows programs the layout's virtual-key codes, with AltGr as Ctrl+Alt
  (a left Ctrl comes with the right Alt, as on Windows).  It publishes
  the layout's handle to programs in `KUSER_SHARED_DATA` and follows a
  program that changes `Preload`.
- **user32** (`userland/user32/kbd.c`): `ToUnicode(Ex)` and `ToAscii(Ex)`
  with AltGr and dead keys (`TranslateMessage` posts `WM_DEADCHAR`),
  `MapVirtualKey(Ex)`, `VkKeyScan(Ex)`, `GetKeyNameText`,
  `GetKeyboardLayout`, `GetKeyboardLayoutName`, `GetKeyboardLayoutList`,
  `LoadKeyboardLayout`, `ActivateKeyboardLayout` (a program's own
  choice) and `SystemParametersInfo`'s `SPI_GET/SETDEFAULTINPUTLANG`
  (the user's layout) follow it.

The core self-test `welcome` now picks German on the new page and types
Y, Z and ´ e in its box (`zyé`); the new `kbdtest` (64- and 32-bit)
checks German as programs see it and US, French and German by handle,
then puts US back through `SPI_SETDEFAULTINPUTLANG` for the later tests.

Not yet: the desktop's own font has ASCII only, so its apps draw an
accented letter as the letter with the accent drawn over it, and other
characters (ß, €) as an empty box (Windows programs draw them all); the
Terminal's own line editing takes ASCII (programs reading keys get every
character); one layout at a time (no switching with Win+Space, and no
`WM_INPUTLANGCHANGE`).

## Krita leftovers: asyncio, keyboard shortcuts, a painted stroke

What #122 left open on Krita 5.3.4, fixed in NovaOS:

- **Overlapped sockets that wait.**  Krita's Python scripter plugin
  imports `asyncio`, whose Windows event loop (the proactor) asks Winsock
  for `AcceptEx`, `ConnectEx` and the other extension functions through
  `WSAIoctl(SIO_GET_EXTENSION_FUNCTION_POINTER)`; NovaOS answered
  `WSAEOPNOTSUPP` and the import failed.  `ws2_32` now hands out
  `AcceptEx`, `ConnectEx`, `DisconnectEx`, `GetAcceptExSockaddrs`,
  `TransmitFile`, `WSARecvMsg` and `WSASendMsg`, and an overlapped
  request that has to wait (an `AcceptEx` with no connection yet, a
  `WSARecv` with nothing to read) returns `WSA_IO_PENDING` and finishes
  later on the socket's I/O completion port, as on Windows.  The request
  keeps its own copy of the caller's `WSABUF` array (it often lives on the
  caller's stack), `AcceptEx` completes on the listening socket's port,
  `CancelIoEx` and `closesocket` end it with `ERROR_OPERATION_ABORTED`,
  and the port is captured when the request goes pending, so a completion
  still arrives after the socket's handle value has been reused.  The
  kernel's socket control gained "accept into this socket" for `AcceptEx`.
  New self-test `overlaptest` (64- and 32-bit); `asyncio.run` with a TCP
  echo server and client works under NovaOS's Python.
- **Ctrl+N and the other shortcuts.**  `GetKeyState` gave 0x8000 for a
  key that is down; Windows gives 0xFF80 (0xFF81 when toggled), and Qt
  reads the modifiers with `GetKeyState(VK_LCONTROL) & 0x80`, so in every
  Qt program Ctrl, Shift and Alt never looked held and Ctrl+N was a plain
  N.  `user32` also never sent `WM_ACTIVATEAPP`; every top-level window of
  a program now gets it (TRUE) before `WM_NCACTIVATE` when the program
  gains the foreground, and FALSE when it loses it.  `inputtest` checks
  both.
- **A stroke in the corpus test.**  The Krita corpus test now opens its
  new image with Ctrl+N (no click first), drags a stroke across the canvas
  with the default brush, checks the canvas pixels changed, and takes the
  stroke away with Ctrl+Z.  The test
  harness (`tools/novarun.py`) gained `drag`.

Still open: Krita needs OpenGL 2 or later (its Qt Quick widgets build
shaders), so it still needs the App Store's Mesa 3D.

## Sound and the touchpad on the reference laptop (Phase 21.4)

Phase 21's fourth step is the ThinkPad T14 Gen 4's sound and touchpad.
Neither can be emulated by QEMU, so each code path has a QEMU-side check
against a modelled device, and the machine itself gets a hand check
([hardware.md](../hardware.md)).

- **HD Audio controllers with the DSP on.**  Intel's controllers since
  Skylake sit beside an audio DSP; with the DSP enabled in the firmware,
  as on the T14 (Raptor Lake-P, `8086:51ca`), they report PCI class 04.01
  instead of 04.03, with the same HD Audio registers.  The driver now
  takes those by device ID (the list from FreeBSD's `hdac`, BSD licence),
  and other Intel class 04.01 functions only if their version registers
  read HD Audio 1.0, so an AC'97 card (also 04.01, with I/O BARs) is left
  alone.  Around the controller reset it turns off the link's dynamic
  clock gating (`CGCTL.MISCBDCGE`), and it selects traffic class 0 and
  snooped DMA, as Linux does on these chips; codecs get up to 100 ms to
  announce themselves.  The registers are reached through `PciMapBar`, so
  a BAR above the 64 GiB physical map works too.  The digital
  microphones hang off the DSP and stay silent.
- **Speakers and headphones.**  Output pins are routed as before, and the
  driver now remembers each codec's speaker pins and the headphone jacks
  that can sense a plug.  Twice a second (from the mixer thread) it reads
  the jacks and turns the speaker pins off while headphones are in,
  logging `[HDA] Headphones plugged in: speakers off`.  Realtek's ALC256
  family (ALC256, ALC257 as in the T14, ALC236) gets the one vendor
  setting Linux makes for it at start: processing coefficient 0x36 =
  0x5757, which keeps pin 0x1A's PC-beep loopback out of the outputs.
  The boot log line for each codec now carries its subsystem ID, which a
  machine-specific fix needs.
- **I2C-HID touchpads** (`kernel/drivers/i2chid.c`, `i2c_dw.c`).  Laptop
  touchpads are HID devices on an I2C bus.  The ACPI thread now finds
  every PNP0C50 device whose `_STA` says present and reads its I2C
  address, speed and controller from `_CRS`, the HID descriptor register
  from `_DSM` and the controller's PCI function and timings from `_ADR`,
  `FMCN` and `SSCN`, and powers both up (`_PS0`).  The controller driver
  takes Intel's LPSS I2C functions (DesignWare cores, recognised by their
  signature, set up as FreeBSD's `ig4` does) and makes polled transfers.
  The I2C-HID driver reads the HID descriptor, powers the device on,
  resets it and waits for the acknowledge, and hands the report
  descriptor to the USB HID parser.  Touchpads start in mouse mode, so
  relative motion and the click come from their mouse collection; the
  parser now recognises a Touch Pad collection and leaves its finger
  reports alone (which also stops a USB precision touchpad being taken
  for a touch screen).  The touchpad's interrupt is a GPIO pin, and
  NovaOS has no GPIO driver yet, so the `i2chid` thread polls it, every
  10 ms while reports come and every 50 ms when idle, as FreeBSD's
  `iichid` does without an interrupt.  It stops polling around sleep and
  sets the controller and device up again after waking.
- **Tests.**  The Terminal's `hwcheck` (core suite) runs the controller
  matching on the T14's IDs, the codec setup and jack handling on a
  modelled ALC257 (speaker and headphone pins routed with EAPD, the
  headset microphone recorded, coefficient 0x36 written, speakers off
  with headphones in and on again after), and the I2C-HID protocol on a
  modelled touchpad (descriptor, power, reset acknowledge, finger reports
  ignored, mouse reports moving and clicking, an absent device not
  taken).  The core boot gets an ACPI table (`tests/acpi/i2c-touchpad.asl`)
  describing a touchpad as a ThinkPad's DSDT does, on an I2C controller
  QEMU doesn't have: the boot log must show it found and the controller
  reported missing, and a second model whose `_STA` says absent skipped.
  The devices suite's `usbheadset` boot gains an AC'97 card, which the HD
  Audio driver must not take.

On the T14 the hand check is: `devices` shows the audio controller with
**HD Audio** and an I2C controller with **I2C (touchpad)**;
`soundtest tone 440 1000` plays through the speakers and, with
headphones plugged in, through the headphones only; the touchpad moves
the pointer and clicks.  It has not been done on the machine yet.

## Locale formatting and the user locale

Until now `GetDateFormat`, `GetTimeFormat`, `GetNumberFormat` and
`GetCurrencyFormat` formatted the English way whatever locale a program
asked for, and the user's locale was always `en-US`.

- **Formatting in the locale asked for** (`userland/kernel32/nlsformat.c`):
  `GetDateFormat`, `GetTimeFormat`, `GetNumberFormat` and
  `GetCurrencyFormat`, A, W and Ex (the A number functions are new), take
  their pictures, names, separators, grouping and orders from
  `GetLocaleInfo`, which answers every locale other than English from ICU
  (PR #45).  `de-DE` gives `02.10.2026`, `Freitag, 2. Oktober 2026`,
  `14:05:09`, `1.234.567,89` and `1.234.567,89 €`; `ja-JP` gives
  `2026/10/02`, `2026年10月2日`, `9:05:09`, `1,234,567.89` and `¥1,234,568`,
  as Windows does.  The picture rules are Windows': `d`…`dddd`, `M`…`MMMM`
  (the genitive month when the picture has a day number), `y`, `yy`,
  `yyyy`, `g`, `h`/`H`, `m`, `s`, `t`/`tt` and `'quoted'` text;
  `DATE_LONGDATE`, `DATE_YEARMONTH`, `DATE_MONTHDAY`, `TIME_NOSECONDS`,
  `TIME_NOMINUTESORSECONDS`, `TIME_NOTIMEMARKER` and
  `TIME_FORCE24HOURFORMAT`; `NUMBERFMT` and `CURRENCYFMT` with all five
  negative-number and sixteen negative-currency orders, Indian-style
  grouping, rounding half away from zero, and Windows' errors (a value
  that is not a number, flags beside a format, 30 February, a short
  buffer).
- **Closer to Windows' locale data**: Japanese and Chinese long dates have
  no weekday (`yyyy年M月d日`, where ICU's full date ends in one), and the
  yen sign is Windows' narrow `¥` rather than ICU's full-width one.
- **The user locale** is `LocaleName` under
  `HKCU\Control Panel\International`, read once per process by
  `GetUserDefaultLocaleName`, `GetUserDefaultLCID`, `GetUserDefaultLangID`,
  `GetThreadLocale`, `LOCALE_USER_DEFAULT` and a `NULL` locale name.  The
  system locale and the UI language stay `en-US`.  The registry is saved
  to drive C:, so the choice lasts across restarts.
- **`intl.exe`** (System32) shows the user's format (`intl`), lists the
  locales (`intl /list`) and sets one (`intl de-DE`; a neutral name such
  as `ja` becomes `ja-JP`), writing `LocaleName`, `Locale` and the classic
  values beside them (`sShortDate`, `sDecimal`, `iCurrency`…) for programs
  that read the registry themselves.
- **Settings > Time & language** shows the regional format and offers
  fifteen common ones; a click runs `intl.exe`.
- **Tests**: `nlstest` (core suite, 64- and 32-bit) checks the four
  functions in German, Japanese and English against Windows' output;
  `nlstest user` sets `de-DE`, `ja` and `en-US` with `intl.exe` and checks
  that new processes follow; `nlstest set ja-JP` before the suite's
  restart and `nlstest after-restart ja-JP` after it check the choice
  lasts.
- Not yet: user overrides (a changed `sShortDate` alone is not read back),
  `GetDurationFormat`, alternative calendars (`DATE_USE_ALT_CALENDAR`,
  the Japanese era calendar), and native digits in the output.

## Calendars, durations and the user's overrides

The locale work left three things open: a changed `sShortDate` (or any
other regional setting) was not read back, `GetDurationFormat` did not
exist, and every date was Gregorian.

- **The user's overrides** (`userland/kernel32/locale.c`): the classic
  values under `HKCU\Control Panel\International` (`sShortDate`,
  `sDecimal`, `sTimeFormat`, `iCalendarType` and the rest of Windows' list)
  are what `GetLocaleInfo`, `GetLocaleInfoEx` and the formatting functions
  answer for the user's locale, unless the caller passes
  `LOCALE_NOUSEROVERRIDE`.  Other locales keep their own values.
  `SetLocaleInfoA`/`W` (new) change one, and set what Windows derives from
  it: a short date sets `sDate` and `iDate`, a time format `sTime`,
  `iTime`, `iTLZero` and `iTimePrefix`, and a new `sDate` or `sTime` is put
  into the format.  A process reads the values once; new processes see
  the change.  `intl NAME` resets them to the chosen locale's own.
- **`GetDurationFormat` and `GetDurationFormatEx`**
  (`userland/kernel32/nlsformat.c`): a duration in 100 ns ticks, or a
  `SYSTEMTIME`'s hours to milliseconds, in a picture of `d`, `h`/`H`, `m`,
  `s` and up to nine `f`; the largest unit in the picture takes what does
  not fit the next one (`h:mm` of a day and a half is `36:00`).  Without a
  picture, the locale's `LOCALE_SDURATION`.
- **Calendars** (`userland/kernel32/calendar.c`): Gregorian and its US
  English, Middle East French and Arabic variants, the Japanese era
  calendar, Taiwan, the Korean Tangun era, Hijri, Thai Buddhist, Hebrew,
  Persian and Um Al Qura (`CAL_GREGORIAN` ... `CAL_UMALQURA`).  A locale's
  calendars are ICU's list of those commonly used where it is, mapped to
  `CAL_*` ids as .NET does, so `th-TH` writes Buddhist years (`2/10/2569`),
  `fa-IR` Persian dates (`1405/07/10`), `ja-JP` has the Japanese calendar
  besides Gregorian and `ar-SA` Um Al Qura and Hijri.  ICU converts the
  dates and names the months and eras; Hebrew days and years are written
  in Hebrew numerals (`כ"א תשרי תשפ"ז`).  `LOCALE_ICALENDARTYPE` and
  `LOCALE_IOPTIONALCALENDAR` give a locale's first and second calendar;
  `GetDateFormat` writes in the first (or the user's `iCalendarType`), and
  with `DATE_USE_ALT_CALENDAR` in the second in its own format
  (`令和8年10月2日`).  New: `GetCalendarInfo` (A, W, Ex: names, eras and
  their first years, patterns, `CAL_ITWODIGITYEARMAX`, `CAL_RETURN_NUMBER`),
  `SetCalendarInfo` (`CAL_ITWODIGITYEARMAX`, as on Windows),
  `EnumCalendarInfo` (A, W, ExA, ExW, ExEx; `ENUM_ALL_CALENDARS`, the
  Japanese eras newest first), `EnumDateFormats` (A, W, ExA, ExW, ExEx) and
  `EnumTimeFormats` (A, W, Ex), which give the user's format and then the
  locale's own.
- **Where ICU and Windows differ**: the default calendars follow CLDR, so
  `ar-SA` is Gregorian first (Windows still starts it on Um Al Qura); the
  Hijri calendar is ICU's tabular `islamic-tbla`, which is Windows'
  Kuwaiti algorithm without its registry day adjustment; the
  `CAL_GREGORIAN_XLIT_*` calendars are not there.
- **Tests**: `nlstest calendars` (every calendar above, the eras, names,
  errors, the two enumerations and `GetDurationFormat`) and `nlstest
  override` (`SetLocaleInfo` here and in a new process, `SetCalendarInfo`,
  then back to `en-US`'s own), both 64- and 32-bit in the core suite.

## Long command lines in the Terminal and CreateProcess

A 163-character command typed into the Terminal ran as its first 158
characters, with nothing to say so.  Command lines now reach programs
whole, up to Windows' own limits.

- **Terminal.**  The line being typed holds up to 8,191 characters (the
  limit of Windows' `cmd.exe`; it used to be 158) and wraps onto as many
  rows as it needs at the window's width, the prompt's colour, the cursor
  and mouse selection following it across rows.  The same goes for a line
  typed to a program reading a line at a time (after its own prompt, or in
  a full-screen program's window), and for lines recalled with Up/Down,
  pasted, or passed by `TerminalRun`.  The command history keeps the whole
  lines, and the built-in `echo` prints any number of words as typed
  (quotes included, as `cmd.exe` does).  `start PROGRAM ARGS...` and lines
  handed to `cmd.exe /c` have no length limit of their own.
- **Console input.**  A line sent to a program in line mode is a key record
  per character; the console's queue held 1,024, so a longer line typed at
  `cmd.exe`'s prompt lost its end and its Enter.  It holds 16,384 (two
  lines of 8,191 characters).
- **CreateProcess.**  `NtNovaCreateProcess` read at most 8,191 bytes of
  the command line; it now takes 32,766 UTF-16 characters, the longest
  Windows takes (32,767 with the closing NUL), copied a page at a time,
  and refuses a longer one with `STATUS_NAME_TOO_LONG`, which
  `CreateProcess` reports as `ERROR_FILENAME_EXCED_RANGE` (206) as on
  Windows.  A command line too long for the process parameters' pages
  goes in a region of its own.  The parameters' strings (command line,
  image path, current folder) are converted from UTF-8 properly, so
  characters outside ASCII (accents, CJK, emoji as surrogate pairs) reach
  `GetCommandLineW` intact; they used to arrive one UTF-16 unit per byte.
- **Test harness.**  `tools/novarun.py` (which the self-tests and the app
  corpus use) typed a key every 30 ms holding each for 60 ms.  QEMU
  replays a key's hold as a delay in its input queue, so the keys piled up
  there and, some 600 characters in, it began dropping them, key releases
  and Enter included; a dropped release left a key held down, repeating.
  Keys are now held 30 ms and sent 50 ms apart, with a one-second pause
  every 40 keys so that a slow emulated desktop keeps up with long lines.
- **Tests.**  Core self-test `cmdlinetest`: a 1,200-character command typed
  into the Terminal and an 1,100-character one typed at `cmd.exe`'s prompt
  reach the program whole (length, `argv` with quoted paths and escaped
  quotes, last argument); `cmdlinetest spawn` passes 1,000, 8,191 and
  32,766 characters through `CreateProcessW`, 8,191 through
  `CreateProcessA` and 8,000 through `cmd.exe /c`, and checks that 32,767
  are refused (64- and 32-bit).

## Media keys, side buttons and the horizontal wheel

Keyboards' media, volume, browser and launch keys, mice's back and forward
buttons and tilting (horizontal) wheels now work, on USB and on PS/2.

- **Media keys** (`usbhid.c`): USB keyboards send them as Consumer Control
  usages (page 0x0C), usually in a report or on an interface of their own,
  either as an array of usages or as one bit per key; the Power, Sleep and
  Wake keys come as System Control (Generic Desktop 0x81-0x83).  Both are
  found in the report descriptor, a device that has only them is taken
  too ("media keys" in the log), and each key becomes the E0-prefixed
  scancode a PS/2 keyboard sends for it (Microsoft's keyboard scan code
  specification), so everything above the drivers sees one kind of key.
  Keys held are tracked per report, and held volume keys repeat.  The
  keyboard page's own Mute, Volume Up and Volume Down usages (which QEMU's
  USB keyboard sends) map to the same codes.
- **Mouse buttons 4 and 5 and AC Pan**: report-protocol mice report up to
  five buttons, and the horizontal wheel (Consumer "AC Pan", + to the
  right) goes in a new `InputEvent.dw`.  PS/2 mice are switched to
  IntelliMouse Explorer mode (sample rates 200, 200, 80 after the wheel
  mouse's 200, 100, 80), which adds buttons 4 and 5 and a horizontal wheel
  in the fourth byte.
- **Programs** (`um_gui.c`, `user32`): side buttons arrive as
  `WM_XBUTTONDOWN`/`UP`/`DBLCLK` (or the `WM_NCXBUTTON*` forms) with
  `XBUTTON1`/`XBUTTON2` in the high word, `MK_XBUTTON1`/`2` and
  `VK_XBUTTON1`/`2` for `GetKeyState`; the tilt wheel as `WM_MOUSEHWHEEL`.
  `DefWindowProc` turns a side button's release into `WM_APPCOMMAND`
  (`APPCOMMAND_BROWSER_BACKWARD`/`FORWARD`, `FAPPCOMMAND_MOUSE`) and a
  browser, volume, media or launch key (`VK_BROWSER_BACK` to
  `VK_LAUNCH_APP2`) into the matching `APPCOMMAND_*` with
  `FAPPCOMMAND_KEY`; unhandled, it goes up to the parent window, as on
  Windows.
- **The shell**: Volume Up and Down change the playback volume by 2% a
  press, Mute toggles it (the focused program still gets the key), Sleep
  sleeps (S3) and Power shuts down like the power button.
- **Tests**: `inputtest` (core self-tests) plugs a USB mouse in for the
  test and presses its buttons 4 and 5, then unplugs it and tilts the PS/2
  mouse's wheel both ways and presses its button 4, then presses the
  volume keys on the USB keyboard, and checks the messages a full-screen
  window gets.  QEMU has no USB device with Consumer Control or a
  horizontal wheel, so the Terminal's `usbcheck` runs those report
  descriptors (a consumer array, consumer bits, system control, a
  five-button mouse with AC Pan, a keyboard sending the volume usages)
  through the same parser and report handling and compares the events
  they make.

## Sound threads above busy programs: MMCSS and the windowing boost

While Audacity recorded under QEMU without KVM, the thread handing back
`waveIn` buffers and PortAudio's own thread, both TIME_CRITICAL (15), were
held up 100 to 270 ms by Audacity's redraws.  Its window threads, woken by
mouse input, ran at 15 too: NovaOS gave a window's thread NT's +6 for
keyboard and mouse input and the foreground boost (+2) on top, 8 + 6 + 2
capped at 15, level with the sound threads, which then waited out time
slices behind them.  The `waveIn` change before this one kept those
delays from losing audio; this one stops the delays.

- **Input to a window is a +2 boost** (`kernel/um/um_gui.c`), win32k's
  windowing boost, as for any window message.  NT's +6 for keyboard and
  mouse is the I/O increment a driver gives the thread reading the
  device, which on NovaOS is the device poll thread, already at 19.  A
  foreground program's NORMAL window thread now wakes at 12 (8 + 2 + 2),
  below `THREAD_PRIORITY_HIGHEST` of an ABOVE_NORMAL class and below every
  TIME_CRITICAL thread.  The foreground boost still stacks on the wake
  boost, as NT's `KiDeferredReadyThread` adds PsPrioritySeparation to the
  base and increment (ReactOS's copy of it was checked); console input
  keeps +6.
- **A real Multimedia Class Scheduler** (`userland/avrt/avrt.c`,
  `sched_set_mmcss` in `kernel/ke/scheduler.c`).  `avrt.dll` used to set
  `THREAD_PRIORITY_HIGHEST`.  Now `AvSetMmThreadCharacteristics` with an
  audio task ("Pro Audio", "Audio", "Capture", "Playback", "Low Latency")
  runs the thread at real-time priority 18, with no privilege needed, as
  MMCSS's service lifts it on Windows: above any program thread however
  boosted and above the desktop (17), below device polling and the sound
  mixer (19).  Other tasks ("Games" and the rest) and audio tasks at
  `AVRT_PRIORITY_LOW` run at 16; a task Windows does not have fails with
  `ERROR_INVALID_TASK_NAME`; `AvRevertMmThreadCharacteristics` gives back
  the thread's own priority; the ANSI calls now read the task name (they
  passed an empty one).  As on Windows (SystemResponsiveness 20) a
  registered thread may use 80% of a CPU: found running at 8 of the 10
  ticks of a 100 ms period, it drops to its own base until the period
  ends, so a program spinning in one cannot freeze the desktop.
- **NovaOS's own sound threads register**: winmm's `waveOut` thread
  ("Playback"), `waveIn` thread ("Capture") and MIDI synthesizer,
  DirectSound's mixer and capture threads, and WASAPI's event thread
  ("Audio"), as Windows' audio engine runs under MMCSS.

`mmcsstest` (core self-test, x64 and x86): with a busy TIME_CRITICAL
thread on every CPU, a registered thread woken every 5 ms runs within
0.1-0.2 ms (95th percentile), a plain TIME_CRITICAL one after 24-34 ms
(median); "Pro Audio" threads spinning on every CPU for a second keep a
NORMAL thread waiting at most about 90 ms.

## Monitors on one card, plugged in and out

- **A virtio GPU's outputs are monitors.**  A new driver,
  `kernel/drivers/virtio_gpu.c`, drives QEMU's `virtio-vga` and
  `virtio-gpu-pci` in 2D: each output with a monitor on it (up to 16 on
  one card, `max_outputs=N`) shows a picture in memory that the GDI draws
  on like on video memory, and the card copies what changed to the monitor
  (`TRANSFER_TO_HOST_2D` and `RESOURCE_FLUSH`, after each frame and each
  pointer move).  Each output gets the usual list of modes plus the size
  its monitor asks for, which it starts in.  So one card is now enough
  for several monitors, where before each needed its own adapter
  (`[DISPLAY] Head 1: QEMU virtio-vga output 2, 1024x768, 18 mode(s)`).
- **The boot display moves over.**  A `virtio-vga` shows its VGA
  framebuffer on its first output only until a picture is set on any
  output, so when the boot display is a `virtio-vga` with more than one
  output, the primary monitor moves onto a picture in memory too, keeping
  what the screen shows (`[DISPLAY] QEMU virtio-vga has 3 outputs: the
  primary monitor is its output 1`); resolutions still change at run time
  and after S3 every output gets its picture back.  With one output it
  stays on the VBE driver, as before.
- **Monitors come and go.**  When a monitor is connected to or
  disconnected from an output the card says so, and the desktop lays
  itself out again: a new monitor goes to the right of the others, the
  windows (and the pointer) on one that went move to the nearest one left,
  maximized ones fill their new monitor's work area, and programs get
  `WM_DISPLAYCHANGE`; `EnumDisplayMonitors`, `GetMonitorInfo`,
  `SM_CMONITORS` and the virtual screen follow (`[SHELL] Monitors: 3 (were
  2)`).  In QEMU an output gets a monitor from a display window, or from a
  VNC client on that output (`-vnc ...,display=gpu,head=N`) asking for a
  desktop size; 0 x 0 takes it away.
- **The bootloader skips a GOP it can't draw on.**  OVMF's GOP for a
  `virtio-gpu-pci` has no framebuffer (Blt only); when that is the first
  one the firmware lists, the bootloader takes the next adapter's.
- `montest hotplug` (devices self-tests, a new "monitors" boot: one
  `virtio-vga` with three outputs and a monitor on the first) has the test
  plug monitors into the second and third outputs, put a window on the
  third, then unplug both; it checks the monitors, `WM_DISPLAYCHANGE`, and
  that the window and the pointer end up on a monitor that is left.  The
  screenshot is one PNG per output.  `tools/novarun.py` takes screenshots
  of every output of a `virtio-vga`/`virtio-gpu-pci` given an id and
  `max_outputs`.
- Not yet: per-monitor DPI that programs see (`GetDpiForMonitor`,
  per-monitor-aware DPI contexts and `WM_DPICHANGED` when a window crosses
  to a monitor with another scale): programs still get 96 DPI logical
  pixels on every monitor.  Hot-plugging a whole display adapter (PCI
  hot-plug) isn't handled either; monitors come and go on a virtio GPU's
  outputs.

## Multi-touch

Touch screens with more than one finger now work, and programs get them
the way Windows hands them out: `WM_TOUCH` or `WM_POINTER*`.

- **USB digitizers** (`usbhid.c`): a report descriptor with Digitizer
  page finger collections (Tip Switch, Contact Identifier, X and Y), and
  usually a Contact Count, is a multi-touch screen ("multi-touch screen"
  in the log).  Reports are read in hybrid mode too, where a frame's
  contacts come spread over several reports and the first carries the
  count.  Contacts that stop being reported are lifted, and unplugging the
  screen lifts them all.  `usbcheck` runs canned descriptors through it.
- **virtio multi-touch** (`virtio_input.c`, new): the virtio input device
  (`virtio-multitouch-pci` in QEMU) speaks the Linux evdev multi-touch
  protocol B (slots, tracking ids, `ABS_MT_POSITION_X`/`Y`); its axis
  ranges come from the device's config space and it is set up again after
  S3.
- **The desktop** (`desktop.c`): a frame of contacts goes to the program
  window under the first contact; anywhere else (the shell, the taskbar,
  built-in apps) the primary contact acts as an absolute mouse.
- **Programs** (`user32` `pointer.c`): a window that called
  `RegisterTouchWindow` gets `WM_TOUCH` with `GetTouchInputInfo`
  (hundredths of a pixel, `TOUCHEVENTF_DOWN`/`MOVE`/`UP`/`PRIMARY`);
  others get `WM_POINTERDOWN`/`UPDATE`/`UP` with `GetPointerInfo`,
  `GetPointerType` (`PT_TOUCH`), `GetPointerTouchInfo` and the frame
  functions, and `DefWindowProc` promotes the primary contact to
  `WM_LBUTTONDOWN`/`MOUSEMOVE`/`UP`, as on Windows.
  `GetSystemMetrics(SM_DIGITIZER)` and `SM_MAXIMUMTOUCHES` report the
  screen.
- **Tests**: a new `devices` self-test suite boots with a virtio
  multi-touch screen; `touchtest` puts two fingers down on a
  `RegisterTouchWindow` window, moves and lifts them, then taps a window
  that uses pointers, and checks what both get.
- Not yet: gestures (`WM_GESTURE`), pens, the Input Mode feature report
  some USB screens need before they leave mouse mode, and
  `GetMessageExtraInfo`'s touch signature on promoted mouse messages.

## More than one monitor

- **Each further display adapter is another monitor.**  Besides the boot
  display, the display driver drives every other Bochs/QEMU DISPI adapter
  it finds (QEMU's `-device secondary-vga` or `bochs-display`; up to four
  monitors), each with its own list of modes, set at run time through its
  BAR2 registers (`[DISPLAY] Head 1: QEMU secondary-vga ...`).  After S3
  each one gets its mode back.
- **One desktop across them.**  The GDI's back buffer covers the virtual
  desktop: the primary monitor at (0, 0), the others beside, above or
  below it (negative coordinates included).  Each monitor has its own
  scale: the desktop is drawn at the largest, and a 1280x800 monitor next
  to a 2560x1600 one shows each 2x2 block averaged, so both show the same
  logical size.  The other monitors show the wallpaper; the dock, Start
  menu and desktop icons stay on the primary.
- **Windows and the pointer.**  The pointer moves across the edges where
  monitors touch and stops at the outer ones, and is drawn on the screen
  it is on, at that screen's scale.  A dragged window goes with the
  pointer onto the other monitor; maximizing and snapping (by dragging to
  an edge, or Win+Left/Right) fill that monitor's work area, which is all
  of it on monitors without the dock.  After a change of mode or layout
  each window stays on its monitor.
- **The Win32 calls report the real layout.**  `EnumDisplayMonitors`,
  `GetMonitorInfo` (`\\.\DISPLAY1`, `\\.\DISPLAY2`, ..., the work area,
  `MONITORINFOF_PRIMARY`), `MonitorFromWindow`/`Point`/`Rect` (with the
  `MONITOR_DEFAULTTO*` fallbacks), `EnumDisplayDevices` (an adapter per
  monitor, and the monitor on it), `EnumDisplaySettings` and
  `ChangeDisplaySettingsEx` for a named display (its modes, and its place
  in `dmPosition` / `DM_POSITION`), and `GetSystemMetrics`'
  `SM_CMONITORS` and `SM_*VIRTUALSCREEN`.
- **Settings > Display arranges them.**  With more than one monitor the
  page shows them to scale: drag one to move it (it snaps against the
  others' edges and lines up with them), click one to choose it, and the
  resolution buttons below apply to the chosen one.  The layout and each
  display's mode are kept where Windows keeps them,
  `HKLM\SYSTEM\CurrentControlSet\Control\Video\{NovaOS-Display}\000N`
  (`DefaultSettings.*`, and `Attach.RelativeX/Y` for the place), and come
  back at the next boot (`[DISPLAY] Display 2 goes at (0, 800)`).
- `montest` (graphics self-tests, which now boot with a QEMU secondary-vga
  as the second monitor) checks all of that, moves the pointer across, and
  leaves a window on the second monitor for the screenshot; the test saves
  one PNG per monitor (`montest.png`, `montest-2.png`).
  `tools/novarun.py --monitors 2` boots with two monitors the same way.
- Not yet: more than one output of one adapter (QXL or virtio-gpu heads),
  a monitor plugged in or out while running, and per-monitor DPI that
  programs see: they all get 96 DPI logical pixels, as before.

## NetSurf: inline SVG

Phase 19.8 is finished: NetSurf draws an `<svg>` element written inline
in a page's HTML, not only SVG files.

- **Inline `<svg>`** is drawn like an image (`box_inline_svg` in
  `content/handlers/html/box_special.c`): its subtree is written out as an
  SVG document (with the SVG and XLink namespaces the HTML parser keeps on
  the nodes, and SVG's mixed-case names such as `viewBox` and
  `linearGradient`, which libdom stores in lower case) and fetched as a `data:image/svg+xml` URL, so the same
  libsvgtiny parser and plutovg path plotter that draw SVG files draw it.
  Its shapes never become boxes of their own.  The element's `width` and
  `height` attributes size it like an `<img>`'s (`css/hints.c`), CSS
  overrides them, and the `viewBox` scales the drawing into the box.  A
  script changing the SVG's DOM lays the page out again like any other
  change; an unchanged SVG keeps its image across the rebuild.
- **Test**: `nstest`'s page now also carries an inline SVG (a square and a
  circle, its `viewBox` scaled 4x) and a list a script builds with
  `createElement('li')` in a loop; the test checks both shapes' colours
  and sizes and the three list items, before and after the click that
  lays the page out again.

## NetSurf: laying a page out again from the box a script changed

After a script changes a page, NetSurf laid out the whole page again,
even though only one part of it had been built again (#123): the line
layout of every paragraph, then the passes that place positioned boxes,
apply relative offsets and measure what each box's descendants cover.
Now the layout starts at the box whose children were built again.

- **The flow starts at the changed box.**  Each block formatting
  context's loop (`layout_block_context` in
  `content/handlers/html/layout.c`) records, for every box it visits, its
  state when it reaches the box and when it leaves it: the position, the
  margins still collapsing and whether the context has floats.  A layout
  after a change starts the loop at the changed box in the state it
  reached that box in last time (and, for a change inside an
  `overflow` box, does the same in each context on the way down), lays
  the box out again, and carries on with the boxes after it.  A later box
  the loop reaches in the same state as last time, apart from its
  position, would come out the same, only moved: it is moved as a whole
  and the loop continues from the state it left the box in, so only the
  changed box, the boxes above it and the positions of the boxes after
  it are worked out again.  Boxes before the change are not visited.
- **The passes after the flow** (list markers, positioned boxes,
  relative offsets, the overflow extents and iframe positions) visit only
  the boxes laid out again and the boxes above them.  When the change
  moved or resized anything and the page has absolutely positioned or
  fixed boxes, every positioned box is placed again from the static
  position the flow last gave it, since its containing block or its place
  may have changed.
- **What still lays out the whole page:** a change in a box with floats
  in its formatting context (floats placed before the change decide
  where later lines go), a change below a float, an inline-block, a table
  cell, a flex item or a positioned box, a change in a box whose top
  margin collapses into its first child's (a box with no border, padding
  or text before its first child box), a box above the change
  getting a height it did not have, positioned boxes on a page whose
  relative offsets moved floats or an inline's contents, a new window
  size, and a new stylesheet (which rebuilds every box anyway).  Floats
  inside a box that has a formatting context of its own (`overflow`
  other than `visible`) do not count: that box is moved as a whole.
- **Numbers** (TCG, the long page of `nstest`: 1,500 styled paragraphs,
  the fifth changed twelve times between one and three lines): the layout
  after a change takes 6.6 ms instead of 38.8 ms.  Of the full layout's
  38.8 ms, the flow (line layout and positions) took 29.0 ms and the
  passes after it 8.8 ms; from the changed box, the flow takes 1.5 ms and
  the passes 2.7 ms (they and the width measurement still visit each
  child of the boxes above the change, here the body's 1,500
  paragraphs).  With #123's rebuild of the changed part, a change on that
  page now costs about 7 ms of layout where it cost 730 ms before #123.
- **Checks**: `NETSURF_LAYOUT_LOG=file` makes NetSurf append a line per
  script-driven layout with its times (rebuild, widths, flow, placing)
  and the boxes laid out and moved; with `NETSURF_LAYOUT_CHECK=1` every
  third layout from a changed box is followed by a full layout and every
  box's position, size and overflow extent is compared;
  `NETSURF_LAYOUT=full` always lays out the whole page.
- **Test**: after its first page, `nstest` opens four more: the long
  page, a page with iframes below the changed paragraph, one with fixed,
  absolutely and relatively positioned boxes and one with floats in boxes
  of their own.  Each is shown twice while a script changes it twelve
  times, once laid out from the changed box and once with the check; the
  test requires every check to find no difference, the screenshots of
  the two runs (`nstest-PAGE-incremental.png`, `nstest-PAGE-check.png`)
  to be identical above the status bar, and the long page's layout from
  the changed box to take less than half the full layout's time.

## NetSurf: subtree relayout, iframes on changing pages, unsized SVGs

The three NetSurf leftovers from Phase 19.8.

- **Only the changed part of a page is built again.**  A script's change
  used to throw the whole box tree away and build it again.  Now the
  change is recorded at the node whose children changed (libdom's
  `DOMSubtreeModified`; for a changed attribute, `DOMAttrModified`, the
  element's parent, since sibling selectors can restyle its later
  siblings), a burst of changes is merged into the lowest node above all
  of them, and `html_relayout` (`content/handlers/html/html.c`) rebuilds
  only the boxes below the nearest block, inline-block or table-cell box
  above it (`dom_to_box_subtree` in `box_construct.c`, which runs the
  same element and text construction as the first build, `:before`,
  `:after` and normalisation included).  Only those nodes' styles are
  selected again, and only that box and the boxes above it have their
  widths measured again; the rest of the page keeps its boxes, styles and
  cached widths.  A new stylesheet or a change reaching the root element
  still rebuilds everything, and a change inside `<head>` rebuilds
  nothing.  The rebuilt boxes live in a talloc context of their own,
  freed when that part of the page is built again, so a page a script
  keeps changing does not grow.  Nodes a script removes forget their
  boxes at once (`DOMNodeRemoved`), so a node put back elsewhere never
  points at a freed box.
  On a page of 1,500 styled paragraphs where a timer changes one line's
  text, each change took about 600 ms to rebuild plus 130 ms to lay out
  (TCG, 14 changes); now the rebuild is under the 10 ms timer's
  resolution and the layout about 80 ms (15 changes), so a change costs
  about 80 ms instead of 730 ms.  The positioning pass of layout still
  walks the whole page.
- **Pages with iframes are laid out again too.**  These were skipped
  before, because each iframe's browser window hangs off its box.  Now
  the windows are unlinked from the boxes about to go and linked to the
  new boxes of the same elements afterwards
  (`browser_window_unlink_iframes` and `browser_window_relink_iframes` in
  `desktop/frames.c`), so an iframe keeps its page, scroll position and
  state across a rebuild; only when the page's iframes changed (one added
  or removed, or a new `src`) are they all opened again.  Framesets and
  iframes themselves already showed (NetSurf's own frame model: each
  frame is a browser window of its own); frameset pages have no boxes and
  keep the static layout.
- **An SVG without a width and height** drew at no size (a 0 x 0
  viewport when the `<img>` gave none).  libsvgtiny now gives it the
  default size of a replaced element, 300 x 150 CSS pixels, as other
  browsers do, with a missing side following the `viewBox`'s aspect
  ratio (`initialise_parse_state` in `libsvgtiny/src/svgtiny.c`).
- **Parser debug output**: libhubbub printed the tree builder's mode for
  every token to the console (its debug hook is on without `NDEBUG`);
  it is off on NovaOS.
- **Test**: `nstest`'s page also carries an SVG image without a size
  (checked at 300 x 150) and an iframe showing a frameset page with two
  coloured frames, all checked before and after the click that changes
  the page.

## NetSurf: SVG and pages scripts change

Phase 19.8.  NetSurf shows SVG images, and a page a script changes after
it was laid out is laid out and drawn again.

- **SVG** through NetSurf's own libsvgtiny 0.1.8 (MIT,
  `third_party/netsurf/libsvgtiny`), which parses with libdom's XML binding
  on expat 2.7.1 (MIT, `third_party/netsurf/libexpat`): `<img>`,
  `<object>`, CSS backgrounds and `.svg` files opened directly.  Upstream's
  framebuffer frontend had no path plotter ("path unimplemented"), so the
  shapes never showed; NovaOS's (`frontends/framebuffer/framebuffer.c`)
  fills and strokes each path with plutovg (MIT, already in the tree for
  GDI+), anti-aliased, in a scratch surface the size of the shape within
  the clip rectangle, and blends it onto the window's bitmap.
- **Pages a script changes**: NetSurf 3.11 builds the box tree once ("NS
  layout is static"), so text, elements, `style` or `class` attributes or
  stylesheets a script changed after layout never showed.  Now a change to
  the document (libdom's `DOMSubtreeModified`, outside form fields,
  scripts and the title) or a stylesheet arriving late schedules a rebuild
  (`html_relayout` in `content/handlers/html/html.c`): a burst of changes
  coalesces, the box tree is thrown away and built again from the DOM in
  one go (`dom_to_box_sync`), with a fresh CSS selection context and
  libcss's per-node caches dropped, then the page is reformatted at its
  width and redrawn.  Form fields keep their values (they live in the DOM);
  images are fetched again from the cache; the text selection, a drag and
  the caret are dropped.  Pages with frames or iframes keep the static
  layout.
- **SVG images parsed again** on each reformat (a window resize) went
  into the old diagram, so every shape was drawn once more each time;
  now each parse starts from an empty diagram.
- **Alt+F4** closes NetSurf: the window's `WM_CLOSE` (which `SC_CLOSE`
  sends rather than posts) is a quit event now, not only a posted one
  (`userland/netsurf/nsfb_novaos.c`).
- **Test**: `nstest` in the graphics self-tests
  (`tests/selftest/graphics/060-nstest.py`) opens a page with an SVG and a
  box whose `onclick` script changes it; the test checks the screen before
  and after clicking it.
- Not yet: inline `<svg>` elements in HTML (NetSurf draws only SVG files),
  and SVG text, which libsvgtiny places but NetSurf draws in a fixed size.

## OpenGL with no driver installed

Windows always has an OpenGL: without a display driver's, `opengl32.dll`
gives its own OpenGL 1.1 ("GDI Generic"), so a program can create a
context, read the version and fall back.  NovaOS's `opengl32.dll` loaded
Mesa 3D (the App Store's llvmpipe, or virgl on the host GPU) or nothing,
and with nothing every function returned 0, which Krita's Qt took for a
context and crashed on.  It now falls back to a small OpenGL 1.1 of its
own on GDI: a double-buffered RGBA pixel format, contexts made current per
thread, `glGetString` ("NovaOS", "GDI Generic", "1.1.0"), `glViewport`,
`glClearColor`, `glClear`, `glReadPixels` and `SwapBuffers` through
`SetDIBitsToDevice`.  New self-test `glgeneric` (64- and 32-bit, before
Mesa is installed).  Krita gets past start-up on it but still needs
OpenGL 2 for its Qt Quick widgets, so it still needs Mesa 3D.

## A parallel userland build

`tools/build_userland.py` compiled and linked the 80 DLLs and 56
programs one after another (only a few third-party libraries inside a DLL's
`build.py` used several cores).  It now schedules every step as a task that
starts when the tasks it needs are done, on a pool of threads:

- **`--jobs N` / `-j N`.**  At most `N` compiler, linker and resource
  compiler processes run at once; the default is the CPU count and
  `--jobs 1` builds one step at a time.  `--check` and the other options
  behave as before, and CMake and CI keep calling the script without the
  flag, so they use every core.
- **Order that matters is kept.**  A DLL links after the DLLs in its
  `deps`; a program links after the DLLs it imports and after its own
  object.  Nothing else waits on anything, so the x64 and the x86 pass
  each keep all cores busy.
- **Same output.**  The files, their paths and their order in
  `userland_files.c` do not depend on `N`: what each task adds to the
  image is collected per task (`b.built` and `b.placed` in a DLL's
  `build.py` still work) and put in link order afterwards.
- **Readable failures.**  A failed command prints its command line and
  output in one piece, the build stops starting new work, and the script
  exits with status 1.
- **`userland/msvcp140/build.py`** builds `msvcprt_static.lib` under a
  lock, since several DLLs and programs link it.

Cold build of the userland without NetSurf on a 4-core machine: 6 min 16 s
before, 3 min 39 s after (CPU time unchanged, 9 min 41 s).

## The userland build runs its 64-bit, 32-bit and NetSurf parts side by side

The parallel userland build left three parts of `tools/build_userland.py`
one after another: the x64 pass, the x86 pass and NetSurf's build. They now
share the one `--jobs` budget as a single set of dependency-ordered tasks.
NetSurf's roughly 800 objects compile from the start (they need only
headers); `netsurf.exe` links once the x64 start-up objects and the import
libraries of `msvcrt`, `kernel32`, `ntdll`, `ws2_32`, `user32` and `gdi32`
exist. Of the tasks that are ready, the passes' go before NetSurf's objects,
so the 64-bit DLL chain is never queued behind them.

- **The architecture is per task.** Build hooks read it as `b.ARCH`; that is
  now a property of the running task's thread, so the two passes can
  not see each other's value. The files built, their order in the image and
  the `--check` result are unchanged.
- **`msiscript_js.h`** (written by both passes) is replaced atomically.
- **Failures** name their pass (`[x64]`, `[x86]`, `[netsurf]`); NetSurf's
  compile errors are listed in file order.
- `tools/build_netsurf.py` split `build()` into `plan()`, `compile_one()` and
  `link()`; run on its own it behaves as before.

## Pen and mouse pointers: title bars, activation, the pen signature, touch screens listed

What [Pen WM_POINTER messages](#pen-wm_pointer-messages) left open:

- **The pen signature.**  The mouse messages `DefWindowProc` makes of a
  pen's pointer messages (and any other mouse message a pen causes) carry
  Windows' pen marker: `GetMessageExtraInfo` masked with `0xFFFFFF00` is
  `0xFF515700`; a touch's carry `0xFF515780` (bit 7: touch).  Programs that
  ignore pen-made mouse messages because they read the pen themselves
  (Qt, Chromium, drawing programs) can now tell them apart.  user32's queue
  keeps each message's extra information, so `GetMessageExtraInfo` answers
  for the message taken last; `SetMessageExtraInfo` sets it.
- **Non-client pen messages.**  Over the parts of a window user32 hit-tests
  outside the client area (a program's own title bar that answers
  `WM_NCHITTEST` with `HTCAPTION`, as Chromium, Firefox and Qt's frameless
  windows do; scroll bars; the menu bar), a pen gives `WM_NCPOINTERUPDATE`,
  `WM_NCPOINTERDOWN` and `WM_NCPOINTERUP` with the hit-test code in
  `wParam`'s high word; `DefWindowProc` makes them `WM_NCLBUTTONDOWN` / `UP`
  and the moves, so scroll bars and menus work by pen as before.  Enter and
  leave cover the whole window.  The desktop's own title bar and frame stay
  the desktop's: a pen moves and sizes windows there as the mouse does, and
  programs get no messages for it (as for the mouse).
- **`WM_POINTERACTIVATE`.**  A pen tip (or, with `EnableMouseInPointer`, a
  mouse button) going down on a window that is not active sends it
  `WM_POINTERACTIVATE` (pointer id, hit-test code, the top-level window)
  before `WM_POINTERDOWN`; `DefWindowProc` asks the parent of a child window
  and otherwise answers `PA_ACTIVATE`.  The desktop has already activated
  the window by then, so `PA_NOACTIVATE` gives the activation back to the
  window of the program's that had it.
- **The mouse entering and leaving.**  With `EnableMouseInPointer(TRUE)` the
  mouse now gets `WM_POINTERENTER` when it comes over a window and
  `WM_POINTERLEAVE` when it moves to another or off the program's windows,
  and its non-client messages become `WM_NCPOINTER*` like the pen's.
- **Touch screens in `GetPointerDevices`.**  A multi-touch screen is listed
  as `POINTER_DEVICE_TYPE_TOUCH` (its contact count, cursor ids from 2)
  beside the pen; `GetPointerDevice` and `GetPointerDeviceRects` answer for
  it, and touch pointer messages name it as their `sourceDevice`.
- **A PS/2 mouse fix.**  The PS/2 driver left the rest of its input event
  uninitialised, so after a pen had been used its motion could arrive
  tagged as the pen's (and become pen pointer messages).
- **Test**: `pentest` (core suite, 64- and 32-bit) checks the pen
  signature, `WM_POINTERACTIVATE` with `PA_ACTIVATE` and `PA_NOACTIVATE`,
  the non-client messages over a window's own title bar and their
  promotion, the touch-screen listing, and the mouse entering and leaving
  two windows as the test harness moves it (`Nova.move_to`);
  `touchtest` (devices suite) checks the touch signature and the
  touch screen in `GetPointerDevices`.

Not yet: `WM_POINTERACTIVATE` comes after the desktop has activated the
window, so `PA_NOACTIVATE` briefly activates it; the desktop's own title bar
sends programs nothing for a pen (or the mouse).

## Pen WM_POINTER messages

What [Pen tilt and rotation, and virtio pens](#pen-tilt-and-rotation-and-virtio-pens)
left open: a pen's pressure, tilt and rotation reached programs only
through Wintab, and to every other program a pen was a mouse.  Programs
written for Windows 8 and later (Qt 5.12+, Chromium, Firefox, WinUI) read
pens through the `WM_POINTER` messages instead.

- **The desktop tags a pen's pointer motion.**  A pen (USB, virtio or
  synthetic) now posts its packet first and then the pointer motion it
  causes, marked as the pen's (`InputEvent.from_pen`); the mouse messages
  that motion makes go to the program with the packet's number in `MSG`'s
  padding after `message` (`UmSetInputPen`; a 32-bit program's
  `ntdll` puts it after the `MSG`).  When the pen goes out of range the
  window under it gets one more, tagged, move.
- **user32 makes `WM_POINTER*` of them** (`userland/user32/pointer.c`), in
  a client area, as Windows 8 does: `WM_POINTERENTER` when the pen comes
  over a window, `WM_POINTERUPDATE` while it hovers, moves or presses the
  barrel button, `WM_POINTERDOWN` / `WM_POINTERUP` for the tip,
  `WM_POINTERLEAVE` when it moves to another window or out of range, and
  `WM_POINTERCAPTURECHANGED` when the input leaves the window it is
  touching.  The pen is pointer 12, primary; while it touches it stays
  with the window it touched down on.
- **`GetPointerType`** says `PT_PEN`; **`GetPointerPenInfo`** (and
  `GetPointerInfo`, `GetPointerFramePenInfo`, the `...History` forms) give
  the message's pressure (0-1024), `tiltX` / `tiltY` and `rotation` in
  degrees with `penMask` saying which the pen reports, `PEN_FLAG_BARREL`,
  `PEN_FLAG_INVERTED` and `PEN_FLAG_ERASER`, the button flags and
  `ButtonChangeType`, and HIMETRIC positions that
  **`GetPointerDeviceRects`** maps onto the screen (Qt computes its
  sub-pixel pen position that way).  **`GetPointerDevices`** /
  `GetPointerDevice` list the pen as an external one while a pen is
  present.  Each message's data is kept in a ring; what a thread took
  last with `GetMessage` / `PeekMessage` is what these answer with.
- **`DefWindowProc`** gives a pointer message it gets back as the mouse
  message it was made of (`WM_LBUTTONDOWN` for the tip, `WM_RBUTTONDOWN`
  for the barrel button, the moves), so programs that know only the mouse
  or Wintab work as before, and a program that handles `WM_POINTER*`
  itself gets no mouse messages for the pen.
- **`EnableMouseInPointer(TRUE)`** makes the mouse's own client-area
  messages `WM_POINTERDOWN` / `UPDATE` / `UP` for pointer 1 (`PT_MOUSE`,
  the buttons as the first, second and third button), promoted back the
  same way; `IsMouseInPointerEnabled` says so.
- **Test**: `pentest` (core suite) drives a synthetic pen over two
  windows and checks the message sequence, the pen data and the
  promotion; `wintabtest` still sees the pen's clicks (now through the
  promotion); `usbcheck`'s pen checks expect the packet before the motion.

Not yet: `GetMessageExtraInfo` does not carry the pen signature
(`0xFF515700`) on promoted mouse messages; non-client pen messages
(`WM_NCPOINTER*`) and `WM_POINTERACTIVATE` are not sent, a pen over a
title bar is a mouse; mouse-in-pointer mode has no `WM_POINTERENTER` /
`LEAVE` and is not covered by a self-test (no way to drive the mouse from
inside the guest).

## Pen tilt and rotation, and virtio pens

What [GTK pointers and Wintab pen tablets](#gtk-pointers-and-wintab-pen-tablets)
left open: pens reported only where they were and how hard they pressed,
and pens and tablets on virtio input were not used.

- **Tilt and barrel rotation** travel with every pen packet, from the
  device through the desktop (`kernel/wm/tablet.c`) to `wintab32.dll`, in
  tenths of a degree: X tilt (+ the pen's top leaning right), Y tilt (+
  toward the user) and the barrel's twist, clockwise.  A pen says what it
  can report when it comes; a value it can't report stays 0.
- **USB pens**: the HID parser reads X Tilt, Y Tilt and Twist (digitizer
  usages 0x3D, 0x3E, 0x41) through their physical range and unit
  exponent (it now keeps both), so Microsoft's -9000..9000 at exponent -2
  and a plain -127..127 over -90..90 degrees both come out right.
- **Synthetic pens**: `InjectSyntheticPointerInput` passes `tiltX`,
  `tiltY` and `rotation` when `penMask` has `PEN_MASK_TILT_X`/`_Y` or
  `PEN_MASK_ROTATION` (`NtNovaGuiCtl` op 30 arg 4 takes them; arg 5 says
  what the pens present can report).
- **`wintab32.dll`**: with a pen that reports tilt, `DVC_ORIENTATION` has
  azimuth (0-3599, clockwise from the tablet's top) and altitude
  (-900..900) axes, and with barrel rotation a twist axis (0-3599); each
  packet's `ORIENTATION` carries them, the tilt turned into azimuth and
  altitude the way Qt turns them back (the eraser end has a negative
  altitude, as Wacom's driver gives).  `PK_CHANGED` includes
  `PK_ORIENTATION`.  A pen without tilt stays upright (altitude 900).
- **Virtio input pens and tablets** (`kernel/drivers/virtio_input.c`): a
  device with absolute X/Y and `BTN_TOOL_PEN` or `ABS_PRESSURE` (a Linux
  pen passed through with `virtio-input-host-pci`) is a pen: tip, barrel
  buttons, eraser (`BTN_TOOL_RUBBER`), pressure, tilt (`ABS_TILT_X/Y`, by
  their resolution in units a radian) and an Art Pen's rotation (`ABS_Z`),
  moving the pointer and clicking with the tip as a USB pen does.  Any
  other device with absolute X/Y, such as QEMU's `virtio-tablet-pci`, is
  an absolute pointer with its buttons and wheels (it was ignored before).
- **Tests**: `usbcheck` runs a pen descriptor with tilt and twist through
  the HID parser and canned evdev events through the virtio pen and tablet
  decoding; `wintabtest` injects tilted and turned pen reports and checks
  the azimuth, altitude and twist each packet carries and the orientation
  axes; the devices suite's touch boot has a `virtio-tablet-pci` too and
  checks it comes up as an absolute pointer.

Not yet: pen `WM_POINTER` messages (`GetPointerPenInfo`), so tilt reaches
programs through Wintab only.  QEMU has no virtio or USB pen of its own,
so the pen decoding is tested on canned events.

## Per-monitor DPI

- **A monitor can show DPI-aware programs its real density.**  The desktop
  works in logical pixels (96 DPI) and draws them at each monitor's scale,
  so a 2560x1600 screen shows a 1280x800 desktop twice as sharp.  Until
  now every program saw 96 DPI and drew at that size.  Now a monitor at
  scale 2 can be set to 192 DPI for DPI-aware programs: in Settings >
  Display ("DPI for DPI-aware apps"), kept in the registry as `LogPixels`
  under `...\Video\{NovaOS-Display}\000N`, or with `NtNovaGuiCtl`
  `CTL_SET_DPI` (op 31).  96, the default, changes nothing for anyone.
- **Programs get the awareness they ask for.**  user32 reads the
  manifest's `dpiAwareness` (`PerMonitorV2`, `PerMonitor`, `System`,
  `Unaware`, first known value wins) and `dpiAware` (`true`, `true/pm`,
  `per monitor`), the compatibility layer (`__COMPAT_LAYER` with
  `DpiUnaware`, `GdiDpiScaling` or `HighDpiAware`, as Windows' "override
  high DPI scaling" setting writes it), and `SetProcessDpiAwarenessContext`,
  `SetProcessDPIAware` and shcore's `SetProcessDpiAwareness` (which fail
  with access denied once the manifest or an earlier call decided, or a
  window exists).  Programs without either are unaware, as on Windows.
  `GetThreadDpiAwarenessContext`/`SetThreadDpiAwarenessContext`,
  `GetWindowDpiAwarenessContext`, `GetAwarenessFromDpiAwarenessContext`,
  `AreDpiAwarenessContextsEqual` and `GetDpiFromDpiAwarenessContext`
  report it.  `shcore.dll`, which programs that link `shcore.lib` import
  by name, now loads (as shlwapi, where its functions are).
- **What each kind sees.**  Unaware programs keep 96 DPI and logical
  pixels everywhere and are scaled up as before.  System-aware ones see
  the primary monitor's DPI as it was when they started, everywhere.
  Per-monitor-aware ones see each monitor's own: `GetDpiForMonitor`
  (shcore), `GetDpiForWindow`, window and client rectangles, mouse
  positions, `GetCursorPos`, monitor rectangles and work areas,
  `SM_CXSCREEN` and the virtual screen are in the screen's own pixels.
  `GetDpiForSystem`, `LOGPIXELSX/Y`, `AdjustWindowRectEx` (and
  `...ForDpi`), `GetSystemMetricsForDpi`, `SystemParametersInfoForDpi`
  and dialog fonts follow the DPI too.  The desktop gives an aware
  window's bitmap two pixels per logical pixel and shows them one to one
  on the screen, so its text and lines are as sharp as the desktop's own.
- **`WM_DPICHANGED`.**  When a per-monitor-aware window goes to a monitor
  of another DPI (dragged there, or moved with `SetWindowPos`), or its
  monitor's DPI changes, it gets `WM_DPICHANGED` with the new DPI and the
  suggested rectangle (the same place and size on the screen); a program
  that leaves it to `DefWindowProc` is put there anyway.  Per-monitor v2
  programs also get `WM_GETDPISCALEDSIZE` first and
  `WM_DPICHANGED_BEFOREPARENT`/`AFTERPARENT` on their child windows.
- `dpitest` (core self-tests) is per-monitor aware v2 by its manifest: it
  sets the boot's monitor to 192 DPI and back and checks
  `WM_DPICHANGED`, the suggested rectangle, its window's and the monitor's
  rectangles, and that an unaware and a system-aware child (started with
  `__COMPAT_LAYER`) see 96 and 192 DPI.  With a second monitor it moves
  its window there and back.
- Not yet: user32's own controls, menus and scroll bars drawn at 192 DPI
  for an aware program (they keep their 96 DPI sizes in its pixels), the
  stock fonts at the system DPI, threads of one process in different
  awareness contexts (coordinates follow the process), and DPI scaling of
  another process's window coordinates (`GetWindowRect` on a window of a
  program with another awareness).

## errno per thread in the C runtime

`msvcrt.dll` and `ucrtbase.dll` kept one `errno` for the whole process,
so a multi-threaded program that checked `errno` after a failing call on
one thread could read a value another thread had just set.  Windows keeps
it, and the rest of the C runtime's per-thread data, in a block per
thread; NovaOS now does the same (`userland/msvcrt/ptd.c`).

- **What moved into the block.**  `errno` (`_errno`, `_get_errno`,
  `_set_errno`), `_doserrno` (`__doserrno`, `_get_doserrno`,
  `_set_doserrno`, now also set to the Win32 error behind each `errno`
  the runtime maps, as Windows' `_dosmaperr` does), `_fpecode`, `rand`'s
  seed, `strtok`'s and `_wcstok`'s position, and the buffers `gmtime`,
  `localtime`, `asctime`, `ctime`, `_wcserror`, `tmpnam(NULL)` and
  `_wtmpnam(NULL)` return.  A new thread starts with `errno` and
  `_doserrno` 0 and `rand` seeded with 1, as on Windows.
- **How.**  The block lives in a TLS slot (`TlsAlloc`), made on a thread's
  first use and freed by the DLL's new `DllMain` on `DLL_THREAD_DETACH`
  (and on `FreeLibrary`).  Looking it up keeps `GetLastError`
  unchanged, so code that reads `errno` and then `GetLastError` sees the
  failing call's error.  `msvcrt.dll` and `ucrtbase.dll` each keep their
  own block, as they are separate runtimes on Windows too.
  `tools/build_userland.py`'s `link_dll` takes an entry point for DLLs
  linked under another name (`ucrtbase`).
- **Who depends on it.**  Firefox's DLLs (`xul.dll`, `nss3.dll`,
  `mozglue.dll`, `mozavcodec.dll`, `onnxruntime.dll`...) and Microsoft's
  `msvcp140.dll` import `_errno` from the UCRT and run many threads.
- **Test.**  Core self-test `errnotest` (64- and 32-bit).

## Hard links (Phase 17.5)

A file can have several names now, as `CreateHardLink` makes them on
Windows; `ln` in the MSYS2 shell and Git's object store use it.

- **Drive C:** (`kernel/fs/ramfs.c`): a file's names are a ring of nodes
  that share its contents, size, attributes, times and security
  descriptor, so a write, `SetFileAttributes` or `SetSecurityInfo` by one
  name is seen by the others, open handles included.  Deleting a name
  leaves the file to the others; the last name takes it.  Link counts
  (`FileStandardInformation`, `GetFileInformationByHandle`'s
  `nNumberOfLinks`) and the file id (`FileInternalInformation`,
  `nFileIndex`) are the file's, the same by every name, and the volume
  reports `FILE_SUPPORTS_HARD_LINKS`.
- **Kept across restarts** (`kernel/fs/persist.c`): on an NTFS C: a
  linked file is saved as one record with a `$FILE_NAME` in each
  directory (`NtfsLink`), and loading joins the names of a record with
  several again.  FAT has no links, so each name is saved as a copy and
  `\NOVA\LINKS.TXT` lists the names of each linked file; at boot they are
  joined into one file again.
- **NTFS volumes** (`kernel/fs/ntfs.c`): `NtfsLink` adds a `$FILE_NAME`
  and an `$I30` entry and bumps the link count; deleting and renaming
  take the name concerned (a DOS alias goes with its long name), so the
  other names of a file stay; the record and its clusters are freed with
  the last name.  Mounted volumes (D:, ...) get the same through
  `RamfsSource` link calls, and a file whose record has several names is
  joined to its other loaded names when it is read.
- **Syscalls and kernel32**: `NtSetInformationFile(FileLinkInformation)`
  (refuses directories and other drives, replaces a file under the name
  when asked to, checks `FILE_ADD_FILE` on the folder); `CreateHardLinkA/W`
  call it.
- **Checks**: `linktest` in the core suite, and `linktest restarted` after
  the suite's restart checks a linked pair is still one file.  On a
  disk made by `scripts/make-ntfs-disk.sh`, `linktest D:\LinkTest` then
  `scripts/check-ntfs-disk.sh` (ntfsfix, ntfssecaudit and
  `ntfs-check.py`, which checks link counts against names) pass.

## Phase 20: common dialogs and portable programs

Phase 20.1 and 20.2 of the roadmap: the common file dialogs, and three of
the App Store's "untested" portable programs (SumatraPDF, WinMerge, PuTTY)
opening a file or a connection and running in the nightly corpus.

- **Common file dialogs** (`userland/comdlg32/filedlg.c`, `ifiledlg.c`):
  `GetOpenFileName` and `GetSaveFileName` show a real Open / Save As
  dialog: the folder path with an Up button, the folder's contents in a
  list view (folders first, then the files the chosen filter matches, with
  the shell's icons, sizes, types and dates; the drives at the top), the
  file name and the file types.  Double-click or Enter opens a folder or
  picks a file, typing a path goes there, typing a wildcard filters the
  list; Open checks that the file exists, Save As asks before replacing
  one and adds the filter's or the default extension; multi-select and
  folder picking work.  The Vista-style COM dialogs (`CLSID_FileOpenDialog`,
  `CLSID_FileSaveDialog`: `IFileOpenDialog`, `IFileSaveDialog`,
  `IFileDialogEvents`, `IFileDialogCustomize`) sit on the same dialog and
  hand the choice back as shell items, so shell32 gained
  `IShellItem`/`IShellItemArray` (`SHCreateItemFromParsingName`,
  `SHCreateShellItemArray`...).  user32: a key released after a modal
  dialog opened reaches the dialog, and Alt opens the menu bar only when
  pressed alone.  `dlgtest` covers the objects' settings and has
  interactive modes for each dialog; Notepad++ and 7-Zip open and save
  through them.
- **GDI+** (`userland/gdiplus/`, new): the flat `Gdip*` API that the C++
  wrapper classes compile down to, drawn with
  [plutovg](https://github.com/sammycage/plutovg) (`third_party/plutovg`,
  MIT, with FreeType-licensed rasteriser and stroker files): graphics on a
  bitmap, a window or any DC (reading the affected part of the DC into a
  DIB and copying it back, so the DC's clipping holds), world transforms,
  clipping, solid, hatch, texture and gradient brushes, pens with dashes
  and caps, lines, Béziers, arcs, pies, polygons, paths kept as GDI+ keeps
  them (points and types, so `GetPathData`, markers and iterators see what
  programs expect), regions, matrices; bitmaps loaded from PNG, JPEG, BMP
  and GIF files or streams and saved as PNG, JPEG or BMP, converted to and
  from HBITMAPs and HICONs, `LockBits` in every common pixel format;
  fonts, families and string formats mapped onto the faces NovaOS ships
  the way gdi32 maps them, with `DrawString`/`MeasureString` laid out in
  GDI+'s way (wrapping, hotkey prefixes, tabs, alignment, trimming, the
  generic default's em/6 padding), `MeasureCharacterRanges` and driver
  strings.
- **New DLLs**: `winspool.drv` (the spooler's client calls on a machine
  without printers: an empty printer list, no default printer, so print
  menus load and printing reports no printer), `oleacc.dll` (Active
  Accessibility with no client to serve: `LresultFromObject` answers 0),
  and DDE in user32 (`DdeInitialize`, string handles, `DdeConnect` finds
  no server, the `WM_DDE_*` lParam packing), which SumatraPDF uses to look
  for a running copy before opening its own window.
- **MDI** (`userland/user32/mdi.c`): the `MDIClient` class with
  `WM_MDICREATE`, `DESTROY`, `ACTIVATE`, `NEXT`, `GETACTIVE`, `SETMENU`,
  `REFRESHMENU`, `MAXIMIZE`, `RESTORE`, `TILE` and `CASCADE`,
  `DefFrameProc`, `DefMDIChildProc`, `CreateMDIWindow` and
  `TranslateMDISysAccel` (Ctrl+F4, Ctrl+F6, Ctrl+Tab).  Children fill the
  client window, as a maximized child does.  WinMerge is an MFC MDI
  application and needs all of it.
- **user32 and gdi32 for MFC**: `WH_CBT` hooks called for window creation
  and destruction (MFC subclasses every window it creates from
  `HCBT_CREATEWND`; without the hook its frame window had no `CWnd` and was
  deleted); class names up to 255 characters (MFC's generated names such
  as `Afx:0000000140000000:b:...` were cut and not found);
  `GetClassInfo` copies its fields one by one (a `memcpy` from the wrong
  offset lost the window procedure on x64); `WM_SIZE` and `WM_MOVE` reach
  an overlapped window when it is first shown, as on Windows, not during
  `CreateWindow` (PuTTY's handler dereferences the terminal it creates
  after the window); `ToUnicode` with `KF_UP` in the scan code types
  nothing (PuTTY translates key-ups too and typed every character twice);
  framed windows' client areas are clamped to their bitmap, so a program's
  own caption (SumatraPDF) sits under the desktop's title bar instead of
  starting a repaint loop; `WS_CLIPCHILDREN` windows keep their children's
  pixels across a parent repaint.  gdi32: 24-bit `CreateDIBSection` gives
  the program 24-bit rows of its own (in its section when it passed one)
  synced with the 32-bit pixels gdi32 draws on, and `LoadImage` with
  `LR_CREATEDIBSECTION` keeps a 24-bit resource as such a section, in the
  resource's row order (WinMerge reads its toolbar strips back through
  `GetObject` and converts them itself; it got 32-bit zeros and drew black
  squares); mapping modes, world transforms (identity), palettes,
  `TranslateCharsetInfo`, `GetCharABCWidthsFloat`.  comctl32: `TBBUTTON`'s
  reserved bytes are 2 on x86 (the 64-bit layout broke every 32-bit
  toolbar), separators take their width from `TBBUTTON.iBitmap`,
  `TB_GETMETRICS`/`TB_SETMETRICS`.
- **Bootloader**: the identity map covers the first 64 GiB (1 GiB pages
  above 4 GiB), not only 4 GiB: with more than 4 GiB of RAM the firmware
  loads the bootloader near the top of memory and it page-faulted on its
  own code the moment it switched to its page tables (the nightly corpus
  boots with 4 GiB).
- **Loader**: a relative DLL path (`Merge7z\Merge7z.dll`) is searched from
  the program's folder, then the current one, and gets `.dll` when it has
  no extension; `ole32` logs a `CoCreateInstance` of an unregistered class.
- **Winsock** (`userland/ws2_32/wsa.c`): `WSAAsyncSelect`, the window
  message form of `WSAEventSelect` (one message per event, `FD_CONNECT`,
  `FD_ACCEPT`, `FD_READ`, `FD_WRITE`, `FD_CLOSE`; an event is reported
  once and re-enabled by the call that consumes it: `recv`, a `send` that
  would block, `accept`), which PuTTY drives all its networking with;
  `recv` with `MSG_PEEK` and `ioctlsocket(FIONREAD)` through a kernel
  peek (`NetSockPeek`) instead of consuming a byte; `FD_CONNECT` and
  `FD_WRITE` both reported on a socket that asked for both.
- **Programs**: SumatraPDF 3.4.6 (32-bit) opens a PDF and renders it with
  its toolbar, tabs and menus; WinMerge 2.16.50 compares two files side by
  side with the differences highlighted; PuTTY 0.81 makes a raw connection
  to a host, shows what the server sends and sends what is typed.  All
  three are in the nightly corpus (`tools/appcorpus.py`), which now runs
  the windowed programs one at a time (each takes the keyboard, Alt+F4
  closes it) and compares each screenshot with `tests/reference/NAME.png`:
  SumatraPDF on a PDF the script generates, WinMerge on two text files,
  PuTTY on a connection to an echo server the script runs on the host
  (10.0.2.2 on QEMU's user network; the typed line must reach it).
  SumatraPDF's official 32-bit build is taken from the npm package
  `pdf-to-printer` and PuTTY is built from its source release with MinGW,
  because neither project's download site is reachable from every
  network; the build is kept in the corpus cache.
- Not yet: SumatraPDF draws its own caption under the desktop's title bar
  (two title bars); printing (`winspool` has no printers); PuTTY's SSH is
  untested (no SSH server in the test network); WinMerge's folder compare
  and plugins (`icu.dll`, `mlang.dll`) are untried; `IFileDialogCustomize`
  adds no controls.

## The `powertest` self-test no longer races the keyboard's return after the lid wake

- **Before:** `powertest` failed now and then in CI (65 s, "the zone reads
  70 C" and "the zone reads 45 C") and held up pull requests that touched no
  power code; a rerun or a merge of main made it pass.
- **Cause:** a bug in the self-test's helper, not in the power code.  After
  `close_lid` wakes the machine it waits for NovaOS to find the USB keyboard
  again, and it looked for the `[USB] port N: keyboard` line *after* the
  `[SLEEP] Woke up` line.  The xHCI port is re-enumerated while NovaOS
  resumes, so the kernel logs the keyboard either side of `Woke up`
  (under KVM it came first).  When it came first the helper waited its full
  60 s, `powertest` had already asked for the 70 C reading and gone on to
  wait only 30 s for it, and by the time the helper wrote the temperature
  both waits had run out.  The thermal zone and the sleep and wake path
  were fine: the log of every failed run shows the lid closing, sleeping,
  waking and opening correctly.
- **Now:** `close_lid` is done once the log since the lid closed has
  `Woke up` and the keyboard's return after its removal, in either order.
  The checks `powertest` makes are unchanged.

## Priority boosts on wake-up

A thread woken by an event, a semaphore, a condition variable or a
message, with a busy thread of the same priority on its processor, used
to wait for that thread's 20 ms time slice to end: about 19 ms at the
95th percentile.  Windows raises a woken thread's priority above its base
for a while so it runs at once, which is what makes a program's window
answer and an audio thread refill its buffer promptly while something
else computes.  NovaOS now does the same.

- **Two priorities per thread** (`kernel/ke/scheduler.c`): the base
  priority and the current one.  A wake-up raises the current priority to
  the base plus the waker's increment, NT's values: +1 for an event, a
  semaphore, a mutex, an alert (what SRW locks, condition variables and
  critical sections wake with) or a timed wait's deadline, +1 for finished
  file I/O, +2 for a named pipe, the network or a window message, +6 for
  keyboard and mouse input and console input.  Never above 15, never for
  a real-time thread (16 and up), and a boost never lowers a priority that
  is already higher.  A window message boosts only the thread it is for;
  the others the message queue wakes get nothing.
- **The boost wears off** one level for every quantum (two 10 ms ticks)
  the thread runs while boosted, its waits in between included, back to
  its base.  A woken thread that turns busy takes turns with the others
  again within a few quanta.
- **Run queues are ordered by priority**, threads of one priority taking
  turns as before.  A boosted thread preempts a busy thread of its base
  priority on its processor.  A thread whose slice ends goes on running
  while every queued foreground thread has a lower priority (csrss, below
  them, still gets its turn); a yield gives way to any thread.
- **The balance set**: once a second, a thread that has been ready for 3 s
  without running is raised to 15 for one quantum, then drops back to its
  base, so higher-priority threads can't starve it for good.
- **Kernel threads**: the device poll and audio mixer threads moved from
  12 to 16, above any boost, so a boosted program can't hold up input or
  sound.  The desktop moved from 8 to 9, so a +1 boost doesn't queue
  programs ahead of it, while a timed wait's wake (also +1) still preempts
  it.
- **`NtQueryInformationThread`** reports a thread's current and base
  priority instead of a fixed 8.
- **Measured** in QEMU (TCG) on 2 CPUs with a busy thread on each, with
  the new `boosttest` (64- and 32-bit): a woken thread ran after 19.0 ms
  (95th percentile) before, for each of event, semaphore, condition
  variable and thread message, and after 0.03 to 0.10 ms now.
  `sleeptest timer` still passes at 0.32 ms (0.37 ms before) and its
  "Event set" line fell from 19.0 ms to 0.07 ms.  `smpstress` on 4 CPUs
  (6 runs each): the 8-thread critical section took 940 to 1380 ms before
  and 800 to 1070 ms after, so waking a lock's waiter with a boost causes
  no convoy; event ping-pong 40 to 100 ms before, 80 to 150 ms after.
- **Not done**: `SetThreadPriority` and `SetPriorityClass` are still
  accepted and ignored, so every program thread has base 8; NT's extra
  foreground-process boost and quantum stretching are left out.

## Qt programs: KeePassXC

Phase 20.3 starts with KeePassXC 2.7.12 (the portable Qt 5 zip from its
GitHub releases), unmodified.  With `msvcp140.dll` in place it already
opened its window, but every widget's text was blank, and opening a
database ended the program.  It now opens and unlocks a password
database and shows its groups, entries and the selected entry's details.
What was missing:

- **`GetGlyphOutline`** was a stub returning `GDI_ERROR`.  Qt's GDI font
  engine measures each glyph with it (`GGO_METRICS | GGO_GLYPH_INDEX`) and
  draws text from the coverage bitmaps it returns, so without it Qt drew
  nothing.  It now gives the metrics of gdi32's cached glyphs, their
  coverage as `GGO_BITMAP` and `GGO_GRAY2/4/8_BITMAP` (rows padded to
  four bytes), and their outlines as `GGO_NATIVE` (one `TTPOLYGONHEADER`
  per contour, quadratic `TT_PRIM_QSPLINE` segments in 16.16 pixels,
  straight from stb_truetype's shapes), which `wglUseFontOutlines` can use
  too.  The transform argument is taken as the identity.
- **`SetSecurityInfo` on `GetCurrentProcess()`**: KeePassXC replaces its
  own process's DACL so other programs cannot read its memory, and printed
  "Unable to disable core dumps" because `NtSetSecurityObject` did not
  know the pseudo-handles.  The current process and thread pseudo-handles
  are valid there now (their descriptors are not kept yet, as for other
  process handles).
- **HSTRINGs in Windows' layout.**  C++/WinRT does not call
  `WindowsCreateString`: it builds HSTRINGs itself, in the layout Windows
  uses internally (flags, length, two padding words, the character
  pointer; heap strings carry their reference count after that, from the
  process heap), and passes them to combase.  NovaOS's HSTRING kept the
  pointer at offset 8, so `RoGetActivationFactory` read a null class name
  and crashed.  `userland/ole32/winrt.c` now uses that same layout.
- **`Windows.Security.Credentials.KeyCredentialManager`** (Windows Hello)
  activates (`userland/ole32/winrt_classes.c`, the first runtime class
  NovaOS answers for).  KeePassXC asks `IsSupportedAsync` from a PPL task
  when it starts; a failed activation there is an exception no one
  observes, which ends the program.  The operation completes at once with
  `false`, so quick unlock is simply not offered.  `RoOriginateLanguageException`
  exists (C++/WinRT reports errors through it before throwing).
- **Tests**: `qttest` (14 checks: glyph metrics by character and by glyph
  index, gray bitmaps, outlines, C++/WinRT-style reference and heap
  HSTRINGs, Windows Hello activation and its completed operation, the
  process DACL), and KeePassXC in the nightly corpus
  (`tests/appcorpus/870-keepassxc.py`): it starts on a small KDBX 4
  database the corpus writes beside it (master password `novaos`), the
  password is typed into its unlock screen, and the unlocked database's
  screenshot must match `tests/reference/keepassxc.png`.

Next in Phase 20.3: Krita.

## Qt programs: Krita

The last program of Phase 20.3: Krita 5.3.4, the portable zip from
download.kde.org, unmodified.  Unlike KeePassXC's Qt (built with Visual
Studio), Krita and its Qt 5, KDE Frameworks and about 90 other DLLs are
built with LLVM's MinGW toolchain: C++ through `libc++.dll`, exceptions
through `libunwind.dll` on Windows' structured exception handling.  It
did not get past loading its first DLLs; it now starts, and "New Image"
on its welcome page opens an empty A4 image on its OpenGL canvas (Mesa
3D's llvmpipe, from the App Store) with the toolbox, colour selector,
layers and brush presets.  What was missing:

- **The UCRT's `_l` functions.**  libc++ builds every `std::locale` facet
  on the locale-taking C functions, and the loader stopped Krita at the
  first one (`_mbtowc_l`).  `ucrtbase.dll` has them now for its one "C"
  locale: `_isctype` and `_isctype_l`, `_tolower_l`/`_toupper_l`,
  `_towlower_l`/`_towupper_l`, the `_isw*_l` classes, `_strxfrm_l`,
  `_wcscoll_l`, `_wcsxfrm_l`, `_mbtowc_l`, `_strtod_l`, `_strtol_l`,
  `_strtoi64_l`, `_strtoui64_l`, `wcrtomb_s` and `_swab` (LibRaw).
- **`RtlUnwindEx` as Windows does it.**  The CONTEXT a caller passes is
  only storage: Windows starts the unwind where the caller is.  NovaOS
  started from whatever the CONTEXT held, which worked for Visual Studio
  programs (they pass the exception's own CONTEXT) but not for libunwind
  or GCC's libgcc: at the frame that catches, their handler starts a
  second, *collided* unwind to the landing pad with a CONTEXT it never
  filled in, and `_Unwind_Resume` (the end of every cleanup) does the
  same from no handler at all.  The first exception any Krita library
  threw and caught (OpenColorIO's, while reading its configuration)
  landed with the trap flag set from that garbage and ended the program.
  ntdll now keeps, per thread, what a handler is running inside: a
  dispatch (an unwind then starts at the exception's frame, as before),
  an unwind's frame handler (a collided unwind starts again at that
  frame, whose handler then sees the new record) or neither (it starts
  at the caller of `RtlUnwindEx`).
- **Tests**: `unwindtest` (libunwind's throw, catch and collided unwind
  with a CONTEXT full of garbage, a cleanup frame in between, and
  `_Unwind_Resume`'s unwind from no handler) in the core self-tests, and
  Krita in the nightly corpus (`tests/appcorpus/885-krita.py`): the App
  Store installs Mesa 3D, Krita starts, "New Image" and Create open an
  image, and the window must match `tests/reference/krita.png`.  A
  corpus program can now name App Store runtimes it needs
  (`App(runtimes=[...])`), staged for the Store to install without a
  network.  The App Store lists Krita 5.3.4.

Not yet: Krita needs an `opengl32.dll`, and NovaOS has none until Mesa 3D
is installed (Windows always has its own OpenGL 1.1, which would send
Krita to its software canvas; without any, Krita ends); the Python
scripter plugin fails to import `asyncio` (`WSAEOPNOTSUPP` from a
socket call); Qt Quick's JIT cannot register its unwind tables; Ctrl+N
right after start does not reach Krita's window (a click does); and
painting strokes on the canvas is not tested yet.

## Space a power cut left marked as used comes back at boot

A power cut in the middle of saving drive C: never loses a file (the old
copy or the new one comes back), but it could leave the clusters the save
had already filled marked as used in the FAT with no file pointing at
them, and nothing gave them back: a machine that lost power during saves
slowly filled up.

- **The FAT says when to look.**  `kernel/fs/fat.c` now keeps the
  volume's clean-shutdown bit in FAT[1], as Windows does: it is cleared
  on the disk (and flushed) before the first FAT or directory sector of a
  change is written, and set again once a save has flushed everything.
  A volume mounted with it clear was cut off mid-save; one mounted with
  it set is not scanned, so an ordinary boot costs nothing.
- **Reclaimed at mount.**  When drive C:'s volume was not closed
  cleanly, `FatReclaim` reads the whole FAT once, walks every directory
  from the root marking each file's and directory's chain, and frees
  every cluster marked as used that nothing reached (bad clusters and
  reserved values stay).  A chain that runs into a cluster already reached
  (two files sharing clusters, or a loop) stops there and is left alone,
  so a reachable cluster is never freed; a read error or a lack of memory
  frees nothing.  The boot log says what happened:
  `[PERSIST] Drive C: was not closed cleanly: reclaimed N cluster(s) ...`.
  Other FAT volumes (USB sticks, the boot partition) are only mounted,
  never repaired, and keep their bit as they found it.
- **Tested** by the core self-test `power cut`: it stops QEMU, adds three
  chains no file reaches (66 clusters) to the data disk with the bit
  clear, as a cut-off save leaves them, and resets the machine; NovaOS
  must free exactly those and end up with the free-cluster count
  `fsck.fat` finds on a copy of the same disk.

## A reference machine for real hardware (Phase 21.1)

Phase 21 takes NovaOS from QEMU to one real PC.  The first step picks
that PC and lists what NovaOS runs on it.

- **The machine.**  The Lenovo ThinkPad T14 Gen 4 (Intel) with
  integrated graphics: wired Intel Ethernet (I219), one NVMe SSD, xHCI
  USB, HD Audio with a Realtek ALC3287 codec, a lid, a battery, a
  TrackPoint and a touchpad, sold in large numbers and fully specified
  by Lenovo.  [docs/hardware.md](hardware.md) lists each of its devices
  with the Linux driver for it and NovaOS's status: supported (NVMe,
  xHCI, PS/2 keyboard and TrackPoint, ACPI, battery, lid, timers, UEFI
  boot), partial (the display is the firmware's GOP framebuffer in one
  mode; the touchpad works only as a PS/2 mouse) or missing (the I219
  Ethernet controller, HD Audio on a controller with an audio DSP, which
  reports class 04.01 instead of 04.03, a serial console, Wi-Fi,
  Thunderbolt, camera, fingerprint reader, TPM).  The rest of Phase 21
  fills in the missing rows the gate needs.
- **`devices`.**  A new Terminal command (also `lspci`) lists every PCI
  function NovaOS found at boot with its ID, vendor, class and the
  driver that took it, functions without one in red, and a count.  Each
  PCI driver now records what it claimed (`PciClaim` in
  `kernel/hal/pci.c`): the boot display (whichever driver draws through
  it, or the GOP framebuffer), AHCI, NVMe, the four USB host
  controllers, HD Audio, `e1000`/`e1000e`, virtio-net, virtio-gpu and
  virtio-input.  The PCI table holds 128 functions (was 64; a laptop has
  30 to 60) and says so when a machine has more.
- **Test.**  The core suite's `devices` checks that QEMU's AHCI
  controller, VGA card, HD Audio card and xHCI controller show their
  drivers and the host bridge shows as a bridge.

## Release documentation (Phase 22.4)

The documentation NovaOS 0.1 ships with, written for people who did not
build it:

- **A user guide**, [docs/user-guide.md](user-guide.md): getting the ISO,
  trying it in QEMU, installing it, the desktop and its keyboard
  shortcuts, the built-in apps, installing programs from the App Store and
  elsewhere, where files are kept, network, sound, power, and what to do
  when something fails.  It says plainly that 0.1 has only been checked in
  QEMU.
- **A compatibility list**, [docs/compatibility.md](compatibility.md):
  every program known to run, the App Store's untested ones, a status
  (Works, Partly, Untested) and how each was checked (the nightly corpus,
  the CI graphics tests, the core self-tests, or by hand).  Each row is a
  file in `docs/compatibility/`, and `tools/docgen.py` builds the table, so
  a change that makes a program work edits that program's row.  Step 20.7
  will have CI keep the statuses.
- **Where NovaOS runs**, at the top of [docs/hardware.md](hardware.md):
  QEMU (tested on every pull request), other virtual machines (untested),
  real PCs (none checked yet) and Macs.
- **The build guide checked from a clean clone.**  Following
  `docs/building.md` from the top on Ubuntu 24.04 found two faults: its
  package list left out MinGW-w64, without which `msvcp140.dll` does not
  build, and the README's shorter list also left out `libc++-dev`; and
  the Ninja build it recommends stopped at once, because CMake did not
  know the bootloader sub-build makes `bootx64.efi` (CI builds with
  Makefiles, which do not mind).  Both lists now name every package,
  `CMakeLists.txt` declares the bootloader's output, and the same steps
  build `nova.img` in about three minutes on four cores and boot it to the
  desktop with `cmake --build . --target run`.  The macOS guide's Linux
  container command got the same package, and its UTM settings now allow
  an NVMe disk, which NovaOS drives since Phase 18.3.

## Releases from a version tag (Phase 22.5)

NovaOS releases are now made by pushing a version tag
([releasing.md](releasing.md)).  `.github/workflows/release.yml` checks
that the tag names the kernel's version (`v0.1.0` for 0.1.0) and that
release notes exist, runs the whole CI workflow on the tagged commit, and
only when every suite passes publishes the GitHub release with the ISO
that was tested.

- **The release build** fetches the Sound Open Firmware for the audio DSP
  first (`tools/fetch_sof_firmware.py --all`), so laptop microphones work
  in the shipped image; pull requests still build without it.
  `ci.yml` can be called from another workflow (`workflow_call`, input
  `release`) and then also keeps the kernel and boot loader it built.
- **The release's files**: `nova.iso` and `nova.iso.sha256`, the update
  channel (`kernel.elf`, `bootx64.efi`, `novaos-update.txt` from
  `tools/mkupdate.py`) that installed systems read from the newest
  release, and `SHA256SUMS`.
- **Release notes** from `tools/release_notes.py`: the hand-written
  `docs/releases/VERSION.md`, the files with their sizes and SHA-256, and
  every history section added since the previous release.
- **"Latest" belongs to releases.**  Once a release exists, CI's rolling
  `latest` build of `main` is a pre-release, so the README's download
  link and the update channel follow releases; versions with a suffix
  (`0.1.1-rc1`) are published as pre-releases.

## Roblox: the installer runs, the client stops in its anti-cheat

Dean asked for games, starting with Roblox.  Roblox's own installer
(`RobloxPlayerInstaller.exe` from roblox.com) now installs the client on
NovaOS; the client itself does not get past its Hyperion anti-cheat
([compatibility.md](../compatibility.md#roblox-and-anti-cheat)).
Roblox is in the App Store (Media), and the nightly corpus installs it
(`tests/appcorpus/090-roblox.py`; the installer downloads from Roblox's
servers, so the corpus needs the internet for it).

- **Firmware tables.**  `GetSystemFirmwareTable` and
  `EnumSystemFirmwareTables` (kernel32) return the SMBIOS tables (`'RSMB'`:
  the PC's maker, model and serial numbers, which installers and
  licensing code read) and the ACPI tables (`'ACPI'`), through
  `NtQuerySystemInformation(SystemFirmwareTableInformation)`.  The
  bootloader copies the SMBIOS tables it finds in the UEFI configuration
  table (SMBIOS 3, else 2) into its own pages as Windows' `RawSMBIOSData`
  (boot protocol 4); the kernel hands them out (`kernel/hal/firmware.c`,
  `NtNovaFirmwareTable`).  The installer stopped at start without them.
- **The registry below a missing key.**  Opening `A\B\C` when `B` does
  not exist now fails with `ERROR_FILE_NOT_FOUND`, as on Windows, not
  `ERROR_PATH_NOT_FOUND`; Roblox's installer ends itself on any other
  answer.
- **Zw names.**  ntdll exports every `Nt` function a second time under
  its `Zw` name, at the same address, as Windows' ntdll does
  (`userland/ntdll/build.py`).
- `K32GetProcessImageFileNameA` (and psapi's `GetProcessImageFileNameA`).
- `apitest` checks the firmware tables, the registry answer and the Zw
  exports.

How far the client gets: Hyperion's own `syscall` instructions all use
service 0 on NovaOS, so it cannot open files or query the system, its
call to end the process fails, and it then jumps to an unmapped address.
The WebView2 runtime the installer sets up (for the login page) does not
install either: Microsoft's updater stops on a delay-loaded function
NovaOS lacks.

## Saving drive C: without holding the locks

Drive C: lives in memory and is written to its disk once it has been quiet
for a second.  Until now the desktop thread did that write itself, holding
the desktop lock, the file-system lock and the big kernel lock for the
whole disk write: in QEMU a 32 MiB file kept every window, every file call
and most system calls waiting for 1.1 s, and a 64 MiB one for 300-600 ms
(the [timer wake-up](#timer-wake-ups-preempt-the-running-thread) work found it).

- **Snapshot, then write unlocked.**  `kernel/fs/persist.c` now takes the
  file-system lock only long enough to copy the changed part of the tree
  (names, attributes, times, security descriptors and the list of what
  each changed folder should still hold).  File contents are not copied:
  `RamfsLend` lends the file's buffer to the save, and a program that
  writes to a lent file gets its own copy first (copy on write, in
  `kernel/fs/ramfs.c`).  The disk write then runs on its own kernel
  thread, `persist`, holding only a save lock (after desktop and
  file-system in the lock order) and no kernel lock.  Anything that could
  not be saved is remembered and tried again on the next save.
- **Drivers lock per request.**  AHCI now keeps a busy flag per disk, as
  NVMe already did, and USB mass storage takes the kernel lock per 64 KiB
  chunk, so a save does not stall other disks or USB input.
- **A power cut leaves the old file or the new one.**  The FAT driver
  writes the new cluster chain and the FAT before the directory entry
  that points at it, and frees the old chain only after the entries are
  written and the disk has flushed.  In 38 trials that killed QEMU before,
  during and after a 48 MiB save, the file always came back whole, old or
  new.  A kill in the middle of the write can leave the clusters it had
  already filled marked as used but belonging to no file (`fsck.fat`
  reclaims them; 5 of the 38 trials); reclaiming them at mount is a
  separate step.
- **Measured** with the new `savetest` self-test in QEMU (TCG, 2
  processors): with a 32 MiB file the save held the file-system lock for
  0.2-0.8 ms (was 1127 ms), the longest wait for the desktop or
  file-system lock fell from 1127 ms to under 75 ms, and for the kernel
  lock from 10 ms to 2-4 ms.  The save itself still takes 360-380 ms, but
  nobody waits for it.

## Shared DLL pages, and the app corpus under KVM

The first App corpus runs on KVM (PR #98) failed three programs: Firefox
crashed about a minute after it started, at a different place each run,
and Notepad++'s and PuTTY's screenshots differed from their references.
None of it needed KVM.  The same failures came back under TCG once the
guest's memory was as full as in the whole corpus.

- **Firefox ran out of memory.**  Drive C: lives in RAM, so the corpus's
  programs (Inkscape, .NET, Python, ffmpeg, KeePassXC and the rest) take
  much of the 4 GB before Firefox starts, and every Firefox process got
  its own copy of every DLL it loaded: 164 MB of `xul.dll` alone for each
  of about ten processes.  Thread creation failed with
  `STATUS_NO_MEMORY`, then `xul.dll` would not load in a new content
  process.  With 1.5 GB of extra files staged, the crash reproduced under
  TCG every time.  KVM only made Firefox start its processes sooner.
- **Shared image pages** (`kernel/um/um.c`).  A DLL's read-only pages
  (headers, code, read-only data) now map frames shared by every process
  whose page holds the same bytes, as Windows shares image sections.  A
  table keyed by the page's contents counts each frame's users, so a DLL
  relocated or bound differently in one process still shares every page
  that matches.  `PTE_IMAGE` (a bit the MMU ignores) marks them.  They are
  never writable: `VirtualProtect` to a writable protection, or a write
  from another process (`WriteProcessMemory`, as Firefox's launcher and
  sandbox do), gives that process its own copy first.  Writable sections
  are still private.  With the same 1.5 GB staged, Firefox loads its page.
- **AppContainer monikers** (`kernel32`, which is also `kernelbase`):
  `AppContainerRegisterSid`, `AppContainerUnregisterSid`,
  `AppContainerLookupMoniker` and `AppContainerFreeMemory`.  The Chromium
  sandbox inside Firefox looks them up in `kernelbase.dll` and stops the
  browser (a `CHECK`, breakpoint in `firefox.exe`) when one is missing.
  It only goes there for some utility processes, which start sooner when
  the machine is fast.  NovaOS runs no AppContainers; a registration is
  kept for the process so a lookup finds it.
- **Notepad++ and PuTTY** failed because of Firefox.  The corpus typed
  Notepad++'s command while Firefox's processes were still shutting down
  and had the keyboard, so the command was lost and Notepad++'s Alt+F4
  closed the Terminal instead.  `tools/appcorpus.py` now waits for every
  process a windowed program started to end (stopping any left after two
  minutes with `taskkill`) and checks that the Terminal answers before
  the next program.  The App Store window Firefox's install opened stayed
  behind PuTTY (5.5% of its screenshot); `store close`, a new Terminal
  command, closes it after Firefox.  Alone, Notepad++ and PuTTY matched
  their references all along (0.0% and 0.1%).
- **`[PMM] out of memory`** is logged with the request and the free
  memory, on the first failed page allocation and every 4096th after it,
  so a program that stops with an unrelated-looking crash shows why.

Under TCG, Firefox, Notepad++ and PuTTY now pass one after another.  The
KVM runs of PR #98 check the rest.

## Sampling rates, surround devices and a sound device picker

USB speakers, headsets and microphones now run at the sampling rate and
channel count they offer, not only 48 kHz stereo, and the sound device is
a choice: Settings has a Sound page listing every output and input, and
programs can play on or record from a device of their own choosing, as on
Windows.

- **Rates** (`kernel/drivers/usbaudio.c`): an Audio 2.0 clock source's
  rates are read from its `RANGE` (subranges of minimum, maximum and
  step; the usual rates from 8 to 192 kHz that fall in them), and an
  Audio 1.0 format's from its rate list or range.  48 kHz, the mixer's
  own, is taken where offered, else the lowest rate above it, else the
  highest below it; the clock is set to it and read back, and the
  stream runs at whatever the clock reports.  Playback and recording
  convert between the mixer's 48 kHz and the device's rate by linear
  interpolation with an exact rational step, so packets carry a varying
  whole number of frames that averages the device's rate (5 or 6 a
  microframe at 44.1 kHz).
- **Channels**: one to eight channels each way.  A surround speaker gets
  the mixer's left and right in its first two channels (front left and
  right, as Windows plays stereo on surround speakers) and silence in
  the rest, a mono one their average; a microphone with more than two
  channels is recorded from its first two.
- **Names**: devices are named like Windows endpoints after their
  product string, "Speakers (Product)" and "Microphone (Product)", with
  "2- " before the product when another device already has the name.
- **Device picker** (`kernel/drivers/audio.c`): the mixer mixes every
  attached output, each with its own position and silence, instead of
  only the newest one.  Each direction has a default, the newest device
  until another is chosen (`AudioSetDefault`); a stream plays on the
  default or on the device its program chose (`AudioRoute`), and goes to
  the default if that device is unplugged.  Inputs record only while a
  stream records from them.  `NtNovaAudioCtl` gains ops 10 (list the
  devices), 11 (choose the default) and 12 (route a stream).
- **Settings > Sound** lists the outputs and inputs, the default one's
  circle filled; clicking one makes it the default, and the page
  refreshes when a device is plugged in or out.
- **winmm**: `waveOutGetNumDevs`/`waveInGetNumDevs` count the devices
  (oldest first, the order Settings shows), `GetDevCaps` names them, and
  `waveOutOpen`/`waveInOpen` with a device ID use that device;
  `WAVE_MAPPER` follows the default.  Device presence is asked each time
  instead of once per process, so a USB device plugged in later is seen.
- **mmdevapi**: an endpoint for each device; `EnumAudioEndpoints` lists
  them, `GetDefaultAudioEndpoint` returns the default, `GetDevice` finds
  one by ID (the sound card's speakers and microphone keep their IDs), a
  device's friendly name, description and adapter come from its name,
  and an `IAudioClient` activated on an endpoint plays on (records from)
  that device.
- **Tests**: `tools/usbredirpeer.py` takes `--rates`, `--channels`,
  `--mic-channels` and `--product`.  The devices suite's `usbheadset`
  boot plugs a 44.1 kHz Audio 2.0 surround headset (six speaker channels,
  four microphone channels) and then a full-speed speaker: the tone must
  sound at its pitch in the headset's front channels with the other four
  silent, its microphone must be recorded at its pitch, `soundtest ...
  dev=NAME` must play on and record from named devices through
  `waveOut`, `waveIn` and WASAPI, and `soundtest default out|in NAME`
  (Settings' operation) must move the default.  `soundtest endpoints`
  lists the WASAPI endpoints.
- Not yet: asynchronous endpoints' rate feedback, siTDs (full-speed
  isochronous behind a high-speed hub on EHCI), a mixer running at the
  device's rate (it stays 48 kHz and converts), DirectSound and XAudio2
  device enumeration (they use the default), a volume per device (the
  endpoint volume is still one per direction), and remembering the
  chosen default across reboots.

## A volume for each sound device, DirectSound and XAudio2 device lists, and the default kept across restarts

Each sound device now has its own volume, DirectSound and XAudio2 list
every device and open the one a program names, and the output and input
chosen in Settings are still the defaults after a restart, as on Windows.

- **A volume per device** (`kernel/drivers/audio.c`): the endpoint
  volume and mute that were one per direction are now each device's own,
  applied to what is mixed for it or recorded from it.
  `NtNovaAudioCtl` ops 8 and 9 take the device's id with the direction
  (`arg = capture | id << 1`, 0 for the default), so
  `IAudioEndpointVolume` acts on its endpoint's device; the volume keys
  act on the default output.  Settings' Sound page has a slider on each
  device's row (clicking it sets that device's level; clicking elsewhere
  on the row still makes it the default).  `waveOutSetVolume` on a device
  ID is now this program's volume on that device only (its handles there,
  the mapper's while that device is the default, and those it opens there
  later) instead of on all its handles.
- **Kept across restarts**: the chosen default and every level set are
  written to the registry (`HKLM\SOFTWARE\NovaOS\Audio\Render` and
  `Capture`, saved on drive C:), each device under a key that stays the
  same when it comes back: a USB device's vendor, product and port
  (`VID_46F4&PID_0002 at usb1 port 5`), the sound card's "HDA".  At boot
  (`AudioLoadSettings`, once the registry is loaded) and whenever a
  device attaches, it gets its saved level.  While the chosen default is
  attached, a device attached after it no longer takes over (it is
  ranked just below it; the log says "Attached ... (the chosen default
  stays)"); when the chosen device is absent the newest one is the
  default as before, so an unplugged favourite falls back to the
  built-in device, and plugging it in again makes it the default again.
- **DirectSound**: `DirectSoundEnumerate` and
  `DirectSoundCaptureEnumerate` list the "Primary Sound Driver" and then
  every device by its Windows name, with its endpoint's GUID
  (`PKEY_AudioEndpoint_GUID`, `{6e6f7661-6864-6100-0000-...}`) and its
  endpoint ID as the module; `DirectSoundCreate`, `Initialize` and the
  capture equivalents with one of those GUIDs play on (record from) that
  device, and `GetDeviceID` resolves the default GUIDs to the current
  default's.  The sound card's speakers and microphone keep the GUIDs
  they had.
- **XAudio2**: XAudio2 2.7's `GetDeviceCount` and `GetDeviceDetails`
  list the default (index 0, `GlobalDefaultDevice`) and then every
  output with its name and endpoint ID; `CreateMasteringVoice` with an
  index (2.7) or an endpoint ID (2.8 and 2.9) plays on that device.  The
  ID, GUID and name helpers are shared in `userland/winmm/audiodev.h`.
- **Tests**: `soundtest level out|in [LEVEL]`, `wovolume` and `dsenum`,
  `dev=NAME` for `dsound` and `dscapture`, and `xa2test devices [NAME HZ
  MS]`.  The devices suite's `usbheadset` boot (040) sets the surround
  headset to a quarter (its tone must sound a quarter as loud, the other
  devices stay at full), plays and records through DirectSound and plays
  through XAudio2 on the named surround headset, then makes it the
  default and restarts: it must be the default and at a quarter again.
  The `usbaudio` boot (020) does the same with QEMU's speakers at half
  volume.
- **xHCI event ring** (`kernel/drivers/xhci.c`): 4,096 events instead
  of 255.  Isochronous streams post an event per packet (16 a
  millisecond for a high-speed headset playing and recording), so with
  three USB sound devices attaching at boot the ring could fill while the
  driver enumerated the next one; the controller then drops every later
  event, so a command timed out and a stream stalled for good.  A full
  ring is now logged ("[USB] xHCI event ring full").
- Not yet: siTDs (full-speed isochronous behind a high-speed hub on
  EHCI), asynchronous endpoints' rate feedback, and a mixer running at
  the device's own rate (it stays 48 kHz and converts).

## One version string and no stale capability notes

Text inside NovaOS had fallen behind the OS.  The version now lives in one
place, `kernel/ke/version.h` (`NOVA_VERSION`), and the boot banner,
Settings > About, the Terminal's `ver` and title line and `sysinfo` all
print it, so Settings no longer says "Phase 9.5 desktop" and `ver` no longer
says "Phase 8 desktop".  Roadmap phases are tracked in `docs/roadmap/`, not
in the OS.

- **`sysinfo`** reports the real SMP state and CPU count ("SMP on, 4 CPUs")
  from `g_cpu_count` instead of a fixed "SMP off".
- **App Store.**  VLC and Audacity no longer say they need audio output
  NovaOS lacks (both play and record since the WASAPI work); OBS no longer
  says Direct3D is missing and is marked untested.

## System pointers (I-beam, busy, resize arrows, hand...) and SetSystemCursor

The system cursors were all the arrow, `SetSystemCursor` did nothing and a
program's pointer was scaled up by nearest neighbour at 200 %.  Now:

- **Every `IDC_*` pointer has its own shape** (`kernel/gdi/syscursor.c`):
  arrow, I-beam, busy (a turning ring), "working in background" (the arrow
  and a small ring), cross, up arrow, the four resize arrows, move, "no",
  hand and help.  No permissively licensed cursor set has the whole Windows
  set (X.org's MIT "whiteglass" lacks the diagonal resize arrows and the
  "no" sign; Breeze, Bibata, DMZ and Phinger are GPL or CC-BY-SA), so they
  are outlines drawn the way the desktop's arrow already was: unions of
  polygons, discs, rings and ring arcs with an anti-aliased outline and
  fill, rendered at the display's device resolution, so they are sharp at
  200 %.  The kernel renders a shape once per scale and busy-ring phase.
- **The desktop shows resize arrows** over a window's resize edges and
  corners and while one is being dragged.
- **user32**: `LoadCursor(NULL, IDC_*)` gives real 32 x 32 cursors (with a
  64 x 64 image) that `DrawIconEx` and `GetIconInfo` see; `SetCursor` of
  one asks the kernel to draw that system shape (`NtNovaGuiCtl` op 19,
  arg 3).  `DefWindowProc`'s `WM_SETCURSOR` sets the resize arrows for
  `HTLEFT`, `HTTOPRIGHT` and the other edge codes, for programs that draw
  their own frame, and the arrow for other non-client parts.
- **`SetSystemCursor`** replaces a system pointer for every program and
  destroys the cursor it is given, as on Windows (op 27);
  `SystemParametersInfo(SPI_SETCURSORS)` puts NovaOS's own back.  Op 28
  hands user32 the kernel's drawing of a system pointer.
- **Program cursors at 200 %**: a program's cursor is sent at the
  display's scale (op 19, arg 4: device pixels): the 64 x 64 image of a
  cursor that has one, otherwise its 32 x 32 image smoothed up rather than
  doubled pixel by pixel.
- **`cursortest.exe`** (in the core self-tests) checks each `IDC_*` image
  and hot spot, that the desktop draws each pointer over a window and turns
  the busy ring, `SetSystemCursor` and `SPI_SETCURSORS`, and the size a
  program's cursor reaches the desktop at.  `anitest` now expects its
  spinner at the display's scale.
- Not yet: `CopyIcon` of an animated cursor keeps only its first frame;
  `SetSystemCursor` replacements last until restart (they are not saved
  in the registry).

## Thread priorities and priority classes

`SetThreadPriority` and `SetPriorityClass` used to be accepted and
ignored, so every program thread ran at base priority 8: an audio thread
asking for `THREAD_PRIORITY_TIME_CRITICAL` waited behind a program's busy
worker like any other thread, and a background job at `IDLE_PRIORITY_CLASS`
competed with the foreground one.  They now set NT's base priorities.

- **NT's mapping** (`kernel/um/um_thread.c`): a process's class sets the
  base (IDLE 4, BELOW_NORMAL 6, NORMAL 8, ABOVE_NORMAL 10, HIGH 13,
  REALTIME 24) and `SetThreadPriority` adds -2 to 2, kept within 1-15;
  `THREAD_PRIORITY_IDLE` and `TIME_CRITICAL` saturate at 1 and 15 (16 and
  31 in a real-time process).  Changing a class moves every thread of the
  process.  `GetThreadPriority`, `GetPriorityClass`,
  `Set/GetThreadPriorityBoost` and `Set/GetProcessPriorityBoost` work, through
  `NtSetInformationThread` (`ThreadPriority`, `ThreadBasePriority`,
  `ThreadPriorityBoost`) and `NtSetInformationProcess` /
  `NtQueryInformationProcess` (`ProcessPriorityClass`,
  `ProcessPriorityBoost`), which 32-bit programs can now call too.
  `CreateProcess` honours the `*_PRIORITY_CLASS` flags, and a child of an
  IDLE or BELOW_NORMAL process inherits its class, as on Windows.
- **No real-time for programs**: REALTIME (and an absolute priority of 16
  or more) needs SeIncreaseBasePriorityPrivilege, which only an
  administrator's token holds; without it `SetPriorityClass` gives HIGH, as
  Windows does.  So a program never gets above 15.
- **The scheduler** (`sched_set_base_priority`): a new base takes effect
  at once.  A queued thread moves to its new place and preempts the thread
  running on its CPU if it now outranks it; a running thread lowered below
  a queued one gives way.  A priority change ends any boost, and boosts
  from then on start from and decay back to the thread's own base.
- **System threads above programs**: the desktop moved from 9 to 16, the
  device poll and audio mixer threads from 16 to 17, so no program thread,
  at `TIME_CRITICAL` or boosted, can hold up input, window management or
  sound, and a long redraw can't delay a sound buffer.
- **`THREAD_BASIC_INFORMATION.BasePriority`** is now the thread's
  increment over its class, as on NT (it was the absolute base);
  `ProcessBasicInformation` reports the class's base priority.
- **Measured** in QEMU (TCG) on 2 CPUs with the new `prioritytest` (64- and
  32-bit): with wake-up boosts off and a busy NORMAL thread on each CPU, a
  `THREAD_PRIORITY_HIGHEST` thread woken by an event ran after 0.05 ms at
  the 95th percentile, a NORMAL one after 18.8 ms (the busy thread's time
  slice).  `boosttest` 0.08 ms and `sleeptest timer` 0.25 ms still pass.
  `smpstress` on 4 CPUs passes; its 8-thread critical section took
  2060-2440 ms against 1790-2090 ms on main on the same host, the
  desktop now preempting the threads it shares a CPU with.
- **Not done**: NT's foreground-process boost and quantum stretching;
  kernel threads other than these (network, USB, ACPI, saving drive C:)
  stay at 8, so a busy HIGH-class program can delay them until the balance
  set lifts them after 3 s; `ProcessBasePriority` is accepted and ignored.

## Time zones: the first-boot setup's time zone page (Phase 22.1)

NovaOS kept UTC everywhere: the dock clock, `GetLocalTime` and the C
runtime's `localtime` all showed the hardware clock's time.  The clock
still keeps UTC, and the time zone is now kept where Windows keeps it,
`HKLM\SYSTEM\CurrentControlSet\Control\TimeZoneInformation`, with local
time worked out from it:

- **Welcome to NovaOS** has a **Time zone** page between the name and
  the display: type a city to find its zone, or pick one with the arrow
  keys, Page Up/Down or the mouse; the page shows the time there now.
  Next saves it (and `HKLM\SOFTWARE\NovaOS\Setup\TimeZone`).
- **Settings > Time & language** shows the zone, and its Change button
  opens the same page alone.  **`tzutil.exe`** (System32; `/g`, `/s
  NAME[_dstoff]`, `/l`) sets and lists zones as on Windows.
- **The zones**: 139, every Windows zone name (`kernel/ke/tzdata.inc`,
  generated by `tools/gen_timezones.py`): the names and the cities shown
  for each come from Unicode CLDR's `windowsZones.xml` (Unicode License
  v3), the offsets and daylight-saving rules from the IANA tz database
  (public domain) in Windows' form (month, the n-th or last weekday, the
  hour), checked against the years around the one generated for.  Six
  zones whose rule changes from year to year (Chile, Greenland, Morocco,
  Egypt, Israel and the West Bank) keep 2026's.  They are in the registry
  as on Windows, `...\Windows NT\CurrentVersion\Time Zones\NAME` (`Display`,
  `Std`, `Dlt`, `TZI`), rebuilt at every start.
- **The kernel** (`kernel/ke/timezone.c`) shows local time in the dock
  clock, the Start menu's greeting, the Terminal's `date` and `time`, and
  Calendar.
- **kernel32**: `GetTimeZoneInformation` and `GetDynamicTimeZoneInformation`
  (with the standard or daylight answer), `GetTimeZoneInformationForYear`
  for any zone of the list, `EnumDynamicTimeZoneInformation` (advapi32),
  `SetTimeZoneInformation`, `SetDynamicTimeZoneInformation`,
  `SystemTimeToTzSpecificLocalTime(Ex)`,
  `TzSpecificLocalTimeToSystemTime(Ex)`, `GetLocalTime`,
  `FileTimeToLocalFileTime` and `LocalFileTimeToFileTime` follow the zone,
  re-read at most once a second.
- **The C runtime**: `localtime` (with `tm_isdst`), `mktime`, `ctime`,
  `_tzset` and `_timezone`/`_daylight`/`_dstbias`/`_tzname`, `_ftime`,
  `_strdate`/`_strtime` and `strftime`'s `%Z` and `%z` are local;
  `_mkgmtime` and `timegm` stay UTC.

The core self-test `welcome` now picks Tokyo on the new page by typing
it; `tztest` (64- and 32-bit) then checks UTC+9 in `GetLocalTime` and
`localtime` and the rules of Berlin, Sydney and Los Angeles at known
instants, and `tzutil /s UTC` puts the zone back for the later tests.
Files keep UTC times on disk (FAT included).

The keyboard layout page is the rest of step 22.1.

## Timer queue timers on time under load

`sleeptest timer` failed on main now and then (CI under TCG, and under
KVM): its 1 ms timer queue timer fired about 9 ms late at the 95th
percentile with a busy thread on every processor.  A trace of each CPU's
timer interrupts, arms, wake-ups and switches around every late firing
found three scheduler causes, none in the timer queue code itself.

- **A deadline armed over before it fired** (`sched_sleep_until_tsc`).  A
  new sleep re-armed its CPU's one-shot timer for itself whenever the
  deadline the timer was armed for had gone by, on the idea that it had
  fired already.  It may not have: the one-shot count runs a little off the
  TSC and a virtual CPU's timer fires late, so the interrupt for a deadline
  just passed was often still to come.  Re-armed for the new, later sleep,
  it never came, and the first sleeper waited for the next 10 ms tick (the
  "timed wait ends at the next tick" the KVM work traced).  A sleep now
  only arms the timer sooner; a deadline gone by is armed again and fires
  at once.
- **A timer wake queued last.**  A thread woken by a timer (its deadline,
  or the waitable timer it waits on being set, which is how a timer queue's
  worker learns its due time) preempts the running thread of the same
  priority.  When a kernel thread of higher priority was running just then
  (for a few microseconds, usually), it went to the back of the queue
  instead, and the busy thread queued ahead of it ran a whole 20 ms slice
  first.  It now goes first in the queue either way, and runs as soon as
  the higher-priority thread is done.
- **A thread preempted at a tick sent to the back.**  The thread preempted
  for a deadline wake at a timer tick lost its place and its slice (the
  preemption by `IPI_WAKE` already kept both), so a thread starting a 1 ms
  wait of its own waited out the other threads' slices first.  Both
  preemptions now keep it ahead of the threads waiting their turn.

Measured with `sleeptest timer` in QEMU (TCG, two CPUs, a busy thread on
each), on a 4-core host with and without busy host threads; the 1 ms
timer queue timer under load, before and after:

| Host | Runs | Before: median, 95th percentile, max | After |
|---|---|---|---|
| idle | 5 + 5 | 0.14-0.19 ms, 0.20-0.27 ms, up to 9.1 ms | 0.13-0.17 ms, 0.21-0.26 ms, up to 3.3 ms |
| one busy thread | 5 + 5 | 0.14-0.20 ms, 0.24-9.1 ms, up to 19.4 ms | 0.16-0.19 ms, 0.26-3.3 ms, up to 4.4 ms |

The 9 to 19 ms outliers (a missed deadline or a lost turn) are gone.  What
is left comes from a busy host: QEMU itself then delivers a CPU's timer
interrupt 1 to 5 ms late, often on both CPUs at once (the trace shows the
interrupt arriving late, with nothing in the guest holding it up), and
`sleeptest timer` can still fail there (1 run in 5 with one busy host
thread, every run with two).  The test's 1 ms limit is unchanged.

## 1088 TLS indexes and FLS callbacks at process exit

The two pieces the C runtime work left open now behave as on Windows.

- **TLS expansion slots.**  `TlsAlloc` stopped at the TEB's 64 slots, few
  enough that a program loading many DLLs (Firefox's) could run out.  It
  now hands out 64 + 1024 indexes, from `ntdll`
  (`userland/ntdll/ntdll_tls.c`: `RtlTlsAlloc`, `RtlTlsFree`,
  `RtlTlsSetValue`).  Indexes 64-1087 live in a per-thread array the TEB's
  `TlsExpansionSlots` field points at, allocated the first time the
  thread sets one of them and freed when the thread ends, after the DLLs'
  `DLL_THREAD_DETACH`.  `TlsFree` makes the index read zero again in
  every thread of the process, not only the caller: the kernel clears it
  in each thread's TEB or array (`NtSetInformationThread` with
  `ThreadZeroTlsCell`, as on Windows).  The 32-bit `TlsGetValue` and
  `TlsSetValue` also stopped addressing the TEB's slots 8 bytes apart.
- **FLS callbacks at process exit.**  `ExitProcess`, and returning from
  `main`, now run the exiting thread's `FlsAlloc` callbacks before any
  `DLL_PROCESS_DETACH`, as Windows' `LdrShutdownProcess` does; the values
  of threads still running reach their callbacks when a DLL frees its
  index during the detach, and none runs twice.
  `RtlDllShutdownInProgress` now says TRUE while the process detaches.
- **Test.**  Core self-test `tlsslots` (64- and 32-bit).

## The update check waits for an IPv4 address

Right after a restart the update check waited for "an address" and went on
as soon as the machine had a global IPv6 address.  SLAAC finishes before
the DHCP lease, so a channel at an IPv4 address (the self-test's
`http://10.0.2.2:18090/`) was tried with no IPv4 address yet and failed with
"Downloading from 10.0.2.2 failed: Could not connect." (the devices suite's
"up to date" test, 0.8 s after the restart).

When the channel's host is an IPv4 literal, the check now waits for the IPv4
address (the DHCP lease or a static one) and no longer counts an IPv6
address; names and IPv6 literals still go on with either.  The wait is the
same bounded 30 seconds as before, so a machine with no IPv4 network still
reports the failure.

## Updates: NovaOS updates itself from the App Store

An installed NovaOS now updates itself to a newer build
([updates.md](updates.md)).  The App Store has an **Updates** page (and
the Terminal an `update` command) that reads an update channel (by
default the newest GitHub release's `novaos-update.txt`), downloads the
new kernel and boot loader, checks their sizes, SHA-256 and version, and
stages them on the EFI System Partition.  The next restart starts the new
kernel once; when it reaches the desktop it becomes the installed one.
If it never gets there, the following start goes back to the old one and
throws the update away.

- **The boot loader** starts `\EFI\NOVA\kernel.new` once when the update
  is marked pending, marks it as being tried, and starts the old kernel
  again if that mark is still there next time
  (`BOOT_FLAG_UPDATE_TRIAL`, `BOOT_FLAG_UPDATE_FAILED`).
- **`kernel/fs/update.c`**: the channel, the download and checks, the
  staging, and finishing or undoing the update at the next start;
  `FatRename` in the FAT driver.
- **The version** is now 0.1.0 (the release Phase 22 makes), kept in a
  marked field of the kernel image that `tools/mkupdate.py` can stamp on
  a copy; the banner, Settings, `ver` and `sysinfo` read it there.
- **`tools/mkupdate.py`** writes a channel's three files from a build.
- **Self-test**: the devices suite's `update` boot updates `nova.img`
  to a test build one version newer, restarts into it twice, and goes
  back to it when the next update is reset while it first starts.

## USB Audio Class 2.0

USB Audio 2.0 headsets, speakers and microphones now play and record
like the Audio 1.0 ones: plug one in and `waveOut`, `waveIn` and WASAPI
use it, and unplugging it hands sound back to the device before.  Most
current USB headsets and DACs are Audio 2.0 devices.

- **USB audio** (`kernel/drivers/usbaudio.c`): a control interface with
  protocol 0x20 is taken as Audio 2.0.  Its streaming interfaces are the
  ones its Interface Association groups (or else every Audio 2.0
  streaming interface of the device); a setting is used when its
  class-specific AS descriptor has Format Type I with PCM in
  `bmFormats` and two channels (playing) or one or two (recording).  The
  terminal the setting links to names a clock: selectors in front of the
  clock source are switched to their first input, 48 kHz is set on the
  source where the host may set it (`CS_SAM_FREQ_CONTROL`, 4 bytes) and
  read back, and a clock running at anything else is not used.  Feature
  units take Audio 2.0's two-bit controls (unmuted, 0 dB where the host
  may set them).  High-speed endpoints polled every microframe get 6
  frames a packet.
- **Wider samples**, for Audio 1.0 and 2.0 alike: samples of 16 to 32
  bits in 2-, 3- or 4-byte slots.  The mixer's 16-bit samples go into
  the top two bytes of each slot when playing and are taken from there
  when recording (many Audio 2.0 devices offer only 24-bit samples).
- **Tests**: `tools/usbredirpeer.py --uac2` is an Audio 2.0 headset (a
  programmable clock behind a clock selector, 24-bit samples in 4-byte
  slots out and 3-byte slots in, a packet every microframe both ways).
  The devices suite's `usbheadset` boot plugs one into the xHCI
  controller after its other tests (`uac2 tone`, `uac2 record`, `uac2
  capture`, `uac2 unplug`): the tone must sound in its WAV alone, its
  microphone's 988 Hz must be recorded through `waveIn` and WASAPI, and
  unplugging it must hand recording back to the OHCI microphone.
- Not yet: rates other than 48 kHz (the clock's rate ranges are not
  read), more than two channels, asynchronous endpoints' rate feedback,
  siTDs (full-speed isochronous behind a high-speed hub on EHCI), and
  choosing the playback or recording device in Settings.

## Sound at each device's own rate, USB audio rate feedback, and waveOut device 0 as the default

The mixer now runs at each sound device's own rate, sound reaches a
device converted once at most, asynchronous USB audio devices are fed at
the rate they ask for, and `waveOut`/`waveIn` device 0 is the default
device, as on Windows.

- **The mixer at the device's rate** (`kernel/drivers/audio.c`,
  `kernel/drivers/usbaudio.c`): each output and input has a rate (the HD
  Audio card 48 kHz, a USB device the rate its clock was set to), its
  ring holds frames at that rate, and the mixer converts each stream to
  it as it mixes (linear interpolation; a stream already at that rate is
  copied).  Capture streams are converted from the input's rate the same
  way.  `usbaudio.c` no longer converts anything: packets carry the
  ring's frames as they are (spread over the device's channels and slot
  size), a whole number each that averages the rate.  The ring is sized
  for a third of a second at the device's rate.
- **Streams at their own rate**: a stream's frames can be at any rate
  from 8 to 384 kHz (`NtNovaAudioCtl` op 13; ops 5 and 7 now report the
  default device's rate).  `waveOut` and `PlaySound` hand the kernel the
  program's own rate and convert only the sample format, so a 44.1 kHz
  sound on a 44.1 kHz USB headset arrives sample for sample, and a 22.05
  kHz one is converted once instead of twice (to 48 kHz and then to the
  device's rate).  WASAPI keeps its 48 kHz mix format.
- **Asynchronous endpoints' feedback**: a USB speaker on its own clock
  has a feedback endpoint (the second endpoint of the setting, by its
  usage, or the one its `bSynchAddress` names) that says how many frames
  it really plays: at full speed in 10.14 fixed point frames a
  millisecond, at high speed in 16.16 frames a microframe.  NovaOS
  streams it and fills each packet with that many frames on average
  instead of the nominal rate's, so the device never runs dry or
  overflows however long it plays (a value further than an eighth from
  the nominal is ignored, after trying the other format, which some
  full-speed devices use).  The log says "audio output is asynchronous"
  and the rate the device first asks for.
- **`waveOut` device 0 is the default** (`userland/winmm`): device IDs
  list the default device first and then the others oldest first, as on
  Windows, where device 0 is the preferred device (it moves when the
  default does); `waveOutMessage` and `waveInMessage` answer
  `DRVM_MAPPER_PREFERRED_GET` and `DRVM_MAPPER_CONSOLEVOICECOM_GET` with
  device 0.  `WAVE_MAPPER` still follows the default wherever it moves.
- **Eight devices each way**: the mixer takes up to eight outputs and
  eight inputs (it was four, so a fifth USB speaker was silently left
  out; one past eight is now logged), as many as the device lists give
  programs.
- **Tests**: `soundtest tone ... rate=N` plays at N Hz, `soundtest info`
  prints the preferred device IDs, and `tools/usbredirpeer.py
  --feedback HZ` makes the test speaker asynchronous.  The devices
  suite's `usbheadset` boot checks a 44.1 kHz tone arrives unchanged on
  the 44.1 kHz surround headset (035), that device 0 is the default and
  follows it (045), and that a full-speed USB Audio 1.0 speaker saying
  48,500 frames a second gets 48.5 frames a packet and a high-speed USB
  Audio 2.0 one saying 47,600 gets 5.95 a microframe (050).
- Not yet: siTDs (full-speed isochronous behind a high-speed hub on
  EHCI).  QEMU skips active siTDs and has no high-speed hub, so they
  cannot be tested here, and Intel chipsets since 2015 (the reference
  ThinkPad's included) have xHCI only, which handles such devices itself.

## Sound: a speaker another one took over from goes quiet

The devices suite's `usbaudio ohci` and `usbaudio unplug` checks failed on
loaded CI runners: the OHCI speaker's 550 Hz tone read 519 Hz (5.6% flat,
over the 5% limit), while the same tests passed locally.  The cause was in
the mixer, not in isochronous scheduling or the measurement.  When a new
output is attached, the mixer writes only to the new one, but the old
output still streams its ring (a USB speaker keeps its isochronous
transfers going, the HD Audio card its DMA).  Nothing wrote that ring any
more, so the old speaker played its last 341 ms over and over until it
was unplugged or became the playing output again.  When the switch came
while the ring still held the end of a tone, as it can when the mixer
thread runs late on a loaded host, the loop joined onto the tone through
gaps short enough to be bridged, and the recording held one long, flat
"tone" (reproduced locally at 527 Hz by plugging the next speaker in
during the tone).  When the ring held only a little of the tone, the
speaker repeated 100 ms bursts of it every 341 ms, which the check did
not look at.

- `kernel/drivers/audio.c`: at the switch the mixer clears the old
  output's ring beyond what was mixed for it, and from then on keeps
  80 ms of silence ahead of each attached output that is not playing, so
  what was mixed before the switch still plays and then it is quiet.
  (Clearing only from the next tick on was not enough: on a CI runner
  the mixer had fallen behind at the switch and 30 ms of the old lap
  still played.)  This also applies to the HD Audio card when a USB
  headset takes over.
- `tools/selftest.py`: `tones(..., only=True)` also fails when anything
  else sounds in the recording.  The `usbaudio xhci`, `usbaudio uhci` and
  `usbaudio unplug` checks use it on their speakers' WAVs.  With four busy
  host threads, the old kernel failed it (40 repeated bursts on the xHCI
  speaker) and the fixed one passed every run, also with QEMU and three
  busy threads pinned to one host CPU.

## USB microphones, and USB audio on EHCI

USB microphones and the microphones of USB headsets now record: plug one
in and `waveIn` and WASAPI capture record from it, as Windows does, and
from the HD Audio card's microphone again when it is unplugged.
High-speed USB audio devices now work on EHCI controllers too.

- **EHCI** (`kernel/drivers/ehci.c`): isochronous transfers for
  high-speed devices, in iTDs.  Like UHCI's isochronous TDs they go
  straight into the frame list, in front of the interrupt list, and come
  out once their frame has passed; an iTD carries a packet for each
  microframe the endpoint is polled in (eight for one polled every
  microframe), and a pipe polled every 2^n frames gets one every 2^n
  frames.  Full-speed isochronous endpoints behind a high-speed hub would
  need siTDs, which are not written (their pipes are refused; on a root
  port such a device goes to the companion controller, which streams).
- **The mixer** (`kernel/drivers/audio.c`): inputs are attached by their
  drivers (`AudioInputAttach`/`Detach`: a ring, a position and a start
  and stop) instead of being the HD Audio card; the newest records, and
  unplugging it goes back to the one before.  The recording device's
  name (`waveInGetDevCaps`, WASAPI) is the input's own, e.g. "USB
  Microphone (port 1)".  Playback: behind what it has mixed, the mixer
  now keeps the rest of the playing output's ring silent.  When the mixer
  thread was held up for longer than its 80 ms lead (here while a USB
  speaker was being set up on a busy host), the device played what the
  ring held a lap earlier: 30-40 ms of the previous tone's end, which
  failed `usbaudio unplug` about one run in two in this container, on
  `main` too.  Now such a hold-up leaves a gap.
- **USB audio** (`kernel/drivers/usbaudio.c`): besides the first
  streaming interface it can play on, the driver takes the first it can
  record from: a setting whose IN endpoint carries 48 kHz 16-bit PCM,
  mono or stereo.  The IN stream runs from the moment the microphone is
  plugged in; each packet is copied into a 64 KiB ring (a mono
  microphone's samples twice, as stereo), which the mixer reads from
  while something records.
- **Tests**: QEMU has no USB microphone, no high-speed audio device and
  nothing for EHCI's iTDs to talk to, so `tools/usbredirpeer.py` is one:
  a USB Audio Class 1 headset or microphone behind QEMU's `usb-redir`
  device, speaking the usbredir protocol (its speaker's packets go to a
  WAV, its microphone sends a sine in real time).  The devices suite has
  a third boot, `usbheadset`, with no HD Audio card and a high-speed
  headset on an EHCI controller: `soundtest tone` must sound in the
  headset's WAV alone (eight packets an iTD), and `soundtest record` and
  `capture` must record its microphone's tone.  Then full-speed
  microphones, each hearing its own tone, are plugged into xHCI, OHCI and
  UHCI controllers, which must each record the newest one's tone (the
  first isochronous IN on all four controllers), and unplugging the UHCI
  one must hand recording back to the OHCI one.
- Not yet: USB Audio 2.0, siTDs, sampling rates other than 48 kHz,
  asynchronous endpoints' rate feedback, webcams, and choosing the
  playback or recording device in Settings.

## USB isochronous transfers and USB speakers

USB speakers and headsets now play: plug one in and NovaOS's sound moves
to it, as Windows does, and back to the sound card when it is unplugged.
They need isochronous transfers, which no USB controller driver had.

- **USB core** (`kernel/drivers/usb.c`, `usb.h`, `usb_hc.h`): pipes for
  isochronous endpoints; `UsbSetInterface` switches an interface to
  another alternate setting (audio devices keep their streaming endpoint
  out of setting 0), closing the old setting's pipes and opening the new
  one's, which xHCI learns of in one Configure Endpoint that drops and
  adds endpoints; `UsbDevConfig` hands a driver the whole configuration
  descriptor.  `UsbIsoStart` keeps a ring of transfers of one packet per
  service interval scheduled back to back; each finished one goes to the
  driver's callback (to be refilled, or read) and is scheduled again.
- **Controllers**: xHCI queues an Isoch TRB per packet ("as soon as
  possible" after the one before, each with an event); OHCI an
  isochronous ED at the end of the interrupt list with TDs of up to eight
  packets; UHCI TDs placed straight in the frame list and taken out once
  their frame has passed.  EHCI refuses isochronous pipes (no iTDs or
  siTDs yet), so a full-speed audio device works on its companion
  controller.
- **USB audio** (`kernel/drivers/usbaudio.c`): a USB Audio Class 1 driver.
  It picks the streaming setting that carries 48 kHz, 16-bit stereo PCM
  (the mixer's format), sets the sampling rate where the endpoint has
  that control, unmutes the feature unit at 0 dB, and streams the mixer's
  ring: 48 frames a millisecond, eight transfers of 8 ms in flight.
- **The mixer** (`kernel/drivers/audio.c`): outputs are attached by their
  drivers (`AudioOutputAttach`/`Detach`: a ring and a position) instead of
  being the HD Audio card; the newest plays, and unplugging it hands
  playback back to the one before.  The mixer starts without an HD Audio
  card, so a machine with only USB speakers has sound; recording still
  comes from the HD Audio card.
- **Tests**: the devices suite has a second boot, `usbaudio`, with no HD
  Audio card and QEMU `usb-audio` speakers, each recorded to its own WAV:
  one on xHCI at boot, one plugged into an OHCI and one into a UHCI
  controller while NovaOS runs, then the UHCI one unplugged.  `soundtest
  tone` plays after each step, and each speaker's WAV must hold its tones
  (the OHCI one's two: before and after the UHCI speaker came and went).
  `tools/selftest.py` tests can now run a step before their command
  (`before=`, here plugging a speaker in).
- Not yet: isochronous IN (USB microphones and webcams: QEMU 8.2 has no
  device to test them with, so the IN paths are untested), EHCI
  isochronous transfers, asynchronous endpoints' rate feedback, USB Audio
  2.0, and choosing the playback device in Settings.

## OpenGL on the host's GPU (virgl)

- **OpenGL programs now draw on the host's GPU** when NovaOS runs in QEMU
  with a 3D virtio-gpu (`virtio-vga-gl`), as Vulkan and Direct3D programs
  have since Venus: Mesa's virgl encodes their OpenGL calls and QEMU's
  virglrenderer runs them on the host's OpenGL.  `gltest` reports
  `GL_RENDERER virgl (...)` and OpenGL 4.5, 64- and 32-bit.
- **The App Store's "Venus" brings it.**  `tools/build_venus.py` now builds
  Mesa 26.2.4's WGL `opengl32.dll` with the virgl driver beside Venus, from
  the same patched tree, and `venus.7z` carries it as
  `opengl32_virgl.dll` and `libgallium_virgl.dll`, 64- and 32-bit.  Its
  back end, `third_party/mesa-venus/virgl_nova_winsys.c`, is NovaOS's
  version of the winsys Linux's DRM interface gives virgl: a 3D context on
  the VIRGL2 capability set, classic 3D resources whose guest pages the
  kernel gives them, mappable host blobs for persistent and coherent
  buffers, and command streams and transfers fenced on one timeline in
  submission order, which tells when a resource is idle.  Frames reach the
  window through the GDI: the front buffer is read back into its pages and
  copied to the WGL display target, as Mesa's vtest does.
- **NovaOS has its own `opengl32.dll`.**  Like Windows', it is the
  system's, and the OpenGL implementation is the driver's: when a process
  starts it loads `opengl32_virgl.dll` if the machine has a virtio GPU with
  virgl's capability set and Venus is installed, and Mesa 3D's llvmpipe
  (installed as `opengl32_mesa.dll` now) otherwise, and its OpenGL 1.1 and
  `wgl*` exports jump to the one it loaded.  `GALLIUM_DRIVER=virgl` or
  `GALLIUM_DRIVER=llvmpipe` picks one, as in Mesa.
- **The kernel side** (`kernel/drivers/virtio_gpu.c`, `NtNovaGpuCtl` in
  `kernel/um/um_gpu.c`): two new operations create a classic 3D resource
  (`RESOURCE_CREATE_3D`, its guest pages attached with `ATTACH_BACKING` and
  the resource attached to the context) and transfer a box between it and
  its pages (`TRANSFER_TO_HOST_3D` / `TRANSFER_FROM_HOST_3D`, fenced on a
  timeline).  The resource's pages map into the program as a section, like
  a blob's host memory; they are freed only once the card has answered the
  resource's `RESOURCE_UNREF`.  Blobs are now attached to their context,
  as Linux does, because virgl's command streams name them.
- msvcrt has `qsort_s` (Mesa calls it).
- **Tests.**  The graphics suite runs `gltest` on virgl and on llvmpipe,
  64- and 32-bit, and the new `gltest fps 10` draws an OpenGL scene that
  keeps the rasterizer busy (64 blended quads over 640x480) for 10 s on
  virgl and 10 s on llvmpipe, each in a child process whose
  `GALLIUM_DRIVER` names the driver; virgl must draw more frames per
  second.  Under TCG it draws 12.6 to 15.9 frames per second against
  llvmpipe's 0.24 (52 to 65 times as many).
  CI's runner has no GPU: there virglrenderer draws with the host's
  llvmpipe, natively instead of inside NovaOS.
- Not yet: showing OpenGL's frames on the virtio GPU directly (they are
  read back every frame and drawn with the GDI), and virgl on a QEMU
  without blobs and context types (NovaOS's 3D path needs both).

## VLC and Audacity (19.6)

VLC 3.0.21 (the 32-bit PortableApps package) plays an H.264 and AAC MP4
with its Qt interface, video in its window and the sound through WASAPI,
and Audacity 3.7.4 (the 64-bit zip) records ten seconds from the
microphone, stops, and saves the project as an `.aup3` through its save
dialog.  The nightly app corpus runs both (`tests/appcorpus/870-vlc.py`,
`880-audacity.py`): `tools/appcorpus.py` boots with a microphone that hears
a 523 Hz tone and keeps what NovaOS played in `sound.wav` (`App(mic=True)`,
`App(sound=(hz, ms))`), as the core self-tests do, so VLC's 440 Hz tone is
checked after the run; without PulseAudio those two are skipped rather than
failed (the nightly workflow installs it, with QEMU's PulseAudio backend).  The ffmpeg test's clip is now thirty seconds of SMPTE colour bars with
the tone, which VLC loops; VLC offers the decoder its Direct3D formats
first and the display rejects each for want of a converter, which takes
seconds without KVM, so the screenshot waits for the colour bars to show
in VLC's window.  The corpus does not run VLC with `-vv`: its verbose log
goes through the kernel log a line at a time, and on a TCG machine that
is enough for the audio to run late and drop (the same build plays the
whole clip cleanly without it), which a later change should look into.

Two message-loop gaps held VLC's video back.  Qt's Windows event
dispatcher drives its posted events from a `WH_GETMESSAGE` hook: the hook
resets the flag that lets another thread post the wake-up message, and
`user32` accepted the hook but never called it, so after the first wake-up
no cross-thread signal reached the Qt thread again and VLC's video thread
waited forever for the interface to hand it a window.  `GetMessage` and
`PeekMessage` now run the thread's `WH_GETMESSAGE` hooks on every message
they return.  The same hook decides with `GetQueueStatus(QS_INPUT |
QS_TIMER)` whether the queue still holds input; ours reported any pending
message under every flag, so it now reports what is actually queued
(posted messages, mouse, keys, paints, due timers, sent messages) masked by
the flags asked for.

wxWidgets' buffered painting blanked Audacity's toolbars once its shared
buffer had grown larger than the window: it blits the window's part of the
buffer with `StretchDIBits`, whose source y is measured from the bottom of
a bottom-up DIB, and `gdi32` read it from the top and drew the buffer's
empty bottom rows.  Behind that, every GDI call on a 24-bit DIB section
synchronised the 24-bit view with the pixels, which made Audacity's main
thread spend its time in `memcmp`; the view is now synchronised at the
points that read or write it (selecting the bitmap, blits, `GetPixel`,
`GetDIBits`, `SetDIBits`), and only the rows that changed.  `gdi32` holds
10,000 objects (Audacity's theme alone makes thousands of bitmaps) and
every DC starts with the 1x1 default bitmap, which `SelectObject` returns
and accepts back, as wxWidgets restores it.

Getting the two to start took more: the kernel's loader holds 1024 modules
(VLC loads every plugin, about 410), initialises them in dependency order
with an explicit stack (a DLL reached only through another's forwarded
export counts as that one's dependency, so `vcruntime140`, which `msvcrt`
forwards the C++ exception entry points to, is on the list `ntdll` reads
and every throw finds its unwind information), and the loader lock is a
critical section the PEB publishes as `LoaderLock` (Crashpad checks
whether its thread owns it);
`WaitOnAddress` moved to `kernelbase.dll`, where VLC expects not to find it
in `kernel32`; with no network adapter lwIP still runs for 127.0.0.1 and
::1 (Audacity's plugin scanner talks to itself over loopback); `gdiplus`
exports every name wxWidgets' Direct2D-less renderer imports (brushes,
pens, paths, text, 606 in all); `comctl32`'s task dialog takes the
byte-packed `TASKDIALOGCONFIG`, its SysLink reports its ideal size and
strips quoted anchors; `kernel32` gained timer queues (`CreateTimerQueue`,
`CreateTimerQueueTimer`, `ChangeTimerQueueTimer`, `DeleteTimerQueueTimer`),
`user32` the DDE management library (no server answers, so no conversation
opens), `winspool` the printer enumeration (none), `advapi32` the
trustee and explicit-access builders, `wininet` the HTTP session calls
(unreachable, as `InternetOpen` gives no handle), and `msvcrt` about
seventy more calls.  `msvcrt`'s `fprintf` gathers a call's output and
writes it to the stream in one piece, as msvcrt does, rather than a
fragment of the format at a time: on an unbuffered stream (stderr) each
fragment was a system call.  The Terminal's `start` passes a program up
to 32 arguments, not 8 (VLC's command line has nine).  A process killed
from the terminal, or Ctrl+Alt+F12, dumps every thread's user stack with
module and offset, which is how the stalls above were found.

## VLC closes: windows of an ended thread answer nothing

VLC stopped responding as it closed (Alt+F4 in the App corpus): its last
log line was "releasing video...", and every program after it used to
fail until the corpus learnt to stop it from a second Terminal.  A dump of
VLC's threads (Ctrl+Alt+F12) showed four waits chained together: the main
thread joining the playlist, the playlist waiting for the video output to
close, the video output waiting for Qt's window thread to release the
video widget (a blocking queued signal), and Qt's window thread inside
`DestroyWindow`, waiting for an answer to `WM_DESTROY` from a window of a
thread that had already ended.

The video widget's native window had a child window made by VLC's video
event thread.  Qt destroyed the widget; `DestroyWindow` sent `WM_DESTROY`
to that child, VLC's event thread answered it by leaving its message loop
and ended, and `DestroyWindow` then sent `WM_DESTROY` to the child's own
child (also the event thread's).  `user32` sent messages to another
thread's window by queuing them for that thread and waiting until it
answered, and a thread that has ended never answers.  On Windows a
thread's windows go with it, so the message gets no answer and the sender
carries on.

`SendMessage` (and every message `user32` sends itself, such as the
`WM_DESTROY` and `WM_NCDESTROY` of `DestroyWindow`) now also waits on the
receiving thread: when that thread has ended, or ends without taking the
message, the message is taken back and the call returns 0.  VLC now closes
within a second of Alt+F4 and the corpus programs after it start normally.
The new `wndthreads` self-test (64- and 32-bit) destroys a window whose
child belongs to a thread that ends on the child's `WM_DESTROY`, and one
whose child's thread had already ended, and sends a message to the window
of an ended thread; a watchdog fails the test rather than leave the
Terminal waiting.

## VLC's corpus test runs with its verbose log

The VLC corpus test (`tests/appcorpus/870-vlc.py`) now starts VLC with
`-vv`, so the sound check covers a verbose run too: VLC writes some 1,200
log lines while the clip starts, each one twice (to stderr and to
`OutputDebugString`), and all of them reach the kernel log, yet the
recording must still hold an unbroken 440 Hz tone.  Its screenshot now
waits for the colour bars a second time, after the sound has run: under
TCG the clip could loop in between, and VLC shows its cone while it starts
over.  Locally under TCG the run passes with a 26 s unbroken tone.

The Phase 19.6 notes blamed a change between #73 and #94 for VLC's audio
running late and dropping every buffer under `-vv`.  A/B runs under TCG
showed otherwise: the Phase 19.6 branch before it took in main (f37357d)
and the same branch with main up to #94 merged both played only a
30-40 ms blip of the 30 s clip, while current main plays it with a few
seconds of glitches at start and then an unbroken tone.  Nothing in that
range made it worse.  The scheduling work merged since (thread and process
priorities, kernel lock wake-ups) is the likely reason the audio thread
now keeps up; that range was not bisected.

The corpus's ffmpeg test, which makes the clip VLC plays, typed a
163-character command into a Terminal that takes at most 158, so ffmpeg
was handed `C:\Apps\i` as its output file and VLC had nothing to play.
The command drops `-c:a aac` (AAC is MP4's default sound codec anyway)
and fits.  Longer command lines in the Terminal remain a gap: Windows'
console takes up to 8,191 characters.

## Waitable timers on the TSC

Phase 18.7 made `Sleep` and wait timeouts end when they are due; waitable
timers still fired on the 10 ms tick.  They now end on the TSC too.

- **Kernel timer objects keep TSC deadlines** (`kernel/um/um_thread.c`):
  `NtSetTimer` turns the due time and period into TSC values, a wait on a
  timer sleeps until exactly that deadline (`sched_sleep_until_tsc`), and
  a periodic timer stays on its own grid, so it doesn't drift.  Setting a
  timer wakes the threads already waiting on it, which then sleep until
  its new due time (before, such a wait looked again only every 100 ms).
  `NtQueryTimer` reports the time left in 100 ns units.
- **`CreateWaitableTimer` makes a kernel timer** instead of an event that
  a kernel32 thread set when `GetTickCount64` (a 10 ms clock) said it was
  due.  Named timers are shared between processes, and
  `OpenWaitableTimer` opens them.
- **Completion routines run on time.**  `SetWaitableTimer`'s routine runs
  on the thread that set the timer when it waits alertably, as on
  Windows; an alertable wait (`SleepEx`, `WaitFor*ObjectEx`) now ends its
  slice when one of its thread's timer routines is due, and measures time
  on the performance counter instead of the 10 ms tick count.
- **Timer queues** (`CreateTimerQueue`, `CreateTimerQueueTimer`,
  `ChangeTimerQueueTimer`, `DeleteTimerQueueTimer`, `DeleteTimerQueueEx`)
  are new; they, threadpool timers (`SetThreadpoolTimer`) and winmm's
  `timeSetEvent` run their callbacks from worker threads that wait on a
  kernel waitable timer.  A freed timer's worker is kept for the next one.
- `sleeptest timer` measures a 1 ms waitable timer, a 5 ms periodic one
  (each firing against its place on the grid), a 1 ms timer's completion
  routine in `SleepEx` and a 1 ms timer queue timer, idle and under load.
  In QEMU (TCG, 2 CPUs) the 95th percentile under load was 0.26 to
  0.59 ms late over four runs (the 10 ms tick made it up to 10 ms).
- Not yet: a timer queue timer (or any timer) set while another thread
  already waits on it can be late by up to a 20 ms time slice when every
  CPU is busy: a thread woken that way waits for the running thread's
  slice (the scheduler gives woken threads no boost).  `sleeptest timer`
  reports the timer queue case without judging it.  (Fixed the same day,
  see "Timer wake-ups preempt the running thread": the timer queue case
  is judged now.)  `NtSetTimer`'s own APC
  routine (native callers) is still ignored, and `NtSetTimerEx` is not
  implemented.

## Timer wake-ups preempt the running thread

A thread already waiting on a waitable timer when another thread set the
timer (a timer queue's worker, say) used to wait for the running thread's
20 ms time slice to end before it could look at the new due time, when
every CPU was busy.  Now it runs at once, as a thread woken by its own
deadline already did.

- **The scheduler** (`kernel/ke/scheduler.c`): a thread woken from a wait
  preempts the running thread when it has a higher priority, or the same
  priority and a timer woke it (`sched_unblock_timer`, which `um_ob_wake`
  uses for a timer object's waiters).  It goes first in its CPU's run
  queue; a halted CPU takes it if there is one, otherwise the waker sends
  the thread's CPU `IPI_WAKE` (itself too, taken once it re-enables
  interrupts) with a reschedule flag, and that CPU switches in the
  interrupt.  A CPU halted waiting for the kernel lock doesn't switch in
  the middle of that wait; its next timer tick does.
- **A CPU waiting for the kernel lock** wakes none of its sleepers, so a
  thread due on it waited as long as the lock's holder kept the lock.
  Saving drive C: after a big install keeps it for seconds under
  emulation, and the device poll thread due on the waiting CPU waited
  with it: the PS/2 controller's buffer filled and keystrokes were lost
  (graphics CI typed `store install DXK`).  Now the timer interrupt taken
  during that wait hands a due deadline sleeper that doesn't hold the
  lock to another CPU, as a timer wake.  Two graphics runs side by side
  on one machine lost a keystroke this way 4 times in 4 (the old
  scheduler too); with the hand-off, 6 in 6 passed.
- **The preempted thread** goes back after the woken threads but ahead of
  the rest (as on NT), not last, so a waker its wakee preempts doesn't
  wait out every other thread's slice; and it keeps what it has used of
  its slice, so one preempted often still reaches the end of it and the
  threads behind it are not starved.
- **Other wakes are as they were.**  An event set or a lock released with
  no priority difference doesn't preempt, and the thread is queued last:
  preempting there made lock convoys (smpstress's critical section shared
  by 8 threads took about 3 times as long).  Wider versions of this change
  also starved the desktop thread for 3 s once, and let a waiter woken
  while a higher-priority thread ran wait behind the thread it had
  preempted, which CI's `sleeptest timer` caught.
- **Measured** with `sleeptest timer` on two CPUs in QEMU, a busy thread
  on each: the 1 ms timer queue timer was 9 to 19 ms late (95th
  percentile) and is now 0.21 to 0.44 ms late over twenty runs.
  `sleeptest timer` now judges the timer queue case, and reports (without
  judging) how soon a thread waiting on an event runs once another thread
  sets it (9 to 19 ms under load, as before).
- **Not changed**: under load in QEMU the timer interrupt itself sometimes
  fires 1 to 10 ms late (a debug count found as many late fires with the
  old scheduler), so a rare `sleeptest timer` run can still slip past
  1 ms on one case.

## WASAPI: the engine keeps a 100 ms lead

The core suite's `soundtest wasapi` recording check failed on some CI runs
and passed on others: the 660 Hz tone came out with silent gaps (the
zero-crossing pitch read 580 to 620 Hz).  A shared-mode client fills its
own buffer, 30 ms at the least, and `mmdevapi` passed every released frame
straight to the kernel mixer stream and reported the stream's queue as the
padding, so the client never had more than its buffer's worth queued.  On a
loaded host (GitHub's runners) the client's 10 ms wakeups came late by more
than that and the mixer ran dry.

`mmdevapi` now keeps an engine lead, as Windows' audio engine has a buffer
of its own: the kernel stream holds the client buffer plus 100 ms, and the
padding is what the stream holds beyond that lead.  The client therefore
writes 100 ms ahead of the mixer and a late wakeup does not starve it.
`GetStreamLatency` reports the lead with the mixer's 80 ms.  Under a CPU
load that split the tone before, the recording now holds one unbroken
1010 ms tone.

## waveIn: no dropouts when a program falls behind

Audacity's recording in the app corpus came back with a "Dropouts" label
track (13 dropouts in ten seconds under QEMU without KVM, two under KVM)
and a recording shorter than it should be.  Audacity records through
PortAudio's MME host: eight `waveIn` buffers of about 14 ms, taken in turn
by one thread.  When that thread finds every buffer done, PortAudio counts
an input overflow, throws all but the newest away and tells Audacity,
which marks a dropout.  On NovaOS a thread, not the sound card, hands the
buffers back, and while Audacity redraws on a slow machine both that
thread and PortAudio's were held up for 100 to 270 ms.  The waveIn thread
then filled every queued buffer at once from the 1 s the kernel stream
holds, dropped the rest because no buffer was left, and PortAudio dropped
seven more.

`waveIn` now hands the last buffer queued back only once the program has
queued another (or after half a second), when the program ever queued more
than one, so a program that falls behind never finds every buffer done;
and with no buffer queued it keeps the newest half second of recording
instead of dropping it all, so the frames wait for the program's next
buffer.  A program cycling a single buffer gets it back as soon as it is
full, as before.

New self-test `soundtest mme`: records five seconds as PortAudio does with
the thread held up 250 ms every second.  Before, six overflows and 42
buffers lost; now none, and `tools/wavcheck.py --gaps HZ` (used by the
test) finds no jump anywhere in the recorded tone.

In the app corpus, Audacity's ten seconds now come back whole: the
screenshot after Stop differs from the reference in 0.2% of pixels (10%
before, with the dropout track), and its `dir` step that looks for the saved
project is marked a Terminal command, so it passes instead of waiting for a
program that never starts.

## Windows Installer rollback, transforms, patches, services at boot

The gaps [Windows Installer depth](#windows-installer-depth-custom-actions-dialogs-shortcuts-services)
left open, except script custom actions (next).  There is still no
permissive Windows Installer to borrow from (Wine's is LGPL), so this is
NovaOS's own code; the test packages are written by our own pure-Python
writer, so the build needs neither Windows nor msitools.

- **Rollback.**  While `InstallExecuteSequence` runs, the engine keeps a
  journal (`userland/msi/install.c`, "Rollback") of everything it
  changes: files it overwrites or deletes are moved to
  `C:\Config.Msi\*.rbf` first, new files, folders and registry keys are
  noted, registry values keep their old data, services note whether they
  were created, reconfigured, started or stopped, and the product's
  registration and cached package are journalled like any other key and
  file.  If an action fails the journal is played backwards: rollback
  custom actions (type flag `0x500`) run in their place with the
  `CustomActionData` they had, files come back from `Config.Msi`, new
  ones and the folders made for them go, keys and values return to what
  they were, services stop and are deleted or reconfigured back.  A
  successful install deletes the backups at `InstallFinalize`, before
  the commit actions.  `DISABLEROLLBACK=1` (or the `DisableRollback`
  action) turns it off, as on Windows.  Not rolled back: the removal of
  an older product by `RemoveExistingProducts`.
- **Transforms (`.mst`).**  `TRANSFORMS=a.mst;:embedded` on the command
  line, and `MsiDatabaseApplyTransform`, apply them to the database in
  memory (`msidb.c`): rows inserted, deleted or updated column by column,
  tables and columns added or dropped, the transform's own string pool
  merged, its streams laid over the package's.  The summary information's
  validation flags are checked (product code, upgrade code); a transform
  for another product fails with 1624.  The transforms a product was
  installed with are copied to `C:\Windows\Installer\{ProductCode}` and
  applied again for repair and removal.
- **Patches (`.msp`).**  `msiexec /p patch.msp` (or `/update`, or
  `PATCH=` with `/i`) finds the installed product the patch targets
  (1642 if there is none), applies the patch's transform pairs (`T` and
  `#T`, the ones whose validation fits), adds its cabinet streams and
  reinstalls; the patch is cached and recorded with the product, so a
  repair keeps it.  `msiexec /uninstall patch.msp` reinstalls without it
  (`MSIPATCHREMOVE`).  Patches that ship whole files work; binary delta
  patches (rows in the `Patch` table) are refused with a clear message.
- **Services at boot.**  `services.exe` (`userland/programs/services.c`)
  starts at boot, once the desktop is up on an installed system: it marks
  every service stopped (the registry's state is from the last boot),
  then starts each automatic service (`Start` 2), the services it
  depends on first and `DelayedAutoStart` ones last, and logs each result
  (`[SVC] Name: started`).  `services` with no argument lists them.
- **Test packages.**  `tools/msitest/mkmsi.py` writes compound files,
  databases, transforms, patches and MSZIP cabinets;
  `tools/msitest/mkpkg.py` uses it at build time for the packages the
  `msitest` self-test installs (`C:\Tests\Msi`).  The new core self-test
  (`tests/selftest/core/135-msitest.py`) runs `msitest transform`,
  `patch`, `rollback` and `service`, restarts, and checks that
  `services.exe` started the service.
- **Tested in QEMU** with real packages: Node.js 22.11 with a transform
  that adds a failing custom action just before `InstallFinalize`
  returned 1603 and rolled back 3176 changes (2525 files, folders,
  registry, shortcuts, its cached package and registration), leaving no
  `C:\Programs\nodejs` and no product key; Node.js then installed,
  took a whole-file patch (`msiexec /p`: the patched file, version
  22.11.1 in Programs and Features), kept it through a repair, lost it
  again with `msiexec /uninstall` (22.11.0, `node -e` runs) and was
  removed.  CMake 3.30 refused a transform made for another product
  (1624), installed with a validated transform adding a registry value,
  ran `cmake --version`, and its removal applied the cached transform
  and took the value away.  The test service package installed, and
  after a restart `services.exe` started it (`[SVC] NovaTestSvc:
  started`); `msiexec /x` stopped and deleted it.  A rollback custom
  action that fails is logged and the rollback carries on, as on Windows.
  Node.js's `WixRollbackInternetShortcuts` does fail that way (it finds
  no shortcut attributes in its `CustomActionData`); why is still open.
- Fixed on the way: a file is taken only from the cabinet its sequence
  number puts it in, so a patch's new copy is not overwritten by the
  product cabinet's entry with the same key.

## Windows Installer script custom actions (JScript and VBScript)

The last gap [Windows Installer rollback, transforms, patches](#windows-installer-rollback-transforms-patches-services-at-boot)
left: custom actions written in JScript or VBScript.  `msi.dll` used to
log and skip them; it now runs them on a new DLL, `msiscript.dll`
(`userland/msiscript`), in the installing process as Windows does.

- **The engine.**  JScript runs on [mujs](https://mujs.com/) 1.3.7
  (`third_party/mujs`, ISC licence), an ES5 interpreter in one C file.
  One change to it (`third_party/mujs/NOVA-VENDOR.txt`): an assignment
  to a method call, `Session.Property("X") = "1"`, compiles to a call of
  the property's setter, because Microsoft's JScript accepts that form for
  COM properties and installers use it everywhere.  There is no
  permissively licensed VBScript engine, so VBScript is translated to
  JavaScript (`userland/msiscript/vbscript.js`) and runs on the same
  engine: names are matched without regard to case, operators follow
  VBScript's variant rules (`"2" + "3"` is `"23"`, `2 + "3"` is 5, `Not`,
  `And` and `Or` are bitwise on numbers), each statement keeps its source
  line so errors name the script's line, and `vbslib.js` provides about
  seventy built-in functions and sixty constants (`Left`, `Mid`, `InStr`,
  `Replace`, `Split`, `Join`, `UBound`, `CInt`, `Hex`, `TypeName`,
  `vbCrLf`...).  Covered: `Dim`/`ReDim`/`Const`/`Set`, block and one-line
  `If`, `For`, `For Each`, `Do`/`Loop`, `While`/`Wend`, `Select Case`,
  `With`, `Sub` and `Function` with `Exit`, `Call`, `On Error Resume
  Next` with `Err`.  Not covered: `Class`, `Execute`, and `ByRef`
  arguments (every argument is passed by value).
- **What scripts see** (`userland/msiscript/runtime.js`): `Session`
  (`Property`, `TargetPath`, `SourcePath`, `Mode`, `Language`,
  `FeatureRequestState` and the other feature and component states,
  `EvaluateCondition`, `FormatRecord`, `Message`, `DoAction`, `Sequence`,
  `Installer`, `Database`), `Installer` (`CreateRecord`, `OpenDatabase`,
  `Environment`, `FileVersion`, `RegistryValue`, `ProductState`),
  `Database`, `View` and `Record` for SQL queries, and, through
  `CreateObject` or `new ActiveXObject`, `Scripting.FileSystemObject`
  (files, folders, text streams), `Scripting.Dictionary` and
  `WScript.Shell` (`RegRead`, `RegWrite`, `RegDelete`, `Run`,
  `ExpandEnvironmentStrings`, `Environment`, `SpecialFolders`).  The
  installer objects call `msi.dll`'s own API with the session's handle.
  `MsgBox` and `InputBox` are logged and answered with the default, since
  installs run unattended.
- **Every source type**: the script in the `Binary` table (types 5 and
  6), in a file the package installs (21 and 22), as the action's own
  `Target` text (37 and 38) and in a property's value (53 and 54), each
  calling the named function after the script's top level runs.  The
  function's result follows Windows: 2 cancels the install, 3 fails it;
  a script error is logged with its line and fails the action, unless
  the action may fail (`0x40`).  Deferred script actions read their
  `CustomActionData` as other deferred actions do.
- **Test packages.**  None of the real packages NovaOS installs (7-Zip,
  CMake, Node.js, Temurin, KeePassXC, PowerShell 7) carries a script
  custom action, so `tools/msitest/mkpkg.py` builds two:
  `script.msi` with a JScript and a VBScript action of each source type
  (`tools/msitest/scripts/`) that set properties, read them back, query
  the package's `Property` table, write files and the registry, and log
  through `Session.Message`; and `scriptfail.msi`, whose ignored failure
  and real failure end the install with 1603.  The new core self-test
  (`tests/selftest/core/136-msiscript.py`, `msitest script`) checks the
  properties as the `Registry` table wrote them, the files, the log, the
  removal, and the failed install (22 checks).

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

## Authenticode: WinVerifyTrust checks program signatures

Steam's service checks Steam's files with `WinVerifyTrust` and refused them
as unsigned, because NovaOS's `wintrust` did not check signatures.  It now
does, the way Windows does, offline: the signature in a program's
certificate table, its digest, its signer's certificate chain to a trusted
root, and a timestamp that keeps an expired signer valid.  Nothing is
skipped and nothing answers yes without checking.  The cryptography is
Mbed TLS's (Apache-2.0), already in NovaOS for TLS.

- **`WinVerifyTrust`** with `WINTRUST_ACTION_GENERIC_VERIFY_V2` (and the
  driver and published-software actions) over a file or a memory blob: the
  PE's Authenticode digest (SHA-1, SHA-256, SHA-384, SHA-512 or MD5, the
  checksum, the certificate-table entry and the table left out) against
  the signed `SpcIndirectDataContent`, the signer's signature over its
  authenticated attributes, the chain under the Authenticode policy with
  the code-signing usage, PKCS #9 countersignatures and RFC 3161 tokens
  (their own chain with the time-stamping usage), nested signatures
  (`WINTRUST_SIGNATURE_SETTINGS`), and the provider state
  (`WTD_STATEACTION_VERIFY`, `WTHelperProvDataFromStateData` and the
  signer and certificate helpers).  A revocation check fails with
  `CERT_E_REVOCATION_FAILURE`: NovaOS cannot reach revocation lists, and
  "unknown" is never "not revoked".
- **crypt32**: signed PKCS #7 messages (`CryptMsgOpenToDecode`,
  `CryptMsgUpdate`, `CryptMsgGetParam`, `CryptMsgControl`, countersignature
  checks), `CryptQueryObject` (embedded signatures, PKCS #7 and
  certificates, from a file or memory), message stores,
  `CertGetSubjectCertificateFromStore`, `CertVerifyTimeValidity`,
  `CertGetNameString` and `CertNameToStr`, chains checked at a given time,
  and the Authenticode policies in `CertVerifyCertificateChainPolicy`.
  Roots added to the machine's `ROOT` store are kept in
  `C:\Windows\System32\CertStore` and survive restarts; deleted ones stay
  deleted.
- **Roots**: Microsoft's code-signing and time-stamping roots join the
  Mozilla list, one file each in `userland/crypt32/roots`.
- **Catalogs**: `CryptCATAdminAcquireContext`,
  `CryptCATAdminCalcHashFromFileHandle` and the rest; NovaOS has no
  catalog files, so no catalog holds a hash.
- **Tests**: `tools/authenticode` signs test programs under a test root
  in plain Python (`mktests.py`, made the same way on every build), and
  the `authtest` self-test checks the files it must accept and the ones it
  must refuse (a changed byte, a damaged signature, an untrusted root, an
  expired signer without a timestamp, the wrong certificate usage).
- **And**: `RtlFormatCurrentUserKeyPath`, `RtlAcquirePrivilege` and
  `RtlReleasePrivilege` were only in the 32-bit `ntdll`; the 64-bit one
  has them too now, so Steam's 64-bit browser process gets past its start.

## cabinet.dll: installers extract their cabinets

The Visual C++ Redistributable's installer (a WiX Burn bundle, which GOG
GALAXY needs for `mfc140u.dll`) read its manifest through MSXML and then
stopped: it loads `cabinet.dll` to unpack the payloads attached to its
`.exe`, and NovaOS had none.  It has one now.

- **The File Decompression Interface**: `FDICreate`, `FDIIsCabinet`,
  `FDICopy` and `FDIDestroy` (cdecl, at Windows' ordinals 20 to 23, in
  `System32` and `SysWOW64`), with the caller's own memory and file
  functions, so a cabinet inside another file (Burn's attached container)
  works through the caller's offset.  `FDICopy` sends `fdintCABINET_INFO`,
  `fdintCOPY_FILE` (the caller returns a handle, 0 to skip, -1 to stop),
  `fdintCLOSE_FILE_INFO` and `fdintPARTIAL_FILE`, follows a folder into
  the next cabinet of a set (`fdintNEXT_CABINET`, the caller may change the
  path) and extracts that cabinet's files too; errors come back in the
  caller's `ERF` (`FDIERROR_CABINET_NOT_FOUND`, `NOT_A_CABINET`,
  `CORRUPT_CABINET`, `BAD_COMPR_TYPE`, `USER_ABORT`...).  Header
  `userland/include/fdi.h`.  Not yet: the compression side (FCI), Quantum
  compression, `FDITruncateCabinet`.
- **The decompressors** are the Windows Installer's own (`userland/msi/cab.c`
  for stored and MSZIP blocks, `userland/msi/lzx.c` for LZX), compiled
  into `cabinet.dll` as well, so no new code had to be borrowed.  They now
  also join a data block split between two cabinets (the first part, with
  an uncompressed size of 0, ends one cabinet; makecab writes sets that
  way), which `msi.dll`'s multi-cabinet media gets too.
- **Self-test** `cabtest` (core 148, 64- and 32-bit): MSZIP and LZX
  cabinets, a skipped file, an attached container, a two-cabinet set and
  the failures.  Its cabinets come from `tools/make_cabtest_data.py`, with
  a small LZX encoder of its own; the output was checked with cabextract
  and 7-Zip.
- **More 32-bit DLL slots**: with `cabinet.dll` and `msxml6.dll` both in,
  the 41 automatic 16 MiB slots for 32-bit DLLs (0x97000000 to
  0xC0000000) ran out and the build stopped; they now go up to
  0xF0000000.
- **Where the Visual C++ Redistributable stops now** (built with MSXML 6):
  Burn extracts its manifest through `cabinet.dll` and then cannot find
  the manifest's `UX` element (0x80070490): it selects `UX` with no
  prefix in a document whose elements are in a default namespace, which
  MSXML 3's XSLPattern matches and XPath does not.

## CI says which accelerator it used and stops without KVM

The nightly app corpus run of 2026-10-03 23:44 (pull request on a branch of
#166) landed on a runner without `/dev/kvm`.  `tools/ci/enable-kvm.sh` printed
a warning and fell back to TCG, the job went on for 28 minutes and its table
could not confirm the KVM-only fixes.  Now the app corpus job, and boot-test on pushes to main, fail in that step with a clear message when KVM is missing, and the job
summary and the corpus table header state "KVM" or "TCG".  A workflow started
by hand has an `allow_tcg` input for a deliberate TCG run.  The graphics job
keeps its TCG fallback, and so does boot-test on a pull request (a required check must not go red on the runner alone; the first try failed a PR that way).  No timeouts or tests changed.

## Installer ACLs: the VC++ Redistributable installs its first package

The Visual C++ Redistributable's installer (WiX Burn, which GOG GALAXY
needs for `mfc140u.dll`) started its elevated engine and then failed
0x80070005 creating its folder under `C:\ProgramData\Package Cache`.
Burn secures the cache with an access list it builds with
`SetEntriesInAcl` (Administrators and SYSTEM full control, Everyone and
Users read, inherited by everything below), and NovaOS's
`SetEntriesInAcl` returned an empty list: nobody, the elevated engine
included, could add anything to the folder.  NovaOS now builds and keeps
these access lists as Windows does, and checks them as before.

- **`SetEntriesInAcl`** (`advapi32/aclapi.c`) merges its entries into a
  copy of the old list: `GRANT_ACCESS` adds rights to the trustee's allow
  entry, `SET_ACCESS` replaces what the trustee had, `DENY_ACCESS` adds a
  deny entry, `REVOKE_ACCESS` removes the trustee's entries, and the
  result is in canonical order (denies, allows, then the old inherited
  entries).  Trustees are SIDs, account names or `CURRENT_USER`.  Also
  `GetExplicitEntriesFromAcl`, `BuildSecurityDescriptor` (which kept
  nothing before) and the trustee helpers; the types are in `winsec.h`.
- **SDDL** (`advapi32/sddl.c`): `ConvertStringSecurityDescriptorToSecurityDescriptor`
  parses owner, group, DACL and SACL with their flags, ACE types, flags,
  rights by number or name, object GUIDs and SID aliases; it used to
  return a descriptor that let everyone in, whatever the text said.
  `ConvertSecurityDescriptorToStringSecurityDescriptor` writes the real
  descriptor back as text.  The SID aliases are complete (`ME` is the
  medium integrity level, not the user).
- **`LookupAccountName`** finds the account a name names (the user, with
  or without `NOVAOS\` or the computer name, the built-in groups and
  well-known accounts) and fails with `ERROR_NONE_MAPPED` for a name
  that is no account, where it used to answer with the user.
- **Inheritance on `SetNamedSecurityInfo`**: with
  `UNPROTECTED_DACL_SECURITY_INFORMATION` (or neither flag on a DACL that
  was not protected), the folder's inheritable entries are merged in
  after the explicit ones, as Windows' automatic inheritance does.  Burn
  "resets" each cached folder that way: an empty DACL that inherits the
  cache root's entries.  `PROTECTED_DACL_SECURITY_INFORMATION` keeps a
  DACL as given, and the kernel now keeps the protected and
  auto-inherited bits with a file's DACL.  The security calls open files
  with only the right they need (as Windows does), so an owner whose DACL
  grants nothing can still change it.
- **Kernel**: a folder shows an entry its parent passes only to files
  (`OBJECT_INHERIT` without `CONTAINER_INHERIT`) as inherit-only, where it
  used to leave it out.
- **Self-test** `acltest` (core 070, 64- and 32-bit, 112 checks): the
  above, and Burn's cache: the root secured by its owner, a folder in it
  refused to us and made by the elevated (linked) token, reset to inherit,
  a file cached in it that we can read and not write.

**Where the Visual C++ Redistributable stops now**: its elevated engine
caches the bundle, registers it and installs the Minimum Runtime MSI
(`vcruntime140.dll`, `msvcp140.dll` and the rest), then crashes calling
`msi.dll`'s `MsiSourceListAddSourceExW`, which NovaOS does not have (Burn
looks it up and calls it without checking), so the Additional Runtime
with MFC is not installed yet.

## A kernel crash report typed for too early

The self-test `kernel crash report` failed on a slow (TCG) CI runner: after
`crash kernel` and the reset, `crashes last` printed the older report of a
program that had crashed before the reset.  At boot the kernel's report is
only queued; the desktop loop writes it to `C:\NovaOS\Crashes` in `UmPoll`,
after it has handled the input waiting.  A command typed in the first
iteration of that loop ran before the report existed.  `crashes last` now
writes the queued reports first (`UmCrashNewest` does it under the
file-system lock the Terminal already holds).  Found by the MSXML 6 thread
on PR #170, which proposed calling `UmCrashPoll`; that takes the lock the
Terminal holds, so the write is split out instead.

## Windows Installer: the queries bootstrappers make, 32-bit packages in SysWOW64

The Visual C++ Redistributable's installer (a WiX Burn bundle, which GOG
GALAXY needs for `mfc140u.dll`) installed its first package, the Minimum
Runtime, and then crashed: Burn looks up `msi.dll`'s newer functions with
`GetProcAddress` and calls `MsiSourceListAddSourceExW` without checking,
and NovaOS had none of them.  It has them now, and both runtimes install.

- **Product information by installation context**: `MsiGetProductInfoEx`
  and `MsiEnumProductsEx` (per-machine, or the current user's for a
  per-user install; a SID or a context that does not fit is refused as on
  Windows), with the `INSTALLPROPERTY_*` names (`VersionString`, `State`,
  `PackageCode`, `AssignmentType`, `LocalPackage`, `PackageName`,
  `LastUsedSource`...).  `MsiGetProductInfo` and `MsiEnumProducts` now
  answer the same way, and string results follow the Windows Installer's
  length rules (`ERROR_MORE_DATA`, a NULL buffer asks for the length).
- **Source lists**: an install registers where its package came from
  under the product's Installer key (`SourceList`: `PackageName`,
  `LastUsedSource`, `Net\1`), as Windows does, and
  `MsiSourceListAddSourceEx` (Burn adds its package cache there),
  `EnumSources`, `GetInfo`, `SetInfo`, `ClearSource`, `ClearAllEx` and the
  older `AddSource`/`ClearAll` work on it, for products and for patches.
- **Patches**: `MsiDetermineApplicablePatches` and
  `MsiDeterminePatchSequence` say which patches fit a package or an
  installed product, from a patch file or its applicability XML (each
  patch that does not fit gets order -1 and its reason, such as 1642);
  `MsiEnumPatchesEx` and `MsiGetPatchInfoEx` list the patches applied to
  a product.  Patches are ordered as given: NovaOS does not read
  `MsiPatchSequence` yet.
- **32-bit packages**: a package whose summary names the `Intel` platform
  now puts its `SystemFolder` files in `C:\Windows\SysWOW64`, where
  32-bit programs load their DLLs, as 64-bit Windows does; `System64Folder`
  stays `System32`.  The engine running inside a 32-bit process (Burn
  calls `MsiInstallProduct` in-process) turns WoW64 file redirection off,
  as Windows' 64-bit installer service sees the folders as they are, so a
  64-bit package installed from a 32-bit bootstrapper lands in
  `System32`.
- **Upgrades by version range**: `FindRelatedProducts` now honours the
  Upgrade table's `VersionMin`, `VersionMax` (inclusive or not) and
  `Language` columns, and `RemoveExistingProducts` leaves alone what a
  detect-only row found.  Before, every product with the upgrade code
  counted, so a newer Visual C++ runtime (GOG GALAXY carries 14.51) was
  refused as "a later version is already installed" over 14.44.
- **Self-test** `msiqtest` (core 150, 64- and 32-bit) with two new test
  packages from `tools/msitest/mkpkg.py`, `wow32.msi`, `wow64.msi` and the
  upgrade `wow32v2.msi`.
- **Where GOG GALAXY stops now**: its setup installs the x86 and x64
  runtimes it carries and its files; `GalaxyClient.exe` (64-bit, Qt 6
  WebEngine) does not start because `d3d9.dll` is missing (Qt WebEngine
  imports it; NovaOS has Direct3D 9 only from DXVK in the App Store).

## MSXML: XSL Patterns match names as written

The Visual C++ Redistributable's installer (a WiX Burn bundle, which GOG
GALAXY needs for `mfc140u.dll`) extracted its manifest and then stopped
with 0x80070490, "Failed to select user experience node".  Burn creates
`Msxml2.DOMDocument`, MSXML 3's class, sets no selection property and asks
for `UX`, `Payload`, `Chain/MsiPackage` and so on, unprefixed, in a
manifest whose elements are all in a default namespace.  MSXML 3's
default selection language is XSL Patterns, which name an element by its
qualified name as written in the document, so those match; `msxml6.dll`
ran every query as XPath 1.0, where an unprefixed name means no namespace,
so nothing did.

- **XSL Patterns in `msxml6.dll`** (`userland/msxml6/dom.c`): when a
  document's `SelectionLanguage` is `XSLPattern` (the default of the MSXML
  3, 2 and version-independent classes), each element name test becomes a
  test of the node's qualified name: `UX` is any element written `UX`,
  whatever its default namespace, `x:UX` one written with the prefix `x`,
  `x:*` any element written with `x:`, and a prefixed attribute matches as
  written too.  A prefix declared in `SelectionNamespaces` still means its
  namespace.  The query is tokenised by XPath's own lexical rules, so
  functions, node types, axes, the `and`/`or`/`div`/`mod` operators,
  literals and variables are left alone.  XPath (MSXML 6's only language,
  or after `setProperty("SelectionLanguage", "XPath")`) is unchanged.  Not
  yet: XSL Patterns' own operators and methods (`$eq$`, `$and$`, `end()`,
  `index()`), which no installer seen so far uses.
- **The self-test** `msxmltest` (core 147) checks both: Burn's queries on
  an MSXML 3 document, prefixes as written, `SelectionNamespaces`, names
  that look like operators or node types, and the same queries matching
  nothing under XPath and on an MSXML 6 document (130 checks, 64- and
  32-bit).

**Where the Visual C++ Redistributable stops now**: Burn reads its whole
manifest, detects, plans, loads its bootstrapper application and starts
its elevated engine, which fails 0x80070005 (access denied) creating its
cache folder `C:\ProgramData\Package Cache\{bundle id}\` ("Failed to
create cache directory"), so it registers nothing and exits with code 5.
Creating that folder from `cmd` works, so the gap is in the secured
folder Burn makes there (its own ACL) as the elevated engine.

## MSXML 6

Installers read their manifests through MSXML: WiX Burn bundles (the
Visual C++ Redistributable that GOG GALAXY runs) create
`Msxml2.DOMDocument`, and the WebView2 runtime's Edge Update creates
`DOMDocument60` for its update requests.  NovaOS had no MSXML, so both
stopped there (`CO_E_CLASSSTRING` from `CLSIDFromProgID`).  Now
`msxml6.dll` answers them.

- **The parser** is [libxml2](https://gitlab.gnome.org/GNOME/libxml2)
  2.13.8 (`third_party/libxml2`, MIT licence), unchanged, built into
  `msxml6.dll` (`userland/msxml6/build.py`; `userland/msxml6/libxml/`
  holds its `config.h` and the two Windows headers our SDK lacks).
- **The DOM** (`userland/msxml6/dom.c`, `lists.c`): `IXMLDOMDocument3`
  with `load` (a path, `file://` or `http(s)://` URL, a stream, a byte
  array or another document), `loadXML`, `save`, `xml`, `text`,
  `parseError`, `createElement` and the other factories, `importNode`,
  `getElementsByTagName`, and `selectNodes`/`selectSingleNode` on
  libxml2's XPath, with `SelectionNamespaces`; nodes, elements,
  attributes, text, CDATA, comments, processing instructions and
  doctypes, node lists and attribute maps.  As in MSXML, the XML
  declaration is a processing instruction child, `xmlns` declarations
  are attributes, and whitespace-only text is dropped unless
  `preserveWhiteSpace` or `xml:space="preserve"` keeps it.
- **MSXML 3 and 6.**  Windows answers `Msxml2.DOMDocument`,
  `Microsoft.XMLDOM` and the 3.0 classes from `msxml3.dll`; NovaOS
  registers every class to `msxml6.dll` (the kernel's registry defaults,
  `kernel/um/um_registry.c`), which gives the 3.0 ones MSXML 3's
  defaults: DTDs allowed, external entities resolved, XSL patterns
  (evaluated as XPath).  The 6.0 classes prohibit DTDs and resolve
  nothing.  The DLL carries MSXML 6's 6.30 version resource.
- **SAX and XMLHTTP** (`saxreader.c`, `httpreq.c`): `SAXXMLReader` on
  libxml2's SAX2 callbacks; `XMLHTTP` and `ServerXMLHTTP` on WinHTTP.
- **`IID_ISequentialStream`** in `objbase.h` had the wrong GUID.
- **The self-test** `msxmltest` (`tests/selftest/core/147-msxml.py`)
  drives it all in 64- and 32-bit programs.

The Visual C++ Redistributable's installer now reads its manifest and
stops next at `cabinet.dll`, which it loads to unpack its payload.

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

## OpenGL in 16-bit colour: Chocolate Doom in the right colours

Chocolate Doom (SDL2) drew Freedoom in red, pink and yellow noise on Mesa
3D's OpenGL.  SDL2 asks OpenGL for at least 3:3:2 bits of colour, and
Mesa's `wglChoosePixelFormatARB` answers with its closest window format,
4:4:4:4 (16 bits).  Mesa presents such a frame to the window as a 16-bit
DIB whose `BITMAPV5HEADER` carries `BI_BITFIELDS` masks (`0x0F00`, `0x00F0`,
`0x000F`), and gdi32's `StretchDIBits` read every 16-bit DIB as either
5:6:5 or 5:5:5, so each 4-bit channel landed in the wrong bits.

- gdi32 reads 16- and 32-bit DIBs by their own masks (`BI_BITFIELDS` and
  `BI_ALPHABITFIELDS`, any header version), as Windows does: 4:4:4:4,
  5:6:5, 5:5:5, 10:10:10:2 and red-first 8:8:8 frames all show in their
  colours.  The fast path for presenting frames only copies 32-bit DIBs
  whose masks are the usual 0x00RRGGBB.
- Test: the graphics suite's `gltest colors` draws four coloured bars in
  every colour depth Mesa offers a window and reads them back
  (`tests/selftest/graphics/105-gltest-colors.py`), 32- and 64-bit.

Chocolate Doom now shows Freedoom in its own colours, in the 4 bits per
channel SDL2 asked for (as on Windows with Mesa).

## Drive C: files take the memory their contents need

App corpus round four.  In the first corpus run on a tree with round
three's fixes (nightly run 55, on PR #166's branch; that runner had no
KVM, so it ran under TCG), 19 of 21 programs passed: Audacity records
without dropouts and Krita starts and builds its resource database (round
three's floating-point fix), but Krita then stopped with
`0xc0000139` after `[PMM] out of memory: 16385 page(s) wanted, 61 MiB
free`, and the Firefox install stopped with 7-Zip's "Cannot set length
for output file" (`ERROR_DISK_FULL`).  Both came right after the newly
added Roblox install, the first corpus program to install hundreds of
megabytes onto drive C:, and drive C: is kept in memory.

A file's buffer on drive C: grew by doubling as the file was written, so
appending stayed cheap, and it kept the doubled size for as long as the
file existed: a file written in pieces took up to twice its size.  Setting
a file's length (`SetEndOfFile`, which 7-Zip does before unpacking each
file) also rounded up to the next power of two, so Firefox's 84 MB
`omni.ja` needed one free piece of 128 MiB.

- **Files give the rest back.**  When nothing holds a file any more, the
  pages of its buffer past its contents go back to the machine.
- **Files grow where they are.**  A file whose buffer can't hold a write
  first takes the free pages right after it (new `kresize`, with
  `pmm_claim_pages`), and only when those are in use moves to a new,
  doubled buffer.  Setting the length takes exactly that length.
- **A DLL loaded later no longer copies the DLLs it imports from.**  To
  bind a newly loaded DLL's imports from a module the process already
  had, the loader copied that module's whole image into one piece of
  kernel memory (7 MB for Qt5Core, 164 MB for Firefox's `xul.dll`).  It
  now reads only the module's export directory.  When even that fails
  for lack of memory, the load fails with "Out of memory"; before, every
  import from that module was bound to NovaOS's stub for a missing
  function, which is how Krita ended with `0xc0000139` ("unimplemented
  `QString::fromAscii_helper` in Qt5Core.dll", a function Qt5Core.dll
  has) when its next plugin loaded.
- **Seeing it.**  The Terminal's `mem` also prints how many files drive C:
  holds and the memory they take; the app corpus prints both lines after
  each program.

- **Test.**  Core self-test `ramdisktest`: 33 MiB appended in 64 KiB
  writes takes about 33 MiB once closed (64 MiB before), `SetEndOfFile` to
  72 MiB takes 72 MiB (128 MiB before), contents survive files growing in
  turn, shrinking and growing again and appending to a closed file, and
  deleting gives the memory back.

In the corpus with Roblox, Krita and Firefox alone (TCG), the three left
1,924 MB taken before and 1,263 MB after; drive C: then held 2,021 MB of
files in 2,029 MB of memory.

## rpcrt4's NDR engine

COM calls into another process go through a proxy in the caller and a
stub in the server, which marshal each call's arguments into a buffer
and back.  Programs ship those proxies and stubs as proxy/stub DLLs
(Microsoft Edge Update's `psmachine.dll`, for one) generated by MIDL:
tables of format strings that `rpcrt4.dll`'s NDR engine interprets.
NovaOS's `rpcrt4` had none of it, so Edge Update (the WebView2 runtime's
installer) stopped at its `/regserver` step on a missing
`NdrDllRegisterProxy`.  Now `rpcrt4` has the engine; the calls between
processes themselves (ole32's side) are the next step.

- **Types** (`userland/rpcrt4/ndr_types.c`): sizing, marshaling,
  unmarshaling and freeing of the Oicf format strings: base types,
  pointers (ref, unique, full treated as unique, with embedded pointers
  deferred as NDR orders them), simple, conformant and complex
  structures, fixed, conformant, conformant-varying and complex arrays,
  strings, `user_marshal` types and interface pointers (through ole32's
  `CoMarshalInterface`).  Unions, pipes, `transmit_as` and context
  handles are not there yet and fail with `RPC_X_BAD_STUB_DATA`.
- **Calls** (`ndr_proc.c`): stubless proxies (one thunk per method, x64
  and x86, floating-point arguments included) and `NdrStubCall2`, which
  unmarshals a request into an argument block, calls the server object
  (an exception in it becomes `RPC_E_SERVERFAULT`) and marshals the
  results; `NdrClientCall2` and the `/Os` entry points for older stubs.
- **COM objects** (`ndr_ole.c`): interface proxies and `CStdStubBuffer`
  stubs built from a proxy DLL's tables, delegation to a base interface in
  another proxy DLL, the proxy/stub class object (`NdrDllGetClassObject`,
  `NdrDllCanUnloadNow`) and registration (`NdrDllRegisterProxy`,
  `NdrDllUnregisterProxy`).  `rpcproxy.h` and `rpcndr.h` carry what
  MIDL- and widl-generated code includes.
- **ole32** gained `CoGetPSClsid`, which finds an interface's proxy/stub
  class (registered with `CoRegisterPSClsid`, or in
  `HKCR\Interface\{iid}\ProxyStubClsid32`).
- **The self-test** `ndrtest` (`tests/selftest/core/149-ndrtest.py`)
  runs calls of every shape through a widl-built proxy DLL
  (`userland/ndrtestps`) in 64- and 32-bit programs.  It found five bugs
  before this landed: a simple pointer to a string, widl's way of
  describing `[out, string]` parameters, `[in, out]` parameters cleared
  when a call failed, the x86 proxies popping the return value's stack
  slot, and the x64 call into the server lacking unwind data, so a crash
  there was not caught.

## Steam: installs, updates itself and starts its client

Dean asked for games; after Roblox and GOG came Steam.  Steam's own
installer (`SteamSetup.exe`) now installs on NovaOS, and on its first
start Steam downloads its client over HTTPS, updates itself to the 64-bit
client, checks every file and starts it with `SteamService` and its
Chromium browser, `steamwebhelper.exe`.  The browser does not open the
login window yet ([compatibility.md](../compatibility.md#steam)).  Steam
is in the App Store (Media), and the nightly corpus installs it and lets
it update (`tests/appcorpus/092-steam.py`; it downloads from Valve's
servers, so the corpus needs the internet for it).

- **Certificate chains.**  crypt32 builds and checks certificate chains
  (`CertGetCertificateChain`, `CertVerifyCertificateChainPolicy` for the
  base and SSL policies, `CertCreateCertificateContext` and the store
  functions programs pair with them) against the same Mozilla roots
  Schannel uses, with Mbed TLS (`userland/crypt32/certs.c`); 32-bit
  programs find the roots in `SysWOW64` too.  Steam checks the server
  certificate of every download this way.
- **The user's proxy.**  `WinHttpGetIEProxyConfigForCurrentUser` reports
  the proxy, its exceptions and the setup script from the user's Internet
  Settings, as Windows keeps them.
- **Known folders.**  `SHGetKnownFolderPath` knows the Common Files
  folders.
- **Security.**  `SetEntriesInAcl`, `LookupAccountName` for the well-known
  accounts, and a service's security descriptor
  (`QueryServiceObjectSecurity`, `SetServiceObjectSecurity`, with Windows'
  default for a new service).
- **Files.**  `GetFullPathName("C:.")` is the current directory when it is
  on drive C:, as on Windows (the C runtime's `_getcwd` asks for it);
  seeking before the start of a file fails with `ERROR_NEGATIVE_SEEK`,
  the error the C runtime's `fopen("w+")` expects; listing a folder while
  a program moves its files out no longer skips any (the listing resumes
  after the entry it returned last, wherever that is now).  Steam unpacks
  its update into a folder and moves each file out as it lists it.
- **Loading DLLs.**  A `\\?\C:\...` path names the plain `C:\...` one, and
  the power API sets (`api-ms-win-power-*`) are powrprof's.
- New: `imagehlp.dll` (a PE file's certificate table), and in other DLLs
  `SetupDiClassGuidsFromName` with Windows' device class GUIDs,
  `SetupDiGetDeviceInstanceIdA`, `RtlFormatCurrentUserKeyPath`,
  `RtlAcquirePrivilege`, `RegisterSuspendResumeNotification`,
  `DefRawInputProc`, `GetGuiResources`, Winsock's name-space functions
  (`WSALookupServiceBegin` and the rest, with no providers, as Windows
  without any), `PowerReadACValue`, `PowerReadDCValue` and
  `PowerDeterminePlatformRoleEx`.

How far Steam gets: the browser process starts but never starts its GPU
and page processes.  On the way, `SteamService` reports Steam's files as
unsigned because NovaOS's `wintrust` does not check Authenticode yet; the
faithful fix is Authenticode in `wintrust`, never an answer that skips the
check.

## WebView2: Microsoft Edge Update runs its install step

Roblox's login page needs the Microsoft Edge WebView2 runtime, whose
installer is Microsoft Edge Update (32-bit).  It used to stop at start on a
function NovaOS lacked; it now starts, reads its policies and runs its
install step, which ends at the next missing piece, MSXML 6
([compatibility.md](../compatibility.md#webview2)).  The nightly corpus runs
the offline installer (`tests/appcorpus/095-webview2.py`).

- **Task Scheduler 2.0** (`taskschd.dll`, the `TaskScheduler` class):
  `ITaskService`, `ITaskFolder`, `IRegisteredTask`, the task, folder and
  running-task collections and `ITaskDefinition`'s XML.  A registered task
  is its XML in `C:\Windows\System32\Tasks` (UTF-16, as Windows writes it)
  and an entry in the registry's `Schedule\TaskCache`, so tasks survive
  restarts; a task can be read back, disabled, enabled, run (its `<Exec>`
  actions start) and deleted.  Edge Update registers its update tasks this
  way.  Not yet: a scheduler service that starts tasks on their triggers,
  and the definition's object model (triggers and actions as objects).
- **The Data Protection API**: `CryptProtectData` and `CryptUnprotectData`
  (crypt32) seal data with a key of the user's (or, with
  `CRYPTPROTECT_LOCAL_MACHINE`, the machine's), made on first use; the blob
  is encrypted and authenticated (HMAC-SHA-256), so the wrong entropy or a
  changed blob fails with `NTE_BAD_DATA` as on Windows.
- **URLs**: shlwapi's `UrlCombineW`/`UrlCombineA` (RFC 3986 resolution,
  dot segments removed), `UrlEscapeA` and `UrlUnescapeA`.
- **Packages**: `PackageFamilyNameFromFullName`, `PackageIdFromFullName`
  and `GetPackagesByPackageFamily` (none is installed).
- **And**: `MDMRegistration.dll` (the machine is not enrolled in device
  management), `NetGetAadJoinInformation` (not joined to Azure AD),
  `WTSEnumerateSessionsW`/`A` (session 0 and the console's session 1),
  `MakeAbsoluteSD`, userenv's `EnterCriticalPolicySection`,
  `LeaveCriticalPolicySection` and `GetProfileType`, ole32's
  `CoRegisterPSClsid`, `CoGetCallContext` and `CoGetStdMarshalEx`,
  `InternetSetStatusCallback`, `WTHelperGetProvSignerFromChain`,
  `WerRegisterCustomMetadata` and `AppPolicyGetProcessTerminationMethod`.
- **cmd's `type`** shows a UTF-16 file (one starting with a byte-order
  mark, as Edge Update's log does) as text, as Windows' does.
- The `edgeupdtest` self-test checks them, 64- and 32-bit.

<!-- END generated:history -->
